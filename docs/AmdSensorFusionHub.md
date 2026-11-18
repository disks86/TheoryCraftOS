# Sensors (AMD Sensor Fusion Hub)

AMD Ryzen laptops run their sensors on the Sensor Fusion Hub (SFH), a small co-processor (the MP2) in the SoC. The Framework Laptop 13 (AMD) and
16 use it for the ambient light sensor; convertibles also have an accelerometer, gyroscope, magnetometer and a presence sensor behind it.

| PCI id    | Generation | Parts                         |
|-----------|------------|-------------------------------|
| 1022:15E4 | SFH 1.0    | Renoir, Cezanne, Rembrandt    |
| 1022:164A | SFH 1.1    | Phoenix, Hawk Point, Strix    |

The driver is `src/Hardware/Sensor/AmdSfh/` (`AmdSfhDriver`, register layout in `AmdSfhRegisters.hpp`). It maps BAR 2, finds the sensors the
firmware reports (SFH 1.0: the mask in P2C_MSG3; SFH 1.1: the sensor list in the firmware's shared memory), enables each one and adds it to the
system's `SensorHub` as an `ISensor`, next to the I2C HID, Intel ISH and ACPI sensors. Values are read from the per-sensor DMA buffers (1.0) or the
shared memory (1.1) when a program reads the sensor, or by `poll()`:

- ambient light in lux, accelerometer in g, gyroscope in degrees per second, magnetometer in gauss, presence as 1 or 0;
- values are untrusted: a sample that is not finite, out of any real sensor's range, or changed while it was read is dropped;
- a command the firmware does not answer times out after 1.6 s, and start-up stops at the first one;
- `suspend()` / `resume()` disable and re-enable the sensors; `stop()` disables them and frees only the buffers the firmware can no longer write.

Start-up is behind the CMake option `TCOS_AMD_SFH` (off by default, like the Intel ISH, because it has not run on a real machine yet).

**Not verified.** The register offsets, command and response bit layouts, the 1.1 shared memory layout and the scale factors were written from
memory of the Linux `amd_sfh` driver, not from AMD documentation. They are all in one table (`namespace amd_sfh_layout`), each marked with how
sure it is. The host test (`tests/run_host_tests.sh AmdSfhTest`) checks the driver's logic against a model that uses the same table, so it cannot
catch a wrong number. Not done: interrupts (the driver polls and turns the legacy interrupt off), the 1.1 "rver 1" register placement of later
parts, the 1.1 lid/tablet-mode (SRA) sensor, ALS colour temperature, and anything that uses the ambient light value (there is no automatic
brightness yet).
