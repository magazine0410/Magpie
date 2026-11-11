#!/usr/bin/env bash
# The scaling service on Sway, from the outside: magpie-engine is started in
# a Sway that this script starts itself, and driven with magpie-ctl.
#
# Nothing of it reaches the desktop the script is started from. The Sway
# shows nothing (its outputs have no screen) and uses no input device; it,
# the service and the test windows have a runtime directory, a session bus,
# a configuration, a state and a cache directory of their own below a new
# directory in /tmp, which stays there with the logs. The session bus is a
# dbus-daemon that cannot start services, so no portal or other desktop
# service is started a second time.
#
# Usage: sway_service_test.sh BUILD_DIR EFFECTS_DIR
# Exit code 77: there is no program "sway" (or dbus-daemon, or python3).
set -u
B=${1:?the build directory}
EFFECTS=${2:?the directory of the effects}
ENGINE=$B/tools/magpie-engine/magpie-engine
CTL=$B/tools/magpie-ctl/magpie-ctl
WINDOW=$B/tests/magpie_test_window

for program in sway swaymsg dbus-daemon python3; do
	command -v "$program" > /dev/null || { echo "no program \"$program\"; skipping"; exit 77; }
done

T=$(mktemp -d /tmp/magpie-sway-service.XXXXXX) || exit 1
echo "logs: $T"

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

PIDS=()
cleanup() {
	for p in "${PIDS[@]}"; do kill "$p" 2> /dev/null; done
	wait 2> /dev/null
}
trap cleanup EXIT

# Everything below runs with these
export XDG_RUNTIME_DIR=$T XDG_STATE_HOME=$T/state XDG_CACHE_HOME=$T/cache MAGPIE_CONFIG_DIR=$T/config
export SWAYSOCK=$T/sway.sock
unset WAYLAND_DISPLAY DISPLAY

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
export DBUS_SESSION_BUS_ADDRESS=unix:path=$T/bus

echo "xwayland disable" > "$T/sway.conf"
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 sway -c "$T/sway.conf" > "$T/sway.log" 2>&1 &
PIDS+=("$!")
for i in $(seq 1 100); do
	[ -S "$SWAYSOCK" ] && [ -S "$T/bus" ] && ls "$T"/wayland-* > /dev/null 2>&1 && break
	sleep 0.1
done
WAYLAND_DISPLAY=$(basename "$(ls "$T"/wayland-* | grep -v '\.lock$' | head -1)")
export WAYLAND_DISPLAY
if ! swaymsg -q -- output HEADLESS-1 mode --custom 1920x1080@120Hz position 0 0; then
	echo "Sway did not start; see $T/sway.log"
	FAILED=1
	exit 1
fi

# One line per window: OUTPUT TITLE X Y WIDTHxHEIGHT BORDER [focused]
windows() {
	swaymsg -t get_tree -r | python3 -c '
import json, sys
def walk(node, output=""):
    if node["type"] == "output":
        output = node["name"]
    if node.get("pid"):
        r = node["rect"]
        print(output, node.get("name"), r["x"], r["y"], "%dx%d" % (r["width"], r["height"]), node["border"],
              "focused" if node["focused"] else "")
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        walk(child, output)
walk(json.load(sys.stdin))'
}
window_line() { windows | grep " $1 " | head -1 | sed 's/ focused$//; s/ $//'; }
output_names() { swaymsg -t get_outputs -r | python3 -c 'import json, sys; print(" ".join(sorted(o["name"] for o in json.load(sys.stdin))))'; }
# The output the service made, as NAME WIDTHxHEIGHT; Sway numbers such
# outputs on and on, so the name is another one every time
virtual_output() {
	swaymsg -t get_outputs -r | python3 -c '
import json, sys
print(*["%s %dx%d" % (o["name"], o["rect"]["width"], o["rect"]["height"])
        for o in json.load(sys.stdin) if o["name"] != "HEADLESS-1"])'
}
status() { "$CTL" status 2>&1; }
# Waits until the status is $1, at most $2 seconds; prints the status
wait_status() {
	local i
	for i in $(seq 1 $(($2 * 10))); do
		[ "$(status)" = "$1" ] && break
		sleep 0.1
	done
	status
}
start_engine() {
	"$ENGINE" --effects-dir "$EFFECTS" --no-cursor-speed -v "$@" >> "$T/engine.log" 2>&1 &
	ENGINE_PID=$!
	PIDS+=("$ENGINE_PID")
	wait_status idle 10 > /dev/null
}
stop_engine() {
	kill "-$1" "$ENGINE_PID"
	local i
	for i in $(seq 1 50); do
		kill -0 "$ENGINE_PID" 2> /dev/null || break
		sleep 0.1
	done
	kill -0 "$ENGINE_PID" 2> /dev/null && echo yes || echo no
}
focus() { swaymsg -q "[title=\"^$1\$\"] focus"; }

"$WINDOW" --title source --app-id magpie.test.source > "$T/source.log" 2>&1 &
PIDS+=("$!")
"$WINDOW" --title other --app-id magpie.test.other --size 300x200 > "$T/other.log" 2>&1 &
PIDS+=("$!")
for i in $(seq 1 50); do
	[ "$(windows | wc -l)" = 2 ] && break
	sleep 0.1
done
sleep 0.3
focus other
BEFORE=$(window_line source)
echo "source: $BEFORE"
check "the test windows are there" "$(windows | wc -l)" "2"

echo "--- start ---"
: > "$T/engine.log"
start_engine
check "the service answers" "$(status)" "idle"
check "it has found Sway" "$(grep -c "The desktop is Sway" "$T/engine.log")" "1"
check "it has bound its four shortcuts" "$(grep -c '^Shortcut ' "$T/engine.log")" "4"
check "a second service is refused" \
	"$("$ENGINE" --effects-dir "$EFFECTS" 2>&1 | grep -c "seems to run already")" "1"

echo "--- fullscreen scaling ---"
"$CTL" scale source
check "scale by title" "$(wait_status scaling 5)" "scaling"
sleep 0.5
VIRTUAL=$(virtual_output)
check "there is an output of the size of the source" "${VIRTUAL#* }" "640x480"
check "the source is on it, without its border" "$(window_line source | cut -d' ' -f1,5,6)" "${VIRTUAL% *} 640x480 none"
"$CTL" stop
check "stop" "$(wait_status idle 5)" "idle"
sleep 0.3
check "the source has its place and its border again" "$(window_line source)" "$BEFORE"
check "the output is gone" "$(output_names)" "HEADLESS-1"

echo "--- windowed scaling ---"
"$CTL" window source
check "scale into a window" "$(wait_status scaling 5)" "scaling"
sleep 0.8
VIRTUAL=$(virtual_output)
check "the source is on an output with a strip around it" "${VIRTUAL#* }" "642x482"
check "the picture is in a window on the monitor" "$(windows | grep -c '^HEADLESS-1 source ')" "1"
"$CTL" scale
check "the fullscreen command changes the way of scaling" "$(wait_status scaling 5) $(sleep 0.8; windows | grep -c '^HEADLESS-1 source ')" "scaling 0"
"$CTL" stop
check "stop" "$(wait_status idle 5)" "idle"
sleep 0.3
check "the source has its place again" "$(window_line source)" "$BEFORE"

echo "--- the service ends while a window is scaled ---"
"$CTL" scale source
check "scaled" "$(wait_status scaling 5)" "scaling"
check "still running after SIGTERM" "$(stop_engine TERM)" "no"
sleep 0.3
check "the source has its place again" "$(window_line source)" "$BEFORE"
check "no output is left" "$(output_names)" "HEADLESS-1"

echo "--- the service is killed while a window is scaled ---"
start_engine
"$CTL" scale source
check "scaled" "$(wait_status scaling 5)" "scaling"
sleep 0.3
check "still running after SIGKILL" "$(stop_engine KILL)" "no"
sleep 0.3
VIRTUAL=$(virtual_output)
check "the output is still there" "${VIRTUAL#* }" "640x480"
check "the source is stranded on it" "$(window_line source | cut -d' ' -f1)" "${VIRTUAL% *}"
start_engine
sleep 0.3
check "the next start puts it back" "$(window_line source)" "$BEFORE"
check "and removes the output" "$(output_names)" "HEADLESS-1"
check "still running after SIGTERM" "$(stop_engine TERM)" "no"

echo "--- auto-scale by a profile ---"
# The first start has written no config.json; this one has a profile for
# the source that scales it when it becomes active
mkdir -p "$MAGPIE_CONFIG_DIR/v5"
python3 - "$MAGPIE_CONFIG_DIR/v5/config.json" "$(readlink -f "$WINDOW")" <<'PY'
import json, sys
path, program = sys.argv[1:3]
config = {
    "scalingModes": [{"name": "Nearest", "effects": [{"name": "Nearest", "scalingType": 1}]}],
    "profiles": [
        {"name": "", "scalingMode": 0, "adjustCursorSpeed": False},
        {"name": "Test source", "packaged": False, "pathRule": program, "classNameRule": "magpie.test.source",
         "autoScale": 1, "scalingMode": 0, "adjustCursorSpeed": False}
    ]
}
json.dump(config, open(path, "w"), indent=2)
PY
focus other
start_engine --config "$MAGPIE_CONFIG_DIR/v5/config.json"
check "idle while another window is active" "$(status)" "idle"
focus source
check "scaled when the source becomes active" "$(wait_status scaling 5)" "scaling"
"$CTL" stop
check "stop" "$(wait_status idle 5)" "idle"
sleep 0.5
check "not scaled again at once after the stop" "$(status)" "idle"
focus other; sleep 0.3
focus source
check "scaled again after another window was active" "$(wait_status scaling 5)" "scaling"
check "still running after SIGTERM" "$(stop_engine TERM)" "no"
sleep 0.3
check "the source has its place again" "$(window_line source | cut -d' ' -f1,3-6)" "$(echo "$BEFORE" | cut -d' ' -f1,3-6)"

echo
echo "warnings and errors of the service:"
grep -hE "\|(warning|error|critical)\|" "$T/engine.log" | sed -E 's/^([^|]*\|[^|]*)\|[^|]*\|[^|]*\|/\1|/' | cut -c1-200 | sort | uniq -c | head -20
echo
echo "$PASSED passed, $FAILED failed"
[ "$FAILED" = 0 ]
