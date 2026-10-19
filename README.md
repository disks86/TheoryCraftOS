# TheoryCraftOS

A minimal x86_64 UEFI operating system written in C++20 with a little
assembly (`src/Hardware/x86_64/cpu.S`). It boots under OVMF in QEMU. The boot console lists each piece of
hardware as it comes up (`[ ok ]`, `[info]`, `[warn]`, `[FAIL]`; also on the QEMU debug console, port 0x402, shown on your
terminal), then opens the desktop: a taskbar with a Start menu whose *Shut Down* entry stops the services, flushes storage,
releases the hardware and powers off. A boot with warnings or failures waits (up to 15 s) for a key before the desktop opens.

## Layout

- `src/main.cpp`, `console.*`, `uefi.hpp`, `runtime.cpp`: entry point, logging facade, UEFI types, compiler runtime stubs.
- `src/Allocators/`: `IAllocator` and the default `Allocator`, a cache-line block pool
  that gets its memory from an `IRandomAccessMemory` (`base()` / `usable_bytes()`, and the `extra_region()`s above 4 GiB). There are two pools: the
  low one (below 4 GiB, where the RAM class puts its main region) and a high one. `allocate()` / `allocate_aligned()` serve from the high pool first
  and fall back to the low one; **`allocate_dma()` (and `allocate_dma_view<T>()`) is for anything a device reads or writes by itself** - descriptor
  rings, DMA buffers, USB transfer buffers, virtio queues - and only ever returns memory below 4 GiB, where every controller (RTL8139, UHCI, OHCI
  among them) can reach it. Every such place in `src/Hardware` and the virgl driver was switched over; new drivers must use it. The UEFI RAM class claims the
  largest free regions above 4 GiB (all but a reserve at the end of each, up to 16 MiB, left to the firmware), `main.cpp` makes sure they are mapped and
  adds them to the allocator. `tests/AllocatorTest.cpp` covers the pools, a region that crosses 4 GiB and a randomized overlap/coalescing check (built without
  the address sanitizer, which keeps the address space above 2 GiB for itself).
- `src/ObjectContainer/`: `IObjectContainer<T>` (count, clear, allocation-free iteration through a plain
  `ObjectCursor`, plus range-for) and its implementations `List<T>` (growable array), `LinkedList<T>`
  (doubly linked, erase while walking), `Dictionary<K, V>` (chained hash map; `Hashing.hpp` has the
  default hash/equality policies, including C strings), `BinaryTree<T>` (self-balancing AVL search tree with
  in-order iteration and pre/post-order visitors), `SortedDictionary<K, V>` (ordered map on the tree),
  `Stack<T>`, `Queue<T>` (ring buffer) and `PriorityQueue<T>` (binary heap; `Comparison.hpp` has the ordering policy). Header-only templates; every allocation goes
  through the `IAllocator` they are given. `src/ContainerDemo` checks them at boot.
- `src/Utility/`: `UniquePointer<T>` and `SharedPointer<T>` (allocator-backed; atomic reference count; a
  pointer to a base class safely owns a derived object even without a virtual destructor), `Optional<T>`,
  `Variant<Ts...>` (visit, get, emplace) and the small `Move.hpp` helpers. Header-only; no weak pointers.
  `src/UtilityDemo` checks them at boot, with a shared pointer copied concurrently on every core.
- `src/Network/`: an IPv4 stack over any `INetworkInterface`, single-threaded and polled (`INetworkStack::poll()`).
  `NetworkStack` does Ethernet framing, ARP (cache + queue of packets awaiting resolution), IPv4 and ICMP echo;
  it owns a `UdpManager`/`UdpSocket` (`IUdpSocket`), a `TcpManager`/`TcpSocket` (`ITcpSocket`: RFC 793 state
  machine, RFC 6298 retransmission timer, fast retransmit, flow control, zero-window probing, listen/accept) and
  a `DhcpClient` (`IDhcpClient`: discover/offer/request/ack, renewal at T1, rediscovery on expiry) and a
  `DnsResolver` (`IDnsResolver`/`IDnsQuery`: A records over UDP with retries, TCP fallback on truncation, CNAME
  chains, TTL cache incl. negative answers, ID/port randomization; `INetworkStack::resolve()` blocks). `IIpLayer`
  is what the transports need from IP. Not supported: IPv6/AAAA, fragmentation, congestion control, SACK, window
  scaling, DNSSEC/EDNS. Time comes from `Hardware/Clock` (`IClock`, `TscClock` calibrated against the firmware timer; the CPU
  interface gained `timestamp_counter()`). `NetworkStackSet` runs one `NetworkStack` per network interface: DHCP is
  started on all of them at once (a silent network doesn't delay the others), and `resolve()` asks every configured
  interface's DNS server and takes the first answer. `src/NetworkStackDemo` reports each interface's lease, pings
  its gateway, resolves names through it, talks UDP/TCP to the host and serves HTTP on port 8080 on every interface.
  **TLS:** `src/Network/Tls/TlsSocket` is a TLS 1.3 / 1.2 client that is itself an `ITcpSocket`, so anything that takes a socket (the print transports,
  the IPP exchange, `TcpStream`) can be given one. Cipher suites: ChaCha20-Poly1305, AES-128/256-GCM (TLS 1.3 and the ECDHE_RSA / ECDHE_ECDSA suites of TLS
  1.2); key exchange X25519, P-256, P-384 (HelloRetryRequest, TLS 1.3 key updates and the extended master secret are handled); server signatures RSA
  PKCS#1 v1.5 and PSS, ECDSA. Certificates are validated by `src/Crypto/X509` (chain building from the supplied intermediates to a `TrustStore`
  - PEM bundles can be loaded, a pinned self-signed certificate works - signatures, dates, CA/path-length/key-usage rules, server-auth usage, SAN
  host names with `*.` wildcards or the IP address; no revocation, no name constraints) and the host check cannot be forgotten: with neither a name nor
  an address to check the handshake fails unless `skip_hostname_check` is set. The clock for the date checks is passed in (`TlsConfig::clock`; the
  firmware RTC is local time, so wiring one up is left to the caller), randomness comes from `crypto::IRandom` (`DrbgRandom` over `CpuEntropySource`:
  RDSEED/RDRAND, untested on hardware). The primitives are in `src/Crypto` (SHA-2, HMAC, HKDF, AES-GCM, ChaCha20-Poly1305, X25519, big integers, RSA
  verification, P-256/P-384, X.509) and were checked against OpenSSL's output; `tests/TlsReplayTest.cpp` plays recordings of real `openssl s_server`
  handshakes (every version/suite/certificate combination above, plus a retry request and key updates) back to the client, then damages them in
  many ways; `tests/tls_live.sh` runs the client against live `openssl s_server` and Python TLS servers (50 checks: combinations, failures, 2 MB echoes both
  ways, IPPS printing). Not supported: server mode, client certificates, session resumption / 0-RTT, renegotiation, SSL 3.0 and TLS 1.0/1.1 (refused),
  RSA key exchange, DHE, Ed25519 / RSA-PSS certificates, P-521, OCSP stapling checks. AES uses table lookups, so it can leak through the cache (ChaCha20
  is offered first for that reason); the elliptic-curve private-key code is constant-time apart from the group law's exceptional cases. Nobody has
  audited this; do not take it for a vetted library. IPP can run over it (`IppPrinter::set_tls`, `ipps://`), and discovery finds `_ipps._tcp`.
- `src/Hardware/StorageDevice/`: block storage. `IStorageDevice` reads/writes/flushes whole logical blocks
  (synchronous, any buffer alignment and size; `StorageDeviceBase` checks ranges and chunks big requests through a
  DMA-able bounce buffer). Native drivers: `Virtio/` (virtio-blk, virtio-scsi), `NVMe/`, `Ahci/` (SATA disks and
  ATAPI drives; `AhciController` is shared by the drives on its ports), `Ide/` (legacy parallel ATA/ATAPI, PIO),
  `Usb/` (mass storage, Bulk-Only Transport, over the bulk endpoints of the xHCI and EHCI drivers), and `UEFI/` (firmware
  Block I/O, for any controller the firmware drives). `SystemStorageDevices` finds them all (partitions are not
  listed); `src/StorageDemo` reports each device, cross-checks reads and, only on a device whose first block begins
  with `TCOS-SCRATCH`, tests write/flush/read-back. Not covered: floppy and SD/MMC (only through the firmware),
  and the LSI/MegaRAID/MPT/PVSCSI SCSI adapters (only if the firmware has a driver for them - the stock OVMF does not).
  USB hot-plug: `IUsbHostController::poll_hotplug()` / `port_generation()` (xHCI implements them: it reads the root ports' connect-change
  bits, releases a removed device's slot and endpoints and enumerates a new one; `UsbBus::poll_hotplug()` merges the controllers).
  `SystemStorageDevices::poll()` turns that into `StorageEvent`s: a stick that was plugged in is brought up and listed, one that was pulled
  out is shut down and unlisted (devices are shared pointers, `present()` says whether one is still there, and a request to a device
  whose port moved on to another device fails at once instead of reaching the new one). `SystemFileSystem` mounts every FAT volume of
  every device into a `MountTable` (the root is a folder per volume, named by its label, "Memory" when there is none) and its
  service thread ("hotplug", 250 ms) mounts and unmounts as sticks come and go; handles open on a removed volume answer
  `Status::NoMedia` and the volume is freed when they are closed; the desktop's file explorer refreshes itself. Limits: only xHCI
  reports hot-plug (EHCI/OHCI/UHCI controllers keep the devices present at start-up), no hubs, only LUN 0 of a stick, no media-change
  detection for card readers, no safe-eject command (FAT is read-only here, so nothing is lost by pulling a stick out), other device
  classes (audio, cameras, printers, keyboards) are still only taken up at start-up, and a machine without a screen does not poll.
  `tests/StorageHotplugTest.cpp` runs the whole chain against a fake controller with fake Bulk-Only sticks (a stick pulled out
  mid-command, a port that gets another stick before the removal was seen, partitioned and blank sticks, handles outliving volumes);
  `tests/StorageHotplugThreadTest.cpp` does it with real threads under ThreadSanitizer. The xHCI port-change handling itself has no
  test (it needs a simulated controller) and has not been run on hardware or QEMU.
  USB mass storage safe eject: `IStorageDevice::eject()` (USB sticks implement it) flushes the write cache (SYNCHRONIZE CACHE; a stick
  that answers ILLEGAL REQUEST has nothing to flush, any other failure aborts with the device untouched), allows removal (PREVENT ALLOW
  MEDIUM REMOVAL) and stops the unit (START STOP UNIT, with the tray eject for an optical drive); afterwards `present()` is false and
  `ejected()` is true, so the existing removal path takes the device out of the list (a `StorageEvent` with `ejected`), and it is not found
  again until it is physically plugged in again. `SystemFileSystem::eject(volume, force)` does the whole job for a volume: refuses while
  anything is open on the drive (`InUse`; with `force` the handles answer `NoMedia`), ejects the device, then unmounts all its volumes
  (`Failed` leaves everything mounted); the memory volume is `NotEjectable`. The file explorer has File > Eject (the selected drive at the root,
  else the drive the current folder is on; the answer is shown in its status bar; `main` wires it to `SystemFileSystem::eject`). Limits: FAT
  is read-only here, so there is never unwritten data of ours to lose - the flush matters to future writers; USB sticks only (internal
  disks report `NotEjectable`); an optical drive whose tray was opened is not rediscovered without replugging it; no "eject on hot-plug
  button" or per-LUN eject. `tests/StorageEjectTest.cpp` covers the commands, a stick without or with a failing cache sync, an optical drive,
  in-use and forced eject, and the stick staying gone; the thread test now has an ejector thread running against the readers and plugger.
  USB HID hot-plug: several drivers watch the same ports, so `UsbBus::service_hotplug()` only makes the controllers do their work and
  each driver finds what changed through `UsbPortWatcher` (it remembers every root port's generation and reports ports with a
  new device; storage works this way too). `SystemInput::poll()` (or its "input-hotplug" service, 250 ms) takes up keyboards, mice
  and tablets plugged in later and drops those pulled out: the device first releases the keys and buttons it held
  (`KeyboardBase::release_all()`, `MouseBase::release_all()`), `CompositeKeyboard::remove()` / `CompositeMouse::remove()` hand over
  what it still had and nothing calls into it afterwards, and it is freed. The merged `keyboard()` and `mouse()` are thread-safe
  against that; the desktop is always given them (even with no device yet) and shows the pointer only while a mouse exists. Limits: xHCI
  only, no hubs, at most 8 keyboards and 8 mice (a ninth is left alone and not retried), a keyboard's LEDs and
  non-pointer/non-keyboard HID devices (gamepads, media keys) are not handled, and a machine without a screen does not poll.
  `tests/InputHotplugTest.cpp` and (ThreadSanitizer) `tests/InputHotplugThreadTest.cpp` cover it against a fake controller.
- `src/Hardware/AudioDevice/`: audio output. `IAudioDevice` plays one fixed format (48 kHz, 16-bit signed, stereo)
  and is polled: `write()` copies whole frames into the device's ring and returns how many it took, `queued_bytes()` /
  `free_bytes()` say how much is waiting, plus volume, mute and statistics (frames played, underruns). A cyclic-DMA
  driver reads the hardware position and `PlaybackRing` does the bookkeeping (played bytes are zeroed, so an underrun
  plays silence); poll at least every ~300 ms while playing. Drivers: `Intel/IntelHdaAudioDevice` (any HD Audio
  controller: CORB/RIRB verbs, finds an output pin and the DAC behind it, unmutes the path, stream 0 over a buffer
  descriptor list), `Intel/Ac97AudioDevice` (ICH AC'97, PCM-out engine over 32 descriptors), `Virtio/VirtioSoundDevice`
  (period buffers on the transmit queue; volume applied in software) and `Software/SoftwareAudioDevice` (discards
  samples at real-time pace; `SystemAudioDevices` uses it when no sound hardware is found). Capture: `IAudioDevice::capture()` gives the
  recording side (`IAudioCapture`: `read()`, `available_bytes()`, `flush()`, gain, mute, statistics incl. lost frames), filled
  by the hardware into a `CaptureRing` (HD Audio input stream from a mic/line-in pin, AC'97 PCM-in from the mic, virtio-sound
  receive queue, or silence from the software device). Not covered: other formats/rates (no resampling), jack detection,
  interrupts, capture from several inputs at once. `src/AudioDemo` plays a short tone at boot
  (`TCOS_SELFTESTS`); `tests/AudioTest.cpp` covers the ring and the software device, `tests/AudioCaptureTest.cpp` the capture ring, software input and hub, `tests/AudioConvertTest.cpp` the resampler, format conversion and pan law. The `run` targets give QEMU an
  `intel-hda` + `hda-duplex` with a `none` audio backend.
  USB audio: `Usb/UsbAudioDevice` drives USB Audio Class 1.0 and 2.0 devices (sound cards, DACs, headsets, microphones;
  `Usb/UsbAudioDescriptors` reads the entities and alternate settings, every length checked). It picks the 48 kHz alternate
  setting (16-bit stereo first, else mono, 24/32-bit or more channels, converted on the way), sets the UAC2 clock, plays and
  records over isochronous endpoints (packets queued ~24 ms ahead, silence when the ring is dry, mono duplicated on capture),
  follows an asynchronous device's feedback endpoint, and uses the feature unit for volume/mute (software gain without one).
  `SystemAudioDevices` lists USB outputs after the PCI ones (a record-only device comes after the software fallback).
  Isochronous transfers are a new polled API on `IUsbHostController` implemented on xHCI only (Configure Endpoint, one
  Isoch TD per packet, Start Isoch ASAP); UHCI/OHCI/EHCI devices are left alone. Limits: 48 kHz only (no resampling), no
  hubs and no hot-plug, no high-bandwidth (multi-transaction) endpoints, one output and one input stream per device, frames
  count as played when queued (about 24 ms early), and the mixer must call at least every ~20 ms (its 5 ms pump does).
  `tests/UsbAudioTest.cpp` covers it against a fake controller and fake UAC1/UAC2 devices; the xHCI isochronous path, the
  feedback loop and the real timing have not been run on hardware or QEMU.
  USB audio hot-plug: `SystemAudioDevices::poll()` finds headsets, DACs and microphones plugged in later (through `UsbPortWatcher`, like
  storage and input) and drops those pulled out (`UsbAudioDevice::present()` follows the port's generation; its `shutdown()` waits for
  a write or read running on another thread). Devices are shared pointers; when the last device that plays sound goes, the software
  fallback is brought up. `audio::AudioRouter` (service thread "audio-hotplug", 250 ms) keeps the sound server on the right devices:
  a newly plugged output becomes the mixer's device (`AudioMixer::set_device()`), when the mixer's device leaves it moves to the first
  other hardware output, else the fallback; the capture hub follows the same rules for microphones (`AudioCaptureHub::set_source()`,
  which also accepts no source - the hub now exists even without a microphone). Streams stay open throughout, the master volume and mute
  are applied to the new device, and what was cut for the old one (a few ms) is dropped. Limits: xHCI only, no hubs, a policy of
  "newest device wins" with no per-program routing or remembered preferences, no sample-rate conversion (as before), and audio
  playing to a device that is pulled out is lost rather than re-queued. `tests/AudioHotplugTest.cpp` covers the logic against a fake
  four-port controller; `tests/AudioHotplugThreadTest.cpp` runs it with real threads under ThreadSanitizer.
- `src/Audio/`: the sound server. Audio hardware plays one stream, so `AudioMixer` owns the device (the first one
  `SystemAudioDevices` found) and any number of programs mix through it: `open_stream()` gives an `AudioStream`
  (a 32 KiB ring in the fixed format, own volume and mute, `write()` never blocks, `close()` when done). `pump()` keeps
  the device topped up to ~50 ms, sums every stream with clipping (32-bit accumulator) and writes nothing when
  all streams are empty; `start_service()` runs it from the "audio" thread every 5 ms. Master volume/mute go to the
  device. Up to 32 streams. `AudioPlayer` (`Audio/AudioPlayer.*`) is a transport - play, pause, stop, seek, loop, volume, pan, peak level - over an `IPcmSource`
  (`MemoryPcmSource`; `WavFile::parse` reads 8/16-bit mono/stereo PCM WAV from memory) feeding one mixer stream; `update()` or its
  service thread keeps the stream full. `tests/AudioPlayerTest.cpp` covers it.
  Streams may use their own `StreamFormat` (4-192 kHz, mono/stereo, 16-bit or 8-bit): `write()` converts to the device
  format with `Resampler` (linear interpolation going up, box averaging going down) and `FormatConverter`, and
  `set_pan(-100..100)` places a mono sound (equal power) or balances a stereo one (`Pan.hpp`, compile-time table).
  Not covered: higher-quality resampling filters, surround, per-stream effects.
  `AudioCaptureHub` is the same idea for recording: one microphone, many `CaptureStream`s, each getting every frame recorded since
  it was opened (own gain/mute and own `StreamFormat`, converted as the audio arrives; oldest audio overwritten if a reader lags); the "capture" thread feeds them.
  `tests/AudioMixerTest.cpp` covers it; `src/AudioDemo` mixes two tones at boot (`TCOS_SELFTESTS`).
- `src/Hardware/VideoCodec/`: video encode/decode. `IVideoCodecDevice` (a GPU's video engine, or the software fallback) hands out
  sessions: `IVideoDecoder` (`submit(packet)` / `receive(frame)`, `flush`, `reset` for a seek) and `IVideoEncoder`, polled and
  non-blocking, with `VideoFrame` views in `Bgra8888`, `I420` or `Nv12` and a `VideoPacket` carrying pts and keyframe flag
  (`VideoTypes.hpp`; `VideoFrameBuffer` owns frame memory; `ColorConvert` does BT.601 conversions in integers, full or limited
  range). `Software/SoftwareVideoCodec` really decodes and encodes **Motion JPEG** (`Jpeg.hpp`: a baseline JPEG decoder - any
  sampling up to 2x2, restart markers, several scans - using the IJG accurate integer DCT, bit-exact with a reference decoder
  on 4:4:4 and grey, and an encoder with per-picture optimal Huffman tables, quality 1-100, 4:2:0) and **raw** I420/NV12/BGRA
  frames, and decodes **H.264 / AVC** (`H264.hpp`, see below). The vendor classes `NVIDIA/NvidiaVideoEngine` (NVDEC/NVENC), `AMD/AmdVcnVideoEngine` (VCN) and
  `Intel/IntelMediaVideoEngine` (Quick Sync) are what `SystemVideoCodecs` creates for a display controller of that vendor: they
  report `info().hardware_engine` but `accelerated` stays false - this OS does not drive those engines, so their sessions run on
  the CPU, and **H.265, VP9 and AV1 are not available** (and H.264 can only be decoded, not encoded) until a native driver exists
  (that is where it goes). Not covered: arithmetic JPEG, capture devices.
  The H.264 decoder (`H264*.cpp`, about 3,700 lines, integer only) covers Baseline, Main and High profile 8-bit 4:2:0 progressive video:
  CAVLC and CABAC, I/P/B slices, 4x4 and 8x8 transforms, quantisation matrices, intra 4x4/8x8/16x16, quarter-sample motion with
  multiple reference frames, spatial and temporal direct prediction, explicit and implicit weighted prediction, long-term
  references and reference list modification, multiple slices, I_PCM, frame cropping and the deblocking filter.
  Annex B streams (parameter sets in the stream) or, after `DecoderConfig::codec_config` is given an avcC record, length-prefixed
  NAL units as stored in MP4. Pictures come out in display order a few packets late (`flush()` drains them). **Not covered:**
  interlaced coding (field pictures, MBAFF), flexible macroblock ordering, data partitioning, SP/SI slices, lossless
  (transform bypass), bit depths above 8 and 4:2:2/4:4:4. It was checked bit for bit against ffmpeg on x264 streams (CAVLC and CABAC,
  B pyramids, weighted prediction, both direct modes, 8x8 transform, quantisation matrices, slices, constrained intra, noise at
  QP 8, odd sizes, open GOP, intra refresh) and on OpenH264 streams (long-term references, reordering), and fuzzed under ASAN/UBSAN.
  `tests/VideoCodecTest.cpp` includes a fuzz run of the JPEG decoder under ASAN/UBSAN.
- `src/Hardware/Camera/`: cameras. `ICamera` (polled: `start(width, height, fps)` or `start_mode()`, `pump()` services the hardware, `read_frame()`
  copies the newest unread picture into a `VideoFrameBuffer` in any pixel format; brightness, contrast, saturation and sharpness as 0..100)
  and `SystemCameras` (finds the cameras; `start_service()` runs `pump()` every 4 ms from the "camera" thread, which `main` starts when a camera
  is present - cameras are found at boot but only start streaming when a program asks). `Usb/UvcCamera` drives USB video class 1.0/1.1/1.5
  devices on xHCI: `UvcDescriptors` reads the formats (Motion JPEG; uncompressed YUY2, UYVY, NV12, I420), frames and rates and the alternate
  settings; `start_mode()` does the probe/commit negotiation, picks the smallest alternate setting that carries the payload and collects the
  isochronous payloads into pictures (frame ID, end-of-frame and error bits; a stream is joined at the next picture boundary). Pictures are
  decoded when read: JPEG through the software decoder (`UvcMjpeg` supplies the standard Huffman tables that most cameras leave out), packed
  YUV converted. Limits: xHCI only (isochronous API shared with USB audio), no hubs and no hot-plug, no bulk-streaming cameras and no
  high-bandwidth alternate settings (so a camera whose uncompressed or large formats need more than one transaction per interval offers
  only the modes that fit, usually Motion JPEG), no H.264/frame-based formats, no still capture, no pan/tilt/zoom or exposure controls, a
  new picture replaces an unread one, and `pump()` must run at least every ~10 ms at high speed. `tests/UvcTest.cpp` covers it against a fake
  controller and fake cameras (descriptor fuzzing under ASAN/UBSAN, assembly with lost and damaged payloads, MJPEG and YUV pictures); it has
  not been run with a real camera, on QEMU, or on the real xHCI isochronous path.
  USB camera hot-plug: `SystemCameras::poll()` finds cameras plugged in after start-up (through `UsbPortWatcher`, like storage, input, audio and
  printers) and drops those pulled out; `UvcCamera::present()` follows the port's generation, `pump()`, `start()` and `start_mode()` do nothing
  for a camera that left, and a stream running when it left stops (the event says `was_streaming`). The list holds shared pointers (`share()`;
  `at()` is only good while the camera stays), so a program that keeps a camera sees `present()` turn false and `read_frame()` return false
  instead of touching freed memory; a port that gets another camera never passes the old camera's stream on. The "camera" service thread
  pumps every 4 ms and checks for changes every 250 ms and `main` now always starts it (a camera plugged in later is taken up); each change
  goes to the debug console. Limits: xHCI only, no hubs, a program using a camera that leaves has to notice (nothing is pushed to it), and the
  pictures not yet read are lost with it. `tests/CameraHotplugTest.cpp` (fake four-port controller with fake cameras, including one pulled out
  while streaming) and, under ThreadSanitizer, `tests/CameraHotplugThreadTest.cpp` cover it. The thread test found two real races (an unlocked
  streaming flag and two programs starting the same camera at once, which leaked its buffers), both fixed.
- `src/Hardware/Printer/`: printing. `IPrinter` (polled, never blocks: `submit(job bytes)` queues, `poll()` delivers; `PrinterInfo` says
  languages, resolution, paper sizes, margins) with `NetworkPrinter` for a printer with its own print server: jobs go over raw TCP
  port 9100 (`RawSocketTransport`) or LPD port 515 (`LpdTransport`, RFC 1179, queue name configurable), eight jobs deep, a failed
  delivery retried twice before the job is dropped (`PrinterState::Error`). `SystemPrinters` is the registry (network printers
  are added by address; USB ones are found on the bus by `discover_usb()`, which `main.cpp` runs at boot). `PrintJob` builds a job: a PJL header (job name,
  resolution 300/600/1200, toner save), then either **PCL 5e** pages - bitmaps as raster graphics with PackBits rows, or text laid out
  here in a fixed-pitch font (UTF-8 in, Latin-1 out, wrapped and paginated) - or one **PostScript** program, and the PJL trailer.
  `Halftone` turns a `graphics::Surface` into a 1-bit `PageBitmap` (scaled to fit the printable area with box/bilinear
  filtering; threshold, 8x8 ordered or Floyd-Steinberg dots). `Brother/BrotherHl2070n` is the **Brother HL-2070N**: A4/Letter/Legal/
  Executive/A5/B5/A6, 600 dpi (HQ1200 at 1200), 23 ppm, no duplex, PCL and BR-Script 3 (PostScript), 4.2 mm unprintable margins, LPD
  queue `BINARY_P1`; tray 1 and the manual slot are selectable. The PCL output follows the PCL 5 reference and was verified by
  parsing it back and by the fake-network delivery tests, not on a physical printer (none was available): the first print on real
  hardware may need small adjustments. **`Usb/UsbPrinter`** is the USB Printer Class driver (class 7 / subclass 1, unidirectional or
  bidirectional setting - the latter preferred, selected with SET_INTERFACE), over the existing USB host stack (`Hardware/Bus/Usb`:
  xHCI, EHCI, OHCI and UHCI controllers with bulk endpoints): jobs go out on the bulk OUT endpoint 16 KiB per `poll()` through a
  64 KiB-aligned block, a failed transfer clears the endpoint halt and is retried, a job that fails three times is dropped after a
  printer SOFT_RESET. It reads the IEEE 1284 device ID (`Ieee1284`: manufacturer, model, command set - a model it knows, such as the
  HL-2070N, gets its real capabilities; any other gets what its command set promises), the port status (paper out, off line, error)
  and, on a bidirectional interface, the printer's replies (PJL status).
  **Status over SNMP:** `src/Network/Snmp/` is an SNMP v1/v2c manager (`SnmpMessage`: BER encoding and a bounds-checked decoder for every
  value type; `SnmpClient`: GET / GETNEXT / GETBULK over UDP 161, one request at a time, resent on a timeout, answers matched by
  request id and sender). `PrinterMonitor` uses it to read the Host Resources and Printer MIBs into a `PrinterReport`: engine state,
  error bits (low/no paper, low/no toner, door open, jam, off line, service requested...), the **page counter**
  (prtMarkerLifeCount), every **supply** with its level as a percentage (toner, drum, waste toner, fuser... - `low()` at 10 %, or 90 %
  full for a receptacle), the **alerts** (severity, code, the printer's text) and the front-panel text. `NetworkPrinter::request_report()`
  runs it inside `poll()`; `IPrinter::report()` is the common accessor (a USB printer fills in the error bits from its port status).
  **IEEE 1284.4 ("Dot4") multiplexing** (`Usb/Dot4`): a USB printer interface of protocol 3 carries logical channels over its one bulk
  pipe pair, as multifunction printers use to print and scan at once. `Dot4Session` is the host side with one data channel: Init,
  GetSocketID("PRINT"), OpenChannel, credit-based flow control (a data packet only against credit; CreditRequest when it runs out;
  the printer's own Credit and CreditRequest commands answered), packets cut to the size the printer agreed to, a byte-stream parser
  that copes with packets split over or sharing USB transfers, and CloseChannel / Exit. The packet formats and the command
  sequence were checked against HP's Linux driver source (hplip `io/hpmud/dot4.c`). `UsbPrinter` prefers a plain bidirectional
  interface, then protocol 3, then unidirectional; on a protocol-3 interface `submit()` and `read_back()` go through the channel, a printer
  with no credit to spare just makes `poll()` wait (not a failure), and a broken session is reopened. Not tried on a physical printer.
  **USB printer hot-plug:** `SystemPrinters::poll_usb()` finds printers plugged in after start-up (through `UsbPortWatcher`, like storage,
  input and audio) and drops those pulled out. `UsbPrinter` follows the port's generation (`present()`), has a lock so the hot-plug thread can
  shut it down while a program submits or polls, and a job that was queued when the printer left counts as failed and is reported as
  `PrinterEvent::lost_jobs`; a port that gets a different printer never passes the old one's jobs on. The list holds shared pointers
  (`share()`; `at()` is only good while the printer stays). `start_service()` runs the hot-plug check (250 ms) and delivers the USB
  printers' jobs (10 ms) on a "printers" thread - `main` starts it, which also means queued USB jobs now actually go out; network printers
  are still polled by whoever runs the network stack. Limits: xHCI only, no hubs, no re-queueing of lost jobs, a printer's name
  can repeat when two of the same model are plugged in. `tests/PrinterHotplugTest.cpp` (fake controller with fake printers, including one
  pulled out mid-job) and, under ThreadSanitizer, `tests/PrinterHotplugThreadTest.cpp` cover it; no real printer was involved.
  **SNMPv3 and traps:** `SnmpUsm` implements USM (MD5/SHA-1/SHA-224/SHA-256 authentication, DES/AES-128 privacy, key localization,
  engine discovery and time sync) on top of `src/Crypto` (hashes, HMAC, AES, DES, checked against OpenSSL); the v3 client was checked
  live against a pysnmp agent. `SnmpTrapReceiver` (UDP 162, needs IGMP/multicast-free unicast only) decodes v1/v2c/v3 traps and informs
  (v2c informs are acknowledged, v3 informs are not); `PrinterAlertListener` turns prtAlert and standard traps into events, and
  `SystemPrinters::enable_alerts()` refreshes the sending printer's report. Never run against a real printer.
  **IPP:** `src/Network/Ipp/` encodes IPP/1.1 requests and decodes responses (collections skipped, long strings cut, 1setOf values numbered)
  and `IppExchange` carries one request over HTTP/1.1 on TCP 631 (Content-Length, chunked and close-delimited answers, `100 Continue`
  skipped). `IppPrinter` is an `IPrinter` doing Print-Job (document format settable, busy answers retried), Get-Printer-Attributes
  (state, state reasons as errors and alerts, marker levels as supplies, serial from the device id), Get-Jobs and Cancel-Job. It has
  no page count (IPP has none for the printer; use SNMP); ipps:// works through `set_tls()`; no HTTP authentication, Validate-Job or Create-Job/Send-Document.
  Tested against a scripted server only; the encoder against bytes laid out independently.
  The agent is assumed to be at device index 1 with community "public" (`set_snmp_community()`). 
  **Discovery:** `PrinterDiscovery` looks for printers two ways at once: mDNS / DNS-SD (`src/Network/Mdns/`: queries for `_ipp._tcp`,
  `_printer._tcp` and `_pdl-datastream._tcp`, then SRV / TXT / A for what answers or only partly answers; TXT `rp`, `ty`, `pdl`,
  `usb_MFG` are read; goodbyes remove services) over multicast 224.0.0.251:5353 using the stack's IGMP support, and an SNMP v2c probe
  (sysDescr, sysName, hrDeviceType, prtGeneralPrinterName) sent to the subnet and limited broadcast addresses, which is how BRAdmin-style
  tools find print servers. Both land in one entry per IP address. `SystemPrinters::add_discovered()` turns the entries into printers
  (`make_discovered_printer`: IPP if advertised, else raw 9100 - assumed for a printer only SNMP found, else LPD; the printer details
  discovery cannot know are defaults). Not covered: IPv6, mDNS responses to our own conflicting names (we only browse), escaped dots in instance
  names, BRAdmin's own UDP protocol, scanning a subnet address by address (`probe_host()` asks one), raw ports other than 9100.
  Not tried on a real network or printer.
  Not covered: several simultaneous 1284.4 channels, the printer's Epson FX / IBM emulation, PCL 6
  (PCL XL) output, duplex and colour (the engine has neither). `tests/PrinterTest.cpp` covers PackBits, halftoning, job structure,
  text layout, both network transports and failures, and the USB driver against a scripted host controller (descriptors, device ID,
  chunking, retries, status, read-back, damaged descriptors); like the rest it has not been run against a physical printer.
- `src/Video/`: `AviFile` reads an in-memory AVI (video stream: MJPG, I420/NV12, H264 as Annex B frames, or another named codec flagged for the
  decoders; PCM audio; frames found by scanning `movi`, keyframes from `idx1`), `AviWriter` builds one, and `VideoPlayer` plays it:
  a decoder session from `SystemVideoCodecs`, the audio through an `audio::AudioPlayer` whose position is the clock (picture
  frames are skipped when decoding falls behind; without sound the system clock is used), play/pause/stop/seek (back to the
  nearest keyframe for inter-coded streams)/loop, a BGRA picture with a serial number. Streams that hold pictures back (H.264) are
  played in display order: packets are fed in decode order, and the player keeps a table from display position to sample, so
  seeking restarts at the sync sample before the target. `open_media()` also plays **MP4 / MOV** files: H.264 video (B frames,
  edit lists) with the AAC or MP3 soundtrack decoded up front; `OpenResult::NoDecoder` is what H.265 and the like get.
  `tests/VideoPlayerTest.cpp` covers it with a fake clock and audio device.
- `src/Media/`: picture, sound and video **file formats**, all in software, with hardware used first where there is any.
  `MediaCodecs` is the front door (`decode_image`/`encode_image`/`decode_audio`/`encode_audio`/`decode_video`): JPEG goes to a
  video codec device that really accelerates Motion JPEG (`info().accelerated`, found through `SystemVideoCodecs`; sessions are
  tried in device order), and when there is none - which is every machine today, since the NVIDIA/AMD/Intel engines are named
  but not driven - or the device refuses the data, the CPU codecs do it; each call reports which path ran (`CodecPath`). Every
  other format below has no hardware engine on current machines and is always CPU. Pictures decode to `Image` (frames of
  0xAARRGGBB with per-frame delays and a loop count), sounds to `AudioData` (16-bit mono/stereo, plays through
  `source()` into `AudioPlayer`).
  - Images: PNG and APNG (all colour types and depths, interlacing, tRNS; encoder with adaptive filters and LZ77 + Huffman
    deflate in `Zlib`), JPEG (baseline and progressive, bit-exact with libjpeg-turbo for the checked cases; baseline 4:2:0
    encoder), BMP (1-32 bit, RLE, bit fields), GIF (LZW, interlace, animation; encoder uses a fixed 216-colour palette with
    dithering), PNM/PAM, QOI, TGA, TIFF (strips and tiles, 1-16 bit grey/RGB/palette/alpha, planar, PackBits/LZW/Deflate and the
    predictor; Deflate RGB(A) encoder), WebP (lossless VP8L with all four transforms, colour cache and meta prefix codes, encoder with subtract-green and LZ77;
    lossy VP8 key frames with the ALPH alpha plane, decode only, bit-exact with libwebp's output; animated WebP is composed frame by frame as libwebp's animation decoder does).
  - Sound: WAV (PCM 8-32 bit, float, A-law, mu-law, IMA and Microsoft ADPCM, extensible), AIFF/AIFC, AU, FLAC (full decoder
    with CRC checks; fixed-predictor encoder), MPEG audio (MP3 = Layer III, MPEG-1/2/2.5, mono/stereo/joint/intensity stereo, ID3v2, gapless trimming from the LAME tag; also
    Layers I and II; decoder ported from the CC0 minimp3, within two 16-bit steps of ffmpeg's), Ogg Vorbis (decoder only: codebooks, floor 1, residue 0-2, channel coupling,
    FFT-based IMDCT; within one 16-bit step of ffmpeg's output; floor type 0 and chained streams are not handled), AAC-LC (ADTS `.aac`,
    and the audio of MP4 / M4A / MOV files through the `Mp4` container reader: all of ISO 14496-3's LC tools - long and short blocks,
    window shapes, M/S and intensity stereo, PNS, TNS, pulse data - decoded to the front channel pair, within two 16-bit steps of ffmpeg's output; Main, LTP, SBR/HE-AAC and
    fragmented MP4 are not handled; MP3 inside MP4 works too). Encoders write 16-bit PCM (WAV/AIFF/AU) and FLAC. **Not provided: Opus, HEVC/VP9/AV1 video.**
  - Video files: `VideoFiles` reads GIF, APNG, animated WebP and Y4M (4:2:0/4:2:2/4:4:4/mono) as frames with their own durations, which
    `VideoPlayer::open_media()` plays (AVI as before, now also with A-law, mu-law and ADPCM soundtracks and H.264 video; MP4 / MOV
    with H.264; a still picture opens as a one-frame video).
  `tests/MediaTest.cpp` checks every decoder against fixtures in `tests/data/` whose expected output comes from Pillow/ffmpeg
  (`tests/data/make_fixtures.py` regenerates them; `make_video_fixtures.py` the H.264 streams), plus encoder round trips, the accelerated-device/fallback logic with a
  fake engine, the player, and a fuzz run (truncation, bit flips, noise behind each signature) under ASAN/UBSAN. The Vorbis, WebP and TIFF decoders were
  also checked against libvorbis-, libwebp- and ImageMagick-encoded files with many settings.
- `src/FileSystem/`: filesystems. `IFileSystem` (path based: `open_file`, `open_directory`, `stat`, `create_directory`,
  `remove`, `rename`) hands out `IFile` (offset-based read/write/resize) and `IDirectory` (entry iteration) handles;
  every call returns a `Status` instead of throwing. `Path.hpp` holds the shared path rules. `Ram/` is a
  size-limitable in-memory filesystem, `Fat/` mounts FAT12/16/32 volumes (with long names) read-only from any
  `IStorageDevice` at a given first block. `src/FileSystemDemo` scans each device's partitions and mounts the FAT ones at
  boot (read only); `tests/FileSystemTest.cpp` covers both filesystems.
- `src/Async/`: async/await on C++20 coroutines, shaped like C#'s `Task`. `Task<T>` is lazy and move-only; an `Executor`
  runs tasks cooperatively on one thread (`spawn`, `block_on`, `run`), waiting tasks are polled each pass and the
  executor sleeps when nothing is ready. Awaitables: `delay`, `yield_now`, `wait_until(pred[, timeout])`,
  `current_executor`; plus `when_all`, `AsyncEvent`, `AsyncMutex` and `CancellationSource/Token`. No exceptions: a
  coroutine frame that cannot be allocated makes the task invalid (`FailedResult<T>`, out of memory for stream results;
  a `Task<void>` cannot report it, so check `valid()`). Frames come from `set_frame_allocator`. Avoid `co_await` inside
  a larger `&&`/`||` condition: GCC 16 mis-evaluates it.
- `src/Stream/`: `IStream`, modelled on C#'s `Stream` (`read`/`write`/`flush`/`close`, `can_seek` + `length`/`position`/`seek`/
  `set_length`, read/write timeouts), with `StreamStatus` results instead of exceptions. `Memory/MemoryStream` (growable,
  over a fixed buffer, or read-only over data), `File/FileStream` (over any `IFileSystem` file) and `Tcp/TcpStream`
  (over an `ITcpSocket`, blocking or async, with `TcpListener`; `connect`/`connect_async` by address or name). `StreamHelpers.hpp` has
  `read_exactly`, `read_byte`, `write_text`, `copy_to`; `IAsyncStream` adds `read_async`/`write_async`/`flush_async`
  (with cancellation) and `AsyncStreamHelpers.hpp` the awaitable versions. `src/AsyncDemo` self-checks it at boot. `tests/StreamTest.cpp` runs TcpStream against a real second stack.
- `src/Hardware/StorageDevice/Partition/`: `scan_partitions` reads GPT (header and array CRC-32, backup copy fallback,
  protective MBR) or MBR (primaries plus the extended/logical chain), and treats a bare filesystem as having no table;
  `PartitionStorageDevice` is a range-checked window onto part of a device, so a filesystem mounts a partition like a
  disk. `tests/PartitionTest.cpp` covers it.
- `src/Utility/` bounds-safe memory: `Slice` (a start index and a length, overflow-safe, `fits_in(size)`), `ArrayView<T>`
  (pointer + size: a non-owning, *checked* window - `operator[]`, `subview(Slice)`, `first/last/dropped`, `copy_from`,
  `cast<U>()`, and `try_*` forms that report failure instead), `ByteView`/`ConstByteView`, `ByteReader`/`ByteWriter`
  (bounds-checked, byte-order-aware parsing and building of packets and descriptors; the first failed access sets a
  sticky error) and `allocate_view`/`release_view`, and text: `StringView` (non-owning, checked; `substr`, `find`, `split_once`/`next_token`, `trimmed`, `starts_with`, case-insensitive compare, `parse_uint`, `copy_to`) and `String` (owned, allocator-backed, always NUL-terminated for `c_str()`, move-only; every operation that may need memory returns false on failure). The DNS/name APIs, `parse_ipv4` and `console::puts` take `StringView`. Anywhere code used to pass `T* data, uint32_t length` it passes one
  view: the network stack (every parser and builder), NIC/storage/USB-controller interfaces, descriptor and HID
  parsing, DMA rings and bounce buffers, NPU/ISP buffers, the framebuffer, ACPI/memory-map walking, and the containers'
  own storage (`List::operator[]` is now checked). A violation stops the OS with a message (`bounds_violation`);
  untrusted input goes through the non-aborting forms. `tests/run_host_tests.sh` builds the portable code for the host
  with ASAN/UBSAN and runs unit tests plus fuzzing (thousands of mutated/truncated frames through the network stack,
  random USB/HID/SCSI data through the parsers).
- `src/Kernel/`: the preemptive multithreading core. `Interrupts` builds a GDT/IDT (256 assembly stubs saving the full
  register and x87/SSE state as a `Frame`), `Apic` drives each core's local APIC (calibrated timer tick, wake-up IPIs),
  `ThreadScheduler` (`kernel::start()`) turns the boot context into the main thread (pinned to the boot core), parks
  the other cores in the scheduler (started through the firmware's MP services *before* the tables are replaced),
  and runs kernel threads from one run queue: round-robin with a time slice, per-thread core affinity and
  Performance/Efficiency preference, `sleep`, `join`, `detach`, idle cores woken by IPI. `Sync` has blocking
  `Mutex`, `RecursiveMutex`, `Semaphore`, `Event` (manual/auto reset), `ConditionVariable`, all with timeouts where
  they make sense; `RawSpinLock` is the dependency-free spinlock for the lowest layers. `Delay` gives every core
  TSC-based `delay_us()`/`uptime_us()` (long delays sleep instead of spinning), replacing the firmware's Stall().
  `Firmware` (`kernel::firmware([&]{...})`) funnels every post-start UEFI call through the boot core, one at a time.
  Boot services are *not* exited (so drivers can still be handed back to the firmware); the legacy PIC is masked while
  the kernel runs and the firmware's tables are restored by `kernel::stop()`.
- Paging and MMIO: `Kernel/Paging` (`PageTables`) edits the live page tables (4-level; the firmware's identity map stays): `map()` /
  `map_missing()` / `unmap()` make ranges mapped 1:1 with a memory type (write-back, write-through, uncached, write-combining via the PAT) and
  permissions (no-execute when the CPU has NX on), using the largest pages that fit and splitting the firmware's 1 GiB / 2 MiB pages where only part changes.
  `Hardware/Mmio.hpp` (`MmioRegion`) is a window onto device registers with exact-width, aligned, bounds-checked volatile accesses (`read8..64`,
  split 64-bit access, `update32` / `set_bits32` / `wait32`, `subregion`). `MmioMapper` maps device ranges (uncached, or write-combining for frame buffers)
  and refuses anything the firmware lists as RAM. `Hardware/Bus/Pci/PciBar` sizes BARs the way the PCI spec says (decoding off, ones written, mask read
  back, everything restored; 32- and 64-bit, I/O, bridges) and `pci_map_all_bars()` - called at boot before any driver - maps the BARs whose first page the
  firmware had not mapped, typically 64-bit ones above 4 GiB, leaving everything it did map alone. Limits: no 5-level paging; a changed mapping is only
  flushed on the core that made it (the boot core does all of it before the other cores start, and they load the same PAT); tables of replaced pages are not
  freed; `release()` keeps the page-table entries. The existing drivers still use their own `volatile` helpers on raw addresses and have not been
  converted to `MmioRegion`. `tests/PagingTest.cpp`, `tests/MmioTest.cpp`: against a pretend CPU and PCI function, no real hardware.
- Thread safety: `SpinLock` now masks interrupts while held; the allocator is used through one `ThreadSafeAllocator`
  everywhere; the console serializes output (`console::Guard` keeps several calls together); PCI config access, the
  i8042, every USB host controller, each storage device (plus shared controller/channel state), each NIC and the input
  devices have their own locks; each `NetworkStack` has a recursive lock taken by every public call of the stack, its
  sockets and its resolver, and `NetworkStackSet::start_service()` runs a thread that polls all stacks. `ICPUScheduler`
  tasks now run as kernel threads. Everything is still polled (no device interrupts); waits sleep instead of spinning.
  `src/KernelDemo` and `src/ThreadSafetyDemo` stress all of it from many threads (allocator, console, concurrent
  storage reads/writes across every controller type, UDP/TCP/DNS/ping through one stack).
- `src/Synchronization/`: `SpinLock`/`ScopedLock` (spin with the CPU interface's `pause()`), `Synchronized<T>`
  (any type behind a lock; `with()` for one locked operation, `access()` for a lock-holding handle that can
  be iterated) and `AtomicSharedPointer<T>`. `Allocators/ThreadSafeAllocator` serializes any allocator.
  `ObjectContainer/Concurrent/` has a thread-safe version of every container (`ConcurrentList`,
  `ConcurrentLinkedList`, `ConcurrentDictionary`, `ConcurrentBinaryTree`, `ConcurrentSortedDictionary`,
  `ConcurrentStack`, `ConcurrentQueue`, `ConcurrentPriorityQueue`): values are copied in and out, and nothing
  blocks. `src/ConcurrentDemo` fills them from tasks on every core at boot.
- `src/CPUScheduler/`: `ICPUScheduler` and the default `Scheduler`, a cooperative run-to-completion
  scheduler with high-performance and efficiency queues (growable `Queue`s on an `IAllocator`, guarded by a
  `SpinLock`) that spreads tasks over the cores the `ICentralProcessingUnit` reports (tasks on other cores
  must not call firmware). Give it a `ThreadSafeAllocator`.
- `src/Hardware/`: every class that touches hardware derives from `hardware::IHardware`
  (`name()`, `initialize()`, `shutdown()`): `IoPort`, `DebugConsole`, `TextConsole`, and the CPUs.
  - `Hardware/CentralProcessingUnit/`: `ICentralProcessingUnit` (instructions plus core count, core kinds,
    start-on-core) and its implementations. `X86_64Cpu` (shared base: CPUID, multi-core via UEFI MP
    services) and `SystemCpu` sit at the top. Each vendor has a subfolder holding its base class and one
    small class per microarchitecture: `Intel/` (Conroe ... Diamond Rapids, Atom lines), `AMD/` (K8, K10,
    Bulldozer, Piledriver, Zen ... Zen 5), `Hygon/`, `Zhaoxin/`, and `QEMU/` (the generic qemu64 model).
    The generation is chosen at boot from CPUID vendor and family/model/stepping, via the rule table in
    each vendor's `*Generations.cpp`; this covers every x86-64 model in `qemu-system-x86_64 -cpu help`.
  - `Hardware/GraphicsProcessingUnit/`: `IGraphicsProcessingUnit` (mode setting, 2D drawing, text rendering),
    `FramebufferGpu` (software drawing + 8x16 font on a linear framebuffer), `SystemGpu` (finds the PCI display
    device and picks a class). Vendor folders: `QEMU/` (std VGA with real mode setting through the Bochs VBE
    registers), `Intel/`, `AMD/`, `NVIDIA/` (identify the device but still use the firmware framebuffer until
    native drivers exist), and `UEFI/` (`UefiGopGpu`, the fallback for any other display). `src/GraphicsConsole`
    is a scrolling text console that only uses the GPU interface.
  - `Hardware/Bus/`: `IBus` and one subfolder per bus type, each with an interface and implementations.
    - `Pci/`: `IPciBus` (config access, BARs, capability list), `PciBusBase` (shared scanning), `LegacyPciBus`
      (I/O ports 0xCF8/0xCFC, 256-byte config space).
    - `Pcie/`: `IPcieBus` (adds the extended capability list) and `EcamPcieBus` (memory-mapped config space,
      located through the ACPI MCFG table).
    - `Usb/`: `IUsbBus` and `UsbBus` (finds host controllers on PCI), `IUsbHostController` (one controller:
      ports, enumerated device descriptors) and `UsbTypes.hpp`. `Usb/Xhci/` holds `IXhciController` and the
      polling `XhciController` driver (firmware hand-off, reset, command/event rings, port reset, slot
      enable, address device, control and interrupt-IN transfers, configuration) plus its ring and register helpers. At boot it enumerates
      every device on the root ports and reads its descriptors; HID class drivers (keyboard, mouse, tablet)
      build on it. `Bus/Usb/Ehci` is a USB 2.0 driver (async schedule for control/bulk, periodic for interrupt endpoints; high-speed devices on root ports only, others go to the companion controller); `Bus/Usb/Uhci` and `Bus/Usb/Ohci` drive the USB 1.1 controllers (low/full speed) on top of a shared `Bus/Usb/Legacy/LegacyHostController` (enumeration and control requests); on chips with an EHCI controller the companions start after it. Hubs and other device classes are not handled.
    - `SystemPciBus` picks PCIe/ECAM when firmware provides MCFG, else legacy PCI.
  - `Hardware/Keyboard/`, `Hardware/Mouse/`: `IKeyboard` (key press/release events as USB HID usages, polled)
    and `IMouse` (relative movement or absolute position, wheel, buttons), with shared queue bases and
    `Composite*` classes that merge several devices. One subfolder per transport QEMU offers:
    `Ps2/` (i8042 keyboard with set-1 decoding, mouse with IntelliMouse wheel), `Usb/` (HID keyboard in boot
    protocol; mouse and tablet through a HID report-descriptor parser) and `Virtio/` (virtio-input keyboard,
    mouse and tablet). `Hardware/SystemInput` finds all of them and exposes one keyboard and one mouse.
  - `Hardware/Guest/` - the rest of what QEMU offers a guest, brought up by `SystemGuestServices`: `VirtioRng` (virtio-rng, an `IEntropySource`
    that the SMB/SFTP/FTPS DRBG uses when the CPU has no RDSEED/RDRAND), `VirtioBalloon` (virtio-balloon: a service thread follows the host's
    `balloon` target, in 256 KiB chunks with a kept page at each end so allocator bookkeeping is never ballooned; capped at 1 GiB) and `PvPanic`
    (pvpanic on port 0x505: `kernel::panic` tells the host). The `run` targets add all three. Not covered: the QEMU guest agent (virtio-serial),
    virtio-vsock/fs/9p/console, fw_cfg, vmgenid. Checked by booting under QEMU (balloon inflate/deflate through the monitor); pvpanic's panic path is untested.
  - **VirtualBox:** `Hardware/VirtualBox/VmmDev` is the guest device (PCI 80EE:CAFE, requests through an I/O port; polled) that the Guest Additions use.
    It identifies the guest, takes absolute pointer positions (`Mouse/VirtualBox/VBoxMouse` merges them with the emulated PS/2 mouse's buttons; the
    desktop then needs no mouse capture), passes the host window's size to the display (`SystemGuestServices::set_display_request_handler`; the
    nearest mode the VGA offers that fits), follows the host's balloon (1 MiB chunks, capped at 1 GiB) and sends the heartbeat. The VBoxVGA adapter is the
    Bochs VBE interface (`QemuStdVgaGpu` takes 80EE:BEEF), and `NetworkInterface/AMD/PcnetNetworkInterface` drives the default PCnet adapter (tested
    on QEMU's `pcnet`: DHCP exchange both ways). VirtualBox's e1000, AHCI, NVMe, HD Audio, AC'97, OHCI/xHCI, virtio-* devices were already covered.
    **Not done:** HGCM (shared folders, shared clipboard, seamless windows, drag and drop), VBoxSVGA/VMSVGA, the LsiLogic/BusLogic SCSI adapters, the host
    time sync (the host clock is readable with `VmmDev::host_time_ms` but nothing sets the RTC: it is local time here and the zone is unknown).
    There is no VirtualBox in the test setup and QEMU has no VMMDev: `VmmDev` follows VMMDev.h from memory and has never run, so treat the request
    layouts (the heartbeat in particular) as unverified.
  - **Hyper-V:** `Hardware/HyperV/` - `HyperV` (CPUID detection, guest OS id, hypercall page, SynIC message/event pages; HvPostMessage / HvSignalEvent),
    `VmBus` (version negotiation, offers, GPADLs, channel open/close, ring buffers) and `HyperVIntegration` (heartbeat, shutdown/restart - the desktop
    shuts down and the machine powers off or cold-resets - and time sync, whose host time is kept but does not set the RTC), run by `SystemHyperV` on a
    thread pinned to the boot core. Checked under QEMU/KVM with `-cpu host,hv-vpindex,hv-synic,...`: the hypercall page and SynIC come up and hypercalls
    work; QEMU has no VMBus host, so the VMBus protocol, ring buffers and integration components are written from the protocol as documented and
    have never talked to a real Hyper-V. **Not done:** the synthetic network, SCSI, video, keyboard and mouse devices (netvsc/storvsc/hyperv-fb/hid; on a
    generation 2 VM the firmware's drivers still let the UEFI fallbacks reach the disk, network and screen), KVP, VSS, file copy, dynamic memory
    (balloon), the legacy DEC 21140 NIC of generation 1 VMs.
  - `Hardware/Bus/Ps2/` (`I8042Controller`) and `Hardware/Bus/Virtio/` (modern virtio-PCI transport,
    virtqueues, `VirtioInputDevice`); `Bus/Usb/Hid/` has the report-descriptor parser and interface finder.
  - `Hardware/ImageSignalProcessor/`: `IImageSignalProcessor` (raw Bayer frame -> RGB image, settings, frame
    statistics) with `ImageTypes.hpp`. `Software/` is the CPU pipeline (bilinear demosaic, black level,
    white-balance gains, gamma table); `Intel/` has IPU6/IPU7 classes that match the PCI device and still use
    that path until a native driver exists; `SystemImageSignalProcessor` picks one. QEMU has no ISP
    hardware, so only the software path runs there. `src/IspDemo` draws a synthetic frame through it.
  - `Hardware/NetworkInterface/`: `INetworkInterface` (MAC, link state, transmit/receive of raw Ethernet
    frames, statistics; polled) with native drivers - `Virtio/` (virtio-net, modern interface), `Intel/` (e1000
    family: 82540EM/82544GC/82545EM) and `Realtek/` (RTL8139) - and `UEFI/`, a fallback that borrows the
    firmware's driver (UEFI Simple Network Protocol) for any other NIC. `SystemNetworkInterfaces` finds every
    NIC and picks the driver. `src/NetworkDemo` sends ARP requests to QEMU's user-mode gateway at boot.
    Not covered natively: e1000e/igb, pcnet, ne2k, vmxnet3 (the firmware fallback handles those in QEMU).
  - `Hardware/Acpi/`: `AcpiTables` (RSDP -> XSDT/RSDT table lookup).
  - `Hardware/RandomAccessMemory/`: `IRandomAccessMemory` (implemented by `UefiRandomAccessMemory`).
  - `Hardware/NeuralProcessingUnit/`: `INeuralProcessingUnit` (int8 and float matrix products, activations,
    softmax). `Software/` splits products by rows over every core through the scheduler; `Intel/` (NPU
    "VPU": Meteor Lake to Panther Lake) and `AMD/` (XDNA) classes match the PCI device and still use that
    path until a native driver exists; `SystemNeuralProcessingUnit` picks one. QEMU has no NPU hardware, so
    only the software path runs there. `src/NpuDemo` checks it at boot. `src/Math/FloatMath` supplies exp/ln.
  - `Hardware/SystemHardware.hpp`: aliases `SystemCpu` / `SystemRam` to the classes for this architecture.
  - `Hardware/x86_64/port.S`: port I/O assembly for `IoPort`; `asm.hpp` is private to `Hardware/`.

- `src/Graphics/`: 2D and 3D rendering above the GPU layer, loosely modelled on Vulkan. `GraphicsTypes.hpp` has the
  shared pieces (`Vec`/`Mat4` with Vulkan conventions - +Y down, depth 0..1 - formats, `Surface`).
  - `Graphics2D/`: `IRenderer2D` (clip, blend, pixels, lines, rectangles, circles, triangles, images, 8x16 text, `present`).
    `Software/SoftwareRenderer2D` rasterizes on the CPU into a `Surface` and copies it to the display GPU; `Device/DeviceRenderer2D`
    turns every shape into colored triangles and draws them in batches through any `IGraphicsDevice`, so on the virgl device
    it is GPU-accelerated 2D (there are no textures yet: images cost one rectangle per run of equal pixels).
  - `Graphics3D/`: `IGraphicsDevice` creates buffers, images, shader modules and graphics pipelines; `ICommandBuffer` records
    render passes, pipeline/vertex/index binds, push constants, viewport/scissor and (indexed) draws; `submit()` runs them
    and `present()` shows an image. Not SPIR-V (no compiler in the OS): a `ShaderModuleDesc` holds a native C++ function for
    the software device and/or TGSI text for the GPU device; `BuiltinShaders` ships a colored-vertex program with both.
    `Software/SoftwareDevice` is a scanline rasterizer (near-plane clipping, perspective-correct varyings, depth buffer,
    culling, scissor, alpha blending). `Virgl/VirglDevice` drives the host's OpenGL through virtio-gpu's virgl protocol
    (Gallium-style state objects, TGSI shaders, buffers pushed on `unmap()`, images read back on request).
  - `SystemGraphics` picks the back ends: the virgl device if the display GPU is a virtio-gpu with a 3D engine and the
    device passes a self-test, else the software device (`renderer_2d()` likewise). `src/GraphicsDemo` checks it at boot: 2D shapes,
    a conventions check (orientation, culling, depth - identical on both devices) and a spinning depth-tested cube with
    a readback check. `tests/GraphicsTest.cpp` covers the math, both 2D renderers and the software device on the host.
  - `Hardware/GraphicsProcessingUnit/Virtio/VirtioGpu` is the display driver for QEMU's virtio-gpu (`-device virtio-gpu-pci`):
    the screen is a host resource backed by guest memory, `IGraphicsProcessingUnit::present()` copies the dirty rectangle over
    (the console calls it after every output call), and with a `-gl` device (virglrenderer) it negotiates virgl, creates a 3D
    context and exposes its command channel to `VirglDevice`. `FramebufferGpu` gained dirty-rectangle tracking and `blit()`.
    Not covered: textures/samplers, SPIR-V, compute, real GPUs (Intel/AMD/NVIDIA classes still use the firmware framebuffer),
    and virtio-gpu cursor/EDID/resize events.

- `src/Gui/`: a small GUI toolkit that draws through `IRenderer2D`, so it is GPU-drawn wherever 2D is. `Control` is the base
  of everything and any control can hold children. Layout is by dock style (`Control::layout()`): children are placed in the
  order they were added, `Top`/`Bottom` take a strip of the area that is left (their own height, full width), `Left`/`Right` the
  same sideways, `Fill` takes the rest (add it last) and `None` keeps its own bounds; padding shrinks the client area.
  `Controls/Panel.hpp` has `Panel` (flat/raised/sunken container) and `UniformPanel`, which splits its area evenly between its
  visible children, horizontally or vertically, with spacing (the leftover pixels go one each to the first children).
  Controls: `Label`, `Button`, `TextBox` (single-line editing, caret, scrolling, max length, read-only), `DropDown` (list popup;
  mouse and keyboard) and `MenuBar`/`Menu`/`MenuItem` (classic menus: shortcut text, check marks, separators, disabled items,
  sliding between menus, keyboard navigation). Input and display controls: `Slider` (horizontal/vertical, step/page keys, wheel,
  drag), `LevelMeter` (segmented green/yellow/red bar with peak hold), `CheckBox`, `RadioButton` + `RadioGroup` (exclusive, arrow keys
  move check and focus), `Toggle` (on/off switch), `ColorPicker` (swatch + popup with a saturation/brightness square and hue
  strip; `Gui/Color.hpp` has the integer HSV conversions) `ScrollBar`, `DataGrid` (a table over an `IGridModel` - or its own `TextGridModel` via `add_column`/`add_row` - with sortable,
  resizable columns, row selection, vertical/horizontal scrolling; numbers sort as numbers, the sort is stable), `ImageView` (a `Surface`
  at 1-3200% zoom: pixels repeat zooming in and average zooming out, drag to pan, wheel/keys to zoom, fit mode, chequerboard
  under transparency, drawn through a viewport-sized cache), `AudioPlayerControl` (play/pause, stop, seek and volume sliders, time,
  level meter, over an `audio::AudioPlayer`), `VideoPlayerControl` (the picture in an `ImageView` that enlarges to fill, with
  play/pause, stop, seek, loop, volume and time, over a `video::VideoPlayer`) and `DatePicker` (YYYY-MM-DD field + month calendar popup;
  `Gui/Date.hpp` has the Gregorian date maths, years 1-9999). Callbacks are `Handler`s (function + context pointer). `Window` is the root: title bar,
  focus (Tab/Shift+Tab), mouse capture, hover, and one popup layer drawn above the tree (drop-down lists and menus use it);
  `render()` lays out, paints and presents when `dirty()`. `InputPump` feeds a window from `IKeyboard`/`IMouse` (relative or
  absolute). Controls do not own their children: whoever creates them keeps them alive while they are in a tree. Not yet:
  submenus, text selection/clipboard, scrolling lists, a mouse cursor sprite beyond the simple arrow, several windows.
  `src/GuiDemo` builds a window with all of it at boot and drives it with synthetic input (typing, choosing, menu items)
  while checking the results; `tests/GuiTest.cpp` covers layout, the basic controls and the input pump on the host; `tests/GuiViewsTest.cpp` covers the scroll bar, data grid, image view and player control; `tests/GuiControlsTest.cpp` covers the slider, level meter, check box, radio button, toggle, colour and date pickers and their helpers.

- `src/BootLog.*` is the boot log (`bootlog::Entry(Level, "Subsystem").text(...).dec(...)`; counts warnings and failures; `replay()` redraws
  the lines after a display mode change cleared the screen). `src/Desktop/` is the desktop shell built on the GUI toolkit: `Desktop`
  (full-screen `Window`, `DesktopSurface`, a taskbar with a `MenuButton` Start button whose menu opens upwards, and an uptime clock);
  `step()` delivers input and redraws when dirty, `shutdown_requested()` ends the loop in `main.cpp`, which then does the orderly
  power-off. The boot-time self-checks and demos of every subsystem (containers, kernel, scheduler, NPU, network stack, storage,
  filesystems, thread safety, ISP, graphics, GUI) are still there: configure with `-DTCOS_SELFTESTS=ON` to run them before the desktop.

- `src/Gui/Windows/`: a stacking window manager. `Frame` is a top-level window (bevelled border, title bar, minimize / maximize /
  close buttons at the top right, a client area for ordinary controls; each button can be disabled and is then drawn greyed and
  ignores clicks; the title bar drags, the borders and corners resize, Escape can close). `WindowManager` holds the frames in
  stacking order (last = top): pressing the mouse anywhere on a window raises and activates it (through
  `Control::on_descendant_pressed`), active windows have a lit title bar, minimize hides, maximize fills the area above the
  taskbar and toggles back, close removes the window and runs its `on_closed` handler. Modal windows disable every other window
  (disabled windows ignore input, look inactive and are skipped by Tab: `Control::effectively_enabled`) and stay on top; the active
  window is the Tab scope (`Window::set_focus_scope`). `WindowSwitcher` is the taskbar's window list (dialogs are left out; the
  active window's button is pressed in; clicking it minimizes, clicking another restores/raises that window). `Gui/MessageDialog` is a
  modal two-button question (Enter = default button, Escape/close = no). The desktop's Start menu has *About* (an ordinary window) and
  *Shut Down...*, which asks for confirmation in such a dialog.

- GUI threading (`src/Gui/Window.*`, `RenderLoop.*`, `Control.hpp`): the desktop runs on three kinds of thread. The *input thread*
  (the boot loop: `Desktop::pump()`) reads the keyboard, mouse and clock; a *render thread* (`RenderLoop`) sleeps until something marks
  the window dirty and then draws it; a *handler thread* (`Window::start_handler_thread()`) runs the callbacks controls fire
  (button clicks, menu items, changed text, closed windows, dialog answers - in order, one at a time, never under the UI
  lock). A handler that sleeps for seconds therefore freezes neither painting nor the pointer. All shared state is one recursive
  lock per window (`Window::ui_mutex()`, taken through `UiGuard`): painting, input delivery and **every public mutator** of a control
  (`set_text`, `add`/`remove`, `set_bounds`, menu item flags, window-manager operations, `MessageDialog::show`, ...) take it, so any
  thread may change the UI at any time - from a handler, from `Window::run_async()` work (a background kernel thread whose completion
  handler is fired when it returns), or from your own thread - without corrupting what is being drawn. Plain getters do not lock:
  read them through `Window::locked([&]{ ... })` from another thread. Rules: a `Window` must outlive its controls; destroying a
  control drops its queued handlers and waits for one that is running (not from inside the UI lock);
  `Handler::immediate()` marks toolkit-internal callbacks that must run in the firing thread; before `kernel::start()` (or with no handler
  thread) handlers simply run inline. `tests/GuiThreadTest.cpp` runs the real thing with pthreads (`tests/HostThreads.cpp`) under
  ThreadSanitizer: a 400 ms handler, handler ordering, a four-thread stress test (text, drop-down items, controls and windows
  coming and going, menu flags, titles) while the window is drawn and clicked, `run_async`, and destroying controls with handlers
  queued or running. (`Kernel/Sync.hpp`'s `RecursiveMutex` now reads its owner atomically: a benign race ThreadSanitizer reported.)

## Requirements

`cmake`, `ninja` (or make), `x86_64-w64-mingw32-gcc/g++` (mingw-w64),
`qemu-system-x86_64`, OVMF/edk2 firmware, `gdb`.

mingw-w64 is used because it emits PE32+ directly with the UEFI-compatible
calling convention - no gnu-efi or lld needed.

## Build & run

```sh
cmake -S . -B build -G Ninja
cmake --build build              # produces build/esp/EFI/BOOT/BOOTX64.EFI
cmake --build build --target run # boot in QEMU (OS output on your terminal)
cmake --build build --target run-gl # same with a virtio-gpu 3D device: QEMU renders through the host GPU (virgl)
```

`run-gl` needs QEMU built with virglrenderer and a host OpenGL stack; the OS then reports `Graphics: virgl (virtio-gpu),
accelerated`. `scripts/qemu-debug.sh` takes `QEMU_GPU=gl` (virgl), `virtio` (virtio-gpu, software rendering) or the default
`std` (Bochs VGA, software rendering). Without a 3D engine everything falls back to the software renderers.

### Debugging in QEMU

`scripts/gdb-proxy.py` first rebuilds `BOOTX64` (output goes to the debugger console),
then starts QEMU (frozen at reset, gdbstub on :1234) and
bridges gdb to it: `target remote | scripts/gdb-proxy.py`. It rewrites
software breakpoints to hardware ones, so breakpoints set before OVMF has
loaded the image still work. OS output appears on gdb's stderr and in
`<build>/debugcon.log`.

```sh
gdb -ex 'symbol-file build/BOOTX64.efi' -ex 'target remote | scripts/gdb-proxy.py' \
    -ex 'break kernel_main' -ex continue
```

### CLion

`.run/Debug TheoryCraftOS.run.xml` is a *GDB Remote Debug* configuration using
the command above. Select **Debug TheoryCraftOS** in the run dropdown (not the
auto-generated `BOOTX64` entry, which tries to run the `.efi` natively) and
press Debug. QEMU opens, the debugger is paused at reset; press Resume.
Use the **QEMU** config to just run the OS. Set the debugger to GDB (the
bundled one works). If the symbol path is wrong for your build directory,
edit it in the configuration.

## License

GNU Affero General Public License v3.0 or later - see [LICENSE](LICENSE). Every source file carries the standard notice.
