/* Herdr agent status over the local Unix socket API. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../util.h"

#define HERDR_RESPONSE_LIMIT (1024U * 1024U)
#define HERDR_TIMEOUT_MS 250

struct cursor {
	const char *p;
	const char *end;
};

struct state_counts {
	size_t blocked;
	size_t done;
	size_t working;
	size_t idle;
	size_t unknown;
};

static int attention_frame;

static void
skip_space(struct cursor *cursor)
{
	while (cursor->p < cursor->end &&
	       (*cursor->p == ' ' || *cursor->p == '\t' ||
	        *cursor->p == '\r' || *cursor->p == '\n'))
		cursor->p++;
}

static int
parse_string(struct cursor *cursor, const char **start, size_t *length,
             int *plain)
{
	const char *begin;

	if (cursor->p >= cursor->end || *cursor->p++ != '"')
		return 0;
	begin = cursor->p;
	*plain = 1;
	while (cursor->p < cursor->end) {
		unsigned char ch = (unsigned char)*cursor->p++;

		if (ch == '"') {
			*start = begin;
			*length = (size_t)(cursor->p - begin - 1);
			return 1;
		}
		if (ch < 0x20)
			return 0;
		if (ch == '\\') {
			int i;

			*plain = 0;
			if (cursor->p >= cursor->end)
				return 0;
			ch = (unsigned char)*cursor->p++;
			if (ch == 'u') {
				for (i = 0; i < 4; i++) {
					if (cursor->p >= cursor->end ||
					    !((*cursor->p >= '0' && *cursor->p <= '9') ||
					      (*cursor->p >= 'a' && *cursor->p <= 'f') ||
					      (*cursor->p >= 'A' && *cursor->p <= 'F')))
						return 0;
					cursor->p++;
				}
			} else if (!strchr("\\\"/bfnrt", ch)) {
				return 0;
			}
		}
	}
	return 0;
}

static int
string_is(const char *start, size_t length, int plain, const char *expected)
{
	return plain && strlen(expected) == length &&
	       !memcmp(start, expected, length);
}

static int skip_value(struct cursor *, unsigned int);

static int
skip_number(struct cursor *cursor)
{
	const char *start = cursor->p;

	if (cursor->p < cursor->end && *cursor->p == '-')
		cursor->p++;
	if (cursor->p >= cursor->end)
		return 0;
	if (*cursor->p == '0') {
		cursor->p++;
	} else if (*cursor->p >= '1' && *cursor->p <= '9') {
		do {
			cursor->p++;
		} while (cursor->p < cursor->end && *cursor->p >= '0' &&
		         *cursor->p <= '9');
	} else {
		return 0;
	}
	if (cursor->p < cursor->end && *cursor->p == '.') {
		cursor->p++;
		if (cursor->p >= cursor->end || *cursor->p < '0' ||
		    *cursor->p > '9')
			return 0;
		while (cursor->p < cursor->end && *cursor->p >= '0' &&
		       *cursor->p <= '9')
			cursor->p++;
	}
	if (cursor->p < cursor->end &&
	    (*cursor->p == 'e' || *cursor->p == 'E')) {
		cursor->p++;
		if (cursor->p < cursor->end &&
		    (*cursor->p == '+' || *cursor->p == '-'))
			cursor->p++;
		if (cursor->p >= cursor->end || *cursor->p < '0' ||
		    *cursor->p > '9')
			return 0;
		while (cursor->p < cursor->end && *cursor->p >= '0' &&
		       *cursor->p <= '9')
			cursor->p++;
	}
	return cursor->p > start;
}

static int
skip_value(struct cursor *cursor, unsigned int depth)
{
	const char *start;
	size_t length;
	int plain;

	if (depth > 64)
		return 0;
	skip_space(cursor);
	if (cursor->p >= cursor->end)
		return 0;
	if (*cursor->p == '"')
		return parse_string(cursor, &start, &length, &plain);
	if (*cursor->p == '{') {
		cursor->p++;
		skip_space(cursor);
		if (cursor->p < cursor->end && *cursor->p == '}') {
			cursor->p++;
			return 1;
		}
		for (;;) {
			if (!parse_string(cursor, &start, &length, &plain))
				return 0;
			skip_space(cursor);
			if (cursor->p >= cursor->end || *cursor->p++ != ':')
				return 0;
			if (!skip_value(cursor, depth + 1))
				return 0;
			skip_space(cursor);
			if (cursor->p >= cursor->end)
				return 0;
			if (*cursor->p == '}') {
				cursor->p++;
				return 1;
			}
			if (*cursor->p++ != ',')
				return 0;
			skip_space(cursor);
		}
	}
	if (*cursor->p == '[') {
		cursor->p++;
		skip_space(cursor);
		if (cursor->p < cursor->end && *cursor->p == ']') {
			cursor->p++;
			return 1;
		}
		for (;;) {
			if (!skip_value(cursor, depth + 1))
				return 0;
			skip_space(cursor);
			if (cursor->p >= cursor->end)
				return 0;
			if (*cursor->p == ']') {
				cursor->p++;
				return 1;
			}
			if (*cursor->p++ != ',')
				return 0;
		}
	}
	if ((size_t)(cursor->end - cursor->p) >= 4 &&
	    (!memcmp(cursor->p, "true", 4) || !memcmp(cursor->p, "null", 4))) {
		cursor->p += 4;
		return 1;
	}
	if ((size_t)(cursor->end - cursor->p) >= 5 &&
	    !memcmp(cursor->p, "false", 5)) {
		cursor->p += 5;
		return 1;
	}
	return skip_number(cursor);
}

static int
parse_agent(struct cursor *cursor, struct state_counts *counts)
{
	const char *key, *value;
	size_t key_length, value_length;
	int key_plain, value_plain, found = 0;

	skip_space(cursor);
	if (cursor->p >= cursor->end || *cursor->p++ != '{')
		return 0;
	skip_space(cursor);
	if (cursor->p < cursor->end && *cursor->p == '}')
		return 0;
	for (;;) {
		if (!parse_string(cursor, &key, &key_length, &key_plain))
			return 0;
		skip_space(cursor);
		if (cursor->p >= cursor->end || *cursor->p++ != ':')
			return 0;
		skip_space(cursor);
		if (string_is(key, key_length, key_plain, "agent_status")) {
			if (found || !parse_string(cursor, &value, &value_length,
			                           &value_plain))
				return 0;
			found = 1;
			if (string_is(value, value_length, value_plain, "blocked"))
				counts->blocked++;
			else if (string_is(value, value_length, value_plain, "done"))
				counts->done++;
			else if (string_is(value, value_length, value_plain, "working"))
				counts->working++;
			else if (string_is(value, value_length, value_plain, "idle"))
				counts->idle++;
			else
				counts->unknown++;
		} else if (!skip_value(cursor, 1)) {
			return 0;
		}
		skip_space(cursor);
		if (cursor->p >= cursor->end)
			return 0;
		if (*cursor->p == '}') {
			cursor->p++;
			return found;
		}
		if (*cursor->p++ != ',')
			return 0;
		skip_space(cursor);
	}
}

static int
parse_agents(struct cursor *cursor, struct state_counts *counts)
{
	skip_space(cursor);
	if (cursor->p >= cursor->end || *cursor->p++ != '[')
		return 0;
	skip_space(cursor);
	if (cursor->p < cursor->end && *cursor->p == ']') {
		cursor->p++;
		return 1;
	}
	for (;;) {
		if (!parse_agent(cursor, counts))
			return 0;
		skip_space(cursor);
		if (cursor->p >= cursor->end)
			return 0;
		if (*cursor->p == ']') {
			cursor->p++;
			return 1;
		}
		if (*cursor->p++ != ',')
			return 0;
		skip_space(cursor);
	}
}

static int
parse_result(struct cursor *cursor, struct state_counts *counts)
{
	const char *key, *value;
	size_t key_length, value_length;
	int key_plain, value_plain;
	int agents_found = 0, type_found = 0;

	skip_space(cursor);
	if (cursor->p >= cursor->end || *cursor->p++ != '{')
		return 0;
	skip_space(cursor);
	if (cursor->p < cursor->end && *cursor->p == '}')
		return 0;
	for (;;) {
		if (!parse_string(cursor, &key, &key_length, &key_plain))
			return 0;
		skip_space(cursor);
		if (cursor->p >= cursor->end || *cursor->p++ != ':')
			return 0;
		skip_space(cursor);
		if (string_is(key, key_length, key_plain, "agents")) {
			if (agents_found || !parse_agents(cursor, counts))
				return 0;
			agents_found = 1;
		} else if (string_is(key, key_length, key_plain, "type")) {
			if (type_found || !parse_string(cursor, &value,
			                                &value_length, &value_plain) ||
			    !string_is(value, value_length, value_plain, "agent_list"))
				return 0;
			type_found = 1;
		} else if (!skip_value(cursor, 1)) {
			return 0;
		}
		skip_space(cursor);
		if (cursor->p >= cursor->end)
			return 0;
		if (*cursor->p == '}') {
			cursor->p++;
			return agents_found && type_found;
		}
		if (*cursor->p++ != ',')
			return 0;
		skip_space(cursor);
	}
}

static int
parse_response(const char *json, size_t length, struct state_counts *counts)
{
	struct cursor cursor = { json, json + length };
	const char *key;
	size_t key_length;
	int key_plain, result_found = 0;

	memset(counts, 0, sizeof(*counts));
	skip_space(&cursor);
	if (cursor.p >= cursor.end || *cursor.p++ != '{')
		return 0;
	skip_space(&cursor);
	if (cursor.p < cursor.end && *cursor.p == '}')
		return 0;
	for (;;) {
		if (!parse_string(&cursor, &key, &key_length, &key_plain))
			return 0;
		skip_space(&cursor);
		if (cursor.p >= cursor.end || *cursor.p++ != ':')
			return 0;
		skip_space(&cursor);
		if (string_is(key, key_length, key_plain, "result")) {
			if (result_found || !parse_result(&cursor, counts))
				return 0;
			result_found = 1;
		} else if (!skip_value(&cursor, 1)) {
			return 0;
		}
		skip_space(&cursor);
		if (cursor.p >= cursor.end)
			return 0;
		if (*cursor.p == '}') {
			cursor.p++;
			break;
		}
		if (*cursor.p++ != ',')
			return 0;
		skip_space(&cursor);
	}
	skip_space(&cursor);
	return result_found && cursor.p == cursor.end;
}

static long long
monotonic_milliseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
		return -1;
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int
wait_for(int fd, short events, long long deadline)
{
	struct pollfd pfd = { fd, events, 0 };
	long long now;
	int remaining, ready;

	for (;;) {
		now = monotonic_milliseconds();
		if (now < 0 || now >= deadline)
			return 0;
		remaining = (deadline - now > INT_MAX) ? INT_MAX :
		            (int)(deadline - now);
		ready = poll(&pfd, 1, remaining);
		if (ready > 0)
			return (pfd.revents & events) != 0;
		if (ready == 0)
			return 0;
		if (errno != EINTR)
			return 0;
	}
}

static int
resolve_socket_path(char *path, size_t size, const char *configured)
{
	const char *base;
	int length;

	if (configured && configured[0])
		length = snprintf(path, size, "%s", configured);
	else if ((base = getenv("HERDR_SOCKET_PATH")) && base[0])
		length = snprintf(path, size, "%s", base);
	else if ((base = getenv("XDG_CONFIG_HOME")) && base[0])
		length = snprintf(path, size, "%s/herdr/herdr.sock", base);
	else if ((base = getenv("HOME")) && base[0])
		length = snprintf(path, size, "%s/.config/herdr/herdr.sock", base);
	else
		return 0;
	return length >= 0 && (size_t)length < size;
}

static char *
read_response(const char *socket_path, size_t *response_length)
{
	static const char request[] =
		"{\"id\":\"slstatus:herdr\",\"method\":\"agent.list\",\"params\":{}}\n";
	struct sockaddr_un address;
	char *response = NULL, *grown;
	size_t capacity = 4096, length = 0, sent = 0;
	long long deadline;
	ssize_t count;
	int fd = -1, flags, error;
	socklen_t error_length = sizeof(error);

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	if (strlen(socket_path) >= sizeof(address.sun_path))
		return NULL;
	memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
	if ((deadline = monotonic_milliseconds()) < 0)
		return NULL;
	deadline += HERDR_TIMEOUT_MS;
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1 ||
	    (flags = fcntl(fd, F_GETFL, 0)) == -1 ||
	    fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
		goto fail;
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
		if (errno != EINPROGRESS || !wait_for(fd, POLLOUT, deadline) ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) == -1 ||
		    error != 0)
			goto fail;
	}
	while (sent < sizeof(request) - 1) {
		count = send(fd, request + sent, sizeof(request) - 1 - sent,
		             MSG_NOSIGNAL);
		if (count > 0) {
			sent += (size_t)count;
			continue;
		}
		if (count == -1 && errno == EINTR)
			continue;
		if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
		    wait_for(fd, POLLOUT, deadline))
			continue;
		goto fail;
	}
	if (!(response = malloc(capacity)))
		goto fail;
	for (;;) {
		if (length == capacity) {
			if (capacity >= HERDR_RESPONSE_LIMIT)
				goto fail;
			capacity *= 2;
			if (capacity > HERDR_RESPONSE_LIMIT)
				capacity = HERDR_RESPONSE_LIMIT;
			if (!(grown = realloc(response, capacity)))
				goto fail;
			response = grown;
		}
		count = recv(fd, response + length, capacity - length, 0);
		if (count > 0) {
			char *newline = memchr(response + length, '\n', (size_t)count);

			length += (size_t)count;
			if (newline) {
				length = (size_t)(newline - response);
				break;
			}
			continue;
		}
		if (count == 0) {
			if (!length)
				goto fail;
			break;
		}
		if (errno == EINTR)
			continue;
		if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
		    wait_for(fd, POLLIN, deadline))
			continue;
		goto fail;
	}
	close(fd);
	*response_length = length;
	return response;

fail:
	if (fd != -1)
		close(fd);
	free(response);
	return NULL;
}

static int
append_count(char *status, size_t size, size_t *used, const char *symbol,
             size_t count)
{
	int length;

	if (!count)
		return 1;
	length = snprintf(status + *used, size - *used, "%s%s%zu",
	                  *used ? " " : "", symbol, count);
	if (length < 0 || (size_t)length >= size - *used)
		return 0;
	*used += (size_t)length;
	return 1;
}

const char *
herdr_status(const char *configured_path)
{
	struct state_counts counts;
	char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	char status[256];
	char *response = NULL;
	const char *blocked_symbol = "!", *done_symbol = "";
	size_t length, total, used = 0;

	if (!resolve_socket_path(socket_path, sizeof(socket_path), configured_path) ||
	    !(response = read_response(socket_path, &length)) ||
	    !parse_response(response, length, &counts)) {
		free(response);
		attention_frame = 0;
		return NULL;
	}
	free(response);
	total = counts.blocked + counts.done + counts.working + counts.idle +
	        counts.unknown;
	if (!total) {
		attention_frame = 0;
		return NULL;
	}
	status[0] = '\0';
	if (counts.blocked || counts.done) {
		if (attention_frame) {
			blocked_symbol = " ";
			/* U+EC03 is an empty 13-pixel Nerd Font glyph. */
			done_symbol = "\xEE\xB0\x83";
		}
		attention_frame = !attention_frame;
	} else {
		attention_frame = 0;
	}
	if (!append_count(status, sizeof(status), &used, blocked_symbol,
	                  counts.blocked) ||
	    !append_count(status, sizeof(status), &used, done_symbol, counts.done) ||
	    !append_count(status, sizeof(status), &used, "…", counts.working) ||
	    !append_count(status, sizeof(status), &used, "○", counts.idle) ||
	    !append_count(status, sizeof(status), &used, "?", counts.unknown))
		return NULL;
	return bprintf("%s", status);
}
