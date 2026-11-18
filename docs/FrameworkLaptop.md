# Framework laptops

Framework Laptop 13 (Intel 11th-13th gen, Core Ultra; AMD Ryzen 7040, Ryzen AI 300), Framework Laptop 16 and Framework Laptop 12.
Each section covers one part of the machines. Other parts are added as their own sections.

## Embedded controller

Every Framework laptop has an embedded controller that runs Framework's fork of the ChromeOS EC. The Intel boards use a Microchip
MEC1521 or MEC172x, and the AMD boards a Nuvoton NPCX. It speaks the Chrome EC host command protocol (version 3) over the LPC bus, as a
Chromebook's EC does. `src/Hardware/EmbeddedController/ChromeEc/` drives it. The design follows Linux `cros_ec_lpc` (including its
MEC and Framework quirks), `ec_commands.h`, and Framework's `framework_tool` and `framework_laptop` module. **All of them are used
from memory. None of it has run on a Framework machine yet.**

### Layers

| File | What it does |
| --- | --- |
| `ChromeEcProtocol.hpp` | **The table.** Every port, command number, memory-map offset and layout constant, each marked `[sure]`, `[likely]` or `[unsure]`. A wrong number is changed here only. It also has the v3 request builder and the response-header parser. |
| `ChromeEcTransport.hpp/.cpp` | `IChromeEcPorts` (8-bit in/out plus a delay; `SystemChromeEcPorts` on the machine, a model in the tests). `ChromeEcTransport` runs the shared kick: wait for the busy bits at 0x204 to clear, write the packet, write 0xDA to 0x204, wait again, read the result byte at 0x200, then read and check the response. `LpcTransport` uses plain I/O at 0x800 (packet) and 0x900 or 0xE00 (memory map). `MecTransport` and `MecEmi` reach the 0x800..0x9FF window through the MEC EMI (address registers at 0x802/0x803, data at 0x804..0x807, byte access or 32-bit auto-increment, as `cros_ec_lpc_io_bytes_mec`). |
| `ChromeEc.hpp/.cpp` | The host command layer, with one `kernel::Mutex` per EC around each transaction and each memory-map read. It also holds an optional `IEcFirmwareLock` (the ACPI interpreters' locks) taken after the mutex. It provides start (signature, protocol-3 flag, HELLO, protocol info, version, chip, features), `GET_CMD_VERSIONS` (v1, cached), fans, keyboard backlight, `LED_CONTROL`, `CHARGE_STATE`, sensor names and the memory map (temperatures, fan RPM, battery, switches). `detect_chrome_ec()` is here too. |
| `FrameworkEc.hpp/.cpp` | Framework's commands: charge limit (get, set, disable), privacy switches, chassis intrusion (read only), fingerprint LED level, and on the Laptop 16 the input deck state (read-only mode) and the expansion bay. |
| `ChromeEcDevices.hpp/.cpp` | `ChromeEcHardwareMonitor` (an `IHardwareMonitor`) and `ChromeEcKeyboardBacklight` (an `IBacklight`, in percent). |
| `ChromeEcStack.hpp/.cpp` | Builds everything in place without allocating. It tries the transports in a safe order, and it handles charge-limit persistence through `IChargeLimitStore` and `describe()`. |
| `SystemChromeEc.hpp/.cpp` | The machine side. It searches the ACPI namespace for the device, uses the interpreters' locks, registers with the hardware monitor and provides `system_chrome_ec()`. |
| `SettingsChargeLimitStore.hpp` | Keeps the charge limit in the system settings (scope `power`, name `battery_charge_limit`). |

### Detection (safe on other machines)

1. The EC is identified by an ACPI device with `_HID`/`_CID` `GOOG0004`, `FRMW0004` or `FRMW0005`. Without one, only **reads** are
   made: the "EC" signature at 0x920 or 0xE20. `PNP0C09` alone does not count, because every laptop's ACPI EC has it.
2. A signature at 0x920 means an LPC EC. Nothing is written yet.
3. Once identified, the MEC EMI address registers (0x802/0x803) may be written, to read the map behind the EMI (EMI address 0x120).
   An "EC" there means a MEC.
4. Otherwise, a signature at 0xE20 means LPC with the map at 0xE00. If host commands do not answer that way, the next try is MEC host
   commands with the plain map at 0xE00 (`cros_ec_lpc`'s `REMAP_MEMORY` quirk). If a MEC answer turns out to be chance, the next try is
   LPC.
5. `ChromeEc::start()` must see the protocol-3 flag in the map (0x27 bit 1) and a correct HELLO. Otherwise nothing is exposed.

When the EC is found through ACPI, the battery interpreter's lock is held around every transaction, and with `TCOS_ACPI_EVENTS` the
event interpreter's lock as well. The firmware's AML on the AMD boards sends host commands under its `ECMT` mutex. Our interpreter
runs one evaluation at a time under its lock, so holding that lock keeps the AML out (the counterpart of `CROS_EC_LPC_QUIRK_AML_MUTEX`).
SMM is not kept out, which is the same exposure Linux has.

### What the system gets

- **Hardware monitor**: `SystemHardwareMonitor::set_embedded_controller()`. `cpu_temperature_millicelsius()` and `fan_rpm()` fall
  back to the EC when the Super I/O has nothing, so the Computer Manager's temperature and fan rows show the EC's values. The EC
  monitor is called outside the Super I/O spin lock, because its transactions may wait.
  - Temperatures come from `EC_MEMMAP_TEMP_SENSOR` (and bank B when thermal version >= 2), named by `EC_CMD_TEMP_SENSOR_GET_INFO`.
    Names containing cpu/peci/apu/soc get the CPU role.
  - Fan RPM comes from `EC_MEMMAP_FAN` (0xFFFF means no fan; 0xFFFE, stalled, reads as 0).
  - There is one PWM channel per fan. `set_pwm_duty` sends `PWM_SET_FAN_DUTY` (percent). Automatic sends `THERMAL_AUTO_FAN_CTRL`.
    `set_fan_target_rpm()` sends `PWM_SET_FAN_TARGET_RPM`. A machine with one fan uses version 0 (every fan). Otherwise version 1
    with the fan index is used, when `GET_CMD_VERSIONS` lists it. The EC does not report its curve's duty, so `duty_known` stays false
    until the OS sets one, and Manual without an earlier duty is refused.
- **Keyboard backlight**: `ChromeEcKeyboardBacklight` (`PWM_GET/SET_KEYBOARD_BACKLIGHT`). It is created, but nothing in the
  desktop drives it yet. Fn+Space is handled by the EC itself.
- **Charge limit**: `ChromeEcStack::set_charge_limit(max, store)` accepts 20..100, where 100 removes the limit. The value is kept in
  `/AppStore/System/system.db`. At boot, `restore_charge_limit()` sends it to the EC again.
- **Privacy switches**: `ChromeEcStack::privacy_switches()` (camera and microphone connected or cut off), logged at boot.
- **Firmware version**: `ChromeEcHardwareMonitor::describe()` / `ChromeEcStack::describe()` (RO/RW version, running image, chip,
  transport, charge limit, privacy switches, chassis count, expansion bay). It is logged at boot. These are not in the Computer
  Manager's device list yet.

### Uncertain numbers (check before relying on them)

| What | Value used | Mark |
| --- | --- | --- |
| Framework map remap base | 0xE00 | likely |
| MEC EMI base / EMI address = port - 0x800 | 0x800 | likely |
| ACPI ids (which board uses which) | GOOG0004, FRMW0004, FRMW0005 | likely |
| `EC_CMD_CHARGE_STATE` v0 parameter size (cmd + 8-byte union) | 9 bytes | likely |
| Charge limit 0x3E03 and its modes (disable bit 0, set bit 1, get bit 3, override bit 7) | | likely |
| Privacy switches 0x3E14, response {microphone, camera}, 1 = connected | | likely |
| Chassis intrusion 0x3E09 and its layout | | unsure |
| Fingerprint LED 0x3E0E, v0 {set level, get flag} and levels high/medium/low = 0/1/2 | | unsure |
| Input deck state 0x3E16, {board_id[8], state}, mode 0 = read only | | unsure |
| Expansion bay 0x3E1B, {state bits enabled/fault/door, board ids} | | unsure |
| `EC_FEATURE_*` bit numbers | | likely |
| Lowest accepted charge limit (our own floor) | 20 % | policy |

### Tests

Run `tests/run_host_tests.sh ChromeEcTest` (ASan + UBSan). It uses a model of the EC with both transports and the three map places.
The model is written from the same understanding of the protocol, so the tests check robustness and consistency, not the numbers. It
covers:

- framing and checksums;
- an EC that lies: bad checksum, wrong struct version, lengths beyond the packet or the caller's buffer, short answers, a refused
  packet, every single bit of a response flipped, and random packet contents;
- busy for a while, busy forever (Timeout within the poll budget), and busy before the request;
- every command;
- the monitor and the backlight;
- charge-limit persistence;
- detection that writes nothing on a machine without the EC, and only to the EMI address registers when ACPI names an EC that does
  not answer;
- the firmware lock;
- two threads whose transactions must not interleave (a detector in the model; it fires when the mutex is removed).

### Not done

- There is no user interface for the charge limit. No power or battery settings window exists to add it to, and a new window was
  out of scope.
- Nothing has run on hardware: no Framework machine, and no QEMU model of the EC.
- SMBIOS (`src/Hardware/Firmware`, `src/Hardware/Platform/Framework`) is not yet used as a positive identification, the way Linux
  uses its DMI quirk table.
- Host events (`EC_CMD_HOST_EVENT_*`, the SCI the EC raises), `EC_RES_IN_PROGRESS` with `GET_COMMS_STATUS`, the battery commands
  (`BATTERY_GET_STATIC/DYNAMIC`), and LED-matrix or keyboard-matrix commands are not implemented.
- `ChromeEcHardwareMonitor` is not in `HardwareMonitorSet::snapshot()`. It only feeds the CPU temperature and fan fallbacks.

## Which machine this is (SMBIOS)

`src/Hardware/Firmware/Smbios.*` is the system's SMBIOS reader. It looks in the UEFI configuration table for the 64-bit `_SM3_` entry
point first, else the 32-bit `_SM_`, then walks the structure table once at boot. It keeps the BIOS, system, board, chassis, first
processor and populated memory devices, and anything can read them through `system_smbios()`. `parse_smbios_entry` and
`parse_smbios_table` work on views, checking every checksum, length, string number and the double-NUL that ends each string set.
Boot logs the maker and model.

`src/Hardware/Platform/Framework/FrameworkPlatform.*` turns that into a Framework model. `identify()` looks for words in the product name
("Laptop 16", "AMD", "AI 300", "Core Ultra", "12th Gen" ...), so slightly respelled names still match. For each model it gives the EC chip,
the feature set (input modules, expansion bay, privacy switches, AMD sensor hub) and the number of expansion card bays. An unknown
Framework product gets a generic entry instead of a wrong one.

## Expansion cards and input modules

The catalogue (`find_module`) names Framework's USB devices: the HDMI and DisplayPort cards, the storage cards, the Laptop 16 keyboard,
numpad, macropad and LED matrix modules, and the fingerprint reader. Product ids marked "(unverified)" in the source are the least certain.
The other cards are plain USB parts with drivers already in the tree:
- USB-A and USB-C cards: the hub and the port.
- microSD and SD cards: USB mass storage.
- Ethernet card (Realtek RTL8156): its default configuration is vendor-only, so `UsbNetworkInterfaces` moves it to its CDC-NCM
  configuration. There is no native r8152 driver.
- Laptop 16 keyboards, numpad and macropad: QMK firmware, USB HID.

## USB serial ports and the LED matrix module

`src/Hardware/SerialPort/Usb/UsbCdcAcm.*` is a generic USB CDC-ACM serial port (an `ISerialPort`). It covers RP2040 and Arduino boards,
modems and debug consoles, and the LED matrix module:
- The framing goes out as SET_LINE_CODING and RTS/DTR as SET_CONTROL_LINE_STATE.
- Modem lines come in from SERIAL_STATE notifications.
- Received bytes come from a bulk IN endpoint the controller keeps armed (xHCI, EHCI); sending is one staging buffer per poll.

`UsbSerialPorts` adds the ports present at boot and those plugged in later to `SystemSerialPorts`, which gained `remove_gone()` for ports
that are pulled out. Known products get their names.

`LedMatrix.*` drives the Laptop 16 LED Matrix Input Module (32AC:0020, 9 x 34 LEDs) over that port. Each command is `0x32 0xAC` followed
by the command and its parameters, as in Framework's inputmodule-rs (from memory: the table is at the top of `LedMatrix.cpp`). It can set
brightness, show a pattern or a percentage, draw on/off or greyscale pictures, sleep and animate, and read the firmware version. Nothing in
the desktop uses it yet.

## Sensors

The AMD boards keep the ambient light sensor behind AMD's Sensor Fusion Hub; see [AmdSensorFusionHub.md](AmdSensorFusionHub.md)
(opt-in, `-DTCOS_AMD_SFH=ON`). The Intel boards use the ISH (`-DTCOS_ISH=ON`).

## USB-C (UCSI)

The USB-C ports are read through UCSI (`src/Hardware/UsbTypeC/`), which works the same on any laptop that exposes it, not only Framework's.
The firmware describes a device with `_HID` `USBC000` or `_CID` `PNP0CA0`. Its `_CRS` names the shared mailbox (VERSION, CCI, CONTROL,
MESSAGE IN/OUT). Its `_DSM` (UUID `6f8398c2-7ca4-11e4-ad36-631042b5008f`) copies the mailbox to the embedded controller (function 1) and
back (function 2). The firmware sends `Notify(device, 0x80)` when CCI changes.

- `AcpiUcsiTransport` finds the device, reads `_CRS` (a 32-bit fixed range, or a DWord or QWord range below 4 GiB), checks function 0 of `_DSM`,
  and reads and writes the mailbox through the AML host's SystemMemory accesses, the same path the firmware's own fields use.
- `UcsiCore` runs the OS side, after Linux `ucsi.c`: PPM_RESET, SET_NOTIFICATION_ENABLE, GET_CAPABILITY, then for each connector
  GET_CONNECTOR_CAPABILITY, GET_CONNECTOR_STATUS (the 1.0, 1.2, 2.0 and 2.1 layouts), GET_PDOS (the source's PDOs, up to 7),
  GET_ALTERNATE_MODES (the partner's: DisplayPort, Thunderbolt) and GET_CABLE_PROPERTY. Each command is acknowledged with ACK_CC_CI.
  Every wait has a timeout. A PPM that stays BUSY gets a CANCEL. ERROR is followed by GET_ERROR_STATUS.
- `UsbTypeCSystem` runs on its own kernel thread (`usb-c`). It wakes on the ACPI notification and also checks CCI every second. It publishes
  the connectors through RCU. If the PPM stops answering, it starts it again later.
- The computer manager lists the ports under USB ("USB-C port 1"). Properties shows the partner, the data and power roles, the contract
  (voltage, current, power), the source PDOs, the alternate modes and the cable.

Tests: `tests/run_host_tests.sh UcsiTest`. It uses a model PPM, plus AML shaped like a laptop's whose `_DSM` passes the mailbox through a
fake embedded controller.

Not checked on real hardware. The UCSI 2.1 power-reading units (5 mA and 5 mV steps) and the field positions come from the spec and Linux, not
from a real device. The system does not send role swaps or alternate-mode entry (SET_UOR, SET_PDR, SET_NEW_CAM). It does not read
GET_CURRENT_CAM, and it does not use MESSAGE OUT. A UCSI 2.x PPM whose mailbox is smaller than 0x210 bytes keeps the 16-byte MESSAGE IN.

## Tests (this part)

`tests/run_host_tests.sh FrameworkTest` (ASan/UBSan) covers:
- SMBIOS: a Framework-like table, both entry points and a bad checksum, a string past the set, a structure longer than the table, every
  cut-short table and every single-bit flip;
- the model catalogue against every product name, a non-Framework machine, and duplicate module ids;
- CDC-ACM: descriptors, including cut-short ones and one without a union descriptor; line coding; SERIAL_STATE; a port against a fake host
  (open, write, read, lines, break, overrun, unplug); a controller without bulk IN streams;
- hot-plug through `UsbSerialPorts` and `SystemSerialPorts`;
- every LED matrix command on the wire.

## Not done

- **Fingerprint reader** (Goodix 27C6:609C): an encrypted, vendor-specific protocol, not attempted.
- **Laptop 16 graphics module** (Radeon RX 7700S, RDNA3): the AMD GPU driver covers GCN and Vega only.
- **Intel Thunderbolt 4 / USB4 on the Intel boards:** needs a software connection manager; the Thunderbolt driver knows Alpine Ridge's
  firmware connection manager.
- **No user interface yet** for the charge limit or the LED matrix.
