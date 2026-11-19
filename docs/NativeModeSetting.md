# Native mode setting

The UEFI GOP framebuffer is the fallback for every GPU. A vendor class can take the display engine over from it; when it cannot, the firmware path stays in charge.

| GPU | Status |
| --- | --- |
| Intel integrated, Gen9.5 / Gen11 / Xe-LP (`IntelGpu`, `IntelDisplay`, `IntelScanout`) | Native, **not run on real hardware** (host tests against a simulated register file only). |
| Intel DG1, Arc, Meteor Lake and later | Firmware framebuffer: their display engine sits elsewhere or is laid out differently. |
| AMD Raven family, 15D8 / 15DD (`AmdGpu`, `AmdDcn1`, `AmdScanout`) | **Own framebuffer only, not run on real hardware.** No resolution choice of its own: the firmware sets every mode (see below). |
| Other AMD | Firmware framebuffer. A mode change needs the Display Mode Library (DML) to compute the plane's request and timing registers, which this OS does not have. |
| NVIDIA | Firmware framebuffer by default. With `-DTCOS_NVIDIA_NATIVE=ON`, Tesla to Turing take the display over (the firmware's mode stays, the head shows the OS's own framebuffer in VRAM; Fermi-Pascal can also set modes), after nouveau: see [Nvidia.md](Nvidia.md). Ampere and later need the GSP firmware, which is not run yet. |

## How the Intel path works

The firmware has already lit the screen, so the driver leaves the PLL, DDI and link alone and changes only what is in front of the transcoder:

- **Pipe source size** and the **primary plane** (surface address, stride, size, linear XRGB8888).
- The **pipe scaler**, which stretches a picture smaller than the panel over the panel's own timings. So a mode is any size from 64x64 up to the panel's, at most 8x
  smaller, and the modes offered are the ones the firmware lists that fit.
- The framebuffer is ordinary memory, sized for the panel, mapped into unused global GTT entries aligned to 256 KiB. The display engine does not see the CPU's caches,
  so `IntelGpu` flushes what each drawing call touched (`clflush`).

The first mode change moves the screen to our framebuffer; after that every mode is set through the registers. Each change is read back, and the previous state
restored if the hardware did not take it. If the engine is not found or not understood at start-up, or the first mode is refused, the firmware's `SetMode` is used as
before. `shutdown()` puts the firmware's plane back.

Not done: changing the panel's own timings (refresh rate, a monitor mode beyond what the firmware lit), more than one pipe, DSC, hot-plug, power management of the
display engine. The register offsets and bits follow the i915 driver from memory of its layout and have to be checked on hardware (a wrong pipe offset on Xe-LP, or a plane
control bit that means something else on one generation, would show as a black or garbled screen; the host tests cannot catch that).

## How the AMD (Raven, DCN 1.0) path works

It changes one thing: the primary plane's **surface address**. Everything the firmware set up for its mode (format, pitch, tiling, the request/deadline registers that
DML computed, the scaler, the timings) stays as it is, so there is no resolution choice beyond the firmware's own mode list.

- `AmdDcn1::Plane::probe()` finds the plane that shows the firmware's framebuffer: same address (the GPU's view, from the memory controller's registers), same pitch, linear,
  uncompressed ARGB8888, no rotation or mirroring, not disabled. Anything else and it refuses.
- The OS's framebuffer goes at the end of video memory (the carve-out the BIOS sets aside), short of the 16 MiB tail guard and clear of the firmware's part
  (`amd_plan_display_region()`); the video engine's plan is told to leave it alone. It must lie inside the VM system aperture, where the engine reads physical memory
  without translation.
- The firmware's picture is copied over, flushed out of the CPU's caches, and the address written high half first (the low half latches), as `hubp1_program_surface_flip_and_addr()` does.
  The in-use address is then read back; if the engine does not follow, or an underflow appears, the old address is written back.
- A mode change goes through the firmware (`SetMode`): the display engine is given back to the firmware's framebuffer first, the picture copied back, and the
  framebuffer taken over again for the new mode.

Register numbers are generated from AMD's own headers (`scripts/gen_vcn1_regs.py`, `AmdDcn1Registers.inc`); the sequence is ported from amdgpu's `dcn10_hubp.c`.
What is untested is how a real APU reacts: the host tests use a simulated register file. Not done: DML and therefore any mode of our own, Renoir/Cezanne (DCN 2.1/3.0)
and later, hot-plug, and a second display.

## Secondary AMD GPU display

`AMD/AmdSecondaryGpu` drives the display of a discrete AMD GPU that is **not** the boot display (a hybrid laptop's Radeon, which often owns the HDMI and USB-C
ports, or a second card). Nobody lit a screen on it, so there is no firmware mode to keep and the whole path is programmed from scratch. It covers the DCE 11.2
display of the Polaris family: Polaris 10/11/12 (Radeon RX 460/470/480/5x0 and their mobile parts) and Vega M. `GpuNativeDrivers.cpp` builds it for vendor
0x1002, display class, when `amd_gpu_identify` says discrete Polaris; Vega 10 (DCE 12) and Navi and later (DCN 2+) are not covered - the repository's DCN 1.0 code
(Raven, above) only repoints a plane the firmware set up, so it is no small step towards them.

Everything that writes to the card is behind `TCOS_AMD_NATIVE` (CMake option, off). Without it `initialize()` fails before touching the card and the pool keeps the
read-only probe (`AmdGpuProbe`).

| Step | Programmed from scratch | Reused |
|---|---|---|
| Video BIOS | - | `AmdVbios::find`: ACPI VFCT, the copy in video memory (POSTed card), the ROM through the SMC |
| POST | ASIC_Init when BIOS_SCRATCH_7 lacks ASIC_INIT_COMPLETE (`amdgpu_device_need_post`) | `AtomBios::asic_init` |
| Memory | system aperture over video memory, scratch page, VGA aperture locked out, BIF framebuffer access (`gmc_v8_0_mc_program`, no GART) | `AmdGmc8::program_apertures`; FB_LOCATION / CONFIG_MEMSIZE as the POST set them (`amd_read_gmc8_memory_map`) |
| Display block | power gating init (EnableDispPowerGating ATOM_INIT), every DIG PHY init (transmitter ACTION_INIT), hot plug pins on (`dce_v11_0_hw_init`) | `AmdModeSet`, `amd_dce11::Interrupts` |
| Connectors / EDID | eDP and LVDS skipped (the integrated GPU's) | `AtomConnectors`, `AtomI2c` + `AmdDdc` (HDMI/DVI), `AmdAux` (DisplayPort) |
| Mode | the EDID's preferred detailed timing if the output carries it, else the largest | `edid_detailed_timing`, `mode_valid`, `monitor_bpc` (AmdMonitor) |
| Mode set | the sequence of `dce_v11_0_crtc_prepare/mode_set/commit` and the encoder's on CRTC 0: power gating, lock, dpms off, CRTC source, FMT, SetPixelClock (combo PHY PLL or DP DTO), DTD timing, plane, watermarks (VBIOS default clocks), overscan, scaler, DIG off, CRTC on, unblank, DIG setup + panel mode, transmitter on | `AmdModeSet`, `amd_dce11::Scanout`, `amd_dce11::Watermarks` |
| DisplayPort | link configuration from the DPCD, training, video on | `DpLinkConfig`, `DpLinkTrainer` + `AmdDpSource` |
| Surface | linear ARGB8888 at 32 MiB into video memory (BAR 0), pitch 64-pixel aligned, drawn by the CPU (`FramebufferGpu`), `present()` flushes the HDP | `amd_gmc8_flush_hdp` |
| Shutdown | DP video off, sink to D3, transmitter off, blank, CRTC off, plane off, power gated, combo PHY PLL off, hot plug pins off | |

Tests: `tests/run_host_tests.sh AmdSecondaryGpuTest` (builds `AmdSecondaryGpuTest` with the flag and `AmdSecondaryGpuOffTest` without): mode choice, the POST of a card
that needs it, the aperture registers, the command tables of one HDMI mode in the driver's order with their parameters, the plane, drawing and the HDP flush, another
mode, the DisplayPort path with link training, a sink that never locks, no monitor, no mode, a non-AtomBIOS image, too little memory, shutdown, truncated and
bit-flipped video BIOS data tables and EDIDs, and the refusal without the flag.

Not done or not verified: never run on a card (the tests use simulated registers and a test video BIOS whose tables only pass their parameters on). Not done: the DCE
clock (SetDCEClock: the display clock stays as the POST left it), the Polaris memory controller microcode update, the display golden registers, HDMI audio and
infoframes, the EDID's CTA extension timings, several CRTCs, hot plug (a monitor plugged in later needs the driver built again), Resizable BAR, the ATRM and
expansion ROM sources of the video BIOS (the pool's PCI bus is read-only here).
