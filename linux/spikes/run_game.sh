#!/usr/bin/env bash
# Real-game test for Strategy B: move a running game to a virtual output of its
# own size, hover the pointer over it (no clicks, no keys), and move it back.
#
# Usage: run_game.sh WINDOW_UUID GAME_PID WIDTHxHEIGHT HOME_OUTPUT HOVER_X HOVER_Y [HOVER2_X HOVER2_Y]
# HOVER_* are positions inside the game image.
set -u
cd "$(dirname "$0")"
B=$PWD/build
O=out/game
U=$1
GPID=$2
SIZE=$3
HOME_OUTPUT=$4
HOVER_X=$5
HOVER_Y=$6
HOVER2_X=${7:-}
HOVER2_Y=${8:-}
VN=Virtual-MagpieGame
mkdir -p "$O"

layout() {
	kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' \
		| grep -E "^Output|Geometry|priority|Scale|Modes|enabled|connected" | cut -c1-150
}
show() { "$B/kwin_windows/kwin_windows" | grep "$U" | sed -E 's/ title=.*//; s/^window uuid=\{[^}]+\} pid=[0-9]+ app_id="[^"]+" resource="[^"]+" //'; }
alive() { kill -0 "$GPID" 2>/dev/null && echo alive || echo GONE; }
last() { grep '^t=' "$O/cast_game.log" | tail -1 | sed -E 's/^t=([0-9]+) mono=[0-9.]+ (frames=[0-9]+) (cursor_only=[0-9]+).*(cursor_meta=[0-9]+).*(pos=[-0-9,]+ bitmap=[0-9x]+).*/t=\1 \2 \3 \4 \5/'; }

layout > "$O/layout_before.txt"
read -r HX HY HW HH <<< "$(grep -A6 " $HOME_OUTPUT " "$O/layout_before.txt" | grep Geometry | sed -E 's/.*Geometry: ([0-9]+),([0-9]+) ([0-9]+)x([0-9]+).*/\1 \2 \3 \4/')"
echo "before:        $(show) [$(alive)]"

"$B/kwin_cast/kwin_cast" --virtual MagpieGame "$SIZE" --seconds 45 --no-dmabuf --dump "$O/vout" > "$O/cast_game.log" 2>&1 &
CAST_PID=$!
trap 'kill $CAST_PID 2>/dev/null' EXIT
sleep 3
grep -E "^format|refused" "$O/cast_game.log" | sed -E 's/Spa:Enum:VideoFormat://' | cut -c1-110
layout | grep -A6 "$VN" | grep -E "Modes|Geometry"
read -r VX VY <<< "$(layout | grep -A6 "$VN" | grep Geometry | head -1 | sed -E 's/.*Geometry: (-?[0-9]+),(-?[0-9]+) .*/\1 \2/')"
if ! grep -q " $SIZE " "$O/cast_game.log"; then
	echo "virtual output does not have the requested size: stopping"
	exit 1
fi

"$B/kwin_windows/kwin_windows" --send-to-output "$U" "$VN" > /dev/null
sleep 3
echo "on virtual:    $(show) [$(alive)]"
kill -USR1 "$CAST_PID"
sleep 0.5

echo "--- pointer to $HOVER_X,$HOVER_Y in the game (motion only) ---"
"$B/kwin_pointer/kwin_pointer" move $((VX + HOVER_X - 2)) $((VY + HOVER_Y - 2)) sleep 300 move $((VX + HOVER_X)) $((VY + HOVER_Y)) sleep 1200
echo "X server:      $(timeout 4 "$B/x11_cursor/x11_cursor" --seconds 1 | sed -n '2p' | sed -E 's/^mono=[0-9.]+ //' | cut -c1-150)"
echo "stream:        $(last)"
kill -USR1 "$CAST_PID"
sleep 0.5

if [ -n "$HOVER2_X" ]; then
	echo "--- pointer to $HOVER2_X,$HOVER2_Y ---"
	"$B/kwin_pointer/kwin_pointer" move $((VX + HOVER2_X)) $((VY + HOVER2_Y)) sleep 1500
	echo "stream:        $(last)"
	kill -USR1 "$CAST_PID"
	sleep 0.5
fi

echo "--- back to $HOME_OUTPUT ---"
"$B/kwin_pointer/kwin_pointer" move $((HX + HW / 2)) $((HY + HH / 2)) sleep 200
"$B/kwin_windows/kwin_windows" --send-to-output "$U" "$HOME_OUTPUT" > /dev/null
sleep 2.5
echo "back:          $(show) [$(alive)]"
echo "stream frames per second while it ran: $(grep '^t=' "$O/cast_game.log" | sed -E 's/.* frames=([0-9]+) cursor_only=([0-9]+).*/\1+\2/' | tr '\n' ' ')"
kill "$CAST_PID" 2>/dev/null
wait "$CAST_PID" 2>/dev/null
sleep 2.5

timeout 10 "$B/kwin_cast/kwin_cast" --window "$U" --seconds 2 --no-dmabuf --dump "$O/after.ppm" 2>&1 | grep -E "^format|^wrote" | sed -E 's/Spa:Enum:VideoFormat://' | cut -c1-110
if diff -q "$O/layout_before.txt" <(layout) > /dev/null; then
	echo "monitor layout: identical to before"
else
	echo "monitor layout DIFFERENT:"
	diff "$O/layout_before.txt" <(layout)
fi
echo "game: $(alive)"
