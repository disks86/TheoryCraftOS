#!/usr/bin/env bash
# The App Manager's Windows side in the real kernel under QEMU, on the real network: a build with TCOS_WINGET_SELFTEST=<package> (default jqlang.jq, a
# portable program; try 7zip.7zip for a setup program) fetches winget's index over https once the network is up, installs the package with the kernel's
# HTTP client - a setup program is run in the Windows layer - reports what the Start menu lists, and removes the package again. Passes if the install did.
# Uses its own build folder (build-winget-qemu). Needs QEMU's user network to reach the internet.
# IMAGE_SIZE=2G makes the disk image anew with that size (the default image is 256 MiB, which a large package does not fit: qBittorrent.qBittorrent,
# whose setup program is a 32-bit Nullsoft one, needs about 400 MiB).
set -euo pipefail
cd "$(dirname "$0")/.."
package=${1:-jqlang.jq}
build=${BUILD:-build-winget-qemu}
if [ -n "${IMAGE_SIZE:-}" ]; then
  rm -f "$build/primary.img"
  cmake -S . -B "$build" -G Ninja "-DTCOS_WINGET_SELFTEST=$package" "-DTCOS_PRIMARY_IMAGE_SIZE=$IMAGE_SIZE" >/dev/null
else
  cmake -S . -B "$build" -G Ninja "-DTCOS_WINGET_SELFTEST=$package" >/dev/null
fi
cmake --build "$build" >/dev/null
cmake --build "$build" --target vars >/dev/null
qemu=$(ninja -C "$build" -t commands run-std | tail -1)
qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
rm -f "$build/debugcon.log"
timeout "${QEMU_SECONDS:-300}" bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
grep -a -E "winget-selftest:|DHCP|Network.*address" "$build/debugcon.log" | head -40 || true
grep -a -q "winget-selftest: PASSED" "$build/debugcon.log"
