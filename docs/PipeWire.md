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
| Linux programs using libpipewire (`pw-dump`, `pw-cli`, `wpctl`, session tools; `pw-play`, `pw-record`, `pw-cat` and other `pw_stream` programs) | PipeWire's native protocol (version 3) at `$XDG_RUNTIME_DIR/pipewire-0`, served by `NativeServer` (streams through `client-node`: `NativeClientNode.cpp`). |
| Windows programs | `winmm.dll` (`waveOut*`, `PlaySound`, `sndPlaySound`: `src/Windows/winmm/waveout.inc`; `waveIn*`: `wavein.inc`), `dsound.dll` (DirectSound 8: `src/Windows/dsound/dsound.c`; capture and full duplex: `capture.inc`) WASAPI (`mmdevapi.dll`: `IMMDeviceEnumerator`, `IAudioClient3`, `IAudioCaptureClient` and the rest, `src/Windows/mmdevapi/mmdevapi.c`) and XAudio2 2.9 and 2.8 (`xaudio2_9.dll`, `xaudio2_8.dll`: `src/Windows/xaudio2_9`), all over the `NtTcosAudio` service (510, `src/Windows/tcos_audio.h`), served by `WindowsAudioService`. |

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
| PipeWire's native protocol | `NativeServer.{hpp,cpp}`; `client-node` (pw_stream) in `NativeClientNode.cpp` |
| PulseAudio's native protocol | `PulseServer.{hpp,cpp}` |
| AF_UNIX listeners and the `pipewire` kernel thread; the memfds and eventfds handed to clients (`IDescriptorMaker`) | `SocketServer.{hpp,cpp}`, `ProtocolServer.hpp` |
| eventfd, timerfd, epoll and signalfd for Linux programs: descriptors of the AF_UNIX manager (`UnixType::Event`, `Timer`, `Epoll`, `Signal`) | `src/Process/UnixSockets.{hpp,cpp}`, `LinuxSockets.cpp` |
| Priority-inheritance futexes (glibc's `PTHREAD_PRIO_INHERIT` mutexes, which libpipewire's loops use) | `src/Process/LinuxThreads.cpp`, `IUserMemory::compare_exchange32` |
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
  starts again with sound from the moment it resumes. Native programs' record streams on the hub (WebRTC's microphone through the desktop, labelled
  "Web Browser" with `CaptureStream::set_label`) are found by `refresh()` and shown as record nodes of the "TheoryCraftOS" client, with their gain
  as volume; the graph's own streams are marked `server_owned` so they are not shown twice.
- **The monitor.** The sink has output ports `monitor_FL`/`monitor_FR` (`port.monitor`). `create_record(..., monitor = true)` makes a record stream
  that the mixer feeds what it mixes (`AudioMixer::add_tap`, an `IAudioTap` such as a `CaptureStream`), before the master volume (the device's). While
  nothing plays nothing is mixed, so a monitor stream hears nothing (as WASAPI loopback does) rather than silence. PulseAudio shows it as the source
  `tcos_output.stereo.monitor` (index sink | 1 << 24, `@DEFAULT_MONITOR@`, the default source when there is no microphone); `NtTcosAudio`'s
  OPEN_CAPTURE takes it with `[1] = 1`, which WASAPI's `AUDCLNT_STREAMFLAGS_LOOPBACK` on the render endpoint uses.

- **client-node (libpipewire's `pw_stream`).** `pw-play`, `pw-record` and every `pw_stream` program export their own node: Core
  `create_object("client-node", ..., version 6)`. The server makes it a playback or record stream of the graph (by `media.class`; at
  `set_active` or the first `port_update`, so that the update before them can say the stream's format) and hands the
  client, with SCM_RIGHTS, the memory it shares with the kernel (memfds: the node's `pw_node_activation`, the port's buffers and its
  `spa_io_buffers` area) and two eventfds (the wake and done signals of `transport`). It drives the adapter the stream wraps: a PortConfig
  (convert mode, F32 at the stream's own rate and channel count, mono or stereo, from the EnumFormat in its update, else 48 kHz stereo; F32
  because the AVX2 S16 output of audioconvert assumes an aligned destination and crashes), the port's Format, then
  buffers as the port's Buffers param asks (2 to 16, 64-byte aligned, at least 8192 frames), the io area and Start. Each cycle (the stream's
  `node.force-quantum`, or its `node.latency` "N/D" as N * rate / D frames, else 1024; 64 to 8192) the server writes the clock into the activation, sets it TRIGGERED and signals the wake eventfd; the client processes and
  sets FINISHED. A playback node is triggered while its stream holds less than two cycles (at least 3072 frames), and the buffer it filled (HAVE_DATA) goes into the
  `PlaybackStream`; a record node gets a buffer filled from its `RecordStream` before each trigger. A cycle that never finishes is given up after
  100 ticks and triggered again; `set_active(false)` pauses. The `pipewire` thread ticks every 5 ms while nodes run (50 ms otherwise). Descriptors
  are the transport's (`IDescriptorMaker`): `SocketServer` makes memfds (`MemoryFile`, whose block the kernel reads and writes in place while the
  client maps it) and eventfds (counters in the AF_UNIX manager, so `poll()`, wake-ups and SCM_RIGHTS take them as sockets; `EFD_NONBLOCK`, as
  PipeWire makes them: libpipewire empties one with a read that must not wait), sends each take's
  descriptors with its first bytes, and releases what the protocol let go of from its own thread (a graveyard).

- **libpipewire itself in the OS.** The real libpipewire runs on the Linux layer (dynamically linked through glibc's `ld.so`, see
  [LinuxDynamicPrograms.md](LinuxDynamicPrograms.md)): its loops are `epoll` sets of eventfds, timerfds, signalfds and sockets, guarded by
  `PTHREAD_PRIO_INHERIT` mutexes. So the Linux layer has:
  - `epoll_create1`, `epoll_ctl`, `epoll_wait`, `epoll_pwait`, `epoll_pwait2`: an interest list kept in the AF_UNIX manager, evaluated as `poll()`
    evaluates descriptors. The list is copied and checked against the descriptor table outside the system lock (a descriptor closed or reused
    meanwhile is dropped, as Linux drops a closed file's); a change made while a thread waits (`epoll_ctl` from another thread) wakes it to look
    again. `EPOLLONESHOT` reports once until `EPOLL_CTL_MOD`; `EPOLLET` is level-triggered (a program reading until EAGAIN sees the same).
    Regular files are refused (EPERM), as on Linux.
  - `timerfd_create`, `timerfd_settime` (relative, `TFD_TIMER_ABSTIME`, periodic, disarming), `timerfd_gettime`: the timer is kept in the
    manager and advanced from the process's clocks whenever it is looked at; `poll()`, `select()`, `epoll_wait` and a blocking `read()` wake when
    the first timer they watch is due (and look again when another thread arms one).
  - `signalfd4`: a descriptor no signal is routed to (signals are not delivered to descriptors); a loop can hold it.
  - PI futexes (`FUTEX_LOCK_PI`, `LOCK_PI2`, `TRYLOCK_PI`, `UNLOCK_PI`): the owner's tid and `FUTEX_WAITERS` in the word as on Linux, through an
    atomic compare-and-exchange on the user's page (`AddressSpace::compare_exchange32`). Without them glibc refuses PI mutexes (ENOTSUP), and
    libpipewire's data loop with them. No priority is lent (the scheduler has no donation).

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
`client-node` (Core create_object): ClientNode update, port_update, set_active, get_node/event/port_buffers (accepted, unused); the events
transport, set_param (PortConfig), port_set_param (Format), port_use_buffers, port_set_io, port_set_mix_info, command (Start, Pause), and Core
add_mem for every memfd. Other factories (`adapter`, `link-factory`, `metadata`, `spa-node-factory`) are refused with `-ENOTSUP`.

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
disabled modes, deferred settings and `CommitDeferredSettings`, the Doppler shift from the buffer's and the listener's velocities with the Doppler
and distance factors). `Play` and `Stop` take back what was mixed ahead and is not in the device yet (the kernel stream is flushed and every playing
buffer steps back as many frames), so a sound starts and stops at once rather than 30 ms or more later. The primary buffer's volume and pan are the whole mix's; with `DSSCL_WRITEPRIMARY`
the program writes it itself. Enumeration (the primary driver and one device), `GetDeviceID`, caps, cooperative levels, speaker configuration and
the class factory (`CLSID_DirectSound`, `CLSID_DirectSound8`; ole32 finds `dsound.dll` for them). Capture: `DirectSoundCaptureCreate(8)`,
`DirectSoundCaptureEnumerate`, `GetDeviceID` for the default (voice) capture device, `IDirectSoundCapture8` (caps, several buffers at once) and
`IDirectSoundCaptureBuffer8`, each a capture stream of the graph: a ring in the program's format that a thread fills every 5 ms at the capture cursor
while it captures (`Start` with or without `DSCBSTART_LOOPING`, `Stop`), `Lock` across the end, `GetCurrentPosition` (capture = read cursor),
position notifications and `DSBPN_OFFSETSTOP`; `IDirectSoundFullDuplex` and `DirectSoundFullDuplexCreate` (a capture and a playback buffer);
`CLSID_DirectSoundCapture(8)` and `CLSID_DirectSoundFullDuplex` in the class factory. No capture effects (`DSCBCAPS_CTRLFX` is refused).

**WASAPI** (loopback: a render endpoint's client with `AUDCLNT_STREAMFLAGS_LOOPBACK` is a capture client of the monitor): `CLSID_MMDeviceEnumerator` (ole32 finds `mmdevapi.dll` for it) with one render endpoint and one capture endpoint (while there is a
microphone: form factor `Microphone`, its own id and GUID, `DEVINTERFACE_AUDIO_CAPTURE` for `ActivateAudioInterfaceAsync`), `IMMDevice`
(`Activate`, the property store: names, form factor, speakers, the engine format), `IMMEndpoint`; `IAudioClient`/`2`/`3` in shared mode, each client a
stream of the graph in its own format (any format the graph converts, as `AUTOCONVERTPCM` would), event-driven or polled, with `IAudioRenderClient`,
`IAudioClock`, `IAudioStreamVolume`, `IChannelAudioVolume`; the process's audio session (`ISimpleAudioVolume`, `IAudioSessionControl2`,
`IAudioSessionManager2`); `IAudioEndpointVolume` as the master volume (scalar = cube root of the amplitude, as `pactl`/`wpctl` show it) with change
callbacks (changes from outside the process, pactl or the desktop, too: a watcher thread looks every 100 ms while there are callbacks); session
events (`IAudioSessionEvents`: volume, mute, active/inactive) and `IMMNotificationClient` (endpoints coming and going); `IAudioMeterInformation` (silence); `ActivateAudioInterfaceAsync`. The padding is what the graph's mixer has not taken yet
(NtTcosAudio STATUS `[4]`), as in Windows, so the device's own queue never stops a client from writing; the clock is what the device has played.
A stream's left/right volume pair becomes its volume and pan in the graph (`Graph::set_balance`, as PulseAudio's per-channel volumes and the
native protocol's `channelVolumes` do), and `SET_MUTE` mutes without losing the volume.
A capture endpoint's client is a record stream: a thread reads what the kernel recorded into the client's ring every period, `IAudioCaptureClient`
hands it out a packet (10 ms) at a time with its device position and QPC time, `GetNextPacketSize` is 0 until a whole packet is there (what is left
comes out once stopped), the padding is everything waiting to be read, and audio the kernel lost (`STATUS [5]`) flags the next packet
`AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY`. The capture endpoint's `IAudioEndpointVolume` is the microphone's master gain (`NtTcosAudio` stream
0xFFFFFFFF); the session's volume applies to the process's render streams only.

**XAudio2** (`xaudio2_9.dll`, and `xaudio2_8.dll` from the same source): each `IXAudio2` is one 32-bit float stream of the graph in its mastering
voice's format, and an engine thread runs the voice graph a 10 ms quantum at a time, 20 to 100 ms ahead of the kernel. Source voices read their
queued buffers (PCM 8/16/24/32-bit, float, MS-ADPCM decoded on submit; play and loop regions, loop counts, `ExitLoop`, `END_OF_STREAM`) a run at a time
and resample with linear interpolation at the frequency ratio, then the voice's filter (state-variable low/band/high pass and notch, one-pole low and
high pass), its effect chain (any XAPO: the program's own, or `CreateAudioVolumeMeter`, `CreateAudioReverb`, `CreateFX`'s EQ, mastering limiter,
reverb and echo), volume, channel volumes and the sends' output matrices (with send filters) into submix voices (run by processing stage) and the
mastering voice. Callbacks (engine and voice: passes, buffer start and end, loop end, stream end) run on the engine thread, which may re-enter the
API; operation sets defer changes until `CommitChanges`. `GetPerformanceData` reports the engine's time in passes, the voices and the latency.
X3DAudio (`X3DAudioInitialize`, `X3DAudioCalculate`: the matrix from distance curves, cones and constant-power panning over the speaker layout,
Doppler, LPF and reverb levels, the inter-aural delay) is in the same DLL, as in Windows.

## Tests

- `tests/run_host_tests.sh PipeWireTest`: codecs (round trips, every truncation, bit flips), conversion both ways (every format through
  `convert_from_s16` and back), the graph mixing native, Linux and Windows streams on a real mixer, recording from a fake microphone through a real
  capture hub (the source, record nodes and links, gains, pause, unplugging), both protocols as clients drive them and fed garbage (every truncation of
  CREATE_RECORD_STREAM and bit flips), the Windows service both ways, the AF_UNIX transport, several threads at once. client-node: the setup a
  `pw_stream` goes through, playback and recording cycles on a real mixer and hub, an empty cycle, a cycle that times out, pause, 600 truncated or
  bit-flipped update, port_update and set_active messages, a descriptor maker that fails; and the same through `SocketServer` over AF_UNIX
  sockets, with the memfds and eventfds it makes received by SCM_RIGHTS (the client writes the memory the kernel reads; nothing leaks).
- `tests/run_host_tests.sh UnixSocketTest`: eventfd too (the counter, `EFD_SEMAPHORE`, `EFD_NONBLOCK`, the limit, `poll()`, a blocked read woken
  from another thread, passing it with SCM_RIGHTS).
- `tests/pipewire_interop.sh`: the host's real `pw-dump`, `pw-cli`, `wpctl`, `pactl`, `paplay` and `parecord` against
  `tests/pipewire/PipeWireHostServer.cpp` (the same graph and protocols on host sockets, a microphone that hears 880 Hz left and 1320 Hz right). Two
  `paplay` at once; the mixed output must hold both tones at the right levels; `parecord` must record each side's tone; the source's and a source
  output's volume set through `pactl` must read back. The host's real `pw-play` and `pw-record` (libpipewire 1.x) go through client-node (the
  host server's maker makes real memfds and eventfds): the mix must hold `pw-play`'s tone, and `pw-record` must record each side's tone.
- `tests/pipewire_emulated.sh [x86_64|aarch64|riscv64] [all|winmm|dsound|record|xaudio2|pwnode]`: on the kernel's CPU emulator, `tests/windows/winmmtest.c` (with the
  OS's own DLLs) and `tests/linux/pulsetest.c` (no C library) play at the same time as a native stream; the mix must hold all three tones at once.
  The second run is `tests/windows/dsoundtest.c`: a DirectSound loop (600 Hz) and a WASAPI event-driven client (700 Hz) from one program, with the
  Linux program and the native stream, and all four tones must be in the mix at once (the DLLs are also put in the RAM disk's System32, since ole32
  loads `dsound.dll` and `mmdevapi.dll` while the program runs). The third run records from the emulator's microphone (880 Hz left at 8000, 1320 Hz
  right at 4000): `tests/windows/recordtest.c` through `waveIn` (22.05 kHz stereo), DirectSound capture (16 kHz float mono) and WASAPI capture (48 kHz
  stereo), each checking both tones' levels, while `pulsetest` records the same microphone with a PulseAudio record stream. The work folder is one
  per checkout (`/tmp/tcos-pwemu-<checksum of the path>`), so another worktree's objects and DLLs are never taken for this one's. The fourth run is
  `tests/windows/xaudio2test.c`: a source voice into the mastering voice and one through a submix (400 and 600 Hz in the mix with the others),
  callbacks, loops, operation sets, matrices, the frequency ratio, MS-ADPCM through a volume meter, the FX objects and X3DAudio. (Its mastering
  voice is 24 kHz mono: the emulator runs about 14 million instructions a second, and a 48 kHz stereo graph of float voices would not keep up.)
  The fifth run is `tests/linux/pwnodetest.c` (no C library) next to `winmmtest`: the `pw_stream` side of client-node through the OS's own system
  calls (SCM_RIGHTS, `mmap(MAP_SHARED)` of the passed memfds, `ppoll` and `read`/`write` on the eventfds), 4 s of 500 Hz, then half a second of
  the microphone on an input node, each side's tone checked; the mix must hold 300, 400 and 500 Hz at once. The sixth run (`libpipewire`, x86-64,
  when the machine has `pw-cat`) runs the host's real `pw-cat` with its libraries, SPA plugins (`libspa-support`, `libspa-audioconvert`, ...),
  modules (`protocol-native`, `client-node`, `adapter`, ...) and `client.conf` copied into the Linux tree: `pw-play` of a 500 Hz WAV must be in
  the mix with the native stream, and `pw-record -n 24000` must write a WAV (copied out with `TCOS_EMU_SAVE`) that holds 880 Hz on the left and
  1320 Hz on the right. `TCOS_EMU_STRACE=1` prints every system call of the emulated program with its result.
- `tests/run_host_tests.sh UnixSocketTest`: timerfd (one-shot, periodic, absolute, disarmed, `poll()` and a blocking read woken when it fires, also
  when another thread arms it meanwhile) and epoll (data, MOD/DEL, EEXIST/ENOENT/EPERM, one-shot, a timer and a socket in one set, a hang-up, a
  descriptor closed while in the set, a waiter woken by another thread's `EPOLL_CTL_ADD`, signalfd) and PI futexes (four threads sharing one
  lock through glibc's fast path and `FUTEX_LOCK_PI`/`UNLOCK_PI`; EDEADLK, EPERM, EAGAIN, ETIMEDOUT).

## Not done

- **libpipewire in QEMU.** The real `pw-play` and `pw-record` run on the CPU emulator over the OS's system calls; they have not been run in the real
  kernel under QEMU yet (the same files would have to be put on the disk image, as `LinuxDynamicPrograms.md` describes).
- **Signals and realtime.** A signalfd never fires (signals are not routed to descriptors), so `pw-cat` cannot be stopped with Ctrl+C through its
  loop; `module-rt` finds no RTKit or session bus and runs without realtime priority (as it does on Linux without them).
- **client-node limits.** One port per node, mono or stereo at the port (a wider stream is downmixed by the adapter in the program), no metadata
  (`SPA_META_Header`, `Busy`), no client-allocated buffers (`port_buffers`), no `pw_filter` or `pw_stream` with `PW_STREAM_FLAG_DRIVER`, no MIDI
  or video. The `adapter` factory (`pw-cli create-node adapter`) is refused.
- **Recording one stream.** PulseAudio's `direct_on_input` (what pavucontrol's per-stream level meters use) is refused; the monitor holds the whole mix.
- **More than one source.** The graph follows the hub's one microphone, as the sink follows the mixer's one output (`move-source-output`,
  `set-default-source` to another device do nothing).
- **More of Windows audio.** XAudio2 has no xWMA or XMA sources, no PLAY_TAILS, and no 2.7 COM classes (`xaudio2_7.dll`, `CLSID_XAudio2`);
  its reverb has no early reflections or room filters. WASAPI has no exclusive mode (refused);
  the master volume has no balance.
  DirectSound has no effects (`SetFX` answers `DSERR_FXUNAVAILABLE`, no capture effects such as AEC or noise suppression) and no property sets
  (`IKsPropertySet`, EAX). WASAPI capture packets are always 10 ms
  (`IAudioClient3`'s smaller shared periods are not offered for capture either).
- **More than one sink.** The graph follows the mixer's one output device; switching devices is the `AudioRouter`'s, not PipeWire's
  (`move-sink-input`, `set-default-sink` to another device do nothing).
- **Formats, controls.** PulseAudio sample-rate updates (`UPDATE_PLAYBACK_STREAM_SAMPLE_RATE`: an `AudioStream`'s rate is fixed), passthrough
  formats, volumes beyond front left/right (a playing stream has a balance: `Graph::set_balance`; other channels follow the louder side; the sink
  and source have none), PipeWire `Route`/`Profile` params, and links made or destroyed by clients.
- **Not run in QEMU** with a real dynamically linked libpulse program: the Linux layer's dynamic loading of such programs is not validated yet.
