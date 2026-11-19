# NVIDIA GPUs (the native driver)

The native NVIDIA driver lives in `src/Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/` and follows nouveau's nvkm (Linux
`drivers/gpu/drm/nouveau/nvkm`) part by part, with envytools' documentation for the hardware and BIOS tables. Until it can drive a
display, the screen stays with the firmware framebuffer (`NvidiaGpu.hpp`), and `NvidiaGpuGenerations.*` keeps classifying cards by PCI
device ID for the media engines.

## Overview / plan

The goal is parity with nouveau. The work goes in this order, each step testable against a register model on the host before it runs on
hardware:

1. **Foundation**: `INvRegisters` (BAR0), chip identification (`NvChip`), the video BIOS (finding it, reading BIT/DCB/I2C/GPIO/connector
   tables: `NvBios`), and detecting whether the GPU was POSTed (`NvDevinit`). Done in this first pass; see below.
2. **Display**: the EVO/NVDisplay engine for Tesla to Turing (nouveau's `nvkm/engine/disp`, `dispnv50`): heads, SORs, DACs, PIORs, core
   and window channels, mode setting, DP link training. These chips can be driven without signed firmware.
3. **I2C, AUX, GPIO, thermal**: DDC/EDID over the DCB's I2C ports, DP AUX, hot plug through the GPIO table, temperature and fan control.
4. **Memory, FIFO, copy engine**: VRAM detection and the framebuffer controller (`fb`), instance memory, the MMU and its page tables,
   channels (`fifo`) and the copy engines (`ce`), so the driver can move data with the GPU.
5. **GSP and graphics firmware**: Turing and later through NVIDIA's GSP-RM firmware (required from GA102 on, the only path for Hopper,
   Ada and Blackwell), and the signed falcon firmware for graphics (`gr`) on Maxwell 2 and later. Not before the steps above work.

What a chip needs is flagged in `Chip`: `needs_gsp` (Ampere and later), `display_native` (Tesla to Turing with a display engine),
`tegra`, `integrated`, `has_display`.

## How it is wired in (and what is on by default)

`NvidiaGpu` (the boot display's driver for vendor 0x10DE) holds an `nv::NvidiaNative` (`NVIDIA/NvidiaNative.*`), which puts the Nvkm parts together.

**Always:**
- BAR0 mapped, the chip identified from `NV_PMC_BOOT_0`/`BOOT_42`. This covers every generation, so an older card or one missing from the device-ID table gets a real name: "NVIDIA GK104 (Kepler)".
- The VRAM size read (`probe_vram`), shown as the GPU's memory in the Computer Manager.
- **The video BIOS read and parsed** (`NvBiosShadow::fetch`, `parse_board`): PRAMIN, PROM, or the legacy shadow at 0xC0000.
  - The PRAMIN window (0x1700) and the ROM shadow bit (0x88050) are put back exactly as the firmware left them. Nouveau switches the shadow on unconditionally; this driver restores the old value.
  - The expansion ROM BAR is not tried, because switching it on is a write to the function's configuration space, which the GPU context does not give.
  - Without an image there are no board tables and no connectors; the rest works.
- The temperature sensor (`NvThermMonitor`, shown as the GPU temperature), now with the board's GPIO table, so the fan's tachometer line is known.
- **The connectors and their monitors' EDID.** The board's connector table is listed with each connector's EDID, read over its own DDC bus or DisplayPort AUX channel (`read_connector_edid`; DP over AUX first, then DDC for DP++).
  - EDIDs are read at start, and again only when a connector's hot-plug GPIO changes (`refresh_connectors()`), so the monitor layer's periodic refresh costs a few GPIO reads.
  - `NvidiaGpu` lists the connectors as the monitor source when at least one EDID came over the GPU's own buses. The connector whose EDID matches the firmware's comes first (the display the firmware shows), then the other connected ones, then the rest. Names are "DP-1", "HDMI-1", "DVI-1", "eDP-1"...
  - The screen is still the firmware's one display, so the mode calls go to it whatever connector they name.
  - Without a GPU-side EDID the firmware's single connector stays, as before.
- Writes in this part: the two registers above, put back, and the I2C/AUX/pad registers that an EDID read drives (as nouveau does when it probes connectors).
- A GPU that reads all ones (powered down) or a chip nobody knows is left alone.

**With `-DTCOS_NVIDIA_NATIVE=ON` (off by default):** `NvidiaGpu` takes the display over (`NvNativeDisplay`, Tesla to Turing).
- Its VRAM comes from `Bar1VramMemory`: the NvFb heap over a 64 MiB window of BAR1 right after the firmware's framebuffer, each object's CPU view a part of that window.
- This relies on the firmware leaving BAR1 a linear view of VRAM, which is how the GOP framebuffer is reached, so no page tables are needed. Checked only as far as "the GOP framebuffer lies in BAR1's first 256 MiB". **The first thing to verify on a real card.**
- A failed takeover leaves the firmware's framebuffer in use; `native()->status()` says why.
- Mode changes and shutdown give the screen back to the firmware first, as AmdGpu does.

**Not wired in yet:**
- the VBIOS's DP drive-level table (BIT 'd'), so mode setting uses placeholder drive levels;
- mode setting beyond the firmware's modes;
- the memory/MMU/FIFO/copy-engine stack;
- secondary NVIDIA GPUs (a laptop's dGPU) get `NvidiaSecondaryGpu` only with `TCOS_NVIDIA_NATIVE` (see "Secondary GPU display" below); otherwise the GpuPool's read-only probe.

`tests/run_host_tests.sh NvidiaNativeTest` covers:
- the start: chip, BAR0 untouched without a VBIOS, powered down, unknown chip, no mapper, unmappable BAR0;
- the VBIOS through the real fetch (a Kepler image where PROM reads it): the board's tables and three connectors, the PRAMIN window and ROM shadow put back, no image at all;
- the connector helpers: DCB types, labels, and the order (the firmware's EDID first).

The EDID transfers themselves are tested in `NvI2cTest` against its I2C/AUX models.
- the BAR1 heap;
- the takeover's refusals before any write: Ampere, a framebuffer outside BAR1, no room.

## Chip identification and the video BIOS

### Identification (`Nvkm/NvChip.cpp`)

`identify_chip(boot0, boot42)` decodes NV_PMC_BOOT_0 (BAR0 0x000000) as nouveau's `nvkm_device_ctor` does:

- If bits 28:24 are not zero, the chipset is bits 28:20 and the revision bits 7:0.
- Otherwise `(boot0 & 0xff00fff0) == 0x20004000` is the NV04/NV05 quirk: these chips have no architecture field, and any of bits 23:20
  set means NV05 (TNT2). Anything else (NV01/NV03, garbage, 0, all ones from a powered-down GPU) is unknown.
- From Turing on (chipset >= 0x160), NV_PMC_BOOT_42 (0x000a00) identifies the chip: architecture bits 29:24, implementation 23:20,
  revision 19:12. It is used when it reads neither 0 nor all ones.
- The family is the chipset's bits 8:4, with nouveau's boundaries: 0x10 NV10 (card type NV_10 for NV10/NV15/NV1A, NV_11 otherwise, and
  the revision dropped), 0x20, 0x30, 0x40 and 0x60 Curie, 0x50/0x80/0x90/0xa0 Tesla, 0xc0/0xd0 Fermi, 0xe0/0xf0/0x100 Kepler,
  0x110/0x120 Maxwell, 0x130 Pascal, 0x140 Volta, 0x160 Turing, 0x170 Ampere, 0x180 Hopper, 0x190 Ada, 0x1a0/0x1b0 Blackwell.
- A chipset nouveau has no table for is unknown, even inside a known family (nouveau refuses it too). The table has every chip of
  nouveau's `device/base.c` from NV04 to AD107, GH100 and the Blackwell chips. The Pascal and later names agree with
  `NvidiaGpuGenerations` (NvBiosTest checks that).

### Finding the image (`Nvkm/NvBiosShadow.cpp`)

`NvBiosShadow::fetch` tries the sources in nouveau's order (`nvbios_shadow`) and scores each image chain as `shadow_image` does: 4 per
image with a good checksum (or of a type that is not checksummed, such as EFI), 3 for an x86 image with a bad checksum from a RAM copy, 2
from a ROM, 0 for an image that is no good. ACPI read 4 KiB at a time is only tried while nothing scored 4, the expansion ROM and the
legacy shadow only while nothing was found. The best score wins (the first on a tie). Every source's result and reason is in
`NvBiosFetchReport`.

| Source | How | Not tried when |
|---|---|---|
| PRAMIN | The copy the VBIOS left in VRAM: the display's window register (0x619f04, 0x625f04 from Volta) must be enabled (bit 3) and point to VRAM (bits 1:0 = 1); its address bits 31:8 << 8 (or 0x1700 << 16 + 0xf0000) is put in NV_PBUS_BAR0_WINDOW (0x1700) and read through 0x700000. 0x1700 is put back. | Before NV50; display fused off (0x022500 bit 0 on Fermi/Kepler, 0x021c04 on Maxwell+, GA100) |
| PROM | The ROM through BAR0 0x300000, with the ROM shadow (bit 0 of PCI config 0x50, through BAR0's config mirror at 0x001800 before NV40, 0x088000 after) switched off while reading. | NV4x IGPs (chipset >= 0x4c) |
| ACPI (one call) | The GPU's `_ROM(offset, length)` for everything at once; the checksum must be good. | No `_ROM` |
| ACPI (4 KiB pages) | The same, page by page until one comes back short (firmware that returns only 4 KiB per call). | No `_ROM`, or an image scoring 4 was found |
| PCI expansion ROM | The ROM BAR (0x30) probed for its size, enabled, the window mapped and read, the register put back. | Something was found |
| Legacy shadow | 128 KiB at 0xC0000. Only for the boot display, and the PCI data structure must name the card's device ID. | Something was found; not the boot display |

Each image is checked as `nvbios_imagen` does: the signature (0x55 0xAA, and nouveau's 0x77 0xBB and "VN"), the PCI data structure
("PCIR", "RGIS" or "NPDS") through the pointer at 0x18, its length (512-byte units at +0x10), type (+0x14) and last-image flag (+0x15),
overridden by the NPDE extension after it when there is one (not for type 0x70). Stricter than nouveau: the first image must name vendor
0x10de. Images longer than the 1 MiB window, of length 0, or chains of more than 8 are refused.

`AmlNvAcpiRom` finds `_ROM` in the ACPI namespace on the device whose `_ADR` is the GPU's (device << 16 | function), the way
`AmdVbios::read_from_atrm` walks for ATRM, and holds the interpreter's lock around each call.

### Reading the tables (`Nvkm/NvBios.cpp`)

`NvBios` finds the BIT table (`ff b8 'B' 'I' 'T'`; entries from +12, their size at +9, count at +10) and the BMP (`ff 7f 'N' 'V' 00`, the
pre-NV40-era structure). `bit_entry(id)` gives other parts of the driver their tables ('I' init scripts, 'd' DP, 'P' performance, 'M'
memory ...). `parse_board` fills `BoardInfo`:

- **Version**: BIT 'i' (bytes 3, 2, 1, 0, 4) or the BMP (+13, +12, +11, +10).
- **DCB** at the 16-bit pointer at 0x36 (not on NV04): 4.x/3.x headers (version, header size, count, entry size, I2C pointer at +4,
  signature 0x4edcbdcb at +6, GPIO at +0x0a, connectors at +0x14 when the header is at least 0x16 long); 2.x (signature at +4, the
  entries run up to the I2C table); 1.5 recognised ("DEV_REC" before it) but its entries are not read, like nvkm. Versions 0x42 and up
  are refused.
- **Outputs** (2.x+): type, I2C index, heads, connector, bus, location, OR mask from the first word; entries of all zeros or all ones and
  EOL end the list, unused ones are skipped. From DCB 4.0, the second word gives DP's maximum link rate (bits 23:21) and lanes (bits
  27:24), and the SOR link (bits 5:4) and external encoder (bits 15:8) for DP, TMDS and LVDS. `hasht`/`hashm` are nouveau's keys for
  matching other per-output tables.
- **I2C**: from DCB 3.0 its own header (version, header size, count, entry size); before, 16 entries of 4 bytes. Types as `dcb_i2c_parse`:
  NV04 bit-banged (CRTC registers), NV4E, NVIO bit-banged and AUX, and from 4.1 the PMGR ports (I2C port bits 4:0, AUX bits 9:5, 0x1f =
  none). With no I2C table, the BMP's two fixed ports.
- **GPIO** (2.x via the pointer 15 bytes before the DCB, 3.x/4.x via the DCB header): line, function, the off/on levels and nouveau's
  param bit, for 16-bit (before 4.0), 32-bit (4.0) and 5-byte (4.1) entries.
- **Connectors** (3.0/4.0 tables): type, location, HPD and DP AUX line masks, and the HPD GPIO function (0x07, 0x08, 0x51, 0x52, 0x5e,
  0x5f, 0x60 for lines 0-6). Entries stay in table order, so `DcbOutput::connector` indexes them.

Everything is read through checked accessors; a table that runs past the image is cut there and `BoardInfo::truncated` is set.
NvBiosTest cuts each test image at every length and flips thousands of bits under ASan/UBSan.

### POST detection (`Nvkm/NvDevinit.cpp`)

`NvDevinit::detect` reports whether the devinit scripts have run: from Fermi on, 0x2240c bit 1 (set by devinit, cleared on suspend);
from Turing on also the devinit completion flags (0x118128 bit 0, 0x118234 = 0xff). Before Fermi nouveau reads VGA CRTC registers through
8-bit port accesses, which `INvRegisters` does not offer yet, so the state is reported as unknown. Tegra GPUs count as POSTed. **There is
no init-script interpreter yet: a GPU that was not POSTed (normally a secondary card the firmware left alone) cannot be brought up, and the
driver must not touch it beyond reading.**

### Not done / uncertain

The layouts and registers written without the nouveau sources at hand are listed in one table, `kUncertain` at the end of
`Nvkm/NvBios.cpp`. In short:

- BOOT_42's revision fields; GB102's chipset number (0x1a2) and the Hopper/Blackwell entries.
- The BAR0 PCI config mirror base used for the ROM shadow bit on each generation; nouveau's exact skip values and scores.
- The I2C header's default-port byte (byte 4, low/high nibble); the GPIO 4.1 second byte at +4; `GpioEntry::output` is not parsed.
- DCB 2.x/3.x SOR links are left 0 (nvkm reads the second word only from 4.0; real Tesla boards carry DCB 4.0).
- PROM on Turing and later (IFR header) is not handled; Open Firmware as a source is not done.
- The DP info table (BIT 'd'), the init-script tables, and the other BIT tables are found (`bit_entry`) but not parsed.
- Nothing has been checked against a real VBIOS dump yet; the tests use synthesized images.

## Display engine

Files: `Nvkm/NvDisplayHw.hpp` (the one table of every register, method number and field this code uses, each marked [S] sure, [R]
remembered from one place, [?] uncertain), `Nvkm/NvDisplay.*` (the engine, its channels and the takeover), `Nvkm/NvDisplayMode.*`
(mode setting and DP training), `tests/NvDisplayTest.cpp`. Everything is written from nouveau and NVIDIA's display class headers as
remembered (no source was at hand) and has run only against the host test's model, never on hardware.

### Generations (`display_class()`)

| Class | Chips | Takeover (A) | Mode set (B) |
|---|---|---|---|
| `Nv50` (507d family) | Tesla, GF100..GF110 (nouveau: `gt215_disp`) | done | not done |
| `Gf119` (907d family) | GF117/GF119, Kepler, Maxwell, Pascal | done | done (TMDS, DP/eDP, LVDS) |
| `Gv100` (c37d + c37e) | Volta | done (window channel) | done (window + immediate channel; TMDS, DP/eDP) |
| `Tu102` (c57d + c57e) | Turing | done (window channel) | done (as Volta; Turing's SOR link enables, VPLL) |
| `Ga102` (c67d + c67e) | GA102..GA107 (no GSP, as nouveau's `ga102_disp`) | refused (the boot display stays the firmware's) | done (as Turing; GA102's SOR rate index and clock, VPLL) |
| `Gsp` | GA100 (no display), Ada and later | refused (`NeedsGsp`) | refused |

A chip whose `Chip::display_native` is false is refused (`NotNative`) before any register is written, except GA10x: `display_native` keeps
meaning "Tesla..Turing" (the boot-display takeover), GA102's class is probed regardless and only mode setting from scratch uses it.

**Ampere and Ada without GSP.** GA102..GA107: nouveau has driven their display without GSP-RM since GA102 support came in (Linux 5.11-ish,
"display only"): `ga102_chipset` uses `ga102_disp` (= `tu102_disp_init`, gv100's channels and supervisor, `ga102_sor`), `ga100_devinit` (the
VPLL; POST is the GFW_BOOT wait), `tu102_bar`, `ga100_mmu` (GP100 tables) and `ga102_ram` (the size at 0x1183a4); the display engine, PRAMIN,
the BARs and the AUX channels are reached from the host directly. Confidence: high that this path exists and works in nouveau; medium for the
register details below (from memory). AD10x (Ada): nouveau supports it only with GSP-RM (`ad102_chipset` has r535 entries only), so there is no
reference for a GSP-less Ada display; this driver refuses it (`NeedsGsp`). Confidence: medium-high.

### How it works

- **EVO channels.** Push buffer (4 KiB, VRAM) of `count << 18 | method` headers and data words; the driver writes PUT (bytes) in the
  channel's user area (core: `0x640000` pre-Volta, `0x680000` Volta+; window *w*: `0x690000 + w * 0x1000`), the engine moves GET (+4).
  A batch that does not fit before the end starts over after a jump word (`0x20000000`). Each batch is waited for (GET == PUT), and an
  UPDATE for its notifier (507d/907d: word 0 bit 0; c37d/c37e: bits 31:30 == 2) in a 4 KiB VRAM notifier page.
- **Instance memory.** 64 KiB of VRAM: the RAMHT (512 entries, nouveau's hash: the handle folded by 9 bits, `chid << 5` mixed in, linear
  probing) and two context DMAs, the notifier page (`0xf0000000`) and all of VRAM (`0xf0000001`), for the core channel (chid 0) and on
  Volta+ also for the window channel (chid 1 + window). `0x610010` = `addr >> 8 | 9` pre-Volta; `0x610010 = 9`, `0x610014 = addr >> 16`
  on Volta+.
- **Claim** (`NvDisplayEngine::claim`, after `*_disp_init`): capability mirrors, the VBIOS handshake (`0x610024`/`0x6100ac` bit 8 and
  `0x6194e8`; `0x6254e8` on Volta+), instance memory, every display interrupt left disabled (nothing in the kernel takes the GPU's
  interrupt; completion and the supervisor are polled), the core channel (`0x610200`/`0x610490`/`0x610b00`), and on Volta+ the window
  channel the firmware's picture is in. Every wait is bounded (`DisplayTimeouts`, 2 s by default): a channel that never answers is a
  status (`ChannelTimeout`, `PushTimeout`, `NotifierTimeout`), not a hang.

### Milestone A: the OS's framebuffer in the firmware's mode (`NvNativeDisplay`)

The same idea as AMD's Raven takeover: the timings, the output resources and the clocks the firmware armed are not touched; only the
surface methods are sent. `engage_display(firmware, &memory)`:

1. reads the armed state of every head (GF119: `0x640000 + method`; Volta+: core `0x688000 + method`, window `0x690800 + w * 0x1000 +
   method`; NV50: the per-method register pairs at `0x610a38..0x610b20`, armed = +4, head stride 0x540) and keeps the heads that show a
   linear 32-bit surface of the firmware's width, height and pitch (all of them when cloned; one on Volta+);
2. allocates the framebuffer in VRAM (`INvMemory`), copies the firmware's picture into it, claims the display;
3. sends offset, size, storage (pitch, linear), format, ISO context DMA and source point for each head (Volta+: SET_SIZE, STORAGE,
   PARAMS, PLANAR_STORAGE, CONTEXT_DMA_ISO, OFFSET, POINT_IN, SIZE_IN/OUT on the window), UPDATE, waits for the notifier and reads the
   armed state back. On any failure the previous surface is sent again and the framebuffer freed, unless a head may still be scanning
   it out (then it is kept allocated for good).

`disengage_display()` copies what was drawn back into the firmware's framebuffer and points the heads at the original offset again.
The channels stay up (the VBIOS cannot take the display back); the engine object must live as long as the GPU (its instance memory
holds the context DMA the armed state names). `display_status()` says why an engage failed.

### Milestone B: mode setting on GF119..Pascal (`NvModeSetter`)

- `compute_head_raster`: nouveau's `nv50_head_atomic_check_mode` (hardware counts from one unit into the sync pulse; interlaced modes
  halve the vertical values and get the second-field blank; `blank_us` underestimate).
- `route_output`: SOR = lowest of DCB `or_mask` the display has, head = the wanted one if DCB `heads` allows it; protocol single TMDS A/B,
  dual TMDS from 165 MHz on a two-link entry, DP A/B, LVDS. External encoders (PIOR) and DACs are refused (`NoOutput`).
- `gf119_mode_methods`: SOR_SET_CONTROL, HEAD_SET_CONTROL_OUTPUT_RESOURCE (sync polarity, depth), HEAD_SET_CONTROL, the raster methods,
  pixel clock (Hz, CLK_CUSTOM, max), viewport, and the surface.
- The UPDATE raises supervisor stages 1..3 (`0x6100ac` bits 2:0, head masks `0x6101d4`, ack `0x6101d0 = 0x80000000`), polled through
  `IDisplayPoller` while the notifier is waited for: 2.1 sets the VPLL through `INvPixelClock::set_vpll(head, kHz)` (to be implemented by
  the clock code as nouveau's `nvkm_devinit_pll_set(PLL_VPLL0 + head)`), 2.2 the RG divider, the DP transfer unit / watermark / audio
  symbols (`nv50_disp_super_2_2_dp`) and the SOR clock (`gf119_sor_clock`).
- DisplayPort: `train_dp_link` tries the link rates from the fastest the sink and the DCB entry allow down, and for each the lanes from
  the most down, skipping configurations that cannot carry the mode (nouveau's `nvkm_dp_train`). Each attempt programs the SOR
  (`NvSorDp`: `0x612300` link speed, `0x61c10c` lanes/enhanced framing, `0x61c130` lane power, `0x61c110`/`0x61c12c` pattern,
  `0x61c118`/`0x61c120`/`0x61c13c` drive) and runs the shared `DpLinkTrainer` over the AUX channel (`IDpcdAux`, from `NvAux`).
  The drive levels come from a `DpDriveTable`; until the VBIOS DP table (BIT 'd') is parsed, a placeholder table is used.
- LVDS and eDP panels are powered first through `INvGpio::set_function(0x01 /* DCB_GPIO_PANEL_POWER */, true)`.

**Volta, Turing, GA10x** (`gv100_mode_methods`, `gv100_window_methods`, `NvModeSetter::set_mode_gv100`): the display is claimed with the
window channel of the head's first window (window 2h: `corec37d_wndw_owner` gives windows 2h and 2h+1 to head h).

1. Core channel (one UPDATE, through the supervisor): every window's owner and usage bounds (`corec37d_init`: format 0x1f / 0x0f on c57d+,
   rotated 0, usage 0x00127fff / 0x00117fff at 0x1004/0x1008/0x1010 + w * 0x80), SOR_SET_CONTROL (0x0300 + sor * 0x20, owner mask and the
   907d protocol codes), the head at 0x2000 + head * 0x400: PROCAMP (c37d: saturation 0x400; c57d+: 0), OUTPUT_RESOURCE (CRC 1, sync polarities
   bits 2/3, depth 7:4 = 1/4/5 for 18/24/30 bpp), CONTROL (interlace), PIXEL_CLOCK 0x200c and its maximum 0x2028 (Hz), HEAD_USAGE_BOUNDS 0x2030
   (0x124 / 0x1014), viewport in 0x204c, point-out adjust / size out / size out min 0x2058..0x2060, raster 0x2064..0x2074.
2. Supervisor (polled): 0x611c30 bits 2:0, cleared through 0x611860; head masks 0x6107ac + head * 4; ack 0x6107a8 = 0x80000000. 2.1: the
   VPLL from the assembly pixel clock (0x682000 + 0x0c); 2.2: RG divider, DP audio symbols 0x616568/0x61656c and watermark 0x616550 (+ head *
   0x800, gv100's), the transfer unit as GF119, the SOR clock: `gf119_sor_clock` on Volta/Turing, `ga102_sor_clock` on GA10x (0x00ec08 = 0,
   0x00ec04 = div2 per SOR at stride 0x10).
3. Window channel: SET_PRESENT_CONTROL 0x0308 = 1, composition 0x02ec..0x02f8 (opaque: depth 255, K1 0xff, K1 / NEG_K1), the image (size,
   pitch layout, X8R8G8B8, pitch / 64, the VRAM context DMA, offset >> 8, point and sizes) and UPDATE; not interlocked with the core (it
   follows the core's UPDATE). The armed window state is read back.
4. Window immediate channel (c37b, chid 33 + w: ctrl 0x610b00 + chid * 0x10, user 0x690000 + (chid - 1) * 0x1000, brought up on first use):
   SET_POINT_OUT 0x0208 = 0, UPDATE.
5. `disable()`: the window's present control and ISO context DMA to 0 (window UPDATE), then SOR_SET_CONTROL 0 through the supervisor.

SOR differences (`SorVariant`, `NvSorDp`): Volta+ lanes in order 0..3 (GF119's are 2,1,0,3) and a 4-bit TX_PU (`gm200_sor_dp_drive`);
Turing and GA10x after the link rate wait 40 ms and set 0x612300 bits 17:16 = 1 and 0x61c10c bits 1:0 = 1 (`tu102_sor_dp_links`' "XXX");
GA10x writes the link rate as an index (RBR 0, HBR 1, HBR2 2, HBR3 3, eDP 2.16/2.43/3.24/4.32 Gbit/s 4..7), a rate without one is refused
and the trainer goes on. RAMHT on Volta+: 8 KiB (`ramht_size = 0x2000`, 10-bit hash). Context DMAs are still used on GA102 (the handle-less
surface addresses came with Blackwell's ca7d).

### Integration (for `NvidiaGpu`)

1. After chip identification, create `nv::NvNativeDisplay` (registers = the BAR0 `MmioNvRegisters`, memory = the `INvMemory` of
   Fb/Instmem) for a chip with `display_native`, owned by the GPU object for its whole life.
2. In the place AmdGpu calls `engage()` (after the firmware mode is known): fill `NvFirmwareFramebuffer` from the GOP mode (pixels = the
   identity-mapped GOP framebuffer as a `ByteView`, width, height, pitch in bytes) and call `engage_display`; on success draw into the
   returned view (`set_framebuffer(view, ...)`); on failure log `display_status()` and keep the GOP framebuffer.
3. Before a firmware mode change (`set_mode`) and at shutdown call `disengage_display()`, then engage again after the change.
4. For milestone B, wire `NvModeSetter` with the clock code's VPLL, the GPIO code and the AUX channel of the DCB output's I2C/AUX port.

### Not done / uncertain

- All register and method numbers are from memory; the [?] entries in `NvDisplayHw.hpp` most of all: GF119 SOR caps mirror
  (`0x6301c4`), the context DMA words of NV50/GV100, the GV100 RAMHT context bits, the window channel's notifier methods
  (`0x021c`/`0x0220`) and storage layout bit, `0x610664` window idle, the NV50 surface state registers, the DP watermark/active-symbol
  registers. Check them against nouveau and the class headers before the first run on hardware.
- The takeover assumes the firmware's ISO context DMA starts at VRAM 0 (its offsets are reused with this driver's VRAM context DMA); the
  firmware's RAMHT is not read.
- Not done: NV50 mode setting, the VBIOS IED scripts of supervisor 2.x and 3.0, DACs/PIORs, HDMI (infoframes, audio, 340 MHz+ high speed:
  TMDS is DVI-style on every class), DP MST and DSC, the cursor and overlay channels, interrupts (all polled), Volta+ heads other than the
  one claimed (one window channel), the window interlocked with the core, Ada and later (GSP only in nouveau), the takeover on GA10x.
- Volta+ numbers marked [?]/[R] in `NvDisplayHw.hpp` that matter most on hardware: the head usage bounds values, the PROCAMP default, the
  OUTPUT_RESOURCE field layout, the composition methods 0x02ec..0x02f8, the RAMHT size, GA102's rate index table and SOR clock block, and
  whether 0x611c30 shows the supervisor's stages with its interrupt left disabled (the GF119 path assumes the same of 0x6100ac).

## I2C, AUX, GPIO, sensors and fans

Step 3 of the plan, after nouveau's `nvkm/subdev/gpio`, `i2c`, `therm`, `fuse` and `clk` (read paths). Each part takes `INvRegisters`, a
`Chip` and the `BoardInfo` the VBIOS parser fills, and is tested in `tests/NvI2cTest.cpp` against a register model (run it with
`tests/run_host_tests.sh NvI2cTest`, ASan/UBSan). **Nothing here has run on hardware.** Every register was written from memory of nouveau;
each file keeps them in one table at its top, with `(?)` on the least certain.

### GPIO (`Nvkm/NvGpio.cpp`)

| Implementation | Chips | Lines | Interrupts (status / enable) |
|---|---|---|---|
| Nv50 | G80-G92, GT200 | 16; nibble per line at 0x00e104/e108/e280/e284 (bit 0 out, 1 not driven, 2 sense) | 0x00e054 / 0x00e050 |
| G94 | G94-GT21x, GF100-GF110 | 32, same registers | + 0x00e074 / 0x00e070 for lines 16-31 |
| Gf119 | GF117/GF119 | 32; 0x00d610 + 4 * line (bit 12 out, 13 not driven, 14 sense), 0x00d604 bit 0 after a change | as G94 |
| Gk104 | Kepler-Turing, GA100 | as Gf119 | 0x00dc00 / 0x00dc08, 0x00dc80 / 0x00dc88 |
| Ga102 | GA102 and later | 0x021200 + 4 * line (?) | 0x021640 / 0x021648, 0x02164c / 0x021654 (?) |

`get(function)` / `set(function, on)` work in the GPIO table's logical levels (nouveau's two-bit `log[]`: bit 0 level, bit 1 drive), so
`hot_plug(ConnectorEntry::hpd)` says whether something is plugged in. `poll_interrupts()` returns the lines that rose and fell (bits 15:0 /
31:16 of each status register) and acknowledges them; `watch(function)` enables both edges. Hot plug is polled: nothing hooks the GPU's
interrupt yet. `voltage_id()` reads the VID lines (functions 0x04, 0x05, 0x06, 0x1a, 0x73); turning it into a voltage needs the VBIOS
voltage table, which is not parsed.

### I2C (`Nvkm/NvI2c.cpp`) and AUX (`Nvkm/NvAux.cpp`)

- **Buses** are bit-banged exactly as `nvkm_i2c_bit_xfer`: START / repeated START / STOP, the address byte, ACK after each byte and NACK
  after the last byte read, each clock rise waiting up to 2.2 ms for a stretching slave (`Timeout` after that, `Busy` when the clock cannot
  come up for the START). Registers: Nv50/G94 `0x00e138, e150, e168, e180, e254, e274, e764, e780, e79c, e7b8` by bus (bits 0/1 drive
  and read back SCL/SDA); GF119 and later `0x00d014 + 0x20 * bus` (bits 0/1 drive, 4/5 read back).
- **Hybrid pads** (a DP++ connector's pins, shared by the bus and an AUX channel) are switched before each use: G94-GM10x 0x00e500 (mode,
  `0xc001` I2C, `0x0002` AUX) and 0x00e50c (bit 0 power down) + 0x50 * pad; GM200 and later 0x00d970/0x00d97c.
- **AUX channels**: G94-GM10x at 0x00e4c0 + 0x50 * channel, GM200 and later at 0x00d930 + 0x50 * channel (data out +0x00, data in +0x10,
  address +0x20, control +0x24, status +0x28, GF119+ auto-DPCD +0x38). A transaction: wait idle, claim (the "unksel" handshake), check the
  sink is there (status bit 28, inverted on GM200), data and request (command << 12, length - 1 or 0x100 for address only), go bit 16,
  wait up to 2 ms, status: reply (19:16), bytes (4:0), timeout (8), errors (11:9). Above that the DRM helper's logic: native reads and
  writes in 16-byte pieces retried 32 times (DEFER, short pieces, timeouts; NACK and no sink at once), I2C over AUX with
  drm_dp_i2c_do_msg's retries and the write status update after an I2C DEFER, address-only requests opening and closing each I2C
  transaction (GF119 and later; G94 cannot send them, so it uses nouveau's own adapter with MOT on all but the last piece). A reply that
  claims more bytes than asked for is a protocol error and is not copied.
- `NvAuxChannel` is an `IDpcdAux`, so the AMD side's `Dpcd` parser (`AMD/Dpcd.*`: capabilities, link status, test requests) reads NVIDIA
  sinks unchanged; both it and `NvI2cBus` are `II2cBus`, so `i2c_edid::read` (`AMD/I2cEdid.*`, drm_do_get_edid with the segment pointer
  at 0x30) reads the EDID over either.
- `read_connector_edid(regs, chip, board, connector, &bytes)` (`Nvkm/NvI2cEdid.cpp`) finds the connector's DCB outputs and their I2C
  entry (type 5 NVIO bit-banged: `drive` + `share` pad; 6 NVIO AUX: `aux`; 0x80 PMGR: `drive` and `aux`), reads DisplayPort over AUX
  first and falls back to DDC (an HDMI monitor on a DP++ port), and the others over DDC. The bytes go to `parse_edid`.
- Thread safety: each `NvI2cBus` / `NvAuxChannel` holds a `kernel::Mutex` per transfer (they sleep, so no spin lock). A bus and the AUX
  channel sharing a pad are different objects: the connector's owner must not use both at once from different threads.

### Temperature, fans and clocks (`Nvkm/NvTherm.cpp`, `Nvkm/NvClk.cpp`)

| | G80 | G84-GT21x | GF1xx | GK1xx | GM10x | GM20x | GP1xx and later |
|---|---|---|---|---|---|---|---|
| Temperature | 0x020014 + VBIOS calibration | 0x020400 °C if fuse 0x1a8 = 1 | same | same | same | same | 0x020460 (bit 29 valid, 1/32 °C) |
| Tachometer | GPIO timed on PTIMER | counter from GT215 (0x00e720/e724/e728) | counter | counter (line via 0x00d79c) | counter | counter | GPIO timed, if the board has the line |
| Fan PWM | GPIO PWM 0x00e114/e118 (lines 4, 9, 16) | same | GF119+: unit by the line's GPIO function, or PTHERM 0x0200d8/dc | as GF119 | 0x10eb10/14 + 4 * line | same | none (PMU firmware; nouveau has none either) |
| Clocks | crystal only | crystal only | gf100 tree | gk104 tree (fractional N) | gk104 tree | crystal only | crystal only |

`NvThermMonitor` is the `IHardwareMonitor` for `SystemHardwareMonitor`: one temperature "GPU", one fan "GPU fan" when the board has a fan
sense line, one PWM when the fan line has a PWM unit (duty 0..255 from the divider/duty pair, inverted when the fan's off level is high).
Manual mode keeps the firmware's divider (the PWM frequency) and writes the duty; Automatic, and `shutdown()`, put back the divider, duty
and routing found at `initialize()`. There is no fan curve of the driver's own, so "automatic" means the VBIOS's boot setting or, where it
drives the fan, the PTHERM controller. Readings above 150 °C, flagged invalid, or from an unfused sensor are empty, not numbers.

`NvClk::read_khz(domain)` decodes the clock tree as left by the firmware (read only): crystal from strap 0x101000 (13.5/14.318/27/25 MHz),
core (GPC), ROP, hubs, PMU, video and memory from the source selects (0x137100, 0x137160, 0x1373f0/f4), dividers and PLLs (0x00e800,
0x00e820, 0x137000..0x1370e0, 0x132000, 0x132020). A PLL that is off or has M = 0 reads empty.

### Not done / uncertain

- Uncertain registers (marked `(?)` in the files): the Ga102 GPIO block and its interrupts (and whether it still needs the 0x00d604
  write); the GM200 pad registers; the auto-DPCD register (0x00e4f8 / 0x00d968); the sink-detect bit's polarity per generation; the G84
  sensor set-up writes (0x020008/0x02000c); the GF119 tachometer routing register 0x00d79c; GM107's PWM registers (nouveau reads
  0x10eb20/24 but writes 0x10eb10/14 + 4 * line; here both use the line form); the fuse access dance per family; GT200's GPIO/I2C
  implementation; Ampere and later's sensor (assumed GP100's). The clock tree is from memory of `gf100.c`/`gk104.c` only.
- Fan sense: nouveau's DCB function is 0x3d; 0x29 (named in `NvBiosTables.hpp`) is also accepted.
- Not done: pre-Tesla (NV04/NV4E CRTC-register I2C, NV40 sensors), the GPIO reset from the table (`nvkm_gpio_reset`'s unk0/unk1 routing),
  AUX hot plug/IRQ registers, a driver fan curve and temperature alarms (nouveau's `therm` polling and thresholds), the VBIOS thermal and
  fan tables (sensor calibration, PWM frequency, fan limits), voltage reading/setting (`volt`), Tesla and GM20x+ clock reading,
  reclocking. Nothing is wired into `NvidiaGpu` yet: the coordinating code constructs `NvGpio`, `NvThermMonitor` and the buses.

## Memory, MMU, channels and the copy engine

The counterpart of nouveau's `subdev/fb`, `subdev/instmem`, `subdev/mmu`, `subdev/bar`, `engine/fifo` and `engine/ce`, written from memory of
nouveau and envytools (nothing downloaded). Each `.cpp` starts with one table of the registers and layouts it uses, each marked
confident / fairly sure / unsure. The working path is Kepler..Turing (Turing without GSP, as nouveau ran it before GSP: the copy engines need
no firmware there, GR would need NVIDIA's signed firmware); Ampere and later are identified and refused with `NeedsGsp`, except for what the
GA10x display needs: `probe_vram_display` (the size at 0x1183a4, `ga102_ram_new`), the GP100 page-table format (`ga100_vmm`), Turing's TLB flush
and Turing's BAR registers (`tu102_bar`).

### Pieces

- `Nvkm/NvFb` — `probe_vram()`: VRAM size, type and reserved regions per family. Tesla 0x10020c (+0x100714 type); Fermi/Kepler FBP count
  0x022438, disabled mask 0x022554, MiB per FBPA 0x11020c + i * 0x1000; Maxwell the GM200 way (0x022458 FBPAs per FBP, 0x021d38 / 0x021c14
  disabled masks); Pascal..Turing 0x02243c FBPAs, 0x021c14, 0x90020c + i * 0x4000, Turing also 0x1183a4 (the firmware's usable MiB, the
  smaller one wins); Ampere+ only 0x1183a4, reported but refused. All ones, absurd partition counts or sizes, or a total outside 8 MiB..256 GiB
  are `Garbage`. Reserved: 256 KiB at the bottom (VGA), 1 MiB at the top (VBIOS), widened on GF119+ to the display's VGA workspace
  (0x625f04). The memory type comes from a BIOS hint, else from the family (`type_guessed`). `fb_init_mmu()` sets the big page size
  (0x100c80) and the sysmem flush page (0x100c10). `NvFb` is the VRAM heap: a fixed-size first-fit `RangeAllocator` (no heap allocation,
  coalescing frees, every allocation recorded so a double or wild release is detected and counted instead of corrupting it) under an
  `INvLock`. `NvVramMemory` is the `INvMemory` for VRAM on top (system-memory targets go to a delegate, the kernel's DMA allocator).
- `Nvkm/NvInstmem` — `PraminWindow`: BAR0 0x700000 shows 1 MiB of VRAM, 0x001700 moves it (address >> 16); every access holds the lock from
  moving the window to touching the word. `NvInstmem`: after BAR2 is up, VRAM is reached through up to 16 BAR2 slots of 64 KiB that are
  remapped on demand (a small software TLB), falling back to PRAMIN. VRAM objects therefore have no `GpuMemory::cpu` view;
  `INvMemory::read32/write32` (added to `NvMemory.hpp`, with a default over `cpu` for system memory) are the way in.
- `Nvkm/NvMmu` — the page-table formats (NV50 2-level with the PD inside every joined instance block; GF100 2-level with a big + small PT per
  PDE; GP100 "v2" 5-level with 16-byte dual PD0 entries; GV100+ adds the 64 subcontext PDBs), `NvVmm` (page directory, tables created on
  demand, a VA allocator, map/unmap/join/translate) and `NvTlb` (0x100cb8/0x100cbc[/0x100cec] on Fermi..Volta, 0xb830a0/a4/b0 on Turing,
  0x100c80 on Tesla). The encoders are public so the tests check them against hand-computed words.
- `Nvkm/NvBar` — BAR1 and BAR2 instance blocks and VMMs, bound with 0x001714 then 0x001704 (Turing 0xb80f48 / 0xb80f40), waiting on
  0x001710 / 0xb80f50 from Maxwell; `flush_writes()` is 0x070000. `init(bar1, bar2, keep_linear_bytes)` keeps the start of BAR1 mapped 1:1 so
  the firmware's GOP frame buffer (which the CPU reaches through BAR1) keeps working once BAR1 turns virtual. `INvAperture` / `MmioAperture`
  are the CPU's word access to a PCI BAR.
- `Nvkm/NvFifo` — PTOP (0x022700) decoding, PFIFO bring-up (PMC_ENABLE, PBDMA mask 0x000204, USERD block in BAR1 via 0x002254 before Volta),
  channels: 4 KiB instance with RAMFC (gk104 or gv100 layout) and the PDB, USERD (GP_GET 0x88 / GP_PUT 0x8c), a 512-entry GPFIFO and a 64 KiB
  push buffer mapped in the channel's VMM; bind 0x800000, enable/disable 0x800004 bits 10/11, runlists (gk104 8-byte entries via
  0x002270/74; Volta+ TSG + channel 16-byte entries, Turing via 0x002b00), preempt 0x002634, kick by writing GP_PUT through BAR1 (Kepler..
  Pascal) or USERD + the usermode doorbell 0x810090 (Volta, Turing). Submissions are serialized per channel by a busy flag (a second
  submitter gets `Busy`; no lock is held while waiting for the ring to drain); a full GPFIFO or a push-buffer wrap waits for GP_GET, bounded
  by `NvTimeouts`.
- `Nvkm/NvCe` — the copy classes (a0b5, b0b5, c0b5/c1b5, c3b5, c5b5; c6b5/c7b5 named and refused) and `NvCopyEngine`: SET_OBJECT, copy
  (pitch, one line per launch, split at 1 GiB) and 32-bit fill (remap with CONST_A), each ending with a one-word semaphore release, and
  `wait()` which polls the semaphore with a bound.

Bringing it up (what the coordinating code does, in this order): `probe_vram` → `NvFb::init` → `PraminWindow` + an `NvVramMemory` over it
(for page tables) → `fb_init_mmu` → `NvTlb`, `NvBar::init` → `NvInstmem::enable_bar2` + an `NvVramMemory` over it (for everything else) →
`NvFifo::init` → a channel VMM, `create_channel` on the copy engine's runlist (`find_copy_engine`) → `NvCopyEngine::bind`.

Locks: every shared structure takes an `INvLock` (`RawNvLock`, a `kernel::RawSpinLock`, in the kernel; a `std::mutex` in the tests, since
`SpinLock` masks interrupts, which user mode cannot: the IoScheduler does the same). Order: FIFO → instmem → VMM → {VRAM heap, PRAMIN, TLB}.

### Tests (`tests/NvMemoryTest.cpp`, `tests/run_host_tests.sh NvMemoryTest`, ASan/UBSan and TSan builds)

A model GPU: register file, VRAM behind PRAMIN and both BAR apertures (translated by walking the driver's page tables from the BAR instance
registers, independently of `NvVmm::translate`), the host (runlist parsing incl. TSG headers, RAMFC, USERD, GPFIFO, the push-buffer method
encoding) and a copy engine executing copies, fills and semaphore releases on VRAM and system memory. Covered: size decoding per family,
all-ones/zero registers and 15 000 random bit flips; allocator alignment, reserved regions, exhaustion, fragmentation, double release, 8
threads with an ownership map; PRAMIN window moves and fills across windows, 6 threads (removing the window lock makes it fail); PTE/PDE/PDB
words of each format against hand-computed values; TLB invalidate sequences; BAR bind order and instance contents for Kepler, Maxwell,
Pascal and Turing; BAR2 slot remapping (also from 5 threads); RAMFC contents; copy, fill and system-memory copy completing through the
GPFIFO on Kepler, Maxwell, Pascal, Volta and Turing, 1 500 submissions wrapping GPFIFO and push buffer, two channels on one runlist; a hung
engine (semaphore wait, idle wait, full GPFIFO and the preempt on destroy all time out, the channel's memory is kept); Ampere refused
without touching a register.

### Not done / uncertain

- Not validated on hardware. The registers marked unsure: the NV50 PDE bits (size field, target codes) and TLB flush engine ids; GF100
  PDE volatile bits 34/35; the GM107 VRAM sizing (done the GM200 way); 0x625f04 (VGA workspace); 0x100c40 (Turing sysmem flush high bits);
  the gv100 RAMFC words beyond nouveau's common ones; Volta's doorbell token; Turing's runlist target bits in 0x002b00.
- Tesla: page tables and PRAMIN only; its BARs are DMA objects (0x001704/08/0c) and are not done; no NV50/GF100 FIFO (Fermi's channels
  differ: no runlists before Kepler). Fermi gets VRAM, MMU and BARs but no channels.
- Ampere+: everything here is GSP-owned; only identification and the 0x1183a4 size.
- No interrupts (completion is polled), no MMU fault handling or recovery of a hung channel beyond keeping its memory, no engine reset,
  no compression tags / kinds beyond pitch, no big pages in channel VMMs by default, page tables are never freed until the VMM goes, one
  VMM per channel is the caller's choice (shared VMMs work), 32 channels at most, RawNvLock masks interrupts while PRAMIN-zeroing a large
  object (a 1 MiB NV50 page table is 256 Ki window writes).
- Nothing is wired into `NvidiaGpu` yet.

## Devinit scripts

A GPU nobody POSTed (a secondary card, or the discrete GPU of a hybrid laptop after a D3cold power cycle: the system firmware only POSTs
it at boot, if at all) has its memory controller, clocks and straps unprogrammed. nouveau then runs the video BIOS's init scripts itself
(nvkm/subdev/bios/init.c, `nvbios_post`); so does this driver, for Tesla to Maxwell GM10x.

### Files

- `Nvkm/NvInitScript.hpp/.cpp`: the interpreter (`NvInitScript`), the init table pointers (`find_init_tables`: BIT 'I' version 1, or
  BMP 5.10+ at bmp + 75: script table +0x00, macro index +0x02, macro +0x04, condition +0x06, I/O condition +0x08, I/O flag condition
  +0x0a, function table +0x0c, the "unknown" script +0x0e, xlat +0x10), the VGA byte access (`INvVgaIo`: `MmioNvVgaIo` for hardware,
  `RegistersNvVgaIo` over the 32-bit `INvRegisters`), the hooks (`INvInitHooks`) and the trace (`INvInitTrace`).
- `Nvkm/NvDevinit.hpp/.cpp`: `NvDevinit::post(const PostInputs&) -> PostReport`, next to the unchanged `detect()`.

### The interpreter

Every opcode of nouveau's table (0x32 .. 0xaa) with nouveau's semantics: the execute flag (`init_exec`: a failed CONDITION /
IO_CONDITION / IO_FLAG_CONDITION / STRAP_CONDITION / RAM_CONDITION / GENERIC_CONDITION / CONDITION_TIME / I2C_IF / I2C_LONG_IF turns
execution off; RESUME turns it on; NOT inverts it; I2C_IF, COMPUTE_MEM, RESET and CONFIGURE_PREINIT force it on for their own accesses);
SUB / SUB_DIRECT / REPEAT nest the executor; JUMP only when executing; DONE ends the script. Register addresses are mangled as
`init_nvreg` does (NV50+: bit 31 + head * 0x800, bit 30 + OR * 0x800, bit 29 + link * 0x80), PMC_ENABLE bit 0 is never cleared by
ZM_REG, GT215/216/218 skip the registers of absent memory partitions (`gt215_devinit_mmio`, 0x001540). VGA ports follow
`nvkm_rdport/wrport`: NV50+ at 0x601000 + port; before, CRTC/attribute/input status at 0x601000 + head * 0x2000, misc/sequencer/graphics
at 0x0c0000 (+ head * 0x2000 from NV40), CR44 selecting the head.

Untrusted image: every byte goes through `NvBios::rd08/16/32` and a read outside the image stops the run (`Truncated`; nouveau reads 0
and goes on). A step budget (default 1,000,000 opcodes, backwards JUMPs included) and a nesting limit (16) stop loops and recursion;
an unknown opcode stops with its value and offset; delays are capped (100 ms each, 10 s per run, `DelayBudget` past it); registers
outside BAR0 are not touched.

Not done by the interpreter, through `INvInitHooks` (defaults: nothing, counted in `NvInitResult`): PLL programming (`set_pll(type, khz)`,
nouveau's `nvkm_devinit_pll_set`: `NvPllInitHooks` in `Nvkm/NvPll`, see "PLL programming" below),
I2C (`i2c_bus(index)` returning an `II2cBus`: `NvI2cBus` or `NvAuxChannel` fit; 0x80/0x81 = the primary/secondary default port), AUX
(`aux_channel(i2c_index)` returning an `IDpcdAux`: `NvAuxChannel`), GPIO resets (`gpio_reset(function)`, 0xff = all; `NvGpio` has no
reset yet), the NV04-era memory init (`meminit()`).

Dry run (`NvInitOptions::dry_run`): the same walk, nothing written: each write goes to the trace and to a 256-entry shadow so later
reads of that register in the run see it. VGA ports, I2C and AUX are not accessed at all in a dry run (they read back the shadow, or 0).
`execute = false` is nouveau's parse-only mode.

Deliberate differences from nouveau: ZM_REG_INDIRECT writes the register (nouveau writes to the image offset, which its own trace line
shows is a slip); ZM_TMDS_GROUP with an unknown selector writes nothing (nouveau writes 0x000004/0x000000); a failed I2C_IF read is
"condition not met" (nouveau compares the low byte of -ENODEV); INIT_IO reads its mask/data bytes with 8-bit reads (nouveau's 16-bit
reads look one byte past the opcode); I2C_LONG_IF does no transfer in a dry run.

### post()

`PostInputs`: the image (`NvBios`), BAR0 (`INvRegisters`), the VGA byte access (`INvVgaIo`; `MmioNvVgaIo` over the BAR0 `MmioRegion` on
hardware), the chip, the board tables (for GPIO_NE), hooks, trace, `dry_run`, `force` (nouveau's NvForcePost), the step budget.
Order (nvkm_devinit_preinit + post): unlock the extended CRTC registers (CR3f = 0x57); decide whether a POST is needed (Fermi+:
0x2240c bit 1 clear, unless the display engine is present (0x022500 / GM107 0x021c04 bit 0) and CR00 or CR1A is non-zero; Tesla: no
display engine, or CR00 and CR1A both 0); run the script table's entries in order, then the script at 'I' + 0x0e; on Fermi+ read 0x2240c
again (`posted_after`). `PostOutcome`: Ran, DryRun, AlreadyPosted, Refused (nothing written), Failed (the script's error and offset).

GM20x, Pascal and Volta take the PMU path instead of the scripts (see "GM20x to Volta" below). Refused: pre-Tesla (VGA owner and the
nv04/nv10/nv20 memory init are not done), Turing+ (FWSEC), Tegra (already up).

### PLL programming (`Nvkm/NvPll.hpp/.cpp`, `tests/NvPllTest.cpp`)

After nouveau's bios/pll.c, clk/pllnv04.c, clk/pllgt215.c and the devinit `pll_set` functions:

- `parse_pll_limits(bios, chip, crystal_khz, type, &limits)` is `nvbios_pll_parse`: the table from BIT 'C' (version 1: 16-bit pointer at
  +8; version 2: 32-bit pointer at +0) or the BMP 5.24+ pointer at bmp + 142; versions 0x10/0x11 (u32 kHz limits, fixed M/N ranges by
  BIOS chip), 0x20/0x21 (entries starting with their register, reference clock at +31 when the entry is longer than 0x22), 0x30 (type,
  limits pointer, register), 0x40 (the same plus the reference clock in MHz at +9) and 0x50 (Volta: no register, limits in the entry);
  tables before 0x30 go through nouveau's type <-> register maps (nv04, nv40, nv50, g84; none for GT215+, as nouveau). `type` is a
  `kPll*` value or, above 0xff, a register (INIT_PLL passes registers). No table at all: nouveau's BMP defaults. A reference clock of 0
  in the table means the crystal (`crystal_khz_from_strap`). Not done: C51's VPLL reference override (VGA CR27).
- `nv04_pll_calc` (single and two-stage M/N/P search, with the pre-NV50 M limits and N2/M2 ratio rule by BIOS version) and
  `gt215_pll_calc` (N/M/P, and the fractional N of the first M that fits) are pure functions over `NvPllLimits`.
- `nv_pll_set(inputs, type, khz)` per family (`pll_family`): Nv50 (NV50, G84, G98: VPLL 0x10000611 + M1/N1 + P/M2/N2, the memory PLL
  with its bias, other PLLs P + N1/M1), Gt215 (GT215/216/218, MCP89: VPLL0/1 only), Gf100 (GF100 to Pascal; GM107 and GM200 use it:
  VPLL0..3 only, +0x0c bit 8, +0x04 P/N/M, +0x10 fN << 16), Gv100 (VPLLs at 0x00ef04/0x00ef10 + head * 0x40), Tu102 (as Gv100, then 0x00ef0c =
  0x900 and 0x00ef00 = 0x02000014), Ga100 (GA102..GA107: 0x00ef00 = 0x02080004, 0x00ef18 = N << 16 | fN, 0x00ef04 = P << 16 | M, 0x00e9c0 + head * 4
  = 1). Other types: Unimplemented (nouveau's "unimplemented"); pre-NV50, GA100, Ada and later and Tegra: NoFamily. (Turing+ never run scripts:
  the PLLs are set only for the display's VPLLs.) Every write through `INvRegisters` (and the optional trace).
- `NvPllInitHooks(bios, chip, registers, inner, crystal_khz, trace)`: an `INvInitHooks` whose `set_pll` is `nv_pll_set`; I2C, AUX, GPIO
  resets and meminit go to `inner` (nullptr: none). Pass it as `PostInputs::hooks`. The interpreter never calls hooks in a dry run.
  `last()`, `programmed()`, `failed()` say what happened.

Deliberate differences: a read outside the image fails the parse (`Truncated`); the calculators use 64-bit arithmetic, cap log2 P at
16 and refuse a zero reference clock, M, P or input range (nouveau would divide by zero or overflow; real tables give identical
results); a register outside BAR0 or unaligned is not written (`BadRegister`); GV100 takes the head from the table entry's type, not
the caller's argument (which is a register when INIT_PLL calls it).

Tests: every table version through BIT 'C' v1/v2, by type and by register, the map fallbacks (first entry, absent from the map, no map
on GT215), the BMP defaults by crystal; the calculators against hand-derived coefficients (148.5 and 160 MHz fractional and integer,
135/100/270 MHz and the 1 GHz VCO edge single-stage, 108 MHz two-stage, fixed gain), unreachable frequencies, the divide-by-zero
guards; the register sequences of all four families, the strap crystal, a hostile register; the hooks under `NvInitScript` (INIT_PLL2,
INIT_PLL, delegation, dry run); the image cut at every length and 400 images with bit flips (ASan/UBSan, writes inside BAR0).

### GM20x to Volta: devinit on the PMU (`NvDevinit::post`)

`gm200_devinit_post` as nouveau does it, for chipsets 0x120 .. 0x140 (GM20x, GP100, GP10x, GV100; Turing+ waits for the GPU's own boot instead, below):

1. Checks, before anything is written: BIT 'I' version 1 with at least 0x1c bytes, its +0x14/+0x16 (init tables) and +0x18/+0x1a (boot
   scripts) ranges inside the image, and a usable DEVINIT application (`find_pmu_app(bios, kPmuAppDevinit)`: BIT 'p' version 2's table,
   the first entry of type 0x04 with a descriptor; `nvbios_pmuRm`'s fields, every section checked to lie inside the image, rounded to
   words, without wrapping). A failure is `Refused` (or `AlreadyPosted` when 0x2240c says POSTed; nouveau fails the device instead).
2. The preinit decision as on Fermi+ (0x2240c bit 1, the CRTC fallback).
3. Falcon reset (`nvkm_falcon_reset`): 0x10a048 &= ~3, 0x10a014 = ~0, then PMC_ENABLE bit 13 off/on (gm200_pmu: GM20x, GP100) or two
   pulses of 0x10a3c0 bit 0 (gp102_pmu: GP102 .. GV100), the memory scrub (0x10a10c bits 1-2, 10 ms), 0x10a084 = BOOT_0.
4. Upload: boot code (non-secure) and code (secure) through the IMEM port (0x10a180 with auto-increment, a tag at 0x10a188 every 256
   bytes, padded to 256 bytes), data through the DMEM port (0x10a1c0/0x10a1c4); then the argument block's two DMEM targets
   (`pmu_args(args + 8, 8)` and `(args + 8, 0x10)`) receive the init tables and the boot scripts. Before writing, the application
   and both targets are checked against the falcon's IMEM/DMEM sizes from HWCFG (0x10a108; nouveau does not check).
5. Start: mailbox 0x10a040 = 0x5000, boot vector, DMACTL 0, CPUCTL 2; poll 0x10a040 bit 13 for `pmu_timeout_us` (2 s) every
   `pmu_poll_us`: a timeout is `Failed`.
6. PRE_OS (type 0x01), if present and `run_preos`: reset, upload, start, not waited for (gm200_devinit_preos; failures ignored).

Nouveau runs no host scripts after this; the per-output encoder scripts stay `encoder_scripts_pending`. `PostReport::pmu` records the
applications, the DMEM targets, the words written, started/completed/waited. Dry run: nothing written; the DMEM targets are resolved
from the image's data section (which is what the falcon's DMEM would hold), the word counts and PRE_OS presence reported. nouveau's
gp102/gv100 have no devinit of their own beyond gv100's pll_set, so there is no fuse or secure-boot check to make here.

Tests (`NvInitScriptTest`): the 'p' table (found, decoy entry without a descriptor, no table / wrong version / zero pointer, sections
outside the image, boot end before start, truncation); a falcon model checked word by word (IMEM contents and secure flags, padding,
tags, DMEM contents including the whole last word, the start after the uploads, only falcon/PMC registers written), PRE_OS, the
engine-reset chips, GP100, the timeout, a stuck scrub, an application or table target that does not fit, dry run, POSTed / forced,
the refusals, Turing; the image cut at every length and 400 bit-flipped tables (live and dry).

### Turing and later: waiting for GFW_BOOT (`NvDevinit::post`)

From TU10x on, the GPU boots itself after every reset: its ROM's FWSEC firmware (signed, run by the GPU's security processor) executes the
devinit scripts, and the host may not. nouveau's `tu102_devinit_post` therefore only waits (`tu102_devinit_wait`), and nova-core does the
same (`gfw.rs`, `wait_gfw_boot_completion`). `post()` for `card_type >= 0x160`:

1. Poll `NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK` (0x118128) until bit 0 (`read_protection_level0`: FWSEC lowered the group's
   read protection) is set; only then read `NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0` (0x118234), whose bits 7:0 are the GFW boot progress,
   0xff when done.
2. Every `gfw_poll_us` (1 ms) for at most `gfw_timeout_us` (4 s, nova-core's figure; nouveau allows about 2 s).
3. Done at the first read: `AlreadyPosted`; done while waiting: `Ran`; not in time: `Failed` (the reason says whether the protection was
   never lowered or the progress stuck, `PostReport::gfw` has the last progress, the polls and the time). 0x118128 reading all ones (no
   device) is `Failed` at once. A dry run reads once (`AlreadyPosted` or `DryRun`).

Nothing is written, so `NvidiaNative::run_post` does the real wait even in the default build: a Turing/Ampere/Ada dGPU is waited for after
a D3cold power cut before the probe reads it again. nouveau then starts the VBIOS's PRE_OS application on the PMU (`gm200_devinit_preos`);
that is not done (on Turing+ the PMU belongs to the firmware; GSP is out of scope).

Confidence in the registers: high. 0x118128 bit 0 and 0x118234 bits 7:0 == 0xff are what nouveau's tu102.c polls and what nova-core's
`regs.rs` names; the order (protection bit first, then the progress) is nova-core's. Not validated on a real card.

### Tests (`tests/NvInitScriptTest.cpp`, `tests/run_host_tests.sh NvInitScriptTest`)

A Kepler image from `NvBiosImages.hpp` with an init table, condition / I/O condition / macro / xlat tables, a BIT 'M' entry and
hand-written scripts, a register model, a VGA index/data port model and fake I2C/AUX/PLL/GPIO hooks. Covered: the register opcodes
(ZM_REG, NV_REG, ZM_REG_SEQUENCE, ZM_REG_GROUP, ZM_REG16, ANDN/OR_REG, COPY_ZM_REG, ZM_MASK_ADD, COPY_NV_REG, XLAT, MACRO,
ZM_REG_INDIRECT, the reserved ones), conditions with NOT/RESUME (and what does not run while off: SUB, JUMP, delays), CONDITION_TIME
polling, VGA opcodes at the NV50 mirror and the pre-NV50 PRMVIO/head mapping with CR44, the 32-bit VGA adapter, SUB/SUB_DIRECT/REPEAT/
JUMP through post, delays and their caps, PLL/I2C/I2C_IF/AUX/GPIO/GPIO_NE/COMPUTE_MEM/RAM_RESTRICT_* through the hooks and the RAM strap,
dry run giving the same write sequence as the live run, parse mode; endless JUMP, recursive SUB / SUB_DIRECT, nested REPEATs, unknown
opcode, delay budget; the sample image cut at every length and 400 images with random bit flips (ASan/UBSan); post() for a not-POSTed
Kepler, a POSTed one (and forced), the CRTC fallback, Tesla with and without VGA access, dry run, the refused generations, a failing
script, an image without an init table; the Turing+ GFW_BOOT wait (`test_gfw_boot`).

### Not done / unverified

- Never run on hardware. The VGA access through `RegistersNvVgaIo` (32-bit read-modify-write) is right for a model only; on hardware
  `MmioNvVgaIo` (byte MMIO) is the one to use.
- PLLs are programmed only when the caller passes `NvPllInitHooks` (otherwise the run counts `plls_unsupported`); see the PLL section's
  limits (VPLLs only from GT215 on, as nouveau). No GPIO reset, no NV04-era memory init, INIT_CONFIGURE_MEM/CLK on BIOS major <= 2 stop
  with `Unsupported`.
- nv50_devinit_init's per-output encoder scripts (`PostReport::encoder_scripts_pending`) are left to the display code.
- Turing+ GFW_BOOT wait: tested against a register model (done at once, after some polls, never, read-protected for good, dry run, a dead
  GPU, uneven poll intervals, TU104/GA102/AD104); PRE_OS is not started afterwards.
- GM20x/Pascal/Volta PMU devinit: tested against a falcon model only (the register layout follows nouveau's gm200.c/falcon code; no
  real VBIOS DEVINIT application has been run). `NvPllInitHooks` is not wired into `NvidiaNative`/`NvidiaGpu` yet (the coordinator's
  part); `NvidiaNative::run_post` reaches the PMU path through `post()` (a dry run in the default build).

## Secondary GPU display

`NVIDIA/NvidiaSecondaryGpu.*` is the display driver of an NVIDIA GPU that is not the boot display (a hybrid laptop's discrete GPU, which often
drives the HDMI and USB-C ports, or a second card). There is no firmware framebuffer and no mode the firmware left: everything is set up from
scratch. The GpuPool builds it from the row in `Multi/GpuNativeDrivers.cpp` (vendor 0x10DE, PCI class 0x03) for any adapter that has no driver
yet; the pool holds a power lease for it, so the GPU stays awake while it exists.

**Off by default.** Everything that writes to the card is inside `#ifdef TCOS_NVIDIA_NATIVE`, as `NvidiaGpu::engage` is. Without the flag
`initialize()` fails before mapping or reading anything, the pool releases the GPU and `bind_probes` gives it the read-only probe as before.

### What it does (`initialize()` with the flag)

1. `NvidiaNative` starts as a secondary GPU (`configure_secondary(nullptr)`: the VBIOS from the card, the init scripts when it was not
   POSTed), reads the connectors and their EDIDs.
2. Refuses (and the probe takes over) unless the display class is GF119 (GF119/GF117, Kepler, Maxwell, Pascal), GV100, TU102 or GA102
   (GA102..GA107; `nv::sets_modes`, the reason in `nv::refusal`): Tesla has no virtual BAR here and its mode setting is not written; Ada and
   later need the GSP.
3. Needs the VRAM size (`probe_vram_display`: `probe_vram`, or on GA10x the firmware's usable size) and a video BIOS (the VPLL's limits come
   from it).
4. Maps the first `Bar1LinearVram::linear_bytes_for(vram)` bytes of PCI BAR1 (128 MiB, less with little VRAM) for the CPU.
5. `NvSecondaryDisplay::start()` (pure: everything through `Inputs`, so the host test drives it against the display model):
   - the display engine's `probe()` (reads only);
   - the output: the first connected connector with an EDID that is not an internal panel (LVDS/eDP: see "The built-in panel"); of its
     DCB outputs the DisplayPort one when the EDID came over AUX, the TMDS one when it came over DDC (a DP++ port lists both);
   - the modes: every detailed timing of the EDID's base block and CTA extensions (`decode_detailed_timing`, drm_mode_detailed's rules),
     one per size, filtered by what the output carries at 24 bpp (`max_output_clock_khz`: single-link TMDS 165 MHz, dual-link 330 MHz,
     DP the DCB's link rate x lanes, HBR2 x4 when it says nothing) and by the raster/pitch limits; the preferred one is what `parse_edid`
     (`Monitor/Edid.cpp`) calls preferred, else the first base-block timing. Start mode: the preferred one if it fits, else the most pixels
     (`choose_mode`);
   - nothing has been written up to here: no monitor, no fitting mode, too little VRAM or BAR1, the wrong display class all fail cleanly;
   - BAR1 (below), a scanout surface (pitch = width x 4 rounded to 256, 64 KiB aligned, X8R8G8B8 pitch-linear), and the mode set through
     `NvModeSetter` (Nvkm/NvDisplayMode: claim, SOR control, head output resource/control, raster, pixel clock, viewport, surface, UPDATE with
     the supervisor polled; the VPLL in supervisor 2.1 through `NvVpllClock` over `Nvkm/NvPll`'s `nv_pll_set(kPllVpll0 + head)`; DP link
     training with nouveau's fallback from the fastest configuration). When DP training fails at every configuration for a mode, the next
     slower mode is tried.
6. `FramebufferGpu` draws into the surface through the CPU's BAR1 view; `present()` flushes the BAR write path (0x070000).

`set_mode()` allocates a surface for the new mode, sets it and releases the old one; on a failure the old mode is set again. `IMonitorSource`
lists every connector of the board (the driven one first, then connected, then the rest; `describe_connector`), the EDIDs, and the modes of
the driven connector (`device_modes`, `set_display_mode`).

`shutdown()` (also the destructor): `NvModeSetter::disable(head, sor)` (SOR control 0, the head's ISO context DMA 0, an UPDATE through the
supervisor whose stages are only acknowledged), the display engine's channels stopped and its push buffer, instance memory and notifier
released, the surface released, and `NvBar::fini()` (bit 31 of 0x001704 and 0x001714 cleared: BAR1 back to its reset state). The BARs' own
page tables stay in the heap until the next start or the destructor.

### The built-in panel (muxed laptops, `IPanelOutput`)

On a laptop with a display mux the discrete GPU's DCB lists the panel's output (eDP: a DP output on a connector of type 0x47; LVDS: an LVDS
output on 0x40/0x41). `NvidiaSecondaryGpu` implements `IPanelOutput` (`Multi/GpuMuxPanel.hpp`) for `Multi/GpuMuxSwitch`:

- **Standby**: when `initialize()` finds no external monitor but the DCB has a panel output (`find_panel_output`), it succeeds with nothing lit
  ("panel standby"), so the pool keeps the driver for the switch (and, through its lease, the GPU awake: a cost only with the flag).
  `GpuNativeDrivers.cpp` stores `panel_output()` in `GpuNode::panel`.
- `light_panel()`: refused (`Busy`) while an external monitor is driven (one head at a time); otherwise an `NvSecondaryDisplay` with
  `Inputs::panel` set (kept once made, lit or not, so its power sequencer remembers when the panel went off): the panel output and its SOR (reads only), BAR1, then `Nvkm/NvPanelPower`: the power line, the waits, the EDID read
  again over the panel's AUX/DDC (`NvidiaNative::reread_connector`; an eDP panel answers only with power; the connector's EDID from start is
  the fallback), the modes (LVDS up to 400 MHz as `nouveau_conn_mode_valid`), the mode set with `Request::edp` and `panel_powered`, and the
  backlight. `NvSecondaryDisplay::panel_result()` says how a failure ended (`NoPanel`, `PowerFailed`, `NoEdid`, `NoMode`, `LinkTraining`,
  `ModeSet`); everything powered is switched off again.
- The driver's display lock (a recursive mutex) is held while the display is used or replaced: the switch lights the panel from its own thread
  while the desktop presents. A lit panel holds it for the whole power sequence.
- `release_panel()`: the display's `shutdown()`: backlight off, T9, the head blanked and the SOR let go, the channels stopped, BAR1 off, T10, the
  power line off.
- **Power** (`NvPanelPower`): DCB GPIO function 0x01 (`DCB_GPIO_PANEL_POWER`) switched on only when it reads off and off again only if it was
  switched on here (nouveau's `nvkm_dp_enable` / `aux_pwr_pu`); a board without the function gets the waits alone. The waits are the
  specifications' upper limits, as Linux's `intel_pps.c` falls back to them: eDP T1+T3 210 ms, T8 50, T9 50, T10 500, T11+T12 510; LVDS (SPWG)
  60 / 200 / 200 / 50 / 510. The power cycle: with `Inputs::now_us` only the rest of T11+T12 since the last power-off, without a clock all of
  it. nouveau waits 300 ms after an eDP panel's power; the panel's own delays in the VBIOS are not read.
- **Backlight**: the SOR's PWM as `nouveau_backlight.c`'s `nva3_set_intensity` (0x61c080 divider, 0x61c084 control + SOR x 0x800, bits 31 and
  30 with the level): the level the panel had, or full; 0 on release. A divider of 0 means the backlight is elsewhere (DPCD, the EC): nothing is
  written. `[S]` confidence.
- **eDP link training** (`train_dp_link` in `Nvkm/NvDisplayMode`): the eDP DPCD (revision 0x700); an eDP 1.4 panel's rate table (0x010) is
  trained on instead of the bandwidth codes, fastest first (nouveau's `dp->rate[]`), each set by DP_LINK_RATE_SET with DP_LINK_BW_SET 0 and
  on the SOR as rate / 27 MHz (rates that are not a multiple, or above the DCB limit, skipped), the lanes down from the most for each.

Not done: the VBIOS's LVDS/eDP scripts (the LVDS link configuration and IED scripts: an LVDS panel is set up by the mode methods alone),
the panel's VBIOS power delays, DPCD (eDP AUX) backlight control, a panel without an EDID (the VBIOS LVDS mode table), the panel together with
an external monitor. `Dpcd::parse_capabilities` (AMD code) multiplies the rate table's entries by 200 into "kHz" (a 1.62 Gbit/s entry,
8100, becomes 1 620 000, not the 162 000 the rest of the code uses); `train_dp_link` divides such values by 10, so real panels work either
way, and the tests encode the table in the parser's units.

### How BAR1 and VRAM are mapped (and why)

The **nouveau way, with page tables** (`Bar1LinearVram`), not the legacy linear BAR1:

- An `NvFb` heap over `[reserved_head, linear)` of VRAM (`probe_vram`'s VGA reserve below; the VBIOS/VGA-workspace tail above is outside
  `linear`). Every object this driver makes comes from it, so all of them lie in the linear part.
- The page tables and the BAR instance blocks come from the same heap through `NvVramMemory` over the **PRAMIN window** (BAR0 0x700000,
  0x001700), which needs no BAR set up.
- `NvBar::init(256 MiB, 32 MiB, linear)`: BAR2 (instance 0x001714; Turing and GA10x 0xb80f48) and BAR1 (0x001704; 0xb80f40) instance
  blocks, their VMMs (GF100 format on Fermi/Kepler/Maxwell, GP100 on Pascal, Volta, Turing and GA10x, the last three with the subcontext PDBs),
  BAR1's VA `[0, linear)` mapped 1:1 onto VRAM `[0, linear)` with 4 KiB pages; then both VMMs' TLBs invalidated (hub only; 0x100cbc up to Volta,
  0xb830b0 on Turing and GA10x). So for the CPU every object is at BAR1 + its VRAM offset, and the display engine gets the plain VRAM offset.
- Why not the legacy mode: the boot display relies on "BAR1 is a linear view of VRAM while 0x001704 is not valid", which the GOP framebuffer
  proves for that GPU. Nothing proves it for a secondary GPU: its VBIOS, a previous OS or a resume may have left the virtual BAR on with
  page tables of its own. Building the tables makes BAR1 show exactly what the driver says, whatever state it was in; `translate()` on the
  BAR1 VMM checks it in the host test.
- Assumed, not checked: PCI BAR1 is at least `linear` (128 MiB) long (every card with a native display has 256 MiB or more; the BAR size is not
  probed, because sizing writes the BAR in configuration space) and BAR2 at least 32 MiB of VA (nothing is mapped in BAR2; the CPU never
  touches it).
- No write combining: the BAR1 mapping is uncached MMIO (as `Bar1VramMemory`), so drawing is slow; zeroing a surface writes bytes.

### Pool wiring

- `GpuContext` already had `IRegisterMapper* registers`. `GpuPool::bind(st, pci, boot, IRegisterMapper* registers = nullptr)` and
  `bind_native_drivers(st, pci, IRegisterMapper* registers = nullptr)` pass it into every driver's context; `main.cpp` passes
  `paging_ok ? &register_mapper : nullptr` (as for `bind_probes`). The other drivers in the table ignore it.
- One row appended to `kDrivers`: `NvidiaSecondaryGpu::supports` (vendor 0x10DE, class 0x03), built with `build_driver<NvidiaSecondaryGpu>`.

### Tests (`tests/run_host_tests.sh NvidiaSecondaryGpuTest`, ASan/UBSan)

Built twice: with `-DTCOS_NVIDIA_NATIVE` and without (`NvidiaSecondaryGpuOffTest`). The display model of NvDisplayTest moved to
`tests/NvDisplayModel.hpp`, NvidiaNativeTest's fake card and allocator to `tests/NvTestCard.hpp`; the test adds the PRAMIN window and the MMU's
invalidate status on top of the model, so the page tables land in the model's VRAM and BAR1 is that same VRAM.

- EDID modes: the detailed timing decoder (progressive, interlaced, stereo, monitor descriptors, no sync width, truncated), base and CTA
  timings, the duplicate size dropped, the preferred mark, truncation at every 7th byte and a flip of every 3rd bit; `max_output_clock_khz`;
  `choose_mode` (preferred, too fast for single-link TMDS, nothing fits).
- HDMI from scratch: 0x001714 then 0x001704 with the instance addresses, the TLB invalidated after them, BAR1 translating the surface and the
  ends of the linear part 1:1 and nothing past it; SOR 1 owned by head 0 as TMDS A, the 1080p raster, sync, blank, pixel clock, viewport,
  surface size/storage/format, the model scanning out the surface, one VPLL call at 148.5 MHz, three supervisor acks, the TMDS link speed;
  pixels written through the CPU view found in VRAM; `flush()` writes 0x070000.
- Another mode (720p) and back, the old surface released, a bad index; a mode that never latches: the old one set again and kept.
- DisplayPort: the DP output of a DP++ port trained at HBR2 x4 on a model sink, 2160p30 offered over DP; the same monitor over DDC on that
  port goes to the TMDS output and 2160p30 is not offered.
- Refusals with no register written: no monitor, only the internal panel connected, connected without an EDID, every mode too fast, a Volta
  display, too little VRAM, a BAR1 window too small; a DP sink that never trains (every mode tried, the display taken down again).
- Shutdown: SOR and head ISO armed 0 through the supervisor, the core channel stopped, both BARs off, the heap back to the BAR tables only,
  no bad release; a second shutdown writes nothing; a start again works; nothing leaks after destruction.
- `NvVpllClock`: heads past the fourth and a zero clock refused; no write without a PLL limits table.
- `NvidiaSecondaryGpu` on the fake card: with the flag, a Kepler with no monitor fails with "no monitor" and takes everything down, an Ampere
  is refused, no register mapper is refused; without the flag, refused with BAR0 untouched and nothing mapped. Its `IPanelOutput` on a board
  without a panel output (`NoPanel`), and without the flag (`NotSupported`, nothing touched).
- The panel (eDP-1 of the Kepler board as the panel; a power line, an eDP 1.4 sink that answers only with power, an EDID hook): the order
  power line, T1+T3, first AUX access, mode set, T8, PWM; SOR 3 as DP A; the rate table's 5.4 Gbit/s entry set by index with BW_SET 0 and
  the SOR at 0x14; the PWM at the panel's old level; shutdown in the order PWM 0, T9, the blanking UPDATE, T10, power line off; restarted at
  once the whole power cycle first, with a clock only its rest, long after none; a line already on (left on), one that cannot be read back, no
  line, a stuck line (`PowerFailed`, nothing lit), no PWM; no EDID (`NoEdid`, unpowered again), the EDID from start used, every rate failing
  (`LinkTraining`), the fastest failing (3.24 Gbit/s, SOR 0x0c), no panel output (`NoPanel`, nothing written); LVDS (the SPWG waits, LVDS
  protocol on the SOR, no AUX).
- The display model resets the core channel's GET with PUT when PUT is written while the channel is being set up (before, a second
  `start()` on one model counted push-buffer errors); on Volta+ the same for every channel whose control word is not enabled.
- Turing (TU104) and GA10x (GA104) from scratch, HDMI: BAR2 then BAR1 at 0xb80f48/0xb80f40 (nothing at 0x001704/0x001714), the TLB through
  0xb830b0 after them, GP100 tables with subcontexts translating 1:1; the 8 KiB RAMHT, the core channel and window 0's window (slot 1) and
  immediate (slot 33) channels, Turing's lock pins; SOR 1 owned by head 0 at 0x0320, the windows' owners and corec57d's usage bounds, the
  output resource, raster, pixel clock and maximum, head usage bounds and viewport at the c37d offsets; window 0's image armed and scanned out,
  placed by its immediate channel; one VPLL call, three supervisor acks at 0x6107a8; the SOR clock (gf119's on Turing, 0x00ec00 on GA104);
  drawing; 720p and the old surface released; shutdown (window image off, SOR let go through the supervisor, all three channels stopped,
  both BARs off, the heap back to the tables) and up again; nothing leaks.
- Turing and GA10x DisplayPort on the DP++ port: HBR2 x4, the SOR at code 0x14 (Turing) or index 2 (GA102), the link enables and the
  40 ms wait, lanes 0..3, gv100's watermark register.
- The eDP panel on a Turing laptop (the Kepler panel test's sequence: power, T1+T3, AUX, the supervisor at 0x6107a8, T8, PWM; the rate table's
  5.4 Gbit/s entry, SOR 3 at 0x0360; shutdown in order) and on GA10x (the entry as rate index 2).
- Refusals: Ada (`NeedsGsp`, nothing written), Tesla (`NotImplemented`); `NvidiaSecondaryGpu` on the fake card: Ada refused with the GSP reason,
  TU104 and GA104 past the class and the VRAM size to "no monitor"; without the flag, TU104 and GA104 refused with BAR0 untouched.
- NvDisplayTest: the class map (GA102..GA107 `Ga102`, GA100 and Ada `Gsp`), the GA10x takeover refused, a mode set on Volta, Turing and GA102
  at the engine level (every c37d method offset, the window after the core, the SOR clock per class, disable), the SOR variants (lane bytes,
  TX_PU width, link enables, GA102's rate index; a rate GA102 has no index for refused).

### Not done / unverified

- **Nothing of it has run on a real card.** The register and method numbers are NvDisplayHw.hpp's (remembered from nouveau, marked by
  confidence there); the display model is typed from the same memory. The Volta/Turing/GA10x path most of all: see "Display engine / Not
  done" for the numbers to check first.
- `NvidiaSecondaryGpu::initialize()` end to end with a monitor is not tested (the fake card's buses answer nothing); its parts are.
- VBIOS IED scripts (supervisor 2.x / 3.0) and the DP drive-level table: not run / placeholders; some boards need them for TMDS/DP.
- HDMI 2.0 (scrambling above 340 MHz), infoframes, audio; DP MST; DSC; deep colour; several heads at once; the internal panel's gaps above.
- Hot plug: a monitor plugged in after the driver started is listed (`refresh_connectors`) but not lit; one unplugged keeps its head running.
- The virtual BARs are not restored after a GPU power cycle (the pool never power-cycles a GPU with a display driver).
- An `NvBar::init` that fails half-way can leave BAR2 pointing at tables that are then freed.
