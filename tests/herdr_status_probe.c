#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

char buf[1024];

const char *herdr_status(const char *);

static int
serve_fixture(int listener, const char *fixture)
{
	char request[512], data[4096];
	struct timespec pause = { 0, 500000000 };
	ssize_t count;
	int client;
	FILE *fp;

	if ((client = accept(listener, NULL, NULL)) == -1)
		return 0;
	count = recv(client, request, sizeof(request) - 1, 0);
	if (count <= 0) {
		close(client);
		return 0;
	}
	request[count] = '\0';
	if (!strstr(request, "\"method\":\"agent.list\"")) {
		close(client);
		return 0;
	}
	if (!strcmp(fixture, "--stall")) {
		nanosleep(&pause, NULL);
		close(client);
		return 1;
	}
	if (!(fp = fopen(fixture, "r"))) {
		close(client);
		return 0;
	}
	while ((count = (ssize_t)fread(data, 1, sizeof(data), fp)) > 0) {
		ssize_t sent = 0, result;

		while (sent < count) {
			result = send(client, data + sent, (size_t)(count - sent),
			              MSG_NOSIGNAL);
			if (result <= 0) {
				fclose(fp);
				close(client);
				return errno == EPIPE || errno == ECONNRESET;
			}
			sent += result;
		}
	}
	fclose(fp);
	(void)send(client, "\n", 1, MSG_NOSIGNAL);
	close(client);
	return 1;
}

int
main(int argc, char *argv[])
{
	struct sockaddr_un address;
	const char *result;
	pid_t child;
	int i, listener, status;

	if (argc == 3 && !strcmp(argv[1], "--missing")) {
		result = herdr_status(argv[2]);
		puts(result ? result : "NULL");
		return 0;
	}
	if (argc < 3)
		return 2;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	if (strlen(argv[1]) >= sizeof(address.sun_path))
		return 2;
	memcpy(address.sun_path, argv[1], strlen(argv[1]) + 1);
	if ((listener = socket(AF_UNIX, SOCK_STREAM, 0)) == -1 ||
	    bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1 ||
	    listen(listener, argc - 2) == -1)
		return 2;
	if ((child = fork()) == -1)
		return 2;
	if (child == 0) {
		for (i = 2; i < argc; i++) {
			if (!serve_fixture(listener, argv[i]))
				_exit(1);
		}
		close(listener);
		_exit(0);
	}
	for (i = 2; i < argc; i++) {
		result = herdr_status(argv[1]);
		puts(result ? result : "NULL");
	}
	close(listener);
	if (waitpid(child, &status, 0) == -1 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		return 1;
	return 0;
}
