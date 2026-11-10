#!/usr/bin/env bash
# Runs magpie-viewer on a KDE Plasma desktop and prints what it measures.
# This is the part of the viewer's verification that needs a compositor; it
# opens windows, and some parts move the pointer by software, so nobody
# should use the machine while those run. All parts together take about seven
# minutes.
#
# It uses the test window and the helper programs of the Phase 0 feasibility
# tests, which have to be built first:
#   cmake -S linux/spikes -B linux/spikes/build -G Ninja && cmake --build linux/spikes/build
#
# Usage: viewer_desktop_test.sh [part...]
#   basic       capture in several configurations (windows only)
#   damage      does the compositor's damage agree with the pictures? (windows only)
#   resize      the source goes fullscreen and back (windows only)
#   portal      capture through the portal; a person has to answer its dialog
#   cursor      is the cursor drawn where it belongs? (moves the pointer)
#   cursoronly  a still source over which only the pointer moves (moves the pointer)
#   latency     the delay the viewer adds (moves the pointer)
# Without arguments: basic damage resize.
#
# Environment:
#   SOURCE_OUTPUT   monitor for the test window and, mostly, the viewer (default DP-6)
#   VIEWER_OUTPUT   monitor for the viewer in the cursor parts, where it must
#                   not cover the source (default HDMI-A-2)
#   OUT             directory for logs and pictures (default: a new one in /tmp)
set -u
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
B=$REPO/linux/build
SB=$REPO/linux/spikes/build
SOURCE_OUTPUT=${SOURCE_OUTPUT:-DP-6}
VIEWER_OUTPUT=${VIEWER_OUTPUT:-HDMI-A-2}
OUT=${OUT:-$(mktemp -d /tmp/magpie-viewer-test.XXXXXX)}
mkdir -p "$OUT"
PARTS=("$@")
[ ${#PARTS[@]} -eq 0 ] && PARTS=(basic damage resize)

VIEWER=$B/tools/magpie-viewer/magpie-viewer
TESTSOURCE="$SB/t8_testsource/testsource --no-focus --duration 600"
for program in "$VIEWER" "$SB/t8_testsource/testsource" "$SB/kwin_windows/kwin_windows" \
	"$SB/kwin_cast/kwin_cast" "$SB/kwin_pointer/kwin_pointer"; do
	[ -x "$program" ] || { echo "missing: $program"; exit 1; }
done
echo "logs and pictures: $OUT"

PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

windows() { "$SB/kwin_windows/kwin_windows"; }
pointer() { "$SB/kwin_pointer/kwin_pointer" "$@"; }
wants() { for part in "${PARTS[@]}"; do [ "$part" = "$1" ] && return 0; done; return 1; }

# place PID OUTPUT: moves the window of the process to the monitor; prints its id
place() {
	local uuid
	uuid=$(windows | grep "pid=$1 " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
	[ -n "$uuid" ] && "$SB/kwin_windows/kwin_windows" --send-to-output "$uuid" "$2" > /dev/null
	echo "$uuid"
}

geometry() { windows | grep "$1" | sed -E 's/.*(frame=[^ ]+).*(client=[^ ]+).*/\1 \2/'; }

# start_source LOG COMMAND...: sets SOURCE_PID, UUID, and OX, OY to the origin
# of the window's client area
start_source() {
	local log=$OUT/$1
	shift
	env "$@" > "$log" 2>&1 &
	SOURCE_PID=$!
	PIDS+=("$SOURCE_PID")
	sleep 1.5
	UUID=$(place "$SOURCE_PID" "$SOURCE_OUTPUT")
	[ -z "$UUID" ] && { echo "the source window did not appear"; exit 1; }
	sleep 0.8
	read -r OX OY <<< "$(windows | grep "$UUID" | sed -E 's/.*client=[0-9]+x[0-9]+([+-][0-9]+)([+-][0-9]+).*/\1 \2/' | tr -d '+')"
	echo "source $(geometry "$UUID")"
}

stop() { kill "$1" 2>/dev/null; wait "$1" 2>/dev/null; }

# start_viewer NAME OUTPUT ARGS...: sets VIEWER_PID and VUUID
start_viewer() {
	local name=$1 output=$2
	shift 2
	"$VIEWER" "$@" > "$OUT/$name.log" 2>&1 &
	VIEWER_PID=$!
	PIDS+=("$VIEWER_PID")
	sleep 1.2
	VUUID=$(place "$VIEWER_PID" "$output")
	sleep 1.0
}

# The viewer's report without the lines of --stats and the log
report() { grep -vE "^t=|^[0-9]{4}-" "$OUT/$1.log" | cut -c1-240; }

###############################################################################
if wants basic; then
	echo
	echo "################ basic: capture in several configurations ################"
	# shellcheck disable=SC2086
	start_source source_basic.log QT_QPA_PLATFORM=wayland $TESTSOURCE
	for spec in \
		"dmabuf:" \
		"shm:--no-dmabuf" \
		"validate:--validate --effect FSR/FSR_EASU,type=fit --effect FSR/FSR_RCAS" \
		"second_gpu:--gpu 1" \
		"fifo:--present-mode fifo" \
		"limit60:--max-fps 60 --effect Lanczos,type=fit"; do
		name=${spec%%:*}
		echo "===== $name: ${spec#*:} ====="
		# shellcheck disable=SC2086
		start_viewer "basic_$name" "$SOURCE_OUTPUT" --window "$UUID" --seconds 6 ${spec#*:}
		wait "$VIEWER_PID"
		echo "exit status $?"
		report "basic_$name" | grep -E "^Graphics|^Stream|^From|^ +[0-9]|^Shown|^Arrival|^Validation" | awk '!seen[$0]++'
	done
	stop "$SOURCE_PID"

	echo "===== an X11 client as the source ====="
	# shellcheck disable=SC2086
	start_source source_basic_x11.log QT_QPA_PLATFORM=xcb $TESTSOURCE
	start_viewer basic_x11 "$SOURCE_OUTPUT" --window "$UUID" --seconds 6
	wait "$VIEWER_PID"
	echo "exit status $?"
	report basic_x11 | grep -E "^Stream|^From|^ +[0-9]|^Shown|^Arrival" | awk '!seen[$0]++'
	stop "$SOURCE_PID"
fi

###############################################################################
if wants damage; then
	echo
	echo "################ damage: every frame is compared, the damage is not used ################"
	for spec in "animated_wayland:QT_QPA_PLATFORM=wayland:" "static_wayland:QT_QPA_PLATFORM=wayland:--static" \
		"animated_x11:QT_QPA_PLATFORM=xcb:"; do
		IFS=: read -r name platform extra <<< "$spec"
		# shellcheck disable=SC2086
		start_source "source_$name.log" "$platform" $TESTSOURCE $extra
		start_viewer "damage_$name" "$SOURCE_OUTPUT" --window "$UUID" --seconds 5 --duplicates always --no-damage
		wait "$VIEWER_PID"
		echo "--- $name ---"
		report "damage_$name" | grep -E "^From the source|^ +[0-9]"
		stop "$SOURCE_PID"
	done
fi

###############################################################################
if wants resize; then
	echo
	echo "################ resize: the source goes fullscreen and back ################"
	# shellcheck disable=SC2086
	start_source source_resize.log QT_QPA_PLATFORM=wayland $TESTSOURCE
	start_viewer resize "$SOURCE_OUTPUT" --window "$UUID" --seconds 10 --stats -v --validate
	sleep 1.5
	# State bit 8 is "fullscreen"
	"$SB/kwin_windows/kwin_windows" --set-state "$UUID" 8 8 > /dev/null
	echo "fullscreen: $(geometry "$UUID")"
	sleep 3
	"$SB/kwin_windows/kwin_windows" --set-state "$UUID" 8 0 > /dev/null
	echo "restored: $(geometry "$UUID")"
	wait "$VIEWER_PID"
	echo "exit status $?"
	grep -E "Capture stream format|^t=|^Shown|^Validation" "$OUT/resize.log" | sed -E 's/^[^|]*\|[^|]*\|[^|]*\|[^|]*\|//' | cut -c1-160
	stop "$SOURCE_PID"
fi

###############################################################################
if wants portal; then
	echo
	echo "################ portal: choose \"Magpie test source\" in the dialog ################"
	# shellcheck disable=SC2086
	start_source source_portal.log QT_QPA_PLATFORM=wayland $TESTSOURCE
	for run in "first:--pick" "second:"; do
		name=${run%%:*}
		echo "--- $name run: --portal ${run#*:} ---"
		# shellcheck disable=SC2086
		timeout 150 "$VIEWER" --portal ${run#*:} --seconds 6 -v > "$OUT/portal_$name.log" 2>&1 &
		pid=$!
		PIDS+=("$pid")
		# The viewer's window appears once the dialog has been answered
		for _ in $(seq 1 700); do
			sleep 0.2
			kill -0 "$pid" 2>/dev/null || break
			if windows | grep -q "pid=$pid "; then
				place "$pid" "$SOURCE_OUTPUT" > /dev/null
				break
			fi
		done
		wait "$pid"
		echo "exit status $?"
		# The time between these two lines is the time the dialog was open
		grep -E "Using graphics card|The portal casts" "$OUT/portal_$name.log" | cut -c1-23
		report "portal_$name" | grep -E "^Source|^Stream|^From|^Shown" | awk '!seen[$0]++'
	done
	stop "$SOURCE_PID"
fi

###############################################################################
if wants cursor; then
	echo
	echo "################ cursor: where it is drawn ################"
	# Without a title bar, so that positions in the stream are positions in the window
	# shellcheck disable=SC2086
	start_source source_cursor.log QT_QPA_PLATFORM=wayland $TESTSOURCE --frameless --static
	for spec in "a:200:150:--cursor-scale 0" "b:500:400:--cursor-scale 1" "c:320:240:--cursor-scale 3 --cursor-bilinear"; do
		IFS=: read -r tag px py extra <<< "$spec"
		for mode in with without; do
			args=()
			[ "$mode" = without ] && args+=(--no-cursor)
			# The viewer goes to another monitor: KWin reports no cursor for a
			# source that is covered at the pointer's position
			# shellcheck disable=SC2086
			start_viewer "cursor_${tag}_$mode" "$VIEWER_OUTPUT" --window "$UUID" --seconds 6 -v \
				--effect Nearest,scale=2 --window-size 1280x960 $extra "${args[@]}"
			# KWin reports the cursor when it moves
			pointer move $((OX + px - 1)) $((OY + py)) sleep 60 move $((OX + px)) $((OY + py)) sleep 300
			timeout 10 "$SB/kwin_cast/kwin_cast" --window "$VUUID" --cursor hidden --no-dmabuf --seconds 2 \
				--dump "$OUT/cursor_${tag}_$mode.ppm" > "$OUT/cast_cursor_${tag}_$mode.log" 2>&1
			wait "$VIEWER_PID"
		done
		# Whether the image's colours are multiplied by alpha, as the drawing assumes
		grep -E "Cursor image|cursor image" "$OUT/cursor_${tag}_with.log" | sed -E 's/^[^|]*\|[^|]*\|[^|]*\|[^|]*\|//' | awk '!seen[$0]++'
		report "cursor_${tag}_with" | grep -E "^Cursor"
		scale=${extra#--cursor-scale }
		python3 "$(dirname "$0")/viewer_cursor_check.py" "$OUT/cursor_${tag}_with.ppm" "$OUT/cursor_${tag}_without.ppm" \
			"$px" "$py" "${scale%% *}"
	done
	stop "$SOURCE_PID"
fi

###############################################################################
if wants cursoronly; then
	echo
	echo "################ cursoronly: a still source, the pointer circles over it ################"
	"$B/tools/magpie-present-test/magpie-present-test" --source-fps 1 --seconds 16 --window-size 640x360 > "$OUT/still_source.log" 2>&1 &
	STILL_PID=$!
	PIDS+=("$STILL_PID")
	sleep 1.5
	SUUID=$(place "$STILL_PID" "$SOURCE_OUTPUT")
	sleep 0.8
	read -r OX OY <<< "$(windows | grep "$SUUID" | sed -E 's/.*client=[0-9]+x[0-9]+([+-][0-9]+)([+-][0-9]+).*/\1 \2/' | tr -d '+')"

	# 600 positions on a circle, one every 5 ms
	read -r -a moves <<< "$(python3 -c "
import math
print(' '.join('move %d %d sleep 5' % ($OX + 320 + 100 * math.cos(i * 0.05), $OY + 180 + 100 * math.sin(i * 0.05)) for i in range(600)))")"
	start_viewer cursoronly "$VIEWER_OUTPUT" --window "$SUUID" --seconds 8 --stats
	pointer "${moves[@]}"
	wait "$VIEWER_PID"
	grep -E "^t=" "$OUT/cursoronly.log" | cut -c1-160
	report cursoronly | grep -E "^From|^ +[0-9]|^Shown"
	stop "$STILL_PID"
fi

###############################################################################
if wants latency; then
	echo
	echo "################ latency: age of the test window's frames on arrival ################"
	# The test window draws the time of each frame as a row of cells, which
	# kwin_cast reads from what it captures. Captured from the source itself
	# and from the viewer's window showing the source 1:1, the difference is
	# what the viewer adds.
	# shellcheck disable=SC2086
	start_source source_latency.log QT_QPA_PLATFORM=wayland $TESTSOURCE --frameless
	pointer move $((OX + 320)) $((OY + 400))

	ages() {
		grep -E "^t=" "$1" | sed -E 's/.*frames=([0-9]+).*age_ms\(min\/med\/max\)=([^ ]+).*/frames=\1 age min\/median\/max=\2 ms/' | tail -3
	}

	echo "--- the source, captured directly ---"
	timeout 12 "$SB/kwin_cast/kwin_cast" --window "$UUID" --cursor hidden --seconds 4 > "$OUT/cast_direct1.log" 2>&1
	ages "$OUT/cast_direct1.log"
	for mode in mailbox fifo; do
		echo "--- the viewer's window, present mode $mode ---"
		start_viewer "latency_$mode" "$SOURCE_OUTPUT" --window "$UUID" --seconds 9 --present-mode "$mode" \
			--effect Nearest,scale=1 --window-size 640x480 --no-cursor
		timeout 12 "$SB/kwin_cast/kwin_cast" --window "$VUUID" --cursor hidden --seconds 4 > "$OUT/cast_viewer_$mode.log" 2>&1
		ages "$OUT/cast_viewer_$mode.log"
		wait "$VIEWER_PID"
		report "latency_$mode" | grep -E "^Arrival|^Shown"
	done
	echo "--- the source, captured directly again ---"
	timeout 12 "$SB/kwin_cast/kwin_cast" --window "$UUID" --cursor hidden --seconds 4 > "$OUT/cast_direct2.log" 2>&1
	ages "$OUT/cast_direct2.log"
	stop "$SOURCE_PID"
fi
