#!/usr/bin/env bash
# The engine features beyond plain fullscreen scaling, on a KDE Plasma
# desktop, with the test window on the reference machine's monitor DP-6
# (1920x1080 at 3840,0 with DP-5 on its left and HDMI-A-2 left of that):
#
#   options      title bar kept, maximized and fullscreen sources, recovery of
#                a maximized source after a kill, screenshots, benchmark mode,
#                hiding a resting cursor
#   full         fullscreen scaling on one monitor
#   windowed     scaling into a window: place, pointer, resizing, closing
#   3d           3D game mode: the pointer stays with the source
#   all          one picture over all monitors
#   intersected  one picture over the monitors the window touches
#   toolbar      toolbar and profiler: buttons, screenshot, stop
#   service      magpie-engine: windowed scaling, the toolbar's mode and
#                minimize buttons, the toolbar command, the D-Bus properties
#   focus        coming back through KWin's edge barrier; another window
#                becoming active during windowed scaling
#
# It moves the pointer and clicks on the toolbar and into its own test
# windows, so nobody should use the machine while it runs (about six minutes
# for all parts). "all" and "intersected" cover several monitors for a few
# seconds. It sends no keys and writes outside OUT only the state file of the
# scaler, which is removed again.
#
# It uses the test window and the helper programs of the Phase 0 feasibility
# tests, which have to be built first:
#   cmake -S linux/spikes -B linux/spikes/build -G Ninja && cmake --build linux/spikes/build
#
# Usage: features_desktop_test.sh [PART...]      (default: all parts)
# Environment: OUT, the directory for logs and pictures (default: a new one in /tmp)
set -u
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
PARTS=("$@")
[ ${#PARTS[@]} -eq 0 ] && PARTS=(options full windowed 3d all intersected toolbar service focus)
B=$REPO/linux/build
SB=$REPO/linux/spikes/build
OUT=${OUT:-$(mktemp -d /tmp/magpie-features-test.XXXXXX)}
mkdir -p "$OUT/shots"
echo "logs: $OUT"
MONITOR=DP-6
SCALE=$B/tools/magpie-scale/magpie-scale
CTL=$B/tools/magpie-ctl/magpie-ctl
APP_ID=io.github.magazine0410.Magpie

PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

windows() { "$SB/kwin_windows/kwin_windows"; }
pointer() { "$SB/kwin_pointer/kwin_pointer" "$@"; }
window_line() { windows | grep "$1" | sed -E 's/.*(frame=[^ ]+).*(client=[^ ]+) (state=[^ (]+).*/\1 \2 \3/'; }
own_line() { windows | grep "app_id=\"$APP_ID\"" | sed -E 's/.*(frame=[^ ]+).*(client=[^ ]+) (state=[^ (]+).*title="(.*)"/\1 \2 \3 title=\4/'; }
own_uuid() { windows | grep "app_id=\"$APP_ID\"" | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1; }
set_state() { "$SB/kwin_windows/kwin_windows" --set-state "$1" "$2" "$3" > /dev/null; sleep 1; }
last() { grep -E "$1" "$2" | tail -1; }
check() { if [ "$2" = "$3" ]; then echo "ok    $1: $3"; else echo "FAIL  $1: expected '$2', got '$3'"; fi; }
has() { if echo "$3" | grep -qE "$2"; then echo "ok    $1: $3"; else echo "FAIL  $1: expected /$2/, got '$3'"; fi; }
prop() { busctl --user get-property "$APP_ID" "/${APP_ID//.//}" "$APP_ID" "$1" 2>&1; }
png_size() { python3 -c "
import struct, sys
try:
    data = open(sys.argv[1], 'rb').read(26)
    print('%dx%d' % struct.unpack('>II', data[16:24]))
except OSError:
    print('missing')" "$1"; }
kwin_script() {
	local name="magpie-test-$RANDOM"
	echo "$1" > "$OUT/script.js"
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting loadScript ss "$OUT/script.js" "$name" > /dev/null
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting start > /dev/null
	sleep 0.4
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting unloadScript s "$name" > /dev/null
}
# Where the pointer is and which window is active, asked from a KWin script
busctl --user monitor --json=short --match="type='method_call',interface='org.magpie.Probe'" > "$OUT/probe.jsonl" 2>/dev/null &
PIDS+=("$!")
sleep 0.5
cat > "$OUT/probe.js" <<'JS'
callDBus("org.magpie.Probe", "/Probe", "org.magpie.Probe", "Report",
	Math.round(workspace.cursorPos.x), Math.round(workspace.cursorPos.y),
	workspace.activeWindow ? String(workspace.activeWindow.resourceClass) : "none");
JS
probe() {
	local name="magpie-probe-$RANDOM"
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting loadScript ss "$OUT/probe.js" "$name" > /dev/null
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting start > /dev/null
	sleep 0.3
	busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting unloadScript s "$name" > /dev/null
	tail -1 "$OUT/probe.jsonl" | python3 -c "
import json, sys
try:
    data = json.loads(sys.stdin.readline())['payload']['data']
    print('pointer %s,%s active: %s' % (data[0], data[1], data[2]))
except Exception as error:
    print('no report (%s)' % error)
"
}
frames() { grep -E "^(Frames|Pointer|Scaling ended)" "$1" | tr '\n' ' '; }
grab() { # grab OUTPUT NAME: one picture of a monitor, as OUT/NAME.ppm
	"$SB/kwin_cast/kwin_cast" --output "$1" --cursor hidden --no-dmabuf --seconds 1 --dump "$OUT/$2.ppm" > /dev/null 2>&1
}

LOG=$OUT/source.log
"$SB/t8_testsource/testsource" --resizable --duration 500 --title "Magpie test source" > "$LOG" 2>&1 &
SOURCE_PID=$!
PIDS+=("$SOURCE_PID")
sleep 1.5
UUID=$(windows | grep "pid=$SOURCE_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
"$SB/kwin_windows/kwin_windows" --send-to-output "$UUID" "$MONITOR" > /dev/null
sleep 0.8
BEFORE=$(window_line "$UUID")
echo "source: $BEFORE"

for part in "${PARTS[@]}"; do
echo
case $part in
options)
	# A source that does not change, so that only the scaler changes the picture
	"$SB/t8_testsource/testsource" --resizable --static --duration 150 --title "Magpie test source" > "$OUT/static_source.log" 2>&1 &
	STATIC_PID=$!; PIDS+=("$STATIC_PID")
	sleep 1.5
	SUUID=$(windows | grep "pid=$STATIC_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
	"$SB/kwin_windows/kwin_windows" --send-to-output "$SUUID" "$MONITOR" > /dev/null
	sleep 0.8
	SBEFORE=$(window_line "$SUUID")
	mkdir -p "$OUT/shots"
	echo "--- title bar kept, screenshot ---"
	"$SCALE" --window "$SUUID" --title-bar --seconds 4 --screenshot-at 2 --screenshots-dir "$OUT/shots" \
		--screenshot-name "%PN%_%WT:6%" -v > "$OUT/scale_title.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 1.8
	echo "while scaling: $(window_line "$SUUID")"
	wait "$P"; echo "exit status $?"
	check "restored" "$SBEFORE" "$(window_line "$SUUID")"
	grep -E "Scaling [0-9]+x[0-9]+ on|Screenshot|screenshot" "$OUT/scale_title.log" | sed -E 's/^[^|]*\|[^|]*\|[^|]*\|[^|]*\|//' | cut -c1-160
	check "screenshot file" "1361x1080" "$(png_size "$OUT/shots/testsource_Magpie.png")"

	echo "--- without title bar, second screenshot gets a number ---"
	"$SCALE" --window "$SUUID" --seconds 3 --screenshot-at 1.5 --screenshots-dir "$OUT/shots" \
		--screenshot-name "%PN%_%WT:6%" -v > "$OUT/scale_plain.log" 2>&1
	check "restored" "$SBEFORE" "$(window_line "$SUUID")"
	check "screenshot file" "1440x1080" "$(png_size "$OUT/shots/testsource_Magpie (2).png")"

	echo "--- maximized source ---"
	set_state "$SUUID" 4 4
	BEFORE_MAX=$(window_line "$SUUID")
	echo "maximized: $BEFORE_MAX"
	"$SCALE" --window "$SUUID" --seconds 2 > "$OUT/scale_max_refused.log" 2>&1
	echo "without --allow-maximized: exit status $?, $(tail -1 "$OUT/scale_max_refused.log")"
	check "untouched" "$BEFORE_MAX" "$(window_line "$SUUID")"
	"$SCALE" --window "$SUUID" --allow-maximized --seconds 4 --screenshot-at 2 --screenshots-dir "$OUT/shots" \
		--screenshot-name "max" -v > "$OUT/scale_max.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 1.8
	echo "while scaling: $(window_line "$SUUID")"
	wait "$P"; echo "exit status $?"
	check "restored" "$BEFORE_MAX" "$(window_line "$SUUID")"
	grep -E "Scaling [0-9]+x[0-9]+ on" "$OUT/scale_max.log" | sed -E 's/^[^|]*\|[^|]*\|[^|]*\|[^|]*\|//' | cut -c1-160
	echo "screenshot: $(png_size "$OUT/shots/max.png")"

	echo "--- maximized source, scaler killed ---"
	"$SCALE" --window "$SUUID" --allow-maximized --seconds 20 > "$OUT/scale_max_kill.log" 2>&1 &
	P=$!
	sleep 1.8
	echo "while scaling: $(window_line "$SUUID")"
	kill -9 "$P"; wait "$P" 2>/dev/null
	sleep 1
	echo "after the kill: $(window_line "$SUUID")   state file: $(ls ~/.local/state/magpie/ | tr '\n' ' ')"
	"$SCALE" --window "$SUUID" --allow-maximized --seconds 1 -v > "$OUT/scale_max_recover.log" 2>&1
	grep -E "did not end properly" "$OUT/scale_max_recover.log" | sed -E 's/^[^|]*\|[^|]*\|[^|]*\|[^|]*\|//' | cut -c1-160
	check "restored after the next start" "$BEFORE_MAX" "$(window_line "$SUUID")"
	echo "state file: $(ls ~/.local/state/magpie/ | tr '\n' ' ')"

	echo "--- fullscreen source ---"
	set_state "$SUUID" 4 0
	check "not maximized any more" "$SBEFORE" "$(window_line "$SUUID")"
	set_state "$SUUID" 8 8
	BEFORE_FS=$(window_line "$SUUID")
	echo "fullscreen: $BEFORE_FS"
	"$SCALE" --window "$SUUID" --allow-maximized --seconds 4 -v > "$OUT/scale_fs.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 1.8
	echo "while scaling: $(window_line "$SUUID")"
	wait "$P"; echo "exit status $?"
	check "restored" "$BEFORE_FS" "$(window_line "$SUUID")"
	grep -E "Scaling [0-9]+x[0-9]+ on" "$OUT/scale_fs.log" | sed -E 's/^[^|]*\|[^|]*\|[^|]*\|[^|]*\|//' | cut -c1-160
	set_state "$SUUID" 8 0
	check "not fullscreen any more" "$SBEFORE" "$(window_line "$SUUID")"

	echo "--- benchmark mode with a source that does not change ---"
	"$SCALE" --window "$SUUID" --seconds 3 > "$OUT/scale_static.log" 2>&1
	echo "normal:    $(grep Frames "$OUT/scale_static.log")"
	"$SCALE" --window "$SUUID" --seconds 3 --benchmark > "$OUT/scale_benchmark.log" 2>&1
	echo "benchmark: $(grep Frames "$OUT/scale_benchmark.log")"

	echo "--- cursor auto-hide (the pointer is moved by software) ---"
	"$SCALE" --window "$SUUID" --toolbar off --hide-cursor 1 --seconds 9 -v > "$OUT/scale_hide.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 1.5
	"$SB/kwin_cast/kwin_cast" --output "$MONITOR" --cursor hidden --no-dmabuf --seconds 7 --dump "$OUT/hide" > "$OUT/cast_hide.log" 2>&1 &
	CAST=$!
	sleep 1
	# Onto the picture: the scaler sends it to the source
	pointer move 4800 540
	sleep 0.5
	kill -USR1 "$CAST"	# 1: cursor shown
	sleep 2
	kill -USR1 "$CAST"	# 2: hidden after a second of rest
	sleep 0.3
	pointer rel 6 0
	sleep 0.4
	kill -USR1 "$CAST"	# 3: shown again
	sleep 0.3
	wait "$CAST" 2>/dev/null
	wait "$P"; echo "exit status $?"
	ls "$OUT"/hide* 2>/dev/null | tr '\n' ' '; echo
	python3 - "$OUT" <<-'PY'
	import glob, sys
	out = sys.argv[1]
	def load(path):
	    with open(path, 'rb') as f:
	        data = f.read()
	    parts = data.split(b'\n', 3)
	    w, h = map(int, parts[1].split())
	    return w, h, parts[3]
	files = sorted(glob.glob(out + '/hide.*.ppm'), key=lambda p: int(p.split('.')[-2]))
	if len(files) < 3:
	    print('FAIL  only %d pictures of the monitor' % len(files))
	    sys.exit()
	pics = [load(f) for f in files[:3]]
	def diff(a, b):
	    w, h, pa = a
	    _, _, pb = b
	    count = 0
	    xs, ys = [], []
	    for i in range(0, min(len(pa), len(pb)), 3):
	        if pa[i:i+3] != pb[i:i+3]:
	            count += 1
	            p = i // 3
	            xs.append(p % w); ys.append(p // w)
	    box = '%d,%d to %d,%d' % (min(xs), min(ys), max(xs), max(ys)) if xs else '-'
	    return count, box
	d12 = diff(pics[0], pics[1]); d23 = diff(pics[1], pics[2]); d13 = diff(pics[0], pics[2])
	print('shown -> hidden: %d pixels differ (%s)' % d12)
	print('hidden -> shown again: %d pixels differ (%s)' % d23)
	print('%s  the cursor disappeared after the delay and came back on movement' %
	      ('ok   ' if d12[0] > 30 and d23[0] > 30 else 'FAIL '))
	PY
	check "restored" "$SBEFORE" "$(window_line "$SUUID")"
	kill "$STATIC_PID" 2>/dev/null
	sleep 0.5
	kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$UUID\") { workspace.activeWindow = w; } }"
	;;
full)
	echo "--- fullscreen on one monitor, as before the rewrite ---"
	pointer move 4000 900
	"$SCALE" --window "$UUID" --seconds 9 -v --validate > "$OUT/scale_full.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 2.5
	grep -E "^Scaling " "$OUT/scale_full.log" | cut -c1-120
	has "source on the virtual output" "frame=640x480\+1\+17464" "$(window_line "$UUID")"
	pointer move 4800 540 sleep 400
	has "pointer on the source's centre" "pointer 321,17704" "$(probe)"
	pointer click sleep 300
	has "click" "x=320 y=240" "$(last '^press' "$LOG")"
	pointer rel -5000 0 sleep 500
	has "pushed far left: on DP-5" "pointer 3839,[0-9]+ " "$(probe)"
	# More than KWin's edge barrier of 100 pixels between monitors
	pointer rel 300 0 sleep 500
	has "back: on the source" "pointer [0-9]+,17[0-9]+ " "$(probe)"
	wait "$P"; echo "exit status $?"
	frames "$OUT/scale_full.log"; echo
	grep -ciE "validation" "$OUT/scale_full.log" | sed 's/^/validation messages: /'
	check "restored" "$BEFORE" "$(window_line "$UUID")"
	;;
windowed)
	echo "--- windowed scaling ---"
	pointer move 2880 900
	"$SCALE" --window "$UUID" --windowed --seconds 24 -v --validate > "$OUT/scale_windowed.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 2.5
	grep -E "^Scaling " "$OUT/scale_windowed.log" | cut -c1-140
	has "source on the virtual output" "frame=640x480\+1\+17465" "$(window_line "$UUID")"
	OWN=$(own_line)
	echo "the scaled window: $OWN"
	has "its content is 1080x810 around the source's centre" "client=1080x810\+4260\+113 " "$OWN"
	has "the source is the active window" "active: testsource" "$(probe)"
	grab "$MONITOR" windowed
	# Onto the content's centre
	pointer move 4800 518 sleep 500
	has "pointer on the source's centre" "pointer 32[01],1770[45] " "$(probe)"
	pointer click sleep 300
	has "click" "x=3(19|20) y=2(39|40)" "$(last '^press' "$LOG")"
	pointer rel -5000 0 sleep 500
	has "pushed far left: just left of the window" "pointer 4259,[0-9]+ " "$(probe)"
	pointer move 4800 518 sleep 500
	pointer rel 0 -5000 sleep 500
	has "pushed far up: on the title bar" "pointer [0-9]+,112 " "$(probe)"
	pointer move 4800 518 sleep 400

	echo "resized by the desktop to a wider frame:"
	OWN_UUID=$(own_uuid)
	kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$OWN_UUID\") { const g = w.frameGeometry; w.frameGeometry = { x: g.x - 100, y: g.y, width: g.width + 200, height: g.height }; } }"
	sleep 1.5
	OWN=$(own_line)
	echo "the scaled window: $OWN"
	has "the content kept 4:3" "client=1280x960\+" "$OWN"
	pointer move 4800 518 sleep 500
	echo "pointer in the resized window: $(probe)"
	grab "$MONITOR" windowed_resized

	echo "closed by the desktop:"
	kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$OWN_UUID\") { w.closeWindow(); } }"
	wait "$P"; echo "exit status $?"
	grep -E "^Scaling ended" "$OUT/scale_windowed.log"
	frames "$OUT/scale_windowed.log"; echo
	grep -ciE "validation" "$OUT/scale_windowed.log" | sed 's/^/validation messages: /'
	sleep 0.5
	check "restored" "$BEFORE" "$(window_line "$UUID")"
	check "the scaled window is gone" "" "$(own_line)"

	echo "the next windowed scaling starts with a given factor:"
	"$SCALE" --window "$UUID" --windowed --window-scale 1.5 --seconds 3 > "$OUT/scale_windowed2.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 2
	has "content 960x720" "client=960x720\+" "$(own_line)"
	wait "$P"
	check "restored" "$BEFORE" "$(window_line "$UUID")"
	;;
3d)
	echo "--- 3D game mode ---"
	pointer move 2880 540
	"$SCALE" --window "$UUID" --3d --seconds 6 -v > "$OUT/scale_3d.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 2.5
	has "source on the virtual output without a strip" "frame=640x480\+0\+17464" "$(window_line "$UUID")"
	has "the pointer went to the source's centre from another monitor" "pointer 320,17704" "$(probe)"
	pointer rel -5000 0 sleep 500
	has "pushed far left: still with the source" "pointer 0,17704" "$(probe)"
	pointer rel 320 0 sleep 300
	wait "$P"; echo "exit status $?"
	frames "$OUT/scale_3d.log"; echo
	has "the pointer is where its picture was" "pointer 480[01],54[01]" "$(probe)"
	check "restored" "$BEFORE" "$(window_line "$UUID")"
	;;
all)
	echo "--- one picture over all monitors ---"
	pointer move 4000 900
	"$SCALE" --window "$UUID" --monitors all --seconds 7 -v --validate > "$OUT/scale_all.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 2.5
	grep -E "^Scaling " "$OUT/scale_all.log" | cut -c1-140
	has "source on the virtual output without a strip" "frame=640x480\+0\+17464" "$(window_line "$UUID")"
	grab HDMI-A-2 all_left
	grab DP-5 all_middle
	grab DP-6 all_right
	# The picture is 1440x1080 in the middle of 5760x1080: its centre is at 2880,540
	pointer move 2880 540 sleep 500
	has "pointer on the source's centre" "pointer 320,17704" "$(probe)"
	pointer click sleep 300
	has "click" "x=320 y=240" "$(last '^press' "$LOG")"
	wait "$P"; echo "exit status $?"
	frames "$OUT/scale_all.log"; echo
	grep -ciE "validation" "$OUT/scale_all.log" | sed 's/^/validation messages: /'
	check "restored" "$BEFORE" "$(window_line "$UUID")"
	;;
intersected)
	echo "--- a picture over the monitors the window touches ---"
	kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$UUID\") { const g = w.frameGeometry; w.frameGeometry = { x: 3600, y: 264, width: g.width, height: g.height }; } }"
	sleep 0.5
	STRADDLING=$(window_line "$UUID")
	echo "source across DP-5 and DP-6: $STRADDLING"
	pointer move 1000 900
	"$SCALE" --window "$UUID" --monitors intersected --seconds 7 -v > "$OUT/scale_intersected.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 2.5
	grep -E "^Scaling " "$OUT/scale_intersected.log" | cut -c1-140
	grab DP-5 intersected_middle
	grab DP-6 intersected_right
	# The picture is 1440x1080 in the middle of DP-5 and DP-6: its centre is at 3840,540
	pointer move 3840 540 sleep 500
	has "pointer on the source's centre" "pointer 32[01],1770[45]" "$(probe)"
	pointer rel -5000 0 sleep 500
	has "pushed far left: on HDMI-A-2" "pointer 1919,[0-9]+ " "$(probe)"
	wait "$P"; echo "exit status $?"
	frames "$OUT/scale_intersected.log"; echo
	check "restored" "$STRADDLING" "$(window_line "$UUID")"
	kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$UUID\") { const g = w.frameGeometry; w.frameGeometry = { x: 4480, y: 264, width: g.width, height: g.height }; } }"
	sleep 0.5
	;;
toolbar)
	echo "--- toolbar and profiler on a fullscreen picture ---"
	pointer move 2880 900
	"$SCALE" --window "$UUID" --toolbar show --developer --screenshots-dir "$OUT/shots" --screenshot-name "toolbar" \
		--effect FSR/FSR_EASU,type=fit --effect FSR/FSR_RCAS --seconds 30 -v --validate > "$OUT/scale_toolbar.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 2.5
	grep -E "^Scaling |overlay uses|icon font" "$OUT/scale_toolbar.log" | sed -E 's/^[^|]*\|[^|]*\|[^|]*\|[^|]*\|//' | cut -c1-140
	grab "$MONITOR" toolbar
	# The picture is at 4080..5520 of the desktop. Its centre first.
	pointer move 4800 540 sleep 400
	has "pointer on the source's centre" "pointer 321,17704" "$(probe)"
	# Up to the top edge, under the toolbar: the pointer comes to the monitor
	pointer rel 0 -400 sleep 500
	has "moved up onto the toolbar: the pointer is on the monitor" "pointer 480[0-3],[0-3] " "$(probe)"
	# The profiler button, the second from the left
	pointer move 4672 14 sleep 300 click sleep 600
	grab "$MONITOR" profiler
	# The screenshot button, the third
	pointer move 4704 14 sleep 300 click sleep 1200
	check "screenshot taken with the button" "1440x1080" "$(python3 -c "
import struct, sys
try:
    data = open(sys.argv[1], 'rb').read(26)
    print('%dx%d' % struct.unpack('>II', data[16:24]))
except OSError:
    print('missing')" "$OUT/shots/toolbar.png")"
	# Away from the toolbar: the pointer belongs to the source again
	pointer move 4800 400 sleep 500
	has "below the toolbar: on the source" "pointer [0-9]+,17[0-9]+ " "$(probe)"
	pointer click sleep 300
	has "click reaches the source" "x=320 y=17[78]" "$(last '^press' "$LOG")"
	# The stop button at the right end
	pointer move 4960 14 sleep 400 click
	wait "$P"; echo "exit status $?"
	frames "$OUT/scale_toolbar.log"; echo
	grep -E "^Scaling ended" "$OUT/scale_toolbar.log"
	grep -ciE "validation" "$OUT/scale_toolbar.log" | sed 's/^/validation messages: /'
	check "restored" "$BEFORE" "$(window_line "$UUID")"
	;;
service)
	echo "--- the service: windowed scaling, the buttons that change the mode and minimize ---"
	pointer move 2880 900
	"$B/tools/magpie-engine/magpie-engine" --no-shortcuts --no-cursor-speed -v > "$OUT/engine.log" 2>&1 &
	ENGINE_PID=$!; PIDS+=("$ENGINE_PID")
	sleep 1
	check "status at start" "idle" "$("$CTL" status)"
	"$CTL" window "$UUID"; sleep 2.5
	check "status" "scaling" "$("$CTL" status)"
	has "scaled into a window" "client=1080x810\+4260\+113 " "$(own_line)"
	echo "Windowed: $(prop Windowed)   SourceActive: $(prop SourceActive)"
	echo "SourceRect: $(prop SourceRect)   DestinationRect: $(prop DestinationRect)"
	# The toolbar hides until the pointer is near. The button that changes
	# the mode is the second from the right.
	pointer move 4800 300 sleep 400
	pointer move 4932 127 sleep 500
	grab "$MONITOR" service_windowed
	pointer click; sleep 3
	check "status after the mode button" "scaling" "$("$CTL" status)"
	echo "Windowed: $(prop Windowed)   DestinationRect: $(prop DestinationRect)"
	has "source placed as for a fullscreen picture" "frame=640x480\+1\+17464" "$(window_line "$UUID")"
	check "no scaled window left" "" "$(own_line)"

	echo "the toolbar command:"
	"$CTL" toolbar; sleep 0.3; "$CTL" toolbar; sleep 0.3; "$CTL" toolbar; sleep 0.3
	grep -E "Toolbar: " "$OUT/engine.log" | sed -E 's/.*(Toolbar: .*)/\1/' | tr '\n' ' '; echo

	echo "the minimize button of the toolbar:"
	pointer move 4800 300 sleep 300
	pointer move 4904 14 sleep 500 click; sleep 1.5
	check "status" "waiting" "$("$CTL" status)"
	has "source minimized" "minimized" "$(window_line "$UUID")"
	"$SB/kwin_windows/kwin_windows" --set-state "$UUID" 2 0 > /dev/null; sleep 2.5
	check "shown again" "scaling" "$("$CTL" status)"
	"$CTL" stop; sleep 1
	check "stopped" "idle" "$("$CTL" status)"
	check "restored" "$BEFORE" "$(window_line "$UUID" | sed 's/state=-/state=active/')"
	kill -TERM "$ENGINE_PID"; wait "$ENGINE_PID"; echo "service ended with status $?"
	grep -E "State: " "$OUT/engine.log" | sed -E 's/.*State: //' | tr '\n' ' '; echo
	ls ~/.config/magpie/ 2>/dev/null | sed 's/^/config directory: /'
	;;
focus)
	echo "--- coming back from the neighbouring monitor through KWin's edge barrier ---"
	pointer move 4000 900
	"$SCALE" --window "$UUID" --toolbar off --seconds 7 > "$OUT/scale_back.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 2.5
	pointer move 4800 540 sleep 400
	pointer rel -5000 0 sleep 500
	has "pushed far left: on DP-5" "pointer 3839,[0-9]+ " "$(probe)"
	pointer rel 300 0 sleep 500
	has "300 px back: on the source" "pointer [0-9]+,17[0-9]+ " "$(probe)"
	wait "$P"
	frames "$OUT/scale_back.log"; echo

	echo "--- windowed: another window is activated while the pointer is on the source ---"
	"$SB/t8_testsource/testsource" --duration 30 --no-focus --title "Magpie other window" > "$OUT/other.log" 2>&1 &
	OTHER_PID=$!; PIDS+=("$OTHER_PID")
	sleep 1.2
	OTHER=$(windows | grep "pid=$OTHER_PID " | sed -E 's/.*uuid=(\{[^}]+\}).*/\1/' | head -1)
	kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$OTHER\") { w.frameGeometry = { x: 3900, y: 600, width: w.frameGeometry.width, height: w.frameGeometry.height }; } }"
	"$SCALE" --window "$UUID" --windowed --toolbar off --seconds 10 > "$OUT/scale_focus.log" 2>&1 &
	P=$!; PIDS+=("$P")
	sleep 2.5
	pointer move 4800 518 sleep 500
	has "pointer on the source" "pointer 32[01],1770[45] " "$(probe)"
	kwin_script "for (const w of workspace.windowList()) { if (w.internalId.toString() === \"$OTHER\") { workspace.activeWindow = w; } }"
	sleep 0.8
	has "the pointer went to its picture on the monitor" "pointer 480[01],51[789] " "$(probe)"
	echo "scaled window: $(own_line | cut -c1-90)"
	sleep 0.5
	has "and stays there while it does not move" "pointer 480[01],51[789] " "$(probe)"
	pointer rel 3 0 sleep 500
	has "moved: on the source again" "pointer [0-9]+,17[0-9]+ " "$(probe)"
	wait "$P"
	frames "$OUT/scale_focus.log"; echo
	kill "$OTHER_PID" 2>/dev/null
	sleep 0.5
	check "restored" "$BEFORE" "$(window_line "$UUID" | sed 's/state=-/state=active/')"
	;;
*)
	echo "unknown part: $part"
	;;
esac
done

echo
echo "--- warnings and errors of the scaler and the service ---"
grep -hE "\|(warning|error)\|" "$OUT"/*.log | sed -E 's/^[^|]*\|([a-z]+)\|[^|]*\|[^|]*\|/[\1] /' | cut -c1-220 | sort | uniq -c | head -15
pointer move 4800 900
echo "done; pictures of the monitors are in $OUT as .ppm files"
