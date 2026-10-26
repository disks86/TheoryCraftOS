# Multiple GPUs (integrated + discrete)

Code: `src/Hardware/GraphicsProcessingUnit/Multi/`, test: `tests/MultiGpuTest.cpp` (`tests/run_host_tests.sh MultiGpuTest`).

| File | Job |
| --- | --- |
| `GpuTopology` | Finds every PCI display controller and classifies it `Integrated`, `Discrete`, `Virtual` or `Unknown` from the vendor tables (Intel/AMD generation tables say `discrete`; NVIDIA is always discrete). Picks the **boot display**: the adapter whose PCI memory/IO decoding the firmware left on, else the first. |
| `GpuPowerPolicy` | `gpu_select()`: which adapter for a workload. Desktop, video and web go to the integrated GPU; games and compute go to the discrete one, except on battery below 30 % or with battery saver. `GpuPreference::LowPower` / `HighPerformance` override. A single GPU is always chosen; virtual adapters only when nothing else exists. |
| `GpuManager` | Puts every discrete adapter that is not the boot display into PCI D3hot at start-up, and wakes it (D0 + 10 ms) on `acquire()`; the last `release()` puts it back to sleep. |

`SystemGpu` builds the display driver on `gpu_boot_display()`, so on a hybrid machine the screen runs on the GPU the firmware used.

## Rendering and presenting on any GPU

Nothing here is limited to two GPUs: the machine's GPUs are a list of up to `GpuTopology::kMaxAdapters`, and everything below loops over it.

| File | Job |
| --- | --- |
| `Multi/GpuPool` | One `GpuNode` per adapter: its driver (`IGraphicsProcessingUnit`), and the virtio / VMware driver when it is one. The boot display's driver is adopted from `SystemGpu`; `GpuNativeDrivers.cpp` builds a native driver for every other adapter that has one (a table: QEMU/VirtualBox VGA, VMware SVGA, virtio-gpu). A driver keeps its GPU awake. |
| `Graphics/RenderNode` | The 3D back end of one GPU (virgl / SVGA3D if it works, else the CPU renderer). Moved out of `SystemGraphics`, which now has one for the display GPU. |
| `Graphics/Renderer2DSlot` | The 2D renderer of one GPU (device-drawn, or CPU into a back buffer shown through the GPU's driver). Also moved out of `SystemGraphics`. |
| `Graphics/GraphicsPool` | A `RenderNode` and a 2D renderer per GPU. `acquire(GpuSelectionInput)` gives a `RenderLease` on the GPU the policy picks (waking it; it sleeps again when the lease ends), `acquire_node(i)` on a named one. `present_image(image, target, x, y)` reads a finished image back and writes it through GPU `target`'s driver, so a frame rendered on GPU A is shown on GPU B, either direction. `renderer_2d(i)` draws to GPU i's own screen. |

`main.cpp` builds the pool and the graphics pool after `SystemGraphics` and logs the GPUs when there are several.

## Not done

- **No native driver for the vendor GPUs.** The Intel, AMD and NVIDIA classes drive the UEFI framebuffer, which belongs to the boot display, so a second Intel/AMD/NVIDIA GPU
  has a node with no driver: it cannot show anything, and renders only on the CPU. Real rendering on a discrete card needs a mode-setting driver and a 3D engine driver per
  vendor; each plugs into `GpuNativeDrivers.cpp` (display) and `RenderNodeAccel.cpp` (3D). What works today is every combination of QEMU VGA, VMware SVGA and virtio-gpu
  (e.g. QEMU with several `-device virtio-gpu-pci`).
- The desktop does not span monitors on several GPUs: `SystemMonitors` and the monitor compositor still drive the display GPU only. `present_image()` and `renderer_2d(i)` put
  pictures on the other GPUs' screens, but nothing yet lays windows out across them.
- A GPU is not taken through a power cycle while it has a driver (the drivers cannot restore their state), so a GPU with a driver stays awake.
- Cross-GPU copies are a read-back plus a blit through the CPU (no peer-to-peer DMA, no shared surfaces), and `GraphicsPool` is not thread safe.
- Muxed laptops (a hardware switch that moves the panel between GPUs), ACPI `_PR3`/`_OFF` power resources and PCIe link-state (ASPM) control.
- Battery state is not read by the manager; the caller fills `GpuSelectionInput` (see `Battery/AcpiBattery`).
- Tested against fake PCI buses and fake screens (`MultiGpuTest`, `GraphicsPoolTest`) and built for UEFI, but not run on real hybrid hardware or in QEMU with several GPUs.
