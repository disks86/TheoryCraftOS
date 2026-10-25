# TheoryCraftOS

A hobby x86_64 UEFI operating system written in C++20 with a little assembly. It boots under OVMF in QEMU, lists each piece
of hardware as it comes up, then opens a graphical desktop with a taskbar, a Start menu and a set of built-in programs
(file explorer, web browser, media players, database query tool and more).

## A Mega Kernel

TheoryCraftOS is deliberately a **Mega Kernel**: almost everything that other systems put in user space lives in the
kernel image and runs as kernel threads in one address space. That includes:

- hardware drivers (USB, storage, audio, video codecs, cameras, printers, network cards, GPU/virgl)
- file systems and the storage stack
- the network stack: IPv4, TCP, UDP, DHCP, DNS, TLS 1.3/1.2, HTTP/1.1, HTTP/3 over QUIC, WebSockets, SNMP, IPP
- the sound server and the media codecs
- the database layer, including a built-in SQL database and a PostgreSQL client
- a JavaScript engine, an HTML/CSS/DOM web browser model (with a WebAssembly runtime, WebGL 1/2 and WebGPU on a software rasterizer, service workers and the Cache API) and a regular expression engine
- the GUI toolkit and the desktop itself

There is no system-call boundary or driver-model split. Services call each other directly through interfaces, which keeps
the system small to reason about and fast, at the cost of isolation. Memory safety is handled in the code instead: all
memory is owned by allocator-backed containers and smart pointers and passed around as bounds-checked views (see
[CLAUDE.md](CLAUDE.md)), and the decoders and parsers are tested with truncated and bit-flipped input under ASan/UBSan.

## Requirements

`cmake`, `ninja` (or make), `x86_64-w64-mingw32-gcc/g++` (mingw-w64), `qemu-system-x86_64`, OVMF/edk2 firmware, `gdb`.

mingw-w64 is used because it emits PE32+ directly with the UEFI-compatible calling convention - no gnu-efi or lld needed.

## Build & run

```sh
cmake -S . -B build -G Ninja
cmake --build build              # produces build/esp/EFI/BOOT/BOOTX64.EFI
cmake --build build --target run # boot in QEMU (OS output on your terminal); virgl 3D when this QEMU has virtio-gpu-gl, else standard VGA
cmake --build build --target run-gl # always with a virtio-gpu 3D device: QEMU renders through the host GPU (virgl)
cmake --build build --target run-std # always with the standard VGA card (software rendering)
```

`run-gl` needs QEMU built with virglrenderer and a host OpenGL stack. `scripts/qemu-debug.sh` and `scripts/gdb-proxy.py`
take `QEMU_GPU=gl` (virgl), `virtio` (virtio-gpu, software rendering), `std` (Bochs VGA, software rendering) or the default
`auto`. Without a 3D engine everything falls back to the software renderers.

The VM has an Intel HD Audio card (`ich9-intel-hda` with an `hda-duplex` codec) played through the host's sound server: PipeWire,
else PulseAudio, ALSA or SDL. `QEMU_AUDIO=<driver>` in the scripts (`-DQEMU_AUDIO_DRIVER=<driver>` for the cmake targets) picks
another `-audiodev` driver; `QEMU_AUDIO=wav` records to `audio.wav` in the build directory to check without speakers.

Host tests build with ASan and UBSan and run with `tests/run_host_tests.sh <TestName>`.

## AArch64 (work in progress)

Any ARM64 machine with UEFI firmware is the target. The first milestone is a bootable `BOOTAA64.EFI` holding the portable layers
(console, RAM, allocator, ACPI tables, the SPCR debug UART, the firmware framebuffer). Exception vectors, the GIC (v2 and v3, from the
ACPI MADT), the generic-timer tick and the preemptive thread scheduler run on the boot core, at EL1 or EL2 (checked in QEMU with
`-machine virt[,virtualization=on][,gic-version=3]`). The other cores start through PSCI `CPU_ON` (SMC or HVC as the FADT says) and copy the boot core's MMU setup (checked with `-smp 4`). Floating-point state across switches, PCIe (ECAM) and
the drivers are still to port before the rest of the OS can join this image.

Needs `clang` (aarch64 target), `lld-link` (rustup's `rust-lld` works), mingw-w64's libstdc++ headers, `qemu-system-aarch64` and AAVMF:

```
cmake -S . -B build-aarch64 -DTCOS_ARCH=aarch64 -G Ninja
cmake --build build-aarch64 --target run   # QEMU "virt"; the log appears on the serial console in your terminal
```

## RISC-V (work in progress)

Any RV64 machine with UEFI firmware (and SBI, which every RISC-V UEFI platform has) is the target. LLVM cannot emit RISC-V PE/COFF, so
the image is linked as a position-independent ELF with `ld.lld` and `scripts/elf2efi.py` rewrites it as a PE32+ `BOOTRISCV64.EFI` (machine
`0x5064`, base relocations from the ELF's relative relocations), the way EDK2's GenFw does. The first milestone matches the AArch64 one: the
portable layers (console, RAM, allocator, ACPI tables, the firmware framebuffer), supervisor trap handling, the SBI timer tick, the preemptive
thread scheduler, and the other harts, which start through SBI `hart_start` (the boot hart comes from the firmware's `RISCV_EFI_BOOT_PROTOCOL`
and the harts from the ACPI MADT RINTC entries; the timebase from the RHCT). Console output through SBI is only enabled for a kernel panic,
because it usually reaches the same serial port as the UEFI text console. PLIC/APLIC/IMSIC interrupts, PCIe (ECAM) and the drivers are still
to port before the rest of the OS can join this image.

Needs `clang` (riscv64 target), `ld.lld` (rustup's `gcc-ld/ld.lld` works), Python 3, mingw-w64's libstdc++ headers, `qemu-system-riscv64` and
the edk2 `RISCV_VIRT_CODE.fd`/`RISCV_VIRT_VARS.fd`:

```
cmake -S . -B build-riscv64 -DTCOS_ARCH=riscv64 -G Ninja
cmake --build build-riscv64 --target run   # QEMU "virt"; the log appears on the serial console in your terminal
```

## Debugging in QEMU

`scripts/gdb-proxy.py` rebuilds `BOOTX64`, starts QEMU frozen at reset with a gdbstub on :2159 and bridges gdb to it. It
rewrites software breakpoints to hardware ones, so breakpoints set before OVMF has loaded the image still work. OS output
appears on gdb's stderr and in `<build>/debugcon.log`.

```sh
gdb -ex 'symbol-file build/BOOTX64.efi' -ex 'target remote | scripts/gdb-proxy.py' \
    -ex 'break kernel_main' -ex continue
```

## CLion

`.run/Debug TheoryCraftOS.run.xml` is a *GDB Remote Debug* configuration using the command above. Select **Debug
TheoryCraftOS** in the run dropdown (not the auto-generated `BOOTX64` entry, which tries to run the `.efi` natively) and
press Debug. QEMU opens and the debugger is paused at reset; press Resume. Use the **QEMU** configuration to just run the OS.
Set the debugger to GDB (the bundled one works). If the symbol path is wrong for your build directory, edit it in the
configuration.

## License

GNU Affero General Public License v3.0 or later - see [LICENSE](LICENSE). Every source file carries the standard notice.
