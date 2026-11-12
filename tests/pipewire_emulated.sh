#!/usr/bin/env bash
# The kernel's PipeWire reached by real Linux and Windows programs through the OS's own system call layers, mixing with a native stream: on the kernel's
# CPU emulator (tests/EmulationRun.cpp built with -DTCOS_EMU_AUDIO: the graph, its AF_UNIX sockets, NtTcosAudio, a mixer whose device keeps what it
# plays, and a native 300 Hz stream) tests/windows/winmmtest.c (winmm.dll's waveOut and PlaySound, 400 Hz) and tests/linux/pulsetest.c (the
# PulseAudio protocol over socket(2), 500 Hz; no C library) run at the same time. Both must pass their checks, and the mixed output must hold all three
# tones at once. A second run does the same with tests/windows/dsoundtest.c (dsound.dll at 600 Hz and WASAPI at 700 Hz, both from one program): all of
# 300, 500, 600 and 700 Hz must be in the mix at once. A third run records: the emulator's microphone (880 Hz left, 1320 Hz right) is heard by
# tests/windows/recordtest.c (waveIn, DirectSound capture, WASAPI capture) and by pulsetest's record stream at the same time. A fourth run plays
# tests/windows/xaudio2test.c (XAudio2 2.9: 400 Hz from a source voice, 600 Hz through a submix): 300, 400, 500 and 600 Hz must be in the mix at once.
# A fifth run plays winmmtest next to tests/linux/pwnodetest.c (libpipewire's pw_stream side of client-node over pipewire-0: memfds and eventfds passed
# with SCM_RIGHTS, mmap, ppoll; 500 Hz, then recording the microphone): 300, 400 and 500 Hz must be in the mix at once.
# A sixth run (x86-64, when the host has pw-cat) runs the host's real libpipewire: pw-cat (pw-play) and its libraries, SPA plugins, modules and
# client.conf copied into the Linux tree, dynamically linked through glibc's ld.so. It plays a 500 Hz WAV next to the native stream (both must be in the
# mix at once), then records 24000 frames of the microphone into a WAV that is copied out and checked (880 Hz left, 1320 Hz right). Then pw-cat --midi
# plays a MIDI file (all 12 notes must reach the server's synthesizer), and tests/linux/pwmedia.c (built against the host's libpipewire) runs each of its
# modes: a driver camera and a watcher (video), the same with the camera's own memfd buffers (video-alloc), a pw_filter's MIDI heard by a pw_stream
# (midi), and a virtual sink of the adapter factory played into by an audio driver and recorded from (adapter).
# Usage: tests/pipewire_emulated.sh [x86_64|aarch64|riscv64] [all|winmm|dsound|record|xaudio2|pwnode|libpipewire]   (the Linux program's CPU, default x86_64; which runs, default all).
# Needs mingw, clang and rust's ld.lld.
set -euo pipefail
cd "$(dirname "$0")/.."
arch=${1:-x86_64}
which=${2:-all}
# (one work folder per checkout: the objects and the DLL build of another worktree must not be taken for this one's)
work=${TMPDIR:-/tmp}/tcos-pwemu-$(printf %s "$PWD" | cksum | cut -d" " -f1)
mkdir -p "$work/obj" "$work/sys32"
for tool in x86_64-w64-mingw32-gcc clang; do command -v "$tool" >/dev/null || { echo "skipped (no $tool)"; exit 0; }; done
sources=(tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Crypto/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp
         src/Kernel/Sync.cpp tests/KernelStubs.cpp src/PipeWire/*.cpp src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/CaptureStream.cpp src/Audio/AudioCaptureHub.cpp src/Hardware/AudioDevice/CaptureRing.cpp)
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
x86_64-w64-mingw32-gcc -O1 -Wall -Wextra -o "$work/dsoundtest.exe" tests/windows/dsoundtest.c -ldsound -lole32 -luuid
x86_64-w64-mingw32-gcc -O1 -Wall -Wextra -o "$work/recordtest.exe" tests/windows/recordtest.c -lwinmm -ldsound -lole32 -luuid
x86_64-w64-mingw32-gcc -O1 -Wall -Wextra -o "$work/xaudio2test.exe" tests/windows/xaudio2test.c -L"$work/build/windows" -l:xaudio2_9.dll -lole32 -luuid
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
clang --target="$arch-linux-gnu" "${extra[@]}" -O1 -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables "${linker[@]}" \
  -Wall -Wextra -o "$work/pwnodetest-$arch" tests/linux/pwnodetest.c

fail=0
if [ "$which" = all ] || [ "$which" = winmm ]; then
  "$work/emurun" "$work/winmmtest.exe" "$work/sys32" "$work/pulsetest-$arch" | tee "$work/run.out"
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
fi
if [ "$which" = all ] || [ "$which" = dsound ]; then
  TCOS_EMU_TONES=300,500,600,700 "$work/emurun" "$work/dsoundtest.exe" "$work/sys32" "$work/pulsetest-$arch" | tee "$work/run-dsound.out"
  grep -q "dsoundtest: [0-9]* checks, 0 failed" "$work/run-dsound.out" || { echo "FAIL dsoundtest"; fail=1; }
  grep -q "pulsetest: [0-9]* checks, 0 failed" "$work/run-dsound.out" || { echo "FAIL pulsetest (with dsoundtest)"; fail=1; }
  grep -q "faulted=0" "$work/run-dsound.out" && grep -q "second program: exit=0" "$work/run-dsound.out" || { echo "FAIL a program faulted or failed (dsound run)"; fail=1; }
  # DirectSound 600 Hz (8000 mono, both sides full: 4000), WASAPI 700 Hz (0.25: ~4096), with the native 300 Hz and the Linux 500 Hz, all at once
  python3 - "$work/run-dsound.out" <<'PY' || fail=1
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r"tones 300:(\d+) 400:(\d+) 500:(\d+) 600:(\d+) 700:(\d+); together (\d+)", text)
assert m, "no audio summary"
t300, t400, t500, t600, t700, together = map(int, m.groups())
ok = 2200 < t300 < 2800 and 1800 < t500 < 2400 and 3600 < t600 < 4400 and 3600 < t700 < 4500 and together > 1500
print("ok   the mix holds DirectSound's and WASAPI's tones with the others" if ok else "FAIL the mix: %s" % m.group(0))
sys.exit(0 if ok else 1)
PY
fi
if [ "$which" = all ] || [ "$which" = record ]; then
  TCOS_EMU_TONES=300,500 "$work/emurun" "$work/recordtest.exe" "$work/sys32" "$work/pulsetest-$arch" | tee "$work/run-record.out"
  grep -q "recordtest: [0-9]* checks, 0 failed" "$work/run-record.out" || { echo "FAIL recordtest"; fail=1; }
  grep -q "pulsetest: [0-9]* checks, 0 failed" "$work/run-record.out" || { echo "FAIL pulsetest (with recordtest)"; fail=1; }
  grep -q "faulted=0" "$work/run-record.out" && grep -q "second program: exit=0" "$work/run-record.out" || { echo "FAIL a program faulted or failed (record run)"; fail=1; }
fi
if [ "$which" = all ] || [ "$which" = xaudio2 ]; then
  TCOS_EMU_TONES=300,400,500,600 "$work/emurun" "$work/xaudio2test.exe" "$work/sys32" "$work/pulsetest-$arch" | tee "$work/run-xaudio2.out"
  grep -q "xaudio2test: [0-9]* checks, 0 failed" "$work/run-xaudio2.out" || { echo "FAIL xaudio2test"; fail=1; }
  grep -q "pulsetest: [0-9]* checks, 0 failed" "$work/run-xaudio2.out" || { echo "FAIL pulsetest (with xaudio2test)"; fail=1; }
  grep -q "faulted=0" "$work/run-xaudio2.out" && grep -q "second program: exit=0" "$work/run-xaudio2.out" || { echo "FAIL a program faulted or failed (xaudio2 run)"; fail=1; }
  # XAudio2's 400 Hz and 600 Hz (8000 and 0.25 into a mono mastering voice, at the centre: ~2828 and ~2896), with the native 300 Hz and the Linux 500 Hz
  python3 - "$work/run-xaudio2.out" <<'PY' || fail=1
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r"tones 300:(\d+) 400:(\d+) 500:(\d+) 600:(\d+) 700:(\d+); together (\d+)", text)
assert m, "no audio summary"
t300, t400, t500, t600, t700, together = map(int, m.groups())
ok = 2200 < t300 < 2800 and 2400 < t400 < 3200 and 1800 < t500 < 2400 and 2400 < t600 < 3300 and together > 1500
print("ok   the mix holds XAudio2's tones with the others" if ok else "FAIL the mix: %s" % m.group(0))
sys.exit(0 if ok else 1)
PY
fi
if [ "$which" = all ] || [ "$which" = pwnode ]; then
  "$work/emurun" "$work/winmmtest.exe" "$work/sys32" "$work/pwnodetest-$arch" | tee "$work/run-pwnode.out"
  grep -q "winmmtest: [0-9]* checks, 0 failed" "$work/run-pwnode.out" || { echo "FAIL winmmtest (with pwnodetest)"; fail=1; }
  grep -q "pwnodetest: [0-9]* checks, 0 failed" "$work/run-pwnode.out" || { echo "FAIL pwnodetest"; fail=1; }
  grep -q "faulted=0" "$work/run-pwnode.out" && grep -q "second program: exit=0" "$work/run-pwnode.out" || { echo "FAIL a program faulted or failed (pwnode run)"; fail=1; }
  # the client-node's 500 Hz (0.1295 on each side: the same in the mix as pulsetest's) with the native 300 Hz and winmm's 400 Hz, all at once
  python3 - "$work/run-pwnode.out" <<'PY' || fail=1
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r"native 300 Hz (\d+), windows 400 Hz (\d+), linux 500 Hz (\d+); all three at once (\d+)", text)
assert m, "no audio summary"
native, windows, linux, together = map(int, m.groups())
ok = 2200 < native < 2800 and 3600 < windows < 4400 and 1800 < linux < 2400 and together > 1500
print("ok   the mix holds the client-node's tone with the others" if ok else "FAIL the mix: %s" % m.group(0))
sys.exit(0 if ok else 1)
PY
fi
if { [ "$which" = all ] || [ "$which" = libpipewire ]; } && [ "$arch" = x86_64 ]; then
  if ! command -v pw-cat >/dev/null || [ ! -f /usr/share/pipewire/client.conf ]; then
    echo "skipped libpipewire (no pw-cat on this machine)"
  else
    # the files pw-cat needs: its interpreter, every library ldd finds, the plugins and modules it loads, its configuration
    pwcat=$(command -v pw-cat)
    extra=()
    for m in protocol-native client-node client-device adapter metadata rt session-manager; do extra+=("/usr/lib/pipewire-0.3/libpipewire-module-$m.so"); done
    for l in support/libspa-support support/libspa-dbus support/libspa-journal audioconvert/libspa-audioconvert videoconvert/libspa-videoconvert control/libspa-control; do
      extra+=("/usr/lib/spa-0.2/$l.so")
    done
    python3 - "$work/tone500.wav" <<'PY'
import math, struct, sys
r, n = 48000, 48000 * 3
d = b"".join(struct.pack("<hh", int(4243 * math.sin(2 * math.pi * 500 * i / r)), int(4243 * math.sin(2 * math.pi * 500 * i / r))) for i in range(n))
open(sys.argv[1], "wb").write(b"RIFF" + struct.pack("<I", 36 + len(d)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, r, r * 4, 4, 16) + b"data" + struct.pack("<I", len(d)) + d)
PY
    files=$( { ldd "$pwcat" | awk '/=>/ { if ($3 ~ /^\//) print $3 } /^\t\// { print $1 }'
               for l in "${extra[@]}"; do [ -f "$l" ] && { ldd "$l" | awk '/=>/ { if ($3 ~ /^\//) print $3 }'; echo "$l"; }; done
               readelf -l "$pwcat" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p'
               echo /usr/share/pipewire/client.conf; echo "$work/tone500.wav"; } | sort -u | paste -sd:)
    have_pwmedia=0
    if pkg-config --exists libpipewire-0.3; then
      gcc -O1 -g -Wall -Wextra -o "$work/pwmedia" tests/linux/pwmedia.c $(pkg-config --cflags --libs libpipewire-0.3) -lm && have_pwmedia=1
    fi
    python3 - "$work/song.mid" <<'PY'
import struct, sys
ev = b"".join(b"\x00" + bytes([0x90, 60 + i, 100]) + b"\x60" + bytes([0x80, 60 + i, 0]) for i in range(12)) + b"\x00\xff\x2f\x00"
open(sys.argv[1], "wb").write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(ev)) + ev)
PY
    files="$files:$work/song.mid"
    if [ $have_pwmedia -eq 1 ]; then
      files="$files:$( { ldd "$work/pwmedia" | awk '/=>/ { if ($3 ~ /^\//) print $3 }'; } | sort -u | paste -sd:)"
    fi
    TCOS_EMU_TONES=300,500 TCOS_EMU_ARGS="--playback $work/tone500.wav" TCOS_EMU_LINUX_FILES="$files" "$work/emurun" "$pwcat" "$work/sys32" > "$work/run-pwplay.out" 2>&1 || true
    grep -E "^\[E\]" "$work/run-pwplay.out" | grep -v mod.rt || true
    grep -E "^audio: native|exit=" "$work/run-pwplay.out"
    grep -q "\[exit=0 faulted=0" "$work/run-pwplay.out" || { echo "FAIL pw-play (libpipewire) did not end well"; fail=1; }
    python3 - "$work/run-pwplay.out" <<'PY' || fail=1
import re, sys
m = re.search(r"tones 300:(\d+) 400:(\d+) 500:(\d+) 600:(\d+) 700:(\d+); together (\d+)", open(sys.argv[1]).read())
assert m, "no audio summary"
t300, t500, together = int(m.group(1)), int(m.group(3)), int(m.group(6))
ok = 2200 < t300 < 2800 and 1800 < t500 < 2400 and together > 1500
print("ok   libpipewire's pw-play is in the mix with the native stream" if ok else "FAIL the mix: %s" % m.group(0))
sys.exit(0 if ok else 1)
PY
    rm -f "$work/pwrecord.wav"
    TCOS_EMU_SAVE="/tmp/pwrecord.wav=$work/pwrecord.wav" TCOS_EMU_ARGS="--record -n 24000 /tmp/pwrecord.wav" TCOS_EMU_LINUX_FILES="$files" "$work/emurun" "$pwcat" "$work/sys32" > "$work/run-pwrecord.out" 2>&1 || true
    grep -q "faulted=0" "$work/run-pwrecord.out" || { echo "FAIL pw-record (libpipewire) faulted"; fail=1; }      # (pw-cat -n ends with 1 on Linux too)
    python3 - "$work/pwrecord.wav" <<'PY' || fail=1
import math, struct, sys, wave
try:
    w = wave.open(sys.argv[1])
except Exception as e:
    print("FAIL pw-record wrote nothing usable: %s" % e); sys.exit(1)
n, ch, r = w.getnframes(), w.getnchannels(), w.getframerate()
d = struct.unpack("<%dh" % (n * ch), w.readframes(n))
def amp(side, f, start=12000, cnt=4800):
    s1 = s2 = 0.0; c = 2 * math.cos(2 * math.pi * f / r)
    for i in range(start, start + cnt):
        s0 = d[i * ch + side] + c * s1 - s2; s2 = s1; s1 = s0
    return math.sqrt(max(0, s1 * s1 + s2 * s2 - c * s1 * s2)) * 2 / cnt
l880, r1320, l1320, r880 = amp(0, 880), amp(1, 1320), amp(0, 1320), amp(1, 880)
ok = n == 24000 and ch == 2 and 7000 < l880 < 9000 and 3400 < r1320 < 4600 and l1320 < 500 and r880 < 500
print("%s pw-record (libpipewire) recorded 880 Hz left %.0f, 1320 Hz right %.0f" % ("ok  " if ok else "FAIL", l880, r1320))
sys.exit(0 if ok else 1)
PY
    # MIDI: pw-cat --midi plays a song of 12 notes; every one reaches the synthesizer
    TCOS_EMU_ARGS="--playback --midi $work/song.mid" TCOS_EMU_LINUX_FILES="$files" "$work/emurun" "$pwcat" "$work/sys32" > "$work/run-pwmidi.out" 2>&1 || true
    grep -E "^midi:|exit=" "$work/run-pwmidi.out"
    grep -q "\[exit=0 faulted=0" "$work/run-pwmidi.out" && grep -q "midi: 24 messages to the synthesizer, 12 note-ons" "$work/run-pwmidi.out" &&
      echo "ok   pw-cat --midi (libpipewire) played its 12 notes on the synthesizer" || { echo "FAIL pw-cat --midi"; fail=1; }
    if [ $have_pwmedia -eq 1 ]; then
      for mode in video video-alloc midi adapter; do
        TCOS_EMU_ARGS="$mode" TCOS_EMU_LINUX_FILES="$files" "$work/emurun" "$work/pwmedia" "$work/sys32" > "$work/run-pwmedia-$mode.out" 2>&1 || true
        grep -E "^pwmedia:|exit=" "$work/run-pwmedia-$mode.out"
        grep -q "pwmedia: $mode ok" "$work/run-pwmedia-$mode.out" && grep -q "\[exit=0 faulted=0" "$work/run-pwmedia-$mode.out" || { echo "FAIL pwmedia $mode"; fail=1; }
      done
    else
      echo "skipped pwmedia (no libpipewire headers on this machine)"
    fi
  fi
fi
[ $fail -eq 0 ] && echo "pipewire emulated: all passed" || echo "pipewire emulated: FAILED"
exit $fail
