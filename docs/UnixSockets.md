# Unix sockets (AF_UNIX)

TheoryCraftOS has one set of AF_UNIX sockets for the whole system, shared by Linux and Windows programs. A Windows server and a Linux client (or
the other way round) talk over one connection exactly as two programs of the same kind would: Linux programs use the usual socket system calls,
Windows programs use `ws2_32.dll` with `<afunix.h>`, and both reach the same sockets under the same names.

| Part | Where |
| --- | --- |
| The sockets: names, connections, queues, readiness, descriptor passing | `src/Process/UnixSockets.{hpp,cpp}` (`UnixSocketManager`) |
| `struct sockaddr_un` codec (the same layout on both systems) | `src/Process/UnixSocketAddress.hpp` |
| Linux system calls | `src/Process/LinuxSockets.{hpp,cpp}`, numbers in `LinuxSyscalls.cpp`, `read`/`write`/`fstat`/`fcntl`/`ioctl`/`dup` in `LinuxFiles.cpp` |
| Windows socket services for AF_UNIX handles | `src/Process/WindowsUnixSockets.{hpp,cpp}`, reached from `WindowsSockets.cpp` |
| Winsock | `src/Windows/ws2_32/ws2_32.c` (`AF_UNIX`, `SIO_AF_UNIX_GETPEERPID`, the `AF_UNIX` protocol entry) |
| Tests | `tests/UnixSocketTest.cpp` (`tests/run_host_tests.sh UnixSocketTest`) |

## Names

- **Paths** are bound to the file they name in the OS's file system, not to their text. Each program's path is resolved in its own view (relative
  paths against its current directory) and turned into the path of the file system underneath. A Linux program sees the Windows `C:` drive at
  `/mnt/c` (as under WSL, `filesystem::layout::kLinuxWindowsDrive`), so `C:\Users\Public\app.sock` in a Windows program and
  `/mnt/c/Users/Public/app.sock` in a Linux one are the same socket. Below `/mnt/c` names are compared without regard to case, as Windows does.
  A Linux path outside `/mnt/c` (`/tmp/x.sock`) is out of a Windows program's reach, as on WSL: the Windows tree is `C:` only.
- `bind()` creates the socket's file. It stays after the socket is closed, as on both systems: a later `bind()` of the same name fails with
  `EADDRINUSE`/`WSAEADDRINUSE` until the file is unlinked or deleted. `stat()` from a Linux program reports a file that a socket is bound to as
  `S_IFSOCK`. Connecting to a path whose file is missing is `ENOENT` (Linux) / `WSAECONNREFUSED` (Windows); to a file nobody listens on,
  `ECONNREFUSED`.
- **Abstract names** (`sun_path[0] == 0`) have no file. They are one namespace for both kinds of program, so they also connect a Windows program to
  a Linux one with no shared folder needed. `bind()` with only the family (address length 2) picks an abstract name of five hex digits (Linux's
  autobind).
- `getsockname()`/`getpeername()`/`accept()` report a name as the program that bound it spelled it (a Linux client of a Windows server sees
  `C:\...`). An accepted socket's own name is the listener's.

## Types and behaviour

- `SOCK_STREAM`, `SOCK_SEQPACKET` and `SOCK_DGRAM` (connected or not; `SOCK_RAW` is `SOCK_DGRAM`, as on Linux). Winsock offers `SOCK_STREAM` only,
  as Windows does; the kernel service takes the others.
- `connect()` of a stream or seqpacket socket completes at once when the listener's backlog has room (the listener's `accept()` then finds the other
  end) and waits (or says "would block") while it is full. A connected datagram socket takes datagrams only from its peer (`EPERM` for others).
- Each socket has one receive queue of 128 KiB (`kBufferBytes`, what `SO_RCVBUF`/`SO_SNDBUF` report); a sender waits for room in its peer's queue.
  A blocking stream send sends everything; a non-blocking one what fits. A message larger than the queue is `EMSGSIZE`.
- Closing: the other end of a connection reads end-of-file, then sends get `EPIPE` (no `SIGPIPE`: signals are not delivered). A socket closed with
  data it never read gives its peer `ECONNRESET`; so does a listener closed with connections it never accepted, to their clients.
- `shutdown()`, `MSG_PEEK`, `MSG_DONTWAIT`, `MSG_WAITALL`, `MSG_TRUNC`, `O_NONBLOCK`/`SOCK_NONBLOCK`/`FIONBIO`, `SOCK_CLOEXEC`, `FIONREAD`,
  `SO_RCVTIMEO`/`SO_SNDTIMEO` (`EAGAIN` / `WSAETIMEDOUT` when they pass), `SO_TYPE`, `SO_ERROR`, `SO_ACCEPTCONN`, `SO_DOMAIN`, `SO_PROTOCOL`.
- `poll`, `ppoll`, `select`, `pselect6` (Linux) and `select` (Winsock, `NtSelectSockets`, mixing network and unix sockets) report readiness; a
  wait on unix sockets only sleeps until one changes. Files and devices in a Linux `poll()` are always ready.
- **Credentials**: `SO_PEERCRED` (Linux) and `WSAIoctl(SIO_AF_UNIX_GETPEERPID)` (Windows) give the process id of the other end, whichever kind of
  program it is; `SO_PASSCRED` makes `recvmsg()` add `SCM_CREDENTIALS` with the sender's pid. uid and gid are 0 (there are no users yet).
- **Descriptor passing**: `SCM_RIGHTS` (Linux) carries any descriptor - files, sockets, pipes - to another process. The message holds a use of each
  (`descriptor_token_add`) until it is received; descriptors the receiver has no room for (`MSG_CTRUNC`) or that a socket still held when it was
  closed are released. As on Linux, descriptors need at least one byte to travel with on a stream, and a stream `recv()` stops before a segment
  that carries descriptors once it has taken bytes. `MSG_CMSG_CLOEXEC` is honoured.
- `sendmmsg`/`recvmmsg` (with `MSG_WAITFORONE`), `readv`/`writev`, `accept4` flags.

## Design

`UnixSocketManager` keeps up to 4096 sockets in a table of slots; a socket's id carries a generation, so a stale id (a socket closed meanwhile)
is never mistaken for a new one. Descriptor tables hold a socket by id (`DescriptorKind::UnixSocket`), with a reference count for duplicated,
inherited and passed descriptors.

Everything is guarded by the system's one lock and wake-up (`ISystemSync`, the one the synchronization objects and pipes use): a blocked call must
look at a socket and go to sleep without a wake-up falling in between, and every change wakes the waiters (`unix_wait`, shaped like
`windowsabi::wait_until`). The lock is never held across the file system or another manager: socket files are made and looked up before it is
taken, and descriptors a closing socket's queue held are released after it is let go. User memory is copied into and out of kernel buffers
outside the lock as well.

## Not done

- Two sockets that hold each other in their queues through `SCM_RIGHTS` and are both closed are not collected (Linux has a garbage collector
  for such cycles); a socket sent over itself is the simplest such cycle.
- `SO_PEERSEC`, `SO_PEEK_OFF`, `SIOCOUTQ` (always 0: sent bytes are in the peer's queue at once), the `recvmmsg` overall timeout,
  and `MSG_OOB`.
- The Linux view lists nothing in `/mnt` (`c` is reached, not listed). Windows `\\wsl$`-style paths into the Linux tree do not exist.
- Winsock: AF_UNIX overlapped I/O and `WSAEventSelect` work only as they do for the network sockets (polled).
- The Linux socket calls know only AF_UNIX: AF_INET/AF_INET6 answer `EAFNOSUPPORT` (Windows programs have those through `ws2_32`).
- Not yet exercised by a real Linux or Windows program in QEMU; the host test drives the system calls directly.
