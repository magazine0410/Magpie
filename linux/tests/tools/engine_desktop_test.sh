#!/usr/bin/env bash
# The engine service driven by magpie-ctl, on a KDE Plasma desktop: start and
# stop, a source that is minimized and shown again, a source that changes its
# size, another window on the same monitor becoming active, the requests to
# keep the screen on and to hold back notifications, and recovery after the
# service was killed.
#
# It opens windows that take keyboard focus, but sends no input and does not
# move the pointer. No shortcut is registered and the speed of the pointer
# devices is not touched. It takes about a minute and a half.
#
# It uses the test window and the helper programs of the Phase 0 feasibility
# tests, which have to be built first:
#   cmake -S linux/spikes -B linux/spikes/build -G Ninja && cmake --build linux/spikes/build
#
# Usage: engine_desktop_test.sh
# Environment: OUT, the directory for logs (default: a new one in /tmp)
set -u
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
B=$REPO/linux/build
SB=$REPO/linux/spikes/build
OUT=${OUT:-$(mktemp -d /tmp/magpie-engine-test.XXXXXX)}
mkdir -p "$OUT"
MONITOR=DP-6
CTL=$B/tools/magpie-ctl/magpie-ctl
echo "logs: $OUT"

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
start_engine() {
	"$B/tools/magpie-engine/magpie-engine" --no-shortcuts --no-cursor-speed -v >> "$OUT/engine.log" 2>&1 &
	ENGINE_PID=$!
	PIDS+=("$ENGINE_PID")
	sleep 1
}
status() { "$CTL" status; }

: > "$OUT/engine.log"
"$SB/t8_testsource/testsource" --duration 150 --no-focus --title "Magpie other window" > "$OUT/other.log" 2>&1 &
PIDS+=("$!")
OTHER_PID=$!
"$SB/t8_testsource/testsource" --duration 150 --resizable > "$OUT/source.log" 2>&1 &
SOURCE_PID=$!
PIDS+=("$SOURCE_PID")
sleep 1.5
OTHER=$(windows | grep "pid=$OTHER_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
UUID=$(windows | grep "pid=$SOURCE_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
"$SB/kwin_windows/kwin_windows" --send-to-output "$OTHER" "$MONITOR" > /dev/null
"$SB/kwin_windows/kwin_windows" --send-to-output "$UUID" "$MONITOR" > /dev/null
sleep 0.8
BEFORE=$(window_line "$UUID")
echo "source before: $BEFORE"

start_engine
echo "status at start: $(status)"

echo "--- scale by id, stop with the shortcut's action ---"
"$CTL" scale "$UUID"; sleep 1.5
echo "status: $(status)   window: $(window_line "$UUID")"
# The active window is the source, so the toggle stops
"$CTL" scale; sleep 1
AFTER=$(window_line "$UUID")
echo "after the toggle: $(status)   same place and size: $([ "${BEFORE% state=*}" = "${AFTER% state=*}" ] && echo yes || echo "NO: $AFTER")"

echo "--- the toggle scales the active window ---"
kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$UUID\") { workspace.activeWindow = w; } }"
"$CTL" scale; sleep 1.5
echo "status: $(status)   window: $(window_line "$UUID")"

echo "--- the source is minimized and shown again ---"
"$SB/kwin_windows/kwin_windows" --set-state "$UUID" 2 2 > /dev/null; sleep 1
echo "minimized: $(status)   window: $(window_line "$UUID")"
"$SB/kwin_windows/kwin_windows" --set-state "$UUID" 2 0 > /dev/null; sleep 2
echo "shown again: $(status)   window: $(window_line "$UUID")"

echo "--- the source changes its size ---"
kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$UUID\") { const g = w.frameGeometry; w.frameGeometry = { x: g.x, y: g.y, width: 800, height: 600 }; } }"
sleep 2.5
echo "resized: $(status)   window: $(window_line "$UUID")"
grep -E "Scaling [0-9]+x[0-9]+ on" "$OUT/engine.log" | tail -1 | sed -E 's/^[^|]*\|[^|]*\|[^|]*\|[^|]*\|//' | cut -c1-120
"$CTL" stop; sleep 1
echo "stopped: $(status)   window: $(window_line "$UUID")"
# Back to the first size for the rest of the test
kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$UUID\") { const g = w.frameGeometry; w.frameGeometry = { x: g.x, y: g.y, width: 640, height: 508 }; } }"
sleep 0.5
BEFORE=$(window_line "$UUID")

echo "--- another window on the same monitor becomes active ---"
"$CTL" scale "$UUID"; sleep 1.5
echo "status: $(status)"
kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$OTHER\") { workspace.activeWindow = w; } }"
sleep 1
AFTER=$(window_line "$UUID")
echo "after the other window was activated: $(status)   same place and size: $([ "${BEFORE% state=*}" = "${AFTER% state=*}" ] && echo yes || echo "NO: $AFTER")"

echo "--- the service is killed while scaling ---"
"$CTL" scale "$UUID"; sleep 1.5
echo "status: $(status)"
kill -9 "$ENGINE_PID"; wait "$ENGINE_PID" 2>/dev/null; sleep 1
echo "after the kill: $(window_line "$UUID")   state file: $(ls ~/.local/state/magpie/ | tr '\n' ' ')"
start_engine
echo "after the restart: $(status)   window: $(window_line "$UUID")   state file: $(ls ~/.local/state/magpie/ | tr '\n' ' ')"
kill -TERM "$ENGINE_PID"; wait "$ENGINE_PID"; echo "service ended with status $?"

echo "--- keep screen on and hold back notifications ---"
inhibited() {
	echo "screen kept on: $(busctl --user call org.kde.Solid.PowerManagement.PolicyAgent /org/kde/Solid/PowerManagement/PolicyAgent org.kde.Solid.PowerManagement.PolicyAgent ListInhibitions | grep -c 'A window is scaled')  notifications held back: $(busctl --user get-property org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications Inhibited | awk '{print $2}')"
}
echo "before: $(inhibited)"
"$B/tools/magpie-scale/magpie-scale" --window "$UUID" --seconds 8 --keep-screen-on --quiet > "$OUT/scale_inhibit.log" 2>&1 &
SCALE_PID=$!
# Plasma lists a request to keep the screen on only after about five seconds
sleep 6.5
echo "while scaling: $(inhibited)"
wait "$SCALE_PID"
sleep 0.5
echo "after: $(inhibited)"

echo
echo "--- messages of the service ---"
grep -E "\|(warning|error)\|" "$OUT/engine.log" | sed -E 's/^[^|]*\|([a-z]+)\|[^|]*\|[^|]*\|/[\1] /' | cut -c1-200 | sort | uniq -c | head -12
grep -E "State: " "$OUT/engine.log" | sed -E 's/.*State: //' | tr '\n' ' '
echo
