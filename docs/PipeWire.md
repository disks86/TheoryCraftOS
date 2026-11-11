# PipeWire in the kernel

TheoryCraftOS has a PipeWire media graph in the kernel (`src/PipeWire`), laid over the desktop's sound server (`audio::AudioMixer`). Every program that
makes sound is in that one graph, whatever kind of program it is, and the mixer adds all of their streams up into the output device. Native,
Linux and Windows programs therefore play at the same time, each stream with its own volume and mute, and the master volume is the sink's.
Recording goes the other way through the same graph: its source is the microphone the capture hub (`audio::AudioCaptureHub`) shares, and every
program that records, of any kind, gets a record stream of it.

| Who | How it reaches the graph |
| --- | --- |
| Native programs (media player, web audio, MIDI synth, ...) | `AudioMixer::open_stream` as before. `Graph::refresh` finds the streams and shows each as a node of the "TheoryCraftOS" client (`AudioStream::set_label` names it). |
| Linux programs using libpulse (most desktop programs: browsers, SDL, games, `paplay`, `pactl`) | PulseAudio's native protocol at `$XDG_RUNTIME_DIR/pulse/native`, served by `PulseServer`, as pipewire-pulse does. |
| Linux programs using libpipewire (`pw-dump`, `pw-cli`, `wpctl`, session tools) | PipeWire's native protocol (version 3) at `$XDG_RUNTIME_DIR/pipewire-0`, served by `NativeServer`. |
| Windows programs | `winmm.dll` (`waveOut*`, `PlaySound`, `sndPlaySound`: `src/Windows/winmm/waveout.inc`; `waveIn*`: `wavein.inc`), `dsound.dll` (DirectSound 8: `src/Windows/dsound/dsound.c`; capture and full duplex: `capture.inc`) and WASAPI (`mmdevapi.dll`: `IMMDeviceEnumerator`, `IAudioClient3`, `IAudioCaptureClient` and the rest, `src/Windows/mmdevapi/mmdevapi.c`), all over the `NtTcosAudio` service (510, `src/Windows/tcos_audio.h`), served by `WindowsAudioService`. |

`XDG_RUNTIME_DIR` is `/run/user/0` in the Linux programs' default environment (`/Linux/run/user/0` on the system's disk; see
[FileSystemLayout.md](FileSystemLayout.md)). Both sockets are also bound as abstract names, so Windows programs can reach them through ws2_32's
`AF_UNIX` too.

## Files

| Part | Where |
| --- | --- |
| The graph: core, clients, the "default" metadata, the device and sink for the mixer, the device and source for the capture hub, a node with ports and links per stream, volumes, snapshots | `Graph.{hpp,cpp}` |
| A protocol client's stream: any common sample format, 1 to 32 channels, 4 to 192 kHz, folded to 16-bit mono or stereo for an `AudioStream` | `PlaybackStream.{hpp,cpp}` |
| A protocol client's record stream: 16-bit mono or stereo from an `audio::CaptureStream` of the hub, turned into the program's format and channels | `RecordStream.{hpp,cpp}` |
| SPA PODs (the native protocol's values) | `Pod.{hpp,cpp}` |
| PulseAudio tagstructs and property lists | `Tagstruct.{hpp,cpp}`, `Properties.hpp` |
| PipeWire's native protocol | `NativeServer.{hpp,cpp}` |
| PulseAudio's native protocol | `PulseServer.{hpp,cpp}` |
| AF_UNIX listeners and the `pipewire` kernel thread | `SocketServer.{hpp,cpp}`, `ProtocolServer.hpp` |
| NtTcosAudio | `src/Process/WindowsAudio.{hpp,cpp}` (layout, handler pointer), `WindowsAudioService.{hpp,cpp}` (the work), `src/Windows/tcos_audio.h` (the DLLs' side) |
| Start-up | `src/main.cpp` (after the X server) |

## How it fits together

- **The mixer stays the one owner of the device.** A graph stream is an `AudioStream` of the mixer with a `PlaybackStream` in front that converts
  the program's format. Nodes, ports and links describe that (a stream's output ports are linked to the sink's `playback_FL`/`playback_FR`); no
  separate scheduling graph runs. A device change (USB headset plugged in, see `AudioRouter`) shows as the sink's new description.
- **Changes are pulled, not pushed.** Each change bumps the graph's serial; each object keeps the serial of its last change. The protocol servers
  compare a snapshot (id, type, serial) with what their clients were told and send registry `global`/`global_remove`, new `info` events, or PulseAudio
  subscription events. Nothing calls back into a protocol from the graph's lock.
- **Played means heard.** `PlaybackStream::drained()` and `played_bytes()` count what the device still holds (`AudioMixer::played_frames()`), so a
  PulseAudio DRAIN, a `WOM_DONE` and a synchronous `PlaySound` return when the sound has played out, not when it went into the device's queue.
- **Locks.** The graph's mutex is never held while the mixer's is taken. Order: Windows service, then graph, then a playback stream, then the
  `AudioStream`; the mixer takes its own, then the `AudioStream`'s. The socket thread never holds the system lock while it calls a protocol.
- **Paused streams.** `AudioStream::set_paused` (PulseAudio cork, `waveOutPause`) keeps what is queued and gives the mixer nothing.
- **Recording.** With a capture hub (`Graph(allocator, mixer, hub)`; the kernel passes the one `main.cpp` makes), the graph has a second device and
  the source node `tcos_input.stereo` (`Audio/Source`, ports `capture_FL`/`capture_FR`, `default.audio.source`), named after the microphone in use
  and suspended while there is none. `Graph::create_record` opens a `CaptureStream` on the hub at the program's rate (half a second of ring) and adds a
  `Stream/Input/Audio` node linked from the source; the hub resamples, `RecordStream` converts to the program's format (each output channel takes its
  side; centre and mono the average, LFE silence). The source's volume and mute are the hub's master gain and mute (the microphone's), a record
  stream's its `CaptureStream`'s gain (neither goes above 1). A paused (corked, stopped) record stream drops what the microphone hears meanwhile, so it
  starts again with sound from the moment it resumes. Native capture streams (WebRTC's through the desktop) are not shown as nodes.

## What the protocols serve

**PulseAudio** (protocol 35 and older): AUTH (no shared memory is offered, so libpulse sends the audio in the packets), SET_CLIENT_NAME, playback
streams (create, delete, drain, cork, flush, trigger, prebuf, latency, buffer attributes, name and property updates) with REQUEST, STARTED and
PLAYBACK_STREAM_KILLED, server/sink/sink-input/client info and lists, sink and sink-input volume and mute, kill-sink-input, SUBSCRIBE and its events,
STAT, LOOKUP_SINK. Record streams (create, delete, cork, flush, latency, buffer attributes, name and property updates, RECORD_STREAM_KILLED): the
server sends what they record on their channel a fragment at a time (`fragsize`, 50 ms by default, 5 to 250 ms), and stops sending to a client that
does not read (the hub's ring drops the oldest audio instead). Source and source-output info and lists, their volume and mute (SET_SOURCE_OUTPUT_VOLUME
is command 98: the numbers from 98 on were one off before), kill-source-output, LOOKUP_SOURCE, the default source in the server info, source and
source-output subscription events. Sample formats: U8, A-law, µ-law, S16, S24, S24-32, S32 (both byte orders except S24-32 BE), float 32.

**PipeWire native**: Core hello/sync/pong/get_registry/destroy, Registry bind for Core, Client, Device, Node, Port, Link and Metadata, the info of
bound objects (sent again when they change), Client update_properties/get_permissions, Node and Port enum_params/subscribe_params (Props, EnumFormat,
Format), Node set_param(Props) for volume and mute (the source's too), the metadata's `default.audio.sink` and `default.audio.source`.

**winmm**: `waveOutOpen` (PCM 8/16/24/32-bit, float 32/64, `WAVE_FORMAT_EXTENSIBLE`, `WAVE_FORMAT_QUERY`, function/event/window/thread callbacks),
prepare/unprepare/write, pause/restart/reset/break loop, position (bytes, samples, ms; frozen while paused), per-stream and master volume, device caps,
error texts, `PlaySound`/`sndPlaySound` of a WAVE file or image (sync, async, loop, stop). `waveIn*` (one device while there is a microphone):
open (the same formats and callbacks), prepare/unprepare/add buffer, start/stop/reset (Stop returns the header being filled with what it holds,
Reset every header and the position starts again), position, caps, error texts. A worker thread reads the kernel's capture stream into the queued
headers every 5 ms and returns each one (`WIM_DATA`) once it is full.

**DirectSound** mixes in the program, as Windows does: each `IDirectSound8` has one 16-bit stereo 48 kHz stream of the graph and a thread that mixes
its playing secondary buffers into it every 5 ms (fixed point; linear interpolation at each buffer's own frequency), keeping the kernel's stream about
30 ms full (more when the thread is woken late). Buffers: PCM 8/16/24/32-bit and float, 1 to 8 channels, `Lock`/`Unlock` across the end,
`Play`/`Stop` (looping, one-shot), cursors (play = what the device has played, write = where the mixer has read to), `SetCurrentPosition`,
volume (hundredths of a dB), pan, frequency (100 Hz to 200 kHz, at once), position notifications and `DSBPN_OFFSETSTOP`, `DuplicateSoundBuffer`
(shared samples), 3D buffers and the listener (distance with min/max and rolloff, the cone, an equal-power pan from the direction, head-relative and
disabled modes, deferred settings and `CommitDeferredSettings`). The primary buffer's volume and pan are the whole mix's; with `DSSCL_WRITEPRIMARY`
the program writes it itself. Enumeration (the primary driver and one device), `GetDeviceID`, caps, cooperative levels, speaker configuration and
the class factory (`CLSID_DirectSound`, `CLSID_DirectSound8`; ole32 finds `dsound.dll` for them). Capture: `DirectSoundCaptureCreate(8)`,
`DirectSoundCaptureEnumerate`, `GetDeviceID` for the default (voice) capture device, `IDirectSoundCapture8` (caps, several buffers at once) and
`IDirectSoundCaptureBuffer8`, each a capture stream of the graph: a ring in the program's format that a thread fills every 5 ms at the capture cursor
while it captures (`Start` with or without `DSCBSTART_LOOPING`, `Stop`), `Lock` across the end, `GetCurrentPosition` (capture = read cursor),
position notifications and `DSBPN_OFFSETSTOP`; `IDirectSoundFullDuplex` and `DirectSoundFullDuplexCreate` (a capture and a playback buffer);
`CLSID_DirectSoundCapture(8)` and `CLSID_DirectSoundFullDuplex` in the class factory. No capture effects (`DSCBCAPS_CTRLFX` is refused).

**WASAPI**: `CLSID_MMDeviceEnumerator` (ole32 finds `mmdevapi.dll` for it) with one render endpoint and one capture endpoint (while there is a
microphone: form factor `Microphone`, its own id and GUID, `DEVINTERFACE_AUDIO_CAPTURE` for `ActivateAudioInterfaceAsync`), `IMMDevice`
(`Activate`, the property store: names, form factor, speakers, the engine format), `IMMEndpoint`; `IAudioClient`/`2`/`3` in shared mode, each client a
stream of the graph in its own format (any format the graph converts, as `AUTOCONVERTPCM` would), event-driven or polled, with `IAudioRenderClient`,
`IAudioClock`, `IAudioStreamVolume`, `IChannelAudioVolume`; the process's audio session (`ISimpleAudioVolume`, `IAudioSessionControl2`,
`IAudioSessionManager2`); `IAudioEndpointVolume` as the master volume (scalar = cube root of the amplitude, as `pactl`/`wpctl` show it) with change
callbacks; `IAudioMeterInformation` (silence); `ActivateAudioInterfaceAsync`. The padding is what the graph's mixer has not taken yet
(NtTcosAudio STATUS `[4]`), as in Windows, so the device's own queue never stops a client from writing; the clock is what the device has played.
A stream's left/right volume pair becomes its volume and pan in the graph (`audio::pan_for_gains`), and `SET_MUTE` mutes without losing the volume.
A capture endpoint's client is a record stream: a thread reads what the kernel recorded into the client's ring every period, `IAudioCaptureClient`
hands it out a packet (10 ms) at a time with its device position and QPC time, `GetNextPacketSize` is 0 until a whole packet is there (what is left
comes out once stopped), the padding is everything waiting to be read, and audio the kernel lost (`STATUS [5]`) flags the next packet
`AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY`. The capture endpoint's `IAudioEndpointVolume` is the microphone's master gain (`NtTcosAudio` stream
0xFFFFFFFF); the session's volume applies to the process's render streams only.

## Tests

- `tests/run_host_tests.sh PipeWireTest`: codecs (round trips, every truncation, bit flips), conversion both ways (every format through
  `convert_from_s16` and back), the graph mixing native, Linux and Windows streams on a real mixer, recording from a fake microphone through a real
  capture hub (the source, record nodes and links, gains, pause, unplugging), both protocols as clients drive them and fed garbage (every truncation of
  CREATE_RECORD_STREAM and bit flips), the Windows service both ways, the AF_UNIX transport, several threads at once.
- `tests/pipewire_interop.sh`: the host's real `pw-dump`, `pw-cli`, `wpctl`, `pactl`, `paplay` and `parecord` against
  `tests/pipewire/PipeWireHostServer.cpp` (the same graph and protocols on host sockets, a microphone that hears 880 Hz left and 1320 Hz right). Two
  `paplay` at once; the mixed output must hold both tones at the right levels; `parecord` must record each side's tone; the source's and a source
  output's volume set through `pactl` must read back.
- `tests/pipewire_emulated.sh [x86_64|aarch64|riscv64] [all|winmm|dsound|record]`: on the kernel's CPU emulator, `tests/windows/winmmtest.c` (with the
  OS's own DLLs) and `tests/linux/pulsetest.c` (no C library) play at the same time as a native stream; the mix must hold all three tones at once.
  The second run is `tests/windows/dsoundtest.c`: a DirectSound loop (600 Hz) and a WASAPI event-driven client (700 Hz) from one program, with the
  Linux program and the native stream, and all four tones must be in the mix at once (the DLLs are also put in the RAM disk's System32, since ole32
  loads `dsound.dll` and `mmdevapi.dll` while the program runs). The third run records from the emulator's microphone (880 Hz left at 8000, 1320 Hz
  right at 4000): `tests/windows/recordtest.c` through `waveIn` (22.05 kHz stereo), DirectSound capture (16 kHz float mono) and WASAPI capture (48 kHz
  stereo), each checking both tones' levels, while `pulsetest` records the same microphone with a PulseAudio record stream. The work folder is one
  per checkout (`/tmp/tcos-pwemu-<checksum of the path>`), so another worktree's objects and DLLs are never taken for this one's.

## Not done

- **libpipewire playback.** `pw_stream` programs (`pw-cat`, `pw-play`) ask Core::create_object for the `client-node`/`adapter` factories, which need
  shared-memory buffers (memfd + `mmap(MAP_SHARED)` of a descriptor) and eventfds; the Linux layer has neither yet, so the server answers `-ENOTSUP`.
  Those programs fail; libpulse programs work.
- **libpipewire recording** has the playback side's gap (`pw_stream` needs `client-node`); `pw-record` fails, `parecord` works.
- **Monitors and loopback.** The sink has no monitor source (`tcos_output.stereo.monitor`), so recording what plays (OBS, WASAPI loopback,
  `AUDCLNT_STREAMFLAGS_LOOPBACK`) is refused; PulseAudio's `direct_on_input` (recording one stream) too.
- **Native recorders in the graph.** The hub's own streams (WebRTC's `getUserMedia` through the desktop) are not shown as nodes, so `pactl list
  source-outputs` does not list them.
- **More than one source.** The graph follows the hub's one microphone, as the sink follows the mixer's one output (`move-source-output`,
  `set-default-source` to another device do nothing).
- **More of Windows audio.** XAudio2 is still missing (it would go over WASAPI). WASAPI has no exclusive mode (refused) and no loopback capture;
  endpoint and session notifications are accepted but never sent (nothing changes from outside the process); the master volume has no balance.
  DirectSound has no effects (`SetFX` answers `DSERR_FXUNAVAILABLE`, no capture effects such as AEC or noise suppression), no Doppler shift, no
  property sets (`IKsPropertySet`, EAX); after `Stop` what it had mixed ahead (30 ms or more) still plays. WASAPI capture packets are always 10 ms
  (`IAudioClient3`'s smaller shared periods are not offered for capture either).
- **More than one sink.** The graph follows the mixer's one output device; switching devices is the `AudioRouter`'s, not PipeWire's
  (`move-sink-input`, `set-default-sink` to another device do nothing).
- **Formats, controls.** PulseAudio sample-rate updates, passthrough formats, per-channel volumes (the loudest channel is taken), PipeWire
  `Route`/`Profile` params, and links made or destroyed by clients.
- **Not run in QEMU** with a real dynamically linked libpulse program: the Linux layer's dynamic loading of such programs is not validated yet.
