# AGENTS.md - TheoryCraftOS Quick Reference

## Architecture

**Mega-Kernel Design**: Almost everything runs in kernel space (drivers, filesystems, network stack, audio/video codecs). This is deliberate, not a mistake. User mode is minimal.

**Target Platforms**: x86_64 (primary), aarch64 (UEFI), riscv64 (UEFI with ELF->PE conversion via `scripts/elf2efi.py`)

## Build & Run

```bash
# Configure
cmake -B build -DTCOS_ARCH=x86_64

# Build
cmake --build build --target BOOTX64

# Run (QEMU with 4 cores, debug build)
cmake --build build --target run
```

**Arch-specific builds**:
- `TCOS_ARCH=aarch64` → AArch64 UEFI image
- `TCOS_ARCH=riscv64` → RISC-V UEFI ELF (rewritten as PE32+)

## Key Scripts

- `scripts/gdb-proxy.py` - GDB stub bridge (hardware breakpoints, QEMU frozen at reset)
- `scripts/elf2efi.py` - RISC-V ELF → PE32+ conversion
- `scripts/make-primary-image.sh` - Creates FAT32 primary image with Windowslibs
- `scripts/qemu-debug.sh` - Start QEMU with debug ready

## Filesystem Layout

| Path | Description |
|------|-------------|
| `/` | Root (primary partition) |
| `/EFI` | EFI system partition (mounted separately) |
| `/Desktop/Downloads, Documents, Pictures, Videos, Audio` | User files |
| `/Windows` | Windows process root (`C:\`) |
| `/Linux` | Linux process root (`/`) |
| `/AppStore/<App>/` | Per-application data |

Use `filesystem::layout` constants, not string literals.

## Concurrency

- Use `Concurrent*` containers first; add lock-free only where profiling demands it
- Interrupt handlers use `hardware::Mutex` (no sleeping, spin-only)
- Test concurrent code with `tests/run_host_tests.sh <TestName>`

## Style & Conventions

- C++20, `-fno-exceptions -fno-rtti -ffreestanding`
- 64-byte (cache line) alignment for hot structures
- Kernel threads in one address space, user mode minimal

## Debugging

```bash
# GDB with hardware breakpoint support
gdb -ex 'symbol-file build/BOOTX64.efi' \
    -ex 'target remote \| scripts/gdb-proxy.py' \
    -ex 'break kernel_main' -ex continue
```

CLion: Use `.run/Debug TheoryCraftOS.run.xml` config, not the auto-generated BOOTX64 entry.
