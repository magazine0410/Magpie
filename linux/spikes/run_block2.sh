#!/usr/bin/env bash
# T5 and the rest of T6: a temporary virtual output.
#
# What happens, in about a minute:
#   * a 640x480 virtual monitor named "MagpieTest" is created through KWin's
#     screencast protocol and exists for at most 40 s;
#   * the test source window (first as a Wayland client, then as an X11 client
#     under Xwayland) loses its title bar, is moved to the virtual monitor, made
#     fullscreen there, receives a software pointer move and one click, and is
#     moved back to the home output (default DP-6);
#   * the pointer ends in the middle of the home output.
# The virtual monitor disappears when the script ends or is interrupted.
set -u
cd "$(dirname "$0")"
B=$PWD/build
OUT=out/block2
HOME_OUTPUT=${1:-DP-6}
mkdir -p "$OUT"

PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

layout() {
	kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' \
		| grep -E "^Output|Geometry|priority|Scale|Modes|enabled|connected" | cut -c1-170
}
windows() { "$B/kwin_windows/kwin_windows"; }
show() { windows | grep "$UUID" | sed -E 's/ title=.*//; s/^window //'; }

layout > "$OUT/layout_before.txt"
HOME_GEOMETRY=$(grep -A6 " $HOME_OUTPUT " "$OUT/layout_before.txt" | grep Geometry | sed -E 's/.*Geometry: ([0-9]+),([0-9]+) ([0-9]+)x([0-9]+).*/\1 \2 \3 \4/')
read -r HX HY HW HH <<< "$HOME_GEOMETRY"

echo "################ Xwayland client: plain window capture (as in T2) ################"
QT_QPA_PLATFORM=xcb "$B/t8_testsource/testsource" --frameless --duration 12 > "$OUT/source_x11_capture.log" 2>&1 &
X_PID=$!
PIDS+=("$X_PID")
sleep 1.5
UUID=$(windows | grep "pid=$X_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
show
timeout 15 "$B/kwin_cast/kwin_cast" --window "$UUID" --seconds 4 --vk 0 2>&1 | grep -E "^RESULT|^format|^t=" | cut -c1-330
grep -E "^started|^fps" "$OUT/source_x11_capture.log" | sed -n '1p;3p'
kill "$X_PID" 2>/dev/null

echo
echo "################ T5: create the virtual output ################"
timeout 50 "$B/kwin_cast/kwin_cast" --virtual MagpieTest 640x480 --seconds 40 > "$OUT/cast_virtual.log" 2>&1 &
CAST_PID=$!
PIDS+=("$CAST_PID")
sleep 2.5
grep -E "^RESULT|^format|refused|not offered" "$OUT/cast_virtual.log"
layout > "$OUT/layout_during.txt"
VNAME=$(windows | grep '^outputs:' | tr ' ' '\n' | grep -vE "^(outputs:|HDMI-A-2|DP-5|DP-6)$" | head -1)
if [ -z "$VNAME" ]; then
	echo "RESULT: no new output appeared"
	cat "$OUT/cast_virtual.log"
	exit 1
fi
echo "new output as seen by Wayland clients: $VNAME"
echo "--- as seen by KScreen ---"
diff "$OUT/layout_before.txt" "$OUT/layout_during.txt" | grep '^>' | sed 's/^> //'
VGEOMETRY=$(grep -A6 -i "magpietest\|$VNAME" "$OUT/layout_during.txt" | grep Geometry | head -1 | sed -E 's/.*Geometry: (-?[0-9]+),(-?[0-9]+) ([0-9]+)x([0-9]+).*/\1 \2 \3 \4/')
read -r VX VY VW VH <<< "$VGEOMETRY"
echo "virtual output geometry: ${VW}x${VH} at $VX,$VY"

run_client() {
	local label=$1 platform=$2
	local log="$OUT/source_$label.log"
	echo
	echo "################ T5/T6: $label client on the virtual output ################"
	QT_QPA_PLATFORM=$platform "$B/t8_testsource/testsource" --duration 30 \
		--message "Automatic test. Please do not use the mouse." > "$log" 2>&1 &
	SOURCE_PID=$!
	PIDS+=("$SOURCE_PID")
	sleep 1.5
	UUID=$(windows | grep "pid=$SOURCE_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
	if [ -z "$UUID" ]; then
		echo "test window not found"
		return
	fi
	echo "start:        $(show)"
	"$B/kwin_windows/kwin_windows" --set-state "$UUID" 80000 80000 > /dev/null
	sleep 0.5
	echo "no_border:    $(show)"
	"$B/kwin_windows/kwin_windows" --send-to-output "$UUID" "$VNAME" > /dev/null
	sleep 1
	echo "on virtual:   $(show)"
	"$B/kwin_windows/kwin_windows" --set-state "$UUID" 8 8 > /dev/null
	sleep 2.5
	echo "fullscreen:   $(show)"

	"$B/kwin_pointer/kwin_pointer" move $((VX + 320)) $((VY + 240)) sleep 300 move $((VX + 340)) $((VY + 240)) sleep 400 click sleep 400
	echo "stream after pointer move: $(grep '^t=' "$OUT/cast_virtual.log" | tail -1 | sed -E 's/.*(frames=[0-9]+).*(age_ms[^ ]+ ).*(cursor_meta=[0-9]+).*(pos=[-0-9,]+ bitmap=[0-9x]+).*/\1 \2\3 \4/')"
	sleep 1.5

	"$B/kwin_pointer/kwin_pointer" move $((HX + HW / 2)) $((HY + HH / 2)) sleep 300
	"$B/kwin_windows/kwin_windows" --set-state "$UUID" 8 0 > /dev/null
	sleep 0.5
	"$B/kwin_windows/kwin_windows" --send-to-output "$UUID" "$HOME_OUTPUT" > /dev/null
	sleep 1
	echo "back home:    $(show)"
	"$B/kwin_windows/kwin_windows" --set-state "$UUID" 80000 0 > /dev/null
	sleep 0.5
	echo "border back:  $(show)"

	echo "--- what the client saw ---"
	grep -E "^started|screen-changed" "$log"
	echo "fps per second: $(grep '^fps' "$log" | sed -E 's/^fps=([0-9,.]+).*screen=([^ ]+).*/\1@\2/' | tr '\n' ' ')"
	echo "pointer events: $(grep -E '^(press|release|pointer-|motion)' "$log" | sed -E 's/ events=[0-9]+//' | tr '\n' ';' | cut -c1-300)"
	kill "$SOURCE_PID" 2>/dev/null
	wait "$SOURCE_PID" 2>/dev/null
}

run_client wayland wayland
run_client x11 xcb

echo
echo "################ stream of the virtual output, per second ################"
grep '^t=' "$OUT/cast_virtual.log" | sed -E 's/^t=([0-9]+) mono=[0-9.]+ (frames=[0-9]+) (cursor_only=[0-9]+).*(type=[a-z-]+) (age_ms[^ ]+).*(cursor_meta=[0-9]+).*(pos=[-0-9,]+).*/\1:\2 \3 \4 \5 \6 \7/' | tr '\n' ';' | cut -c1-2400
echo

kill "$CAST_PID" 2>/dev/null
wait "$CAST_PID" 2>/dev/null
sleep 2
layout > "$OUT/layout_after.txt"
echo
echo "################ layout after the virtual output is gone ################"
if diff -q "$OUT/layout_before.txt" "$OUT/layout_after.txt" > /dev/null; then
	echo "identical to the layout before the test"
else
	echo "DIFFERENT from before:"
	diff "$OUT/layout_before.txt" "$OUT/layout_after.txt"
fi
echo "done"
