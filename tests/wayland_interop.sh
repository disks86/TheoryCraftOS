#!/usr/bin/env bash
# The kernel's Wayland compositor (src/Wayland) against real Wayland clients of the development machine: libwayland-client + libxkbcommon
# (tests/wayland/wl_client.c), four of those at once, and foot (a terminal) when it is installed. tests/wayland/WaylandHostServer.cpp serves
# wayland::Server on a socket in a scratch XDG_RUNTIME_DIR, with descriptors passed as SCM_RIGHTS.
# Needs libwayland-client, libxkbcommon, wayland-scanner and wayland-protocols. Usage: tests/wayland_interop.sh
set -euo pipefail
cd "$(dirname "$0")/.."
out=${TMPDIR:-/tmp}/tcos-wayland-interop
mkdir -p "$out"
runtime=$(mktemp -d /tmp/tcoswl.XXXXXX)      # (short: a socket path is at most 107 bytes)
trap 'rm -rf "$runtime"' EXIT
protocols=/usr/share/wayland-protocols
if ! command -v wayland-scanner >/dev/null || [ ! -f /usr/include/wayland-client.h ] || [ ! -d $protocols ]; then echo "skipped (no libwayland/wayland-protocols)"; exit 0; fi
for p in stable/xdg-shell/xdg-shell unstable/xdg-decoration/xdg-decoration-unstable-v1; do
  wayland-scanner client-header "$protocols/$p.xml" "$out/$(basename $p)-client-protocol.h"
  wayland-scanner private-code "$protocols/$p.xml" "$out/$(basename $p)-protocol.c"
done
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src tests/wayland/WaylandHostServer.cpp $(ls src/Wayland/*.cpp | grep -v WaylandSocketServer) src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp src/Wayland/Media/WaylandClipboardPng.cpp src/Media/Image/Png.cpp src/Media/Image.cpp src/Media/Zlib.cpp src/Math/FloatMath.cpp \
  src/Graphics/Graphics2D/Software/SoftwareRenderer2D.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp tests/KernelStubs.cpp -o "$out/WaylandHostServer"
gcc -O1 -g -Wall -I "$out" tests/wayland/wl_client.c "$out/xdg-shell-protocol.c" "$out/xdg-decoration-unstable-v1-protocol.c" -lwayland-client -lxkbcommon \
  -o "$out/wl_client"

run_server() {      # <name> <seconds> <dump>
  "$out/WaylandHostServer" "$runtime" "$1" "$2" "$3" > "$out/$1.log" 2>&1 &
  server=$!
  for _ in $(seq 50); do grep -q listening "$out/$1.log" 2>/dev/null && break; sleep 0.1; done
}
status=0
echo "== wl_client"
run_server wayland-tcos 30 "$out/wl_client.ppm"
XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=wayland-tcos timeout 20 "$out/wl_client" || status=1
kill $server; wait $server || true
grep -q "ERROR: AddressSanitizer\|runtime error" "$out/wayland-tcos.log" && { cat "$out/wayland-tcos.log"; status=1; }

echo "== 4 wl_clients at once"
run_server wayland-tcos4 30 "$out/parallel.ppm"
pids=()
for k in 1 2 3 4; do WL_CLIENT_PARALLEL=1 XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=wayland-tcos4 timeout 25 "$out/wl_client" > "$out/parallel.$k.log" 2>&1 & pids+=($!); done
for p in "${pids[@]}"; do wait "$p" || status=1; done
grep -h "checks" "$out"/parallel.*.log
kill $server; wait $server || true
grep -q "ERROR: AddressSanitizer\|runtime error" "$out/wayland-tcos4.log" && { cat "$out/wayland-tcos4.log"; status=1; }

if command -v foot >/dev/null; then
  echo "== foot"
  run_server wayland-foot 8 "$out/foot.ppm"
  XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=wayland-foot timeout 6 foot -o main.font=monospace:size=11 sh -c 'echo "hello from foot on TheoryCraftOS"; sleep 4' > "$out/foot.client.log" 2>&1 || true
  wait $server || true
  cat "$out/wayland-foot.log"
  grep -q 'mapped .* "' "$out/wayland-foot.log" || { echo "foot showed no window"; cat "$out/foot.client.log"; status=1; }
  grep -q "ERROR: AddressSanitizer\|runtime error" "$out/wayland-foot.log" && status=1
fi
if command -v wl-copy >/dev/null && command -v wl-paste >/dev/null; then
  echo "== wl-clipboard (copy and paste: the desktop's clipboard to a client, a client to another and to the desktop's clipboard)"
  TCOS_CLIPBOARD_TEXT="text the desktop copied" run_server wayland-clip 12 ""
  pasted=$(XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=wayland-clip timeout 5 wl-paste --no-newline 2>"$out/clip.paste1.log" || true)
  echo "pasted the desktop's: '$pasted'"
  [ "$pasted" = "text the desktop copied" ] || status=1
  XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=wayland-clip timeout 8 wl-copy "copied by wl-copy" 2>"$out/clip.copy.log" || true
  sleep 0.5
  pasted=$(XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=wayland-clip timeout 5 wl-paste --no-newline 2>"$out/clip.paste2.log" || true)
  echo "pasted wl-copy's: '$pasted'"
  [ "$pasted" = "copied by wl-copy" ] || status=1
  kill $server; wait $server || true
  grep "clipboard:" "$out/wayland-clip.log" || true
  grep -q "clipboard: copied by wl-copy" "$out/wayland-clip.log" || { echo "the desktop's clipboard did not get wl-copy's text"; status=1; }
  grep -q "ERROR: AddressSanitizer\|runtime error" "$out/wayland-clip.log" && { cat "$out/wayland-clip.log"; status=1; }
  echo "== wl-clipboard pictures (image/png: the desktop's picture to a client, a client's picture to the desktop's clipboard)"
  TCOS_CLIPBOARD_IMAGE=1 run_server wayland-clipimg 12 ""
  types=$(XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=wayland-clipimg timeout 5 wl-paste --list-types 2>"$out/clipimg.list.log" || true)
  echo "offered: $types"
  [ "$types" = "image/png" ] || status=1
  XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=wayland-clipimg timeout 5 wl-paste --type image/png > "$out/pasted.png" 2>"$out/clipimg.paste.log" || true
  python3 - "$out/pasted.png" <<'PY' || status=1
import struct, sys, zlib
data = open(sys.argv[1], 'rb').read()
assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
w, h, depth, ctype = struct.unpack('>IIBB', data[16:26])
pos, idat = 8, b''
while pos < len(data):
    n, kind = struct.unpack('>I4s', data[pos:pos + 8]); idat += data[pos + 8:pos + 8 + n] if kind == b'IDAT' else b''; pos += 12 + n
raw, bpp, rows, prev = zlib.decompress(idat), 4 if ctype == 6 else 3, [], bytearray(w * (4 if ctype == 6 else 3))
for y in range(h):
    f, line = raw[y * (w * bpp + 1)], bytearray(raw[y * (w * bpp + 1) + 1:(y + 1) * (w * bpp + 1)])
    for i in range(len(line)):
        a, b, c = (line[i - bpp] if i >= bpp else 0), prev[i], (prev[i - bpp] if i >= bpp else 0)
        p = a + b - c; pr = a if abs(p - a) <= abs(p - b) and abs(p - a) <= abs(p - c) else (b if abs(p - b) <= abs(p - c) else c)
        line[i] = (line[i] + [0, a, b, (a + b) // 2, pr][f]) & 255
    rows.append(line); prev = line
print('pasted picture', w, 'x', h, 'colour type', ctype, 'first', bytes(rows[0][:4]).hex(), 'pixel 3', bytes(rows[1][:4]).hex())
assert (w, h, ctype) == (3, 2, 6) and bytes(rows[0][:4]) == b'\xff\x00\x00\xff' and bytes(rows[1][:4]) == b'\xff\xff\xff\x80'
PY
  python3 -c 'import struct,zlib,sys
def chunk(k, d): return struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d) & 0xffffffff)
rows = b"".join(b"\x00" + b"".join(bytes([10 * x, 20 * y, 30, 255]) for x in range(5)) for y in range(3))
sys.stdout.buffer.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 5, 3, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))' > "$out/copied.png"
  XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=wayland-clipimg timeout 8 wl-copy --type image/png < "$out/copied.png" 2>"$out/clipimg.copy.log" || true
  sleep 0.5
  kill $server; wait $server || true
  grep "clipboard image:" "$out/wayland-clipimg.log" || true
  # (5 x 3, first pixel R 0 G 0 B 30, last R 40 G 40 B 30)
  grep -q "clipboard image: 5x3 first ff00001e last ff28281e" "$out/wayland-clipimg.log" || { echo "the desktop's clipboard did not get wl-copy's picture"; status=1; }
  grep -q "ERROR: AddressSanitizer\|runtime error" "$out/wayland-clipimg.log" && { cat "$out/wayland-clipimg.log"; status=1; }
fi
if command -v gst-launch-1.0 >/dev/null && gst-inspect-1.0 waylandsink >/dev/null 2>&1; then
  # YUV through wl_shm: GStreamer's colour bars in NV12, I420 and YUY2 (the planes in the pool as the compositor expects them), drawn by the desktop.
  # (drop-allocation: waylandsink copies each frame into a wl_shm buffer of its own. When videotestsrc draws into waylandsink's pool instead, GStreamer 1.28
  # writes NV12 and I420 rows 512 bytes apart but declares stride 320 to wl_shm - its pool's alignment never reaches the wl_buffer.)
  for format in NV12 I420 YUY2; do
    echo "== gstreamer waylandsink $format"
    run_server wayland-gst-$format 6 "$out/gst-$format.ppm"
    XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=wayland-gst-$format timeout 5 gst-launch-1.0 -q videotestsrc pattern=smpte75 num-buffers=90 \
      ! video/x-raw,format=$format,width=320,height=240,framerate=30/1 ! identity drop-allocation=true ! waylandsink > "$out/gst-$format.client.log" 2>&1 || true
    wait $server || true
    python3 - "$out/gst-$format.ppm" <<'PY' || { cat "$out/gst-$format.client.log" "$out/wayland-gst-$format.log"; status=1; }
import sys
data = open(sys.argv[1], 'rb').read()
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
def at(x, y): i = (y * w + x) * 3; return tuple(px[i:i + 3])
# the 75% bars, left to right: white, yellow, cyan, green, magenta, red, blue (each 320 / 7 wide), sampled in the middle of the top part
want = [(191, 191, 191), (191, 191, 0), (0, 191, 191), (0, 191, 0), (191, 0, 191), (191, 0, 0), (0, 0, 191)]
got = [at(int((k + 0.5) * 320 / 7), 60) for k in range(7)]
print('picture', w, 'x', h, 'bars', got)
assert w >= 320 and h >= 240, 'no window of the video'
assert all(abs(a - b) <= 6 for g, e in zip(got, want) for a, b in zip(g, e)), 'wrong colours'
PY
    grep -q "ERROR: AddressSanitizer\|runtime error" "$out/wayland-gst-$format.log" && { cat "$out/wayland-gst-$format.log"; status=1; }
  done
fi
[ $status -eq 0 ] && echo "wayland interop: all passed" || echo "wayland interop: FAILED"
exit $status
