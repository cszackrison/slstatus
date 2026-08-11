/* See LICENSE file for copyright and license details. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CODEX_RESCAN_SECONDS 60
#define USAGE_LOG_READ_LIMIT (2 * 1024 * 1024)
#define OPENCODE_REFRESH_SECONDS (60 * 60)
#define OPENCODE_CACHE_MAX_AGE (OPENCODE_REFRESH_SECONDS + 5 * 60)
#define USAGE_BAR_WIDTH 5

#ifndef SLSTATUS_LIBEXEC
#define SLSTATUS_LIBEXEC "/usr/local/libexec/slstatus"
#endif

#define OPENCODE_HELPER SLSTATUS_LIBEXEC "/opencode-go-usage-cache"
#define AI_USAGE_MENU_HELPER SLSTATUS_LIBEXEC "/ai-usage-menu"

struct usage_window {
	double used;
	time_t resets_at;
	int present;
};

struct usage_snapshot {
	struct usage_window five_hour;
	struct usage_window weekly;
	struct usage_window monthly;
};

static struct usage_snapshot codex_usage;
static char codex_rollout[PATH_MAX];
static char codex_sessions_root[PATH_MAX];
static off_t codex_offset;
static time_t codex_next_scan;
static struct usage_snapshot grok_usage;
static char grok_log[PATH_MAX];
static off_t grok_offset;
static dev_t grok_device;
static ino_t grok_inode;
static pid_t opencode_updater = -1;
static time_t opencode_next_refresh;

static void
print_usage_line(const char *icon, const char *provider, const char *usage)
{
	printf("%s %-11s %s\n", icon, provider,
	       usage ? usage : "unavailable");
}

static int
has_suffix(const char *str, const char *suffix)
{
	size_t slen, suffixlen;

	slen = strlen(str);
	suffixlen = strlen(suffix);
	return slen >= suffixlen &&
	       !strcmp(str + slen - suffixlen, suffix);
}

static int
path_join(char *dst, size_t size, const char *left, const char *right)
{
	int n;

	n = snprintf(dst, size, "%s/%s", left, right);
	return n >= 0 && (size_t)n < size;
}

static int
default_path(char *dst, size_t size, const char *envname,
             const char *fallback_dir, const char *suffix)
{
	const char *base, *home;
	int n;

	base = getenv(envname);
	if (base && base[0]) {
		n = snprintf(dst, size, "%s/%s", base, suffix);
	} else {
		home = getenv("HOME");
		if (!home || !home[0])
			return 0;
		n = snprintf(dst, size, "%s/%s/%s", home, fallback_dir, suffix);
	}

	return n >= 0 && (size_t)n < size;
}

static const char *
json_value(const char *text, const char *key)
{
	const char *p;

	if (!(p = strstr(text, key)))
		return NULL;
	p += strlen(key);
	while (*p == ' ' || *p == '\t' || *p == '\r')
		p++;
	if (*p++ != ':')
		return NULL;
	while (*p == ' ' || *p == '\t' || *p == '\r')
		p++;

	return p;
}

static int
json_string(const char *text, const char *key, char *dst, size_t size)
{
	const char *p, *end;
	size_t len;

	if (!(p = json_value(text, key)) || *p++ != '"')
		return 0;
	if (!(end = strchr(p, '"')))
		return 0;
	len = (size_t)(end - p);
	if (len >= size)
		return 0;
	memcpy(dst, p, len);
	dst[len] = '\0';

	return 1;
}

static int
json_long(const char *text, const char *key, long long *value)
{
	const char *p;
	char *end;
	long long parsed;

	if (!(p = json_value(text, key)))
		return 0;
	errno = 0;
	parsed = strtoll(p, &end, 10);
	if (errno || end == p)
		return 0;
	*value = parsed;

	return 1;
}

static int
json_double(const char *text, const char *key, double *value)
{
	const char *p;
	char *end;
	double parsed;

	if (!(p = json_value(text, key)))
		return 0;
	errno = 0;
	parsed = strtod(p, &end);
	if (errno || end == p)
		return 0;
	*value = parsed;

	return 1;
}

static int
parse_window(const char *rate_limits, const char *key,
             struct usage_snapshot *snapshot)
{
	const char *p, *end;
	char object[512];
	long long minutes, resets_at;
	double used;
	size_t len;
	struct usage_window window;

	if (!(p = json_value(rate_limits, key)) || *p != '{')
		return 0;
	if (!(end = strchr(p, '}')))
		return 0;
	len = (size_t)(end - p + 1);
	if (len >= sizeof(object))
		return 0;
	memcpy(object, p, len);
	object[len] = '\0';

	if (!json_double(object, "\"used_percent\"", &used) ||
	    !json_long(object, "\"window_minutes\"", &minutes) ||
	    !json_long(object, "\"resets_at\"", &resets_at) ||
	    used < 0.0 || used > 100.0 || resets_at <= 0)
		return 0;

	window.used = used;
	window.resets_at = (time_t)resets_at;
	window.present = 1;
	if (minutes == 300)
		snapshot->five_hour = window;
	else if (minutes == 10080)
		snapshot->weekly = window;
	else
		return 0;

	return 1;
}

/* Returns 1 for a subscription snapshot, -1 for an explicit null snapshot. */
static int
parse_codex_line(char *line, struct usage_snapshot *snapshot)
{
	const char *rate_limits;
	char limit_id[64], plan_type[64];
	struct usage_snapshot parsed = {0};

	if (!strstr(line, "\"type\":\"token_count\"") &&
	    !strstr(line, "\"type\": \"token_count\""))
		return 0;
	if (!(rate_limits = json_value(line, "\"rate_limits\"")))
		return 0;
	if (!strncmp(rate_limits, "null", 4))
		return -1;
	if (*rate_limits != '{' ||
	    !json_string(rate_limits, "\"limit_id\"", limit_id,
	                 sizeof(limit_id)) || strcmp(limit_id, "codex") ||
	    !json_string(rate_limits, "\"plan_type\"", plan_type,
	                 sizeof(plan_type)) || !strcmp(plan_type, "unknown"))
		return 0;

	parse_window(rate_limits, "\"primary\"", &parsed);
	parse_window(rate_limits, "\"secondary\"", &parsed);
	*snapshot = parsed;

	return 1;
}

static void
parse_codex_buffer(char *data, size_t len)
{
	char *line, *end;
	int result;

	line = data;
	while (line < data + len && (end = memchr(line, '\n',
	       (size_t)(data + len - line)))) {
		*end = '\0';
		result = parse_codex_line(line, &codex_usage);
		if (result < 0)
			memset(&codex_usage, 0, sizeof(codex_usage));
		line = end + 1;
	}
}

static int
parse_iso8601(const char *text, time_t *when)
{
	struct tm tm, check;
	const char *p;
	time_t parsed;
	int year, month, day, hour, minute, second;
	int offset_hour = 0, offset_minute = 0, offset_sign = 0;

	if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month,
	           &day, &hour, &minute, &second) != 6)
		return 0;
	p = text + 19;
	if (*p == '.') {
		p++;
		if (*p < '0' || *p > '9')
			return 0;
		while (*p >= '0' && *p <= '9')
			p++;
	}
	if (*p == 'Z' && !p[1]) {
		p++;
	} else if ((*p == '+' || *p == '-') && strlen(p) == 6 &&
	           sscanf(p + 1, "%2d:%2d", &offset_hour,
	                  &offset_minute) == 2) {
		offset_sign = *p == '+' ? 1 : -1;
		p += 6;
	} else {
		return 0;
	}
	if (*p || year < 1970 || month < 1 || month > 12 || day < 1 ||
	    day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
	    second < 0 || second > 59 || offset_hour > 23 ||
	    offset_minute > 59)
		return 0;

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = year - 1900;
	tm.tm_mon = month - 1;
	tm.tm_mday = day;
	tm.tm_hour = hour;
	tm.tm_min = minute;
	tm.tm_sec = second;
	parsed = timegm(&tm);
	if (parsed < 0 || !gmtime_r(&parsed, &check) ||
	    check.tm_year != year - 1900 || check.tm_mon != month - 1 ||
	    check.tm_mday != day || check.tm_hour != hour ||
	    check.tm_min != minute || check.tm_sec != second)
		return 0;
	parsed -= offset_sign * (offset_hour * 60 + offset_minute) * 60;
	*when = parsed;

	return 1;
}

static int
parse_grok_line(char *line, struct usage_snapshot *snapshot)
{
	const char *p, *end;
	char object[512], period_type[64], resets[64];
	struct usage_snapshot parsed = {0};
	struct usage_window window;
	double used;
	size_t len;

	if (!json_double(line, "\"creditUsagePercent\"", &used) ||
	    used < 0.0 || used > 100.0 ||
	    !(p = json_value(line, "\"currentPeriod\"")) || *p != '{' ||
	    !(end = strchr(p, '}')))
		return 0;
	len = (size_t)(end - p + 1);
	if (len >= sizeof(object))
		return 0;
	memcpy(object, p, len);
	object[len] = '\0';
	if (!json_string(object, "\"type\"", period_type,
	                 sizeof(period_type)) ||
	    !json_string(object, "\"end\"", resets, sizeof(resets)) ||
	    !parse_iso8601(resets, &window.resets_at))
		return 0;

	window.used = used;
	window.present = 1;
	if (!strcmp(period_type, "USAGE_PERIOD_TYPE_WEEKLY"))
		parsed.weekly = window;
	else if (!strcmp(period_type, "USAGE_PERIOD_TYPE_MONTHLY"))
		parsed.monthly = window;
	else
		return 0;
	*snapshot = parsed;

	return 1;
}

static void
parse_grok_buffer(char *data, size_t len)
{
	char *line, *end;

	line = data;
	while (line < data + len && (end = memchr(line, '\n',
	       (size_t)(data + len - line)))) {
		*end = '\0';
		parse_grok_line(line, &grok_usage);
		line = end + 1;
	}
}

static int
rollout_is_newer(const struct stat *st, const char *path, time_t best_mtime,
                 const char *best_path)
{
	return st->st_mtime > best_mtime ||
	       (st->st_mtime == best_mtime && strcmp(path, best_path) > 0);
}

static void
scan_rollouts(const char *dir, int depth, char *best, size_t best_size,
              time_t *best_mtime)
{
	struct dirent *entry;
	struct stat st;
	char path[PATH_MAX];
	DIR *dp;

	if (!(dp = opendir(dir)))
		return;
	while ((entry = readdir(dp))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
		    !path_join(path, sizeof(path), dir, entry->d_name) ||
		    stat(path, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode) && depth < 3) {
			scan_rollouts(path, depth + 1, best, best_size,
			              best_mtime);
		} else if (S_ISREG(st.st_mode) &&
		           !strncmp(entry->d_name, "rollout-", 8) &&
		           has_suffix(entry->d_name, ".jsonl") &&
		           rollout_is_newer(&st, path, *best_mtime, best)) {
			snprintf(best, best_size, "%s", path);
			*best_mtime = st.st_mtime;
		}
	}
	closedir(dp);
}

static void
scan_day(time_t when, char *best, size_t best_size, time_t *best_mtime)
{
	struct tm tm;
	char day[32], path[PATH_MAX];

	if (!localtime_r(&when, &tm) ||
	    !strftime(day, sizeof(day), "%Y/%m/%d", &tm) ||
	    !path_join(path, sizeof(path), codex_sessions_root, day))
		return;
	scan_rollouts(path, 3, best, best_size, best_mtime);
}

static void
select_codex_rollout(int full_scan)
{
	struct stat st;
	char best[PATH_MAX] = "";
	time_t best_mtime = 0, now;

	if (!full_scan && codex_rollout[0] && !stat(codex_rollout, &st)) {
		snprintf(best, sizeof(best), "%s", codex_rollout);
		best_mtime = st.st_mtime;
	}

	now = time(NULL);
	if (full_scan || !best[0])
		scan_rollouts(codex_sessions_root, 0, best, sizeof(best),
		              &best_mtime);
	else {
		scan_day(now, best, sizeof(best), &best_mtime);
		scan_day(now - 24 * 60 * 60, best, sizeof(best), &best_mtime);
	}

	if (strcmp(best, codex_rollout)) {
		snprintf(codex_rollout, sizeof(codex_rollout), "%s", best);
		codex_offset = 0;
		memset(&codex_usage, 0, sizeof(codex_usage));
	}
	codex_next_scan = now + CODEX_RESCAN_SECONDS;
}

static void
read_log_updates(const char *path, off_t *offset,
                 void (*parse_buffer)(char *, size_t))
{
	struct stat st;
	char *data;
	ssize_t nread;
	off_t start, last_complete;
	size_t length, skip;
	int fd;

	if (!path[0] || stat(path, &st) < 0)
		return;
	if (st.st_size == *offset)
		return;

	start = *offset;
	if (start < 0 || start > st.st_size ||
	    st.st_size - start > USAGE_LOG_READ_LIMIT) {
		start = st.st_size > USAGE_LOG_READ_LIMIT ?
		        st.st_size - USAGE_LOG_READ_LIMIT : 0;
	}
	length = (size_t)(st.st_size - start);
	if (!length || !(data = malloc(length + 1)))
		return;
	if ((fd = open(path, O_RDONLY)) < 0) {
		free(data);
		return;
	}
	if (lseek(fd, start, SEEK_SET) < 0 ||
	    (nread = read(fd, data, length)) <= 0) {
		close(fd);
		free(data);
		return;
	}
	close(fd);
	data[nread] = '\0';

	skip = 0;
	if (start > 0 && start != *offset) {
		char *newline = memchr(data, '\n', (size_t)nread);
		if (!newline) {
			free(data);
			return;
		}
		skip = (size_t)(newline - data + 1);
	}
	last_complete = -1;
	for (ssize_t i = nread - 1; i >= (ssize_t)skip; i--) {
		if (data[i] == '\n') {
			last_complete = i;
			break;
		}
	}
	if (last_complete >= 0) {
		parse_buffer(data + skip, (size_t)last_complete + 1 - skip);
		*offset = start + last_complete + 1;
	}
	free(data);
}

static void
read_codex_updates(void)
{
	read_log_updates(codex_rollout, &codex_offset, parse_codex_buffer);
}

static int
parse_cache_token(const char *token, double *number)
{
	char *end;
	double value;

	if (!strcmp(token, "-"))
		return 0;
	errno = 0;
	value = strtod(token, &end);
	if (errno || end == token || *end || value < 0.0 || value > 100.0)
		return 0;
	*number = value;

	return 1;
}

static int
parse_cache_time(const char *token, time_t *when)
{
	char *end;
	long long value;

	if (!strcmp(token, "-"))
		return 0;
	errno = 0;
	value = strtoll(token, &end, 10);
	if (errno || end == token || *end || value <= 0)
		return 0;
	*when = (time_t)value;

	return 1;
}

static int
read_claude_cache(const char *path, struct usage_snapshot *snapshot)
{
	char version[8], five_used[32], five_reset[32];
	char week_used[32], week_reset[32];
	double used;
	time_t resets_at;
	FILE *fp;

	memset(snapshot, 0, sizeof(*snapshot));
	if (!(fp = fopen(path, "r")))
		return 0;
	if (fscanf(fp, "%7s %31s %31s %31s %31s", version, five_used,
	           five_reset, week_used, week_reset) != 5 ||
	    strcmp(version, "v1")) {
		fclose(fp);
		return 0;
	}
	fclose(fp);

	if (parse_cache_token(five_used, &used) &&
	    parse_cache_time(five_reset, &resets_at)) {
		snapshot->five_hour.used = used;
		snapshot->five_hour.resets_at = resets_at;
		snapshot->five_hour.present = 1;
	}
	if (parse_cache_token(week_used, &used) &&
	    parse_cache_time(week_reset, &resets_at)) {
		snapshot->weekly.used = used;
		snapshot->weekly.resets_at = resets_at;
		snapshot->weekly.present = 1;
	}

	return snapshot->five_hour.present || snapshot->weekly.present;
}

static int
read_opencode_cache(const char *path, struct usage_snapshot *snapshot)
{
	char version[8], fetched[32], five_used[32], five_reset[32];
	char week_used[32], week_reset[32], month_used[32], month_reset[32];
	double used;
	time_t fetched_at, now, resets_at;
	FILE *fp;

	memset(snapshot, 0, sizeof(*snapshot));
	if (!(fp = fopen(path, "r")))
		return 0;
	if (fscanf(fp, "%7s %31s %31s %31s %31s %31s %31s %31s",
	           version, fetched, five_used, five_reset, week_used,
	           week_reset, month_used, month_reset) != 8 ||
	    strcmp(version, "v1")) {
		fclose(fp);
		return 0;
	}
	fclose(fp);

	now = time(NULL);
	if (!parse_cache_time(fetched, &fetched_at) || fetched_at > now + 60 ||
	    now - fetched_at > OPENCODE_CACHE_MAX_AGE)
		return 0;
	if (parse_cache_token(five_used, &used) &&
	    parse_cache_time(five_reset, &resets_at)) {
		snapshot->five_hour.used = used;
		snapshot->five_hour.resets_at = resets_at;
		snapshot->five_hour.present = 1;
	}
	if (parse_cache_token(week_used, &used) &&
	    parse_cache_time(week_reset, &resets_at)) {
		snapshot->weekly.used = used;
		snapshot->weekly.resets_at = resets_at;
		snapshot->weekly.present = 1;
	}
	if (parse_cache_token(month_used, &used) &&
	    parse_cache_time(month_reset, &resets_at)) {
		snapshot->monthly.used = used;
		snapshot->monthly.resets_at = resets_at;
		snapshot->monthly.present = 1;
	}

	return snapshot->five_hour.present || snapshot->weekly.present ||
	       snapshot->monthly.present;
}

static int
remaining_percent(double used)
{
	int remaining;

	remaining = (int)(100.0 - used + 0.5);
	if (remaining < 0)
		remaining = 0;
	else if (remaining > 100)
		remaining = 100;

	return remaining;
}

static int
format_usage_bar(char *bar, size_t size, int remaining)
{
	static const char *partial[] = { "", "▏", "▎", "▍", "▌", "▋", "▊", "▉" };
	size_t used = 0;
	int eighths, i, length;
	const char *cell;

	eighths = (remaining * USAGE_BAR_WIDTH * 8 + 50) / 100;
	for (i = 0; i < USAGE_BAR_WIDTH; i++) {
		if (eighths >= 8) {
			cell = "█";
			eighths -= 8;
		} else if (eighths > 0) {
			cell = partial[eighths];
			eighths = 0;
		} else {
			cell = "░";
		}
		length = snprintf(bar + used, size - used, "%s", cell);
		if (length < 0 || (size_t)length >= size - used)
			return 0;
		used += (size_t)length;
	}

	return 1;
}

static int
append_usage_window(char *dst, size_t size, size_t *used,
                    const char *label, struct usage_window *window,
                    time_t now)
{
	char bar[32];
	int length, remaining;

	if (!window->present || window->resets_at <= now)
		return 1;
	remaining = remaining_percent(window->used);
	if (!format_usage_bar(bar, sizeof(bar), remaining))
		return 0;
	length = snprintf(dst + *used, size - *used, "%s%s [%s] %d%%",
	                  *used ? " " : "", label, bar, remaining);
	if (length < 0 || (size_t)length >= size - *used)
		return 0;
	*used += (size_t)length;

	return 1;
}

static const char *
format_usage(struct usage_snapshot *snapshot)
{
	time_t now;
	size_t used = 0;

	now = time(NULL);
	buf[0] = '\0';
	if (!append_usage_window(buf, sizeof(buf), &used, "5hr",
	                         &snapshot->five_hour, now) ||
	    !append_usage_window(buf, sizeof(buf), &used, "wk",
	                         &snapshot->weekly, now) ||
	    !append_usage_window(buf, sizeof(buf), &used, "mo",
	                         &snapshot->monthly, now))
		return NULL;

	return used ? buf : NULL;
}

static void
start_opencode_update(time_t now)
{
	pid_t result;

	if (opencode_updater > 0) {
		result = waitpid(opencode_updater, NULL, WNOHANG);
		if (result == 0)
			return;
		if (result == opencode_updater || (result < 0 && errno != EINTR))
			opencode_updater = -1;
	}
	if (opencode_updater > 0 || now < opencode_next_refresh)
		return;
	opencode_next_refresh = now + OPENCODE_REFRESH_SECONDS;
	opencode_updater = fork();
	if (opencode_updater == 0) {
		execl(OPENCODE_HELPER, OPENCODE_HELPER, (char *)NULL);
		_exit(127);
	}
}

const char *
claude_subscription_usage(const char *cache_path)
{
	char path[PATH_MAX];
	struct usage_snapshot snapshot;

	if (!cache_path) {
		if (!default_path(path, sizeof(path), "XDG_CACHE_HOME", ".cache",
		                  "slstatus/claude-usage-v1"))
			return NULL;
		cache_path = path;
	}
	if (!read_claude_cache(cache_path, &snapshot))
		return NULL;

	return format_usage(&snapshot);
}

const char *
openai_subscription_usage(const char *sessions_root)
{
	char path[PATH_MAX];
	time_t now;

	if (!sessions_root) {
		if (!default_path(path, sizeof(path), "CODEX_HOME", ".codex",
		                  "sessions"))
			return NULL;
		sessions_root = path;
	}
	if (strcmp(sessions_root, codex_sessions_root)) {
		snprintf(codex_sessions_root, sizeof(codex_sessions_root), "%s",
		         sessions_root);
		codex_rollout[0] = '\0';
		codex_offset = 0;
		codex_next_scan = 0;
		memset(&codex_usage, 0, sizeof(codex_usage));
	}

	now = time(NULL);
	if (!codex_rollout[0])
		select_codex_rollout(1);
	else if (now >= codex_next_scan)
		select_codex_rollout(0);
	read_codex_updates();

	return format_usage(&codex_usage);
}

const char *
grok_subscription_usage(const char *log_path)
{
	struct stat st;
	char path[PATH_MAX];

	if (!log_path) {
		if (!default_path(path, sizeof(path), "GROK_HOME", ".grok",
		                  "logs/unified.jsonl"))
			return NULL;
		log_path = path;
	}
	if (strcmp(log_path, grok_log)) {
		snprintf(grok_log, sizeof(grok_log), "%s", log_path);
		grok_offset = 0;
		grok_device = 0;
		grok_inode = 0;
		memset(&grok_usage, 0, sizeof(grok_usage));
	}
	if (stat(grok_log, &st) < 0 || !S_ISREG(st.st_mode))
		return NULL;
	if (grok_device != st.st_dev || grok_inode != st.st_ino) {
		grok_offset = 0;
		grok_device = st.st_dev;
		grok_inode = st.st_ino;
		memset(&grok_usage, 0, sizeof(grok_usage));
	}
	read_log_updates(grok_log, &grok_offset, parse_grok_buffer);

	return format_usage(&grok_usage);
}

const char *
opencode_go_usage(const char *cache_path)
{
	char path[PATH_MAX];
	struct usage_snapshot snapshot;
	time_t now;

	now = time(NULL);
	if (!cache_path) {
		start_opencode_update(now);
		if (!default_path(path, sizeof(path), "XDG_CACHE_HOME", ".cache",
		                  "slstatus/opencode-go-usage-v1"))
			return NULL;
		cache_path = path;
	}
	if (!read_opencode_cache(cache_path, &snapshot))
		return NULL;

	return format_usage(&snapshot);
}

void
ai_usage_report(void)
{
	char path[PATH_MAX];
	const char *usage;

	usage = claude_subscription_usage(NULL);
	print_usage_line("", "Claude", usage);
	usage = openai_subscription_usage(NULL);
	print_usage_line("", "OpenAI", usage);
	usage = grok_subscription_usage(NULL);
	print_usage_line("", "Grok", usage);
	usage = default_path(path, sizeof(path), "XDG_CACHE_HOME", ".cache",
	                     "slstatus/opencode-go-usage-v1") ?
	        opencode_go_usage(path) : NULL;
	print_usage_line("󰚩", "OpenCode Go", usage);
}

void
ai_usage_menu(int button)
{
	pid_t child, launcher;

	if (button != 1 || (launcher = fork()) < 0)
		return;
	if (launcher == 0) {
		child = fork();
		if (child == 0) {
			execl(AI_USAGE_MENU_HELPER, AI_USAGE_MENU_HELPER,
			      (char *)NULL);
			_exit(127);
		}
		_exit(child < 0 ? 127 : 0);
	}
	while (waitpid(launcher, NULL, 0) < 0 && errno == EINTR)
		;
}
