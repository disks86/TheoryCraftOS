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
| `SystemChipset` | Kernel glue: picks the driver (AMD first on AMD CPUs, otherwise Intel first), maps the FCH PM MMIO block, owns the `SmbusHost` in a `Synchronized` |

`main.cpp` probes after the `Platform` line and logs `Chipset` (identity, functions, SMBus, flash, ME) and one `Memory` line per module.

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

In QEMU (q35, KVM) the boot log shows `Chipset: ICH9 ...`. OVMF leaves the ICH9 SMBus host disabled, so no SPD is read there.

## Not done

- Not run on real hardware. The ID tables come from Linux and the PCI ID repository. The Promontory 300/400 SATA and bridge IDs, and B840 being
  Promontory 19, are the least certain entries.
- Nova Lake, Diamond Rapids and Lunar Lake hubs have no published SMBus ID yet. Lunar Lake is named from the CPU only, and the other two are not
  in the catalogue.
- No GPIO community tables per PCH (`IntelPchGpio` still only knows Sunrise Point-LP), no P2SB/sideband access, no SPI flash
  reads (only the protection bits), no PCH thermal sensor (the 00:12.0 MMIO register), no TCO watchdog.
- AMD: no SMN or Data Fabric register access beyond counting nodes (CPU temperature still has its own SMN read), no FCH GPIO, no eSPI, no
  SMBus port switching.
- The memory scan runs at boot only. Nothing exposes the modules in the inventory window yet.
