# Hybrid graphics: platform pieces

The parts of a hybrid-graphics laptop that are not the GPU drivers: the mux that moves the panel between the GPUs, the platform's control
of the discrete GPU's power, and the PCI Express link the discrete GPU sits on. The interfaces are in
`src/Hardware/GraphicsProcessingUnit/Multi/GpuPlatformPower.hpp` (`IGpuPlatformPower`, `IGpuMux`); the GPU manager decides when to use
them.

## Apple gmux (`Multi/AppleGmux.hpp`)

After Linux `drivers/platform/x86/apple-gmux.c`. The gmux of the dual-GPU MacBook Pros is the ACPI device with `_HID` `APP000B`; its
`_CRS` gives an I/O port window.

- `AppleGmux::find_device(vm)` walks the namespace for a present `APP000B` device. `AppleGmux::find_resource(vm, device, &res)` evaluates
  its `_CRS` and reads the first I/O range (I/O port, fixed I/O, Word or DWord I/O descriptors) and the first memory range.
  `apple_gmux_parse_resources()` is the parser alone; every descriptor length is checked.
- `AppleGmux(io, base, length, delay_us, external_switchable)` takes the ports through `IGmuxPorts` (the `IPortIo` byte ports of the
  hardware monitor code plus a 32-bit read). `X86GmuxPorts` (in `AppleGmuxPorts.cpp`, kept apart so host tests need no in/out) is the
  machine's.
- `detect()` (also `initialize()`) reads the version registers at base+4..6. Values other than all ones: the **classic** layout, where
  register N is port base+N. All ones: if the window reaches base+0xD4, the **indexed** layout is probed (0xAA, 0x55 written to
  base+0xCC/0xCD must read back as 0x55AA); its registers sit behind a mailbox (value at 0xC2, read index 0xD0, write index and handshake
  bit at 0xD4) and the version is one 32-bit register. Every handshake wait is bounded (200 polls 100 us apart, as Linux), and a timeout
  makes the access fail. Nothing answering (all ones, scratch registers that do not keep values) leaves `layout()` `None` and every call
  returns false.
- `IGpuMux`: `active()` reads `GMUX_PORT_SWITCH_GET_DISPLAY` (2: integrated). `switch_to()` writes DDC (1/2), display (2/3) and, on
  models where the external port goes through the gmux (`external_switchable`, Linux keeps a DMI list), external (2/3), as
  `gmux_switchto()`. `switch_ddc()` moves only the DDC lines, on the classic gmux only (Linux: pre-retina models can switch DDC alone;
  retina models need the eDP set up on both GPUs).
- `IGpuPlatformPower` (`GpuPowerMechanism::AppleGmux`): `power_on()` writes 1 then 3 to `GMUX_PORT_DISCRETE_POWER`, `power_off()` 1 then 0.
- `IBacklight`: brightness in percent over `GMUX_PORT_MAX_BRIGHTNESS` (clamped to 24 bits) and `GMUX_PORT_BRIGHTNESS` (24-bit level).
  Raw units through `max_brightness()`, `brightness()`, `set_brightness()`.

Not done: the MMIO gmux of T2-era models (`find_resource()` reports the memory range; nothing drives it), the gmux's GPE interrupt (Linux
waits up to 200 ms on it after a power change; here the caller's `power_on_delay_ms()` wait covers it), Linux's DMI quirks, and the
"which backlight wins" decision (Linux registers the gmux backlight only when the ACPI video backlight type is vendor). Not validated
on hardware.

## PCI Express link and functions (`Multi/PcieLink.hpp`)

Free functions over `IPciBus`:

- `pci_find_upstream_bridge(pci, function, &bridge)`: of the type 1 headers whose secondary..subordinate range holds the function's bus,
  the innermost (highest secondary bus). False on the root bus.
- `pci_device_functions(pci, function, ArrayView<PciDeviceInfo>)`: every function of the same bus/device, in function order. A laptop
  NVIDIA GPU has VGA at 0, HD audio at 1, and from Turing xHCI at 2 and UCSI at 3. All of them must be in D3hot before the GPU's link can go
  down (the HD audio function otherwise keeps the GPU in D0): put functions N..1 in D3hot, then 0; wake 0 first. (Some firmware hides the
  HD audio function; Linux's `quirk_nvidia_hda` sets bit 25 of config 0x488 to show it. Not done here.)
- `pcie_link_capabilities()`, `pcie_link_state()`: the PCI Express capability's port type, Link Capabilities (speed, width, ASPM support and
  exit latencies, Clock PM, DLL Link Active reporting), the endpoint's acceptable latencies, Link Status and Control.
- `pcie_link_disable(pci, bridge)`: sets Link Disable in a root port or switch downstream port.
- `pcie_link_enable(pci, bridge, delay, timeout_ms, settle_ms)`: clears Link Disable, sets Retrain Link in a second write, then polls Data
  Link Layer Link Active every millisecond up to `timeout_ms` where the port reports it (false if it never sets), or waits 100 ms where it
  does not; then `settle_ms` (100 ms by default, the wait before the first configuration request).
- `pcie_aspm_negotiate(upstream, downstream...)`: L0s and L1 where both ends support them and the link's exit latency (the larger end's)
  is within each endpoint's acceptable latency; Clock PM where every downstream function supports it. `pcie_aspm_apply()` writes it:
  bits being turned off go to the functions first and then the bridge, bits being turned on to the bridge first and then the functions
  (the spec's ASPM L1 order, as Linux `pcie_config_aspm_link()`); all functions get the same value; Clock PM only in the functions.
  `pcie_aspm_configure(pci, function)` does both for the bridge above a function and every function of its device.
- `pcie_bridge_d3_possible(pci, bridge)`: a root or downstream port with a PM capability; a hot-plug slot only with DLL Link Active
  reporting. Linux's firmware checks (`_S0W`, BIOS date, quirks) are the caller's.

Link Control writes put 0 in the Link Status half, so its RW1C bits are left alone.

Not done: Common Clock Configuration and the link retrain Linux does before ASPM, L1 PM substates (L1.1/L1.2, the L1 PM Substates
extended capability), the switch hop latency Linux adds per switch, Link Capabilities 2 / target speed, and anything in extended config
space (the registers used are in the first 256 bytes). Not validated on hardware.

## Tests

- `tests/AppleGmuxTest.cpp`: a model port window for both layouts (the indexed handshake completes after a few polls, late, never, or with
  a stale "done" left over), the switch writes and read-back, the 1/0 and 1/3 power sequences, brightness and percent, a dead window, a
  window too small or wrapping past 0xFFFF, the `_CRS` parser (truncations and bit flips), and finding the device in AML (with `_STA`).
- `tests/PcieLinkTest.cpp`: a model hierarchy with real capability lists: root port with a four-function GPU, an iGPU on the root bus, a
  switch, a PCIe-to-PCI bridge with a conventional card. Bridge lookup, function listing, Link Disable/enable with DLL Link Active after
  three polls, never (timeout), or not reported (fixed wait), the ASPM negotiation table, the write order for enabling, disabling and
  changing ASPM, devices with no PCI Express capability, the D3 check, a looping capability list.

Run them with `tests/run_host_tests.sh AppleGmuxTest PcieLinkTest`.
