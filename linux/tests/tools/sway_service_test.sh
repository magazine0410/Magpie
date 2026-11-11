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

# A machine without a graphics card, such as a container: Sway composes in
# software
ls /dev/dri/renderD* > /dev/null 2>&1 || export WLR_RENDERER=${WLR_RENDERER:-pixman}

echo "xwayland disable" > "$T/sway.conf"
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 sway -c "$T/sway.conf" > "$T/sway.log" 2>&1 &
SWAY_PID=$!
PIDS+=("$SWAY_PID")
for i in $(seq 1 100); do
	[ -S "$SWAYSOCK" ] && [ -S "$T/bus" ] && ls "$T"/wayland-* > /dev/null 2>&1 && break
	sleep 0.1
done
WAYLAND_DISPLAY=$(basename "$(ls "$T"/wayland-* | grep -v '\.lock$' | head -1)")
export WAYLAND_DISPLAY
if ! swaymsg -q -- output HEADLESS-1 mode --custom 1920x1080@120Hz position 0 0; then
	# Not a fault of Magpie: for example a container that may not start
	# Sway, whose file has a capability
	echo "Sway did not start, so there is nothing to test with; skipping. Its log:"
	head -5 "$T/sway.log"
	exit 77
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
# ENGINE_ENV: put in front of the command, for example "env -u SWAYSOCK"
start_engine() {
	${ENGINE_ENV:-} "$ENGINE" --effects-dir "$EFFECTS" --no-cursor-speed -v "$@" >> "$T/engine.log" 2>&1 &
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

echo "--- a nested session ---"
# The window of the picture, as SIZE; empty if there is none
picture() { windows | grep " $1 " | head -1 | cut -d' ' -f5; }
# No profile that scales the test window by itself any more, which would
# happen whenever a nested session ends and the window is active again; and
# a profile that says what a nested session offers its program
python3 - "$MAGPIE_CONFIG_DIR/v5/config.json" "$(readlink -f "$WINDOW")" <<'PY'
import json, sys
path, program = sys.argv[1:3]
config = json.load(open(path))
config["profiles"] = [
    config["profiles"][0],
    {"name": "Nested test", "packaged": False, "pathRule": program, "classNameRule": "magpie.test",
     "autoScale": 0, "scalingMode": 0, "adjustCursorSpeed": False,
     "launchNested": True, "nestedWidth": 800, "nestedHeight": 600, "allowTearing": True}
]
json.dump(config, open(path, "w"), indent=2)
PY
: > "$T/engine.log"
start_engine
"$CTL" launch --size 640x480 -- "$WINDOW" --title nested-game --resizable
check "launch a program in a nested session" "$(wait_status scaling 10)" "scaling"
check "what is scaled is a nested session" "$("$CTL" nested)" "yes"
check "its picture waits for the monitor's refresh" "$(grep -c "may tear" "$T/engine.log")" "0"
NESTED_SWAY=$(pgrep -x -P "$ENGINE_PID" sway)
check "the service has started a Sway of its own" "$(echo "$NESTED_SWAY" | wc -w)" "1"
for i in $(seq 1 30); do [ -n "$(picture nested-game)" ] && break; sleep 0.1; done
check "the picture covers the monitor and has the program's title" "$(picture nested-game)" "1920x1080"
check "the program's window is not one of the desktop" "$(windows | grep -c ' nested-game ')" "1"
"$CTL" window
sleep 1
check "the windowed command shows it in a window" "$(wait_status scaling 5) $([ "$(picture nested-game)" != 1920x1080 ] && echo window)" "scaling window"
"$CTL" scale
sleep 1
check "the fullscreen command shows it fullscreen again" "$(picture nested-game)" "1920x1080"
"$CTL" scale source
sleep 0.5
check "a window is not scaled in its place" "$(status) $("$CTL" nested)" "scaling yes"
"$CTL" stop
check "stop ends it" "$(wait_status idle 5) $("$CTL" nested)" "idle no"
sleep 0.5
check "its Sway has ended" "$(kill -0 "$NESTED_SWAY" 2> /dev/null && echo running || echo ended)" "ended"
check "the picture is gone" "$(picture nested-game)" ""

"$CTL" launch --tearing --size 640x480 -- "$WINDOW" --title nested-game --resizable
check "launched with tearing allowed" "$(wait_status scaling 10) $(grep -c "may tear" "$T/engine.log")" "scaling 1"
"$CTL" window
sleep 1
check "in a window the picture does not tear" "$(wait_status scaling 5) $(grep -c "may tear" "$T/engine.log")" "scaling 1"
"$CTL" scale
sleep 1
check "fullscreen again it may" "$(wait_status scaling 5) $(grep -c "may tear" "$T/engine.log")" "scaling 2"
"$CTL" stop
check "stop" "$(wait_status idle 5)" "idle"

"$CTL" launch --windowed --size 400x300 -- "$WINDOW" --title short-lived --duration 1.5
check "a program that ends by itself" "$(wait_status scaling 10)" "scaling"
check "the session ends a few seconds after it" "$(wait_status idle 10)" "idle"

"$CTL" launch --profile "Nested test" -- "$WINDOW" --title nested-game --resizable
check "launched with a profile" "$(wait_status scaling 10)" "scaling"
check "the session has the size of the profile" "$(grep -c "Nested session started: 800x600" "$T/engine.log")" "1"
check "and may tear as the profile says" "$(grep -c "may tear" "$T/engine.log")" "3"
NESTED_SWAY=$(pgrep -x -P "$ENGINE_PID" sway)
check "still running after SIGTERM" "$(stop_engine TERM)" "no"
sleep 0.5
check "its Sway has ended with the service" "$(kill -0 "$NESTED_SWAY" 2> /dev/null && echo running || echo ended)" "ended"

echo "--- a desktop without a back end ---"
# Without the variable that names Sway's socket the service finds neither
# Sway nor KWin
: > "$T/engine.log"
ENGINE_ENV="env -u SWAYSOCK" start_engine
check "the service runs" "$(status)" "idle"
check "it says that the desktop has no back end" "$(grep -c "has no back end" "$T/engine.log")" "1"
"$CTL" scale source
sleep 0.5
check "a window cannot be scaled" "$(status)" "idle"
"$CTL" launch --size 640x480 -- "$WINDOW" --title nested-game --resizable
check "a program in a nested session can" "$(wait_status scaling 10)" "scaling"
for i in $(seq 1 30); do [ -n "$(picture nested-game)" ] && break; sleep 0.1; done
check "its picture covers the monitor" "$(picture nested-game)" "1920x1080"
"$CTL" stop
check "stop" "$(wait_status idle 5)" "idle"
check "still running after SIGTERM" "$(stop_engine TERM)" "no"

echo "--- what is scaled goes away under the service ---"
: > "$T/engine.log"
start_engine
"$WINDOW" --title victim --app-id magpie.test.victim > /dev/null 2>&1 &
VICTIM=$!
PIDS+=("$VICTIM")
for i in $(seq 1 30); do [ -n "$(window_line victim)" ] && break; sleep 0.1; done
"$CTL" scale victim
check "a window is scaled" "$(wait_status scaling 5)" "scaling"
sleep 0.3
kill -9 "$VICTIM"
check "its program is killed, and scaling ends" "$(wait_status idle 5)" "idle"
sleep 0.3
check "the output made for it is removed" "$(output_names)" "HEADLESS-1"
"$CTL" scale source
check "another window is scaled" "$(wait_status scaling 5)" "scaling"
sleep 0.3
# The compositor itself: as when it crashes or is restarted
kill -9 "$SWAY_PID"
for i in $(seq 1 50); do kill -0 "$ENGINE_PID" 2> /dev/null || break; sleep 0.1; done
check "Sway is killed, and the service ends" "$(kill -0 "$ENGINE_PID" 2> /dev/null && echo running || echo ended)" "ended"
check "it says why" "$(grep -c "connection to the desktop is lost" "$T/engine.log")" "1"

echo
echo "warnings and errors of the service:"
grep -hE "\|(warning|error|critical)\|" "$T/engine.log" | sed -E 's/^([^|]*\|[^|]*)\|[^|]*\|[^|]*\|/\1|/' | cut -c1-200 | sort | uniq -c | head -20
echo
echo "$PASSED passed, $FAILED failed"
[ "$FAILED" = 0 ]
