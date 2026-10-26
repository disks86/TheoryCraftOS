# Native mode setting

The UEFI GOP framebuffer is the fallback for every GPU. A vendor class can take the display engine over from it; when it cannot, the firmware path stays in charge.

| GPU | Status |
| --- | --- |
| Intel integrated, Gen9.5 / Gen11 / Xe-LP (`IntelGpu`, `IntelDisplay`, `IntelScanout`) | Native, **not run on real hardware** (host tests against a simulated register file only). |
| Intel DG1, Arc, Meteor Lake and later | Firmware framebuffer: their display engine sits elsewhere or is laid out differently. |
| AMD | Firmware framebuffer. The display engine (DCE/DCN) needs the PSP/SMU brought up with firmware blobs the OS does not load. |
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
