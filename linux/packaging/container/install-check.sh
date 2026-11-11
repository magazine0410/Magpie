#!/usr/bin/env bash
# Runs inside a plain Fedora container, started by container-check.sh:
# installs the RPM from /out with its dependencies and tries the installed
# programs. Nothing of the source or build tree is here, so whatever the
# package fails to bring shows.
set -u
PASSED=0
FAILED=0
check() {
	if [ "$2" = "$3" ]; then
		PASSED=$((PASSED + 1)); echo "  ok    $1"
	else
		FAILED=$((FAILED + 1)); echo "  FAIL  $1: \"$2\", expected \"$3\""
	fi
}

echo "################ dnf install ################"
dnf -y --setopt=keepcache=1 install /out/magpie-upscaler-[0-9]*.x86_64.rpm > /out/install.log 2>&1
check "the package installs with its dependencies" "$?" "0"
grep -E "^(Error|Problem| - )" /out/install.log | head -10
echo "packages installed with it: $(grep -cE "^ +[a-zA-Z0-9_.+-]+ +(x86_64|noarch)" /out/install.log)"
check "sway is installed as a weak dependency" "$(rpm -q sway > /dev/null && echo yes)" "yes"
check "no file of the package is missing" "$(rpm -V magpie-upscaler | wc -l)" "0"
check "the .desktop file is valid" "$(desktop-file-validate /usr/share/applications/io.github.magazine0410.Magpie.desktop 2>&1 | wc -l)" "0"
check "every library of the programs is found" \
	"$(for p in /usr/bin/magpie /usr/bin/magpie-engine /usr/bin/magpie-ctl /usr/bin/magpie-scale /usr/bin/magpie-scale-image /usr/bin/magpie-viewer /usr/bin/magpie-fxc; do ldd "$p"; done | grep -c "not found")" "0"

# What only the checks need: a Vulkan implementation without a graphics
# card, and a session bus
dnf -y --setopt=keepcache=1 install mesa-vulkan-drivers mesa-dri-drivers dbus-daemon python3 procps-ng >> /out/install.log 2>&1

export HOME=/root XDG_RUNTIME_DIR=/tmp/runtime
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
cd /tmp

echo "################ the installed programs ################"
check "magpie-scale-image lists the software renderer" "$(magpie-scale-image --list-gpus 2>&1 | grep -c llvmpipe)" "1"
python3 - <<'PY'
import struct, zlib
w, h = 64, 48
rows = b"".join(b"\0" + bytes((x * 4 % 256, y * 5 % 256, (x + y) % 256) * 1 for x in range(w)) for y in range(h))
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
open("/tmp/in.png", "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
PY
magpie-scale-image --effect Lanczos,scale=2 /tmp/in.png /tmp/out.png > /out/scale-image.log 2>&1
check "an effect of the installed directory scales a picture" "$? $(python3 -c 'import struct; d = open("/tmp/out.png", "rb").read(); print(*struct.unpack(">II", d[16:24]))' 2>&1)" "0 128 96"
magpie-fxc /usr/share/magpie/effects/FSR/FSR_EASU.hlsl > /out/fxc.log 2>&1
check "magpie-fxc compiles an installed effect" "$?" "0"

# A session bus that cannot start services
cat > /tmp/bus.conf <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=/tmp/runtime/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
dbus-daemon --nofork --config-file=/tmp/bus.conf > /out/bus.log 2>&1 &
for i in $(seq 1 50); do [ -S /tmp/runtime/bus ] && break; sleep 0.1; done
export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/runtime/bus

# The window of the application, on a display that does not exist
QT_QPA_PLATFORM=offscreen magpie > /out/app.log 2>&1 &
APP=$!
sleep 6
check "the application runs" "$(kill -0 "$APP" 2> /dev/null && echo yes)" "yes"
check "it answers on the session bus" "$(magpie-ctl status 2>&1)" "idle"
check "its window loaded without errors" "$(grep -ciE "qml.*(error|is not installed|not a type|unavailable)|module .* is not installed" /out/app.log)" "0"
kill "$APP" 2> /dev/null
wait "$APP" 2> /dev/null

echo "################ a nested session with the installed service ################"
export SWAYSOCK=/tmp/runtime/sway.sock WLR_RENDERER=pixman
echo "xwayland disable" > /tmp/sway.conf
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 sway -c /tmp/sway.conf > /out/sway.log 2>&1 &
for i in $(seq 1 100); do [ -S "$SWAYSOCK" ] && ls /tmp/runtime/wayland-* > /dev/null 2>&1 && break; sleep 0.1; done
WAYLAND_DISPLAY=$(basename "$(ls /tmp/runtime/wayland-* | grep -v '\.lock$' | head -1)")
export WAYLAND_DISPLAY
magpie-engine --no-shortcuts --no-cursor-speed -v > /out/engine.log 2>&1 &
ENGINE=$!
sleep 2
check "magpie-engine runs on the Sway" "$(magpie-ctl status 2>&1)" "idle"
magpie-ctl launch --size 320x240 -- /out/magpie_test_window --title nested --resizable
for i in $(seq 1 100); do [ "$(magpie-ctl status 2>&1)" = scaling ] && break; sleep 0.1; done
check "a program is scaled in a nested session" "$(magpie-ctl status 2>&1) $(magpie-ctl nested 2>&1)" "scaling yes"
sleep 1
check "frames are scaled with the installed effects and font" "$(grep -c "Scaling 320x240" /out/engine.log)" "1"
magpie-ctl stop
for i in $(seq 1 50); do [ "$(magpie-ctl status 2>&1)" = idle ] && break; sleep 0.1; done
check "stop ends it" "$(magpie-ctl status 2>&1)" "idle"
kill "$ENGINE" 2> /dev/null
wait "$ENGINE" 2> /dev/null
grep -hE "\|(warning|error|critical)\|" /out/engine.log | sed -E 's/^([^|]*\|[^|]*)\|[^|]*\|[^|]*\|/\1|/' | cut -c1-200 | sort | uniq -c | head -10

echo
echo "$PASSED passed, $FAILED failed"
[ "$FAILED" = 0 ]
