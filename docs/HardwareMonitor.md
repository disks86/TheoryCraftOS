# Motherboard hardware monitoring

Desktop boards measure their temperatures, fan speeds and supply voltages, and drive their fan headers, with the hardware monitor of
the Super I/O chip (Nuvoton, ITE or Fintek). `src/Hardware/HardwareMonitor` finds that chip and reads it. The register layouts follow
the Linux `nct6775`, `it87` and `f71882fg` drivers.

## Design

| File | What it does |
| --- | --- |
| `IHardwareMonitor.hpp` | The interface: temperature (millidegrees C), fan (RPM), voltage (millivolts, scaled by the chip's internal dividers) and PWM (duty 0..255, manual or automatic) channels, each with a short label and a role (CPU, system, chipset). Every reading is an `Optional`: a missing sensor or a nonsense register value gives no reading, never a made-up number. PWM writes return `bool`. |
| `PortIo.hpp/.cpp` | `IPortIo` (8-bit `read8`/`write8` on a 16-bit port) with `X86PortIo` over `IoPort`, plus `IndexedPorts` for the address/data pair at base+5/+6. A scripted model takes its place in the tests. `ISmbusHostPorts` could not be reused because it is relative to one base. |
| `SuperIo.hpp/.cpp` | Detection at 0x2E and 0x4E: the keys (0x87 0x87 for Nuvoton and Fintek, 0x87 0x01 0x55 0x55 for ITE, with 0x55 0xAA at 0x4E), the chip id at 0x20/0x21 (Fintek's vendor id 0x1934 at 0x23/0x24 tells it apart from Nuvoton), logical-device select (0x07), activate (0x30 bit 0), base (0x60/0x61) and the exit sequences. Also the chip table: ids, names, sensor device, and whether a driver here supports the chip. On NCT6791 and later the probe clears the I/O-space lock (global register 0x28 bit 4), as Linux does. |
| `NuvotonNct67xx` | NCT6775/6776/6779/6791/6792/6793/6795/6796/6797/6798/6799. Banked 16-bit registers (bank in index 0x4E, selected on every access) behind a vendor-id check (0x4F reads 0xA3 or 0x5C). Temperatures come from 0x027, 0x150 and (NCT6775/6776) 0x250, labelled by their source registers (0x621..). Fans are 16-bit counts (NCT6775), 13-bit counts (NCT6776) or RPM registers (NCT6779 and later). Voltages use Linux's `scale_in`. PWM duty is read at 0x001/0x003/..., written at 0x109/0x209/..., and the mode is bits 7:4 of 0x102/0x202/.... A `NuvotonLayout` table per generation holds all of this. |
| `Ite87xx` | IT8705/8712/8716/8718/8720/8721/8726/8728/8771/8772/8792 and IT8620/8622/8625/8628/8655/8665/8686/8688/8689/87952. It checks 0x58 == 0x90. Temperatures are at 0x29..0x2B (bits 7:6 of 0x51 name the PECI input), fans are 16-bit counts (8-bit with a divisor on the IT8705, selectable on the IT8712), and voltages are 16, 12 or 10.9 mV per count with the internal inputs doubled. PWM uses 7-bit duty on the older parts and the 8-bit duty registers on the newer ones, with bit 7 of the control register meaning automatic. An `IteLayout` table per generation holds this. |
| `FintekF718xx` | F71862/F71863, F71868, F71869(A), F71882, F71889(E/A). Monitoring must be started (0x01). Temperatures are at 0x72/0x74/0x76, fans are 12-bit counts at 0xA0+16n (RPM = 1500000/count), voltages are 8 mV with VCC3V/VSB3V/VBAT doubled, PWM duty is at 0xA3+16n, and the mode is in two bits per output of 0x96. |
| `HardwareMonitorSet` | Probes both ports and owns up to two chips (for example IT8686 at 0x2E and IT8792 at 0x4E). The drivers live in `Optional` slots in the set, so there is no allocation: `SystemChipset`, which has no allocator, holds it from boot. It provides `cpu_temperature_millicelsius()`, `fan_rpm()` and `snapshot()`. It is not thread-safe. |
| `SystemHardwareMonitor` | The set on the real ports inside a `Synchronized`, so that one core's index/data or bank sequence never interleaves with another's. `SystemChipset::probe` probes it (whatever the chipset) and registers it as `system_hardware_monitor()`. |

`SystemMonitor::cpu_temperature_millicelsius()` falls back to the board's CPU temperature when the processor's own sensor gives
nothing. `SystemMonitor::fan_rpm()` feeds a "Fan speed" row in Computer Manager's Performance tab. The row is hidden while no fan
reports. Fan outputs are only written when a caller asks. Nothing in the OS changes them on its own.

## Tests

`tests/run_host_tests.sh HardwareMonitorTest` (ASan + UBSan) runs against a model of the port protocol: keys, logical devices,
exits, and the sensor window with Nuvoton banks. It covers:

- detection of each family, including two ITE chips at once and Fintek told apart from Nuvoton;
- no chip, ids 0xFFFF and 0x0000, an unknown id, the unsupported NCT6687, and a window that is unassigned, inactive or does not answer;
- every channel kind with its scaling;
- PWM manual, automatic and back to the firmware's curve;
- random and bit-flipped register images, fan counts of 0 and 0xFFFF, and random configuration space;
- six threads sharing one set behind a mutex, with the model flagging any split index/data pair or bank selection.

## Not validated / not done

- **None of this has run on real hardware.** The register layouts, ids, scaling tables and fan/PWM counts come from the Linux
  drivers as remembered, not from datasheets or a board. Treat every value as unverified until checked against `sensors` on the same
  machine. Known soft spots:
  - the NCT6798/6799 voltage scaling (Linux has changed it between versions; the NCT6779 table is used);
  - the ITE 10.9 mV parts and the fan and PWM counts of the IT86xx;
  - Fintek's mode bits.
- NCT6683/6686/6687 (EC-style interface), F71858 (16-bit temperature format), F71808E and F81865 are identified but not read. The
  F71878 is not in the table: its id was not known with confidence.
- Only the main temperature inputs are read: Nuvoton 0x027/0x150/0x250 and ITE's three. The extra temperature registers (NCT6779+ "temp
  alternate" and PECI/TSI registers, ITE's 4..6) are not, and neither are limits, alarms, beep, VID, intrusion, or the Smart Fan curve
  points.
- Nuvoton fan-input pin multiplexing is not read, so unconnected headers read 0 RPM instead of being hidden.
- The firmware's ACPI code or SMM may use the same chip at the same time. Nothing can lock them out, and Linux has the same
  exposure. ITE's "no exit" quirk for a second chip is not handled: the probe always exits configuration mode.
- Laptops keep these sensors in the embedded controller, which is not covered here.
