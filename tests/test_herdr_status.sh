#!/bin/sh
set -eu

tmp=$(mktemp -d /tmp/slstatus-herdr.XXXXXX)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cc -I. -D_DEFAULT_SOURCE -std=c99 -pedantic -Wall -Wextra -Os \
	-o "$tmp/probe" tests/herdr_status_probe.c components/herdr.c util.c

assert_equal() {
	if [ "$1" != "$2" ]; then
		printf 'expected: %s\nactual:   %s\n' "$2" "$1" >&2
		exit 1
	fi
}

printf '%s\n' \
	'{"id":"fixture","result":{"agents":[{"agent_status":"working","tokens":{"agent_status":"done"},"terminal_title":"agent_status blocked"},{"agent_status":"blocked"},{"agent_status":"done"},{"agent_status":"working"},{"agent_status":"idle"},{"agent_status":"unknown"},{"agent_status":"future"}],"type":"agent_list"}}' \
	> "$tmp/mixed.json"
assert_equal "$("$tmp/probe" "$tmp/mixed.sock" \
	"$tmp/mixed.json" "$tmp/mixed.json")" \
	"$(printf '!1 ✓1 …2 ○1 ?2\n 1  1 …2 ○1 ?2')"

printf '%s\n' \
	'{"result":{"type":"agent_list","agents":[{"agent_status":"working"}]}}' \
	> "$tmp/working.json"
assert_equal "$("$tmp/probe" "$tmp/reset.sock" "$tmp/mixed.json" \
	"$tmp/working.json" "$tmp/mixed.json")" \
	"$(printf '!1 ✓1 …2 ○1 ?2\n…1\n!1 ✓1 …2 ○1 ?2')"

printf '%s\n' '{"result":{"agents":[],"type":"agent_list"}}' \
	> "$tmp/empty.json"
assert_equal "$("$tmp/probe" "$tmp/empty.sock" "$tmp/empty.json")" "NULL"

printf '%s\n' '{"id":"fixture","error":{"code":"not_running"}}' \
	> "$tmp/error.json"
assert_equal "$("$tmp/probe" "$tmp/error.sock" "$tmp/error.json")" "NULL"

printf '%s\n' '{"result":{"agents":[' > "$tmp/malformed.json"
assert_equal "$("$tmp/probe" "$tmp/malformed.sock" \
	"$tmp/malformed.json")" "NULL"

printf '%s\n' \
	'{"result":{"agents":[{"agent_status":"done"}],"type":"other"}}' \
	> "$tmp/wrong-type.json"
assert_equal "$("$tmp/probe" "$tmp/wrong.sock" \
	"$tmp/wrong-type.json")" "NULL"

assert_equal "$("$tmp/probe" --missing "$tmp/missing.sock")" "NULL"
assert_equal "$("$tmp/probe" "$tmp/stall.sock" --stall)" "NULL"

printf '%s' \
	'{"result":{"agents":[{"agent_status":"working","title":"' \
	> "$tmp/oversized.json"
dd if=/dev/zero bs=1048576 count=1 2>/dev/null \
	| tr '\000' x >> "$tmp/oversized.json"
printf '%s\n' '"}],"type":"agent_list"}}' >> "$tmp/oversized.json"
assert_equal "$("$tmp/probe" "$tmp/oversized.sock" \
	"$tmp/oversized.json")" "NULL"

printf 'herdr status tests passed\n'
