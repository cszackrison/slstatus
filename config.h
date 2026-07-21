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
	{ claude_subscription_usage, "| Claude %s ", NULL, 1 },
	{ openai_subscription_usage, "| OpenAI %s ", NULL, 1 },
	/*{ disk_free, " %.5s GiB | ", "/" },
	{ ipv4, "E: %s | ", "enp9s0" },*/
	/*{ wifi_perc, "W: (%3s%% on ", "wlp8s0" },
	{ wifi_essid, "%s) ", "wlp8s0" },*/
	/*{ ipv4, "%s | ", "wlp8s0" },*/
	{ vol_status, "| \x01%s  ", NULL, 0 },
	{ mic_status, "\x04%s  ", NULL, 1 },
	{ razer_dpi, "\x02󰍽 %s DPI  ", "0003:1532:0083.", 1 },
	{ razer_battery, "󰁹 %s%%  ", "0003:1532:0083.", 1 },
	/*{ battery_state, "%s ", "BAT1" },*/
	/*{ battery_perc, "%3s%% | ", "BAT1" },*/
	{ temp_fallback, "\x03󰔏 %s° | ", "/sys/class/thermal/thermal_zone2/temp,hwmon=k10temp,hwmon=coretemp,/sys/class/thermal/thermal_zone0/temp", 0 },
	{ datetime, "%s", "%D %a %I:%M", 0 },
};

/*
	see formatting here: $ man strftime
*/
