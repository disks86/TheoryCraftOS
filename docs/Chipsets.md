# Chipsets

`src/Hardware/Chipset` covers the chips next to the CPU: the Intel platform controller hub (PCH, or the PCH part of an Intel SoC) and the AMD
fusion controller hub (FCH) plus the Promontory chipset of AM4/AM5 boards. It identifies the part, lists its PCI functions, finds its SMBus host,
and reads the SPD of the memory modules on that bus. On Intel it also reports the BIOS flash protection and the management engine status.

## Files

| File | What it does |
| --- | --- |
| `IChipset.hpp/.cpp` | The `IChipset` interface, `ChipsetFunction` (what a PCI function of the chipset is, from its class code and slot), `ChipsetInventory` |
| `Intel/IntelPch` | Catalogue: every hub from ICH8 to Panther Lake/Wildcat Lake, the C600..C740 server PCHs, Birch Stream and the Atom SoCs. Each has its SMBus IDs, form (discrete, on package, SoC) and whether SPI has its own function. Also which hubs each CPU family/model/stepping pairs with |
| `Intel/IntelPchChipset` | Bus 0 devices 0x10..0x1F: hub identity (by SMBus ID, else from the CPU), SMBus BAR/HOSTC, BIOS_CNTL (0xDC), HECI HFSTS1 |
| `AMD/AmdChipsets` | Catalogue: FCH generations (SB7x0, SB8x0, Hudson, KERNCZ) with how each one decodes its SMBus base, and the Promontory 300/400/19/21 and Bixby (X570) chipsets by device ID |
| `AMD/AmdFchChipset` | FCH at 00:14 (revision, SMBus base from PM registers or config space, selected SMBus port), the board chipset and its chip count, Data Fabric nodes |
| `Smbus/SmbusHost` | The i801/PIIX4 host protocol (quick, byte, byte data, word data, block) over `ISmbusHostPorts`, with Intel INUSE_STS and AMD host-semaphore arbitration, a 100 ms timeout and kill |
| `Smbus/SmbusIoPorts` | The real I/O port backend (kernel only) |
| `Smbus/Spd` | SPD decode for DDR3/DDR4/DDR5 (type, form, capacity, ranks, width, ECC, speed bin, JEP106 maker), and reading over SMBus with EE1004 page and SPD5118 hub page handling |
| `Intel/IntelGpioLayouts` (+ `.inc`) | GPIO community layouts of 42 Intel GPIO controllers: Sunrise Point through Meteor Lake/Arrow Lake/800 series, the server PCHs and the Atom SoCs. Each has the memory resource index, the pin range and the pad groups with their ACPI GPIO numbers. Generated from the Linux pinctrl drivers by `scripts/gen_intel_gpio_layouts.py` |
| `Bus/I2c/IntelGpio` | `IntelPchGpio`: attaches a layout's communities and maps an ACPI GPIO number to its pad. The pad stride comes from REVID (16 bytes from 0x94). Reads PADCFG0 (level, driven value, function). `PlatformI2cHid` picks the layout by `_HID`/`_UID` |
| `Intel/IntelPchThermal` | The PCH thermal sensor (00:12.0, Haswell..Comet Lake, Wellsburg, Lewisburg): TEMP/TSEL/CTT through BAR 0. It is an `IThermalSource` named "chipset" and is added to the `ThermalManager` |
| `Intel/IntelTcoWatchdog` | The TCO watchdog (iTCO v4/v6): timeout, start/stop/ping, NO_REBOOT (v6 in TCO1_CNT), and "the last reset was the watchdog". `IntelPchChipset` finds TCOBASE/TCOCTL in the SMBus function |
| `AMD/AmdNodes` | Zen nodes: the root complexes (several per node on EPYC), SMN read/write through each node's 0x60/0x64 pair, Data Fabric config and FICAA/FICAD indirect reads (old and new offsets), DF revision (DF2..DF4.5), fabric block count, DRAM hole, and Tctl/CCD temperatures with k10temp's layouts |
| `SystemChipset` | Kernel glue: picks the driver (AMD first on AMD CPUs, otherwise Intel first), maps the FCH PM MMIO block, owns the `SmbusHost` in a `Synchronized` |

`main.cpp` probes after the `Platform` line. It logs `Chipset` lines for identity, functions, SMBus, flash, ME, PCH temperature, TCO watchdog, and the
AMD nodes/fabric with per-node Tctl and CCD temperatures, then one `Memory` line per module.

## Design rules

- **Configuration space is only read.** The probe never writes PCI config. On AMD it reads three PM registers. If the firmware left the SMBus
  host disabled (HOSTC.HST_EN, DecodeEn bit 4), it stays disabled and the boot log says so.
- **The SMBus is shared through one lock.** Use `SystemChipset::with_smbus`. The arbitration with the firmware (Intel INUSE_STS, AMD
  SMBSLVCNT bit 4/5) is claimed and released around every transaction. A held semaphore returns `Busy` and the transaction is not retried.
- **SPD paging is restored.** DDR4 EE1004 page selection (0x36/0x37) and the SPD5118 MR11 page are put back to how they were found. A hub
  whose page register is write-protected still reports type and speed from page 0.
- **AMD FCH revision ≥ 0x51 reads PM through MMIO** (0xFED80300), as Linux's piix4 does. Without a register mapper it falls back to
  0xCD6/0xCD7 and treats all-0xFF as unreachable.
- **SPD is skipped when the FCH routes the SMBus to a port other than 0.** The port is not switched.
- **One lock for SMN.** `AmdNodes` lives in a `Synchronized` in `SystemChipset`. Once nodes are found, `cpu_temperature_millicelsius` is pointed at it
  (`set_amd_smn_reader`), so the CPU-temperature read and the chipset's own SMN and DF reads never split each other's index/data pairs.
- **The watchdog is never armed by the probe.** Nothing in the OS pings it yet, so a TCO the firmware left counting is halted, and the boot log says
  so. The probe also reports and clears SECOND_TO_STS ("the last reset was this watchdog"). Version 4 can be halted but not armed, because
  NO_REBOOT is behind the P2SB bridge.
- **The PCH sensor is read, not enabled.** If the firmware left TSEL off, the boot log says so.
- **GPIO tables are generated, not typed.** Regenerate with `python3 -I scripts/gen_intel_gpio_layouts.py <linux tree> > src/Hardware/Chipset/Intel/IntelGpioLayouts.inc`.

## Sources

SMBus IDs and register layout: Linux `drivers/i2c/busses/i2c-i801.c` and `i2c-piix4.c`. EE1004/SPD5118: Linux `drivers/misc/eeprom/ee1004.c`
and `drivers/hwmon/spd5118.c`. HFSTS1 fields: coreboot `me.h`. Promontory IDs: Linux `xhci-pci.c` and the PCI ID repository. SPD layout:
JEDEC SPD annexes for DDR3/DDR4/DDR5.

## Testing

`tests/run_host_tests.sh ChipsetTest` (ASan + UBSan) covers:

- the SMBus host against a register-level model of both flavors: busy, collision, timeout and kill, block bounds;
- SPD decoding, including every truncated prefix and every single-bit flip;
- SPD reading against EE1004 and SPD5118 models;
- both catalogues, cross-checked against the Intel CPU generation rules (every model the CPU table knows pairs with a hub, apart from listed exceptions);
- both drivers on fake PCI buses: Z390, q35 ICH9, a hidden SMBus, X670E/B650/X570, a Zen+ laptop, SB700.

`tests/run_host_tests.sh ChipsetDevicesTest` covers:

- the generated GPIO layouts: groups tile their communities, no GPIO number collides, `_UID` matching, and Cannon Point-LP's gapped numbering
  reaching the right pad with 16-byte pads;
- the PCH sensor: off, locked, critical point, a missing BAR;
- the TCO watchdog against a register model that flags a write of NMI_NOW, a firmware-locked NO_REBOOT, and version 4 halting;
- AMD nodes against a model with SMN per root and FICAA/FICAD:
  - Zen 3 DF3 with legacy offsets, and Zen 4 DF4/DF4.5 with the new offsets;
  - a two-socket EPYC with four roots per node;
  - CCD validity, and six threads sharing the SMN pair without splitting it. A `std::mutex` stands in for the kernel `SpinLock`, which executes `cli`.

In QEMU (q35, KVM) the boot log shows `Chipset: ICH9 ...`. OVMF leaves the ICH9 SMBus host disabled, so no SPD is read there.

## Not done

- Not run on real hardware. The ID tables come from Linux and the PCI ID repository. The Promontory 300/400 SATA and bridge IDs, and B840 being
  Promontory 19, are the least certain entries.
- Lunar Lake has no published SMBus ID, so it is named from the CPU only. Lunar Lake and Panther Lake GPIO layouts come from ACPI `_DSD` in Linux
  (`pinctrl-intel-platform.c`) and are not in the generated table. Lynx Point (Haswell) GPIO uses another register layout and is not covered.
- GPIO is read-only: no interrupts (pins are polled), no pad changes. Nothing lists the GPIO state of a machine yet.
- PCH temperature from Ice Lake on goes through the PMC/ACPI and is not read here; the ACPI thermal zones still cover it where the firmware has them.
- TCO watchdog: there is no pinging service, so the OS cannot arm it on purpose yet. The ICH/5..9 series TCO (in the LPC ACPI block, NO_REBOOT
  in RCBA) and AMD's FCH watchdog (sp5100_tco) are not covered.
- AMD: no DRAM address map or address translation (atl), no HSMP mailbox, no SMU messages, no Data Fabric performance counters, no FCH GPIO,
  no eSPI, no SMBus port switching. Promontory has no documented temperature sensor.
- No P2SB/sideband access, and no SPI flash reads (only the protection bits).
- The memory scan runs at boot only. Nothing exposes the modules in the inventory window yet.
