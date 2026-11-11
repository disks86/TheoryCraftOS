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
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src tests/wayland/WaylandHostServer.cpp src/Wayland/WaylandDesktop.cpp src/Wayland/WaylandInput.cpp src/Wayland/WaylandKeymap.cpp src/Wayland/WaylandServer.cpp src/Wayland/WaylandSurfaces.cpp src/Wayland/WaylandDmabuf.cpp src/Wayland/WaylandData.cpp src/Wayland/WaylandScale.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp src/Math/FloatMath.cpp \
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
fi
[ $status -eq 0 ] && echo "wayland interop: all passed" || echo "wayland interop: FAILED"
exit $status
