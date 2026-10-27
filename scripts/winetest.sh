#!/usr/bin/env bash
# TheoryCraftOS - a minimal UEFI x86_64 operating system.
# Copyright (C) 2026 Christopher Schaefer
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Runs Wine's tests (tests/windows/wine) of some DLLs in QEMU and summarises the result.
#   scripts/winetest.sh <dll> [test ...]     e.g. scripts/winetest.sh ntdll large_int rtl
# Builds the tests into build/winetests, boots the OS headless with tests/windows/winerun.exe as the program (TCOS_WINE_TEST_PROGRAM) and the tests next to it, and writes
# the output to build/winetests/<dll>.log and a summary (checks failed per test) to the terminal. Do not run two at once: they share the build directory.
set -euo pipefail
cd "$(dirname "$0")/.."
dll=$1; shift
out=build/winetests
mkdir -p "$out"
python3 tests/windows/wine.py build "$out" "$dll" >/dev/null
cc=${CC:-x86_64-w64-mingw32-gcc}
$cc -O1 -o "$out/winerun.exe" tests/windows/winerun.c
if [ $# -gt 0 ]; then tests=("$@"); else tests=($(cd "$out" && ls "${dll}"_*.exe | sed "s/^${dll}_//; s/\.exe$//")); fi
# Runs the tests in rounds: a test that hangs the whole machine (a stalled debug console for 90 s) is recorded and the round restarts with the tests after it.
pending=()
for t in "${tests[@]}"; do pending+=("$t"); done
: > "$out/$dll.log"
round=0
while [ ${#pending[@]} -gt 0 ] && [ $round -lt 40 ]; do
  round=$((round+1))
  files=()
  : > "$out/winerun.txt"
  [ -n "${WINETEST_ENV:-}" ] && echo "!$WINETEST_ENV" >> "$out/winerun.txt"
  names=()
  for t in "${pending[@]}"; do
    # "name@VAR=value" runs the test with an environment variable set (WINETEST_DEBUG=2 for traces, or one a test of your own reads)
    env_part=""; name=$t; case "$t" in *@*) env_part=${t#*@}; name=${t%%@*};; esac
    [ -f "$out/${dll}_$name.exe" ] || { echo "no program for $dll $name"; continue; }
    case " ${files[*]:-} " in *" $PWD/$out/${dll}_$name.exe "*) ;; *) files+=("$PWD/$out/${dll}_$name.exe");; esac
    [ -n "$env_part" ] && echo "!$env_part" >> "$out/winerun.txt"
    echo "${dll}_$name.exe" >> "$out/winerun.txt"
    names+=("$name")
  done
  [ ${#names[@]} -eq 0 ] && break
  files+=("$PWD/$out/winerun.txt")
  cat > "$out/qemu.sh" <<'QEMU'
#!/bin/sh
# Runs QEMU without a display until the program prints WINERUN DONE, the debug console is silent for 90 s, or 40 minutes pass; then stops it.
log=$(dirname "$0")/../debugcon.log
/usr/bin/qemu-system-x86_64 -display none -enable-kvm "$@" >/dev/null 2>&1 &
pid=$!
n=0; quiet=0; last=0
while kill -0 $pid 2>/dev/null && [ $n -lt 2400 ]; do
  grep -q "WINERUN DONE" "$log" 2>/dev/null && break
  size=$(stat -c %s "$log" 2>/dev/null || echo 0)
  if grep -q "=== .* start" "$log" 2>/dev/null; then
    if [ "$size" = "$last" ]; then quiet=$((quiet+1)); else quiet=0; last=$size; fi
    [ $quiet -ge 90 ] && break
  fi
  sleep 1; n=$((n+1))
done
kill $pid 2>/dev/null
wait $pid 2>/dev/null
exit 0
QEMU
  chmod +x "$out/qemu.sh"
  : > build/debugcon.log
  cmake -S . -B build -DTCOS_USERMODE_DEMO=OFF -DTCOS_WINE_TEST_PROGRAM="$PWD/$out/winerun.exe" "-DTCOS_WINE_TEST_FILES=$(IFS=';'; echo "${files[*]}")" -DQEMU="$PWD/$out/qemu.sh" >/dev/null
  ninja -C build esp primary vars >"$out/build.log" 2>&1 || { tail -20 "$out/build.log"; exit 1; }
  ninja -C build run-std >/dev/null 2>&1 || true
  sed -n '/Windows test program/,$p' build/debugcon.log | grep -v "^\[" | grep -v "^  \(test program\|Windows test program\)" >> "$out/$dll.log"
  # the tests that did not start in this round are the next round's; the one that started and never ended is recorded as hung
  last_started=$(grep "^=== .* start" build/debugcon.log | tail -1 | sed 's/^=== \(.*\) start$/\1/')
  if grep -q "WINERUN DONE" build/debugcon.log; then pending=(); break; fi
  if [ -z "$last_started" ]; then echo "round $round: nothing started"; break; fi
  echo "=== $last_started HUNG (the machine stopped answering)" >> "$out/$dll.log"
  rest=(); seen=0
  for t in "${pending[@]}"; do
    name=${t%%@*}
    if [ $seen -eq 1 ]; then rest+=("$t"); fi
    [ "${dll}_$name.exe" = "$last_started" ] && seen=1
  done
  pending=("${rest[@]:-}")
  [ "${pending[0]:-}" = "" ] && pending=()
done
# a summary: per test, the failed checks (lines "Test failed"), the todo that failed, and the exit state
awk '/^=== .* start/{name=$2} /Test failed|Test succeeded inside todo/{f[name]++} /^[^ ]*:[0-9]+:.*Tests? (failed|succeeded)/{} /^=== .* (exit|timeout|did not|HUNG)/{st[$2]=$0} END{for(n in st) printf "%-40s failed checks %-5d %s\n", n, f[n], st[n]}' "$out/$dll.log" | sort
grep -c "WINERUN DONE" "$out/$dll.log" | sed 's/^1$/finished/; s/^0$/DID NOT FINISH/'
