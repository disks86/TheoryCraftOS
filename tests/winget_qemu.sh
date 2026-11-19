#!/usr/bin/env bash
# The App Manager's Windows side in the real kernel under QEMU, on the real network: a build with TCOS_WINGET_SELFTEST=<package> (default jqlang.jq, a
# portable program; try 7zip.7zip for a setup program) fetches winget's index over https once the network is up, installs the package with the kernel's
# HTTP client - a setup program is run in the Windows layer - reports what the Start menu lists, and removes the package again. Passes if the install did.
# RUN_ARGS="--a|--b" gives them arguments; KEEP=ON leaves the package installed (read its files with build-winget-qemu/tools/primary_image).
# RUN=ON also starts each installed program (up to a minute each) before the removal and says how it ended.
# Uses its own build folder (build-winget-qemu). Needs QEMU's user network to reach the internet.
# IMAGE_SIZE=20G makes the disk image anew with that size (the default is 10 GiB; scripts/make-primary-image.sh grows a smaller image, keeping its files.
# A 256 MiB image does not fit large packages: qBittorrent.qBittorrent, whose setup program is a 32-bit Nullsoft one, needs about 400 MiB).
set -euo pipefail
cd "$(dirname "$0")/.."
package=${1:-jqlang.jq}
build=${BUILD:-build-winget-qemu}
if [ -n "${IMAGE_SIZE:-}" ]; then
  rm -f "$build/primary.img"
  cmake -S . -B "$build" -G Ninja "-DTCOS_WINGET_SELFTEST=$package" "-DTCOS_WINGET_SELFTEST_RUN=${RUN:-OFF}" "-DTCOS_WINGET_SELFTEST_RUN_ARGUMENTS=${RUN_ARGS:-}" "-DTCOS_WINGET_SELFTEST_KEEP=${KEEP:-OFF}" "-DTCOS_PRIMARY_IMAGE_SIZE=$IMAGE_SIZE" >/dev/null
else
  cmake -S . -B "$build" -G Ninja "-DTCOS_WINGET_SELFTEST=$package" "-DTCOS_WINGET_SELFTEST_RUN=${RUN:-OFF}" "-DTCOS_WINGET_SELFTEST_RUN_ARGUMENTS=${RUN_ARGS:-}" "-DTCOS_WINGET_SELFTEST_KEEP=${KEEP:-OFF}" >/dev/null
fi
cmake --build "$build" >/dev/null
cmake --build "$build" --target vars >/dev/null
qemu=$(ninja -C "$build" -t commands run-std | tail -1)
qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
rm -f "$build/debugcon.log"
timeout "${QEMU_SECONDS:-300}" bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
grep -a -E "winget-selftest:|DHCP|Network.*address" "$build/debugcon.log" | head -40 || true
grep -a -q "winget-selftest: PASSED" "$build/debugcon.log"
