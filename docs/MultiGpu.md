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

## Laptops: runtime power of the discrete GPU

What Linux does with runtime PM and vga_switcheroo, in `GpuManager` (test: `tests/run_host_tests.sh GpuRuntimePowerTest`, ASan and TSan):

| Piece | What it does |
| --- | --- |
| Companion functions | The other functions of a discrete GPU's device (HD audio, Turing's USB-C and UCSI) are found at start-up, go to D3hot before the GPU and wake after it. A function a driver uses (`mark_function_busy`) stays awake and keeps the GPU out of D3cold; `main.cpp` marks the HD-audio function busy because the HD-audio driver binds to it and cannot follow a power cut. |
| Autosuspend | `set_autosuspend_ms` (the kernel uses 5 s, as Linux): the last `release()` leaves the GPU on, `service(now_ms)` (called once a second from the main loop) puts it to sleep once it has been idle that long. |
| D3cold | With an `IGpuPlatformPower` attached and `allow_cold` set, `service()` takes a D3hot GPU on to D3cold. The type 0 header of the GPU and its functions is saved before and written back after the power returns (BARs, ROM BAR, interrupt line, command last). |
| Power source | `set_power_status` (from the battery read every 10 s) fills in what a `select()` caller leaves Unknown: on battery below 30 % or with power saver, heavy work goes to the integrated GPU. |
| Probes | `GpuPool::bind_probes` gives a driverless GPU an `IGpuProbe` (`Multi/GpuProbe.hpp`, `GpuProbes.cpp`): for NVIDIA the native driver's read-only start (chip, VRAM, VBIOS, sensors, the EDID of monitors on the GPU's own ports). A probe sets itself up again after a wake, so a GPU with only a probe may go cold. `probe_temperature` never wakes a GPU. |
| POST | A secondary NVIDIA GPU that the firmware did not POST (and every one after a power cut) needs its VBIOS init scripts (`Nvkm/NvInitScript`, `NvDevinit::post`, see [Nvidia.md](Nvidia.md)). They write to the card, so they run only with `TCOS_NVIDIA_NATIVE`; otherwise they are walked dry and the boot log says so. Tesla to Maxwell 1 only: GM20x-Volta need the PMU DEVINIT upload, Turing+ FWSEC. |
| Render choice | `GraphicsPool::acquire` no longer wakes a discrete GPU with no driver and no 3D engine (the work would run on the CPU anyway): the display GPU gets it. |

The platform handlers (attached in `main.cpp` once the ACPI tables are loaded, one per sleeping discrete GPU):

- [MultiGpuAcpi.md](MultiGpuAcpi.md): `Multi/Acpi/`: finding the GPU's ACPI device, `_PR3`/`_PR0` power resources with `_PS3`/`_PS0`, NVIDIA Optimus / JT / the old hybrid `_DSM`, AMD ATPX (power and mux). Test `GpuAcpiPowerTest`.
- [MultiGpuPlatform.md](MultiGpuPlatform.md): Apple gmux (`Multi/AppleGmux`: mux, discrete power, backlight) and the PCI Express link (`Multi/PcieLink`: upstream bridge, device functions, link disable/retrain, ASPM negotiation). Tests `AppleGmuxTest`, `PcieLinkTest`.

## Not done

- **No native driver for the vendor GPUs.** The Intel, AMD and NVIDIA classes drive the UEFI framebuffer, which belongs to the boot display, so a second Intel/AMD/NVIDIA GPU
  has no display driver: it cannot show anything, and renders only on the CPU. NVIDIA GPUs get a probe (identification, sensors, connectors); AMD and Intel discrete GPUs
  get none. Each display driver plugs into `GpuNativeDrivers.cpp` (display) and `RenderNodeAccel.cpp` (3D). What works today is every combination of QEMU VGA, VMware
  SVGA and virtio-gpu.
- The desktop does not span monitors on several GPUs: `SystemMonitors` and the monitor compositor still drive the display GPU only.
- A GPU with a display driver is never power-cycled (the drivers cannot restore their state), so it stays awake.
- The mux is found (gmux, ATPX, NVIDIA hybrid `_DSM`) but nothing switches the panel: that needs a display driver on the discrete GPU.
- Not used yet: ASPM (`pcie_aspm_configure`) and link disable on the root port around D3cold; the root port's `_DSM` function 9 delay; reference counting of power
  resources shared with other devices; the HD-audio driver's own runtime PM (a GPU whose audio function is enabled stays in D3hot); NVIDIA PLL programming in the init
  scripts.
- Cross-GPU copies are a read-back plus a blit through the CPU, and `GraphicsPool` is not thread safe.
- Tested against fake PCI buses, fake AML and fake ports, and built for UEFI, but not run on real hybrid hardware or in QEMU with several GPUs.
