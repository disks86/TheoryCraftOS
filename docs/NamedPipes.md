# Named pipes (FIFOs) for Linux programs

`mkfifo`, `mknod(S_IFIFO)` and the pipes behind them, as Linux programs use them: a writer and a reader that find each other by a path, with pipe
semantics in between. Linux programs reach them through `mknod`/`mknodat` (glibc's `mkfifo()` is `mknodat`), `open`, `read`/`readv`, `write`/`writev`,
`poll`/`ppoll`/`select`/`epoll`, `fstat`, `fcntl` and `close`; the kernel's own services open them too (`KernelFifo`, which PipeWire's
`module-pipe-sink` and `module-pipe-source` use, see [PipeWire.md](PipeWire.md)).

## Files

| What | Where |
| --- | --- |
| The pipes (`UnixType::Fifo`), their open ends (`UnixType::FifoEnd`), the FIFO names of file systems without FIFO nodes | `src/Process/UnixSockets.{hpp,cpp}` |
| Making a FIFO node, telling one apart, the kernel's own ends (`KernelFifo`) | `src/Process/Fifo.{hpp,cpp}` |
| `open()` of a FIFO, its read/write/readv/writev, `F_GETFL`'s access mode, `fstat`'s `S_IFIFO` for FIFOs and `pipe2`'s ends | `src/Process/LinuxSockets.cpp` |
| `mknodat`/`mknod`, `stat`/`statx`/`getdents64` types, `F_GETPIPE_SZ` | `src/Process/LinuxFiles.cpp` (numbers in `LinuxSyscalls.cpp`) |
| Real FIFO inodes on ext2/3/4 (`IFileSystem::create_special`, `DirectoryEntry::special`) | `src/FileSystem/Ext/`, `src/FileSystem/FileSystemTypes.hpp`, `IFileSystem.hpp`, `Mount/` |
| Unlink and rename keep the names up to date | `src/Process/ProcessFiles.cpp` (`remove`, `rename`) |

## Where the node lives

A FIFO is two things: a **name** in the file system that `stat()` reports as `S_IFIFO`, and a **pipe** (a buffer and the counts of who has it open)
that exists while someone has the name open.

- **The name.** On a file system that keeps Unix node types, ext2/3/4, it is a real FIFO inode (`IFileSystem::create_special(path,
  SpecialNode::Fifo)`: mode `S_IFIFO | 0644`, no blocks, directory entry type `EXT2_FT_FIFO`), which e2fsck and debugfs see as one, survives a
  reboot and is visible to Linux on the same disk; FIFOs a Linux system made on the disk are found too (`DirectoryEntry::special`). The other file
  systems (FAT, NTFS, the RAM disk, BeFS, the network ones) have no such node type: there the name is an **empty regular file** that the system's
  `UnixSocketManager` remembers as a FIFO by its path in the OS's file system (`fifo_mark`, at most 4096 names). `ProcessFiles::remove` and
  `rename` (every unlink and rename of a Linux or Windows program) forget or move the remembered names, a folder's with it. Remembered names last
  until the system restarts; after that such a file is a plain empty file again (a program that `mkfifo`s at start-up makes it again, and
  `mkfifo` of a name that exists fails with `EEXIST` as on Linux). The primary partition and `/Linux` are usually FAT, so this is the common case.
- **The key.** Both kinds of name are keyed by the path of the OS's file system (`ProcessFiles::host_path_of`), as path-bound AF_UNIX sockets are:
  `/tmp/x` of a Linux program is `/Linux/tmp/x`; `/mnt/c/...` is the Windows tree. So `/Linux/tmp` works, and a kernel service opens the same pipe
  as the program by that path.
- **The pipe.** It belongs to the AF_UNIX manager (`UnixSockets.hpp`), under the system's one lock and wake-up, made by the first `open()` of the
  name and gone with its last end, whatever it still held. Each `open()` is an end of its own (`FifoEnd`), and the descriptor's token is that end's
  id with `DescriptorKind::UnixSocket`, so `read`, `write`, `poll`, `epoll`, `dup`, `fork`, `SCM_RIGHTS` and `close` treat it as they treat sockets
  and eventfds without code of their own. Its buffer is the anonymous pipes' and sockets' queue (`ByteBuffer` with a read head, compacted the same
  way), 64 KiB as on Linux (`F_GETPIPE_SZ`; `F_SETPIPE_SZ` only up to that).

## Behaviour

POSIX's and Linux's rules:

- `open(O_RDONLY)` waits until a writer has opened the FIFO (also one that came and went meanwhile); `open(O_WRONLY)` waits for a reader. With
  `O_NONBLOCK` the reader does not wait, and the writer fails with `ENXIO` if nobody has it open for reading. `O_RDWR` never waits. The wait is
  `unix_wait`: it ends with `EINTR` when the program is told to stop. `O_CREAT|O_EXCL` on an existing FIFO is `EEXIST`, `O_DIRECTORY` `ENOTDIR`,
  `O_TRUNC` is ignored.
- A read takes what is there, up to its size; with nothing there it is the end of the file (0) when no writer is left, else it waits (`EAGAIN`
  with `O_NONBLOCK`). `readv` is one read spread over the pieces.
- A write of up to `PIPE_BUF` (4096) bytes goes in whole or waits; a bigger one may be split, a blocking one returns when all of it went in. No
  reader left: `EPIPE` (no `SIGPIPE`: signals are not delivered to the Linux layer). A small `writev` is gathered and written whole.
- `poll`: `POLLIN` while data is there; `POLLHUP` for a reader once a writer has come and gone since it opened (never before any writer, as Linux's
  `f_pipe` rule); `POLLOUT` while `PIPE_BUF` fits; `POLLERR` for a writer with no reader. `FIONREAD` works.
- `fstat` of an end is `S_IFIFO` (and `pipe2`'s ends now report `S_IFIFO` too); `F_GETFL` gives the access mode it was opened with.
- Unlinking or renaming the name while it is open leaves the open ends on their pipe. A new open of the renamed name gets that pipe; a FIFO made
  again at the old name gets a new one.
- `mknod`: type 0 or `S_IFREG` makes an empty file, `S_IFIFO` a FIFO, `S_IFSOCK` a socket node where the file system has them (else `EPERM`), devices
  `EPERM` (no device numbers here), anything else `EINVAL`. Permission bits are not kept (there are no file modes yet).
- `getdents64` reports `DT_FIFO` for both kinds of name.

`KernelFifo` (for the kernel's services) is an end opened for reading and writing at once, as PulseAudio opens its pipes: it never waits for a
partner, makes the FIFO if asked, and neither call blocks.

## Tests

- `tests/run_host_tests.sh UnixSocketTest`: `linux_fifo` runs the calls one at a time twice, on a RAM file system (names remembered) and on one that
  keeps FIFO nodes as ext does: mknod's types (every type value), `EEXIST`, `ENXIO`, `EBADF` the wrong way, `ESPIPE`, the 64 KiB buffer and `EAGAIN`,
  `PIPE_BUF` atomicity at the edge, readv/writev, the end of the file, `POLLHUP`, `EPIPE` and `POLLERR`, `O_RDWR`, data gone with the last end,
  unlink and rename while open, `DT_FIFO`, epoll, a kernel end talking to a program's end, `pipe2`'s `fstat`; and the remembered names as folders
  are moved and removed, and the list's limit. `fifo_threads`: blocking opens in both orders from two processes, a writer that came and went ending a
  reader's wait, a blocked read woken by a write, four writers of 4096-byte records read whole by one reader, 1 MiB through the pipe in order.
- `ExtTest fifo`: a FIFO made by mke2fs from a host FIFO is reported and listed as one and cannot be opened for data; FIFO and socket nodes made by
  `create_special` pass e2fsck and are FIFO/socket inodes to debugfs; rename and remove; e2fsck again.
- `tests/pipewire_emulated.sh [arch] pipe`: `tests/linux/fifotest.c` (no C library, x86-64/AArch64/RISC-V) on the CPU emulator: mkfifo, stat,
  `ENXIO`, write/read/ppoll, the end of the file and `POLLHUP`, unlink; then the kernel's pipe modules (see PipeWire.md).

## Not done

- No `SIGPIPE` on a write without a reader (only `EPIPE`), as for sockets.
- Remembered names (FAT, NTFS, RAM, ...) do not survive a restart, are matched by the exact path (a case-insensitive file system's other spellings
  are not the FIFO), and a name removed by something other than a process (the desktop's file manager) stays remembered until a file of that name is
  made again and stat'd. ext's FIFO inodes have none of these limits.
- Windows programs see a FIFO as an empty file (ext: a file they cannot open); there is no bridge to Windows named pipes (`\\.\pipe\`).
- No device nodes (`S_IFCHR`/`S_IFBLK`) and no file permission bits.
- `O_ASYNC`/`F_SETSIG`, `splice`/`tee`/`vmsplice` on pipes are not there.
