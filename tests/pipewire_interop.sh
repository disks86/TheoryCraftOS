#!/usr/bin/env bash
# The kernel's PipeWire (src/PipeWire) against the real PipeWire and PulseAudio client libraries: tests/pipewire/PipeWireHostServer.cpp serves the graph,
# the native protocol and the PulseAudio protocol on the host's AF_UNIX sockets, then the host's own pw-dump, pw-cli, wpctl, pactl and paplay are pointed
# at them (PIPEWIRE_REMOTE, PULSE_SERVER). Two paplay runs play at once; the server's mixed output must hold both tones at the right levels.
# Usage: tests/pipewire_interop.sh     (needs pipewire's and pulseaudio's client tools and python3; skipped without them)
set -euo pipefail
cd "$(dirname "$0")/.."
for tool in pw-dump pw-cli pactl paplay python3; do
  command -v "$tool" >/dev/null || { echo "skipped (no $tool)"; exit 0; }
done
work=${TMPDIR:-/tmp}/tcos-pwinterop
# (sun_path holds 108 bytes: the sockets get a short folder of their own)
sock=$(mktemp -d /tmp/tcos-pw.XXXXXX)
mkdir -p "$work/obj"
trap 'pkill -TERM -f "$work/pwhost $sock" 2>/dev/null || true; rm -rf "$sock"' EXIT
sources=(tests/pipewire/PipeWireHostServer.cpp src/PipeWire/Graph.cpp src/PipeWire/NativeServer.cpp src/PipeWire/PlaybackStream.cpp src/PipeWire/Pod.cpp
         src/PipeWire/PulseServer.cpp src/PipeWire/Tagstruct.cpp src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Kernel/Sync.cpp
         tests/KernelStubs.cpp)
printf '%s\n' "${sources[@]}" | xargs -P "$(nproc)" -I{} sh -c 'g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src -w -c "{}" -o "'"$work"'/obj/$(echo "{}" | tr / _).o"'
g++ -fsanitize=address,undefined -pthread "$work"/obj/*.o -o "$work/pwhost"

fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
"$work/pwhost" "$sock/pipewire-0" "$sock/native" "$work/out.wav" 120 2> "$work/server.log" &
for _ in $(seq 50); do [ -S "$sock/native" ] && break; sleep 0.1; done
export PIPEWIRE_REMOTE=$sock/pipewire-0 PULSE_SERVER=unix:$sock/native

python3 - "$work" <<'PY'
import math, struct, sys, wave
work = sys.argv[1]
for name, freq, rate, channels, amp in (("a", 440, 44100, 2, 8000), ("b", 660, 22050, 1, 6000)):
    w = wave.open(f"{work}/{name}.wav", "wb"); w.setnchannels(channels); w.setsampwidth(2); w.setframerate(rate)
    frames = b"".join(struct.pack("<h", int(amp * math.sin(2 * math.pi * freq * i / rate))) * channels for i in range(int(rate * 1.5)))
    w.writeframes(frames); w.close()
PY

timeout 10 pw-dump > "$work/dump.json" 2> "$work/dump.err" || true
check "pw-dump reads the graph" "python3 -c \"
import json; d = json.load(open('$work/dump.json'))
sink = [o for o in d if o['type'] == 'PipeWire:Interface:Node' and o['info']['props'].get('media.class') == 'Audio/Sink'][0]
ports = [o for o in d if o['type'] == 'PipeWire:Interface:Port']
assert sink['info']['params']['Props'][0]['channelVolumes'] == [1.0, 1.0]
assert sink['info']['params']['EnumFormat'][0]['format'] == 'S16LE'
assert len(ports) == 2 and all(p['info']['params']['Format'] for p in ports)
assert any(o['type'] == 'PipeWire:Interface:Metadata' and o['metadata'][0]['key'] == 'default.audio.sink' for o in d)
\""
check "pw-dump reports no protocol error" "! grep -q 'invalid message' '$work/dump.err'"
check "pw-cli lists the sink" "timeout 5 pw-cli ls Node | grep -q 'node.name = \"tcos_output.stereo\"'"
if command -v wpctl >/dev/null; then check "wpctl status shows the sink" "timeout 5 wpctl status | grep -q 'Host test speakers'"; fi
check "pactl info" "timeout 5 pactl info | grep -q 'Default Sink: tcos_output.stereo'"
check "pactl list sinks" "timeout 5 pactl list sinks | grep -q 'Name: tcos_output.stereo'"
check "pactl set-sink-volume" "timeout 5 pactl set-sink-volume @DEFAULT_SINK@ 50% && timeout 5 pactl get-sink-volume @DEFAULT_SINK@ | grep -q ' 50%'"
check "pactl set-sink-mute" "timeout 5 pactl set-sink-mute @DEFAULT_SINK@ 1 && timeout 5 pactl get-sink-mute @DEFAULT_SINK@ | grep -q yes && timeout 5 pactl set-sink-mute @DEFAULT_SINK@ 0"
timeout 5 pactl set-sink-volume @DEFAULT_SINK@ 100%
(timeout 4 pactl subscribe > "$work/subscribe.txt" 2>&1 || true) &
(sleep 0.7; timeout 5 pactl list sink-inputs > "$work/inputs.txt" 2>&1 || true) &
sleep 0.3
start=$(date +%s%N)
timeout 15 paplay "$work/a.wav" & a=$!
timeout 15 paplay "$work/b.wav" & b=$!
check "paplay (44.1 kHz stereo)" "wait $a"
check "paplay (22.05 kHz mono) at the same time" "wait $b"
elapsed=$(( ($(date +%s%N) - start) / 1000000 ))
check "played in real time (${elapsed} ms for 1.5 s)" "[ $elapsed -ge 1300 ] && [ $elapsed -le 4000 ]"
wait
check "pactl list sink-inputs saw both" "[ \$(grep -c 'application.name = \"paplay\"' '$work/inputs.txt') -eq 2 ]"
check "pactl subscribe saw the streams come and go" "grep -q \"'new' on sink-input\" '$work/subscribe.txt' && grep -q \"'remove' on sink-input\" '$work/subscribe.txt'"
pkill -TERM -f "$work/pwhost $sock" || true
for _ in $(seq 50); do pgrep -f "$work/pwhost $sock" >/dev/null || break; sleep 0.1; done
check "the mixed output holds both tones" "python3 - '$work/out.wav' <<'PY'
import cmath, math, struct, sys, wave
w = wave.open(sys.argv[1]); n = w.getnframes(); s = struct.unpack('<%dh' % (2 * n), w.readframes(n)); left = s[0::2]
loud = [i for i, v in enumerate(left) if abs(v) > 200]
assert len(loud) > 48000, len(loud)
mid = loud[len(loud) // 2]; seg = left[mid - 4800: mid + 4800]
mag = lambda f: abs(sum(v * cmath.exp(-2j * math.pi * f * k / 48000) for k, v in enumerate(seg))) / len(seg)
a, b, off = mag(440), mag(660), mag(1000)
print('   440 Hz %.0f, 660 Hz %.0f, 1 kHz %.0f' % (a, b, off))
assert 3700 < a < 4300 and 1900 < b < 2350 and off < 100      # 8000/2, and 6000 * 0.707 (a mono stream at the centre) / 2
PY"
grep -q "ERROR: AddressSanitizer\|runtime error" "$work/server.log" && { echo "FAIL sanitizer report in the server"; fail=1; }
exit $fail
