#!/usr/bin/env bash
# The settings application on a KDE Plasma desktop: start into the tray, a
# second start that shows the window, closing the window, scaling through the
# service inside the application, auto-scale by a profile, and a clean end.
#
# What it does to the desktop: Magpie's tray icon appears and goes away
# three times, its main window is shown once for a moment, and two test
# windows are opened on $MONITOR and made the active window in turns. It
# sends no input and does not move the pointer. The global shortcuts are
# registered (the desktop asks about them only the first time), the speed of
# the pointer devices is not touched. It takes about a minute.
#
# The application runs with a configuration directory of its own below $OUT,
# so the user's config.json is neither read nor written.
#
# It uses the test window and the helper programs of the Phase 0 feasibility
# tests, which have to be built first:
#   cmake -S linux/spikes -B linux/spikes/build -G Ninja && cmake --build linux/spikes/build
#
# Usage: app_desktop_test.sh
# Environment: OUT, the directory for logs (default: a new one in /tmp)
set -u
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
B=$REPO/linux/build
SB=$REPO/linux/spikes/build
OUT=${OUT:-$(mktemp -d /tmp/magpie-app-test.XXXXXX)}
mkdir -p "$OUT"
MONITOR=DP-6
CTL=$B/tools/magpie-ctl/magpie-ctl
export MAGPIE_CONFIG_DIR=$OUT/config
CONFIG=$MAGPIE_CONFIG_DIR/v5/config.json
echo "logs: $OUT"

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
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

windows() { "$SB/kwin_windows/kwin_windows"; }
window_line() { windows | grep "$1" | sed -E 's/.*(frame=[^ ]+).*(client=[^ ]+) (state=[^ ]+).*/\1 \2 \3/'; }
kwin_script() {
	local name="magpie-test-$RANDOM"
	echo "$1" > "$OUT/script.js"
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting loadScript ss "$OUT/script.js" "$name" > /dev/null
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting start > /dev/null
	sleep 0.4
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting unloadScript s "$name" > /dev/null
}
activate() {
	kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$1\") { workspace.activeWindow = w; } }"
}
status() { "$CTL" status 2>&1; }
# Waits until the status is $1, at most $2 seconds; prints the status
wait_status() {
	local i
	for i in $(seq 1 $(($2 * 5))); do
		[ "$(status)" = "$1" ] && break
		sleep 0.2
	done
	status
}
start_app() {
	"$B/app/magpie" -t >> "$OUT/app.log" 2>&1 &
	APP_PID=$!
	PIDS+=("$APP_PID")
	wait_status idle 10 > /dev/null
}
stop_app() {
	kill -TERM "$APP_PID"
	local i
	for i in $(seq 1 50); do
		kill -0 "$APP_PID" 2>/dev/null || break
		sleep 0.1
	done
	kill -0 "$APP_PID" 2>/dev/null && echo yes || echo no
}
app_windows() { windows | grep -c "pid=$APP_PID .*title=\"Magpie\""; }

: > "$OUT/app.log"
# The application logs to a file of its own, which may have earlier lines
LOG=${XDG_STATE_HOME:-$HOME/.local/state}/magpie/logs/magpie.log
LOG_START=$([ -f "$LOG" ] && wc -l < "$LOG" || echo 0)
# The other window is a program of its own, so that the profile for the test
# source does not apply to it: a profile recognises a window by its program
cp "$SB/t8_testsource/testsource" "$OUT/otherwindow"
"$OUT/otherwindow" --duration 120 --no-focus --title "Magpie other window" > "$OUT/other.log" 2>&1 &
OTHER_PID=$!
PIDS+=("$OTHER_PID")
"$SB/t8_testsource/testsource" --duration 120 --title "Magpie test source" > "$OUT/source.log" 2>&1 &
SOURCE_PID=$!
PIDS+=("$SOURCE_PID")
sleep 1.5
OTHER=$(windows | grep "pid=$OTHER_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
UUID=$(windows | grep "pid=$SOURCE_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
APP_ID=$(windows | grep "pid=$SOURCE_PID " | sed -E 's/.*app_id="([^"]*)".*/\1/' | head -1)
"$SB/kwin_windows/kwin_windows" --send-to-output "$OTHER" "$MONITOR" > /dev/null
"$SB/kwin_windows/kwin_windows" --send-to-output "$UUID" "$MONITOR" > /dev/null
sleep 0.8
activate "$OTHER"
BEFORE=$(window_line "$UUID")
echo "source: app ID \"$APP_ID\", $BEFORE"

echo "--- start into the tray ---"
start_app
check "the service answers" "$(status)" "idle"
check "no main window with -t" "$(app_windows)" "0"
check "the first start wrote config.json" "$([ -s "$CONFIG" ] && echo yes || echo no)" "yes"

echo "--- a second start shows the window of the first ---"
SECOND=$("$B/app/magpie" 2>&1); SECOND_EXIT=$?
sleep 1.5
check "the second start ends" "$SECOND_EXIT: $SECOND" "0: Magpie is running already."
check "the main window is shown" "$(app_windows)" "1"
MAIN=$(windows | grep "pid=$APP_PID .*title=\"Magpie\"" | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$MAIN\") { w.closeWindow(); } }"
sleep 1
check "closing the window leaves the program in the tray" "$(app_windows) $(kill -0 "$APP_PID" 2>/dev/null && echo running)" "0 running"
check "the service still answers" "$(status)" "idle"

echo "--- the program ends on SIGTERM ---"
check "still running after SIGTERM" "$(stop_app)" "no"
check "the name on D-Bus is free" "$(status)" "Magpie is not running."

# A profile for the test source that scales it when it becomes active. No
# profile changes the speed of the pointer devices.
python3 - "$CONFIG" "$APP_ID" "$(readlink -f "$SB/t8_testsource/testsource")" <<'PY'
import json, sys
path, app_id, program = sys.argv[1:4]
config = json.load(open(path))
config["profiles"][0]["adjustCursorSpeed"] = False
config["profiles"].append({
    "name": "Test source", "packaged": False, "pathRule": program, "classNameRule": app_id,
    "autoScale": 1, "scalingMode": 0, "adjustCursorSpeed": False
})
json.dump(config, open(path, "w"), indent=2)
PY
# Closing Magpie's window made some window the active one; were it the test
# source, it would be scaled as soon as the program starts
activate "$OTHER"; sleep 0.5
start_app

echo "--- scaling through the service of the application ---"
check "idle while another window is active" "$(status)" "idle"
"$CTL" scale "$UUID"
check "scale by id" "$(wait_status scaling 5)" "scaling"
"$CTL" stop
check "stop" "$(wait_status idle 5)" "idle"
sleep 1.5
check "the source has its place again" "$(window_line "$UUID" | sed 's/ state=.*//')" "${BEFORE% state=*}"

echo "--- auto-scale by the profile ---"
check "not scaled again at once after the stop" "$(status)" "idle"
activate "$OTHER"; sleep 0.6
activate "$UUID"
check "scaled when it becomes active again" "$(wait_status scaling 5)" "scaling"
activate "$OTHER"
check "scaling ends when the other window becomes active" "$(wait_status idle 5)" "idle"
sleep 1
check "and stays ended" "$(status)" "idle"

echo "--- the program ends while a window is scaled ---"
activate "$UUID"
check "scaled" "$(wait_status scaling 5)" "scaling"
check "still running after SIGTERM" "$(stop_app)" "no"
sleep 0.8
check "the source has its place again" "$(window_line "$UUID" | sed 's/ state=.*//')" "${BEFORE% state=*}"
check "no virtual output is left" "$(windows | head -1 | grep -c Virtual)" "0"

echo
echo "what the application printed:"
cut -c1-200 "$OUT/app.log" | head -20
echo "its log (warnings and errors):"
tail -n +$((LOG_START + 1)) "$LOG" 2>/dev/null | grep -E "\|(warning|error|critical)\|" | sed -E 's/^([^|]*\|[^|]*)\|[^|]*\|[^|]*\|/\1|/' | cut -c1-220 | head -20
echo
echo "$PASSED passed, $FAILED failed"
[ "$FAILED" = 0 ]
