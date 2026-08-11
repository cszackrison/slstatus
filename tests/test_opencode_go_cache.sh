#!/bin/sh
set -eu

tmp=$(mktemp -d /tmp/slstatus-opencode-go.XXXXXX)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

assert_equal() {
	if [ "$1" != "$2" ]; then
		printf 'expected: %s\nactual:   %s\n' "$2" "$1" >&2
		exit 1
	fi
}

printf '%s\n' \
	'{"usage":{"rolling":{"percent":25.4,"resetsAt":"2099-01-01T01:00:00Z"},"weekly":{"percent":96,"resetsAt":"2099-01-01T02:00:00Z"},"monthly":{"percent":60,"resetsAt":"2099-01-01T03:00:00Z"}}}' \
	> "$tmp/usage.json"
XDG_CACHE_HOME="$tmp/cache" scripts/opencode-go-usage-cache \
	--parse "$tmp/usage.json"
set -- $(tr '\t' ' ' < "$tmp/cache/slstatus/opencode-go-usage-v1")
assert_equal "$1" "v1"
assert_equal "$3" "25.4"
assert_equal "$5" "96"
assert_equal "$7" "60"
assert_equal "$4" "4070912400"

printf '%s\n' \
	'{"usage":{"rolling":{"percent":5,"resetsAt":"2099-01-01T01:30:00+00:00"}}}' \
	> "$tmp/partial.json"
XDG_CACHE_HOME="$tmp/cache" scripts/opencode-go-usage-cache \
	--parse "$tmp/partial.json"
set -- $(tr '\t' ' ' < "$tmp/cache/slstatus/opencode-go-usage-v1")
assert_equal "$3" "5"
assert_equal "$5" "-"
assert_equal "$7" "-"

printf '<html>not JSON</html>\n' > "$tmp/bad.json"
if XDG_CACHE_HOME="$tmp/cache" scripts/opencode-go-usage-cache \
	--parse "$tmp/bad.json"; then
	printf 'malformed response unexpectedly succeeded\n' >&2
	exit 1
fi
[ ! -e "$tmp/cache/slstatus/opencode-go-usage-v1" ]

if HOME="$tmp/home" XDG_CACHE_HOME="$tmp/cache" \
	XDG_CONFIG_HOME="$tmp/config" scripts/opencode-go-usage-cache; then
	printf 'missing configuration unexpectedly succeeded\n' >&2
	exit 1
fi
[ ! -e "$tmp/cache/slstatus/opencode-go-usage-v1" ]

mkdir -p "$tmp/config/slstatus" "$tmp/fakebin"
printf '%s\n' '{"apiKey":"test-api-key"}' \
	> "$tmp/config/slstatus/opencode-go.json"
chmod 600 "$tmp/config/slstatus/opencode-go.json"

printf '%s\n' \
	'#!/bin/sh' \
	'set -eu' \
	'output=' \
	'header_file=' \
	'endpoint=' \
	'while [ "$#" -gt 0 ]; do' \
	'case $1 in' \
	'--output) shift; output=$1 ;;' \
	'--header) shift; case $1 in @*) header_file=${1#@} ;; esac ;;' \
	'https://*) endpoint=$1 ;;' \
	'esac' \
	'shift' \
	'done' \
	'[ "$endpoint" = "https://opencode.ai/zen/go/v1/usage" ]' \
	'[ -n "$output" ] && [ -n "$header_file" ]' \
	'grep -qx "Authorization: Bearer test-env-key" "$header_file"' \
	'cp "$FAKE_RESPONSE" "$output"' \
	': > "$FAKE_CALLED"' \
	> "$tmp/fakebin/curl"
chmod 755 "$tmp/fakebin/curl"
FAKE_RESPONSE="$tmp/usage.json" FAKE_CALLED="$tmp/called" \
	OPENCODE_API_KEY=test-env-key HOME="$tmp/home" \
	XDG_CACHE_HOME="$tmp/cache" XDG_CONFIG_HOME="$tmp/missing-config" \
	PATH="$tmp/fakebin:$PATH" scripts/opencode-go-usage-cache
[ -e "$tmp/called" ]
set -- $(tr '\t' ' ' < "$tmp/cache/slstatus/opencode-go-usage-v1")
assert_equal "$3" "25.4"

printf '%s\n' '#!/bin/sh' 'exit 7' > "$tmp/fakebin/curl"
chmod 755 "$tmp/fakebin/curl"
printf 'v1 stale data\n' > "$tmp/cache/slstatus/opencode-go-usage-v1"
if HOME="$tmp/home" XDG_CACHE_HOME="$tmp/cache" \
	XDG_CONFIG_HOME="$tmp/config" PATH="$tmp/fakebin:$PATH" \
	scripts/opencode-go-usage-cache; then
	printf 'network failure unexpectedly succeeded\n' >&2
	exit 1
fi
[ ! -e "$tmp/cache/slstatus/opencode-go-usage-v1" ]

printf 'opencode go cache tests passed\n'
