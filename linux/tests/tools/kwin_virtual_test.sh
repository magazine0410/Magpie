#!/usr/bin/env bash
# Scaling on KWin, in a KWin that this script starts itself and that shows
# nothing: fullscreen and windowed scaling of the test window, the same on
# an output with a scale factor, a monitor that is disabled while a window
# is scaled on it, and a KWin that is killed.
#
# Nothing of it reaches the desktop the script is started from. The second
# KWin has outputs without a screen (kwin_wayland --virtual) and no input
# devices; it, the scaler and the test window have a runtime directory, a
# session bus, a configuration, a state and a cache directory of their own
# below a new directory in /tmp, which stays there with the logs. The
# session bus is a dbus-daemon that cannot start services. The one thing
# shared with the session is PipeWire, through which KWin hands over its
# screen cast: the script names the session's PipeWire socket to the
# programs it starts, and short-lived video streams appear there.
#
# Usage: kwin_virtual_test.sh BUILD_DIR EFFECTS_DIR
# Exit code 77: there is no kwin_wayland, kscreen-doctor or dbus-daemon, or
# no PipeWire session.
set -u
B=${1:?the build directory}
EFFECTS=${2:?the directory of the effects}
SCALE=$B/tools/magpie-scale/magpie-scale
VIEWER=$B/tools/magpie-viewer/magpie-viewer
WINDOW=$B/tests/magpie_test_window

PASSED=0
FAILED=0
check() {
	# check DESCRIPTION ACTUAL EXPECTED
	if [ "$2" = "$3" ]; then
		PASSED=$((PASSED + 1)); echo "  ok    $1"
	else
		FAILED=$((FAILED + 1)); echo "  FAIL  $1: \"$2\", expected \"$3\""
	fi
}

###############################################################################
# Inside the second KWin: one group of checks, named by the first argument
if [ "${MAGPIE_KWIN_TEST_PART:-}" != "" ]; then
	T=$MAGPIE_KWIN_TEST_DIR
	PART=$MAGPIE_KWIN_TEST_PART
	# What the checks print is shown by the script that started this KWin
	exec > "$T/$PART.out" 2>&1
	PIDS=()
	trap 'for p in "${PIDS[@]}"; do kill "$p" 2> /dev/null; done' EXIT

	# The line of the window with this title: SIZE+X+Y of its frame
	frame() { "$VIEWER" --list-windows 2> /dev/null | grep "\"$1\"\$" | awk '{ print $2 }' | head -1; }
	wait_for_window() { local i; for i in $(seq 1 50); do [ -n "$(frame "$1")" ] && break; sleep 0.1; done; }
	# What the scaler says it scales from and to
	described() { grep -oE "Scaling [0-9]+x[0-9]+ on [^ ]+ \([0-9.]+ Hz\) to [^;]+" "$1" | head -1 | sed -E 's/ \([0-9.]+ Hz\)//'; }
	# Runs a KWin script and waits until KWin has carried it out
	kwin_script() {
		local name="magpie-test-$RANDOM"
		echo "$1" > "$T/$name.js"
		busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting loadScript ss "$T/$name.js" "$name" > /dev/null
		busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting start > /dev/null
		sleep 0.4
		busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting unloadScript s "$name" > /dev/null
	}
	scale() {
		# scale LOG ARGUMENT...
		local log=$1
		shift
		"$SCALE" --window source --effects-dir "$EFFECTS" --effect Nearest,type=fit -v "$@" > "$log" 2>&1
	}

	if [ "$PART" = scaling ] || [ "$PART" = scaled-output ]; then
		if [ "$PART" = scaled-output ]; then
			kscreen-doctor output.Virtual-0.scale.1.5 > /dev/null 2>&1
			sleep 1
			echo "--- an output with a scale factor of 150 % ---"
		else
			echo "--- scaling ---"
		fi
		"$WINDOW" --title source > /dev/null 2>&1 &
		PIDS+=("$!")
		wait_for_window source
		BEFORE=$(frame source)
		check "the test window has a title bar" "${BEFORE%%+*}" "640x508"

		scale "$T/$PART-fullscreen.log" --seconds 3 &
		SCALER=$!
		sleep 2
		check "the window is on an output of its size, without its title bar" "$(frame source | cut -d+ -f1)" "640x480"
		wait "$SCALER"
		check "fullscreen scaling ends by itself" "$?" "0"
		# The monitor has 1920x1080 pixels with and without the scale factor
		check "the picture has the monitor's pixels" "$(described "$T/$PART-fullscreen.log")" \
			"Scaling 640x480 on Virtual-Magpie to 1440x1080 on Virtual-0"
		check "the window is back where it was" "$(frame source)" "$BEFORE"
		check "nothing is left to undo" "$(ls "$XDG_STATE_HOME/magpie" 2> /dev/null | grep -c scaling-state)" "0"

		scale "$T/$PART-windowed.log" --windowed --window-scale 1.5 --seconds 3
		check "windowed scaling ends by itself" "$?" "0"
		check "its picture has one and a half times the pixels of the window" \
			"$(described "$T/$PART-windowed.log" | sed -E 's/ in a window.*//')" "Scaling 640x480 on Virtual-Magpie to 960x720"
		check "the window is back where it was" "$(frame source)" "$BEFORE"
	fi

	if [ "$PART" = monitor ]; then
		echo "--- the monitor with the picture is disabled ---"
		"$WINDOW" --title source > /dev/null 2>&1 &
		PIDS+=("$!")
		wait_for_window source
		kwin_script 'const w = workspace.windowList().find(w => w.caption == "source"); workspace.sendClientToScreen(w, workspace.screens.find(s => s.name == "Virtual-1"));'
		BEFORE=$(frame source)
		check "the window is on the second monitor" "$([ "$(echo "$BEFORE" | cut -d+ -f2)" -ge 1920 ] && echo yes)" "yes"
		scale "$T/monitor.log" --seconds 15 &
		SCALER=$!
		sleep 2.5
		kscreen-doctor output.Virtual-1.disable > /dev/null 2>&1
		for i in $(seq 1 50); do kill -0 "$SCALER" 2> /dev/null || break; sleep 0.1; done
		check "scaling ends" "$(kill -0 "$SCALER" 2> /dev/null && echo running || echo ended)" "ended"
		wait "$SCALER"
		check "without an error" "$?" "0"
		check "because the monitor is gone" "$(grep -c "because the monitor with the picture is gone" "$T/monitor.log")" "1"
		AFTER=$(frame source)
		check "the window has its size and title bar" "${AFTER%%+*}" "640x508"
		check "and is on the monitor that is left" "$([ "$(echo "$AFTER" | cut -d+ -f2)" -lt 1920 ] && echo yes)" "yes"

		# KWin keeps an arrangement for every set of outputs, and has kept
		# "disabled" for the set with the virtual output. Enabled again,
		# the monitor is on, but the next virtual output would switch it off
		# once more if the scaler did not put it back.
		kscreen-doctor output.Virtual-1.enable output.Virtual-1.position.1920,0 > /dev/null 2>&1
		sleep 1
		kwin_script 'const w = workspace.windowList().find(w => w.caption == "source"); workspace.sendClientToScreen(w, workspace.screens.find(s => s.name == "Virtual-1"));'
		check "enabled again, the monitor has the window" "$([ "$(frame source | cut -d+ -f2)" -ge 1920 ] && echo yes)" "yes"
		scale "$T/monitor-again.log" --seconds 3
		check "the window is scaled there again" "$? $(described "$T/monitor-again.log")" \
			"0 Scaling 640x480 on Virtual-Magpie to 1440x1080 on Virtual-1"
		check "after the scaler put the monitor back" "$(grep -c "KWin changed the monitors when the virtual output appeared" "$T/monitor-again.log")" "1"
		scale "$T/monitor-third.log" --seconds 3
		check "and the time after that KWin leaves it alone" \
			"$? $(grep -c "KWin changed the monitors" "$T/monitor-third.log") $(described "$T/monitor-third.log")" \
			"0 0 Scaling 640x480 on Virtual-Magpie to 1440x1080 on Virtual-1"
	fi

	if [ "$PART" = kill ]; then
		"$WINDOW" --title source > /dev/null 2>&1 &
		PIDS+=("$!")
		wait_for_window source
		scale "$T/kill.log" --seconds 30 &
		SCALER=$!
		sleep 2.5
		# The KWin this runs in, by its process and not by its name: the
		# desktop has a KWin as well
		kill -9 "$PPID_KWIN"
		for i in $(seq 1 50); do kill -0 "$SCALER" 2> /dev/null || break; sleep 0.1; done
		kill -0 "$SCALER" 2> /dev/null && echo running > "$T/kill.result" || echo ended > "$T/kill.result"
	fi

	echo "$PASSED $FAILED" > "$T/$PART.count"
	exit 0
fi

###############################################################################
for program in kwin_wayland kscreen-doctor dbus-daemon busctl; do
	command -v "$program" > /dev/null || { echo "no program \"$program\"; skipping"; exit 77; }
done
# KWin hands its screen cast over through the session's PipeWire
PIPEWIRE_DIR=${PIPEWIRE_RUNTIME_DIR:-${XDG_RUNTIME_DIR:-}}
[ -n "$PIPEWIRE_DIR" ] && [ -S "$PIPEWIRE_DIR/pipewire-0" ] || { echo "no PipeWire session; skipping"; exit 77; }

T=$(mktemp -d /tmp/magpie-kwin-test.XXXXXX) || exit 1
echo "logs: $T"
mkdir -p "$T/config" "$T/cache" "$T/state"

PIDS=()
cleanup() {
	for p in "${PIDS[@]}"; do kill "$p" 2> /dev/null; done
	wait 2> /dev/null
}
trap cleanup EXIT

export PIPEWIRE_RUNTIME_DIR=$PIPEWIRE_DIR
export XDG_RUNTIME_DIR=$T XDG_CONFIG_HOME=$T/config XDG_CACHE_HOME=$T/cache XDG_STATE_HOME=$T/state
export MAGPIE_CONFIG_DIR=$T/magpie
unset WAYLAND_DISPLAY DISPLAY SWAYSOCK

# A session bus without the directories of services, so that nothing can be
# started through it
cat > "$T/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=$T/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
dbus-daemon --nofork --config-file="$T/bus.conf" > "$T/bus.log" 2>&1 &
PIDS+=("$!")
for i in $(seq 1 50); do [ -S "$T/bus" ] && break; sleep 0.1; done
export DBUS_SESSION_BUS_ADDRESS=unix:path=$T/bus

SELF=$(readlink -f "$0")
# run_part NAME OUTPUT_COUNT: a KWin of its own for the part, which ends
# with it
run_part() {
	local part=$1 outputs=$2 socket="magpie-test-$1"
	# The part learns the KWin's process through its parent. In the
	# background, so that the shell says nothing when the part kills it.
	timeout 90 kwin_wayland --virtual --no-lockscreen --no-global-shortcuts --no-kactivities \
		--socket "$socket" --output-count "$outputs" --width 1920 --height 1080 \
		--exit-with-session "env WAYLAND_DISPLAY=$socket MAGPIE_KWIN_TEST_PART=$part MAGPIE_KWIN_TEST_DIR=$T sh -c 'PPID_KWIN=\$PPID exec bash $SELF $B $EFFECTS'" \
		> "$T/kwin-$part.log" 2>&1 &
	wait $! 2> /dev/null
	# A part that outlives its KWin, which it has killed, needs a moment
	local i
	for i in $(seq 1 80); do [ -f "$T/$part.count" ] && break; sleep 0.1; done
	cat "$T/$part.out" 2> /dev/null
	if [ -f "$T/$part.count" ]; then
		read -r passed failed < "$T/$part.count"
		PASSED=$((PASSED + passed))
		FAILED=$((FAILED + failed))
	fi
}

run_part scaling 1
if [ ! -f "$T/scaling.count" ]; then
	# Not a fault of Magpie: for example a machine on which KWin cannot run
	# without a screen
	echo "The second KWin did not run the checks, so there is nothing to test with; skipping. Its log:"
	tail -5 "$T/kwin-scaling.log"
	exit 77
fi
run_part scaled-output 1
run_part monitor 2

echo "--- KWin is killed while a window is scaled ---"
run_part kill 1
check "the scaler ends when KWin is gone" "$(cat "$T/kill.result" 2> /dev/null)" "ended"
check "it says why" "$(grep -c "connection of the KWin back end is lost" "$T/kill.log" | sed 's/^[1-9][0-9]*$/yes/')" "yes"

echo
echo "$PASSED passed, $FAILED failed"
[ "$FAILED" = 0 ]
