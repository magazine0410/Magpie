#!/usr/bin/env bash
# T4, T6 (injection half), T7 and T3 in one hands-off run of about a minute.
#
# What happens on screen, all on the target output (default DP-6):
#   * the test source window opens and the pointer is moved in circles over it
#     by software (twice, 5 s each);
#   * a translucent overlay covers the output for 8 s while the pointer is moved
#     onto the test window and one left click is sent to it;
#   * a translucent overlay that accepts input covers the output for 4 s;
#   * an opaque overlay covers the output twice for 6 s.
# Everything ends by itself. Ctrl+C stops the script and closes its windows.
set -u
cd "$(dirname "$0")"
B=$PWD/build
OUT=out/block1
TARGET=${1:-DP-6}
mkdir -p "$OUT"

mono() { python3 -c 'import time; print("%.2f" % time.monotonic())'; }
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

# Starts a test source, moves it to the target output and sets UUID, OX, OY.
start_source() {
	local log=$1
	shift
	"$B/t8_testsource/testsource" --frameless --duration 70 "$@" > "$log" 2>&1 &
	SOURCE_PID=$!
	PIDS+=("$SOURCE_PID")
	sleep 1.2
	UUID=$("$B/kwin_windows/kwin_windows" | grep "pid=$SOURCE_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
	if [ -z "$UUID" ]; then
		echo "test window not found"
		exit 1
	fi
	"$B/kwin_windows/kwin_windows" --send-to-output "$UUID" "$TARGET" > /dev/null
	sleep 0.8
	local geometry
	geometry=$("$B/kwin_windows/kwin_windows" | grep "$UUID" | sed -E 's/.*client=[0-9]+x[0-9]+([+-][0-9]+)([+-][0-9]+).*/\1 \2/')
	OX=$(echo "$geometry" | cut -d' ' -f1 | tr -d '+')
	OY=$(echo "$geometry" | cut -d' ' -f2 | tr -d '+')
	CX=$((OX + 320))
	CY=$((OY + 240))
	echo "source pid=$SOURCE_PID uuid=$UUID client origin=$OX,$OY"
}

stop_source() {
	kill "$SOURCE_PID" 2>/dev/null
	wait "$SOURCE_PID" 2>/dev/null
}

echo "################ T4/T6: cursor metadata, animated source ################"
start_source "$OUT/source_animated.log" --message "Automatic test. Please do not use the mouse."
timeout 20 "$B/kwin_cast/kwin_cast" --window "$UUID" --seconds 5 \
	--inject-circle "$CX" "$CY" 100 --origin "$OX" "$OY" 2>&1 | tee "$OUT/cast_animated.log" \
	| grep -E "^RESULT|^format|^t=" | cut -c1-400
echo "source saw: $(grep -c '^motion' "$OUT/source_animated.log") motion lines, last: $(grep '^motion' "$OUT/source_animated.log" | tail -1)"
stop_source

echo
echo "################ T4: cursor metadata, static source ################"
start_source "$OUT/source_static.log" --static --message "Automatic test. Please do not use the mouse."
timeout 20 "$B/kwin_cast/kwin_cast" --window "$UUID" --seconds 5 \
	--inject-circle "$CX" "$CY" 100 --origin "$OX" "$OY" 2>&1 | tee "$OUT/cast_static.log" \
	| grep -E "^RESULT|^format|^t=" | cut -c1-400
stop_source

echo
echo "################ T7: input through an overlay with an empty input region ################"
start_source "$OUT/source_t7.log" --message "Automatic test. Please do not use the mouse."
"$B/kwin_pointer/kwin_pointer" move $((OX - 200)) $((OY - 100))
sleep 0.5
before=$(grep -c '^motion' "$OUT/source_t7.log")
"$B/overlay/overlay" --output "$TARGET" --seconds 8 --alpha 110 > "$OUT/overlay_t7.log" 2>&1 &
PIDS+=("$!")
sleep 2
echo "focus while overlay is up: $("$B/kwin_windows/kwin_windows" | grep "$UUID" | sed -E 's/.*state=([^ ]+).*/\1/')"
"$B/kwin_pointer/kwin_pointer" move "$CX" "$CY" sleep 200 move $((CX + 20)) "$CY" sleep 300
after=$(grep -c '^motion' "$OUT/source_t7.log")
echo "motion lines in source before/after moving the pointer under the overlay: $before/$after"
if [ "$after" -gt "$before" ]; then
	"$B/kwin_pointer/kwin_pointer" click
	sleep 0.5
	echo "source events: $(grep -E '^(press|release|pointer-)' "$OUT/source_t7.log" | tr '\n' ';')"
	echo "expected press at x=340 y=240 (window coordinates)"
else
	echo "pointer did not reach the source: no click sent"
fi
sleep 4
grep -E "^configured|^overlay mono" "$OUT/overlay_t7.log" | head -4
echo "source fps under translucent overlay: $(grep '^fps' "$OUT/source_t7.log" | tail -4 | sed -E 's/^fps=([0-9,.]+).*active=([01])/\1(active=\2)/' | tr '\n' ' ')"

echo
echo "################ T7: the same overlay, accepting input ################"
"$B/kwin_pointer/kwin_pointer" move $((OX - 200)) $((OY - 100))
sleep 0.3
before=$(grep -c '^motion' "$OUT/source_t7.log")
"$B/overlay/overlay" --output "$TARGET" --seconds 4 --alpha 110 --input > "$OUT/overlay_input.log" 2>&1 &
PIDS+=("$!")
sleep 1.5
"$B/kwin_pointer/kwin_pointer" move "$CX" "$CY" sleep 200 move $((CX + 20)) "$CY" sleep 300
after=$(grep -c '^motion' "$OUT/source_t7.log")
echo "motion lines in source before/after: $before/$after (equal means the overlay took the input)"
sleep 2.5
grep -E "pointer-enter|pointer_events=[1-9]" "$OUT/overlay_input.log" | head -3
echo "focus afterwards: $("$B/kwin_windows/kwin_windows" | grep "$UUID" | sed -E 's/.*state=([^ ]+).*/\1/')"

echo
echo "################ T3: source frame rate under an opaque overlay ################"
echo "--- without capture ---"
t0=$(mono)
"$B/overlay/overlay" --output "$TARGET" --seconds 6 > "$OUT/overlay_t3a.log" 2>&1
t1=$(mono)
sleep 1.5
echo "overlay up from mono=$t0 to mono=$t1"
grep -E "^overlay mono" "$OUT/overlay_t3a.log" | sed -n '2,3p'
echo "source fps (mono fps):"
grep '^fps' "$OUT/source_t7.log" | sed -E 's/^fps=([0-9,.]+) mono=([0-9.,]+).*/\2 \1/' | tail -9 | tr '\n' ';'
echo
echo "--- with the window being captured ---"
timeout 20 "$B/kwin_cast/kwin_cast" --window "$UUID" --seconds 9 --cursor hidden > "$OUT/cast_t3.log" 2>&1 &
PIDS+=("$!")
sleep 2
t0=$(mono)
"$B/overlay/overlay" --output "$TARGET" --seconds 6 > "$OUT/overlay_t3b.log" 2>&1
t1=$(mono)
sleep 1.5
echo "overlay up from mono=$t0 to mono=$t1"
echo "source fps (mono fps):"
grep '^fps' "$OUT/source_t7.log" | sed -E 's/^fps=([0-9,.]+) mono=([0-9.,]+).*/\2 \1/' | tail -10 | tr '\n' ';'
echo
echo "capture (mono frames age):"
grep '^t=' "$OUT/cast_t3.log" | sed -E 's/^t=[0-9]+ mono=([0-9.]+) frames=([0-9]+).*age_ms\(min\/med\/max\)=([^ ]+).*/\1 \2 \3/' | tr '\n' ';'
echo
stop_source
echo "done"
