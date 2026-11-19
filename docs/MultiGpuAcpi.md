# Hybrid graphics: the ACPI side

`src/Hardware/GraphicsProcessingUnit/Multi/Acpi/` holds the firmware methods that cut a laptop's discrete GPU's power to D3cold and, on
muxed machines, move the panel between the GPUs. They implement `IGpuPlatformPower` and `IGpuMux` (`Multi/GpuPlatformPower.hpp`). The
code follows Linux: the PCI core's D3cold through ACPI power resources, `nouveau_acpi.c` and `amdgpu_atpx_handler.c`.

| File | What it does |
|------|--------------|
| `GpuAcpiNamespace` | finds a PCI function's Device, calls `_DSM`, reads and switches the `_PR0`/`_PR3` power resources |
| `AmdAtpx` | AMD's ATPX method: answer parsers, the `amdgpu_atpx_validate` rules, power control, display/DDC mux |
| `GpuAcpiPower` | the NVIDIA handlers (Optimus, JT, the old hybrid mux `_DSM`), `probe()`, and the `find()` / `find_mux()` factories |

## Finding the GPU in the namespace

The GPU's Device has `_ADR = device << 16 | function`. `_ADR` has no bus number, so the code matches the bus through the parent Device:

- A Device right below a host bridge (`PNP0A08`/`PNP0A03`) is on the bus that host bridge's `_BBN` names (0 if it has no `_BBN`).
- A Device below a root port is on that port's secondary bus, which the namespace does not record. The caller passes the root port's
  `PciAddress` as a hint. The root port is matched by its `_ADR` on the host bridge whose `_BBN` equals the port's bus.

Limits:

- `_SEG` is ignored.
- A bridge behind another bridge (a switch, Thunderbolt) is matched by its `_ADR` alone.
- Without a hint, a GPU behind a root port is a guess. Several Devices can have `_ADR 0`, for example the Wi-Fi card. The code picks the one
  that looks most like a GPU (`_ROM`/`ATRM`, then `_DSM`/`ATPX`, then `_PR3`/`_PS3`). On a tie, the first in namespace order wins.

## D3cold through power resources

The code reads `_PR3` from the GPU's own Device first. If the GPU has none, it reads `_PR3` from the root port, which is where most laptops
put it. It reads `_PR0` from the same Device. A list entry counts as a power resource only if it has its own `_ON` and `_OFF` methods. The
code does not need the interpreter's `NodeKind`, so the interpreter was not changed.

- **`power_off`** runs:
  1. the GPU's `_PS3`;
  2. the root port's `_PS3`, if the resources are the root port's;
  3. `_OFF` on each `_PR3` resource, last first. It skips a resource whose `_STA` says it is off.

  It stops at the first fault, so a failed `_PS3` cuts nothing.
- **`power_on`** runs:
  1. `_ON` on the `_PR0` resources, then on the `_PR3` resources that are not in `_PR0`, first first. It skips a resource whose `_STA` says
     it is on.
  2. the root port's `_PS0`, then the GPU's `_PS0`.

  It keeps going after a fault, so that as much as possible comes up, and then returns false.

Resources shared with other devices are reference counted: see "Shared power resources" below.

## NVIDIA (`find()` with vendor `0x10DE`)

- **JT `_DSM`** (`CBECA351-…`, revision `0x103`). Function 0 must report bit 1. The code reads the capabilities with function 1. If the GPU
  or its root port has power resources, `NvidiaJtPower` uses them, as Linux does.
- **Optimus `_DSM`** (`A486D8F8-…`, revision `0x100`). Function 0 must report bit `0x1A`. On probe, the code reads the capabilities with
  function `0x1A` and argument 0. `NvidiaOptimusPower::power_off` then:
  1. calls function `0x1B` with 3, where function 0 reports it;
  2. calls function `0x1A` with `0x03000001` (power off at `_PS3`), passed as a 4-byte buffer;
  3. runs `_PS3` and the power resources.

  As in nouveau (`optimus_skip_dsm`), the `_DSM` is skipped when `_PR3` exists. The handler is created only when there are power resources
  or a `_PS3`.
- **Old hybrid mux `_DSM`** (`9D95A0A0-…`, revision `0x102`, integer argument). Function 0 must report bit 3. The `_DSM` can be on the
  discrete GPU's Device or on the integrated GPU's (pass `integrated_gpu`).
  - Power: function 3 with 2 turns the GPU off, with 1 turns it on.
  - Mux (`NvidiaHybridDsm`, from `find_mux`): function 2 with `0x11` moves the panel to the integrated GPU, with `0x12` to the discrete GPU.
    The answer `0x80000002` means the call failed.

Order: JT with power resources, then Optimus, then the hybrid `_DSM`, then the power resources alone.

## AMD (`find()` with vendor `0x1002`)

ATPX is looked for in this order: on the discrete GPU's Device, on the integrated GPU's, on the root port's, then on any Device in the
namespace.

- Function 0 returns the version and the supported functions.
- Function 1 returns the PX parameters. `valid_bits` = flags & valid flags.
- The rules from `amdgpu_atpx_validate`:
  - A separate I2C mux means both muxes are present.
  - A muxed CRT, TV or DFP output means a display mux is present.
  - Dynamic PX or dynamic dGPU power-off means power control is available.
  - **MS hybrid graphics (bit 14)**: power control is off and the machine is hybrid.
- Choice:
  1. If the machine is hybrid, or the GPU or its root port has `_PR3`, the power resources are used. A hybrid machine without `_PR3` gets
     nothing.
  2. Otherwise, ATPX `POWER_CONTROL` is used: `{size 3, state}`, with a 200 ms wait after power off through the `sleep_ms` callback, run
     outside the interpreter's lock.
- Mux (`find_mux`, when there is a display mux): switch-start, display mux, I2C mux, switch-end, as in `amdgpu_atpx_switchto`.
  `switch_ddc` uses the I2C mux.
- Each answer's size word is checked against both the buffer and the structure. A short or lying answer, or one that is not a buffer, means
  ATPX is not used.

Neither ATPX nor the NVIDIA mux `_DSM` can be asked which GPU drives the panel. `active()` reports the last switch, or false before one.

## Calling it

```cpp
UniquePointer<IGpuPlatformPower> power =
    GpuAcpiPower::find(aml, gpu, root_port, vendor_id, allocator, integrated_gpu, sleep_ms);
UniquePointer<IGpuMux> mux =
    GpuAcpiPower::find_mux(aml, gpu, root_port, vendor_id, allocator, integrated_gpu, sleep_ms);
GpuAcpiProbe probe = GpuAcpiPower::probe(aml, gpu, root_port, vendor_id, integrated_gpu);   // for the log
```

Every handler takes the interpreter's recursive lock for each evaluation. The interpreter must outlive the handlers.

## Tests

`tests/GpuAcpiPowerTest.cpp` (`tests/run_host_tests.sh GpuAcpiPowerTest`) builds a DSDT whose methods record their arguments and log calls
into root Names. It covers:

- device lookup: hint, decoy, `_BBN`, a missing bridge;
- `_PR3` on the GPU and on the root port: order, `_STA` skips, `_PR0`;
- garbage `_PR3` lists and a faulting `_PS3`;
- Optimus: the UUID bytes, revision, function and buffer argument, and the skip when `_PR3` exists;
- JT;
- the hybrid mux `_DSM`;
- ATPX: verify, parameters, power, mux sequence, the hybrid rule, and truncated, lying or bit-flipped answers;
- the `_DSM` result helpers.

## Not done

- **Not checked on hardware.** The AML is test-built.
- `_SEG`, deeper bridge hierarchies, and the case where no hint is given (see above).
- JT power control (`JT_FUNC_POWERCONTROL`) and GC6.
- Optimus `_DSM` quirks: the DMI lists Linux keeps.
- ATPX functions 8 and 9 (connector mapping).
- The `GpuPowerMechanism` list has no entry for the old NVIDIA hybrid `_DSM`, so `NvidiaHybridDsm` reports `NvidiaOptimus`.

## Shared power resources

ACPI 6 (7.2) lets several devices share a power resource; it may only be turned off when no device that needs it is in a state that needs
it. Linux keeps a reference count per resource (`drivers/acpi/power.c`). Here that is `AcpiPowerResourceRegistry`
(`GpuAcpiNamespace.hpp`), one per machine (`AcpiPowerResourceRegistry::global()`, constant-initialized):

- It records, per resource node, the holders (namespace Devices) and whether each needs the resource now. Up to 32 resources, 8 holders each.
- `introduce(resource, holder)`: the first sighting counts the holder as needing the resource (it was found in D0); later calls change nothing.
- `need()` / `drop()` change one holder and return how many need it now / how many others still do. `holders_needing()` and `needs()` read it.
- A full table means "not tracked": `need()` says 1 and `drop()` says 0, which is the old behaviour (the resource is turned off).
- Thread safe with its own `kernel::Mutex`. The decision and the `_ON`/`_OFF` that follows run under the interpreter's lock, so another
  holder cannot change things in between.

`GpuPowerResourceSet` uses it:

- `read()` introduces the GPU as holder of its `_PR0` and `_PR3` resources. The holder (`identity()`) is the GPU's Device, or the Device that
  holds the resources when the GPU has none.
- `power_off()` drops the GPU from all of them and runs `_OFF` only for `_PR3` resources no other holder needs (another GPU, another device's
  root port on the same rail). If an `_OFF` faults, the GPU stays in D3hot and takes its claim back.
- `power_on()` takes them all again before turning them on.
- `use_registry()` points a set at a private registry (tests).

Other code (the HD-audio function, a Wi-Fi root port) can register as a holder with `introduce()`/`need()`/`drop()` on the global registry.
Nothing does yet: today only GPU sets are holders.

## Device readiness durations (PCI firmware `_DSM` function 9)

The PCI Firmware Specification (3.3, 4.6.9, "_DSM for Specifying Device Readiness Durations") defines function 9 of the PCI `_DSM`, UUID
`E5C937D0-3553-4D7A-9117-EA4D19C3434D` (`kPciFirmwareDsmUuid`), revision 3. It returns a Package of five Integers, each a time in microseconds
after which the function answers configuration requests:

1. after a Conventional Reset,
2. after the link comes up (DL_Up),
3. after a Function Level Reset,
4. after D3hot to D0,
5. after VF Enable is set.

`AcpiPciReadiness::read()` calls it only when function 0 (same UUID and revision) lists bit 9, and only accepts a package of at least five
elements. An element that is not an integer or does not fit 32 bits is left empty. `d3cold_delay_ms()` takes the longer of the reset and
DL_Up times, rounds up to milliseconds, and uses it only when it is shorter than the default of 100 ms (the firmware can shorten the wait, never
lengthen it). Linux does the same in `pci_acpi_optimize_delay()`, though it uses only the reset time.

`GpuPowerResourceSet::read()` reads it from the GPU's own Device first, which is where the specification and Linux put it, and then from the
root port's. Every handler's `power_on_delay_ms()` returns that value: `AcpiGpuPowerResources`, `NvidiaOptimusPower` and `NvidiaJtPower` take
it from their set, and `NvidiaHybridDsm` and `AmdAtpx` get it as a constructor argument from `find()`. The D3hot-to-D0 time (element 4) is read
but not used. The GPU manager's D3hot wait is its own.

Tests (`GpuAcpiPowerTest`):

- shared rail: the GPU and a Wi-Fi root port on one resource; off only after both, on again; through `find()`; a private registry; an `_OFF`
  that faults;
- the registry on its own: idempotent introduce, counting, full tables, and six threads (also run under ThreadSanitizer by hand);
- readiness: on the GPU, on the root port only, function 0 without bit 9, a short package, times above the default, a string element, and the
  NVIDIA hybrid and ATPX handlers.

Not done or not verified:

- The function 9 layout is from the specification text as remembered and from Linux's reader; it has not been checked against firmware.
- Table loads that free nodes would need `clear()`; the interpreter never frees them today.
