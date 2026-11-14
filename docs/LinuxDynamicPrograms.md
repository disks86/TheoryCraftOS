# Dynamically linked Linux programs

A Linux program built against a C library's shared objects (glibc's `libc.so.6`, `libwayland-client.so.0`, ...) runs the way it does on Linux. The
kernel does what Linux's `execve` does for such a program, and the program's own dynamic loader (`ld.so`) does the rest.

1. **The kernel maps the program and its interpreter.** `UserProgram::load` sees the `PT_INTERP` segment and reads the file it names from the Linux tree,
   following symbolic links on the way and at the end (`/lib64/ld-linux-x86-64.so.2` is a link on most distributions). The file is read from `/Linux`
   (`filesystem::layout::kLinux`). `load_dynamic_elf` (`src/Process/ImageLoader.cpp`) maps the program as a static one is mapped, a PIE at
   `kElfPieBase`. It then maps the interpreter, which must be a shared object for the same CPU and must not ask for an interpreter itself, at
   `kElfInterpreterBase` (0x7E0000000000). That place is below the `mmap` area, so the libraries never land on it.
2. **The thread starts in the interpreter.** The auxiliary vector tells it where everything is:
   - `AT_BASE` is the interpreter's address.
   - `AT_ENTRY` is the program's own entry point.
   - `AT_PHDR`, `AT_PHENT` and `AT_PHNUM` describe the program's program headers.
3. **`ld.so` loads the libraries itself**, with `openat`, `read`, `fstat` and `mmap` of the files, then `mprotect`. It looks in its built-in system
   directories (`/usr/lib` and the like); the system has no `/etc/ld.so.cache`, which `ld.so` then does without. `dlopen` works the same way later.

## What the system calls do for it

| Call | What it does here |
|------|-------------------|
| `mmap` of a regular file | A private copy of the file made at the time of the call. Pages past the end of the file are zero. `MAP_FIXED` over part of an earlier mapping works, which is how `ld.so` places each segment. A shared mapping that is only read is the same copy; a shared mapping for writing is `ENODEV`. A descriptor opened without read access is `EACCES`. |
| `MAP_FIXED_NOREPLACE`, `MAP_STACK` | Supported. `MAP_STACK` (0x20000) used to be refused, which broke every `pthread_create`. |
| `clone3` | What glibc tries first. It makes the same threads as `clone`. The `pidfd` field is ignored unless `CLONE_PIDFD` is set, as Linux does. It refuses (`EINVAL`) a pidfd, an exit signal (that is fork), `set_tid` and a cgroup. A larger structure is accepted while its unknown fields are zero (`E2BIG` otherwise). |
| `clone` | `CLONE_SYSVSEM` is 0x40000. It was defined as 0x40000000 (`CLONE_NEWNET`), which made glibc's thread flags `ENOSYS`. |

**Threads in the kernel.** `Task::start_linux_thread` (`src/Kernel/UserMode.cpp`) starts the child as a copy of the calling thread's registers, with
0 as the call's result. The copy matters because glibc's `clone3` path keeps the function and its argument in registers. The child also gets the stack
it was given and its own `fs` base under `CLONE_SETTLS`. The emulator already did the same (`EmulatedProcess`).

**The emulated CPU.** Two changes were needed for glibc on the emulated x86-64 CPU:
- **CPUID vendor.** The CPU now reports the vendor `AuthenticAMD`, with the x87 FPU bit and the L1/L2/L3 cache leaves (0x80000005, 0x80000006) and
  address sizes (0x80000008). glibc's `ld.so` reads the feature leaves only for vendors it knows; with the old vendor string it saw no features and
  refused every library ("CPU ISA level is lower than required").
- **`vldmxcsr` and `vstmxcsr`.** The VEX forms of these two instructions now run (libm uses them).

## Installing a program

A dynamically linked program needs its interpreter and libraries in `/Linux` at the paths it was built for: `/Linux/lib64/ld-linux-x86-64.so.2`,
`/Linux/usr/lib/libc.so.6`, and so on (`ldd` lists them). The disk image tool puts them there:
`build/tools/primary_image <image> file:/Linux/usr/lib/libc.so.6=/usr/lib/libc.so.6 ...`. A program whose interpreter is missing fails to start
with a message that names the interpreter.

## Tests

- `tests/run_host_tests.sh LoaderTest`:
  - `test_load_dynamic_elf`: mapping, the auxiliary vector, interpreters that cannot be, and a fixed-address program in the interpreter's place;
    600 truncated or bit-flipped programs and interpreters under ASan and UBSan.
  - `test_dynamic_user_program`: a missing interpreter, a symlinked `/lib64` and a symlinked interpreter, links that go round, a file that is not ELF,
    and every `mmap` case above.
  - `test_clone3`.
- `tests/linux_dynamic_emulated.sh` uses the development machine's glibc on the kernel's CPU emulator:
  - `tests/linux/dynamictest.c`: stdio, libm, malloc, four threads with thread-local storage, a constructor, `dlopen`/`dlsym`/`dlclose`.
    21 checks.
  - `tests/wayland/wl_client.c`: libwayland-client, libxkbcommon and libffi against the kernel's compositor. 24 checks; the window reaches the
    desktop.
- **QEMU, the real kernel:** both programs were run with the autorun trick (`TCOS_AUTORUN`, see the Wayland memory notes), with the host's `ld.so`,
  `libc.so.6`, `libm.so.6`, `libwayland-client.so.0`, `libxkbcommon.so.0` and `libffi.so.8` added to a copy of `primary.img`. dynamictest passed
  21/21 with native threads; wl_client passed 24/24 (`--parallel --screen=any`).

## /proc/self and links

There is no /proc file system, but the links programs read to find themselves are answered (src/Process/LinuxFiles.cpp): `/proc/self/exe`
(the program file, links resolved: what Rust's `current_exe`, glibc's `$ORIGIN` and AppImage launchers like sharun use), `/proc/self/cwd`,
`/proc/self/root` and `/proc/self/fd/<n>`, also as `/proc/<own pid>/...` and `/proc/thread-self/...`. Opening or `stat`ing a path through
`exe`, `cwd` or `root` reaches the file itself. `readlink` returns the targets of the file tree's symbolic links, and `lstat`, `newfstatat`
and `statx` with `AT_SYMLINK_NOFOLLOW` report links as links. Tests: `ProcessFilesTest` (links, /proc/self, execve with a recording runtime)
and `tests/exec_qemu.sh` (tests/linux/exectest.c on the real kernel: readlink, lstat, an exec of itself with a kept and a close-on-exec
descriptor, the exit code coming back).

## /bin/sh

The Linux tree's `/bin/sh` and `/bin/bash` are markers (a first line of `#!tcos-builtin-shell`, made by `prepare_program_folders` only where
nothing is), not programs. `execve` of one with a script (`sh [options] script args`, or a `#!/bin/sh` script) runs the script with the OS's own
shell (src/Program/AppRunScript.hpp, installed by `desktop::install_linux_builtin_shell` at boot), then execs the program the script finally starts;
a script that starts nothing ends the program with 0. A real shell binary (named `sh`, `bash`, `dash` or `ash`) asked to run a script gets the
same treatment, since without `fork` it could not get past its first `$( )`; with `-c` or interactively it runs as itself. `$( )` and `( )` are
subshells in the built-in shell: what they change of the variables, `"$@"` and the folder stays in them.

The file systems keep no Unix permissions, so `stat` reports every file as executable (0755, or 0555 read-only), as Linux shows a FAT volume with
the default masks and as `access(X_OK)` already answered. Launchers check the bit before they run a program or a shell (sharun's `is_exe`).

## Not done

- **x86-64 only in practice.** The loader is not specific to x86-64, but the development machine has no AArch64 or RISC-V C library to test with.
  AArch64 and RISC-V programs run only on the emulator, where `clone3` is shared code.
- **File mappings are copies.** Pages are not shared between processes or with the file: a library mapped by ten programs takes ten copies, and a
  file written after it was mapped is not seen through the mapping. Shared writable file mappings are not supported.
- **No vDSO** (`AT_SYSINFO_EHDR`): glibc makes real system calls for `clock_gettime` and `gettimeofday`.
- **No `/etc/ld.so.cache`, no `ldconfig`.** Only the interpreter's built-in directories, `LD_LIBRARY_PATH` and `RUNPATH` are searched.
- **`execve` starts a new process.** `execve`/`execveat` (src/Process/LinuxExec.cpp) start the new image as a child process with exactly the
  arguments and environment given, the current folder and every descriptor that is not close-on-exec; the old program closes what it had,
  stops its other threads, waits and ends with the new program's code. So the process id changes across an exec, and a parent waiting on the
  old id sees the new program's result. `#!` scripts run their interpreter. There is no `fork`, `vfork` or `wait4`.
- **Not tried yet:** a whole distribution's programs (GTK applications, foot) inside the OS. They need more libraries in the image and more system
  calls, which will show up as they are tried.
