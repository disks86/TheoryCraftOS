#!/usr/bin/env bash
# The kernel's X server (src/X11) against real X clients of the development machine: libX11 / libxcb (tests/x11/xlib_client.c) and xprop.
# tests/x11/X11HostServer.cpp serves x11::Server on the host's abstract socket @/tmp/.X11-unix/X<display>; the clients connect with DISPLAY=:<display>.
# Needs the libX11 headers and library (xprop is used when installed). Usage: tests/x11_interop.sh [display]   (default 87)
set -euo pipefail
cd "$(dirname "$0")/.."
display=${1:-87}
out=${TMPDIR:-/tmp}/tcos-x11-interop
mkdir -p "$out"
if [ ! -f /usr/include/X11/Xlib.h ]; then echo "skipped (no libX11 headers)"; exit 0; fi
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src tests/x11/X11HostServer.cpp src/X11/X11DragAndDrop.cpp src/X11/X11Drawing.cpp src/X11/X11Input.cpp src/X11/X11Requests.cpp \
  src/X11/X11Tables.cpp src/X11/X11Windows.cpp src/Kernel/Sync.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp tests/KernelStubs.cpp \
  -o "$out/X11HostServer"
gcc -O1 -g -Wall tests/x11/xlib_client.c -lX11 -o "$out/xlib_client"
gcc -O1 -g -Wall tests/x11/xdnd_client.c -lX11 -o "$out/xdnd_client"

"$out/X11HostServer" "$display" 60 "$out/windows.ppm" > "$out/server.log" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null || true' EXIT
for _ in $(seq 50); do grep -q listening "$out/server.log" 2>/dev/null && break; sleep 0.1; done

status=0
echo "== xlib_client"
DISPLAY=:$display XAUTHORITY=/dev/null "$out/xlib_client" || status=1
if command -v xprop >/dev/null; then
  echo "== xprop -root"
  DISPLAY=:$display XAUTHORITY=/dev/null xprop -root _NET_SUPPORTING_WM_CHECK _NET_WORKAREA || status=1
fi
# several clients at once
echo "== 4 xlib clients at once"
pids=()
for _ in 1 2 3 4; do DISPLAY=:$display XAUTHORITY=/dev/null XLIB_CLIENT_SHARED=1 "$out/xlib_client" > "$out/parallel.$_.log" 2>&1 & pids+=($!); done
for p in "${pids[@]}"; do wait "$p" || status=1; done
grep -h "checks" "$out"/parallel.*.log
kill "$server"
wait "$server" || true
if grep -q "ERROR: AddressSanitizer\|runtime error" "$out/server.log"; then cat "$out/server.log"; status=1; fi
# XDND with the desktop: a server of its own that plays the desktop's part (TCOS_XDND)
echo "== xdnd_client"
TCOS_XDND=1 "$out/X11HostServer" "$((display + 1))" 30 > "$out/xdnd-server.log" 2>&1 &
xserver=$!
for _ in $(seq 50); do grep -q listening "$out/xdnd-server.log" 2>/dev/null && break; sleep 0.1; done
DISPLAY=:$((display + 1)) XAUTHORITY=/dev/null "$out/xdnd_client" || status=1
kill "$xserver" 2>/dev/null || true
wait "$xserver" || true
grep "desktop drop\|program drop" "$out/xdnd-server.log" || true
grep -q "program drop at 700,450: /Desktop/Documents/b.txt" "$out/xdnd-server.log" || { echo "the desktop did not get the client's file"; status=1; }
if grep -q "ERROR: AddressSanitizer\|runtime error" "$out/xdnd-server.log"; then cat "$out/xdnd-server.log"; status=1; fi
[ $status -eq 0 ] && echo "x11 interop: all passed" || echo "x11 interop: FAILED"
exit $status
