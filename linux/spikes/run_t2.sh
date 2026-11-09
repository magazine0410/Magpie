#!/usr/bin/env bash
# T2: capture the test source window through KWin's screencast protocol in four
# configurations and print what arrives. Opens a 640x480 window for ~40 s.
#
# Usage: run_t2.sh [output name, default DP-6] [extra testsource arguments]
set -u
cd "$(dirname "$0")"
B=$PWD/build
OUT=out/t2
TARGET=${1:-DP-6}
shift || true
mkdir -p "$OUT"

"$B/t8_testsource/testsource" --frameless --duration 45 "$@" > "$OUT/source.log" 2>&1 &
SOURCE_PID=$!
trap 'kill $SOURCE_PID 2>/dev/null' EXIT
sleep 1.5

UUID=$("$B/kwin_windows/kwin_windows" | grep 'title="Magpie test source"' | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
if [ -z "$UUID" ]; then
	echo "test window not found"
	cat "$OUT/source.log"
	exit 1
fi
echo "uuid=$UUID"
"$B/kwin_windows/kwin_windows" --send-to-output "$UUID" "$TARGET" | sed -E 's/ title=.*//'
sleep 1

for run in "default:" "nvidia:--vk 0" "radv:--vk 1" "shm:--no-dmabuf"; do
	name=${run%%:*}
	args=${run#*:}
	echo
	echo "===== capture run: $name ($args) ====="
	# shellcheck disable=SC2086
	timeout 20 "$B/kwin_cast/kwin_cast" --window "$UUID" --seconds 6 $args 2>&1 \
		| tee "$OUT/cast_$name.log" | grep -vE "^stream state: (connecting|paused|unconnected)"
done

echo
echo "===== source log ====="
grep -E "^started|^fps|screen-changed" "$OUT/source.log" | awk 'NR<=3 || NR%6==0'
