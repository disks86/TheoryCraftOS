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
| PipeWire's native protocol | `NativeServer.{hpp,cpp}`; `client-node` (pw_stream, pw_filter) and the `adapter` factory's virtual sinks in `NativeClientNode.cpp` |
| MIDI: the bus every program's MIDI port is on (to the synthesizer, from keyboards), raw MIDI and UMP | `MidiBus.{hpp,cpp}` |
| Video: what a producer shows, for its watchers and the kernel (packed and planar YUV, RGB, grey, encoded) | `VideoChannel.{hpp,cpp}` |
| The machine's cameras as Video/Source nodes of the kernel | `CameraSource.{hpp,cpp}` |
| The screen as a Video/Source node (screen sharing) | `ScreenSource.{hpp,cpp}`, `gui::IFrameTap` (`src/Gui/Window.hpp`), `dxgk::IPresentTap` (`src/Kernel/Dxgkrnl/Adapter.hpp`) |
| Video conversion for watchers | `VideoConvert.{hpp,cpp}` |
| The server's GPU images for DMA-BUF watchers (exports of the Direct3D layer) | `DxgGpuImages.hpp`, `IGpuImages` (`SocketServer.hpp`) |
| Decoding encoded video for raw watchers and the viewer (Motion JPEG itself, the other codecs through `IVideoDecoders`) | `VideoDecode.{hpp,cpp}` |
| The kernel's own watcher (the desktop's Video Viewer) | `VideoWatcher.{hpp,cpp}`, `src/Desktop/VideoViewer.{hpp,cpp}` |
| A virtual sink's audio: a mixer of its own over a silent device on the graph's clock | `VirtualSink.hpp` |
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

- **client-node (libpipewire's `pw_stream` and `pw_filter`).** `pw-play`, `pw-record`, `pw-midiplay`, camera and screen-sharing programs and every
  `pw_stream` or `pw_filter` program export their own node: Core `create_object("client-node", ..., version 6)`. The server hands the client, with
  SCM_RIGHTS, the memory it shares with the kernel (memfds: the node's `pw_node_activation`, the ports' `spa_io_buffers` areas, each port's
  buffers) and two eventfds (the wake and done signals of `transport`). At `set_active` (or the first `port_update` after it, so that the update
  before can say the stream's format) the graph node is made, by `media.class` (and the ports' formats): audio into a `PlaybackStream` or from a
  `RecordStream` (or into and out of a virtual sink the stream names with `target.object`), MIDI onto the graph's `MidiBus`, video into a
  `VideoChannel` of a producer's or from one. The node is bound (`bound_props`) and told its clock and position again (`set_io` Clock and Position:
  the activation now names the node, and a driver is driving when they name the same clock).
  - **Adapters and raw nodes.** A `pw_stream` of audio or video wraps an adapter, which has no ports until it is configured: audio gets a PortConfig
    in convert mode (F32 at the stream's own rate and channel count, mono or stereo, from the EnumFormat in its update, else 48 kHz stereo; F32
    because the AVX2 S16 output of audioconvert assumes an aligned destination and crashes), video in passthrough mode (the stream's own port). A
    node whose ports came before it was active (a `pw_filter`; a MIDI `pw_stream`, which has no adapter) is raw and keeps its ports: up to 16 of
    them, each with its own io area. A filter's float audio ports (`32 bit float mono audio`) are channels: its outputs one stream to the speakers
    (a channel each, in the usual order: FL FR, then FC LFE RL RR ... folded into the speakers' two), its inputs one stream recorded from the
    microphone (each port a channel). A filter with both (an effect, `Audio/Filter`) is one graph node that does both (`Graph::create_filter`).
  - **Formats.** The adapter's audio port gets the server's PortConfig format; any other port its own EnumFormat fixated (every Choice its default:
    a camera's size and frame rate, a MIDI port's `application/control`, a float port's DSP format); a video watcher the format of the producer it
    is linked to (`target.object` names the producer by `node.name`, `object.serial` or id, else the first producer; until there is one it waits).
  - **Buffers and metadata.** Once a port took its format and says what it wants (its Buffers param: how many, 2 to 16; the data's size and stride;
    the memory types; its Meta params), the server makes its buffers in one memfd: each buffer's metadata (`SPA_META_Header`, `Busy`, `VideoCrop`,
    `VideoDamage`, `Cursor`, ... as the port asks, up to 8, each rounded to 8 bytes), a chunk per data block, then the data blocks (audio at least
    8192 frames, MIDI 16 KiB unless it asks for more, video what the port asks; MemPtr, or MemId with page-aligned data when the port takes only
    memfds). A planar video port that asks for as many blocks as its format has planes (`SPA_PARAM_BUFFERS_blocks`: GStreamer, libcamera) gets a
    block a plane; otherwise the planes lie one after another in one block (each after the last at the stride it implies: the luma's, halved for
    I420's chroma, the same for NV12's). The server
    fills in `SPA_META_Header` (time stamp, sequence number) on what it gives, reads a producer's time stamp from it, and does not write a buffer
    whose `SPA_META_Busy` count is not 0.
  - **Client-allocated buffers.** An output port that can allocate (`SPA_PORT_FLAG_CAN_ALLOC_BUFFERS`: `PW_STREAM_FLAG_ALLOC_BUFFERS`, a DMA-BUF or
    memfd producer) gets `port_use_buffers` with `SPA_NODE_BUFFERS_FLAG_ALLOC`: the metadata and chunks in the server's memory, no data (its type is
    the mask of types it may use, as PipeWire says it). It answers with `port_buffers`: each buffer's memfd per block (passed with SCM_RIGHTS),
    offset and size, which the server maps (`SocketServer` turns a client's memfd into a descriptor of the file's bytes) and reads in place. A
    DMA-BUF is refused (ENOTSUP); so is a buffer outside its memfd or a descriptor that did not come.
  - **Format changes while streaming.** A video producer that says a new EnumFormat (`pw_stream_update_params`) that fixates to another format,
    and a watcher whose producer changed, are negotiated again while the node runs: the port is given the new Format (the program drops its
    buffers), its old memory is removed (`remove_mem`), and new buffers follow once it says what it wants of them. Memory ids are given out as the
    program's pool does (`pw_map`: the last removed first), or its pool refuses the next `add_mem`.
  - **Cycles.** Each cycle (the stream's `node.force-quantum`, or its `node.latency` "N/D" as N * rate / D frames, else 1024; 64 to 8192) the server
    writes the clock into the activation, sets it TRIGGERED and signals the wake eventfd; the client processes and sets FINISHED, and what its
    outputs produced (HAVE_DATA) is taken and the buffer handed back. Inputs are filled before the trigger. Playback to the speakers is triggered
    while its stream holds less than two cycles (at least 3072 frames, into the speakers' mixer or a virtual sink's), recording once a quantum is
    recorded, MIDI and video on the server's clock (a video watcher when its producer has a new frame). A cycle that never finishes is given up after 100 ticks and triggered
    again; `set_active(false)` pauses. The `pipewire` thread ticks every 5 ms while nodes run (50 ms otherwise).
  - **Drivers.** A video or MIDI node with `node.driver` (`PW_STREAM_FLAG_DRIVER`: a camera, `video-src`; the program calls
    `pw_stream_trigger_process` on its own timer) drives itself: its activation requires its own trigger once a cycle, the server never triggers it,
    and takes what its outputs' io areas hold (and fills its inputs) as they say. An audio driver follows the server's clock instead (as in PipeWire,
    where the sound card's driver wins): its position is an activation of the server's (`set_io` Position to another memfd), so it is not driving
    and is triggered as any other stream.
  - **MIDI.** A MIDI output's buffers hold a Sequence of controls: `SPA_CONTROL_Midi` (raw MIDI 1.0 bytes, running status, real-time bytes in the
    middle of a message, whole system exclusive messages up to 64 KiB) and `SPA_CONTROL_UMP` (MIDI 1.0 and MIDI 2.0 channel voice and system
    packets, scaled down; system exclusive as 64-bit Data Messages, put together across controls and cycles); every message goes to the bus with
    the time its control's offset says (the cycle's distances kept, a cycle late): the synthesizer gets each at its time (the bus schedules them;
    the server ticks every millisecond while some wait). A MIDI input is given a Sequence of what the bus brought whose time has come, each at its
    place in the cycle before (the distances between messages kept): UMP for a port with `control.ump` (`32 bit raw UMP`; sysex in Data Message
    pieces), else raw MIDI. System exclusive reaches a USB synthesizer (USB-MIDI CIN 4..7 packets) and the MPU-401 whole; the software
    synthesizer takes the GM, GM2, GS and XG resets; a USB keyboard's system exclusive messages are put back together from its packets and reach
    the programs' inputs (and a sound maker). The bus sends programs' notes to the synthesizer (in the kernel: a USB synthesizer or the MPU-401 when there is one, else the
    software synthesizer through the mixer, which keeps running a few seconds after a program's last note) and to every other program's MIDI
    input; keyboards' keys go to the programs' inputs too. The graph shows the bus as the node `tcos_midi.bridge` (`Midi/Bridge`), each program's
    MIDI node linked to it.
  - **Video.** A producer's frames are copied out of its buffer into its `VideoChannel` at each cycle, whole; a watcher's input is given the newest
    frame it has not had, at its own stride. Raw formats: packed RGB (RGBx, BGRx, xRGB, xBGR, RGBA, BGRA, ARGB, ABGR, RGB, BGR, RGB16, BGR16, RGB15,
    BGR15), packed YUV (YUY2, UYVY, YVYU, AYUV), planar YUV (I420, YV12, Y41B, Y42B, Y444, NV12, NV21) and grey (GRAY8, GRAY16); up to 8192 on a side.
    Encoded video (H.264, MJPEG, VP8, VP9, H.265 and the other video subtypes but Bayer) is carried as whole frames of any length (16 MiB at most).
    The kernel can take a channel's frames too (`Graph::video_of`).
  - **The screen.** `ScreenSource.cpp`: a Video/Source node of the kernel's (`tcos-screen`) whose frames are what the desktop draws, BGRx at the
    screen's size, at most 10 a second and only while watched: the renderer reads its frame back between drawing and presenting
    (`IRenderer2D::read_frame`: the software renderer's surface, the device renderer's back image) and the desktop's window hands it to its
    `gui::IFrameTap`; a new watcher makes the desktop draw once, and while the screen is still its last frame is sent again once a second. A
    program that presents straight to the display (the Direct3D kernel's present: a full-screen Direct3D program, the conformance scenes) gives
    frames too, through `dxgk::IPresentTap` (the shown image read back after the present): while it presents, the desktop's frames are passed over,
    and 250 ms after its last present the desktop is drawn again. (The desktop's 3D test window is in the desktop's frames: its cube is composed into
    them.) For OBS, video calls' screen sharing, `pw-record`-style recorders.
  - **Conversion and scaling for watchers.** A watcher whose EnumFormat does not list its producer's format is offered the first format it lists
    that the frames can be converted to; one that cannot take its producer's size (a fixed size, a list without it, a range it is outside) is
    offered its own (the fixed one, the list's first, the producer's clamped into the range). Each frame is converted and scaled
    (`VideoConvert.cpp`, through RGBA of 16 bits a component, so 10-, 12- and 16-bit formats keep their bits; BT.601 limited range worked out on
    samples scaled to 16 bits; averaged where it shrinks and bilinear where it grows), for every raw format:
    packed RGB of 8, 16 and 10 bits a component, ARGB64, float RGBA and dsp video, planar and semi-planar YUV of 8, 10 and 12 bits (I420 ... P010,
    NV16, NV24, NV61), packed YUV (YUY2, UYVY, YVYU, VYUY, AYUV, AYUV64) and grey. A raw watcher of an encoded producer of a known size is offered
    raw when its frames can be decoded, and given each picture decoded (`VideoDecode.cpp`): Motion JPEG always (the JPEG decoder starts from the
    standard Huffman tables, which webcams' MJPEG leaves out); H.264 and H.265 (Annex B access units), MPEG-2, VC-1, VP8 and VP9 through decoder
    sessions of the machine's video codecs (`Graph::set_video_decoders`; main gives it `SystemVideoCodecs`), a picture a codec holds back coming out
    with a later frame.
  - **DMA-BUF producers.** A producer that allocates its own buffers may give GPU images (`SPA_DATA_DmaBuf`) when they are exports of `/dev/dxg`:
    the transport wraps one as a CPU copy (`Descriptor::gpu`, `sync_from_gpu` through `DxgBufferFile::copy_image`), a frame is copied out of the
    image before it is read when something needs the pixels, and a frame the GPU still draws is handed back unread.
  - **Zero-copy.** A producer that allocates (DMA-BUFs or memfds, all of one kind) shares its buffers (`VideoShare`, carried by its channel and its
    frames). A watcher that takes them as they are - the producer's format and size, and its Buffers' `dataType` names that memory (or, saying no
    types, its EnumFormat names a modifier and they are DMA-BUFs) - is given the producer's own descriptors (`add_mem` of type DmaBuf or MemFd, the
    data `MemId`s, read-only) and each cycle the buffer of the newest frame, its chunks and header written in the watcher's own part of it: nothing
    is copied. The producer's buffer is held while a watcher may read it (given and not yet recycled through its io, or the newest frame) and an
    older one nobody holds goes back to the producer instead (one a cycle: the io's room). When every watcher of the producer takes its buffers so,
    no copy is made at all (the channel's frame has no pixels); a watcher in memory, a converting one or the kernel's own makes the server copy again.
    A watcher already running is negotiated again when its producer's buffers become shareable to it, or stop being shared. A frame is taken only
    once it is drawn: with explicit sync, once its acquire point is reached; without, once the GPU is done with its DMA-BUFs (`Descriptor::gpu_ready`,
    `dxgk::export_idle`). One that is not is held (shown at a later tick, not dropped; a newer one in the meantime passes it over), so a watcher
    given a shared DMA-BUF never sees a frame half drawn and needs no fence of its own. On a cycle where a producer the server drives gives nothing,
    a buffer it shared that nobody holds goes back to it (the io has room then).
  - **Explicit sync.** A video producer that allocates and offers a layout depending on `SPA_META_SyncTimeline` (`SPA_PARAM_BUFFERS_metaType`; its
    data blocks and two SyncObj blocks a buffer: its acquire and release timelines, as PipeWire's video-src-sync does) is given that layout - the
    plain one it offers after it is kept for when the server cannot - and the RELEASE feature meta when its SyncTimeline Meta asks for it. Its
    timelines are read and raised through the descriptors (`timeline_value`, `timeline_signal`: the /dev/dxg fences a Linux program exports as DRM
    sync objects, `DxgSyncFile`). The server clears `UNSCHEDULED_RELEASE` (its promise) when it can raise the release timeline, and raises it to the
    frame's release point when the buffer goes back; timelines it cannot read (an eventfd standing in) are not promised, and the frame is taken as the
    GPU allows.
  - **The server's DMA-BUFs.** A watcher that takes DMA-BUFs only (or names a modifier in its EnumFormat) of a producer whose buffers it cannot
    share is given GPU images of the server's, one a buffer (`IDescriptorMaker::make_gpu_image`: the transport's `IGpuImages`, which main makes
    with `DxgGpuImages` over the Direct3D layer's first adapter - exports of `/dev/dxg`, so a client imports them as any DMA-BUF of the system).
    They are 32-bit RGB (BGRx or BGRA, chosen when the watcher lists one); each frame is written into the image's CPU copy at its pitch and then
    into the image (`sync_to_gpu`). The Format names the modifier (linear, 0) when the watcher's EnumFormat named one.
  - **The Video Viewer.** A Start menu app of the desktop (`src/Desktop/VideoViewer.cpp`) lists the graph's video producers, watches the one
    picked (through `VideoWatcher.cpp`, a watcher node of the kernel's, so a camera streams while it is shown) and shows its newest picture fitted
    to the window, converted to RGB (encoded video decoded as for a raw watcher); it says when a source went, cannot be decoded here, or sent a
    picture that cannot be decoded.
  - **The kernel's cameras.** Every camera `SystemCameras` finds is a Video/Source node of the kernel's (`CameraSource.cpp`: `tcos-camera-<id>`,
    described by the camera's name) that programs watch as any producer. It streams only while watched: started at 640 x 480, 30 frames a second
    (or the camera's nearest mode) when its first watcher comes, stopped two seconds after the last one went; its newest picture goes out as I420.
    The kernel's loop updates them every 20 ms; a camera pulled out takes its node with it.
  Descriptors are the transport's (`IDescriptorMaker`): `SocketServer` makes memfds (`MemoryFile`, whose block the kernel reads and writes in place
  while the client maps it) and eventfds (counters in the AF_UNIX manager, so `poll()`, wake-ups and SCM_RIGHTS take them as sockets;
  `EFD_NONBLOCK`, as PipeWire makes them: libpipewire empties one with a read that must not wait), sends each take's descriptors with its first
  bytes, takes the descriptors clients send (`receive_with_fds`: each message's `n_fds` are the next ones that came), and releases what the
  protocol let go of from its own thread (a graveyard).

- **The adapter factory: virtual sinks.** `pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=... }'` (and
  `pw_core_create_object(core, "adapter", ...)`) makes a virtual sink: a graph node (`Audio/Sink`, or the media class given) with input ports and
  as many monitor ports (`audio.channels` or `audio.position`, mono or stereo; `audio.rate`, default 48 kHz), bound to the client's proxy, at most
  16 of them. Its audio is a mixer of its own (`VirtualSink.hpp`: an `AudioMixer` over the silent `SoftwareAudioDevice`, which plays at the real
  rate on the graph's clock; `Graph::refresh` pumps it), so streams of every protocol play into it as into the speakers and what records from it
  is a tap of that mixer, made the sink's own rate and channels on the way (`CaptureStream::set_shape`: a mono sink's monitor holds both sides
  averaged, a 16 kHz one loses what 16 kHz cannot carry; the mixer itself works at the speakers' 48 kHz stereo) (`Graph::create_playback`/`create_record` with the sink as target). PipeWire streams name it with `target.object`;
  PulseAudio lists it with its monitor (`<name>.monitor`), plays into it (`paplay --device=<name>`), records it (`parecord
  --device=<name>.monitor`, or the sink's own name), sets its volume (its mixer's master volume) and reports it in subscriptions; Windows programs
  find it as a WASAPI render endpoint of its own (NtTcosAudio `VIRTUAL_SINKS`; `OPEN`/`OPEN_CAPTURE` with its node; its endpoint volume is its
  own; loopback on it records it), as a winmm wave output device after the speakers (named after it; its device volume is its own) and as a
  DirectSound device (`DirectSoundEnumerate`, its GUID the speakers' with its node in the last bytes, as its endpoint id). With `object.linger=true` it stays when its client goes (it is the kernel's then); `Core destroy` of its proxy
  removes it, and its streams make and hear silence. Other SPA factories (`api.alsa.pcm.sink`, `audiotestsrc`, ...) are refused with `-ENOTSUP`: no
  SPA plugin runs in the kernel.

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
STAT, LOOKUP_SINK, LOAD_MODULE / UNLOAD_MODULE / GET_MODULE_INFO(_LIST) for `module-null-sink` (`pactl load-module module-null-sink
sink_name=... sink_properties=... rate=... channels=... channel_map=...`: a virtual sink as pipewire-pulse makes one, the kernel's, so it stays
after pactl exits until it is unloaded; module indexes carry pipewire-pulse's 1 << 29 flag; the sink and its monitor name their module, and the
monitor reports the sink's own format; other modules answer `PA_ERR_NOENTITY`, arguments the module does not take `PA_ERR_INVALID`). Record streams (create, delete, cork, flush, latency, buffer attributes, name and property updates, RECORD_STREAM_KILLED): the
server sends what they record on their channel a fragment at a time (`fragsize`, 50 ms by default, 5 to 250 ms), and stops sending to a client that
does not read (the hub's ring drops the oldest audio instead). Source and source-output info and lists, their volume and mute (SET_SOURCE_OUTPUT_VOLUME
is command 98: the numbers from 98 on were one off before), kill-source-output, LOOKUP_SOURCE, the default source in the server info, source and
source-output subscription events. Sample formats: U8, A-law, µ-law, S16, S24, S24-32, S32 (both byte orders except S24-32 BE), float 32.

**PipeWire native**: Core hello/sync/pong/get_registry/destroy, Registry bind for Core, Client, Device, Node, Port, Link and Metadata, the info of
bound objects (sent again when they change), Client update_properties/get_permissions, Node and Port enum_params/subscribe_params (Props, EnumFormat,
Format), Node set_param(Props) for volume and mute (the source's too), the metadata's `default.audio.sink` and `default.audio.source`.
`client-node` (Core create_object): ClientNode update, port_update (its params: EnumFormat, Buffers, Meta; its info: flags, properties),
set_active, port_buffers (a port's own memfds), get_node and event (accepted, unused); the events transport, set_param (PortConfig), set_io
(Clock, Position), port_set_param (Format), port_use_buffers (with metadata; ALLOC for a port that allocates), port_set_io, port_set_mix_info,
command (Start, Pause), and Core add_mem for every memfd. `adapter` (Core create_object, `PipeWire:Interface:Node`) makes
`support.null-audio-sink` virtual sinks. Other factories (`link-factory`, `metadata`, `spa-node-factory`) and other `factory.name`s of the adapter are
refused with `-ENOTSUP`. MIDI nodes and ports show `application/control` as their format; video ones their producer's.

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
  sockets, with the memfds and eventfds it makes received by SCM_RIGHTS (the client writes the memory the kernel reads; nothing leaks). The MIDI bus
  (raw MIDI with running status, system exclusive, real-time bytes inside messages, cut-short messages; UMP both ways; routing; a full queue;
  3000 truncated or bit-flipped inputs; four listeners and four senders on threads at once) and the video channel (strides, cut-short frames, a
  producer and three watchers on threads checking every frame is whole). client-node's media: a raw MIDI output (no PortConfig, cycles on the
  clock, raw MIDI and UMP to the synthesizer) heard by a UMP input with a keyboard; a driver camera with `SPA_META_Header` and `VideoDamage` (never
  triggered, its frames taken from its io) and a watcher given its format and frames; a camera that allocates its own buffers (`port_buffers`:
  refused without its descriptor, outside its memfd, for a DMA-BUF); an audio driver made to follow; a filter's two float ports played as stereo;
  the adapter (a virtual sink played into and recorded from, a refused `factory.name`, destroying it, `object.linger`); 900 truncated or
  bit-flipped port_update (with blocks and encoded formats) and port_buffers messages. The transport takes a memfd and an eventfd from a client.
  Also: system exclusive (raw with real-time bytes inside, broken and unterminated, the 64 KiB limit; UMP Data Messages of every length and
  random packets), the queues' byte limit and `pop_if`, the bus's output on time (scheduling, its limit, from threads while another pumps), notes
  and sysex at offsets through client-node to the synthesizer at their times and to a UMP listener at the same distances; every video format's
  planes, planar and encoded publishing, 2000 random planes against random formats; NV12 in two blocks to a one-block watcher, the camera
  changing to I420 at another size (Format again, `remove_mem`, the removed memory ids given again, new buffers both sides), MJPEG; an effect
  filter (two inputs from the microphone, two outputs to the speakers, one node) and a 5.1 filter folded to stereo; a virtual sink from the
  Windows service (into it, its monitor, its volume, the sink going while streams are open); and the camera sources with a fake camera (a node
  each, started by a watcher, its I420 pictures, stopped two seconds after the last watcher, pulled out).
  Then: the 10/12-bit and other new formats' planes, conversion between every pair of the 8-bit formats (a grey ramp and red round trips, short
  and resized frames refused), a BGRA-only watcher of an I420 camera given converted frames; the screen source (only while watched, 10 a second,
  BGRx words, the size changing); the kernel's watcher (listing, watching, YUV converted, encoded and gone said); a DMA-BUF camera (copied out of
  the image, a busy frame handed back unread). `GuiTest` checks the window's frame tap, `GuiViewsTest` the Video Viewer with fake sources, and
  `UsbMidiTest` keyboards' system exclusive (every length, pieces across calls, broken, too long, 3000 random packets). Also: every raw format's
  round trip and 300 random format and size pairs, scaling (halved and doubled ramps, a watcher of a fixed smaller size), Motion JPEG decoded for a
  raw watcher and the viewer (a real file), the still screen's keep-alive (with the render thread and the loop at once). Then: H.264 (from the
  software encoder) and VP8 (a file) decoded picture for picture as a decoder of the software codecs gives them, the codec switched, truncated
  and bit-flipped frames; an H.264 camera given decoded and halved to a raw watcher, not offered raw without decoders; the viewer showing H.264;
  deep conversion (a 10-bit luma ramp of every value exact or within one and all distinct, 16-bit RGB through float, GRAY16 and 10-bit RGB exactly,
  deep YUV frames made again through each other); a mono and a 16 kHz virtual sink's monitor (both sides averaged; a 1 kHz tone kept, 20 kHz
  lost) against a plain one; module arguments (quotes, escapes, PulseAudio property names, truncation and bit flips) and `module-null-sink` over
  the protocol (load, list, info for old and new clients, kept after the client goes, unload, refused modules and arguments, a sink destroyed
  another way); the screen source taking a program's presents over the desktop's frames and giving them back. `DxgkrnlTest` checks the present
  tap (flips and blits handed over, not when unwanted or refused, adapters added after it). Zero-copy: a DMA-BUF camera of three buffers and a
  DMA-BUF watcher (given the camera's very descriptors as DMA-BUFs, read-only; each frame's buffer, nothing read from the images and no pixels in
  the channel; the camera's buffer held until the watcher recycled it, the oldest free one going back; a watcher that did not run let go of the
  buffer it never took), a memory watcher joining (the server copies again, from the image), a watcher naming a modifier (its Format says linear;
  given the camera's DMA-BUFs without saying types), watchers going (holds let go); a DMA-BUF-only watcher of a camera in memory given GPU images
  (each frame written into one at its pitch and synced), and one in NV12 given none. Explicit sync: a camera offering a SyncTimeline layout given
  it (three blocks a buffer, SyncObj types, the RELEASE feature meta), a frame whose acquire point is not reached held until it is, the promise
  made (flag cleared), the release point signalled when its buffer goes back; a DMA-BUF the GPU still draws held until drawn; a camera the server
  drives given back the buffers a watcher held when it went, on its idle cycles. `PipeWireDmabufTest` also sends a received DMA-BUF on to a
  second connection (the very export arrives) and makes a GPU image with `DxgGpuImages` whose pixels the client that receives it finds; the export
  is drawn (`gpu_ready`), and a fence exported as a timeline is read and raised through its descriptor (never lowered). `VideoCodecTest` decodes a
  JPEG without its DHT segments as with them, and `tests/run_host_tests.sh PipeWireDmabufTest` passes a real export of the Direct3D layer through
  the transport (SCM_RIGHTS) and checks its copy follows the image.
- `tests/run_host_tests.sh UnixSocketTest`: eventfd too (the counter, `EFD_SEMAPHORE`, `EFD_NONBLOCK`, the limit, `poll()`, a blocked read woken
  from another thread, passing it with SCM_RIGHTS).
- `tests/pipewire_interop.sh`: the host's real `pw-dump`, `pw-cli`, `wpctl`, `pactl`, `paplay` and `parecord` against
  `tests/pipewire/PipeWireHostServer.cpp` (the same graph and protocols on host sockets, a microphone that hears 880 Hz left and 1320 Hz right). Two
  `paplay` at once; the mixed output must hold both tones at the right levels; `parecord` must record each side's tone; the source's and a source
  output's volume set through `pactl` must read back. The host's real `pw-play` and `pw-record` (libpipewire 1.x) go through client-node (the
  host server's maker makes real memfds and eventfds): the mix must hold `pw-play`'s tone, and `pw-record` must record each side's tone.
  `tests/linux/pwmedia.c`, built against the host's libpipewire, runs its seven modes (below; `video-zerocopy`: the watcher's frames must be in the
  camera's own memfds, the same inodes; `video-sync`: the camera must be given the explicit-sync layout and the RELEASE feature); `pw-midiplay` plays a MIDI file (the server's
  synthesizer must get all its notes, and pwmedia's system exclusive message whole); `pw-cli create-node adapter` makes a lingering null sink that
  `pw-cli ls` lists, `pw-play --target` plays into and `pw-record --target` records from (the recording must hold the tone at full level); then
  PulseAudio reaches the same sink: `pactl` lists it and its monitor and sets its volume, `paplay --device=cli-null` plays into it and `parecord
  --device=cli-null.monitor` must record the tone. `pactl load-module module-null-sink sink_name=pa-null rate=16000 channels=1` must make a mono
  16 kHz sink that `pactl list short modules` and `pactl list sinks` (its description and owner module) show, `pactl load-module module-loopback`
  must fail, and `pactl unload-module` must take the sink away.
- `tests/pipewire_emulated.sh [x86_64|aarch64|riscv64] [all|winmm|dsound|record|nullsink|xaudio2|pwnode|libpipewire]`: on the kernel's CPU emulator, `tests/windows/winmmtest.c` (with the
  OS's own DLLs) and `tests/linux/pulsetest.c` (no C library) play at the same time as a native stream; the mix must hold all three tones at once.
  The second run is `tests/windows/dsoundtest.c`: a DirectSound loop (600 Hz) and a WASAPI event-driven client (700 Hz) from one program, with the
  Linux program and the native stream, and all four tones must be in the mix at once (the DLLs are also put in the RAM disk's System32, since ole32
  loads `dsound.dll` and `mmdevapi.dll` while the program runs). The third run records from the emulator's microphone (880 Hz left at 8000, 1320 Hz
  right at 4000): `tests/windows/recordtest.c` through `waveIn` (22.05 kHz stereo), DirectSound capture (16 kHz float mono) and WASAPI capture (48 kHz
  stereo), each checking both tones' levels, while `pulsetest` records the same microphone with a PulseAudio record stream. The work folder is one
  per checkout (`/tmp/tcos-pwemu-<checksum of the path>`), so another worktree's objects and DLLs are never taken for this one's. The `nullsink`
  run makes a virtual sink in the emulator (`TCOS_EMU_NULL_SINK`) and runs `tests/windows/virtualsinktest.c`: WASAPI must list it as a render
  endpoint of its own (found by its name, its id found again, its volume apart from the speakers'), and a loopback client on it must record the
  600 Hz another client plays into it and none of the speakers' native 300 Hz; then winmm's device 1 (400 Hz) and a DirectSound device made for
  its enumerated GUID (500 Hz) must be heard there too. The fourth run is
  `tests/windows/xaudio2test.c`: a source voice into the mastering voice and one through a submix (400 and 600 Hz in the mix with the others),
  callbacks, loops, operation sets, matrices, the frequency ratio, MS-ADPCM through a volume meter, the FX objects and X3DAudio. (Its mastering
  voice is 24 kHz mono: the emulator runs about 14 million instructions a second, and a 48 kHz stereo graph of float voices would not keep up.)
  The fifth run is `tests/linux/pwnodetest.c` (no C library) next to `winmmtest`: the `pw_stream` side of client-node through the OS's own system
  calls (SCM_RIGHTS, `mmap(MAP_SHARED)` of the passed memfds, `ppoll` and `read`/`write` on the eventfds), 4 s of 500 Hz, then half a second of
  the microphone on an input node, each side's tone checked; the mix must hold 300, 400 and 500 Hz at once. The sixth run (`libpipewire`, x86-64,
  when the machine has `pw-cat`) runs the host's real `pw-cat` with its libraries, SPA plugins (`libspa-support`, `libspa-audioconvert`, ...),
  modules (`protocol-native`, `client-node`, `adapter`, ...) and `client.conf` copied into the Linux tree: `pw-play` of a 500 Hz WAV must be in
  the mix with the native stream, and `pw-record -n 24000` must write a WAV (copied out with `TCOS_EMU_SAVE`) that holds 880 Hz on the left and
  1320 Hz on the right. Then `pw-cat --midi` plays a 12-note MIDI file (all 12 must reach the emulator's synthesizer), and `tests/linux/pwmedia.c`
  runs each mode: `video` (a driver camera, `PW_STREAM_FLAG_DRIVER` with `SPA_META_Header`, and a watcher in one program: 15 frames seen whole and in
  order), `video-alloc` (the same with `PW_STREAM_FLAG_ALLOC_BUFFERS`: the camera's own memfds), `video-zerocopy` (video-alloc with a watcher taking memfds only: given the camera's own, the same inodes), `video-sync` (video-alloc with explicit sync: the three-block layout and the RELEASE feature), `video-yuv` (NV12 with a block a plane, then the
  camera says I420 at another size while streaming: both are negotiated again and the watcher must see whole frames of each), `midi` (a
  `pw_filter` MIDI output's 16 notes, the note-offs half way through the cycle, and a 22-byte system exclusive message heard by a `pw_stream` MIDI
  input whole, its controls in order and some past the cycle's start), `filter` (an `Audio/Filter` with two float inputs and two outputs: its
  inputs must hear the microphone's two tones while its outputs play) and `adapter` (a virtual sink made with the adapter factory, an audio driver
  playing 500 Hz into it, a stream recording it: the tone at its level). `TCOS_EMU_STRACE=1` prints every system call of the emulated program with its result.
- `tests/run_host_tests.sh UnixSocketTest`: timerfd (one-shot, periodic, absolute, disarmed, `poll()` and a blocking read woken when it fires, also
  when another thread arms it meanwhile) and epoll (data, MOD/DEL, EEXIST/ENOENT/EPERM, one-shot, a timer and a socket in one set, a hang-up, a
  descriptor closed while in the set, a waiter woken by another thread's `EPOLL_CTL_ADD`, signalfd) and PI futexes (four threads sharing one
  lock through glibc's fast path and `FUTEX_LOCK_PI`/`UNLOCK_PI`; EDEADLK, EPERM, EAGAIN, ETIMEDOUT).

## Not done

- **libpipewire in QEMU.** The real `pw-play` and `pw-record` run on the CPU emulator over the OS's system calls; they have not been run in the real
  kernel under QEMU yet (the same files would have to be put on the disk image, as `LinuxDynamicPrograms.md` describes).
- **Signals and realtime.** A signalfd never fires (signals are not routed to descriptors), so `pw-cat` cannot be stopped with Ctrl+C through its
  loop; `module-rt` finds no RTKit or session bus and runs without realtime priority (as it does on Linux without them).
- **client-node limits.** An adapter's audio is mono or stereo at its port (a wider stream is downmixed by the adapter in the program); a filter's
  float ports are channels in the usual order (`audio.channel` is not read), and an effect's input is the microphone (not another stream).
  DMA-BUFs are taken only from producers that allocate (`port_buffers`) and only as exports of `/dev/dxg`. Zero-copy needs the watcher to take
  the producer's very format and size (a converting or scaling watcher gets a copy); a producer that shares buffers with a slow watcher that holds
  many of them runs short of buffers (frames are dropped, nothing breaks); after the watchers go, a producer that drives itself gives a buffer each
  cycle and gets one back, so as many stay held as were (PipeWire's client-node has no reuse_buffer either; a producer the server drives gets them
  back on its idle cycles). The server's own DMA-BUFs are 32-bit RGB only (no YUV images: the Direct3D layer exports no planar formats) and are
  written by the CPU. Watchers are not given timelines (the server waits for the acquire point for them); a watcher's own release is not waited
  for beyond its recycling the buffer. No tiled formats (NV12 tiles, v210) or modifiers other than linear. Encoded video is decoded only for a
  producer whose EnumFormat gives a size (the watcher's Format needs one), and AV1, MPEG-4 Part 2 and H.263 are not decoded (SPA has no AV1
  subtype; the others have no software decoder here); a raw producer is never encoded for an encoded watcher. `SPA_META_Busy` is honoured but never set by the server; cursor, bitmap and
  damage metadata are carried, not read. MIDI's sub-cycle timing costs a cycle of latency. No `set_activation` peers (the server is every node's
  only peer), links made by clients (`link-factory`), latency or tag params passed on; audio streams are not negotiated again after Start.
- **Video sources and showing video.** The screen source has no cursor of its own and no damage metadata; when a program and the desktop both
  present, the program's frames win until it has been quiet for 250 ms (the display shows whichever presented last). The Video Viewer shows one
  source at a time.
- **The adapter's other factories.** Only `support.null-audio-sink` (and PulseAudio's `module-null-sink`, which makes one); no other PulseAudio
  modules (`module-loopback`, `module-remap-sink`, ...). A virtual sink's mixer works at 48 kHz stereo: streams into it are mixed there and its
  monitor is made the sink's own format, so what the sink's rate and channels cannot carry is lost, but it is not mixed at that rate.
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
