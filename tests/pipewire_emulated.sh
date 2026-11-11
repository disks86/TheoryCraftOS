#!/usr/bin/env bash
# The kernel's PipeWire reached by real Linux and Windows programs through the OS's own system call layers, mixing with a native stream: on the kernel's
# CPU emulator (tests/EmulationRun.cpp built with -DTCOS_EMU_AUDIO: the graph, its AF_UNIX sockets, NtTcosAudio, a mixer whose device keeps what it
# plays, and a native 300 Hz stream) tests/windows/winmmtest.c (winmm.dll's waveOut and PlaySound, 400 Hz) and tests/linux/pulsetest.c (the
# PulseAudio protocol over socket(2), 500 Hz; no C library) run at the same time. Both must pass their checks, and the mixed output must hold all three
# tones at once.
# Usage: tests/pipewire_emulated.sh [x86_64|aarch64|riscv64]   (the Linux program's CPU, default x86_64). Needs mingw, clang and rust's ld.lld.
set -euo pipefail
cd "$(dirname "$0")/.."
arch=${1:-x86_64}
work=${TMPDIR:-/tmp}/tcos-pwemu
mkdir -p "$work/obj" "$work/sys32"
for tool in x86_64-w64-mingw32-gcc clang; do command -v "$tool" >/dev/null || { echo "skipped (no $tool)"; exit 0; }; done
sources=(tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Crypto/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp
         src/Kernel/Sync.cpp tests/KernelStubs.cpp src/PipeWire/*.cpp src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp)
flags="-std=c++20 -O1 -pthread -I src -w -DTCOS_EMU_AUDIO"
objs=()
jobs=()
for f in "${sources[@]}"; do
  o="$work/obj/$(echo "$f" | tr '/' '_').o"
  objs+=("$o")
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$(find src -name '*.hpp' -newer "$o" -print -quit)" ]; then jobs+=("$f"); fi
done
if [ ${#jobs[@]} -gt 0 ]; then
  printf '%s\n' "${jobs[@]}" | xargs -P "$(nproc)" -I{} sh -c 'o="'"$work"'/obj/$(echo "{}" | tr / _).o"; g++ '"$flags"' -c "{}" -o "$o" || { rm -f "$o"; exit 255; }'
fi
g++ -pthread "${objs[@]}" -o "$work/emurun"

# the OS's DLLs (winmm.dll among them) and the Windows program
if [ ! -f "$work/build/build.ninja" ]; then
  cmake -S . -B "$work/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-x86_64-uefi.cmake" -DCMAKE_BUILD_TYPE=Debug > /dev/null
fi
ninja -C "$work/build" windows_dlls > /dev/null
cp "$work"/build/windows/*.dll "$work/sys32/"
x86_64-w64-mingw32-gcc -O1 -Wall -Wextra -o "$work/winmmtest.exe" tests/windows/winmmtest.c -lwinmm
# the Linux program
lld=$(ls "$HOME"/.rustup/toolchains/*/lib/rustlib/*/bin/gcc-ld/ld.lld 2>/dev/null | head -1 || true)
case $arch in
  aarch64) extra=(-mno-outline-atomics) ;;
  riscv64) extra=(-march=rv64gc -mno-relax) ;;
  *) extra=() ;;
esac
linker=(-fuse-ld=lld)
[ -n "$lld" ] && linker=("--ld-path=$lld")
clang --target="$arch-linux-gnu" "${extra[@]}" -O1 -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables "${linker[@]}" \
  -Wall -Wextra -o "$work/pulsetest-$arch" tests/linux/pulsetest.c

"$work/emurun" "$work/winmmtest.exe" "$work/sys32" "$work/pulsetest-$arch" | tee "$work/run.out"
fail=0
grep -q "winmmtest: [0-9]* checks, 0 failed" "$work/run.out" || { echo "FAIL winmmtest"; fail=1; }
grep -q "pulsetest: [0-9]* checks, 0 failed" "$work/run.out" || { echo "FAIL pulsetest"; fail=1; }
grep -q "faulted=0" "$work/run.out" && grep -q "second program: exit=0" "$work/run.out" || { echo "FAIL a program faulted or failed"; fail=1; }
# all three tones at once: native 300 Hz (5000 stereo: 2500), Windows 400 Hz (8000: 4000), Linux 500 Hz (6000 mono at the centre: ~2121)
python3 - "$work/run.out" <<'PY' || fail=1
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r"native 300 Hz (\d+), windows 400 Hz (\d+), linux 500 Hz (\d+); all three at once (\d+)", text)
assert m, "no audio summary"
native, windows, linux, together = map(int, m.groups())
ok = 2200 < native < 2800 and 3600 < windows < 4400 and 1800 < linux < 2400 and together > 1500
print("ok   the mix holds all three programs' tones" if ok else "FAIL the mix: %s" % m.group(0))
sys.exit(0 if ok else 1)
PY
[ $fail -eq 0 ] && echo "pipewire emulated: all passed" || echo "pipewire emulated: FAILED"
exit $fail
