#!/usr/bin/env bash
# How long a frame takes from being drawn by a program to being shown on the
# monitor, on a KDE Plasma desktop: without scaling, and scaled by Magpie in
# several variants (ways of presenting, a pointer on the picture, a source at
# 60 frames per second, heavier effects, the other graphics card, windowed
# scaling, a nested session). It measures and prints; it has no checks that
# pass or fail.
#
# The source is magpie_latency_source, a 640x480 window that numbers its
# frames, on the monitor DP-6. The times come from the compositor
# (presentation-time), so they end where the picture is handed to the
# monitor. See latency_report.py for the lines it prints.
#
# It moves the pointer and sends clicks to the test window, and the nested
# parts open a fullscreen window that has the keyboard, so nobody should use
# the machine while it runs (about four minutes for all parts). The numbers
# in it assume 1920x1080 monitors with DP-5 at 1920,0 and DP-6 at 3840,0,
# which is the reference machine.
#
# It uses helper programs of the Phase 0 feasibility tests, which have to be
# built first:
#   cmake -S linux/spikes -B linux/spikes/build -G Ninja && cmake --build linux/spikes/build
#
# Usage: latency_desktop_test.sh [PART]...
#   direct      the source by itself (always run; the others compare with it)
#   fullscreen  fullscreen scaling, pointer on another monitor
#   present     the same with the present modes fifo and immediate
#   pointer     the pointer on the picture, and ten clicks
#   fps60       a source that draws 60 times per second
#   effect      two effects that take the graphics card longer
#   gpu         scaled on the second graphics card
#   windowed    windowed scaling
#   nested      nested sessions with the three present modes
# Environment: OUT, the directory for logs (default: a new one in /tmp);
#   RUN_SECONDS, how long each variant is scaled (default 6)
set -u
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
PARTS=("$@")
[ ${#PARTS[@]} -eq 0 ] && PARTS=(fullscreen present pointer fps60 effect gpu windowed nested)
wants() { local p; for p in "${PARTS[@]}"; do [ "$p" = "$1" ] && return 0; done; return 1; }
B=$REPO/linux/build
SB=$REPO/linux/spikes/build
OUT=${OUT:-$(mktemp -d /tmp/magpie-latency-test.XXXXXX)}
mkdir -p "$OUT"
RUN_SECONDS=${RUN_SECONDS:-6}
MONITOR=DP-6
SOURCE=$B/tests/magpie_latency_source
SCALE=$B/tools/magpie-scale/magpie-scale
REPORT=$REPO/linux/tests/tools/latency_report.py
echo "logs: $OUT"

# The scaler and the service keep what they write away from the user's files
export XDG_STATE_HOME=$OUT/state MAGPIE_CONFIG_DIR=$OUT/config

PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

windows() { "$SB/kwin_windows/kwin_windows"; }
pointer() { "$SB/kwin_pointer/kwin_pointer" "$@"; }

# Where the pointer is, as "X Y", through a script that KWin runs
busctl --user monitor --json=short --match="type='method_call',interface='org.magpie.Probe'" > "$OUT/probe.jsonl" 2>/dev/null &
PIDS+=("$!")
sleep 0.5
cat > "$OUT/probe.js" <<'EOF'
callDBus("org.magpie.Probe", "/Probe", "org.magpie.Probe", "Report",
	Math.round(workspace.cursorPos.x), Math.round(workspace.cursorPos.y));
EOF
pointer_position() {
	local name="magpie-probe-$RANDOM"
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting loadScript ss "$OUT/probe.js" "$name" > /dev/null
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting start > /dev/null
	sleep 0.3
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting unloadScript s "$name" > /dev/null
	tail -1 "$OUT/probe.jsonl" | python3 -c "
import json, sys
try:
    data = json.loads(sys.stdin.readline())['payload']['data']
    print(data[0], data[1])
except Exception:
    pass
"
}

# start_source NAME [ARGUMENT]...: the source on the monitor; sets SOURCE_PID,
# SOURCE_LOG and UUID
start_source() {
	local name=$1
	shift
	SOURCE_LOG=$OUT/source_$name.log
	"$SOURCE" --log "$SOURCE_LOG" --duration 600 "$@" 2> "$OUT/source_$name.err" &
	SOURCE_PID=$!
	PIDS+=("$SOURCE_PID")
	sleep 1.2
	UUID=$(windows | grep "pid=$SOURCE_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
	"$SB/kwin_windows/kwin_windows" --send-to-output "$UUID" "$MONITOR" > /dev/null
	sleep 0.8
}
stop_source() { kill "$SOURCE_PID" 2>/dev/null; wait "$SOURCE_PID" 2>/dev/null; sleep 0.3; }

# direct NAME [ARGUMENT]...: the source by itself for some seconds; its log
# is the baseline of what follows
direct() {
	local name=$1
	shift
	echo
	echo "--- $name: the source without scaling ---"
	"$SOURCE" --log "$OUT/direct_$name.log" --duration 600 "$@" 2> "$OUT/direct_$name.err" &
	local pid=$!
	PIDS+=("$pid")
	sleep 1.2
	local uuid
	uuid=$(windows | grep "pid=$pid " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
	"$SB/kwin_windows/kwin_windows" --send-to-output "$uuid" "$MONITOR" > /dev/null
	sleep 0.8
	# Only what was drawn once the window was on the monitor counts
	local skip
	skip=$(wc -l < "$OUT/direct_$name.log")
	sleep 4
	kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	BASELINE=$OUT/baseline_$name.log
	tail -n "+$((skip + 1))" "$OUT/direct_$name.log" > "$BASELINE"
	python3 "$REPORT" "$BASELINE"
}

# scale NAME [ARGUMENT]...: scales the running source and reports
scale() {
	local name=$1
	shift
	echo
	echo "--- $name ---"
	MAGPIE_LATENCY_LOG=$OUT/magpie_$name.log "$SCALE" --window "$UUID" --seconds "$RUN_SECONDS" -v "$@" > "$OUT/scale_$name.log" 2>&1 &
	SCALE_PID=$!
	PIDS+=("$SCALE_PID")
}
finish_scale() {
	local name=$1
	wait "$SCALE_PID"
	local status=$?
	[ "$status" -ne 0 ] && echo "magpie-scale ended with status $status"
	grep -E "^# " "$OUT/magpie_$name.log" 2>/dev/null | head -1 | cut -c3-240
	grep -E "Swap chain|error|warn|Scaling did not" "$OUT/scale_$name.log" | sed -E 's/^[^|]*\|[^|]*\|[^|]*\|[^|]*\|//' | cut -c1-200 | head -5
	python3 "$REPORT" --baseline "$BASELINE" "$SOURCE_LOG" "$OUT/magpie_$name.log" | tail -n +5
	sleep 0.7
}
scale_and_report() { scale "$@"; finish_scale "$1"; }

echo "KWin $(kwin_wayland --version 2>/dev/null | sed 's/kwin //'); tearing allowed in KWin's settings: $(kreadconfig6 --file kwinrc --group Compositing --key AllowTearing --default true)"
kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -E "^Output|Geometry|Vrr:" | paste - - - |
	sed -E 's/Output: [0-9]+ //; s/ [0-9a-f-]{36}//; s/\t+/ /g'
START_POINTER=$(pointer_position)
echo "pointer at the start: ${START_POINTER:-unknown}"

# The pointer away from the monitor of the test: in the middle of DP-5
pointer move 2880 540

direct free
BASELINE_FREE=$BASELINE

if wants fullscreen || wants present || wants pointer || wants effect || wants gpu || wants windowed; then
	start_source free
	BASELINE=$BASELINE_FREE
	if wants fullscreen; then
		scale_and_report fullscreen_mailbox --effect Lanczos,type=fit --present-mode mailbox
	fi
	if wants present; then
		scale_and_report fullscreen_fifo --effect Lanczos,type=fit --present-mode fifo
		scale_and_report fullscreen_immediate --effect Lanczos,type=fit --present-mode immediate
	fi
	if wants pointer; then
		# The middle of the picture, where the source has the square that
		# answers a click
		scale pointer_on_picture --effect Lanczos,type=fit --present-mode mailbox
		sleep 2
		pointer move 4800 540 sleep 500
		for _ in 1 2 3 4 5 6 7 8 9 10; do pointer click sleep 250; done
		finish_scale pointer_on_picture
		pointer move 2880 540
	fi
	if wants effect; then
		scale_and_report effect_fsr --effect FSR/FSR_EASU,type=fit --effect FSR/FSR_RCAS
		scale_and_report effect_anime4k_ul --effect Anime4K/Anime4K_Upscale_UL,type=fit
		scale_and_report effect_artcnn --effect Ani4Kv2_ArtCNN_C4F32_i2,type=fit
	fi
	if wants gpu; then
		scale_and_report gpu1_mailbox --gpu 1 --effect Lanczos,type=fit --present-mode mailbox
	fi
	if wants windowed; then
		scale_and_report windowed_mailbox --windowed --effect Lanczos,type=fit --present-mode mailbox
	fi
	stop_source
fi

if wants fps60; then
	direct fps60 --fps 60
	start_source fps60 --fps 60
	scale_and_report fps60_mailbox --effect Lanczos,type=fit --present-mode mailbox
	scale_and_report fps60_fifo --effect Lanczos,type=fit --present-mode fifo
	stop_source
fi

if wants nested; then
	if busctl --user status io.github.magazine0410.Magpie > /dev/null 2>&1; then
		echo
		echo "Magpie is running, so the nested parts are skipped (the service can run only once)."
	else
		# The desktop opens the fullscreen window on the monitor with the pointer
		pointer move 4800 540
		for mode in mailbox fifo immediate; do
			echo
			echo "--- nested_$mode ---"
			MAGPIE_LATENCY_LOG=$OUT/magpie_nested_$mode.log MAGPIE_PRESENT_MODE=$mode \
				"$B/tools/magpie-engine/magpie-engine" --effects-dir "$REPO/src/Effects" --mode Lanczos \
				--no-shortcuts --no-cursor-speed -v > "$OUT/engine_nested_$mode.log" 2>&1 &
			ENGINE_PID=$!
			PIDS+=("$ENGINE_PID")
			sleep 1.5
			SOURCE_LOG=$OUT/source_nested_$mode.log
			"$B/tools/magpie-ctl/magpie-ctl" launch --size 640x480 -- "$SOURCE" --log "$SOURCE_LOG" --duration 600 > /dev/null
			sleep 2.5
			if [ "$mode" = mailbox ]; then
				for _ in 1 2 3 4 5 6 7 8 9 10; do pointer click sleep 250; done
			else
				sleep 2.5
			fi
			sleep "$((RUN_SECONDS > 5 ? RUN_SECONDS - 5 : 1))"
			# Ends the session and with it the program
			"$B/tools/magpie-ctl/magpie-ctl" stop > /dev/null 2>&1
			sleep 0.5
			kill "$ENGINE_PID" 2>/dev/null
			wait "$ENGINE_PID" 2>/dev/null
			grep -E "^# " "$OUT/magpie_nested_$mode.log" 2>/dev/null | head -1 | cut -c3-240
			grep -E "Swap chain|error|warn|did not" "$OUT/engine_nested_$mode.log" | sed -E 's/^[^|]*\|[^|]*\|[^|]*\|[^|]*\|//' | cut -c1-200 | head -5
			python3 "$REPORT" --baseline "$BASELINE_FREE" "$SOURCE_LOG" "$OUT/magpie_nested_$mode.log" | tail -n +5
			sleep 0.7
		done
	fi
fi

# shellcheck disable=SC2086
[ -n "$START_POINTER" ] && pointer move $START_POINTER
echo
echo "pointer at the end: $(pointer_position)"
echo "left running: $(pgrep -x magpie-engine > /dev/null && echo magpie-engine) $(pgrep -x magpie_latency_ > /dev/null && echo source) $(pgrep -x sway > /dev/null && echo sway)"
echo "outputs: $(kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -E "^Output" | sed -E 's/Output: [0-9]+ ([^ ]+).*/\1/' | tr '\n' ' ')"
