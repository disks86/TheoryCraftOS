# Multiple GPUs (integrated + discrete)

Code: `src/Hardware/GraphicsProcessingUnit/Multi/`, test: `tests/MultiGpuTest.cpp` (`tests/run_host_tests.sh MultiGpuTest`).

| File | Job |
| --- | --- |
| `GpuTopology` | Finds every PCI display controller and classifies it `Integrated`, `Discrete`, `Virtual` or `Unknown` from the vendor tables (Intel/AMD generation tables say `discrete`; NVIDIA is always discrete). Picks the **boot display**: the adapter whose PCI memory/IO decoding the firmware left on, else the first. |
| `GpuPowerPolicy` | `gpu_select()`: which adapter for a workload. Desktop, video and web go to the integrated GPU; games and compute go to the discrete one, except on battery below 30 % or with battery saver. `GpuPreference::LowPower` / `HighPerformance` override. A single GPU is always chosen; virtual adapters only when nothing else exists. |
| `GpuManager` | Puts every discrete adapter that is not the boot display into PCI D3hot at start-up, and wakes it (D0 + 10 ms) on `acquire()`; the last `release()` puts it back to sleep. |

`SystemGpu` builds the display driver on `gpu_boot_display()`, so on a hybrid machine the screen runs on the GPU the firmware used.
`main.cpp` creates a `GpuManager` after `SystemGpu` and logs the adapters on a hybrid machine.

## Not done

- No driver is built for the second GPU (`SystemGpu` has inline storage for one); the policy and power state exist, but there is no render offload path
  yet. Whoever adds one takes a lease around its work and presents the result through the display GPU (copy over PCIe).
- Muxed laptops (a hardware switch that moves the panel between GPUs), ACPI `_PR3`/`_OFF` power resources and PCIe link-state (ASPM) control.
- Battery state is not read by the manager; the caller fills `GpuSelectionInput` (see `Battery/AcpiBattery`).
- Not tested on real hybrid hardware, only against a fake PCI bus.
