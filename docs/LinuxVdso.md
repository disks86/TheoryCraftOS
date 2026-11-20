# The Linux vDSO

Linux programs read the clock very often (event loops, timers, frame pacing, `performance.now()` in a browser). Without a vDSO every read was a
system call, and every system call takes the process-wide lock (`task->mutex` in `on_syscall`, src/Kernel/UserMode.cpp), so under load a clock read
cost microseconds. Every x86-64 Linux process now gets a vDSO, a small shared object the C library finds through the auxiliary vector, whose
`clock_gettime`, `gettimeofday`, `time` and `clock_getres` run entirely in user mode. Windows programs got the same fix earlier for
`QueryPerformanceCounter` (the timestamp counter's rate and start in KUSER_SHARED_DATA, `tcos_user_interrupt_time` in src/Windows/tcos_win.h).

## What a process sees

```
  kLinuxVdsoHint (0x7FFFF7FF0000, or the next free room)
  +-------------------------+ <- data page (vvar): read-only, struct tcos_vdso_data
  | magic, version, flags,  |
  | tsc_ticks_per_us,       |
  | tsc_at_start,           |
  | realtime_offset_ns      |
  +-------------------------+ <- AT_SYSINFO_EHDR: the vDSO image, read + execute
  | ELF header ... .text    |    linux-vdso.so.1, version LINUX_2.6
  +-------------------------+
```

- **Exports** (version `LINUX_2.6`, the names and version glibc and musl ask for): `__vdso_clock_gettime`, `__vdso_gettimeofday`,
  `__vdso_time`, `__vdso_clock_getres`, and the plain names `clock_gettime`, `gettimeofday`, `time`, `clock_getres` as weak aliases (as Linux).
  Both `DT_HASH` (musl needs it) and `DT_GNU_HASH` (glibc prefers it) are there, with `DT_VERSYM`/`DT_VERDEF`. A note carries the Linux version
  6.1.0, what `uname` reports. There is no `__vdso_getcpu`: glibc then makes the `getcpu` system call.
- **The time.** `CLOCK_MONOTONIC = (TSC - tsc_at_start) / tsc_ticks_per_us * 1000`: whole microseconds, exactly what the kernel's own
  `monotonic_ns()` returns (`hardware::uptime_us() * 1000`), so a vDSO read and a system call never disagree beyond the microsecond that passes
  between them. `CLOCK_REALTIME = CLOCK_MONOTONIC + realtime_offset_ns` (modulo 2^64), which is how `ProgramHost::realtime_ns()` is defined.
  `MONOTONIC_RAW`, `MONOTONIC_COARSE` and `BOOTTIME` are the monotonic clock, `REALTIME_COARSE` the realtime clock and `TAI` realtime + 37 s,
  as the system call answers. A counter a little behind the start (another core's) reads as 0, never as a time far in the future.
- **What goes to the kernel.** The vDSO makes the very system call the C library would have made when no rate is published (`tsc_ticks_per_us`
  0), for the CPU-time clocks (`CLOCK_PROCESS_CPUTIME_ID`, `CLOCK_THREAD_CPUTIME_ID`) and any other or dynamic clock id (EINVAL comes from the
  kernel), for `gettimeofday` with a time zone pointer, and for `clock_getres` of an id outside 0..11. `clock_getres` of 0..11 answers 1 ns, as the
  system call does.
- **Bad pointers** fault in the program (as on Linux) instead of returning EFAULT.

## Where it is made

| Piece | What it does |
|---|---|
| `src/Process/LinuxVdso/vdso_data.h` | The data page's layout and the formula (`tcos_vdso_clock_ns`). C and C++: the vDSO and the kernel both include it. |
| `src/Process/LinuxVdso/vdso_x86_64.c`, `vdso_x86_64.lds` | The vDSO itself (freestanding C, `rdtsc` + the formula, `syscall` fallback) and its layout (one R+X segment from address 0, `tcos_vvar` at -4096, version script). |
| `tools/build_linux_vdso.sh` | Builds it with clang + GNU ld and writes `src/Process/LinuxVdso/LinuxVdsoImage.inc` (checked in; refuses an image with relocations). Run it after changing any of the three files above and commit the `.inc`. |
| `src/Process/LinuxVdso.{hpp,cpp}` | `linux_vdso_image(arch)`, `fill_linux_vdso_data`, `map_linux_vdso` (maps data page + image, fills the data from the host), `refresh_linux_vdso_data`. |
| `IProcessHost::user_clock` (src/Process/Process.hpp) | Optional: a host promises its clocks follow the formula and hands over rate, start and offset. `ProgramHost` (the kernel's) does on x86-64 once the counter is calibrated; test hosts and `ConsoleHost` do not, so their programs' vDSO makes system calls. |
| `UserProgram::load` / `fork_from` | Maps the vDSO for every x86-64 ELF (static or dynamic) after the stack; `build_linux_stack` puts `AT_SYSINFO_EHDR` (33) in the auxiliary vector (`AT_IGNORE` when there is none). A fork's copy has its data page rewritten from the child's host. No room is not a failure: the program then runs without a vDSO. |

The data is written once before the program's first instruction and never changes: the counter's rate and origin are fixed at boot and the wall
clock's offset is fixed per host. If the system ever sets the wall clock while programs run, the offset needs a seqlock-style update (a sequence
word in `flags`' place, read twice by the vDSO); nothing does that today.

## Tests

- `tests/run_host_tests.sh LinuxVdsoTest`: the image's layout (one R+X load at 0, no relocations or `DT_NEEDED`, soname, note); every export found
  the way musl looks (`DT_HASH` + version check, re-implemented from musl's `__vdsosym`) and the way glibc looks (GNU hash, bloom filter, version
  hash and name), and nothing else found; damaged and truncated copies; the formula against the kernel's clocks for many rates, origins and counter
  values; the data page bytes; `map_linux_vdso` (protections, data, a second mapping, refresh, no room, other CPUs); `UserProgram::load` puts
  `AT_SYSINFO_EHDR` on the stack (and not for AArch64); and **the vDSO's own code run on the host** over a data page the test fills (a made-up rate,
  so only the vDSO could have produced the answers), plus its fallbacks, which then reach this machine's Linux.
- `tests/run_host_tests.sh ProgramHostTest`: `ProgramHost::user_clock` keeps `realtime_ns() == monotonic_ns() + offset`.
- `tests/vdso_qemu.sh`: `tests/linux/vdsotest.c` on the real kernel, static and dynamic glibc (and first on this machine's Linux). It finds the vDSO
  through `AT_SYSINFO_EHDR`, looks the functions up itself, checks that they and glibc's agree with the system calls read just before and after (main
  thread, a second thread, a forked child), that `CLOCK_MONOTONIC` never goes back, the fallbacks, that the TheoryCraftOS data page publishes a rate,
  and that `clock_gettime` is at least four times cheaper than the bare system call.

## Not done

- **x86-64 only.** AArch64, RISC-V and LoongArch programs run on the emulator and get no vDSO (their signal handlers still return through the
  per-process trampoline page, see LinuxSyscalls.md). A vDSO for them would read the generic counter (`cntvct_el0`, `rdtime`) and would also give them
  `__kernel_rt_sigreturn`.
- **musl is not run end to end**: there is no musl toolchain on the development machine. The host test re-implements musl's lookup and finds every
  function through it.
- No `__vdso_getcpu`, no time namespaces, no `CLOCK_*_ALARM`; the clocks have microsecond resolution (the kernel's own), although `clock_getres`
  says 1 ns.
- The data page is mapped per process (40 bytes used of 4096); a single shared page would do once something needs to update it.
