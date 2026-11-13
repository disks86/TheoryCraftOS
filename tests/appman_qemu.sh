#!/usr/bin/env bash
# The App Manager's whole path in the real kernel under QEMU, on the real network: a build with TCOS_APPMAN_SELFTEST=<app> (default appimagetool) fetches
# AppMan's catalog over https once the network is up, installs the application with the kernel's HTTP client (GitHub's API, the release redirect, the
# download streamed onto the primary disk), and reports to the debug console, the Start menu entry included. Passes if the install did.
# Uses its own build folder (build-appman-qemu). Needs QEMU's user network to reach the internet.
set -euo pipefail
cd "$(dirname "$0")/.."
app=${1:-appimagetool}
build=${BUILD:-build-appman-qemu}
cmake -S . -B "$build" -G Ninja "-DTCOS_APPMAN_SELFTEST=$app" >/dev/null
cmake --build "$build" >/dev/null
cmake --build "$build" --target vars >/dev/null
qemu=$(ninja -C "$build" -t commands run-std | tail -1)
qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
rm -f "$build/debugcon.log"
timeout "${QEMU_SECONDS:-240}" bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
grep -a -E "appman-selftest:|DHCP|Network.*address" "$build/debugcon.log" | head -40 || true
grep -a -q "appman-selftest: PASSED" "$build/debugcon.log"
