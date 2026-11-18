# Game controllers

Gamepads and joysticks on USB, in player slots, readable by programs. Code: `src/Hardware/GameController`.

## What is supported

| Protocol | How it is found | Input | Output |
|---|---|---|---|
| Xbox 360 / XInput (`XboxProtocol`) | interface class 0xFF, subclass 0x5D, protocol 0x01 | 20-byte report type 0x00 | rumble, ring LEDs (player 1-4) |
| Xbox One / Series, GIP (`XboxProtocol`) | interface class 0xFF, subclass 0x47, protocol 0xD0 | command 0x20, guide 0x07 (acknowledged) | power-on, guide light, rumble |
| DualShock 4 (`SonyProtocol`) | 054C:05C4, 09CC, 0BA0 | USB report 0x01 | report 0x05: rumble, light bar |
| DualSense, DualSense Edge (`SonyProtocol`) | 054C:0CE6, 0DF2 | USB report 0x01 (mute, Edge paddles, battery) | report 0x02: rumble, light bar, player LEDs |
| Switch Pro (`SwitchProtocol`) | 057E:2009 | handshake (0x80 02/03/02/04), mode 0x30, full report 0x30 | player LEDs (subcommand 0x30) |
| Xbox 360 Wireless Receiver (`XboxProtocol`, `Usb/UsbXbox360Wireless`) | interfaces 0xFF/0x5D/0x81, one per pad (four) | pairing messages (byte 0 bit 3), input at byte 4 | presence inquiry, ring LEDs, rumble |
| Any HID joystick/gamepad (`HidGamepad`) | report descriptor with usage 1:4 or 1:5 | decoded from the descriptor | none |

Most PC pads (Logitech F310/F710, PowerA, PDP, Hori, Razer, Turtle Beach, 8BitDo in X mode) speak one of the Xbox protocols and are found
by their interface, whether or not they are in the catalogue. `GameControllerCatalog.cpp` only adds names, and the protocol for the HID pads
that need their own (Sony, Nintendo).

Everything lands in one model (`GamepadTypes.hpp`): the W3C "standard gamepad" / XInput layout, face buttons named by position (south, east,
west, north), sticks signed 16-bit with up positive, triggers 0..255, and extra bits for touchpad, capture/mute and paddles. The first
sixteen button bits are XInput's own.

## Layers

- `IGameController`: one pad. `poll()` reads what arrived, `state()` is the last state, `set_output()` drives motors and lights.
- `Usb/UsbGameController`: `probe_usb_game_controller()` picks the interface (an Xbox one first, else HID) and its interrupt endpoints.
  Output goes to the interrupt OUT endpoint, or for a HID pad without one as SET_REPORT on endpoint 0.
- `IUsbHostController::start_interrupt_out` / `interrupt_out` are new. xHCI implements them (an interrupt OUT endpoint context, sharing the
  bulk endpoints' slots and synchronous transfer, with its own DMA staging buffer). Other controllers refuse. Then a 360 pad still reports
  but cannot rumble, and Xbox One and Switch Pro pads are not taken, because they say nothing until something is sent to them.
- `GameControllerService`: player slots (8; XInput uses 0..3). A pad takes the lowest free slot and is shown its number (ring quadrant,
  player LEDs, PlayStation light bar colour). A thread polls every 4 ms and scans for hot-plug every 256 ms (`UsbPortWatcher`). One
  `kernel::Mutex` covers slots and drivers, so a program's call never meets a pad that is being polled or dropped.
- `SystemInput::set_usb_filter`: `main.cpp` passes `GameControllerService::is_game_controller`, so the HID mouse driver does not take a
  joystick's X/Y axes for a pointer.
- `NtTcosGamepad` (Windows service 512): `src/Process/WindowsGamepad.hpp` gives the argument layout. `WindowsGamepadService.cpp` is the
  system call, and `GamepadServiceCall.cpp` holds the work on the eight words, tested without a process. `xinput1_4.dll` implements
  XInputGetState(Ex), SetState, GetCapabilities(Ex), GetBatteryInformation, GetKeystroke (button and trigger changes as `VK_PAD_*`) and
  Enable over it. GET_NAME (5) hands out the pad's name.
- Receiver pads: each channel of an Xbox 360 Wireless Receiver is its own `IGameController`, whose `connected()` follows the pairing messages.
  The service keeps such pads among its idle pads (polled, holding no slot) and gives them the lowest free slot when they pair. A pad that is
  switched off goes back to the idle pads; pulling out the receiver drops them all.
- DirectInput (`src/Windows/dinput8`): `EnumDevices` lists one gamepad per occupied slot (instance GUID per slot; product GUID in the
  "PIDVID" form carrying the USB ids). `CreateDevice` makes an `IDirectInputDevice8A/W` (`joystick.inc`) whose objects are X/Y (left stick),
  Rx/Ry (right stick), Z/Rz (left/right trigger), one POV hat and eleven buttons. `joystick_core.h` holds the plain arithmetic, tested on the
  host:
  - matching any `DIDATAFORMAT` (c_dfDIJoystick, c_dfDIJoystick2, hand-made ones) against the bounds, alignment and overlap rules;
  - DIPROP_RANGE, DEADZONE and SATURATION;
  - polled and buffered data (GetDeviceData with peek and overflow).
  
  DIPROP_GUIDANDPATH has `&ig_00` for pads XInput also serves, so programs that use both (SDL) count them once. Also answered: VIDPID,
  JOYSTICKID, the product, instance and type names, and BUFFERSIZE. No force feedback (rumble goes through XInput), no action maps.

- Bluetooth pads (`Bluetooth/`): a transport and a pad meet at a `HidPadChannel`, shared by both. The transport attaches its
  `IHidReportLink`, delivers input reports and detaches before its link goes. The pad reads the reports when the service polls it and is
  present while the channel is attached. `is_bluetooth_gamepad()` routes a HID device to the pads (by ids, else by a joystick or gamepad
  collection); `make_bluetooth_gamepad()` makes the pad. Bluetooth forms:
  - Xbox One S, Series and Elite 2: Microsoft's button order; rumble is output report 3.
  - DualShock 4: the reduced report 0x01 until feature 0x02 is read, then 0x11. Output 0x11 with CRC-32.
  - DualSense: reduced 0x01 until feature 0x05, then 0x31. Output 0x31 with a sequence tag and CRC.
  - Switch Pro: simple 0x3F until subcommand 0x03, then 0x30.
  - Generic HID pads.

  A pad's start-up (a feature read or subcommand) runs on its first poll on the service's thread, never on the transport's.
- LE pads: `HidOverGatt` reads the Device Information service's PnP ID and the report map, and sends pads to the service
  (`set_game_controllers`) instead of making them keyboards. Notifications get their report id put back in front. Output reports go to a
  `GattPadLink` that queues them (latest per id); they are written from the adapter's thread (`HidOverGatt::service`, the profile hook),
  since a remote is only used from there.
- Classic pads (DualShock 4, DualSense, Switch Pro, classic HID pads): `Hardware/Bluetooth/HidpHost` (HIDP over L2CAP) reads the report
  map and the PnP ids over SDP, and sends pads to the service (`set_game_controllers`). Output goes as DATA|Output on the interrupt channel,
  feature reads as GET_REPORT on the control channel; both are queued and sent from the adapter's thread, and `get_feature()` waits up to
  1 s for the answer. A paired pad that reconnects by itself (PS or Home button) is accepted with a role switch. See the classic HID
  paragraph of `docs/DellXps9575.md` and `ClassicHidTest`.

## Tests

- `tests/run_host_tests.sh GameControllerTest` (ASan/UBSan) covers every protocol's reports and messages, with truncation and bit-flip
  fuzzing of each decoder. It also covers two HID descriptors (a DirectInput pad with a report id and hat; a 16-bit pad without an id), the
  catalogue, the USB driver against a fake host (start-up messages, GIP acknowledgement, Switch handshake, SET_REPORT fallback, no interrupt
  OUT), the service's slots and hot-plug, NtTcosGamepad's words and names, and the wireless receiver (protocol, channel discovery from cut-short
  descriptors, pads pairing, leaving and pairing again, per-channel LEDs and rumble, the receiver unplugged).
- GameControllerTest also covers the Bluetooth forms:
  - the shifted Sony reports, the CRC (a flipped bit fails it), the Switch simple report, all fuzzed;
  - routing and cut-short report maps;
  - pads behind a fake link: feature read once on first poll, ring overflow, detach;
  - an Xbox pad over HID over GATT against a fake GATT remote: PnP ID, id put back, rumble queued then written by service(), no
    keyboard made, cut-short and bit-flipped maps.
- `tests/run_host_tests.sh DirectInputJoystickTest` (ASan/UBSan) covers dinput8's data-format matching, axis scaling, POV, state filling,
  the change buffer and the object lookups of properties.
- `tests/run_host_tests.sh GameControllerThreadTest` (ThreadSanitizer) runs the service thread while one thread plugs pads in and out of four
  ports and two threads read and rumble them through the service and the system call's words.

## Not done / not validated

- Nothing has been tried with a real pad or on real xHCI hardware. Byte layouts follow the public protocol descriptions (Linux `xpad`,
  `hid-playstation`, `hid-nintendo`, SDL). The xHCI interrupt OUT path is compiled but not run in QEMU (QEMU has no USB gamepad device).
- Bluetooth pads (DualShock 4/DualSense Bluetooth reports with CRC, Xbox Bluetooth LE, Switch Pro over Bluetooth); the Xbox Wireless
  Adapter (Xbox One/Series pads over its own radio). The 360 receiver's battery level and headsets are not read.
- dinput8's COM layer (`joystick.inc`) is compiled but has only been run through its arithmetic on the host; no program has used it yet.
- Switch Pro: no rumble (HD Rumble encoding), sticks use a nominal calibration instead of the one in SPI flash. DualSense: no adaptive
  triggers, touchpad positions or motion sensors. DualShock 4: no touchpad or motion.
- Generic HID pads use a fixed button convention. A mapping database (like SDL's) would place oddly wired pads correctly.
- No force feedback for HID joysticks (PID page) or through DirectInput, no DirectInput system keyboard and mouse, no Windows.Gaming.Input.

## Linux programs: /dev/input

`src/Process/LinuxInput.cpp` gives Linux programs (SDL2/SDL3 without udev, Wine/Proton's evdev backend, anything using libevdev or the
joystick API) the standard nodes. `hardware::install_linux_gamepads` (`src/Hardware/LinuxGamepads.cpp`, called from `main.cpp`) hands it an
`InputPadProvider` over the service, so the process layer does not depend on the hardware code.

- Numbering: player slot N is `/dev/input/eventN` (character device 13:64+N) and `/dev/input/jsN` (13:N). Only occupied slots exist; numbers
  from 8 up are left for other input devices. `/dev/input` lists them (`DT_CHR`), and its modification time moves when the set of pads changes
  (SDL's scan without udev looks for that).
- evdev: `struct input_event` (24 bytes), `EVIOCGVERSION`, `EVIOCGID` (`BUS_USB`, vendor, product, version 0x0100), `EVIOCGNAME`, `EVIOCGPHYS`,
  `EVIOCGUNIQ` (`ENOENT`, as Linux for a pad without a serial), `EVIOCGPROP`, `EVIOCGBIT` (`EV_SYN`, `EV_KEY`, `EV_ABS`, `EV_FF`), `EVIOCGKEY`
  (drops queued key events, as Linux does), `EVIOCGABS`, `EVIOCGRAB`, `EVIOCREVOKE`, `EVIOCSCLOCKID`, `EVIOCSFF`/`EVIOCRMFF`/`EVIOCGEFFECTS`
  (16 `FF_RUMBLE` effects, `FF_GAIN`), and `EV_FF` writes to play and stop. Effects add up and are scaled by the gain (ff-memless), strong to
  the low-frequency motor; closing the descriptor stops them.
- Layout as the Linux drivers have it: xpad for Xbox and generic HID pads (A/B/X/Y = 0x130/0x131/0x133/0x134 by label), hid-playstation and
  hid-nintendo for Sony and Nintendo pads (by position, plus `BTN_TL2`/`BTN_TR2`; Nintendo capture is `BTN_Z`, no analog triggers). Sticks
  -32768..32767 with Y down positive (fuzz 16, flat 128), `ABS_Z`/`ABS_RZ` 0..255, d-pad `ABS_HAT0X`/`HAT0Y`.
- Events are the difference between what the client last reported and the slot's state, ending with `SYN_REPORT`; a new one is made only once
  the queue is read empty, so a slow reader skips states but never sees them out of order. `O_NONBLOCK` (open, `fcntl`, `FIONBIO`) gives
  `EAGAIN`; otherwise a read looks again every 4 ms. poll/epoll/select report readability the same way. An emptied slot, or another pad in it,
  gives `ENODEV` (and `POLLHUP | POLLERR`) on the old descriptors.
- Joystick API: `struct js_event`, `JSIOCGVERSION`, `JSIOCGAXES` (8), `JSIOCGBUTTONS` (11 on an Xbox pad), `JSIOCGNAME`, `JSIOCGAXMAP`,
  `JSIOCGBTNMAP`, the `JS_EVENT_INIT` burst, values through joydev's default correction (-32767..32767).
- Tests: `tests/run_host_tests.sh LinuxInputTest` opens the nodes through the real system calls of a Linux process over the service and a fake
  USB host: stat and the listing, every ioctl with bad sizes and pointers, both event streams, blocking reads, poll and epoll, rumble, grabs,
  unplug and replug, and five threads at once (also clean under ThreadSanitizer when built by hand).
- Not done: `/sys/class/input` and udev (`/run/udev`), so SDL's udev backend and libudev users do not see the pads; `EVIOCSABS`,
  `EVIOCSKEYCODE`, `EVIOCGMASK`/`EVIOCSMASK`, `JSIOCSCORR`/`JSIOCGCORR`, the joystick API's old 12-byte read; extra buttons (touchpad click,
  paddles, Xbox share) are not reported; inotify on `/dev/input` (SDL falls back to its timed scan); rumble from two descriptors of one pad
  is not combined (the last one written wins); a timed effect ends when the program next touches the descriptor, not on the dot; the
  service keeps no per-slot connection number, so a pad unplugged and replaced by one of the same model between two looks is taken for the
  same one.

## Xbox Wireless Adapter for Windows

`Usb/XboxWirelessAdapter` drives Microsoft's USB adapter for Xbox One and Series controllers: 045E:02E6 (first generation), 045E:02FE (the
slim second generation) and 045E:02F9 / 045E:091E (built into some laptops and headsets). The adapter is a MediaTek MT76x2U-style radio
(its Windows driver is `mt7612us.sys`): the host loads firmware into it, turns the radio on, picks the quietest of a list of channels and sends
a beacon; controllers associate, and each then talks GIP (the same messages as a wired pad, `XboxProtocol`) in 802.11 QoS data frames.
`open_xbox_wireless_adapter()` returns eight pads, one per client slot, sharing one `XboxWirelessAdapter` (a `SharedPointer`, every member
behind one `kernel::Mutex`). A slot's pad is `connected()` from the controller's GIP announcement until it disassociates, is lost by the
radio, or says it powers off, so it holds a player slot only then (the idle pool, as with the Xbox 360 receiver). The adapter's button opens
pairing (the beacon says so and its light blinks) until one controller pairs; `XboxWirelessAdapter::set_pairing()` does the same from the host.
Pure encoders and decoders are in `Usb/XboxWirelessFrames`.

**Firmware.** The firmware is Microsoft's and is not in this repository. The driver reads it with the Wi-Fi drivers' `FileSystemFirmwareSource`,
so it must be at:

| Adapter | File |
|---|---|
| 045E:02E6 | `/firmware/xow_dongle.bin` |
| 045E:02FE | `/firmware/xow_dongle_045e_02fe.bin` |
| 045E:02F9, 045E:091E | `/firmware/xow_dongle_045e_02f9.bin` or `/firmware/xow_dongle_045e_091e.bin`, else `/firmware/xow_dongle.bin` |

These are the names the Linux drivers xow and xone use under `/lib/firmware`, so a file prepared for them can be copied over as it is. To get
it legally, take it from Microsoft's own driver for the adapter, which Windows installs from Windows Update when the adapter is plugged in
(it is also listed in the Microsoft Update Catalog as the "Xbox Wireless Adapter for Windows" driver): the driver package (a `.cab`, or the
installed copy under `C:\Windows\System32\DriverStore\FileRepository\` on a Windows machine that has used the adapter) contains the firmware
image, `FW_ACC_00U.bin` for the first adapter (xone's `xone-get-firmware.sh` takes the slim adapter's from a second package). Extract it with
`expand` on Windows or `cabextract` elsewhere, rename it as above and copy it to `/firmware`. The file is Microsoft's copyrighted work, licensed
with its driver for use with the adapter: use your own copy with your own adapter and do not pass it on (which is why it is not shipped here). Without the file the adapter is refused and
the log says which file is missing and where it is looked for.

The kernel hands the driver its firmware source and log once the file system with `/firmware` is mounted (`set_xbox_wireless_environment`).
An adapter taken up before that gets its pads at once and starts on their next poll after it; if the file is missing then, it is refused,
logged and its pads leave.

- Tests: `tests/run_host_tests.sh XboxWirelessAdapterTest` (ASan/UBSan, then ThreadSanitizer) runs the driver against a model of the adapter
  on a fake USB host: the frame layouts with truncated and bit-flipped packets, refusal without firmware (and the log line), the download
  sequence (FCE address and length before every chunk, ILM and DLM where they belong, the IVB load last), damaged firmware files,
  association, power-on, input with and without the radio's padding, the guide acknowledgement, battery, rumble, pairing, departures, nine
  controllers for eight slots, garbage on the endpoints, the adapter unplugged mid-stream, the service's idle pool, and nine threads at once.
- Not checked against a real adapter. The chip's registers and vendor requests are mt76's (`mt76x02_regs.h`, `mt76x2/usb_mcu.c`); the parts
  specific to the Xbox protocol (the MCU event numbers, the beacon's vendor element and its pairing flag, the pairing frames, the association
  response body, the DLM load address, the LED modes, the endpoint roles, the channel list) are remembered from xow and are collected in the
  `mt` and `xbox_table` namespaces of `XboxWirelessFrames.hpp`, marked UNCERTAIN, to be corrected from a USB capture.
- Not done: the MCU's command answers are not waited for; no ROM patch is loaded (xow loads none); the built-in adapters' firmware names are
  a guess; controller firmware updates, audio (headsets), the chatpad and the controllers' own pairing memory across adapters are not handled;
  pairing does not time out by itself; an adapter refused for a missing file is tried again only when it is plugged in again.
