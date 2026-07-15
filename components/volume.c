/* See LICENSE file for copyright and license details. */
#include <err.h>
#include <fcntl.h>
#include <sys/soundcard.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../util.h"

const char *
vol_perc(const char *card)
{
	unsigned int i;
	int v, afd, devmask;
	char *vnames[] = SOUND_DEVICE_NAMES;

	afd = open(card, O_RDONLY | O_NONBLOCK);
	if (afd == -1) {
		warn("Cannot open %s", card);
		return NULL;
	}

	if (ioctl(afd, SOUND_MIXER_READ_DEVMASK, &devmask) == -1) {
		warn("Cannot get volume for %s", card);
		close(afd);
		return NULL;
	}
	for (i = 0; i < LEN(vnames); i++) {
		if (devmask & (1 << i) && !strcmp("vol", vnames[i])) {
			if (ioctl(afd, MIXER_READ(i), &v) == -1) {
				warn("vol_perc: ioctl");
				close(afd);
				return NULL;
			}
		}
	}

	close(afd);

	return bprintf("%d", v & 0xff);
}

const char *
vol_status(const char *unused)
{
	FILE *fp;
	char line[512], *pct;
	int muted = 0, volume = -1;

	(void)unused;

	if ((fp = popen("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null", "r"))) {
		if (fgets(line, sizeof(line), fp) && strstr(line, "yes"))
			muted = 1;
		pclose(fp);
	}

	if ((fp = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r"))) {
		while (fgets(line, sizeof(line), fp)) {
			if ((pct = strchr(line, '%'))) {
				char *start = pct;
				while (start > line && isdigit((unsigned char)start[-1]))
					start--;
				if (start != pct) {
					volume = atoi(start);
					break;
				}
			}
		}
		pclose(fp);
	}

	if (volume < 0 && (fp = popen("amixer sget Master 2>/dev/null", "r"))) {
		while (fgets(line, sizeof(line), fp)) {
			if (strstr(line, "[off]"))
				muted = 1;
			if ((pct = strchr(line, '%'))) {
				char *start = pct;
				while (start > line && isdigit((unsigned char)start[-1]))
					start--;
				if (start != pct)
					volume = atoi(start);
			}
		}
		pclose(fp);
	}

	if (muted)
		return bprintf("󰖁 mute");
	if (volume < 0)
		return NULL;
	if (volume == 0)
		return bprintf("󰕿 %d%%", volume);
	if (volume < 50)
		return bprintf("󰕿 %d%%", volume);
	if (volume < 80)
		return bprintf("󰖀 %d%%", volume);
	return bprintf("󰕾 %d%%", volume);
}

static int
sink_volume(void)
{
	FILE *fp;
	char line[512], *pct;
	int volume = -1;

	if (!(fp = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r")))
		return -1;
	while (fgets(line, sizeof(line), fp)) {
		if ((pct = strchr(line, '%'))) {
			char *start = pct;
			while (start > line && isdigit((unsigned char)start[-1]))
				start--;
			if (start != pct) {
				volume = atoi(start);
				break;
			}
		}
	}
	pclose(fp);

	return volume;
}

void
volume_click(int button)
{
	char cmd[64];
	int volume;

	if (button == 2) {
		(void)system("pactl set-sink-mute @DEFAULT_SINK@ toggle");
		return;
	}
	if (button != 1 && button != 3)
		return;

	if ((volume = sink_volume()) < 0)
		return;
	volume += button == 1 ? 10 : -10;
	if (volume < 0)
		volume = 0;
	else if (volume > 100)
		volume = 100;

	snprintf(cmd, sizeof(cmd), "pactl set-sink-volume @DEFAULT_SINK@ %d%%", volume);
	(void)system(cmd);
}
