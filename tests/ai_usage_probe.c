#include <stdio.h>
#include <string.h>

char buf[1024];

const char *claude_subscription_usage(const char *);
const char *openai_subscription_usage(const char *);
const char *opencode_go_usage(const char *);

int
main(int argc, char *argv[])
{
	const char *result;

	if (argc != 3)
		return 2;
	if (!strcmp(argv[1], "claude"))
		result = claude_subscription_usage(argv[2]);
	else if (!strcmp(argv[1], "openai"))
		result = openai_subscription_usage(argv[2]);
	else if (!strcmp(argv[1], "opencode"))
		result = opencode_go_usage(argv[2]);
	else
		return 2;

	puts(result ? result : "NULL");
	return 0;
}
