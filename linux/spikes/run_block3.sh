#!/usr/bin/env bash
# Two follow-up tests, about 45 s, hands off the mouse.
#
# T9: an X11 test window opens on the home output (default DP-6). The pointer
#     circles over it for 9 s; for 4 of those seconds the cursor is hidden
#     through the X server (XFixesHideCursor).
# T5b: the 640x480 virtual monitor is created again for at most 25 s. A custom
#     200 Hz mode is added to it with kscreen-doctor and selected, and the test
#     window is run on it to see which frame rate it reaches.
#     This writes display configuration for the virtual output only. A copy of
#     ~/.config/kwinoutputconfig.json is saved first and compared afterwards.
set -u
cd "$(dirname "$0")"
B=$PWD/build
OUT=out/block3
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
uuid_of() { windows | grep "pid=$1 " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1; }
client_origin() { windows | grep "$1" | sed -E 's/.*client=[0-9]+x[0-9]+([+-][0-9]+)([+-][0-9]+).*/\1 \2/' | tr -d '+'; }

layout > "$OUT/layout_before.txt"
cp ~/.config/kwinoutputconfig.json "$OUT/kwinoutputconfig.before.json" 2>/dev/null
read -r HX HY HW HH <<< "$(grep -A6 " $HOME_OUTPUT " "$OUT/layout_before.txt" | grep Geometry | sed -E 's/.*Geometry: ([0-9]+),([0-9]+) ([0-9]+)x([0-9]+).*/\1 \2 \3 \4/')"

echo "################ T9: hide and read the cursor through the X server ################"
QT_QPA_PLATFORM=xcb "$B/t8_testsource/testsource" --frameless --duration 16 \
	--message "Automatic test. Please do not use the mouse." > "$OUT/source_x11.log" 2>&1 &
SOURCE_PID=$!
PIDS+=("$SOURCE_PID")
sleep 1.5
UUID=$(uuid_of "$SOURCE_PID")
"$B/kwin_windows/kwin_windows" --send-to-output "$UUID" "$HOME_OUTPUT" > /dev/null
sleep 0.8
read -r OX OY <<< "$(client_origin "$UUID")"
echo "X11 test window client origin=$OX,$OY"
timeout 20 "$B/kwin_cast/kwin_cast" --window "$UUID" --seconds 9 \
	--inject-circle $((OX + 320)) $((OY + 240)) 100 --origin "$OX" "$OY" > "$OUT/cast_x11.log" 2>&1 &
PIDS+=("$!")
sleep 2
"$B/x11_cursor/x11_cursor" --hide 4 --seconds 7 > "$OUT/x11_cursor.log" 2>&1
sleep 1.5
echo "--- X server view (every other sample) ---"
awk 'NR==1 || /XFixes/ || NR%3==0' "$OUT/x11_cursor.log" | cut -c1-200
echo "--- stream cursor metadata per second (mono, frames, metas, with bitmap, bitmap size, lag) ---"
grep '^t=' "$OUT/cast_x11.log" | sed -E 's/^t=[0-9]+ mono=([0-9.]+) frames=([0-9]+).*cursor_lag_ms\(min\/med\/max\)=([^ ]+) cursor_meta=([0-9]+) cursor_moves=([0-9]+) bitmaps=([0-9]+) pos=([-0-9,]+) bitmap=([0-9x]+).*/\1 frames=\2 metas=\4 bitmaps=\6 size=\8 lag=\3/'
kill "$SOURCE_PID" 2>/dev/null
wait "$SOURCE_PID" 2>/dev/null

echo
echo "################ T5b: custom refresh rate on the virtual output ################"
timeout 35 "$B/kwin_cast/kwin_cast" --virtual MagpieTest 640x480 --seconds 25 > "$OUT/cast_virtual.log" 2>&1 &
CAST_PID=$!
PIDS+=("$CAST_PID")
sleep 2.5
VNAME=$(windows | grep '^outputs:' | tr ' ' '\n' | grep -vE "^(outputs:|HDMI-A-2|DP-5|DP-6)$" | head -1)
echo "virtual output: $VNAME"
echo "--- addCustomMode 640x480 @ 200 Hz ---"
kscreen-doctor "output.$VNAME.addCustomMode.640.480.200000.full" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | tail -3
sleep 1.5
layout | grep -A5 "$VNAME" | grep -E "Modes|Geometry"
echo "--- select the 200 Hz mode ---"
kscreen-doctor "output.$VNAME.mode.640x480@200" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | tail -3
sleep 1.5
layout | grep -A5 "$VNAME" | grep -E "Modes|Geometry"
read -r VX VY <<< "$(layout | grep -A6 "$VNAME" | grep Geometry | head -1 | sed -E 's/.*Geometry: (-?[0-9]+),(-?[0-9]+) .*/\1 \2/')"

"$B/t8_testsource/testsource" --frameless --duration 12 > "$OUT/source_virtual.log" 2>&1 &
SOURCE_PID=$!
PIDS+=("$SOURCE_PID")
sleep 1.5
UUID=$(uuid_of "$SOURCE_PID")
"$B/kwin_windows/kwin_windows" --send-to-output "$UUID" "$VNAME" > /dev/null
sleep 0.5
"$B/kwin_windows/kwin_windows" --set-state "$UUID" 8 8 > /dev/null
sleep 6
"$B/kwin_windows/kwin_windows" --set-state "$UUID" 8 0 > /dev/null
"$B/kwin_windows/kwin_windows" --send-to-output "$UUID" "$HOME_OUTPUT" > /dev/null
sleep 0.5
echo "--- client on the virtual output ---"
grep -E "screen-changed" "$OUT/source_virtual.log"
echo "client fps per second: $(grep '^fps' "$OUT/source_virtual.log" | sed -E 's/^fps=([0-9,.]+).*screen=([^ ]+).*/\1@\2/' | tr '\n' ' ')"
echo "stream format lines: $(grep '^format' "$OUT/cast_virtual.log" | sed -E 's/.*(max_framerate=[0-9/]+).*/\1/' | tr '\n' ' ')"
echo "stream frames per second: $(grep '^t=' "$OUT/cast_virtual.log" | sed -E 's/.* frames=([0-9]+).*/\1/' | tr '\n' ' ')"
kill "$SOURCE_PID" 2>/dev/null
"$B/kwin_pointer/kwin_pointer" move $((HX + HW / 2)) $((HY + HH / 2))

kill "$CAST_PID" 2>/dev/null
wait "$CAST_PID" 2>/dev/null
sleep 2
layout > "$OUT/layout_after.txt"
echo
echo "################ afterwards ################"
if diff -q "$OUT/layout_before.txt" "$OUT/layout_after.txt" > /dev/null; then
	echo "monitor layout: identical to before"
else
	echo "monitor layout DIFFERENT from before:"
	diff "$OUT/layout_before.txt" "$OUT/layout_after.txt"
fi
if diff -q "$OUT/kwinoutputconfig.before.json" ~/.config/kwinoutputconfig.json > /dev/null 2>&1; then
	echo "kwinoutputconfig.json: unchanged"
else
	echo "kwinoutputconfig.json changed:"
	diff "$OUT/kwinoutputconfig.before.json" ~/.config/kwinoutputconfig.json | head -40
fi
echo "done"
