# Linux system calls

What the kernel answers when a Linux program makes a system call. The table that maps numbers to calls is in
`src/Process/LinuxSyscalls.cpp` (x86-64 has its own numbers; AArch64, RISC-V and LoongArch share the generic ones). A number that is
not in the table answers `ENOSYS` and is reported to the host (`IProcessHost::unhandled_syscall`).

Every call Linux has is in the table (386 on x86-64, 328 on the other CPUs, as of Linux 6.18; `LinuxSyscallTest` checks it): either it
is implemented, or it has the answer a Linux kernel gives when that part is not there (see *Fixed answers* below). So an unhandled
number means a call newer than this list.

| Area | Source | Calls |
| --- | --- | --- |
| Files | `LinuxFiles.cpp` | open, openat, openat2, creat, close, close_range, read, write, pread, pwrite, readv, writev, preadv, pwritev, preadv2, pwritev2, lseek, stat family, statx, getdents, getdents64, mkdir, mknod, rmdir, unlink, rename, renameat2, link, symlink, readlink, truncate, fsync, statfs, flock, fcntl, dup, sendfile, copy_file_range, ioctl, memfd_create, fallocate |
| Memory | `LinuxSyscalls.cpp`, `LinuxMisc.cpp` | brk, mmap, munmap, mprotect, pkey_mprotect, mremap, madvise, process_madvise, msync, mincore, mlock family, get_mempolicy, set_mempolicy, mbind, set_mempolicy_home_node, migrate_pages, move_pages, process_vm_readv, process_vm_writev |
| Threads | `LinuxThreads.cpp` | clone, clone3 (threads), futex, futex_waitv, futex_wake, futex_wait, futex_requeue, set_tid_address, gettid, exit, sched_getaffinity |
| Processes | `LinuxProcess.cpp`, `LinuxExec.cpp` | fork, vfork, clone and clone3 (processes), execve, execveat, wait4, waitid, exit_group, the uid/gid/group calls, getpgid, setpgid, getsid, setsid, capget, capset, getrlimit, setrlimit, prlimit64, getrusage, times, sysinfo, getpriority, setpriority, the sched_* calls, getcpu, personality, prctl, membarrier, robust lists, kcmp, pidfd_open, pidfd_send_signal, pidfd_getfd, process_mrelease |
| Signals | `LinuxSignals.cpp` | rt_sigaction, rt_sigprocmask, rt_sigpending, rt_sigtimedwait, rt_sigsuspend, pause, sigaltstack, kill, tkill, tgkill, rt_sigqueueinfo, alarm, getitimer, setitimer, timer_create, timer_settime, timer_gettime, timer_getoverrun, timer_delete |
| Sockets and events | `LinuxSockets.cpp` | AF_UNIX sockets, pipes, eventfd, timerfd, signalfd, epoll, poll, select |
| Time | `LinuxSyscalls.cpp` | clock_gettime, clock_getres, clock_nanosleep, nanosleep, gettimeofday, time |
| System V IPC | `LinuxIpc.cpp` | shmget, shmat, shmdt, shmctl, semget, semop, semtimedop, semctl, msgget, msgsnd, msgrcv, msgctl |
| POSIX message queues | `LinuxIpc.cpp` | mq_open, mq_unlink, mq_timedsend, mq_timedreceive, mq_notify, mq_getsetattr |
| Asynchronous I/O | `LinuxAio.cpp` | io_setup, io_destroy, io_submit, io_cancel, io_getevents, io_pgetevents |
| File watching | `LinuxInotify.cpp` | inotify_init, inotify_init1, inotify_add_watch, inotify_rm_watch |

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

**wait.** The parent keeps a record of each child until `wait4` or `waitid` has reported it. A waiting parent looks again every 2 ms.
`SIGCHLD` is sent when a child ends (its `siginfo` does not say which: handlers call `waitpid`). Children never stop or continue, so
`WUNTRACED` and `WCONTINUED` report nothing.

**Signals.** The kernel keeps handlers, masks and the alternate stack, and carries signals out: a signal whose action is the default
ends the process (exit code 128 + signal, reported to a waiting parent as "killed by signal"), an ignored one is dropped, a blocked one
waits, and one with a handler has its handler entered.

- *Where handlers are entered.* When a thread is about to go back to user mode: after a system call, at a timer tick that found it in
  user mode (so a loop that never calls the kernel is interrupted too), and when the processor reports a fault. The handler gets the
  frame Linux gives it on its CPU (`siginfo_t`, `ucontext_t` with every register and the floating-point state) and returns through
  `rt_sigreturn`; it may change the registers it returns to. `SA_SIGINFO`, `SA_ONSTACK`, `SA_NODEFER`, `SA_RESETHAND` and `SA_RESTART`
  are honoured. On x86-64 the action must have a restorer (`SA_RESTORER`), which every C library supplies. AArch64, RISC-V and LoongArch
  C libraries give none (Linux lends them a few instructions in the vDSO): the process gets a page of its own with those
  instructions the first time a handler is entered.
- *Faults.* A page fault, a division error, an invalid instruction and the like become `SIGSEGV`, `SIGFPE`, `SIGILL`, `SIGBUS` or
  `SIGTRAP` with `si_code` and `si_addr`. Without a handler, or with the signal blocked, the process ends.
- *Where signals come from.* The process itself (`kill`, `tgkill`, `raise`, `abort`), its timers (`alarm`, `setitimer`), the
  processor, another process's `kill` (the target acts on it by its own settings) and a child that ended (`SIGCHLD`). `tgkill` and
  faults are for one thread; the rest is taken by whichever thread of the process looks first and does not block it.
- *Calls that wait.* A read on a pipe or socket, `futex`, `wait4`, `nanosleep`, `poll`, `epoll_wait`, a lock that is waited for and so
  on end with `EINTR` when a signal is there for the thread. With `SA_RESTART` the calls Linux restarts are started again after the
  handler. A read of the console and a wait for a priority-inheritance futex are not cut short.
- *Where this works.* For an x86-64 program on the kernel, and for x86-64, AArch64, RISC-V and LoongArch programs in the emulator, each
  with its own frame layout (`struct rt_sigframe` of that CPU: AArch64's with the `fpsimd_context` record and a frame record above it,
  RISC-V's with the double-precision registers, LoongArch's with the floating-point record and the record that ends the list).
  `rt_sigreturn` on LoongArch also takes the LSX and LASX records Linux writes for a thread that used the vector registers, of which
  the emulator has none.

The code is `LinuxSignals.cpp` (`deliver_signals`, `signal_return`, `deliver_fault`); the kernel calls it from `on_syscall`,
`on_user_tick` / `on_user_fault` in `Kernel/UserMode.cpp`, the emulator from `EmulatedProcess::thread_loop`.

**Ids.** Every program starts as root. The uid, gid and group calls keep what a program sets and read it back; nothing checks an id
afterwards. Process groups and sessions are kept per process; there is no job control.

**Record locks.** `fcntl` `F_SETLK`, `F_SETLKW`, `F_GETLK` and the `F_OFD_*` forms follow the POSIX rules for ranges (a new lock replaces
the owner's old ones where they overlap, an unlock can split one) and are seen by every process. A process's locks go when it ends, not
when it closes one of several descriptors of the file. `flock` locks and record locks do not see each other, as on Linux.

**Memory.** `mremap` grows in place when the pages behind are free and otherwise moves the mapping (`MREMAP_MAYMOVE`); `MREMAP_FIXED`
and `MREMAP_DONTUNMAP` are refused, and a view of shared memory cannot grow. `madvise(MADV_DONTNEED)` makes anonymous private pages read
as zeros again; copies of files and shared views keep their bytes. Nothing is ever paged out, so `mlock` only checks the range and
`mincore` tells which pages have memory.

**POSIX timers.** `timer_create` with `SIGEV_SIGNAL`, `SIGEV_THREAD_ID` (what the C library's `SIGEV_THREAD` asks for) and `SIGEV_NONE`;
up to 32 in a process. A timer's signal carries `SI_TIMER`, the timer's id, its overrun and its value. Every clock ticks alike here (the
CPU-time clocks too), and a timer is looked at where the interval timers are: at a system call, a timer tick, or when a wait ends.

**Futexes.** `FUTEX_WAKE_OP` and the newer calls: `futex_waitv` (a wait for any of up to 128 words, what Wine and Proton use),
`futex_wake`, `futex_wait`, `futex_requeue`. The requeue-PI pair of the old call answers `ENOSYS`.

**System V IPC.** Shared memory segments (up to 64 MiB each, 128 in the system), semaphore sets (128 sets of up to 128) and message
queues (64, 16 KiB each, messages of up to 8 KiB), found by key or id by every Linux process and kept until `IPC_RMID`. A removed
segment stays while processes have it attached, and can still be attached by id, as on Linux. `SEM_UNDO` is taken back when the
process ends. Nothing is checked against the permission bits (every program is root). A removed segment that a process still had
attached when it ended is freed at the next shared-memory call anyone makes.

**POSIX message queues.** Up to 32 named queues; a queue's descriptor works with `poll` and `epoll`. `mq_notify` with `SIGEV_SIGNAL`
and `SIGEV_NONE`; `SIGEV_THREAD`, which the C library does with a netlink socket, is refused.

**Asynchronous I/O.** The `io_*` calls (libaio; not the C library's `aio_read`, which uses threads and needs nothing). A request is
carried out when it is submitted and its result waits for `io_getevents`, which is what a program sees on Linux when the data is cached.
`IOCB_CMD_PREAD`, `PWRITE`, `PREADV`, `PWRITEV`, `FSYNC`, `FDSYNC`, `NOOP` and `IOCB_FLAG_RESFD`; `IOCB_CMD_POLL` is refused. Four
contexts in a process, 128 results waiting in each.

**pidfd.** `pidfd_open` for the caller, a child or any process there is; the descriptor turns readable when the process ends, takes
`waitid(P_PIDFD)` and `pidfd_send_signal`. `pidfd_getfd`, `process_madvise` and `process_vm_readv`/`writev` work on the caller itself and
answer `EPERM` for another process. `clone3` still refuses `CLONE_PIDFD`.

**inotify.** A watch is a change notification of the object manager on the watched folder; the Linux file calls report what they make,
remove, rename and write (and do no work for it while nobody watches). Told: `IN_CREATE`, `IN_DELETE`, `IN_MODIFY`, `IN_MOVED_FROM` /
`IN_MOVED_TO` with a cookie (also between two watched folders), `IN_DELETE_SELF`, `IN_MOVE_SELF`, `IN_IGNORED`, `IN_Q_OVERFLOW`,
`IN_ISDIR`; `IN_ONESHOT`, `IN_MASK_ADD`, `IN_MASK_CREATE` and `IN_ONLYDIR` are honoured. Not told: `IN_ACCESS`, `IN_OPEN`,
`IN_CLOSE_WRITE`, `IN_CLOSE_NOWRITE`, `IN_ATTRIB`, and changes made by Windows programs or the system itself. The events of one watch
come in the order things happened; those of several watches of one instance come watch by watch. 128 watches in the whole system, 32
instances.

**One memory node, no protection keys.** `get_mempolicy`, `set_mempolicy`, `mbind`, `move_pages` and `migrate_pages` accept every policy
and answer for a machine with one node. `pkey_alloc` answers `ENOSPC`, as on a processor without protection keys.

**Fixed answers.** Calls for parts of Linux that are not here answer as a kernel without that part does, so that programs take their
fallback:

- `ENOSYS`: `io_uring_*`, `fanotify_*`, `add_key`/`request_key`/`keyctl`, `landlock_*`, `lsm_*`, `memfd_secret`, `mseal`, `statmount`,
  `listmount`, `listns`, `seccomp`, `remap_file_pages`, `quotactl`, `quotactl_fd`, `bpf`, `perf_event_open`, `userfaultfd`, `rseq`,
  `rseq_slice_yield`, `sysfs`, `ustat`, `modify_ldt`, `set_thread_area`, `get_thread_area`, `uprobe`, `uretprobe`, and the numbers Linux
  itself answers `ENOSYS` for (`afs_syscall`, `getpmsg`, `putpmsg`, `security`, `tuxcall`, `vserver`, `create_module`,
  `get_kernel_syms`, `query_module`, `nfsservctl`, `_sysctl`, `uselib`, `epoll_ctl_old`, `epoll_wait_old`, `lookup_dcookie`).
- `EOPNOTSUPP`: `name_to_handle_at`, `open_by_handle_at`, the `*xattrat` calls, `file_getattr`, `file_setattr`, `map_shadow_stack`.
- `EPERM`: see below.

**Refused on purpose.** `chroot`, `mount`, `umount2`, `pivot_root`, `swapon`, `swapoff`, `reboot`, `sethostname`, `setdomainname`,
`syslog`, `ptrace`, `settimeofday`, `clock_settime`, `adjtimex`, `unshare`, `setns`, `acct`, `vhangup`, `iopl`, `ioperm`, the module
calls (`init_module`, `finit_module`, `delete_module`), `kexec_load`, `kexec_file_load`, `fchroot` and the newer mount calls
(`open_tree`, `move_mount`, `fsopen`, `fsconfig`, `fsmount`, `fspick`, `mount_setattr`, `open_tree_attr`) answer `EPERM`. The extended-attribute
calls answer `EOPNOTSUPP` (no file system here keeps any). `splice`, `tee` and `vmsplice` answer `EINVAL`, which makes programs fall
back to `read` and `write`.

## Not there yet

- The SVE, SME, RISC-V vector and LoongArch LSX / LASX registers in a handler's frame (only the general, NEON and floating-point
  registers are kept); queued real-time signals (signals of one kind do not queue, and `sigqueue`'s value is not kept); stopping and
  continuing (`SIGSTOP`, `SIGTSTP`, `SIGCONT` do nothing); Ctrl+C as `SIGINT` (it still ends the program); the AVX halves of the vector
  registers in a handler's frame; a `signalfd` that real signals reach (it is never readable).
- Copy-on-write for `fork`, and a `vfork` child that shares its parent's memory.
- The parts with a fixed answer above: `io_uring`, `fanotify`, `seccomp` filters, `bpf`, `perf_event_open`, `userfaultfd`, namespaces,
  restartable sequences, the key retention service, landlock, quotas, extended attributes, file handles.
- A shared mapping of a regular file that is written through (`MAP_SHARED` with `PROT_WRITE`).
- Sockets other than `AF_UNIX` for Linux programs; file permission bits and owners (the calls succeed and keep nothing).
- Of inotify: `IN_CLOSE_WRITE`, `IN_OPEN`, `IN_ACCESS`, `IN_ATTRIB`. Of asynchronous I/O: requests that would wait (`IOCB_CMD_POLL`).
  `CLONE_PIDFD`. `mq_notify` with `SIGEV_THREAD`. The requeue-PI futex operations.

## Tests

- `tests/run_host_tests.sh LinuxSyscallTest`: the calls one by one, with bad pointers and sizes, and that every call Linux has is in
  the table. (`TCOS_TEST_VERBOSE=1` names each test as it starts, for one that hangs.) The LoongArch signal frame is compared
  byte for byte with frames a LoongArch program was given under qemu's user mode (`tests/linux/sigframe_loongarch.c`, checked-in output,
  `tests/linux/build_sigframe_loongarch.sh`), and `rt_sigreturn` is given those frames.
- `tests/run_host_tests.sh LinuxForkTest`: `tests/linux/forktest.c` and `signaltest.c` (no libc) under the emulator, compared with
  what the same programs print on the build machine's own Linux kernel. `signaltest.c` is also run as an AArch64, a RISC-V and a LoongArch
  program (checked-in builds, `tests/linux/build_signaltest.sh`), compared with what those print under qemu's user mode.
- `tests/fork_qemu.sh`: those two and the glibc programs `spawntest.c`, `handlertest.c` and `ipctest.c` (System V IPC, POSIX message
  queues and timers, inotify, pidfd, asynchronous I/O, through the C library's own functions and structures) on the real kernel under
  QEMU, compared the same way; then `signaltest.c` as an AArch64, a RISC-V and a LoongArch program on the kernel's emulator.
  `NATIVE="ipctest" EMULATED="" tests/fork_qemu.sh` runs one.
