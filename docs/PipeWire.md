# PipeWire in the kernel

TheoryCraftOS has a PipeWire media graph in the kernel (`src/PipeWire`), laid over the desktop's sound server (`audio::AudioMixer`). Every program that
makes sound is in that one graph, whatever kind of program it is, and the mixer adds all of their streams up into the output device. Native,
Linux and Windows programs therefore play at the same time, each stream with its own volume and mute, and the master volume is the sink's.

| Who | How it reaches the graph |
| --- | --- |
| Native programs (media player, web audio, MIDI synth, ...) | `AudioMixer::open_stream` as before. `Graph::refresh` finds the streams and shows each as a node of the "TheoryCraftOS" client (`AudioStream::set_label` names it). |
| Linux programs using libpulse (most desktop programs: browsers, SDL, games, `paplay`, `pactl`) | PulseAudio's native protocol at `$XDG_RUNTIME_DIR/pulse/native`, served by `PulseServer`, as pipewire-pulse does. |
| Linux programs using libpipewire (`pw-dump`, `pw-cli`, `wpctl`, session tools) | PipeWire's native protocol (version 3) at `$XDG_RUNTIME_DIR/pipewire-0`, served by `NativeServer`. |
| Windows programs | `winmm.dll` (`waveOut*`, `PlaySound`, `sndPlaySound`: `src/Windows/winmm/waveout.inc`), `dsound.dll` (DirectSound 8: `src/Windows/dsound/dsound.c`) and WASAPI (`mmdevapi.dll`: `IMMDeviceEnumerator`, `IAudioClient3` and the rest, `src/Windows/mmdevapi/mmdevapi.c`), all over the `NtTcosAudio` service (510, `src/Windows/tcos_audio.h`), served by `WindowsAudioService`. |

`XDG_RUNTIME_DIR` is `/run/user/0` in the Linux programs' default environment (`/Linux/run/user/0` on the system's disk; see
[FileSystemLayout.md](FileSystemLayout.md)). Both sockets are also bound as abstract names, so Windows programs can reach them through ws2_32's
`AF_UNIX` too.

## Files

| Part | Where |
| --- | --- |
| The graph: core, clients, the "default" metadata, the device and sink for the mixer, a node with ports and links per stream, volumes, snapshots | `Graph.{hpp,cpp}` |
| A protocol client's stream: any common sample format, 1 to 32 channels, 4 to 192 kHz, folded to 16-bit mono or stereo for an `AudioStream` | `PlaybackStream.{hpp,cpp}` |
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

## What the protocols serve

**PulseAudio** (protocol 35 and older): AUTH (no shared memory is offered, so libpulse sends the audio in the packets), SET_CLIENT_NAME, playback
streams (create, delete, drain, cork, flush, trigger, prebuf, latency, buffer attributes, name and property updates) with REQUEST, STARTED and
PLAYBACK_STREAM_KILLED, server/sink/sink-input/client info and lists, sink and sink-input volume and mute, kill-sink-input, SUBSCRIBE and its events,
STAT, LOOKUP_SINK. Sample formats: U8, A-law, µ-law, S16, S24, S24-32, S32 (both byte orders except S24-32 BE), float 32.

**PipeWire native**: Core hello/sync/pong/get_registry/destroy, Registry bind for Core, Client, Device, Node, Port, Link and Metadata, the info of
bound objects (sent again when they change), Client update_properties/get_permissions, Node and Port enum_params/subscribe_params (Props, EnumFormat,
Format), Node set_param(Props) for volume and mute, the metadata's `default.audio.sink`.

**winmm**: `waveOutOpen` (PCM 8/16/24/32-bit, float 32/64, `WAVE_FORMAT_EXTENSIBLE`, `WAVE_FORMAT_QUERY`, function/event/window/thread callbacks),
prepare/unprepare/write, pause/restart/reset/break loop, position (bytes, samples, ms; frozen while paused), per-stream and master volume, device caps,
error texts, `PlaySound`/`sndPlaySound` of a WAVE file or image (sync, async, loop, stop).

**DirectSound** mixes in the program, as Windows does: each `IDirectSound8` has one 16-bit stereo 48 kHz stream of the graph and a thread that mixes
its playing secondary buffers into it every 5 ms (fixed point; linear interpolation at each buffer's own frequency), keeping the kernel's stream about
30 ms full (more when the thread is woken late). Buffers: PCM 8/16/24/32-bit and float, 1 to 8 channels, `Lock`/`Unlock` across the end,
`Play`/`Stop` (looping, one-shot), cursors (play = what the device has played, write = where the mixer has read to), `SetCurrentPosition`,
volume (hundredths of a dB), pan, frequency (100 Hz to 200 kHz, at once), position notifications and `DSBPN_OFFSETSTOP`, `DuplicateSoundBuffer`
(shared samples), 3D buffers and the listener (distance with min/max and rolloff, the cone, an equal-power pan from the direction, head-relative and
disabled modes, deferred settings and `CommitDeferredSettings`). The primary buffer's volume and pan are the whole mix's; with `DSSCL_WRITEPRIMARY`
the program writes it itself. Enumeration (the primary driver and one device), `GetDeviceID`, caps, cooperative levels, speaker configuration and
the class factory (`CLSID_DirectSound`, `CLSID_DirectSound8`; ole32 finds `dsound.dll` for them).

**WASAPI**: `CLSID_MMDeviceEnumerator` (ole32 finds `mmdevapi.dll` for it) with one render endpoint and no capture endpoints, `IMMDevice`
(`Activate`, the property store: names, form factor, speakers, the engine format), `IMMEndpoint`; `IAudioClient`/`2`/`3` in shared mode, each client a
stream of the graph in its own format (any format the graph converts, as `AUTOCONVERTPCM` would), event-driven or polled, with `IAudioRenderClient`,
`IAudioClock`, `IAudioStreamVolume`, `IChannelAudioVolume`; the process's audio session (`ISimpleAudioVolume`, `IAudioSessionControl2`,
`IAudioSessionManager2`); `IAudioEndpointVolume` as the master volume (scalar = cube root of the amplitude, as `pactl`/`wpctl` show it) with change
callbacks; `IAudioMeterInformation` (silence); `ActivateAudioInterfaceAsync`. The padding is what the graph's mixer has not taken yet
(NtTcosAudio STATUS `[4]`), as in Windows, so the device's own queue never stops a client from writing; the clock is what the device has played.
A stream's left/right volume pair becomes its volume and pan in the graph (`audio::pan_for_gains`), and `SET_MUTE` mutes without losing the volume.

## Tests

- `tests/run_host_tests.sh PipeWireTest`: codecs (round trips, every truncation, bit flips), conversion, the graph mixing native, Linux and Windows
  streams on a real mixer, both protocols as clients drive them and fed garbage, the Windows service, the AF_UNIX transport, several threads at once.
- `tests/pipewire_interop.sh`: the host's real `pw-dump`, `pw-cli`, `wpctl`, `pactl` and `paplay` against `tests/pipewire/PipeWireHostServer.cpp`
  (the same graph and protocols on host sockets). Two `paplay` at once; the mixed output must hold both tones at the right levels.
- `tests/pipewire_emulated.sh [x86_64|aarch64|riscv64] [all|winmm|dsound]`: on the kernel's CPU emulator, `tests/windows/winmmtest.c` (with the
  OS's own DLLs) and `tests/linux/pulsetest.c` (no C library) play at the same time as a native stream; the mix must hold all three tones at once.
  The second run is `tests/windows/dsoundtest.c`: a DirectSound loop (600 Hz) and a WASAPI event-driven client (700 Hz) from one program, with the
  Linux program and the native stream, and all four tones must be in the mix at once (the DLLs are also put in the RAM disk's System32, since ole32
  loads `dsound.dll` and `mmdevapi.dll` while the program runs).

## Not done

- **libpipewire playback.** `pw_stream` programs (`pw-cat`, `pw-play`) ask Core::create_object for the `client-node`/`adapter` factories, which need
  shared-memory buffers (memfd + `mmap(MAP_SHARED)` of a descriptor) and eventfds; the Linux layer has neither yet, so the server answers `-ENOTSUP`.
  Those programs fail; libpulse programs work.
- **Recording.** No capture nodes, no PulseAudio record streams, no `waveIn`, no DirectSound capture, no WASAPI capture endpoints (the
  `AudioCaptureHub` is not in the graph yet).
- **More of Windows audio.** XAudio2 is still missing (it would go over WASAPI). WASAPI has no exclusive mode (refused) and no loopback capture;
  endpoint and session notifications are accepted but never sent (nothing changes from outside the process); the master volume has no balance.
  DirectSound has no effects (`SetFX` answers `DSERR_FXUNAVAILABLE`), no Doppler shift, no property sets (`IKsPropertySet`, EAX); after `Stop` what
  it had mixed ahead (30 ms or more) still plays.
- **More than one sink.** The graph follows the mixer's one output device; switching devices is the `AudioRouter`'s, not PipeWire's
  (`move-sink-input`, `set-default-sink` to another device do nothing).
- **Formats, controls.** PulseAudio sample-rate updates, passthrough formats, per-channel volumes (the loudest channel is taken), PipeWire
  `Route`/`Profile` params, and links made or destroyed by clients.
- **Not run in QEMU** with a real dynamically linked libpulse program: the Linux layer's dynamic loading of such programs is not validated yet.
