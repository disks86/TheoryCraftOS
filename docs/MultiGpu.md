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
| Companion functions | The other functions of a discrete GPU's device (HD audio, Turing's USB-C and UCSI) are found at start-up, go to D3hot before the GPU and wake after it. A function a driver uses (`mark_function_busy`) stays awake and keeps the GPU out of D3cold. The HD-audio driver reports when it suspends and resumes (`GpuFunctionPowerBridge`, see "HD audio runtime power" below), so a silent GPU can still go cold. |
| Autosuspend | `set_autosuspend_ms` (the kernel uses 5 s, as Linux): the last `release()` leaves the GPU on, `service(now_ms)` (called once a second from the main loop) puts it to sleep once it has been idle that long. |
| D3cold | With an `IGpuPlatformPower` attached and `allow_cold` set, `service()` takes a D3hot GPU on to D3cold. The type 0 header of the GPU and its functions is saved before and written back after the power returns (BARs, ROM BAR, interrupt line, command last). |
| Power source | `set_power_status` (from the battery read every 10 s) fills in what a `select()` caller leaves Unknown: on battery below 30 % or with power saver, heavy work goes to the integrated GPU. |
| Probes | `GpuPool::bind_probes` gives a driverless GPU an `IGpuProbe` (`Multi/GpuProbe.hpp`, `GpuProbes.cpp`): NVIDIA through the native driver's read-only start (chip, VRAM, VBIOS, sensors, the EDID of monitors on the GPU's own ports), AMD and Intel discrete chips through register reads (see "AMD and Intel probes" below). A probe sets itself up again after a wake, so a GPU with only a probe may go cold. `probe_temperature` never wakes a GPU. |
| POST | A secondary NVIDIA GPU that the firmware did not POST (and every one after a power cut) needs its VBIOS init scripts (`Nvkm/NvInitScript`, PLLs through `Nvkm/NvPll`) or, on GM20x to Volta, the VBIOS's DEVINIT application run on the PMU falcon (`NvDevinit::post`, see [Nvidia.md](Nvidia.md)). Both write to the card, so they run only with `TCOS_NVIDIA_NATIVE`; otherwise they are walked dry and the boot log says so. Turing+ (FWSEC) is refused. |
| Render choice | `GraphicsPool::acquire` no longer wakes a discrete GPU with no driver and no 3D engine (the work would run on the CPU anyway): the display GPU gets it. |
| PCI Express link | `enable_aspm(i)` turns on the L0s/L1/clock-PM states both ends of the GPU's link support (`main.cpp` does it for the discrete GPUs of hybrid machines). The Express capability's Device/Link Control registers are saved and restored with the header across a power cut. `set_link_off_when_cold(i)` disables the root port's link before the cut and retrains it (waiting for the data link) after; off by default. |

The platform handlers (attached in `main.cpp` once the ACPI tables are loaded, one per sleeping discrete GPU):

- [MultiGpuAcpi.md](MultiGpuAcpi.md): `Multi/Acpi/`: finding the GPU's ACPI device, `_PR3`/`_PR0` power resources with `_PS3`/`_PS0`, NVIDIA Optimus / JT / the old hybrid `_DSM`, AMD ATPX (power and mux). Test `GpuAcpiPowerTest`.
- [MultiGpuPlatform.md](MultiGpuPlatform.md): Apple gmux (`Multi/AppleGmux`: mux, discrete power, backlight) and the PCI Express link (`Multi/PcieLink`: upstream bridge, device functions, link disable/retrain, ASPM negotiation). Tests `AppleGmuxTest`, `PcieLinkTest`.

## Not done

- **Display drivers for a second GPU:** NVIDIA has one (`NVIDIA/NvidiaSecondaryGpu`: mode set from the EDID, BAR1 page tables, TMDS and DisplayPort on the GF119
  display class; Kepler to Pascal), behind `TCOS_NVIDIA_NATIVE` and never run on a card. Tesla, Volta and later NVIDIA, and every AMD and Intel discrete GPU, have only a
  probe: they cannot show anything and render only on the CPU. Each display driver plugs into `GpuNativeDrivers.cpp` (display) and `RenderNodeAccel.cpp` (3D).
- The desktop does not span monitors on several GPUs: `SystemMonitors` and the monitor compositor still drive the display GPU only, so a monitor lit by the
  secondary NVIDIA driver shows only what is presented to it (`present_image`, `renderer_2d(i)`).
- A GPU with a display driver is never power-cycled (the drivers cannot restore their state), so it stays awake.
- The mux is found (gmux, ATPX, NVIDIA hybrid `_DSM`) but nothing switches the panel.
- Link disable around D3cold is available but off by default; Turing+ NVIDIA POST (FWSEC); jack sensing while the GPU's HD audio sleeps.
- Cross-GPU copies are a read-back plus a blit through the CPU (no peer-to-peer DMA, no shared surfaces).
- Tested against fake PCI buses, register models, fake AML and fake ports, and built for UEFI, but not run on real hybrid hardware or in QEMU with several GPUs.

## AMD and Intel probes

`make_vendor_gpu_probe` (`Multi/GpuProbes.cpp`) also gives a probe to a discrete AMD or Intel GPU that the identification tables know (`AmdGpuProbe::supported`,
`IntelGpuProbe::supported`; APUs, integrated graphics, Instinct and Ponte Vecchio get none). Both only read registers, as the Linux drivers do, and need nothing set up
again after a power cut: `resume()` checks that the GPU answers and reads the memory size again (keeping the first one if it now reads 0). Tests:
`tests/run_host_tests.sh AmdGpuProbeTest IntelGpuProbeTest`.

| | AMD (`AMD/AmdGpuProbe`, register BAR 5) | Intel (`Intel/IntelGpuProbe`, BAR 0) |
| --- | --- | --- |
| Chip | `amd_gpu_identify`: Polaris and Vega M, Vega 10/12/20, RDNA 1-4 | `intel_gpu_identify`: DG1, Arc Alchemist (DG2), Arc Battlemage |
| Memory | `CONFIG_MEMSIZE` (0x150A, VI) or `RCC_CONFIG_MEMSIZE` (dword 0xDE3, Vega and later), in MiB; written by the VBIOS POST, so 0 on a card nobody POSTed | DG2/Battlemage: `SG_TILE_ADDR_RANGE(0)` (0x1083A0, bits 17:8 in GiB); DG1: `GSMBASE` (0x108100), the size below the stolen memory |
| Temperature | `CG_MULT_THERMAL_STATUS.CTF_TEMP`: through the SMC index (0xC0300014) on VI, THM 0x16600+0x5A on Vega 10/12, +0x5F on Vega 20 and RDNA 1/2; RDNA 3/4: none | `PCU_CR_PACKAGE_TEMPERATURE` (0x145978) on DG1/DG2, `BMG_PACKAGE_TEMPERATURE` (0x138434) on Battlemage, bits 7:0 |
| VBIOS | Polaris/Vega M: the ROM through the SMC (`AmdVbios::read_from_rom_registers`); part number and name strings as `atom.c` finds them | - |
| Writes | the SMC index (and ROM index) registers on VI only | none |

Not done or not verified: none of it has run on real hardware. The RDNA 1/2 temperature is a register the Linux driver does not use (it asks the SMU firmware for a
metrics table), so a reading is only believed between 1 and 150 degrees. RDNA 3/4 put the THM block where the IP discovery table says, so they have no temperature.
The VBIOS of Vega and later (ROM through SMUIO, or ATRM/VFCT, which need the ACPI tables the pool does not have) is not read. No probe reports connected outputs
(that needs the connector table and the hot plug pins). The DG1 temperature register is xe's DG2 offset under i915's DG1 name.

## HD audio runtime power

The discrete GPU's HDMI/DisplayPort audio function (class 04/03) is driven by `IntelHdaAudioDevice`. The GPU can only reach D3cold while
that function is idle. Following Linux's HD-audio runtime PM, the device now suspends itself and resumes on demand.

Suspend (`suspend()`, or `service(now_ms)` once nothing has been written for the idle period) is only allowed when the device is running,
nothing is queued in the playback ring and the capture side is off. It:

1. marks the registers unusable under the playback lock, so the mixer thread only works from memory from then on;
2. stops the output stream;
3. puts the codec's function group in D3 and stops the CORB and RIRB;
4. holds the link in reset (CRST low);
5. saves the PCI header and puts the function in D3hot (PCI PM).

Resume (`resume()`, or the first `write()` that carries sound) brings the function back to D0, waits 10 ms and restores the header. It then
does what `initialize()` does: resets the link, restarts the rings, rediscovers the codec (a display may have changed meanwhile), programs
the path, InfoFrame, converter and stream format again, restarts the stream and applies the volume. While the device is suspended,
`set_volume()`, `retarget()` and position reads do not touch the hardware.

If the resume fails, the device is left suspended-like and stops playing (`PowerState::Failed`).

### Telling the power owner

`IPciFunctionPowerObserver` (`src/Hardware/Bus/Pci/PciFunctionPower.hpp`) has `function_idle(PciAddress)` and `function_busy(PciAddress)`:

- `idle` is called after the hardware is down.
- `busy` is called before any register access on resume, and may take as long as a D3cold wake.
- The calls alternate, are made from thread context and are never made under a spin lock. They are made under the device's
  `power_mutex_`, a sleeping mutex.

`SystemAudioDevices::attach_power_observer(function, observer, idle_ms)` finds the HD-audio controller on a PCI function, attaches the
observer and turns on its idle policy (5 s by default). Without an observer the policy stays off. `SystemAudioDevices::poll()` runs
`service_power(now_ms)`. `main.cpp` and the GPU manager are not wired yet: the coordinator calls `GpuManager::mark_function_busy` from the
observer.

### Locking

The lock order is `power_mutex_`, then the playback lock, then the codec lock. `configure_path()` now stores the volume widget under the
playback lock.

### Tests

`tests/HdaRuntimePmTest.cpp` puts `tests/FakeHda.hpp` behind a simulated PCI function that has a PM capability and loses its header on
D3hot to D0. A link reset wipes the simulated codec. The test covers:

- idle suspend and what it leaves behind;
- no hardware access while suspended;
- resume on write, with the header and codec state restored;
- observer order and alternation;
- a failed resume;
- a controller with no PCI function;
- five threads (writers, volume, `service()`, forced suspend/resume plus `retarget()`), under both ASan and TSan.

### Not done or not verified

- Not run on hardware or in QEMU.
- The controller does not suspend while its capture side runs, because recording has no "open" to wake it with.
- Jacks are not watched while suspended. A resume rediscovers the pin instead.
- Only the type 0 header is saved, not Intel's TCSEL or the vendor registers Linux restores.
