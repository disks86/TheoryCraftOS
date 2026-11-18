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
  Enable over it.

## Tests

- `tests/run_host_tests.sh GameControllerTest` (ASan/UBSan) covers every protocol's reports and messages, with truncation and bit-flip
  fuzzing of each decoder. It also covers two HID descriptors (a DirectInput pad with a report id and hat; a 16-bit pad without an id), the
  catalogue, the USB driver against a fake host (start-up messages, GIP acknowledgement, Switch handshake, SET_REPORT fallback, no interrupt
  OUT), the service's slots and hot-plug, and NtTcosGamepad's words.
- `tests/run_host_tests.sh GameControllerThreadTest` (ThreadSanitizer) runs the service thread while one thread plugs pads in and out of four
  ports and two threads read and rumble them through the service and the system call's words.

## Not done / not validated

- Nothing has been tried with a real pad or on real xHCI hardware. Byte layouts follow the public protocol descriptions (Linux `xpad`,
  `hid-playstation`, `hid-nintendo`, SDL). The xHCI interrupt OUT path is compiled but not run in QEMU (QEMU has no USB gamepad device).
- Bluetooth pads (DualShock 4/DualSense Bluetooth reports with CRC, Xbox Bluetooth LE, Switch Pro over Bluetooth); the Xbox 360 Wireless
  Receiver (045E:0719, its own report framing); Xbox Wireless Adapter.
- Switch Pro: no rumble (HD Rumble encoding), sticks use a nominal calibration instead of the one in SPI flash. DualSense: no adaptive
  triggers, touchpad positions or motion sensors. DualShock 4: no touchpad or motion.
- Generic HID pads use a fixed button convention. A mapping database (like SDL's) would place oddly wired pads correctly.
- No force feedback for HID joysticks (PID page), no DirectInput devices (`dinput8` still lists no joysticks), no Linux `/dev/input/js*` or
  evdev, no Windows.Gaming.Input.
