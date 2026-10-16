# TheoryCraftOS

A minimal x86_64 UEFI operating system written in C++20 with a little
assembly (`src/hardware/x86_64/cpu.S`). It boots under OVMF in QEMU, prints to
the UEFI console and the QEMU debug console (port 0x402, shown on your terminal), then shuts down on a key.

## Layout

- `src/main.cpp`, `console.*`, `uefi.hpp`, `runtime.cpp`: entry point, logging facade, UEFI types, compiler runtime stubs.
- `src/Allocators/`: `IAllocator` and the default `Allocator`, a cache-line block pool
  that gets its memory from an `IRandomAccessMemory` (`base()` / `usable_bytes()`).
- `src/CPUScheduler/`: `ICPUScheduler` and the default `Scheduler`, a cooperative run-to-completion
  scheduler with high-performance and efficiency queues that spreads tasks over the cores the
  `ICentralProcessingUnit` reports (tasks on other cores must not call firmware). Takes an `IAllocator` too.
- `src/hardware/`: every class that touches hardware derives from `hardware::IHardware`
  (`name()`, `initialize()`, `shutdown()`): `IoPort`, `DebugConsole`, `TextConsole`, and the CPUs.
  - `hardware/CentralProcessingUnit/`: `ICentralProcessingUnit` (instructions plus core count, core kinds,
    start-on-core) and its implementations. `X86_64Cpu` (shared base: CPUID, multi-core via UEFI MP
    services) and `SystemCpu` sit at the top. Each vendor has a subfolder holding its base class and one
    small class per microarchitecture: `Intel/` (Conroe ... Diamond Rapids, Atom lines), `AMD/` (K8, K10,
    Bulldozer, Piledriver, Zen ... Zen 5), `Hygon/`, `Zhaoxin/`, and `QEMU/` (the generic qemu64 model).
    The generation is chosen at boot from CPUID vendor and family/model/stepping, via the rule table in
    each vendor's `*Generations.cpp`; this covers every x86-64 model in `qemu-system-x86_64 -cpu help`.
  - `hardware/GraphicsProcessingUnit/`: `IGraphicsProcessingUnit` (mode setting, 2D drawing, text rendering),
    `FramebufferGpu` (software drawing + 8x16 font on a linear framebuffer), `SystemGpu` (finds the PCI display
    device and picks a class). Vendor folders: `QEMU/` (std VGA with real mode setting through the Bochs VBE
    registers), `Intel/`, `AMD/`, `NVIDIA/` (identify the device but still use the firmware framebuffer until
    native drivers exist), and `UEFI/` (`UefiGopGpu`, the fallback for any other display). `src/GraphicsConsole`
    is a scrolling text console that only uses the GPU interface.
  - `hardware/Bus/`: `IBus` and one subfolder per bus type, each with an interface and implementations.
    - `Pci/`: `IPciBus` (config access, BARs, capability list), `PciBusBase` (shared scanning), `LegacyPciBus`
      (I/O ports 0xCF8/0xCFC, 256-byte config space).
    - `Pcie/`: `IPcieBus` (adds the extended capability list) and `EcamPcieBus` (memory-mapped config space,
      located through the ACPI MCFG table).
    - `Usb/`: `IUsbBus` and `UsbBus`, which finds host controllers on PCI (UHCI/OHCI/EHCI/xHCI), and reads the
      root ports of xHCI controllers (device attached, speed). It does not enumerate or talk to devices yet.
    - `SystemPciBus` picks PCIe/ECAM when firmware provides MCFG, else legacy PCI.
  - `hardware/Acpi/`: `AcpiTables` (RSDP -> XSDT/RSDT table lookup).
  - `hardware/RandomAccessMemory/`, `NeuralProcessingUnit/`: `IRandomAccessMemory` (implemented by
    `UefiRandomAccessMemory`) and `INeuralProcessingUnit` (no implementation yet).
  - `hardware/SystemHardware.hpp`: aliases `SystemCpu` / `SystemRam` to the classes for this architecture.
  - `hardware/x86_64/port.S`: port I/O assembly for `IoPort`; `asm.hpp` is private to `hardware/`.

## Requirements

`cmake`, `ninja` (or make), `x86_64-w64-mingw32-gcc/g++` (mingw-w64),
`qemu-system-x86_64`, OVMF/edk2 firmware, `gdb`.

mingw-w64 is used because it emits PE32+ directly with the UEFI-compatible
calling convention - no gnu-efi or lld needed.

## Build & run

```sh
cmake -S . -B build -G Ninja
cmake --build build              # produces build/esp/EFI/BOOT/BOOTX64.EFI
cmake --build build --target run # boot in QEMU (OS output on your terminal)
```

### Debugging in QEMU

`scripts/gdb-proxy.py` first rebuilds `BOOTX64` (output goes to the debugger console),
then starts QEMU (frozen at reset, gdbstub on :1234) and
bridges gdb to it: `target remote | scripts/gdb-proxy.py`. It rewrites
software breakpoints to hardware ones, so breakpoints set before OVMF has
loaded the image still work. OS output appears on gdb's stderr and in
`<build>/debugcon.log`.

```sh
gdb -ex 'symbol-file build/BOOTX64.efi' -ex 'target remote | scripts/gdb-proxy.py' \
    -ex 'break kernel_main' -ex continue
```

### CLion

`.run/Debug TheoryCraftOS.run.xml` is a *GDB Remote Debug* configuration using
the command above. Select **Debug TheoryCraftOS** in the run dropdown (not the
auto-generated `BOOTX64` entry, which tries to run the `.efi` natively) and
press Debug. QEMU opens, the debugger is paused at reset; press Resume.
Use the **QEMU** config to just run the OS. Set the debugger to GDB (the
bundled one works). If the symbol path is wrong for your build directory,
edit it in the configuration.
