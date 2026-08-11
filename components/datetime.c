/* See LICENSE file for copyright and license details. */
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../util.h"

#ifndef SLSTATUS_LIBEXEC
#define SLSTATUS_LIBEXEC "/usr/local/libexec/slstatus"
#endif

#define CALENDAR_HELPER SLSTATUS_LIBEXEC "/calendar-popup"

const char *
datetime(const char *fmt)
{
	time_t t;

	t = time(NULL);
	if (strftime(buf, sizeof(buf), fmt, localtime(&t)) == 0)
		return NULL;

	return buf;
}

void
datetime_click(int button)
{
	pid_t child, launcher;

	if (button != 1 || (launcher = fork()) < 0)
		return;
	if (launcher == 0) {
		child = fork();
		if (child == 0) {
			execl(CALENDAR_HELPER, CALENDAR_HELPER, (char *)NULL);
			_exit(127);
		}
		_exit(child < 0 ? 127 : 0);
	}
	while (waitpid(launcher, NULL, 0) < 0 && errno == EINTR)
		;
}
