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
	'<html><script>rollingUsage:$R[10]={usagePercent:25.4,resetInSec:3600}weeklyUsage:$R[11]={resetInSec:7200,usagePercent:96}monthlyUsage:$R[12]={usagePercent:60,resetInSec:10800}</script></html>' \
	> "$tmp/ssr.html"
XDG_CACHE_HOME="$tmp/cache" scripts/opencode-go-usage-cache \
	--parse "$tmp/ssr.html"
set -- $(tr '\t' ' ' < "$tmp/cache/slstatus/opencode-go-usage-v1")
assert_equal "$1" "v1"
assert_equal "$3" "25.4"
assert_equal "$5" "96"
assert_equal "$7" "60"

printf '%s\n' \
	'<div data-slot="usage-item"><span data-slot="usage-label">Rolling Usage</span><span data-slot="usage-value"><!--$-->5<!--/-->%</span><span data-slot="reset-time"><!--$-->Resets in<!--/--> <!--$-->1 hour 30 minutes<!--/--></span></div>' \
	> "$tmp/slots.html"
XDG_CACHE_HOME="$tmp/cache" scripts/opencode-go-usage-cache \
	--parse "$tmp/slots.html"
set -- $(tr '\t' ' ' < "$tmp/cache/slstatus/opencode-go-usage-v1")
assert_equal "$3" "5"
assert_equal "$5" "-"
assert_equal "$7" "-"

printf '<html>no usage</html>\n' > "$tmp/bad.html"
if XDG_CACHE_HOME="$tmp/cache" scripts/opencode-go-usage-cache \
	--parse "$tmp/bad.html"; then
	printf 'malformed page unexpectedly succeeded\n' >&2
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
printf '%s\n' \
	'{"workspaceId":"wrk_TEST","authCookie":"test-cookie"}' \
	> "$tmp/config/slstatus/opencode-go.json"
chmod 600 "$tmp/config/slstatus/opencode-go.json"
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
