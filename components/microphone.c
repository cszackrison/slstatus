/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../util.h"

static int
get_input_source(char *source, size_t size)
{
	FILE *fp;
	char line[512], name[256];

	if ((fp = popen("pactl get-default-source 2>/dev/null", "r"))) {
		if (fgets(line, sizeof(line), fp)) {
			line[strcspn(line, "\n")] = '\0';
			if (!strstr(line, ".monitor") && line[0]) {
				if (strlen(line) >= size) {
					pclose(fp);
					return 0;
				}
				strcpy(source, line);
				pclose(fp);
				return 1;
			}
		}
		pclose(fp);
	}

	if (!(fp = popen("pactl list sources short 2>/dev/null", "r")))
		return snprintf(source, size, "@DEFAULT_SOURCE@") > 0;

	while (fgets(line, sizeof(line), fp)) {
		if (!strstr(line, ".monitor")) {
			if (sscanf(line, "%*s %255s", name) != 1) {
				pclose(fp);
				return 0;
			}
			snprintf(source, size, "%s", name);
			pclose(fp);
			return 1;
		}
	}

	pclose(fp);
	return 0;
}

const char *
mic_perc(const char *unused)
{
	FILE *fp;
	char cmd[512], line[512], source[256], *pct;
	int muted = 0;

	(void)unused;

	if (!get_input_source(source, sizeof(source)))
		return NULL;

	snprintf(cmd, sizeof(cmd), "pactl get-source-mute '%s' 2>/dev/null", source);
	if ((fp = popen(cmd, "r"))) {
		if (fgets(line, sizeof(line), fp) && strstr(line, "yes"))
			muted = 1;
		pclose(fp);
	}

	if (muted)
		return bprintf("mute");

	snprintf(cmd, sizeof(cmd), "pactl get-source-volume '%s' 2>/dev/null", source);
	if (!(fp = popen(cmd, "r")))
		return NULL;

	while (fgets(line, sizeof(line), fp)) {
		if ((pct = strchr(line, '%'))) {
			char *start = pct;
			while (start > line && isdigit((unsigned char)start[-1]))
				start--;
			pclose(fp);
			return bprintf("%.*s%%", (int)(pct - start), start);
		}
	}

	pclose(fp);
	return NULL;
}

const char *
mic_status(const char *unused)
{
	const char *perc = mic_perc(unused);
	char status[32];

	if (!perc)
		return NULL;
	snprintf(status, sizeof(status), "%s", perc);
	if (!strcmp(status, "mute"))
		return bprintf("󰍭 mute");
	return bprintf("󰍬 %s", status);
}

static int
source_volume(const char *source)
{
	FILE *fp;
	char cmd[512], line[512], *pct;
	int volume = -1;

	snprintf(cmd, sizeof(cmd), "pactl get-source-volume '%s' 2>/dev/null", source);
	if (!(fp = popen(cmd, "r")))
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
mic_click(int button)
{
	char cmd[512], source[256];
	int volume;

	if (!get_input_source(source, sizeof(source)))
		return;

	if (button == 2) {
		snprintf(cmd, sizeof(cmd), "pactl set-source-mute '%s' toggle", source);
		(void)system(cmd);
		return;
	}
	if (button != 1 && button != 3)
		return;

	if ((volume = source_volume(source)) < 0)
		return;
	volume += button == 1 ? 10 : -10;
	if (volume < 0)
		volume = 0;
	else if (volume > 100)
		volume = 100;

	snprintf(cmd, sizeof(cmd), "pactl set-source-volume '%s' %d%%", source, volume);
	(void)system(cmd);
}
