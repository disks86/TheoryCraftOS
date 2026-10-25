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
- a JavaScript engine, an HTML/CSS/DOM web browser model (with a WebAssembly runtime, service workers and the Cache API) and a regular expression engine
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

Host tests build with ASan and UBSan and run with `tests/run_host_tests.sh <TestName>`.

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
