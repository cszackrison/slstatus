/* slstatus configuration */
/* See LICENSE file for copyright and license details. */

/* interval between updates (in ms) */
static const int interval = 1000;

/* text to show if no value can be retrieved */
static const char unknown_str[] = "n/a";

/* maximum output string length */
#define MAXLEN 2048

/* output formats */
static const struct arg args[] = {
	/* function, format, argument */
	/*{ disk_free, " %.5s GiB | ", "/" },
	{ ipv4, "E: %s | ", "enp9s0" },*/
	/*{ wifi_perc, "W: (%3s%% on ", "wlp8s0" },
	{ wifi_essid, "%s) ", "wlp8s0" },*/
	/*{ ipv4, "%s | ", "wlp8s0" },*/
	{ run_command, "V: %4s | ", "amixer sget Master | awk -F\"[][]\" '/%/ { print $2 }' | head -n1" },
	/*{ battery_state, "%s ", "BAT1" },*/
	/*{ battery_perc, "%3s%% | ", "BAT1" },*/
	{ datetime, "%s", "[%a] [%D]%l:%M" },
};

/*
	see formatting here: $ man strftime
*/
