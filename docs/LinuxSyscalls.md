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
| Sockets and events | `LinuxSockets.cpp`, `LinuxInetSockets.cpp` | AF_UNIX, AF_INET and AF_INET6 sockets (socket, bind, connect, listen, accept, accept4, sendto, recvfrom, sendmsg, recvmsg, sendmmsg, recvmmsg, shutdown, getsockname, getpeername, setsockopt, getsockopt), pipes, eventfd, timerfd, signalfd, epoll, poll, select |
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
`SIGCHLD` is sent when a child ends; its `siginfo` names a child that has ended and was not waited for yet, and how it ended. Children never stop or continue, so
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
- *Blocked signals.* A signal that a thread blocks stays pending whatever its action is, also one that would be ignored (`SIGCHLD`,
  `SIGWINCH` by default): it is dropped only if it is still ignored when it is unblocked. That is what lets `sigwaitinfo` and a
  signalfd take such signals without a handler.
- *signalfd.* A read takes the signals of the descriptor's set that are pending for the calling thread or the process, as
  `struct signalfd_siginfo` records (who sent it, a timer's id, overrun and value, a fault's address, for `SIGCHLD` the child and how
  it ended); `poll` and `epoll` report it readable while there is one. A waiting read looks again every 2 ms.
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

**Network sockets.** `AF_INET` and `AF_INET6`, `SOCK_STREAM` (TCP) and `SOCK_DGRAM` (UDP), over the system's network stacks through the
socket manager that Windows programs' sockets use (`Sockets.hpp`); the loopback needs no interface's address.

- A call that would wait does so until the network has made progress, a signal arrives (`EINTR`) or the socket's `SO_RCVTIMEO` /
  `SO_SNDTIMEO` runs out (`EAGAIN`). `O_NONBLOCK`, `SOCK_NONBLOCK`, `FIONBIO` and `MSG_DONTWAIT` make it not wait.
- A `connect` that does not wait answers `EINPROGRESS`; the socket turns writable when the handshake is over and `SO_ERROR` says how it
  went. Nothing listening is `ECONNREFUSED` (an attempt that times out is reported the same way).
- An `AF_INET6` socket takes IPv4 too unless `IPV6_V6ONLY` is set, and shows IPv4 peers as `::ffff:a.b.c.d`, as on Linux.
- `shutdown(SHUT_WR)` ends what this side sends; what the other side still sends can be read. `poll`, `select` and `epoll` know these
  sockets (a wait looks at them every 2 ms). `dup` and `fork` share a socket.
- Options: `SO_REUSEADDR`, `SO_REUSEPORT`, `SO_KEEPALIVE`, `SO_BROADCAST`, `SO_LINGER`, `SO_SNDBUF`, `SO_RCVBUF`, `TCP_NODELAY` and the
  common `IP_*`, `IPV6_*` and `TCP_*` ones are kept and read back, and change nothing in the stack; `SO_TYPE`, `SO_DOMAIN`,
  `SO_PROTOCOL`, `SO_ERROR`, `SO_ACCEPTCONN`, the timeouts and `IPV6_V6ONLY` are real.
- `MSG_PEEK` (also of `AF_UNIX` sockets): the network stack hands data out once, so what is looked at is held with the socket until a
  read takes it, up to 64 KiB of a stream in one look, or one datagram, which the read then gets whole. `poll` and `FIONREAD` count
  what is held.
- Control messages of datagram sockets (`recvmsg`, `recvmmsg`), once the socket asked: `SO_TIMESTAMP` or `SO_TIMESTAMPNS` (the one
  set last), `IP_PKTINFO`, `IP_RECVTTL`, `IP_RECVTOS`, `IPV6_RECVPKTINFO`, `IPV6_RECVHOPLIMIT` and `IPV6_RECVTCLASS`, in Linux's order
  (the time first; through an IPv6 socket an IPv4 datagram gets `IPV6_PKTINFO` with its address mapped, then the `IP_*` ones). The
  network stack does not keep a packet's header, so the answers are worked out: the interface is 1 for the loopback and 2 onwards
  for the network interfaces, the destination is the loopback address or that interface's own address (a broadcast or multicast
  destination is not told apart), the TTL and the hop limit are 64, the TOS and the traffic class 0, and the time is that of the
  `recvmsg` call, not of the packet's arrival. A buffer too small gets what fits, the last message cut short as on Linux, and
  `MSG_CTRUNC`. `msg_flags` carries `MSG_TRUNC` for a datagram longer than the buffers, and `MSG_TRUNC` in the call's flags makes
  the result the datagram's real length (of a stream it throws the bytes away). `MSG_ERRQUEUE` finds the queue empty (`EAGAIN`).
- Control messages given to `sendmsg` are checked the way Linux checks them and then not acted on, so the datagram goes out with
  the usual source address, TTL and TOS: `IP_PKTINFO`, `IP_TTL`, `IP_TOS`, `IP_RETOPTS`, `IPV6_PKTINFO`, `IPV6_HOPLIMIT`,
  `IPV6_TCLASS`, `IPV6_DONTFRAG`, `SO_MARK`, `SO_TIMESTAMPING`, `SCM_TXTIME` and `UDP_SEGMENT` of the right size and range are
  taken, `SCM_RIGHTS`, `SCM_CREDENTIALS` and another family's messages are passed over, anything else of the socket's own levels or
  not whole is `EINVAL`, more than 20480 bytes `ENOBUFS`. (Descriptors and credentials travel over `AF_UNIX` sockets, see above.)
- Limits: a datagram has to fit one frame (1472 bytes over IPv4, `EMSGSIZE` beyond: the stack sends no fragments); `bind` uses every interface
  whatever address it names; no raw or ICMP sockets, no multicast membership; 256 sockets in the system.

**Netlink.** `socket(AF_NETLINK, SOCK_RAW or SOCK_DGRAM, ...)` is how a program asks the kernel about the network and hears of changes
to it (`getifaddrs`, `if_nameindex`, `ip addr`). The kernel's side is `LinuxNetlinkMessages.cpp` (requests in, datagrams out, knowing
nothing of processes); the sockets are `LinuxNetlink.cpp`. Up to 64 at once.

- The network as programs are shown it: the loopback is interface 1, `lo`; each network interface follows as `eth0`, `eth1`, ...
  from number 2 (the numbers `IP_PKTINFO` reports), with its hardware address, its IPv4 address and prefix, its IPv6 link-local and
  global addresses, and the routes these amount to (a default route by the gateway or router, the subnet or prefix on the link).
- `NETLINK_ROUTE`: `RTM_GETLINK`, `RTM_GETADDR` and `RTM_GETROUTE` as lists (`NLM_F_DUMP`, by family if one is named; several
  datagrams of `NLM_F_MULTI` messages and `NLMSG_DONE`), `RTM_GETLINK` for one interface by number or `IFLA_IFNAME`, and
  `RTM_GETROUTE` for one destination (`RTA_DST`: the longest matching prefix; the machine's own addresses are local). Other lists
  (neighbours, rules, ...) are empty. Anything that would change the network is `EPERM`, anything unknown `EOPNOTSUPP`. Errors and
  acknowledgements (`NLM_F_ACK`) come back as `NLMSG_ERROR` with the request, or its header alone with `NETLINK_CAP_ACK`.
- A socket bound to groups (`nl_groups`, `NETLINK_ADD_MEMBERSHIP`) is told of addresses, routes and interfaces that come and go:
  `RTMGRP_LINK`, `RTMGRP_IPV4_IFADDR`, `RTMGRP_IPV6_IFADDR`, `RTMGRP_IPV4_ROUTE`, `RTMGRP_IPV6_ROUTE`. Nothing announces such changes
  in this kernel, so they are found by comparing the network with what the socket was last told, whenever the socket is looked at: a
  `poll` or a read that waits looks again within 100 ms.
- `NETLINK_GENERIC` has the controller, which knows no family but itself (`CTRL_CMD_GETFAMILY` of anything else is `ENOENT`).
  `NETLINK_KOBJECT_UEVENT` can be bound and filtered (`SO_ATTACH_FILTER` is taken and not run) and never hears anything. Other
  protocols are `EPROTONOSUPPORT`.
- The socket calls: `bind` (an address of 0 gets the process's number, a second socket of the process one counted down from
  -4097; one in use is `EADDRINUSE`), `connect`, every way of sending and receiving, `MSG_PEEK`, `MSG_TRUNC`, `FIONREAD`,
  `SO_PASSCRED` (`SCM_CREDENTIALS`: the kernel is process 0 of user 0), `NETLINK_PKTINFO`, the buffer sizes and timeouts, and the
  `NETLINK_*` options kept and read back. Two sockets of one protocol can send to each other by address. A queue holds 512 KiB;
  what does not fit is lost and the next read says `ENOBUFS` once (unless `NETLINK_NO_ENOBUFS`). `listen`, `accept` and `shutdown`
  are `EOPNOTSUPP`.
- The interface ioctls of any socket answer from the same picture: `SIOCGIFNAME` and `SIOCGIFINDEX` (what `if_indextoname` and
  `if_nametoindex` are), `SIOCGIFFLAGS`, `SIOCGIFADDR`, `SIOCGIFNETMASK`, `SIOCGIFBRDADDR`, `SIOCGIFDSTADDR`, `SIOCGIFMTU`,
  `SIOCGIFHWADDR`, `SIOCGIFMETRIC`, `SIOCGIFTXQLEN` and `SIOCGIFCONF`; the ones that set something are `EPERM`.
- Limits: an interface is always shown up and running (there is no link state to tell); packet counts are zero; only the main
  routing table is listed (not the local one); `NETLINK_GET_STRICT_CHK` and `NETLINK_EXT_ACK` are kept and change nothing; no
  messages between the groups of two programs' sockets.

**Finding hosts by name.** A C library does that itself, from files and by asking a DNS server over UDP or TCP; what it needs here are
the files (`LinuxNetworkFiles.cpp`). `/etc/resolv.conf`, `/etc/hosts`, `/etc/nsswitch.conf`, `/etc/services` and `/etc/protocols` are
written into the program's tree (`/Linux/etc`) the first time a program names one of them, as ordinary files.

- `resolv.conf` names the DNS server of each network interface that has one (up to three, IPv4 ones first). Its first line says that it
  was written here, and a file with that line is written again when the servers change (a new DHCP lease). A `resolv.conf` without the
  line, or any of the other files once they exist, is left alone: put your own there and it stays.
- `hosts` has `localhost` (both versions) and the machine's own name; `nsswitch.conf` says `hosts: files dns`; `services` and
  `protocols` have the common ones.
- A datagram socket's `connect` with `AF_UNSPEC` takes its peer away again, and `FIONREAD` of one answers with room for any datagram:
  both are what a resolver relies on.
- glibc asks over `AF_NETLINK` (a list of addresses) which IP versions are configured, for `AI_ADDRCONFIG`. A statically linked glibc
  program resolves names here (the files and DNS lookups are built into the library); anything else in `nsswitch.conf` would need the
  library's own modules in the tree.

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
  registers in a handler's frame; the sender of a signal that came from another process (`si_pid` is 0 then).
- Copy-on-write for `fork`, and a `vfork` child that shares its parent's memory.
- The parts with a fixed answer above: `io_uring`, `fanotify`, `seccomp` filters, `bpf`, `perf_event_open`, `userfaultfd`, namespaces,
  restartable sequences, the key retention service, landlock, quotas, extended attributes, file handles.
- A shared mapping of a regular file that is written through (`MAP_SHARED` with `PROT_WRITE`).
- Of network sockets: datagrams larger than a frame, a source address, TTL or TOS chosen per datagram by a control message, the
  real TTL, TOS and arrival time of a received packet, the error queue, raw sockets. Of netlink: changing the
  network, link state, neighbours, the other protocols (audit, socket diagnostics, netfilter). Of name resolution:
  search domains (none are written to `resolv.conf`), mDNS, and a `resolv.conf` that follows the network while a program runs that
  never names the file again. File permission bits and owners (the calls succeed and keep nothing).
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
  `nettest.c` does the same for TCP and UDP sockets over the loopback, IPv4 and IPv6, control messages included, and `netlinktest.c`
  for netlink (`getifaddrs`, `if_nametoindex`, and requests made by hand; it prints only what every Linux machine has, the loopback). `NATIVE="ipctest" EMULATED="" tests/fork_qemu.sh`
  runs one.
- `tests/run_host_tests.sh LinuxInetSocketTest`: the network socket calls against a simulated second machine (clients, servers,
  datagrams, the loopback, IPv6, epoll, options, bad addresses), the files for finding hosts by name with a resolver's exchange
  with a DNS server, and netlink: the messages on their own (every request, cut short anywhere and with any bit flipped) and the
  sockets through the system calls, a change of address told to a listener among them.
- `resolvetest.c` in `tests/fork_qemu.sh`: `getaddrinfo`, `gethostbyname`, `getnameinfo`, `getservbyname` through glibc on the real
  kernel, from the files and from DNS (QEMU's DNS server hands the questions to this machine's resolver, which must answer for
  `.localhost` and `.invalid` by itself, as systemd-resolved does). The expected output is the dynamically linked build's: a static
  glibc program cannot look hosts up on a machine whose `nsswitch.conf` names modules it has to load.
