#!/bin/sh

set -eu

tmp=$(mktemp -d /tmp/slstatus-audio-menu.XXXXXX)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/bin"

cat > "$tmp/bin/pactl" <<'EOF'
#!/bin/sh

case "$*" in
"get-default-sink")
	printf '%s\n' sink.usb
	;;
"list sinks")
	cat <<'SINKS'
Sink #1
	Name: sink.builtin
	Description: Built-in Audio Analog Stereo
Sink #2
	Name: sink.usb
	Description: USB Speakers
SINKS
	;;
"list short sink-inputs")
	printf '41\t1\tprotocol-native.c\n52\t1\tprotocol-native.c\n'
	;;
"get-default-source")
	printf '%s\n' source.usb
	;;
"list sources")
	cat <<'SOURCES'
Source #1
	Name: sink.builtin.monitor
	Description: Monitor of Built-in Audio
Source #2
	Name: source.builtin
	Description: Built-in Microphone
Source #3
	Name: source.usb
	Description: USB Microphone
SOURCES
	;;
"list short source-outputs")
	printf '73\t3\tprotocol-native.c\n'
	;;
*)
	printf '%s\n' "$*" >> "$PACTL_LOG"
	;;
esac
EOF

cat > "$tmp/bin/st" <<'EOF'
#!/bin/sh

printf '%s\n' "$*" > "$ST_ARGS"
printf 'called\n' >> "$ST_CALLS"
[ -z "${ST_HOLD-}" ] || sleep "$ST_HOLD"
while [ "$#" -gt 4 ]; do
	shift
done
lines=$1
label=$2
input=$3
output=$4
printf '%s\n' "$lines $label" > "$MENU_CONFIG"
cp "$input" "$DMENU_INPUT"
awk -v wanted="$DMENU_CHOICE" '$0 == wanted { print; exit }' \
	"$input" > "$output"
EOF

chmod +x "$tmp/bin/pactl" "$tmp/bin/st"
export PATH="$tmp/bin:$PATH"
export PACTL_LOG="$tmp/pactl.log"
export DMENU_INPUT="$tmp/dmenu.input"
export MENU_CONFIG="$tmp/menu.config"
export ST_ARGS="$tmp/st.args"
export ST_CALLS="$tmp/st.calls"
export SLSTATUS_TERMINAL="$tmp/bin/st"
mkdir -p "$tmp/runtime"
export XDG_RUNTIME_DIR="$tmp/runtime"

export DMENU_CHOICE='1. Built-in Audio Analog Stereo'
sh scripts/audio-device-menu sink
grep -F '1. Built-in Audio Analog Stereo' "$DMENU_INPUT" >/dev/null
grep -F '2. USB Speakers [current]' "$DMENU_INPUT" >/dev/null
grep -F -- '-A 1 -c SlstatusPopup -n slstatus-audio -t Output device -g 68x3' \
	"$ST_ARGS" >/dev/null
grep -Fx '2 Output' "$MENU_CONFIG" >/dev/null
grep -F 'set-default-sink sink.builtin' "$PACTL_LOG" >/dev/null
grep -F 'move-sink-input 41 sink.builtin' "$PACTL_LOG" >/dev/null
grep -F 'move-sink-input 52 sink.builtin' "$PACTL_LOG" >/dev/null

: > "$PACTL_LOG"
export DMENU_CHOICE='1. Built-in Microphone'
sh scripts/audio-device-menu source
grep -F '1. Built-in Microphone' "$DMENU_INPUT" >/dev/null
grep -F '2. USB Microphone [current]' "$DMENU_INPUT" >/dev/null
if grep -F '.monitor' "$DMENU_INPUT" >/dev/null; then
	printf 'monitor source was included in the input menu\n' >&2
	exit 1
fi
grep -F 'set-default-source source.builtin' "$PACTL_LOG" >/dev/null
grep -F 'move-source-output 73 source.builtin' "$PACTL_LOG" >/dev/null

: > "$ST_CALLS"
export ST_HOLD=1
export DMENU_CHOICE='1. Built-in Audio Analog Stereo'
sh scripts/audio-device-menu sink &
menu_pid=$!
while [ ! -s "$ST_CALLS" ]; do
	sleep 0.01
done
sh scripts/audio-device-menu source
wait "$menu_pid"
unset ST_HOLD
[ "$(wc -l < "$ST_CALLS")" -eq 1 ]

printf 'audio device menu tests passed\n'
