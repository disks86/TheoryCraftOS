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

## The desktop across GPUs

The desktop spans the monitors of every GPU that has a display driver (test: `tests/run_host_tests.sh MultiGpuMonitorTest`, ASan and TSan).

- `main.cpp` hands `SystemMonitors` every `GpuPool` node with a driver besides the display GPU (`add_gpu(driver, pool index)`). The display GPU's monitors
  come first and keep their ids (their connector numbers) and order, so one GPU behaves as before; the k-th other GPU's monitors get the ids
  `k * 16 + connector`, from its `IMonitorSource`, or its whole screen as one monitor when it has none. `Monitor::gpu()` is the pool index; the boot log
  and the Display settings page say which GPU drives each monitor.
- The default row puts the other GPUs' monitors to the right, GPU after GPU; they are dragged, scaled, made primary and given modes like any other. A
  mode change goes to that GPU's driver: `IMonitorSource::set_display_mode`, `apply_layout` for a GPU with several monitors, or
  `IGraphicsProcessingUnit::set_mode` (its own mode list) for a GPU that tells nothing about monitors.
- Each GPU has a surface of its own: `physical_placements()` puts each monitor in its own GPU's row, and `targets()` lists the monitors that are not on the
  display GPU. `MonitorDisplay::update(..., targets)` then copies those monitors' part of every rectangle the desktop's renderer blits (its damage) to
  their GPU's screen through the driver's `blit()`, resampled when scaled, and `present()` presents each GPU that got something. The software pointer is
  part of the desktop's picture, so it is drawn on every screen. A copy to another GPU holds that GPU's `GraphicsPool::present_lock`, the lock
  `present_image` takes. With no other GPU nothing of this runs.
- Hot plug works as on the display GPU: `poll()` asks every GPU's monitor source.



What Linux does with runtime PM and vga_switcheroo, in `GpuManager` (test: `tests/run_host_tests.sh GpuRuntimePowerTest`, ASan and TSan):

| Piece | What it does |
| --- | --- |
| Companion functions | The other functions of a discrete GPU's device (HD audio, Turing's USB-C and UCSI) are found at start-up, go to D3hot before the GPU and wake after it. A function a driver uses (`mark_function_busy`) stays awake and keeps the GPU out of D3cold. The HD-audio driver reports when it suspends and resumes (`GpuFunctionPowerBridge`, see "HD audio runtime power" below), so a silent GPU can still go cold. |
| Autosuspend | `set_autosuspend_ms` (the kernel uses 5 s, as Linux): the last `release()` leaves the GPU on, `service(now_ms)` (called once a second from the main loop) puts it to sleep once it has been idle that long. |
| D3cold | With an `IGpuPlatformPower` attached and `allow_cold` set, `service()` takes a D3hot GPU on to D3cold. The type 0 header of the GPU and its functions is saved before and written back after the power returns (BARs, ROM BAR, interrupt line, command last). |
| Power source | `set_power_status` (from the battery read every 10 s) fills in what a `select()` caller leaves Unknown: on battery below 30 % or with power saver, heavy work goes to the integrated GPU. |
| Probes | `GpuPool::bind_probes` gives a driverless GPU an `IGpuProbe` (`Multi/GpuProbe.hpp`, `GpuProbes.cpp`): NVIDIA through the native driver's read-only start (chip, VRAM, VBIOS, sensors, the EDID of monitors on the GPU's own ports), AMD and Intel discrete chips through register reads (see "AMD and Intel probes" below). A probe sets itself up again after a wake, so a GPU with only a probe may go cold. `probe_temperature` never wakes a GPU. |
| POST | A secondary NVIDIA GPU that the firmware did not POST (and every one after a power cut) needs its VBIOS init scripts (`Nvkm/NvInitScript`, PLLs through `Nvkm/NvPll`) or, on GM20x to Volta, the VBIOS's DEVINIT application run on the PMU falcon (`NvDevinit::post`, see [Nvidia.md](Nvidia.md)). Both write to the card, so they run only with `TCOS_NVIDIA_NATIVE`; otherwise they are walked dry and the boot log says so. Turing+ boots itself from its ROM (FWSEC): `post()` waits (bounded, reads only, so in every build) for the GFW_BOOT flags (0x118128 bit 0, then 0x118234 = 0xff). |
| Render choice | `GraphicsPool::acquire` no longer wakes a discrete GPU with no driver and no 3D engine (the work would run on the CPU anyway): the display GPU gets it. |
| PCI Express link | `enable_aspm(i)` turns on the L0s/L1/clock-PM states both ends of the GPU's link support (`main.cpp` does it for the discrete GPUs of hybrid machines). The Express capability's Device/Link Control registers are saved and restored with the header across a power cut. `set_link_off_when_cold(i)` disables the root port's link before the cut and retrains it (waiting for the data link) after; off by default. |

The platform handlers (attached in `main.cpp` once the ACPI tables are loaded, one per sleeping discrete GPU):

- Display mux switch (`Multi/GpuMuxSwitch`, `Multi/GpuMuxPanel.hpp`; test `GpuMuxSwitchTest`, ASan and TSan): vga_switcheroo's switch.
  `GpuMuxSwitch(mux, leases, integrated_index, discrete_index, boot_side)` with each side's `IPanelOutput` attached; `switch_to(side)`
  checks (a mux, a driver on both sides, the target has a panel output and can light it, no switch in flight: a second caller gets `Busy`),
  takes a lease on the target (`GpuManagerLeases`: the GPU wakes), moves DDC first where the mux can, lets the target light the panel, moves
  the mux, then the old side releases the panel and its lease goes back (the discrete GPU may sleep). A failure is undone in reverse; a
  rollback that fails leaves the state unknown (`active()` false) until a mux that can tell (gmux) or `set_known_active()` settles it.
  `can_switch_to(side, &why)` is for a settings page. `main.cpp` keeps the gmux or the `GpuAcpiPower::find_mux` object, builds the switcher
  (discrete side `GpuNode::panel`; integrated side below), publishes it (`GpuMuxSwitch::published()`) and logs the mux kind.
  - Integrated side: `IntelGpu::panel_output()` (`IntelPanelOutput`, `Intel/IntelGpu.hpp`) where Intel native mode setting runs (Gen9.5, Gen11,
    Xe-LP) and the firmware lit an eDP panel; else `GopPanelOutput`, which cannot light the panel again (the switch back is refused until a
    restart). `IntelPanelOutput` follows i915: release = backlight off (PP_CONTROL, PCH PWM) and T9, plane saved, transcoder off (state bit),
    DDI function off, port off (DP_TP_CTL, DDI_BUF_CTL idle bit), panel power off through the panel power sequencer (it makes T10 and the power
    cycle); light = power cycle waited out, panel power on (PP_STATUS on), port on with training pattern 1, eDP training without an AUX
    handshake (pattern 1, 2, idle, normal), DDI function and transcoder on as the firmware had them, the plane put back (`Display::restore`),
    T8 and the backlight. The PLL and the transcoder's timings are never touched, so the panel comes back at the mode the firmware lit.
    Each failing step leaves nothing on (`PowerFailed`, `LinkTraining`, `ModeSet`). Test: `GpuMuxSwitchTest` (a register model).
  - The desktop follows (`GpuMuxDesktop`, the `IBuiltInScreenSwitch` a settings page sees): after the switch, `SystemMonitors::move_panel(from,
    to)` takes the panel's monitor off the old GPU's monitors (not connected there, and ignored by `poll()` while away) and puts the new GPU's
    (its Internal connector, read again; on a GPU without Internal connectors its only free connected one) in the same layout slot: place,
    scale and primary kept, the size of the new GPU's screen. The next `apply()` gives it a `targets()` entry, so `MonitorDisplay` copies its
    part of the desktop to the new GPU (or, back on the display device, draws it there directly). If the monitors find no panel on the new GPU
    the panel is switched back. `main.cpp` also spans a GPU with a zero-sized screen when its driver has a panel output (a discrete GPU in
    panel standby): none of its monitors is used until the panel moves there. Tests: `GpuMuxSwitchTest`, `MultiGpuMonitorTest`.
  - Display Settings' Graphics group (`Desktop/DisplaySettings`, shown when `Desktop::set_graphics_switch` got one): "The built-in screen is
    driven by <GPU> (the integrated GPU)", a button "Use <other GPU> for the Built-in Screen" (unavailable with "Not available because ..."
    under it when `can_switch_to` says no, or while new display settings wait for Keep/Revert), a confirmation (Switch / Cancel, Cancel the
    default: the screen goes dark for a moment), then drawing stops (`Desktop` stops its render loop), the switch, the monitors made real on
    the new GPU, drawing again; a failure is an error notice saying why and where the screen is (or that it may stay dark), and making the
    monitors real failing switches back. Test: `UiGuideTest` (Settings).
- [MultiGpuAcpi.md](MultiGpuAcpi.md): `Multi/Acpi/`: finding the GPU's ACPI device, `_PR3`/`_PR0` power resources with `_PS3`/`_PS0`, NVIDIA Optimus / JT / the old hybrid `_DSM`, AMD ATPX (power and mux). Test `GpuAcpiPowerTest`.
- [MultiGpuPlatform.md](MultiGpuPlatform.md): Apple gmux (`Multi/AppleGmux`: mux, discrete power, backlight) and the PCI Express link (`Multi/PcieLink`: upstream bridge, device functions, link disable/retrain, ASPM negotiation). Tests `AppleGmuxTest`, `PcieLinkTest`.

## Not done

- **Display drivers for a second GPU:** NVIDIA has one (`NVIDIA/NvidiaSecondaryGpu`: mode set from the EDID, BAR1 page tables, TMDS and DisplayPort on the GF119
  display class (Kepler to Pascal) and on the window-channel classes c37d/c57d/c67d (Volta, Turing, GA102..GA107 without GSP, as nouveau)), behind
  `TCOS_NVIDIA_NATIVE` and never run on a card. AMD Polaris 10/11/12, Vega M and Vega 10/12/20 have one too (`AMD/AmdSecondaryGpu`: POST,
  memory controller apertures, EDID mode set through the video BIOS's command tables on DCE 11.2, and on DCE 12 the ATOM firmware tables with the display core's register
  sequences (`AMD/AmdDce12`), HDMI/DVI and DisplayPort with link training; see NativeModeSetting.md),
  behind `TCOS_AMD_NATIVE` and never run on a card. Intel Arc Alchemist (DG2) has one too (`Intel/IntelSecondaryGpu`: power wells, CDCLK, SNPS PHY MPLLB,
  DDI A-D, transcoder/pipe/plane from scratch, the surface in local memory behind GGTT entries, HDMI or DisplayPort with link training; see NativeModeSetting.md),
  behind `TCOS_INTEL_DGPU_NATIVE` and never run on a card. Tesla and Ada and later NVIDIA (Ada: nouveau drives its display only through GSP), AMD Navi and later (DCN), and Intel DG1 and Battlemage, have only a
  probe: they cannot show anything and render only on the CPU. Each display driver plugs into `GpuNativeDrivers.cpp` (display) and `RenderNodeAccel.cpp` (3D).
- Spanning (see "The desktop across GPUs"): the desktop is drawn once, on the CPU, and copied to the other GPUs' screens (no rendering on the GPU that drives
  the monitor); monitors at different scales on several GPUs are resampled from one picture instead of each drawn at its own scale (`MonitorRenderers` is
  for the display GPU only); a monitor hot-plugged on another GPU is seen only if that GPU's monitor source reports connector changes
  (`refresh_connectors`); the console after the desktop shows on the display GPU only, the other screens keep the desktop's last picture.
- A GPU with a display driver is never power-cycled (the drivers cannot restore their state), so it stays awake.
- The mux switch (`Multi/GpuMuxSwitch`, above): none of it has run on a muxed laptop. The integrated side lights the panel again only on Intel
  Gen9.5/Gen11/Xe-LP with native mode setting and an eDP panel the firmware lit (`IntelPanelOutput`, unverified on hardware: register offsets
  from i915, link training without AUX, so a panel that needs AUX training stays dark); AMD integrated and every other case are
  `GopPanelOutput` (no way back until a restart). The discrete side is `NvidiaSecondaryGpu` (with `TCOS_NVIDIA_NATIVE`) only. While the panel
  is on the discrete GPU the desktop is still drawn on the display device and copied over (no rendering on the discrete GPU, no change of the
  display device); the console after the desktop goes to the display device only. The switch blocks the settings page's handler thread for
  the panel power sequence (well under a second) and cannot be cancelled once started.
- Link disable around D3cold is available but off by default; Turing+ NVIDIA POST is only the GFW_BOOT wait (nouveau's PRE_OS start after it
  is not done).
- Hot plug on a sleeping GPU (see "Monitors plugged into a sleeping GPU"): only GPEs named by the GPU's or its root port's `_PRW` are armed
  (`AcpiEvents` does not enable every `_Lxx`/`_Exx` GPE as Linux does at boot, so a board that routes the port's hot-plug line to a GPIO GPE with no
  `_PRW` reports nothing until something else arms that GPE); the HD-audio codec's unsolicited responses are not used (it re-reads its pins only when
  told); only the NVIDIA probe reads connectors (AMD and Intel probes cannot tell, so their notifications are passed on unchecked); a check runs on
  the main loop and stalls it for the wake (up to the D3cold power-on delay). Not run on hardware.
- Cross-GPU copies are a read-back plus a blit through the CPU (no peer-to-peer DMA, no shared surfaces).
- Tested against fake PCI buses, register models, fake AML and fake ports, and built for UEFI. In QEMU only spanning was run (QEMU VGA + a second virtio-gpu,
  both heads checked by screendump); nothing has run on real hybrid hardware.

## Monitors plugged into a sleeping GPU

`Multi/GpuHotplug` (test: `tests/run_host_tests.sh GpuHotplugTest`, ASan and TSan). While a discrete GPU sleeps (D3hot, or D3cold with its power
cut) its own hot-plug detection is dark, so a monitor plugged into an HDMI or DisplayPort port wired to it goes unnoticed. As Linux does:

- **ACPI notifications.** `watch_acpi(gpu, aml, address, bridge)` finds the GPU's Device, its root port and the Devices below the GPU's (its
  outputs) through `GpuAcpiNamespace`; `GpuHotplug` is an `IAcpiNotifyListener` (`AcpiEvents::add_listener`). What counts (`is_output_notification`):
  on the GPU 0x02 (device wake, a PME from the sleeping GPU), 0x80, 0x81; on an output 0x80, 0x81; on the root port 0x00 (bus check), 0x01 (device
  check), 0x02; on the integrated GPU's ACPI video Device (`GpuAcpiRole::OtherAdapter`) 0x81 only. `on_notify` only marks the GPU. `main.cpp` arms
  the GPU's and the root port's `_PRW` wake events (`AcpiEvents::enable_wake`).
- **Polling**, as DRM polls connectors that cannot interrupt: `set_poll_policy(GpuOutputPollPolicy)`, **off by default**. When on, every watched
  GPU whose probe can read outputs is looked at every `period_ms` (10 s; at least 1 s). Each look wakes the GPU, which then stays up for the
  autosuspend time (5 s), so a 10 s period keeps it powered about half the time; that is why it is off. On battery below `min_battery_percent` (30 %)
  or with battery saver, a sleeping GPU is not woken for a poll (an awake one is still looked at; a firmware notification always is).
- **The check.** `service(now_ms)` (from the main loop, next to `GpuManager::service`) takes `GpuPool::refresh_outputs(i)`: a lease (the GPU wakes,
  through D3cold if need be), `IGpuProbe::resume()` if the GPU was woken since the probe last saw it, `IGpuProbe::refresh_outputs()` (NVIDIA:
  `NvidiaNative::refresh_connectors`, the connectors whose hot-plug line changed and their EDIDs), the lease back. A GPU with a display driver is not
  touched (it never sleeps; `SystemMonitors::poll` watches its monitor source), and neither is one whose probe cannot see outputs
  (`IGpuProbe::reads_outputs`, false for AMD and Intel): a notification for them is passed on unchecked (`GpuOutputCheck::CannotTell`).
- **Listeners.** `IGpuOutputListener::outputs_changed(gpu)` is called once per change, from `service()`. `main.cpp`'s listener has every companion
  function of the GPU re-read its codec (`SystemAudioDevices::connections_changed` -> `IntelHdaAudioDevice::connections_changed`: a running controller
  retargets, a suspended one is resumed, which reads the pins and ELD again, and goes idle after 5 s); the loop then runs `SystemMonitors::poll`
  (with `Desktop::apply_monitor_changes`) and, for a GPU with only a probe, logs the monitors on its outputs and says that it cannot show them yet.

Threads: `on_notify` from any thread (a short lock and a bit); `service()` passes are serialized; listeners are added and removed from any thread,
and `remove_listener` returns once no call to that listener is in flight (a listener must not call `remove_listener` or `service` itself). Lock order:
the service lock, the pool's `probe_lock_` (now also taken by `probe_temperature`), the manager's lock; then the dispatch lock, the listener list.

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
(that needs the connector table and the hot plug pins), so `GpuHotplug` passes their notifications on unchecked. The DG1 temperature register is xe's DG2 offset under i915's DG1 name.

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
- Jacks are not watched while suspended (no unsolicited responses). A resume rediscovers the pin, and `connections_changed()` (called when
  `GpuHotplug` reports a display change on the GPU) resumes the controller or retargets it so the pins and ELD are read again.
- Only the type 0 header is saved, not Intel's TCSEL or the vendor registers Linux restores.
