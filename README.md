# TheoryCraftOS

A minimal x86_64 UEFI operating system written in C++20 with a little
assembly (`src/arch/x86_64/cpu.S`). It boots under OVMF in QEMU, prints to
the UEFI console and the QEMU debug console (port 0x402, shown on your terminal), then shuts down on a key.

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

```sh
cmake -S . -B build -G Ninja -DTCOS_DEBUG_WAIT_SECS=10
cmake --build build --target debug   # terminal 1: QEMU + gdbstub on :1234
gdb -x scripts/tcos.gdb              # terminal 2
```

The OS logs its (firmware-chosen) load address to `build/debugcon.log` and
waits `TCOS_DEBUG_WAIT_SECS` seconds (or until `g_debugger_attached` is set). `scripts/tcos.gdb` reads the base,
loads symbols at the right offset, releases the spin and stops at
`kernel_main`. `--target debug-halted` additionally freezes the CPU at reset.

## CLion

1. Open the folder; enable the `uefi-debug` CMake preset (Settings > Build > CMake).
   It pins the cross toolchain, build dir `build/` and a 10 s boot-time debugger wait.
   Set Settings > Build > Debugger to the system GDB (not bundled LLDB).
2. **Run | Edit Configurations**, add:
   - *Shell Script* `QEMU`: script `scripts/qemu-debug.sh`, working dir = project root,
     "Execute in terminal" off. Its tab shows the OS output.
   - *Tools > External Tools* entry `wait-for-os`: program `scripts/wait-for-debug.sh`.
   - *GDB Remote Debug* `Attach`: debugger = system GDB, `'target remote' args` =
     `localhost:1234`, symbol file = `build/BOOTX64.efi`, path mappings none.
     Remove the default *Build* before-launch step and add *Run External tool > wait-for-os*.
   - *Compound* `Debug OS` containing `QEMU` and `Attach`.
   Do not run or debug the auto-generated `BOOTX64` CMake target directly: CLion
   tries to execute the `.efi` natively ("You can't do that when your target is
   `native'"). Delete it or ignore it.
3. Set breakpoints and press Debug on `Debug OS`. QEMU's window opens, the OS
   prints its output in the `QEMU` tab, and execution stops at your breakpoints.

The wait step matters: breakpoints set before the firmware has loaded the image
would be written into memory that is later overwritten.
