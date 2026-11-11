#!/usr/bin/env bash
# A monitor that goes away while a window is scaled on it, on a KDE Plasma
# desktop: the monitor is disabled by software for some seconds and enabled
# again. Scaling has to end by itself, and the window has to be on a monitor
# that is still there.
#
# For those seconds the monitor is dark, and the desktop moves the windows
# and panels that were on it to another monitor; not all of them may return
# to their places when it comes back. The script notes the monitor's place,
# mode and scale before, sets them again when it enables the monitor, and
# says if anything differs afterwards.
#
# KWin keeps an arrangement for every set of outputs, and the set while a
# window is scaled includes Magpie's virtual output. Disabling the monitor
# then makes KWin keep "disabled" for that set, and the monitor would go off
# again whenever a window is scaled. The scaler puts the monitors back when
# it finds them changed, so the script scales once more at its end, which
# makes KWin keep the right arrangement, and checks that.
#
# kwin_virtual_test.sh checks the same in a KWin of its own, which shows
# nothing; this script is for the real thing.
#
# It uses helper programs of the Phase 0 feasibility tests, which have to be
# built first:
#   cmake -S linux/spikes -B linux/spikes/build -G Ninja && cmake --build linux/spikes/build
#
# Usage: monitor_desktop_test.sh [MONITOR]   (default: DP-6)
# Environment: OUT, the directory for logs (default: a new one in /tmp);
#   OFF_SECONDS, how long the monitor stays disabled (default 8)
set -u
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
MONITOR=${1:-DP-6}
B=$REPO/linux/build
SB=$REPO/linux/spikes/build
OUT=${OUT:-$(mktemp -d /tmp/magpie-monitor-test.XXXXXX)}
mkdir -p "$OUT"
OFF_SECONDS=${OFF_SECONDS:-8}
echo "logs: $OUT"
export XDG_STATE_HOME=$OUT/state MAGPIE_CONFIG_DIR=$OUT/config

PASSED=0
FAILED=0
check() {
	if [ "$2" = "$3" ]; then
		PASSED=$((PASSED + 1)); echo "  ok    $1"
	else
		FAILED=$((FAILED + 1)); echo "  FAIL  $1: \"$2\", expected \"$3\""
	fi
}

PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2> /dev/null; done; }
trap cleanup EXIT

windows() { "$SB/kwin_windows/kwin_windows"; }
# One line per output: NAME enabled|disabled X,Y SCALE MODE_ID WIDTHxHEIGHT@RATE
outputs() {
	kscreen-doctor -j 2> /dev/null | python3 -c '
import json, sys
for o in sorted(json.load(sys.stdin)["outputs"], key=lambda o: o["name"]):
    mode = [m for m in o["modes"] if m["id"] == o.get("currentModeId")]
    size = "%dx%d@%.2f" % (mode[0]["size"]["width"], mode[0]["size"]["height"], mode[0]["refreshRate"]) if mode else "-"
    print(o["name"], "enabled" if o["enabled"] else "disabled", "%d,%d" % (o["pos"]["x"], o["pos"]["y"]),
          o.get("scale"), o.get("currentModeId"), size, "priority", o.get("priority"))'
}
field() { outputs | awk -v name="$MONITOR" -v n="$1" '$1 == name { print $n }'; }

outputs > "$OUT/outputs-before.txt"
cat "$OUT/outputs-before.txt"
if [ "$(field 2)" != enabled ]; then
	echo "$MONITOR is not an enabled monitor"
	exit 1
fi
POSITION=$(field 3)
SCALE_FACTOR=$(field 4)
MODE=$(field 5)
ORIGIN_X=${POSITION%,*}
check "there is another monitor for the window to go to" "$([ "$(outputs | grep -c ' enabled ')" -ge 2 ] && echo yes)" "yes"

"$B/tests/magpie_test_window" --title "monitor test" --app-id magpie.test.monitor > /dev/null 2>&1 &
SOURCE_PID=$!
PIDS+=("$SOURCE_PID")
sleep 1.2
UUID=$(windows | grep "pid=$SOURCE_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
"$SB/kwin_windows/kwin_windows" --send-to-output "$UUID" "$MONITOR" > /dev/null
sleep 0.8
frame() { windows | grep "pid=$SOURCE_PID " | sed -E 's/.*frame=([^ ]+).*/\1/'; }
# frame is WIDTHxHEIGHT+X+Y
frame_x() { frame | sed -E 's/^[0-9]+x[0-9]+\+(-?[0-9]+)\+.*/\1/'; }
BEFORE=$(frame)
echo "the test window before: $BEFORE"
check "it is on $MONITOR" "$([ "$(frame_x)" -ge "$ORIGIN_X" ] && [ "$(frame_x)" -lt "$((ORIGIN_X + 1920))" ] && echo yes)" "yes"

"$B/tools/magpie-scale/magpie-scale" --window "$UUID" --seconds 60 --effect Lanczos,type=fit -v > "$OUT/scale.log" 2>&1 &
SCALER=$!
PIDS+=("$SCALER")
sleep 3
check "the window is scaled" "$(grep -c "^Scaling \|Scaling [0-9]*x[0-9]* on" "$OUT/scale.log" | sed 's/^[1-9][0-9]*$/yes/')" "yes"

echo "--- $MONITOR is disabled for $OFF_SECONDS s ---"
kscreen-doctor "output.$MONITOR.disable" > "$OUT/kscreen-disable.log" 2>&1
for i in $(seq 1 60); do kill -0 "$SCALER" 2> /dev/null || break; sleep 0.1; done
check "scaling ends by itself" "$(kill -0 "$SCALER" 2> /dev/null && echo running || echo ended)" "ended"
wait "$SCALER" 2> /dev/null
check "without an error" "$?" "0"
check "because the monitor is gone" "$(grep -c "because the monitor with the picture is gone" "$OUT/scale.log")" "1"
sleep 1
AFTER=$(frame)
echo "the test window while $MONITOR is off: $AFTER"
check "the window has its size and title bar" "${AFTER%%+*}" "${BEFORE%%+*}"
OTHER=$(outputs | awk -v name="$MONITOR" '$1 != name && $2 == "enabled" { print $3 }' | tr '\n' ' ')
ON_OTHER=no
for position in $OTHER; do
	x=${position%,*}
	[ "$(frame_x)" -ge "$x" ] && [ "$(frame_x)" -lt "$((x + 1920))" ] && ON_OTHER=yes
done
check "and is on a monitor that is on" "$ON_OTHER" "yes"
check "nothing is left to undo" "$(ls "$XDG_STATE_HOME/magpie" 2> /dev/null | grep -c scaling-state)" "0"
sleep "$OFF_SECONDS"

echo "--- $MONITOR is enabled again ---"
kscreen-doctor "output.$MONITOR.enable" "output.$MONITOR.position.$POSITION" "output.$MONITOR.mode.$MODE" \
	"output.$MONITOR.scale.$SCALE_FACTOR" > "$OUT/kscreen-enable.log" 2>&1
sleep 3
outputs > "$OUT/outputs-after.txt"
check "the monitors are as they were" "$(diff "$OUT/outputs-before.txt" "$OUT/outputs-after.txt" > /dev/null && echo yes || echo no)" "yes"
diff "$OUT/outputs-before.txt" "$OUT/outputs-after.txt"
echo "the test window afterwards: $(frame)"

echo "--- scaled on $MONITOR again ---"
"$SB/kwin_windows/kwin_windows" --send-to-output "$UUID" "$MONITOR" > /dev/null
sleep 0.8
for run in again third; do
	"$B/tools/magpie-scale/magpie-scale" --window "$UUID" --seconds 3 --effect Lanczos,type=fit -v > "$OUT/scale-$run.log" 2>&1
	check "scaling on $MONITOR works ($run)" "$? $(grep -c "on $MONITOR;" "$OUT/scale-$run.log" | sed 's/^[1-9][0-9]*$/yes/')" "0 yes"
done
check "the scaler put the monitor back once" "$(grep -c "KWin changed the monitors" "$OUT/scale-again.log")" "1"
check "and found it in place the next time" "$(grep -c "output.$MONITOR.enable" "$OUT/scale-third.log")" "0"
outputs > "$OUT/outputs-end.txt"
check "the monitors are as they were at the end" "$(diff "$OUT/outputs-before.txt" "$OUT/outputs-end.txt" > /dev/null && echo yes || echo no)" "yes"

kill "$SOURCE_PID" 2> /dev/null
echo
echo "$PASSED passed, $FAILED failed"
[ "$FAILED" = 0 ]
