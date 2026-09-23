/* See LICENSE file for copyright and license details. */
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SLSTATUS_LIBEXEC
#define SLSTATUS_LIBEXEC "/usr/local/libexec/slstatus"
#endif

#define AI_USAGE_MENU_HELPER SLSTATUS_LIBEXEC "/ai-usage-menu"

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
