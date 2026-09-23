#!/bin/sh
set -eu

tmp=$(mktemp -d /tmp/slstatus-ai-usage.XXXXXX)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/bin"

cat > "$tmp/bin/codexbar" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" > "$ARGS_FILE"
cat <<'CARDS'
╭────────────────────────╮
│ OpenCode Go [api]      │
│ Weekly        87% left │
│ [ ━━━━━━━━━━━        ] │
│ Monthly       94% left │
│ [ ━━━━━━━━━━━━━━━━━━━ ] │
╰────────────────────────╯
CARDS
exit "${CODEXBAR_EXIT:-0}"
EOF
chmod 755 "$tmp/bin/codexbar"

output=$(ARGS_FILE="$tmp/args" PATH="$tmp/bin:$PATH" \
	SLSTATUS_AI_USAGE_REPORT="$PWD/scripts/ai-usage-report" ./slstatus -u)
printf '%s\n' "$output" | grep -F '│ OpenCode Go [api]      │' >/dev/null
printf '%s\n' "$output" | grep -F '│ Weekly        87% left │' >/dev/null
printf '%s\n' "$output" | grep -F '│ [ ███████████░░░░░░░ ] │' >/dev/null
printf '%s\n' "$output" | grep -F '│ Monthly       94% left │' >/dev/null
printf '%s\n' "$output" | grep -F '│ [ ███████████████████ ] │' >/dev/null
[ "$(cat "$tmp/args")" = 'cards --no-color' ]

if ARGS_FILE="$tmp/args" CODEXBAR_EXIT=3 PATH="$tmp/bin:$PATH" \
	SLSTATUS_AI_USAGE_REPORT="$PWD/scripts/ai-usage-report" \
	./slstatus -u > /dev/null; then
	printf 'slstatus did not pass through CodexBar failure\n' >&2
	exit 1
else
	[ "$?" -eq 3 ]
fi

if PATH=/nonexistent SLSTATUS_AI_USAGE_REPORT="$PWD/scripts/ai-usage-report" \
	./slstatus -u > "$tmp/output" 2>&1; then
	printf 'slstatus succeeded without CodexBar\n' >&2
	exit 1
else
	[ "$?" -eq 127 ]
fi
case $(cat "$tmp/output") in
	*'codexbar is unavailable'*) ;;
	*) printf 'missing CodexBar error: %s\n' "$(cat "$tmp/output")" >&2; exit 1 ;;
esac

printf 'ai usage tests passed\n'
