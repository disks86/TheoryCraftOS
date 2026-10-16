# TheoryCraftOS

A minimal x86_64 UEFI operating system written in C++20 with a little
assembly (`src/hardware/x86_64/cpu.S`). It boots under OVMF in QEMU, prints to
the UEFI console and the QEMU debug console (port 0x402, shown on your terminal), then shuts down on a key.

## Layout

- `src/main.cpp`, `console.*`, `uefi.hpp`, `runtime.cpp`: entry point, logging facade, UEFI types, compiler runtime stubs.
- `src/Allocators/`: `IAllocator` and the default `Allocator`, a cache-line block pool
  that gets its memory from an `IRandomAccessMemory` (`base()` / `usable_bytes()`).
- `src/CPUScheduler/`: `ICPUScheduler` and the default `Scheduler`, a cooperative run-to-completion
  scheduler with high-performance and efficiency queues that takes an `ICentralProcessingUnit` and an `IAllocator`.
- `src/hardware/`: every class that touches hardware derives from `hardware::IHardware`
  (`name()`, `initialize()`, `shutdown()`): `IoPort`, `DebugConsole`, `TextConsole`, and the CPUs.
  - `hardware/CentralProcessingUnit/`: `ICentralProcessingUnit` and its implementations
    (`X86_64Cpu`, with its assembly in `X86_64Cpu.S`).
  - `hardware/RandomAccessMemory/`, `GraphicsProcessingUnit/`, `NeuralProcessingUnit/`: interfaces
    (`IRandomAccessMemory`, `IGraphicsProcessingUnit`, `INeuralProcessingUnit`); only RAM has an
    implementation so far (`UefiRandomAccessMemory`).
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
