#!/usr/bin/env bash
# Fullscreen scaling of the test window with magpie-scale, on a KDE Plasma
# desktop. Checks where the window and the pointer are while scaling, that a
# click, a drag and scrolling reach the source at the right place, that the
# pointer leaves to a neighbouring monitor and comes back, and that
# everything is restored, also after the scaler was killed.
#
# It moves the pointer and sends clicks and scrolling to the test window, so
# nobody should use the machine while it runs (about a minute per window).
# The numbers in it assume a 1920x1080 monitor at 3840,0 with a neighbour on
# its left, which is the reference machine's DP-6.
#
# It uses the test window and the helper programs of the Phase 0 feasibility
# tests, which have to be built first:
#   cmake -S linux/spikes -B linux/spikes/build -G Ninja && cmake --build linux/spikes/build
#
# Usage: scaling_desktop_test.sh [wayland|xcb]...
#   The kind of test window: a Wayland client, an X11 client, or both (default).
# Environment: OUT, the directory for logs (default: a new one in /tmp)
set -u
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
PLATFORMS=("$@")
[ ${#PLATFORMS[@]} -eq 0 ] && PLATFORMS=(wayland xcb)
B=$REPO/linux/build
SB=$REPO/linux/spikes/build
OUT=${OUT:-$(mktemp -d /tmp/magpie-scaling-test.XXXXXX)}
mkdir -p "$OUT"
MONITOR=DP-6
echo "logs: $OUT"

PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

windows() { "$SB/kwin_windows/kwin_windows"; }
pointer() { "$SB/kwin_pointer/kwin_pointer" "$@"; }
window_line() { windows | grep "$1" | sed -E 's/.*(frame=[^ ]+).*(client=[^ ]+) (state=[^ ]+).*/\1 \2 \3/'; }
layout() { kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -E "^Output|Geometry" | paste - - | sed -E 's/Output: [0-9]+ //; s/ [0-9a-f-]{36}//; s/\t+/ /g' | sort | tr '\n' ';'; }

busctl --user monitor --json=short --match="type='method_call',interface='org.magpie.Probe'" > "$OUT/probe.jsonl" 2>/dev/null &
PIDS+=("$!")
sleep 0.5
cat > "$OUT/probe.js" <<'EOF'
callDBus("org.magpie.Probe", "/Probe", "org.magpie.Probe", "Report",
	Math.round(workspace.cursorPos.x), Math.round(workspace.cursorPos.y),
	workspace.activeWindow ? String(workspace.activeWindow.resourceClass) : "none");
EOF
probe() {
	local name="magpie-probe-$RANDOM"
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting loadScript ss "$OUT/probe.js" "$name" > /dev/null
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting start > /dev/null
	sleep 0.3
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting unloadScript s "$name" > /dev/null
	tail -1 "$OUT/probe.jsonl" | python3 -c "
import json, sys
try:
    data = json.loads(sys.stdin.readline())['payload']['data']
    name = data[2] if data[2] in ('testsource', 'none') else 'another window'
    print('pointer %s,%s active: %s' % (data[0], data[1], name))
except Exception as error:
    print('no report (%s)' % error)
"
}
last() { grep -E "$1" "$2" | tail -1; }

echo "layout before: $(layout)"
for platform in "${PLATFORMS[@]}"; do
	echo
	echo "################ $platform source ################"
	LOG=$OUT/source_$platform.log
	QT_QPA_PLATFORM=$platform "$SB/t8_testsource/testsource" --duration 150 > "$LOG" 2>&1 &
	SOURCE_PID=$!
	PIDS+=("$SOURCE_PID")
	sleep 1.5
	UUID=$(windows | grep "pid=$SOURCE_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
	"$SB/kwin_windows/kwin_windows" --send-to-output "$UUID" "$MONITOR" > /dev/null
	sleep 0.8
	BEFORE=$(window_line "$UUID")
	echo "before:        $BEFORE"
	pointer move 4000 900

	echo "--- scaling for 16 s ---"
	"$B/tools/magpie-scale/magpie-scale" --window "$UUID" --seconds 16 -v --validate > "$OUT/scale_$platform.log" 2>&1 &
	SCALE_PID=$!
	PIDS+=("$SCALE_PID")
	sleep 4
	grep -E "^Scaling |Output Virtual|error|warn|Scaling did not" "$OUT/scale_$platform.log" | sed -E 's/^[^|]*\|[^|]*\|[^|]*\|[^|]*\|//' | cut -c1-230
	echo "while scaled:  $(window_line "$UUID")"
	echo "source frame rate: $(last '^fps' "$LOG" | cut -c1-60)"
	echo "the pointer was on the monitor, so it went to the source: $(probe)"

	# The picture is 1440x1080 in the middle of the 1920x1080 monitor at 3840,0.
	# Its centre shows the centre of the source.
	pointer move 4800 540 sleep 400
	echo "moved onto the picture's centre: $(probe)"
	pointer click sleep 300
	echo "click:         $(last '^press' "$LOG")"
	pointer rel 50 20 sleep 200 down sleep 100 rel 60 0 sleep 100 rel 0 40 sleep 100 up sleep 300
	echo "drag from:     $(last '^press' "$LOG")   to: $(last '^release' "$LOG")"
	pointer scroll 15 sleep 300
	echo "scroll:        $(last '^wheel' "$LOG")"
	pointer rel -5000 0 sleep 500
	echo "pushed far to the left: $(probe)"
	pointer rel 4 0 sleep 500
	echo "4 px back to the right: $(probe)"
	pointer rel 5000 0 sleep 400
	echo "pushed far to the right (no monitor there): $(probe)"
	pointer rel -300 100 sleep 300
	echo "back inside:   $(probe)"
	wait "$SCALE_PID"
	echo "exit status $?"
	grep -vE "^[0-9]{4}-|^\[" "$OUT/scale_$platform.log" | cut -c1-200
	grep -E "Validation|validation" "$OUT/scale_$platform.log" | head -3 | cut -c1-200
	sleep 0.5
	AFTER=$(window_line "$UUID")
	echo "after:         $AFTER"
	echo "restored exactly: $([ "$BEFORE" = "$AFTER" ] && echo yes || echo NO)"
	echo "pointer after: $(probe)"
	echo "layout:        $(layout)"

	echo "--- scaler killed while scaling ---"
	"$B/tools/magpie-scale/magpie-scale" --window "$UUID" --seconds 30 > "$OUT/scale_kill_$platform.log" 2>&1 &
	SCALE_PID=$!
	sleep 3.5
	echo "while scaled:  $(window_line "$UUID")"
	kill -9 "$SCALE_PID"
	wait "$SCALE_PID" 2>/dev/null
	sleep 1
	echo "after the kill: $(window_line "$UUID")"
	echo "state file:    $(ls ~/.local/state/magpie/ | tr '\n' ' ')"
	# The next start puts the window back before it does anything else
	"$B/tools/magpie-scale/magpie-scale" --window "$UUID" --seconds 2 > "$OUT/scale_recover_$platform.log" 2>&1
	echo "next run:      exit status $?; $(grep -E "did not end properly" "$OUT/scale_recover_$platform.log" | sed -E 's/^[^|]*\|[^|]*\|[^|]*\|[^|]*\|//')"
	sleep 0.5
	AFTER=$(window_line "$UUID")
	echo "after it:      $AFTER"
	echo "restored exactly: $([ "$BEFORE" = "$AFTER" ] && echo yes || echo NO)"
	echo "state file:    $(ls ~/.local/state/magpie/ | tr '\n' ' ')"

	kill "$SOURCE_PID" 2>/dev/null
	wait "$SOURCE_PID" 2>/dev/null
done
pointer move 4800 540
echo
echo "layout after:  $(layout)"
