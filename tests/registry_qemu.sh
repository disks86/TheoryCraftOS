#!/usr/bin/env bash
# The registry of Windows programs outlasts a restart, on the real kernel under QEMU: tests/windows/regpersist.c is the autorun, and the same disk
# image boots three times. QEMU is killed after each boot (no shutdown: what is on the disk is what the writer thread or RegFlushKey put there).
# Run 1 must see 0 runs before, run 2 one, run 3 two; each must find the volatile key of the run before gone. The kernel must say it read the
# registry back on boots 2 and 3. Needs x86_64-w64-mingw32-gcc. Uses its own build folder (build-registry-qemu) so the normal build keeps no autorun.
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-registry-qemu}
work=${TMPDIR:-/tmp}/tcos-registry-qemu
rm -rf "$work"; mkdir -p "$work"
x86_64-w64-mingw32-gcc -O1 -o "$work/regpersist.exe" tests/windows/regpersist.c -ladvapi32
cmake -S . -B "$build" -G Ninja -DTCOS_AUTORUN=/Windows/regpersist.exe -DTCOS_AUTORUN_ARGUMENTS= -DTCOS_AUTORUN_ELEVATED=ON >/dev/null
cmake --build "$build" >/dev/null
cmake --build "$build" --target vars >/dev/null
"$build/tools/primary_image" "$build/primary.img" "file:/Windows/regpersist.exe=$work/regpersist.exe" >/dev/null
qemu=$(ninja -C "$build" -t commands run-std | tail -1)
qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
status=0
for run in 1 2 3; do
  rm -f "$build/debugcon.log"
  timeout "${QEMU_SECONDS:-90}" bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
  cp "$build/debugcon.log" "$work/boot$run.log" 2>/dev/null || true
  echo "-- boot $run"
  grep -a -E "regpersist:|autorun:|Registry|PANIC" "$build/debugcon.log" || echo "no output"
  grep -a -q "regpersist: run $run (HKCU says $((run - 1)) runs before, HKLM $((run - 1)))" "$build/debugcon.log" || status=1
  grep -a -q "regpersist: all [0-9]* checks passed" "$build/debugcon.log" || status=1
  grep -a -q "autorun: ended, result 0, exit code 0x0*2a " "$build/debugcon.log" || status=1
  if [ $run -gt 1 ]; then grep -a -q "Registry.*read back from" "$build/debugcon.log" || status=1; fi
done
[ $status -eq 0 ] && echo "registry qemu: passed" || echo "registry qemu: FAILED"
exit $status
