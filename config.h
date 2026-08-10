/* slstatus configuration */
/* See LICENSE file for copyright and license details. */

/* interval between updates (in ms) */
static const int interval = 1000;

/* text to show if no value can be retrieved */
static const char unknown_str[] = "n/a";

/* maximum output string length */
#define MAXLEN 2048

/* padding inside status segments */
#define PAD " "

/* output formats */
static const struct arg args[] = {
	/* function, format, argument */
	{ herdr_status, "\x05" PAD "AGENTS %s ", NULL, 1 },
	{ claude_subscription_usage, "\x06" PAD " %s ", NULL, 1 },
	{ openai_subscription_usage, "\x07" PAD " %s ", NULL, 1 },
	{ opencode_go_usage, "\x0b" PAD "󰚩 %s ", NULL, 1 },
	/*{ disk_free, " %.5s GiB | ", "/" },
	{ ipv4, "E: %s | ", "enp9s0" },*/
	/*{ wifi_perc, "W: (%3s%% on ", "wlp8s0" },
	{ wifi_essid, "%s) ", "wlp8s0" },*/
	/*{ ipv4, "%s | ", "wlp8s0" },*/
	{ razer_dpi, "\x02" PAD "󰍽 %s DPI", "0003:1532:0083.", 1 },
	{ razer_battery, "\x08" PAD "󰁹 %s%%", "0003:1532:0083.", 1 },
	/*{ battery_state, "%s ", "BAT1" },*/
	/*{ battery_perc, "%3s%% | ", "BAT1" },*/
	{ vol_status, "\x01" PAD "%s", NULL, 0 },
	{ mic_status, "\x04" PAD "%s", NULL, 1 },
	{ temp_fallback, "\x03" PAD "󰔏 %s°", "/sys/class/thermal/thermal_zone2/temp,hwmon=k10temp,hwmon=coretemp,/sys/class/thermal/thermal_zone0/temp", 0 },
	{ datetime, "\x09" PAD " %s ", "%D %a %I:%M", 0 },
};

/*
	see formatting here: $ man strftime
*/
