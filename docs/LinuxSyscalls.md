# Linux system calls

What the kernel answers when a Linux program makes a system call. The table that maps numbers to calls is in
`src/Process/LinuxSyscalls.cpp` (x86-64 has its own numbers; AArch64, RISC-V and LoongArch share the generic ones). A number that is
not in the table answers `ENOSYS` and is reported to the host (`IProcessHost::unhandled_syscall`), which is how to find out what a
program still misses.

| Area | Source | Calls |
| --- | --- | --- |
| Files | `LinuxFiles.cpp` | open, openat, openat2, creat, close, close_range, read, write, pread, pwrite, readv, writev, preadv, pwritev, preadv2, pwritev2, lseek, stat family, statx, getdents, getdents64, mkdir, mknod, rmdir, unlink, rename, renameat2, link, symlink, readlink, truncate, fsync, statfs, flock, fcntl, dup, sendfile, copy_file_range, ioctl, memfd_create, fallocate |
| Memory | `LinuxSyscalls.cpp` | brk, mmap, munmap, mprotect, mremap, madvise, msync, mincore, mlock family |
| Threads | `LinuxThreads.cpp` | clone, clone3 (threads), futex, set_tid_address, gettid, exit, sched_getaffinity |
| Processes | `LinuxProcess.cpp`, `LinuxExec.cpp` | fork, vfork, clone and clone3 (processes), execve, execveat, wait4, waitid, exit_group, the uid/gid/group calls, getpgid, setpgid, getsid, setsid, capget, capset, getrlimit, setrlimit, prlimit64, getrusage, times, sysinfo, getpriority, setpriority, the sched_* calls, getcpu, personality, prctl, membarrier, robust lists |
| Signals | `LinuxSignals.cpp` | rt_sigaction, rt_sigprocmask, rt_sigpending, rt_sigtimedwait, rt_sigsuspend, pause, sigaltstack, kill, tkill, tgkill, rt_sigqueueinfo, alarm, getitimer, setitimer |
| Sockets and events | `LinuxSockets.cpp` | AF_UNIX sockets, pipes, eventfd, timerfd, signalfd, epoll, poll, select |
| Time | `LinuxSyscalls.cpp` | clock_gettime, clock_getres, clock_nanosleep, nanosleep, gettimeofday, time |

## How some of them behave here

**fork.** The child is a full copy made at the call: every private page that has memory is copied (there is no copy-on-write), shared
memory (a `memfd` view) stays shared, the descriptors refer to the same open files. `vfork` and `clone` without `CLONE_THREAD` (what
`posix_spawn` calls, with `CLONE_VM | CLONE_VFORK`) are the same copy: the child does not run in the parent's memory and the parent does
not wait for it. A program that depends on either (a child that reports an `exec` failure through a shared variable) sees the child
exit with its error code instead. The copying is done by whoever runs the program once the call is over:
`UserProgram::fork_from` for the portable part, `Task::start_fork` in the kernel, `EmulatedProcess::start_fork` in the emulator.

**exec in a child.** `execve` starts the new program as a process of its own and the caller waits for it and ends with its code
(`LinuxExec.hpp`). So after `fork` + `exec` the id the parent waits for is still the right one, and ending that process ends the
program it became.

**wait.** The parent keeps a record of each child until `wait4` or `waitid` has reported it. Nothing wakes a waiting parent: it looks
again every 2 ms. `SIGCHLD` is not delivered. Children never stop or continue, so `WUNTRACED` and `WCONTINUED` report nothing.

**Signals.** The kernel keeps handlers, masks and the alternate stack and gives them back, and carries out what needs no handler: a
signal whose action is the default ends the process (exit code 128 + signal, reported to a waiting parent as "killed by signal"), an
ignored one is dropped, a blocked one waits. It does **not** interrupt a thread to run a handler. A signal that has a handler stays
pending, where `sigpending`, `sigtimedwait` (`sigwait`) and `sigsuspend` find it; `sigsuspend` and `pause` return `EINTR` for it as if
the handler had run. Signals come from the process itself (`kill`, `tgkill`, `raise`, `abort`) and from its timers (`alarm`,
`setitimer`), which are looked at whenever the process makes a system call. `kill` of another process ends it if the signal's default
action would; nothing else can be sent across.

**Ids.** Every program starts as root. The uid, gid and group calls keep what a program sets and read it back; nothing checks an id
afterwards. Process groups and sessions are kept per process; there is no job control.

**Record locks.** `fcntl` `F_SETLK`, `F_SETLKW`, `F_GETLK` and the `F_OFD_*` forms follow the POSIX rules for ranges (a new lock replaces
the owner's old ones where they overlap, an unlock can split one) and are seen by every process. A process's locks go when it ends, not
when it closes one of several descriptors of the file. `flock` locks and record locks do not see each other, as on Linux.

**Memory.** `mremap` grows in place when the pages behind are free and otherwise moves the mapping (`MREMAP_MAYMOVE`); `MREMAP_FIXED`
and `MREMAP_DONTUNMAP` are refused, and a view of shared memory cannot grow. `madvise(MADV_DONTNEED)` makes anonymous private pages read
as zeros again; copies of files and shared views keep their bytes. Nothing is ever paged out, so `mlock` only checks the range and
`mincore` tells which pages have memory.

**Refused on purpose.** `chroot`, `mount`, `umount2`, `pivot_root`, `swapon`, `swapoff`, `reboot`, `sethostname`, `setdomainname`,
`syslog`, `ptrace`, `settimeofday`, `clock_settime`, `adjtimex`, `unshare`, `setns` and `acct` answer `EPERM`. The extended-attribute
calls answer `EOPNOTSUPP` (no file system here keeps any). `splice`, `tee` and `vmsplice` answer `EINVAL`, which makes programs fall
back to `read` and `write`.

## Not there yet

- Running signal handlers (delivery to a thread, `rt_sigreturn`), `SIGCHLD`, `SIGSEGV` handlers, POSIX timers (`timer_create`).
- Copy-on-write for `fork`, and a `vfork` child that shares its parent's memory.
- `inotify`, `fanotify`, `io_uring`, the `aio` calls, System V IPC (`shmget`, `semget`, `msgget`), POSIX message queues, `pidfd`,
  `seccomp`, `bpf`, `perf_event_open`, `userfaultfd`, namespaces.
- A shared mapping of a regular file that is written through (`MAP_SHARED` with `PROT_WRITE`), and anonymous shared memory that stays
  shared across `fork`.

## Tests

- `tests/run_host_tests.sh LinuxSyscallTest`: the calls one by one, with bad pointers and sizes.
- `tests/run_host_tests.sh LinuxForkTest`: `tests/linux/forktest.c` (no libc) under the emulator, compared with what the same
  program prints on the build machine's own Linux kernel.
- `tests/fork_qemu.sh`: the same program on the real kernel under QEMU.
