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
#include <time.h>
#include <unistd.h>

#include "../util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CODEX_RESCAN_SECONDS 60
#define CODEX_READ_LIMIT (2 * 1024 * 1024)
#define USAGE_BAR_WIDTH 5

struct usage_window {
	double used;
	time_t resets_at;
	int present;
};

struct usage_snapshot {
	struct usage_window five_hour;
	struct usage_window weekly;
};

static struct usage_snapshot codex_usage;
static char codex_rollout[PATH_MAX];
static char codex_sessions_root[PATH_MAX];
static off_t codex_offset;
static time_t codex_next_scan;

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
read_codex_updates(void)
{
	struct stat st;
	char *data;
	ssize_t nread;
	off_t start, last_complete;
	size_t length, skip;
	int fd;

	if (!codex_rollout[0] || stat(codex_rollout, &st) < 0)
		return;
	if (st.st_size == codex_offset)
		return;

	start = codex_offset;
	if (start < 0 || start > st.st_size ||
	    st.st_size - start > CODEX_READ_LIMIT) {
		start = st.st_size > CODEX_READ_LIMIT ?
		        st.st_size - CODEX_READ_LIMIT : 0;
	}
	length = (size_t)(st.st_size - start);
	if (!length || !(data = malloc(length + 1)))
		return;
	if ((fd = open(codex_rollout, O_RDONLY)) < 0) {
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
	if (start > 0 && start != codex_offset) {
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
		parse_codex_buffer(data + skip,
		                   (size_t)last_complete + 1 - skip);
		codex_offset = start + last_complete + 1;
	}
	free(data);
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

static const char *
format_usage(struct usage_snapshot *snapshot)
{
	time_t now;
	char five_bar[32], week_bar[32];
	int five, five_remaining, week, week_remaining;

	now = time(NULL);
	five = snapshot->five_hour.present &&
	       snapshot->five_hour.resets_at > now;
	week = snapshot->weekly.present && snapshot->weekly.resets_at > now;
	five_remaining = five ? remaining_percent(snapshot->five_hour.used) : 0;
	week_remaining = week ? remaining_percent(snapshot->weekly.used) : 0;
	if ((five && !format_usage_bar(five_bar, sizeof(five_bar),
	                               five_remaining)) ||
	    (week && !format_usage_bar(week_bar, sizeof(week_bar),
	                               week_remaining)))
		return NULL;
	if (five && week)
		return bprintf("5hr [%s] %d%% wk [%s] %d%%", five_bar,
		               five_remaining, week_bar, week_remaining);
	if (five)
		return bprintf("5hr [%s] %d%%", five_bar, five_remaining);
	if (week)
		return bprintf("wk [%s] %d%%", week_bar, week_remaining);

	return NULL;
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
