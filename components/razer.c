/* Razer Basilisk X HyperSpeed status and controls via OpenRazer sysfs. */
#include <dirent.h>
#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../util.h"

#define RAZER_DRIVER_DIR "/sys/bus/hid/drivers/razermouse"
#define RAZER_DPI_MIN 100
#define RAZER_DPI_MAX 16000
#define RAZER_DPI_STEP 100

static char device_path[PATH_MAX];

static int
find_device(const char *device_id)
{
	DIR *dir;
	struct dirent *entry;
	char path[PATH_MAX];
	size_t id_len;

	if (device_path[0] && access(device_path, R_OK) == 0)
		return 0;

	device_path[0] = '\0';
	id_len = strlen(device_id);
	if (!(dir = opendir(RAZER_DRIVER_DIR)))
		return -1;

	while ((entry = readdir(dir))) {
		if (strncmp(entry->d_name, device_id, id_len) != 0)
			continue;
		if (snprintf(path, sizeof(path), "%s/%s/dpi", RAZER_DRIVER_DIR,
		             entry->d_name) >= (int)sizeof(path))
			continue;
		if (access(path, R_OK) == 0) {
			snprintf(device_path, sizeof(device_path), "%s/%s",
			         RAZER_DRIVER_DIR, entry->d_name);
			break;
		}
	}
	closedir(dir);

	return device_path[0] ? 0 : -1;
}

static int
device_file(char *path, size_t size, const char *device_id, const char *name)
{
	if (find_device(device_id) < 0)
		return -1;
	if (snprintf(path, size, "%s/%s", device_path, name) >= (int)size)
		return -1;
	return 0;
}

const char *
razer_dpi(const char *device_id)
{
	char path[PATH_MAX];
	int x, y;

	if (device_file(path, sizeof(path), device_id, "dpi") < 0 ||
	    pscanf(path, "%d:%d", &x, &y) != 2) {
		device_path[0] = '\0';
		return NULL;
	}
	return bprintf("%d", x);
}

const char *
razer_battery(const char *device_id)
{
	char path[PATH_MAX];
	int raw;

	if (device_file(path, sizeof(path), device_id, "charge_level") < 0 ||
	    pscanf(path, "%d", &raw) != 1) {
		device_path[0] = '\0';
		return NULL;
	}
	if (raw < 0)
		raw = 0;
	if (raw > 255)
		raw = 255;
	return bprintf("%d", (raw * 100 + 127) / 255);
}

static int
write_bytes(const char *path, const unsigned char *data, size_t length)
{
	int fd;
	ssize_t written;

	if ((fd = open(path, O_WRONLY)) < 0)
	{
		warn("open %s", path);
		return -1;
	}
	written = write(fd, data, length);
	if (written != (ssize_t)length)
		warn("write %s", path);
	close(fd);
	return written == (ssize_t)length ? 0 : -1;
}

int
razer_adjust_dpi(const char *device_id, int delta)
{
	unsigned char dpi_data[4], stage_data[5];
	char dpi_path[PATH_MAX], stages_path[PATH_MAX];
	int dpi, ignored;

	if (device_file(dpi_path, sizeof(dpi_path), device_id, "dpi") < 0 ||
	    device_file(stages_path, sizeof(stages_path), device_id,
	                "dpi_stages") < 0 ||
	    pscanf(dpi_path, "%d:%d", &dpi, &ignored) != 2)
		return -1;

	dpi += delta;
	if (dpi < RAZER_DPI_MIN)
		dpi = RAZER_DPI_MIN;
	if (dpi > RAZER_DPI_MAX)
		dpi = RAZER_DPI_MAX;

	dpi_data[0] = stage_data[1] = (uint16_t)dpi >> 8;
	dpi_data[1] = stage_data[2] = (uint16_t)dpi & 0xff;
	dpi_data[2] = stage_data[3] = dpi_data[0];
	dpi_data[3] = stage_data[4] = dpi_data[1];
	stage_data[0] = 1;

	/* Keep the hardware DPI button locked to the newly selected value. */
	if (write_bytes(stages_path, stage_data, sizeof(stage_data)) < 0 ||
	    write_bytes(dpi_path, dpi_data, sizeof(dpi_data)) < 0)
		return -1;
	return dpi;
}

void
razer_click(const char *device_id, int button)
{
	int result = 0;

	if (button == 1)
		result = razer_adjust_dpi(device_id, RAZER_DPI_STEP);
	else if (button == 3)
		result = razer_adjust_dpi(device_id, -RAZER_DPI_STEP);
	if (result < 0)
		warnx("could not adjust Razer DPI");
}
