#!/usr/bin/env bash
# The kernel's PipeWire (src/PipeWire) against the real PipeWire and PulseAudio client libraries: tests/pipewire/PipeWireHostServer.cpp serves the graph,
# the native protocol and the PulseAudio protocol on the host's AF_UNIX sockets, then the host's own pw-dump, pw-cli, wpctl, pactl and paplay are pointed
# at them (PIPEWIRE_REMOTE, PULSE_SERVER). Two paplay runs play at once; the server's mixed output must hold both tones at the right levels. The server's
# microphone hears 880 Hz on the left and 1320 Hz on the right: parecord (and pw-dump, pactl's source commands) must find the source and record both.
# libpipewire's own streams go through the client-node factory (shared memory and eventfds passed with SCM_RIGHTS): pw-play's tone must be in the mix
# and pw-record must record both sides of the microphone.
# Usage: tests/pipewire_interop.sh     (needs pipewire's and pulseaudio's client tools and python3; skipped without them)
set -euo pipefail
cd "$(dirname "$0")/.."
for tool in pw-dump pw-cli pactl paplay parecord pw-play pw-record python3; do
  command -v "$tool" >/dev/null || { echo "skipped (no $tool)"; exit 0; }
done
work=${TMPDIR:-/tmp}/tcos-pwinterop
# (sun_path holds 108 bytes: the sockets get a short folder of their own)
sock=$(mktemp -d /tmp/tcos-pw.XXXXXX)
mkdir -p "$work/obj"
trap 'pkill -TERM -f "$work/pwhost $sock" 2>/dev/null || true; rm -rf "$sock"' EXIT
sources=(tests/pipewire/PipeWireHostServer.cpp src/PipeWire/Graph.cpp src/PipeWire/NativeServer.cpp src/PipeWire/NativeClientNode.cpp src/PipeWire/MidiBus.cpp src/PipeWire/VideoChannel.cpp src/PipeWire/VideoConvert.cpp src/PipeWire/VideoDecode.cpp src/PipeWire/Loopback.cpp src/PipeWire/EchoCanceller.cpp src/PipeWire/PlaybackStream.cpp src/PipeWire/RecordStream.cpp src/PipeWire/Pod.cpp
         src/PipeWire/PulseServer.cpp src/PipeWire/Tagstruct.cpp src/Audio/Resampler.cpp src/Audio/AudioStream.cpp src/Audio/AudioMixer.cpp src/Audio/CaptureStream.cpp src/Audio/AudioCaptureHub.cpp src/Hardware/AudioDevice/CaptureRing.cpp src/Hardware/VideoCodec/Software/JpegDecoder.cpp src/Hardware/VideoCodec/ColorConvert.cpp src/Hardware/VideoCodec/VideoFrameBuffer.cpp src/Hardware/VideoCodec/VideoTypes.cpp src/Hardware/AudioDevice/PlaybackRing.cpp src/Hardware/AudioDevice/Software/SoftwareAudioDevice.cpp src/Hardware/AudioDevice/Software/SoftwareAudioCapture.cpp src/Kernel/Sync.cpp
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
sink_ports = [p for p in ports if p['info']['props'].get('node.id') == sink['id']]
assert len(sink_ports) == 4 and len(ports) == 8 and all(p['info']['params']['Format'] for p in ports)      # (playback and monitor ports, the source's two, the MIDI bridge's two)
midi = [p for p in ports if p['info']['props'].get('format.dsp') == '8 bit raw midi']
assert len(midi) == 2 and all(p['info']['params']['Format'][0]['mediaType'] == 'application' for p in midi)
assert sum(1 for p in sink_ports if p['info']['props'].get('port.monitor') in (True, 'true')) == 2
assert any(o['type'] == 'PipeWire:Interface:Metadata' and o['metadata'][0]['key'] == 'default.audio.sink' for o in d)
\""
check "pw-dump shows the source" "python3 -c \"
import json; d = json.load(open('$work/dump.json'))
src = [o for o in d if o['type'] == 'PipeWire:Interface:Node' and o['info']['props'].get('media.class') == 'Audio/Source'][0]
assert src['info']['props']['node.name'] == 'tcos_input.stereo' and src['info']['n-output-ports'] == 2
assert any(o['type'] == 'PipeWire:Interface:Metadata' and any(m['key'] == 'default.audio.source' for m in o['metadata']) for o in d)
\""
check "pw-dump reports no protocol error" "! grep -q 'invalid message' '$work/dump.err'"
check "pw-cli lists the sink" "timeout 5 pw-cli ls Node | grep -q 'node.name = \"tcos_output.stereo\"'"
if command -v wpctl >/dev/null; then check "wpctl status shows the sink" "timeout 5 wpctl status | grep -q 'Host test speakers'"; fi
check "pactl info" "timeout 5 pactl info | grep -q 'Default Sink: tcos_output.stereo'"
check "pactl list sinks" "timeout 5 pactl list sinks | grep -q 'Name: tcos_output.stereo'"
check "pactl set-sink-volume" "timeout 5 pactl set-sink-volume @DEFAULT_SINK@ 50% && timeout 5 pactl get-sink-volume @DEFAULT_SINK@ | grep -q ' 50%'"
check "pactl set-sink-mute" "timeout 5 pactl set-sink-mute @DEFAULT_SINK@ 1 && timeout 5 pactl get-sink-mute @DEFAULT_SINK@ | grep -q yes && timeout 5 pactl set-sink-mute @DEFAULT_SINK@ 0"
timeout 5 pactl set-sink-volume @DEFAULT_SINK@ 100%
check "pactl list sources shows the monitor" "timeout 5 pactl list short sources | grep -q 'tcos_output.stereo.monitor'"
check "pactl info names the default source" "timeout 5 pactl info | grep -q 'Default Source: tcos_input.stereo'"
check "pactl list sources" "timeout 5 pactl list sources | grep -q 'Description: Host test microphone'"
check "pactl set-source-volume and mute" "timeout 5 pactl set-source-volume @DEFAULT_SOURCE@ 50% && timeout 5 pactl get-source-volume @DEFAULT_SOURCE@ | grep -q ' 50%' && timeout 5 pactl set-source-mute @DEFAULT_SOURCE@ 1 && timeout 5 pactl get-source-mute @DEFAULT_SOURCE@ | grep -q yes && timeout 5 pactl set-source-mute @DEFAULT_SOURCE@ 0 && timeout 5 pactl set-source-volume @DEFAULT_SOURCE@ 100%"
# recording: parecord for about a second (raw 16-bit stereo at 44.1 kHz), and the source outputs while it records
(sleep 0.5; timeout 5 pactl list source-outputs > "$work/outputs.txt" 2>&1 || true) & lister=$!
timeout 1.2 parecord --raw --format=s16le --rate=44100 --channels=2 "$work/recorded.raw" || true
wait $lister || true      # (not a bare wait: the server is a background job too)
# a second recording whose volume another client changes (SET_SOURCE_OUTPUT_VOLUME)
(sleep 0.5; index=$(timeout 5 pactl list short source-outputs | cut -f1 | head -1)
 timeout 5 pactl set-source-output-volume "$index" 50% && timeout 5 pactl list source-outputs > "$work/outputs-volume.txt" 2>&1 || true) & lister=$!
timeout 1.5 parecord --raw "$work/recorded-2.raw" || true
wait $lister || true
check "pactl list source-outputs saw parecord" "grep -q 'application.name = \"parecord\"' '$work/outputs.txt'"
check "pactl set-source-output-volume" "grep -q 'Volume:.* 50%' '$work/outputs-volume.txt'"
check "parecord recorded both sides of the microphone" "python3 - '$work/recorded.raw' <<'PY'
import cmath, math, struct, sys
data = open(sys.argv[1], 'rb').read(); n = len(data) // 4
assert n > 22050, n
s = struct.unpack('<%dh' % (2 * n), data[:4 * n])
seg = 11025      # 0.25 s: 220 and 330 whole periods
left, right = s[2 * (n - seg)::2], s[2 * (n - seg) + 1::2]
mag = lambda x, f: 2 * abs(sum(v * cmath.exp(-2j * math.pi * f * k / 44100) for k, v in enumerate(x))) / len(x)
l880, l1320, r880, r1320 = mag(left, 880), mag(left, 1320), mag(right, 880), mag(right, 1320)
print('   left 880 Hz %.0f, 1320 Hz %.0f; right 880 Hz %.0f, 1320 Hz %.0f' % (l880, l1320, r880, r1320))
assert 7200 < l880 < 8800 and 3600 < r1320 < 4400 and l1320 < 300 and r880 < 300
PY"
(timeout 4 pactl subscribe > "$work/subscribe.txt" 2>&1 || true) & subscriber=$!
(sleep 0.7; timeout 5 pactl list sink-inputs > "$work/inputs.txt" 2>&1 || true) & inputs=$!
sleep 0.3
start=$(date +%s%N)
timeout 15 paplay "$work/a.wav" & a=$!
timeout 15 paplay "$work/b.wav" & b=$!
(sleep 0.4; timeout 0.8 parecord -d @DEFAULT_MONITOR@ --raw --format=s16le --rate=48000 --channels=2 "$work/monitor.raw" || true) & monitor=$!
check "paplay (44.1 kHz stereo)" "wait $a"
check "paplay (22.05 kHz mono) at the same time" "wait $b"
elapsed=$(( ($(date +%s%N) - start) / 1000000 ))
check "played in real time (${elapsed} ms for 1.5 s)" "[ $elapsed -ge 1300 ] && [ $elapsed -le 4000 ]"
wait $subscriber $inputs || true      # (not a bare wait: the server is a background job too)
wait $monitor || true
check "parecord of the monitor heard both paplay tones" "python3 - '$work/monitor.raw' <<'PY'
import cmath, math, struct, sys
data = open(sys.argv[1], 'rb').read(); n = len(data) // 4
assert n > 9600, n
s = struct.unpack('<%dh' % (2 * n), data[:4 * n]); left = s[2 * (n - 9600)::2]
mag = lambda f: 2 * abs(sum(v * cmath.exp(-2j * math.pi * f * k / 48000) for k, v in enumerate(left))) / len(left)
a, b = mag(440), mag(660)
print('   monitor: 440 Hz %.0f, 660 Hz %.0f' % (a, b))
assert 7000 < a < 9000 and 3700 < b < 4700      # (before the master volume: 8000, and 6000 * 0.707)
PY"
check "pactl list sink-inputs saw both" "[ \$(grep -c 'application.name = \"paplay\"' '$work/inputs.txt') -eq 2 ]"
check "pactl subscribe saw the streams come and go" "grep -q \"'new' on sink-input\" '$work/subscribe.txt' && grep -q \"'remove' on sink-input\" '$work/subscribe.txt'"
# libpipewire's pw_stream: pw-play (a 1 s tone at 44.1 kHz) and pw-record (2 s at 44.1 kHz), through client-node
python3 - "$work" <<'PY'
import math, struct, wave, sys
w = wave.open(sys.argv[1] + "/c.wav", "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b"".join(struct.pack("<hh", int(7000 * math.sin(2 * math.pi * 330 * i / 44100)), int(7000 * math.sin(2 * math.pi * 330 * i / 44100))) for i in range(44100)))
w.close()
PY
check "pw-play through client-node" "timeout 15 pw-play '$work/c.wav' 2> '$work/pw-play.log'"
rm -f "$work/pw-rec.wav"
timeout -s INT 2 pw-record --rate 44100 --channels 2 "$work/pw-rec.wav" 2> "$work/pw-record.log" || true
check "pw-record recorded both sides of the microphone" "python3 - '$work/pw-rec.wav' <<'PY'
import cmath, math, struct, sys, wave
w = wave.open(sys.argv[1]); n = w.getnframes(); assert n > 44100, n
s = struct.unpack('<%dh' % (2 * n), w.readframes(n)); left, right = s[2 * (n - 11025)::2], s[2 * (n - 11025) + 1::2]
mag = lambda x, f: 2 * abs(sum(v * cmath.exp(-2j * math.pi * f * k / 44100) for k, v in enumerate(x))) / len(x)
l880, r1320, l1320, r880 = mag(left, 880), mag(right, 1320), mag(left, 1320), mag(right, 880)
print('   pw-record: left 880 Hz %.0f, right 1320 Hz %.0f' % (l880, r1320))
assert 7200 < l880 < 8800 and 3600 < r1320 < 4400 and l1320 < 300 and r880 < 300
PY"
# MIDI, video, client-allocated buffers, drivers and the adapter factory, through the real libpipewire: tests/linux/pwmedia.c built against the host's
# libpipewire (each mode checks itself), pw-midiplay (its notes reach the server's synthesizer: counted at the end), pw-cli create-node adapter (a
# lingering virtual sink) and pw-play / pw-record aimed at it
if pkg-config --exists libpipewire-0.3 && command -v pw-midiplay >/dev/null; then
  gcc -O1 -g -Wall -Wextra -o "$work/pwmedia" tests/linux/pwmedia.c $(pkg-config --cflags --libs libpipewire-0.3) -lm
  for mode in video video-alloc video-zerocopy video-sync video-yuv midi filter adapter; do
    check "pwmedia $mode" "timeout 30 '$work/pwmedia' $mode 2> '$work/pwmedia-$mode.log' | tee -a '$work/pwmedia.out' | grep -q 'pwmedia: $mode ok'"
  done
  python3 - "$work/song.mid" <<'PY'
import struct, sys
ev = b"".join(b"\x00" + bytes([0x90, 60 + i, 100]) + b"\x60" + bytes([0x80, 60 + i, 0]) for i in range(12)) + b"\x00\xff\x2f\x00"
open(sys.argv[1], "wb").write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(ev)) + ev)
PY
  check "pw-midiplay" "timeout 20 pw-midiplay '$work/song.mid' 2> '$work/pw-midiplay.log'"
  check "pw-cli create-node adapter (a lingering null sink)" "timeout 5 pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=cli-null media.class=Audio/Sink object.linger=true audio.position=[FL FR] }' > /dev/null && timeout 5 pw-cli ls Node | grep -q 'node.name = \"cli-null\"'"
  python3 - "$work/tone500.wav" <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<hh", int(8000 * math.sin(2 * math.pi * 500 * i / 48000)), int(8000 * math.sin(2 * math.pi * 500 * i / 48000))) for i in range(48000 * 2)))
PY
  rm -f "$work/from-null.wav"
  (timeout -s INT 3 pw-record --target cli-null "$work/from-null.wav" 2> "$work/pw-record-null.log" || true) & recorder=$!
  sleep 0.3
  check "pw-play into the null sink" "timeout 10 pw-play --target cli-null '$work/tone500.wav' 2> '$work/pw-play-null.log'"
  wait $recorder || true
  check "pw-record from the null sink heard pw-play" "python3 - '$work/from-null.wav' <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1]); n, ch, r = w.getnframes(), w.getnchannels(), w.getframerate()
d = struct.unpack('<%dh' % (n * ch), w.readframes(n)); best = 0
for start in range(0, max(0, n - 4800), 2400):
    s1 = s2 = 0.0; c = 2 * math.cos(2 * math.pi * 500 / r)
    for i in range(start, start + 4800):
        s0 = d[i * ch] + c * s1 - s2; s2 = s1; s1 = s0
    best = max(best, math.sqrt(max(0, s1 * s1 + s2 * s2 - c * s1 * s2)) * 2 / 4800)
print('   pw-record from cli-null: 500 Hz %.0f' % best)
assert 7000 < best < 9000
PY"
  # the same sink through PulseAudio: listed with its monitor, played into with paplay --device, recorded with parecord from its monitor, its volume set
  check "pactl list sinks shows the null sink" "pactl list short sinks | grep -q 'cli-null'"
  check "pactl list sources shows its monitor" "pactl list short sources | grep -q 'cli-null.monitor'"
  check "pactl set-sink-volume on the null sink" "pactl set-sink-volume cli-null 50% && pactl get-sink-volume cli-null | grep -q '50%' && pactl set-sink-volume cli-null 100%"
  rm -f "$work/pa-from-null.raw"
  (timeout -s INT 3 parecord --raw --device=cli-null.monitor --format=s16le --rate=48000 --channels=2 "$work/pa-from-null.raw" 2> "$work/parecord-null.log" || true) & recorder=$!
  sleep 0.3
  check "paplay into the null sink" "timeout 10 paplay --device=cli-null '$work/tone500.wav' 2> '$work/paplay-null.log'"
  wait $recorder || true
  check "parecord from the null sink's monitor heard paplay" "python3 - '$work/pa-from-null.raw' <<'PY'
import math, struct, sys
b = open(sys.argv[1], 'rb').read(); n = len(b) // 4
d = struct.unpack('<%dh' % (2 * n), b[:4 * n]); best = 0
for start in range(0, max(0, n - 4800), 2400):
    s1 = s2 = 0.0; c = 2 * math.cos(2 * math.pi * 500 / 48000)
    for i in range(start, start + 4800):
        s0 = d[i * 2] + c * s1 - s2; s2 = s1; s1 = s0
    best = max(best, math.sqrt(max(0, s1 * s1 + s2 * s2 - c * s1 * s2)) * 2 / 4800)
print('   parecord from cli-null.monitor: 500 Hz %.0f' % best)
assert 7000 < best < 9000
PY"
fi
# pactl load-module module-null-sink: a sink of its arguments (mono at 16 kHz, its description), listed as a module, kept after pactl exits, gone again
# with unload-module
module=$(timeout 5 pactl load-module module-null-sink sink_name=pa-null rate=16000 channels=1 sink_properties=device.description=PaNull 2> "$work/load.log")
check "pactl load-module module-null-sink" "[ -n '$module' ] && timeout 5 pactl list short sinks | grep 'pa-null' | grep -q 's16le 1ch 16000Hz'"
check "pactl list modules shows it" "timeout 5 pactl list short modules | grep -q \"^$module	module-null-sink	sink_name=pa-null\""
check "pactl list sinks names its module and description" "timeout 5 pactl list sinks | grep -A3 'Name: pa-null' | grep -q 'Description: PaNull' && timeout 5 pactl list sinks | grep -A6 'Name: pa-null' | grep -q \"Owner Module: $module\""
check "pactl load-module refuses another module" "! timeout 5 pactl load-module module-ladspa-sink 2> /dev/null"
check "pactl unload-module" "timeout 5 pactl unload-module $module && ! timeout 5 pactl list short sinks | grep -q 'pa-null'"
# module-loopback, -remap-sink, -combine-sink: paplay into one sink, parecord another's monitor; the tone must come through (the remapped sink's left
# on its master's right)
python3 - "$work/left700.wav" <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<hh", int(8000 * math.sin(2 * math.pi * 700 * i / 48000)), 0) for i in range(48000 * 2)))
PY
heard() {      # heard <raw s16 stereo file> <Hz> <channel 0|1> <least> : the tone's level on that channel, at least <least>
  python3 - "$1" "$2" "$3" "$4" <<'PY'
import math, struct, sys
b = open(sys.argv[1], 'rb').read(); n = len(b) // 4; hz = float(sys.argv[2]); ch = int(sys.argv[3]); least = float(sys.argv[4])
d = struct.unpack('<%dh' % (2 * n), b[:4 * n]); best = 0
for start in range(0, max(0, n - 4800), 2400):
    s1 = s2 = 0.0; c = 2 * math.cos(2 * math.pi * hz / 48000)
    for i in range(start, start + 4800):
        s0 = d[i * 2 + ch] + c * s1 - s2; s2 = s1; s1 = s0
    best = max(best, math.sqrt(max(0, s1 * s1 + s2 * s2 - c * s1 * s2)) * 2 / 4800)
print('   %s: %.0f Hz on channel %d: %.0f' % (sys.argv[1].split('/')[-1], hz, ch, best))
assert best >= least
PY
}
record_while() {      # record_while <monitor> <file> <sink> <wav> : parecord the monitor while paplay plays the wav into the sink
  rm -f "$2"
  (timeout -s INT 3 parecord --raw --device="$1" --format=s16le --rate=48000 --channels=2 "$2" 2> /dev/null || true) & local rec=$!
  sleep 0.3
  timeout 10 paplay --device="$3" "$4" 2> /dev/null
  wait $rec || true
}
timeout 5 pactl load-module module-null-sink sink_name=lb-a > /dev/null
timeout 5 pactl load-module module-null-sink sink_name=lb-b > /dev/null
loop=$(timeout 5 pactl load-module module-loopback source=lb-a.monitor sink=lb-b latency_msec=60 2> /dev/null)
check "pactl load-module module-loopback" "[ -n '$loop' ] && timeout 5 pactl list short modules | grep -q \"^$loop	module-loopback\""
record_while lb-b.monitor "$work/loop.raw" lb-a "$work/tone500.wav"
check "the loopback carried paplay from one sink to another" "heard '$work/loop.raw' 500 0 3000"
timeout 5 pactl unload-module "$loop"
remap=$(timeout 5 pactl load-module module-remap-sink sink_name=lb-swap master=lb-b channel_map=front-right,front-left 2> /dev/null)
check "pactl load-module module-remap-sink" "[ -n '$remap' ] && timeout 5 pactl list short sinks | grep -q 'lb-swap'"
record_while lb-b.monitor "$work/remap.raw" lb-swap "$work/left700.wav"
check "the remapped sink's left came out on its master's right" "heard '$work/remap.raw' 700 1 3000 && ! heard '$work/remap.raw' 700 0 500 > /dev/null 2>&1"
timeout 5 pactl unload-module "$remap"
combine=$(timeout 5 pactl load-module module-combine-sink sink_name=lb-both slaves=lb-a,lb-b 2> /dev/null)
check "pactl load-module module-combine-sink" "[ -n '$combine' ] && timeout 5 pactl list short sinks | grep -q 'lb-both'"
record_while lb-a.monitor "$work/combine.raw" lb-both "$work/tone500.wav"
check "the combined sink was heard in a slave" "heard '$work/combine.raw' 500 0 3000"
timeout 5 pactl unload-module "$combine"
check "the modules went with unload-module" "! timeout 5 pactl list short sinks | grep -q 'lb-swap\|lb-both' && ! timeout 5 pactl list short modules | grep -q 'module-loopback\|module-remap-sink\|module-combine-sink'"
# module-remap-source, -virtual-source (with an uplink sink) and -echo-cancel: sources of their own (no ".monitor"), recorded by parecord
rsrc=$(timeout 5 pactl load-module module-remap-source source_name=pa-rsrc master=lb-a.monitor channel_map=front-right,front-left 2> /dev/null)
check "pactl load-module module-remap-source" "[ -n '$rsrc' ] && timeout 5 pactl list short sources | grep -q 'pa-rsrc' && ! timeout 5 pactl list short sources | grep -q 'pa-rsrc.monitor'"
record_while pa-rsrc "$work/rsrc.raw" lb-a "$work/left700.wav"
check "the remapped source's right holds its master's left" "heard '$work/rsrc.raw' 700 1 3000 && ! heard '$work/rsrc.raw' 700 0 500 > /dev/null 2>&1"
timeout 5 pactl unload-module "$rsrc"
vsrc=$(timeout 5 pactl load-module module-virtual-source source_name=pa-vsrc master=lb-b.monitor uplink_sink=pa-up 2> /dev/null)
check "pactl load-module module-virtual-source" "[ -n '$vsrc' ] && timeout 5 pactl list short sources | grep -q 'pa-vsrc' && timeout 5 pactl list short sinks | grep -q 'pa-up'"
record_while pa-vsrc "$work/vsrc.raw" pa-up "$work/tone500.wav"
check "the virtual source heard its uplink sink" "heard '$work/vsrc.raw' 500 0 3000"
timeout 5 pactl unload-module "$vsrc"
aec=$(timeout 5 pactl load-module module-echo-cancel source_master=lb-b.monitor sink_master=lb-b 2> /dev/null)
check "pactl load-module module-echo-cancel" "[ -n '$aec' ] && timeout 5 pactl list short sources | grep -q 'echo-cancel-source' && timeout 5 pactl list short sinks | grep -q 'echo-cancel-sink'"
record_while lb-b.monitor "$work/aec-far.raw" echo-cancel-sink "$work/left700.wav"
check "what plays into echo-cancel-sink reaches its sink master" "heard '$work/aec-far.raw' 700 0 3000"
# (the "microphone" is the sink master's monitor: it hears the far end exactly, which the canceller takes out, and a 500 Hz "local voice" played
# straight into the sink master, which it must keep)
rm -f "$work/aec-near.raw"
(timeout -s INT 3 parecord --raw --device=echo-cancel-source --format=s16le --rate=48000 --channels=2 "$work/aec-near.raw" 2> /dev/null || true) & aec_rec=$!
sleep 0.3
timeout 10 paplay --device=lb-b "$work/tone500.wav" 2> /dev/null & aec_voice=$!
timeout 10 paplay --device=echo-cancel-sink "$work/left700.wav" 2> /dev/null
wait $aec_voice || true
wait $aec_rec || true
check "the echo-cancel source keeps the local voice and has the far end taken out" "python3 - '$work/aec-near.raw' <<'PY'
import math, struct, sys
b = open(sys.argv[1], 'rb').read(); n = len(b) // 4; d = struct.unpack('<%dh' % (2 * n), b[:4 * n]); left = d[0::2]
def levels(hz):
    c = 2 * math.cos(2 * math.pi * hz / 48000); out = []
    for start in range(0, max(0, n - 4800), 4800):
        s1 = s2 = 0.0
        for v in left[start:start + 4800]:
            s0 = v + c * s1 - s2; s2 = s1; s1 = s0
        out.append(math.sqrt(max(0, s1 * s1 + s2 * s2 - c * s1 * s2)) * 2 / 4800)
    return out
far, voice = levels(700), levels(500)
print('   echo-cancel source per 100 ms, far end 700 Hz:', ' '.join('%.0f' % l for l in far))
print('   local voice 500 Hz:', ' '.join('%.0f' % l for l in voice))
late_far, late_voice = far[len(far) // 2:], voice[len(voice) // 2:]
assert len(late_far) >= 4 and max(late_far) < 400, late_far
assert max(late_voice) > 2000, late_voice
PY"
timeout 5 pactl unload-module "$aec"
if command -v pw-cli > /dev/null; then
  check "pw-cli create-node adapter media.class=Audio/Source/Virtual: a source to pactl" "timeout 5 pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=cli-vsrc media.class=Audio/Source/Virtual object.linger=true audio.position=[FL FR] }' > /dev/null && timeout 5 pactl list short sources | grep -q 'cli-vsrc' && ! timeout 5 pactl list short sinks | grep -q 'cli-vsrc'"
fi
always=$(timeout 5 pactl load-module module-always-sink 2> /dev/null)
check "pactl load-module module-always-sink, once" "[ -n '$always' ] && ! timeout 5 pactl load-module module-always-sink 2> /dev/null && timeout 5 pactl unload-module '$always'"
check "the source modules went with unload-module" "! timeout 5 pactl list short sources | grep -q 'pa-rsrc\|pa-vsrc\|echo-cancel' && ! timeout 5 pactl list short sinks | grep -q 'pa-up\|echo-cancel'"
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
# pw-play's 330 Hz, later on its own: full scale (7000) on the left
best = 0
for at in range(0, len(left) - 9600, 4800):
    seg = left[at:at + 9600]
    m = abs(sum(v * cmath.exp(-2j * math.pi * 330 * k / 48000) for k, v in enumerate(seg))) * 2 / len(seg)
    best = max(best, m)
print('   pw-play 330 Hz %.0f' % best)
assert 6500 < best < 7500
PY"
if [ -f "$work/pwmedia" ]; then
  check "the synthesizer heard pw-midiplay's 12 notes and pwmedia's 16" "grep -q 'midi: [0-9]* messages to the synthesizer, 28 note-ons' '$work/server.log'"
  check "the synthesizer got pwmedia's system exclusive message whole" "grep -q 'midi: [0-9]* messages to the synthesizer, [0-9]* note-ons, 1 system exclusive' '$work/server.log'"
fi
grep -q "ERROR: AddressSanitizer\|runtime error" "$work/server.log" && { echo "FAIL sanitizer report in the server"; fail=1; }
exit $fail
