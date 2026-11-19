# Native mode setting

The UEFI GOP framebuffer is the fallback for every GPU. A vendor class can take the display engine over from it; when it cannot, the firmware path stays in charge.

| GPU | Status |
| --- | --- |
| Intel integrated, Gen9.5 / Gen11 / Xe-LP (`IntelGpu`, `IntelDisplay`, `IntelScanout`) | Native, **not run on real hardware** (host tests against a simulated register file only). |
| Intel Arc Alchemist (DG2) that is not the boot display (`IntelSecondaryGpu`, `IntelDg2Display`, `IntelSnpsPhy`) | From scratch with `-DTCOS_INTEL_DGPU_NATIVE=ON`, **not run on real hardware** (see below). |
| Intel DG1 and Battlemage as a secondary GPU | Refused by `IntelSecondaryGpu` (the read-only probe stays). |
| Intel DG1, Arc, Meteor Lake and later as the boot display | Firmware framebuffer: their display engine sits elsewhere or is laid out differently. |
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
0x1002, display class, when `amd_gpu_identify` says discrete Polaris, or discrete Vega 10, 12 or 20 (DCE 12, below). Navi and later (DCN 2+) are not covered (see "Why
not DCN" below).

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

### DCE 12: Vega 10, Vega 12, Vega 20

The discrete Vegas (`Engine::Dce12`; Vega 20 reads its pipe fuses) go through the same steps with the SOC15 register layout (segment bases from `vega10_ip_offset.h`; Vega 12
uses Vega 10's table, Vega 20's is the same for these blocks) and an ATOM firmware (format 2.1) video BIOS. Linux has no legacy `dce_v12_0.c`: these chips are driven by the
display core (DC, `dce120_*`), and the register sequences of `AMD/AmdDce12` are DC's. Register numbers and masks are generated from `dce_12_0`, `mmhub_1_0`, `nbio_6_1` and
`hdp_4_0` (`AmdDce12Registers.inc`, `scripts/gen_vcn1_regs.py`); NBIO 7.4 (Vega 20) has the same numbers for the three registers used.

| Step | DCE 12 | Source |
|---|---|---|
| POST | ASIC_Init with firmware_info 3.1's boot-up clocks (the legacy call: `amdgpu_device_asic_init` uses `amdgpu_atom_asic_init` for GC 9.0 - 9.4.0) when BIOS scratch 7 at firmware_info 3.1's `bios_scratch_reg_startaddr` lacks ASIC_INIT_COMPLETE | `amdgpu_device.c`, `amdgpu_atomfirmware_scratch_regs_init` |
| Memory | MMHUB FB location, RCC_CONFIG_MEMSIZE; VGA aperture locked out, HDP set up and flushed, AGP aperture empty, system aperture over video memory with the scratch page, L1 TLB (system access mode 3, MTYPE_UC), BIF_FB_EN, the display hub's AGP aperture off | `gmc_v9_0_hw_init`, `mmhub_v1_0_init_system_aperture_regs` / `_init_tlb_regs`, `hdp_v4_0_init_registers`, `nbio_v6_1_mc_access_enable`, `dce120_update_dchub` (local only) |
| Connectors | display_object_info_table 1.4 (one path per connector, records, GPU encoder, bridge); HPD from gpio_pin_lut 2.1 (DC_GPIO_HPD_A bits); DDC: the I2C record's gpio_pin_lut entry names the DDC line, bit-banged on DC_GPIO_DDCn; DisplayPort over ProcessAuxChannelTransaction | `bios_parser2.c` (`get_gpio_i2c_info`, `bios_parser_get_gpio_pin_info`) |
| Display block | DIG1TransmitterControl 1.6 INIT per DIG (no front end, four lanes), DIG_BE_CNTL.DIG_HPD_SELECT, DC_HPD_EN; no power gating (`dce120_enable_display_power_gating` is disabled in DC) | `dce110_link_encoder_hw_init` |
| Mode set | safe watermarks, blank, SetPixelClock 1.7, SetCRTC_UsingDTDTiming 3, the advanced request, MASTER_UPDATE_MODE / UNDERFLOW_UPDATE_LOCK, EnableCRTC 1, FMT truncation, DMIF buffers, the plane (GFX9 linear, scaler bypass, viewport, LB 30 bpp), the DIG front end on the CRTC and the back end on it, DIG1EncoderControl 1.5 stream setup (and the panel mode for DP), the transmitter on, unblank | `dce110_apply_ctx_to_hw`, `dce110_enable_stream_timing`, `dce120_timing_generator.c`, `dce_mem_input.c`, `dce_transform.c`, `command_table2.c` |
| DisplayPort | trained with the encoder table's training commands (1.5) and SETUP_VSEMPH (1.6) once per lane, video on | `DpLinkTrainer` + `amd_dce12::DpSource` |
| Shutdown | DP video off, sink to D3, transmitter off, front end let go, blank, CRTC off, plane off, DMIF freed, combo PHY PLL powered down (no CRTC, no clock, FORCE_PROG_PPLL) | `dce110_clock_source_power_down` |

Where the driver and this port part: DC programs the stream encoder (`dp/hdmi_set_stream_attribute`), the DP PHY patterns and the AUX engine by registers; here the video
BIOS's DIG1EncoderControl 1.5 (stream setup, panel mode, training commands - the actions atomfirmware.h defines for that revision) and ProcessAuxChannelTransaction do them.
The watermarks are DC's "safe" ones (`dce110_set_safe_displaymarks`), not the bandwidth calculation's (`dce_calcs.c` is not ported). HDMI and DVI run at 8 bits per colour
(the deep colour DTO resync is not ported). The video BIOS comes from the ACPI VFCT table only (the copy in video memory and the ROM read use Volcanic Islands registers).
The HDP flush writes the NBIO's own flush register, not the remapped copy the driver sets up for user mode. The GFX9 tiling fields of the plane are written as for one
pipe, bank and shader engine (they do not matter for a linear surface). Interlaced modes are refused.

Tests: `AmdDce12Test` (register numbers typed in from the headers, the memory map and its refusals with random register values, every register value of the memory
set-up and of the pipe, the command tables' parameter words, the DP source, display_object_info_table 1.4 / gpio_pin_lut 2.1 with truncation and bit flips) and the Vega
runs of `AmdSecondaryGpuTest` (a Vega 10 with HDMI: POST, GMC 9 apertures, plane, the tables in DC's order with their words, another mode, shutdown; a Vega 20 with pipe 0
fused off and DisplayPort trained; all pipes fused off; a non-ATOM-firmware image; damaged images; the refusal without the flag).

### Why not DCN (Navi and later)

DCN 2.0 and later have no command tables for the mode: every mode needs the display mode library (DML: the pipe, DET and watermark calculation that decides whether a mode fits
and how the hubs are split), a different register set for each stage (HUBP, DPP, MPC, OPP, OPTC, DCCG, HUBBUB, DIO), and from DCN 2.1 on the DMCUB microcontroller, whose
firmware runs parts of the sequence. The DCN 1.0 code here (Raven, above) only repoints a plane the firmware lit.

Not done or not verified: never run on a card (the tests use simulated registers and a test video BIOS whose tables only pass their parameters on). Not done: the DCE
clock (SetDCEClock: the display clock stays as the POST left it), the Polaris memory controller microcode update, the display golden registers, HDMI audio and
infoframes, the EDID's CTA extension timings, several CRTCs, hot plug (a monitor plugged in later needs the driver built again), Resizable BAR, the ATRM and
expansion ROM sources of the video BIOS (the pool's PCI bus is read-only here).

## Secondary Intel discrete GPU display (DG2)

`Intel/IntelSecondaryGpu` drives the display of an Arc Alchemist (DG2, display version 13: Alder Lake-P's Xe_LPD display with Synopsys PHYs on every port) that is
**not** the boot display. Nobody lit a screen on it, so every clock, well, PLL, port, transcoder, pipe and plane is programmed from scratch, after the i915 driver of
Linux v6.12 (the files were fetched from git.kernel.org and read; nothing below is from memory unless it says so). `GpuNativeDrivers.cpp` builds it for vendor 0x8086,
display class, when `IntelGpuProbe::platform_of` says DG1, DG2 or Battlemage; DG1 (display version 12: combo and Dekel PHYs, the DPLL manager) and Battlemage
(display version 14: C10/C20 PHYs, `intel_cx0_phy.c`) are refused in `initialize()` before anything is touched, so the pool keeps the probe for them.

Everything that writes to the card is behind `TCOS_INTEL_DGPU_NATIVE` (CMake option, off). Without it `initialize()` fails at once.

| Step | Programmed | i915 source | Confidence |
|---|---|---|---|
| Local memory | GU_CNTL LMEM_INIT must be set (the card's firmware trained it); size from SG_TILE_ADDR_RANGE | i915_reg.h, xe_device.c | high |
| Display core | DC_STATE_EN 0, PCH reset handshake, PW1 (PG0/PG1 fuse waits), CDCLK, DBUF S1+S2 (tracker 8), MBUS ABOX credits, SNPS calibration wait, Wa_14011508470, Wa_14011503030 | `icl_display_core_init`, `hsw_power_well_enable` | high for offsets, medium for completeness |
| Power wells | PGA (idx 5) and PG2 (idx 1) in HSW_PWR_WELL_CTL2 with their fuses; AUX wells (fixed 600 us on DG2, no ack); DDI IO wells | `xehpd_power_wells`, `xelpd_power_wells_main` | high |
| CDCLK | DE PLL ratio 34 (38.4 MHz ref), squash waveform, CDCLK_CTL (cd2x /1, no pipe, decimal); no PCODE on DG2 | `dg2_cdclk_table`, `_bxt_set_cdclk` | high |
| SNPS PHY | MPLLB tables generated from `intel_snps_phy.c` (DP RBR..HBR3, 44 HDMI clocks; each checked against `intel_mpllb_calc_port_clock`), DG2_PLL_ENABLE, force bit, lock; TX_EQ from `dg2_snps_trans` | `intel_mpllb_enable`, `intel_snps_phy_set_signal_levels` | high |
| AUX / DDC | DP_AUX_CH_CTL/DATA (the AUX channel number equals the port), native and I2C-over-AUX with DRM's retries; GMBUS at 0xC5100 with DG2's pins (phy + 1) | `intel_dp_aux.c`, `intel_gmbus.c` | high (the retry policy: medium) |
| DP | TRANS_CLK_SEL by PHY, TRANS_DDI_FUNC_CTL first without enable, drive level 0, sink D0, `DpLinkTrainer` with DP_TP_CTL (per transcoder) and DDI_BUF_CTL as the source, idle then normal; M/N | `tgl_ddi_pre_enable_dp`, `intel_ddi_prepare_link_retrain` | high; M/N medium (classic formula, no 6.12 overhead factor) |
| HDMI | MPLLB at the TMDS clock, IO well, TRANS_CLK_SEL; after the transcoder: level 9, DDI_BUF_CTL on | `intel_ddi_pre_enable_hdmi`, `intel_enable_ddi_hdmi` | high |
| Transcoder / pipe | timings (TRANS_SET_CONTEXT_LATENCY for the vblank start on version 13), PIPESRC, PIPE_MISC, frame start delay, TRANSCONF; then FUNC_CTL enable and TRANSCONF enable | `intel_set_transcoder_timings`, `hsw_crtc_enable`, `intel_enable_ddi` | high |
| Plane | linear XRGB8888, stride / 64, PLANE_COLOR_CTL gamma off, PLANE_SURF last; DDB blocks 0..1023, watermark level 0 only, PIPE_MBUS_DBOX_CTL | skl_universal_plane_regs.h, `intel_mbus_dbox_update` | offsets high; the watermark is **assumed** (20 us latency instead of PCODE's table) |
| Surface | 64 MiB of local memory at 16 MiB (CPU: BAR 2), GGTT entries at 256 MiB written through BAR 0 + 8 MiB as gen8 PTEs with GEN12_GGTT_PTE_LM, then GFX_FLSH_CNTL | `ggtt_probe_common`, `gen8_ggtt_pte_encode` | high for the format; the placement is an assumption |

The DMC firmware (`dg2_dmc_ver2_08.bin`) is not loaded: i915 uses it for the DC5/DC6 states and treats it as optional (a missing DMC means "DC states disabled"); this
driver keeps DC states off, so a mode set should not need it. Confidence medium-high: a reading of i915, not a test on a card.

DDI TC1 is not driven: i915 v6.12 maps port TC1 to PHY_F (`intel_port_to_phy`), for which `intel_phy_is_snps()` is false, while its comments call it PHY E; which SNPS
block and TRANS_CLK_SEL value it takes is not settled. Outputs are found without the VBT or hot plug: each DDI's AUX is asked for a DPCD, else its GMBUS pin for an EDID.

Tests: `tests/run_host_tests.sh IntelSecondaryGpuTest` (with the flag, and `IntelSecondaryGpuOffTest` without) against a DG2 register model: the MPLLB tables against
the port clock formula, the power well enable order, CDCLK, the PHY programming for 148.5 MHz HDMI and RBR DisplayPort, the GGTT entries of the surface, the
transcoder, pipe and plane registers for 1080p, DP training through the AUX registers (with `FakeAuxSink`), another mode, shutdown, the refusals (no monitor, local
memory not trained, a PLL that never locks, a link that never trains, a mode the PHY has no table for, too small a window, DG1 / Battlemage, built without the flag),
truncated and bit-flipped EDIDs and a sink that answers rubbish.

Not done or not verified: never run on a card. Not done for DG2: TC1, hot plug, the VBT (port types, DDC pin and level overrides), several pipes, HDMI 2.0 scrambling
(TMDS above 340 MHz), UHBR, DSC, eDP, audio and infoframes, the EDID's CTA extension timings, a computed watermark from PCODE latencies and SAGV, the DMC and DC states,
runtime power management. Battlemage needs the C10/C20 PHY code and the PICA handshake; DG1 the combo PHY init, its DPLLs and the Dekel PHY.

