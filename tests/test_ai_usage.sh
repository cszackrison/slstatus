#!/bin/sh
set -eu

tmp=$(mktemp -d /tmp/slstatus-ai-usage.XXXXXX)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cc -I. -D_DEFAULT_SOURCE -std=c99 -pedantic -Wall -Wextra -Os \
	-o "$tmp/probe" tests/ai_usage_probe.c components/ai_usage.c util.c

assert_equal() {
	if [ "$1" != "$2" ]; then
		printf 'expected: %s\nactual:   %s\n' "$2" "$1" >&2
		exit 1
	fi
}

now=$(date +%s)
future=$((now + 3600))
past=$((now - 1))

printf 'v1\t25.4\t%s\t96\t%s\n' "$future" "$future" \
	> "$tmp/claude"
assert_equal "$("$tmp/probe" claude "$tmp/claude")" \
	"5hr 75% wk 4%"

printf 'v1\t25.4\t%s\t96\t%s\n' "$past" "$future" \
	> "$tmp/claude"
assert_equal "$("$tmp/probe" claude "$tmp/claude")" "wk 4%"

printf 'not-a-cache\n' > "$tmp/claude"
assert_equal "$("$tmp/probe" claude "$tmp/claude")" "NULL"

mkdir -p "$tmp/sessions/2026/07/21"
rollout=$tmp/sessions/2026/07/21/rollout-test.jsonl
printf '%s\n' \
	'{"type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"codex","primary":{"used_percent":14.0,"window_minutes":10080,"resets_at":9999999999},"secondary":null,"plan_type":"pro"}}}' \
	> "$rollout"
assert_equal "$("$tmp/probe" openai "$tmp/sessions")" "wk 86%"

printf '%s\n' \
	'{"type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"codex","primary":{"used_percent":25.4,"window_minutes":300,"resets_at":9999999999},"secondary":{"used_percent":96,"window_minutes":10080,"resets_at":9999999999},"plan_type":"plus"}}}' \
	> "$rollout"
assert_equal "$("$tmp/probe" openai "$tmp/sessions")" \
	"5hr 75% wk 4%"

printf '%s\n' \
	'{"type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"codex_spark","primary":{"used_percent":5,"window_minutes":10080,"resets_at":9999999999},"secondary":null,"plan_type":"pro"}}}' \
	> "$rollout"
assert_equal "$("$tmp/probe" openai "$tmp/sessions")" "NULL"

printf '%s\n' \
	'{"type":"event_msg","payload":{"type":"token_count","rate_limits":null}}' \
	> "$rollout"
assert_equal "$("$tmp/probe" openai "$tmp/sessions")" "NULL"

cache_root=$tmp/cache
output=$(printf '%s\n' \
	"{\"rate_limits\":{\"five_hour\":{\"used_percentage\":25.4,\"resets_at\":$future},\"seven_day\":{\"used_percentage\":96,\"resets_at\":$future}}}" \
	| XDG_CACHE_HOME="$cache_root" scripts/claude-usage-cache)
case $output in
	"["*"@"*" "*"]") ;;
	*) printf 'unexpected Claude footer: %s\n' "$output" >&2; exit 1 ;;
esac
assert_equal "$("$tmp/probe" claude \
	"$cache_root/slstatus/claude-usage-v1")" "5hr 75% wk 4%"
assert_equal "$(stat -c %a "$cache_root/slstatus")" "700"
assert_equal "$(stat -c %a "$cache_root/slstatus/claude-usage-v1")" "600"

printf 'ai usage tests passed\n'
