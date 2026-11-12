#!/usr/bin/env bash
# Dynamically linked Linux programs on the OS's own system call layer: programs built with the development machine's compiler and C library (glibc, not
# static) run on the kernel's CPU emulator (tests/EmulationRun.cpp, the -DTCOS_EMU_WAYLAND build of tests/wayland_emulated.sh). The kernel maps the
# program and its interpreter (ld.so); ld.so loads the libraries by open and mmap. The host's interpreter and libraries are copied into the Linux tree at
# their own paths (TCOS_EMU_LINUX_FILES). Runs:
#   tests/linux/dynamictest.c   the C library itself (stdio, libm, malloc, threads, TLS, constructors, dlopen)
#   tests/wayland/wl_client.c   a libwayland-client + libxkbcommon client against the kernel's compositor (when the libraries are installed)
# Usage: tests/linux_dynamic_emulated.sh   (x86-64 only: the development machine has no AArch64 or RISC-V C library)
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-waylandemu
TMPDIR=${TMPDIR:-/tmp} tests/wayland_emulated.sh x86_64 > "$work.build.log" 2>&1 || { tail -20 "$work.build.log"; echo "the emulator build (or its own test) failed"; exit 1; }

# The files a program needs at run time: its interpreter and every library ldd resolves, plus extra (dlopen'ed) ones.
linux_files() {      # <program> [extra library ...]
  local program=$1; shift
  { ldd "$program" | awk '/=>/ { if ($3 ~ /^\//) print $3 } /^\t\// { print $1 }'
    for l in "$@"; do ldd "$l" | awk '/=>/ { if ($3 ~ /^\//) print $3 }'; echo "$l"; done
    readelf -l "$program" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p'
  } | sort -u | paste -sd:
}

fail=0
echo "== dynamictest (glibc)"
gcc -O1 -g -o "$work/dynamictest" tests/linux/dynamictest.c -lm
TCOS_EMU_LINUX_FILES=$(linux_files "$work/dynamictest") "$work/emurun" "$work/dynamictest" | tee "$work/dynamic.out"
grep -q "dynamic: .* 0 failed" "$work/dynamic.out" || fail=1
grep -q "faulted=0" "$work/dynamic.out" || fail=1

if [ -f /usr/include/wayland-client.h ] && command -v wayland-scanner >/dev/null; then
  echo "== wl_client (libwayland-client, libxkbcommon)"
  protocols=/usr/share/wayland-protocols
  for p in stable/xdg-shell/xdg-shell unstable/xdg-decoration/xdg-decoration-unstable-v1; do
    wayland-scanner client-header "$protocols/$p.xml" "$work/$(basename $p)-client-protocol.h"
    wayland-scanner private-code "$protocols/$p.xml" "$work/$(basename $p)-protocol.c"
  done
  gcc -O1 -g -I "$work" tests/wayland/wl_client.c "$work/xdg-shell-protocol.c" "$work/xdg-decoration-unstable-v1-protocol.c" -lwayland-client -lxkbcommon -o "$work/wl_client"
  # (WL_CLIENT_PARALLEL: no scripted input here, which only the host test server gives)
  TCOS_EMU_ENV="WL_CLIENT_PARALLEL=1 WL_CLIENT_SCREEN=1024x768" TCOS_EMU_LINUX_FILES=$(linux_files "$work/wl_client") "$work/emurun" "$work/wl_client" | tee "$work/wl_client.out"
  grep -q "checks, 0 failed" "$work/wl_client.out" || fail=1
  grep -q "faulted=0" "$work/wl_client.out" || fail=1
  grep -q 'framed window' "$work/wl_client.out" || { echo "the window did not reach the desktop"; fail=1; }
fi
[ $fail -eq 0 ] && echo "linux dynamic emulated: all passed" || echo "linux dynamic emulated: FAILED"
exit $fail
