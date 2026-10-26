# Native mode setting

The UEFI GOP framebuffer is the fallback for every GPU. A vendor class can take the display engine over from it; when it cannot, the firmware path stays in charge.

| GPU | Status |
| --- | --- |
| Intel integrated, Gen9.5 / Gen11 / Xe-LP (`IntelGpu`, `IntelDisplay`, `IntelScanout`) | Native, **not run on real hardware** (host tests against a simulated register file only). |
| Intel DG1, Arc, Meteor Lake and later | Firmware framebuffer: their display engine sits elsewhere or is laid out differently. |
| AMD Raven family, 15D8 / 15DD (`AmdGpu`, `AmdDcn1`, `AmdScanout`) | **Own framebuffer only, not run on real hardware.** No resolution choice of its own: the firmware sets every mode (see below). |
| Other AMD | Firmware framebuffer. A mode change needs the Display Mode Library (DML) to compute the plane's request and timing registers, which this OS does not have. |
| NVIDIA | Firmware framebuffer. Turing and newer program their display through the signed GSP firmware, which is not documented. |

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
