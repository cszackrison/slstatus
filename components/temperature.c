/* See LICENSE file for copyright and license details. */
#include <stdio.h>
#include <string.h>

#include "../util.h"

static int
read_temp(const char *file, int *temp)
{
	FILE *fp;
	int n;

	if (!(fp = fopen(file, "r")))
		return 0;
	n = fscanf(fp, "%d", temp);
	fclose(fp);

	return n == 1;
}

const char *
temp(const char *file)
{
	int temp;

	return read_temp(file, &temp) ?
	       bprintf("%d", temp / 1000) : NULL;
}

const char *
temp_fallback(const char *candidates)
{
	char list[512], *candidate;
	int i, temp;

	if (!candidates)
		return NULL;

	snprintf(list, sizeof(list), "%s", candidates);
	for (candidate = strtok(list, ","); candidate; candidate = strtok(NULL, ",")) {
		while (*candidate == ' ' || *candidate == '\t')
			candidate++;

		if (!strncmp(candidate, "hwmon=", 6)) {
			char namefile[64], tempfile[64], name[64];
			const char *want = candidate + 6;
			FILE *fp;

			for (i = 0; i < 64; i++) {
				snprintf(namefile, sizeof(namefile),
				         "/sys/class/hwmon/hwmon%d/name", i);
				if (!(fp = fopen(namefile, "r")))
					continue;
				if (!fgets(name, sizeof(name), fp)) {
					fclose(fp);
					continue;
				}
				fclose(fp);
				name[strcspn(name, "\n")] = '\0';
				if (strcmp(name, want))
					continue;

				snprintf(tempfile, sizeof(tempfile),
				         "/sys/class/hwmon/hwmon%d/temp1_input", i);
				if (read_temp(tempfile, &temp))
					return bprintf("%d", temp / 1000);
			}
		} else if (read_temp(candidate, &temp)) {
			return bprintf("%d", temp / 1000);
		}
	}

	return NULL;
}
