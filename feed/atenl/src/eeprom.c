#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "atenl.h"

#define EEPROM_PART_SIZE 20480
#define EEPROM_FILE	"/tmp/atenl-eeprom"

static FILE *mtd_open(const char *mtd)
{
	char line[128], name[64];
	FILE *fp;
	int i;

	fp = fopen("/proc/mtd", "r");
	if (!fp)
		return NULL;

	snprintf(name, sizeof(name), "\"%s\"", mtd);
	while (fgets(line, sizeof(line), fp)) {
		if (!sscanf(line, "mtd%d:", &i) || !strstr(line, name))
			continue;

		snprintf(line, sizeof(line), "/dev/mtd%d", i);
		fclose(fp);
		return fopen(line, "r");
	}
	fclose(fp);

	return NULL;
}

static int
atenl_flash_create_file(struct atenl *an)
{
	char buf[1024];
	ssize_t len;
	FILE *f;
	int fd, ret;

	f = mtd_open(an->mtd_part);
	if (!f) {
		fprintf(stderr, "Failed to open MTD device\n");
		return -1;
	}

	fd = open(EEPROM_FILE, O_RDWR | O_CREAT | O_EXCL, 00644);
	if (fd < 0)
		goto out;

	while ((len = fread(buf, 1, sizeof(buf), f)) > 0) {
		ssize_t w;

retry:
		w = write(fd, buf, len);
		if (w > 0)
			continue;

		if (errno == EINTR)
			goto retry;

		perror("write");
		unlink(EEPROM_FILE);
		close(fd);
		fd = -1;
		goto out;
	}

	ret = lseek(fd, 0, SEEK_SET);
	if (ret) {
		fclose(f);
		close(fd);
		return ret;
	}

out:
	fclose(f);
	return fd;
}

static int
atenl_efuse_create_file(struct atenl *an)
{
	char fname[64], buf[1024];
	ssize_t len;
	int fd_ori, fd, ret;

	snprintf(fname, sizeof(fname),
		"/sys/kernel/debug/ieee80211/phy%d/mt76/eeprom", get_band_val(an, 0, phy_idx));
	fd_ori = open(fname, O_RDONLY);
	if (fd_ori < 0)
		return -1;

	fd = open(EEPROM_FILE, O_RDWR | O_CREAT | O_EXCL, 00644);
	if (fd < 0)
		goto out;

	while ((len = read(fd_ori, buf, sizeof(buf))) > 0) {
		ssize_t w;

retry:
		w = write(fd, buf, len);
		if (w > 0)
			continue;

		if (errno == EINTR)
			goto retry;

		perror("write");
		unlink(EEPROM_FILE);
		close(fd);
		fd = -1;
		goto out;
	}

	ret = lseek(fd, 0, SEEK_SET);
	if (ret) {
		close(fd_ori);
		close(fd);
		return ret;
	}

out:
	close(fd_ori);
	return fd;
}

static bool
atenl_eeprom_file_exists(void)
{
	struct stat st;

	return stat(EEPROM_FILE, &st) == 0;
}

static int
atenl_eeprom_init_file(struct atenl *an, bool flash_mode)
{
	int fd;

	if (!atenl_eeprom_file_exists()) {
		if (flash_mode)
			atenl_dbg("[%d]%s: init eeprom with flash mode\n", getpid(), __func__);
		else
			atenl_dbg("[%d]%s: init eeprom with efuse mode\n", getpid(), __func__);

		if (flash_mode)
			return atenl_flash_create_file(an);

		return atenl_efuse_create_file(an);
	}

	fd = open(EEPROM_FILE, O_RDWR);
	if (fd < 0)
		perror("open");

	an->eeprom_exist = true;

	return fd;
}

static void
atenl_eeprom_init_max_size(struct atenl *an)
{
	switch (an->chip_id) {
	case 0x7915:
		an->eeprom_size = 3584;
		break;
	case 0x7906:
	case 0x7916:
	case 0x7986:
		an->eeprom_size = 4096;
		break;
	default:
		break;
	}
}

static void
atenl_eeprom_init_band_cap(struct atenl *an)
{
	u8 *eeprom = an->eeprom_data;

	if (is_mt7915(an)) {
		u8 val = eeprom[MT_EE_WIFI_CONF];
		u8 band_sel = FIELD_GET(MT_EE_WIFI_CONF0_BAND_SEL, val);
		struct atenl_band *anb = &an->anb[0];

		/* MT7915A */
		if (band_sel == MT_EE_BAND_SEL_DEFAULT) {
			anb->valid = true;
			anb->cap = BAND_TYPE_2G_5G;
			return;
		}

		/* MT7915D */
		if (band_sel == MT_EE_BAND_SEL_2GHZ) {
			anb->valid = true;
			anb->cap = BAND_TYPE_2G;
		}

		val = eeprom[MT_EE_WIFI_CONF + 1];
		band_sel = FIELD_GET(MT_EE_WIFI_CONF0_BAND_SEL, val);
		anb++;

		if (band_sel == MT_EE_BAND_SEL_5GHZ) {
			anb->valid = true;
			anb->cap = BAND_TYPE_5G;
		}
	} else if (is_mt7916(an) || is_mt7986(an)) {
		struct atenl_band *anb;
		u8 val, band_sel;
		int i;

		for (i = 0; i < 2; i++) {
			val = eeprom[MT_EE_WIFI_CONF + i];
			band_sel = FIELD_GET(MT_EE_WIFI_CONF0_BAND_SEL, val);
			anb = &an->anb[i];

			anb->valid = true;
			switch (band_sel) {
			case MT_EE_BAND_SEL_2G:
				anb->cap = BAND_TYPE_2G;
				break;
			case MT_EE_BAND_SEL_5G:
				anb->cap = BAND_TYPE_5G;
				break;
			case MT_EE_BAND_SEL_6G:
				anb->cap = BAND_TYPE_6G;
				break;
			case MT_EE_BAND_SEL_5G_6G:
				anb->cap = BAND_TYPE_5G_6G;
				break;
			default:
				break;
			}
		}
	}
}

static void
atenl_eeprom_init_antenna_cap(struct atenl *an)
{
	if (is_mt7915(an)) {
		if (an->anb[0].cap == BAND_TYPE_2G_5G)
			an->anb[0].chainmask = 0xf;
		else {
			an->anb[0].chainmask = 0x3;
			an->anb[1].chainmask = 0xc;
		}
	} else if (is_mt7916(an)) {
		an->anb[0].chainmask = 0x3;
		an->anb[1].chainmask = 0x3;
	} else if (is_mt7986(an)) {
		an->anb[0].chainmask = 0xf;
		an->anb[1].chainmask = 0xf;
	}
}

int atenl_eeprom_init(struct atenl *an, u8 phy_idx)
{
	bool flash_mode;
	int eeprom_fd;

	set_band_val(an, 0, phy_idx, phy_idx);

	atenl_nl_check_mtd(an);
	flash_mode = an->mtd_part != NULL;

	eeprom_fd = atenl_eeprom_init_file(an, flash_mode);
	if (eeprom_fd < 0)
		return -1;

	an->eeprom_data = mmap(NULL, EEPROM_PART_SIZE, PROT_READ | PROT_WRITE,
			       MAP_SHARED, eeprom_fd, an->mtd_offset);
	if (!an->eeprom_data) {
		perror("mmap");
		close(eeprom_fd);
		return -1;
	}

	an->eeprom_fd = eeprom_fd;
	an->chip_id = *(u16 *)an->eeprom_data;
	atenl_eeprom_init_max_size(an);
	atenl_eeprom_init_band_cap(an);
	atenl_eeprom_init_antenna_cap(an);

	if (get_band_val(an, 1, valid))
		set_band_val(an, 1, phy_idx, phy_idx + 1);

	return 0;
}

void atenl_eeprom_close(struct atenl *an)
{
	msync(an->eeprom_data, EEPROM_PART_SIZE, MS_SYNC);
	munmap(an->eeprom_data, EEPROM_PART_SIZE);
	close(an->eeprom_fd);

	if (!an->eeprom_exist && (an->child_pid || an->cmd_mode))
		if (remove(EEPROM_FILE))
			perror("remove");
}

int atenl_eeprom_write_mtd(struct atenl *an)
{
	bool flash_mode = an->mtd_part != NULL;
	pid_t pid;

	if (!flash_mode)
		return 0;

	pid = fork();
	if (pid < 0) {
		perror("Fork");
		return EXIT_FAILURE;
	} else if (pid == 0) {
		char *part = strdup(an->mtd_part);
		char *cmd[] = {"mtd", "write", EEPROM_FILE, part, NULL};
		int ret;

		ret = execvp("mtd", cmd);
		if (ret < 0) {
			fprintf(stderr, "%s: execl error\n", __func__);
			exit(0);
		}
	} else {
		wait(&pid);
	}

	return 0;
}

/* Directly read some value from driver's eeprom.
 * It's usally used to get calibrated data from driver.
 */
int atenl_eeprom_read_from_driver(struct atenl *an, u32 offset, int len)
{
	u8 *eeprom_data = an->eeprom_data + offset;
	char fname[64], buf[1024];
	int fd_ori, ret;
	ssize_t rd;

	snprintf(fname, sizeof(fname),
		"/sys/kernel/debug/ieee80211/phy%d/mt76/eeprom",
		get_band_val(an, 0, phy_idx));
	fd_ori = open(fname, O_RDONLY);
	if (fd_ori < 0)
		return -1;

	ret = lseek(fd_ori, offset, SEEK_SET);
	if (ret < 0)
		goto out;

	while ((rd = read(fd_ori, buf, sizeof(buf))) > 0 && len) {
		if (len < rd) {
			memcpy(eeprom_data, buf, len);
			break;
		} else {
			memcpy(eeprom_data, buf, rd);
			eeprom_data += rd;
			len -= rd;
		}
	}

	ret = 0;
out:
	close(fd_ori);
	return ret;
}

/* Update all eeprom values to driver before writing efuse */
static void
atenl_eeprom_sync_to_driver(struct atenl *an)
{
	int i;

	for (i = 0; i < 3584; i += 16)
		atenl_nl_write_eeprom(an, i, &an->eeprom_data[i], 16);
}

void atenl_eeprom_cmd_handler(struct atenl *an, u8 phy_idx, char *cmd)
{
	bool flash_mode;

	an->cmd_mode = true;

	atenl_eeprom_init(an, phy_idx);
	flash_mode = an->mtd_part != NULL;

	if (!strncmp(cmd, "sync eeprom all", 15)) {
		atenl_eeprom_write_mtd(an);
	} else if (!strncmp(cmd, "eeprom", 6)) {
		char *s = strchr(cmd, ' ');

		if (!s) {
			fprintf(stderr, "eeprom: please type a correct command\n");
			return;
		}

		s++;
		if (!strncmp(s, "reset", 5)) {
			unlink(EEPROM_FILE);
		} else if (!strncmp(s, "file", 4)) {
			atenl_info("%s\n", EEPROM_FILE);
			atenl_info("Flash mode: %d\n", flash_mode);
		} else if (!strncmp(s, "set", 3)) {
			u32 offset, val;

			s = strchr(s, ' ');
			if (!s)
				return;
			s++;

			if (!sscanf(s, "%x=%x", &offset, &val) ||
			    offset > EEPROM_PART_SIZE)
				return;

			an->eeprom_data[offset] = val;
			atenl_info("set offset 0x%x to 0x%x\n", offset, val);
		} else if (!strncmp(s, "update", 6)) {
			atenl_nl_update_buffer_mode(an);
		} else if (!strncmp(s, "write", 5)) {
			s = strchr(s, ' ');
			if (!s)
				return;
			s++;

			if (!strncmp(s, "flash", 5)) {
				atenl_eeprom_write_mtd(an);
			} else if (!strncmp(s, "efuse", 5)) {
				atenl_eeprom_sync_to_driver(an);
				atenl_nl_write_efuse_all(an, NULL);
			}
		} else if (!strncmp(s, "read", 4)) {
			u32 offset;

			s = strchr(s, ' ');
			if (!s)
				return;
			s++;

			if (!sscanf(s, "%x", &offset) ||
			    offset > EEPROM_PART_SIZE)
				return;

			atenl_info("val = 0x%x (%u)\n", an->eeprom_data[offset],
							an->eeprom_data[offset]);
		}
	} else {
		atenl_err("Unknown command: %s\n", cmd);
	}
}