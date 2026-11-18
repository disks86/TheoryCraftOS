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

## Tests

- `tests/run_host_tests.sh GameControllerTest` (ASan/UBSan) covers every protocol's reports and messages, with truncation and bit-flip
  fuzzing of each decoder. It also covers two HID descriptors (a DirectInput pad with a report id and hat; a 16-bit pad without an id), the
  catalogue, the USB driver against a fake host (start-up messages, GIP acknowledgement, Switch handshake, SET_REPORT fallback, no interrupt
  OUT), the service's slots and hot-plug, NtTcosGamepad's words and names, and the wireless receiver (protocol, channel discovery from cut-short
  descriptors, pads pairing, leaving and pairing again, per-channel LEDs and rumble, the receiver unplugged).
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
