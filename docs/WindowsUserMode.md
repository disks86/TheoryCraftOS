# Windows programs: libraries and system services


Windows (PE) programs run in ring 3 on `ntdll.dll`, `kernel32.dll`, `msvcrt.dll`, `advapi32.dll` and `ws2_32.dll`, freestanding DLLs built from `src/Windows/` and put on
the primary disk image in `C:\Windows\System32` (see [FileSystemLayout.md](FileSystemLayout.md)). ntdll turns the NT services into `SYSCALL`s. The service numbers are this
OS's own (real Windows renumbers its services in nearly every build, which is why programs call ntdll); `src/Windows/tcos_syscalls.h` and `src/Process/WindowsSyscalls.hpp`
hold the numbers and `tests/SyscallTest.cpp` checks that they agree. The services behind the system calls are in `src/Process/Windows*.cpp`; the kernel side is
`src/Kernel/UserMode.cpp`. advapi32.dll is the one library here with no NT services of its own: it is written entirely over kernel32.dll, the way a real one is layered over
kernel32 for the registry.

## Files and directories

| Service | Number |
| --- | --- |
| `NtReadFile` | 0 |
| `NtWriteFile` | 1 |
| `NtClose` | 2 |
| `NtCreateFile` | 10 |
| `NtOpenFile` | 11 |
| `NtQueryInformationFile` | 12 |
| `NtSetInformationFile` | 13 |
| `NtQueryDirectoryFile` | 14 |
| `NtQueryAttributesFile` | 15 |
| `NtDeleteFile` | 16 |
| `NtFlushBuffersFile` | 17 |
| `NtQueryVolumeInformationFile` | 18 |
| `NtLockFile` | 75 |
| `NtUnlockFile` | 76 |

## Pipes

| Service | Number |
| --- | --- |
| `NtCreatePipe` | 55 |
| `NtCreateNamedPipeFile` | 56 |
| `NtFsControlFile` | 57 |

## Mailslots

| Service | Number |
| --- | --- |
| `NtCreateMailslotFile` | 97 |

The one service this needs (a writer is just `CreateFile` on the name, like a named pipe's client end, needing no service of its own).
`CreateMailslot`/`GetMailslotInfo`/`SetMailslotInfo` work: one reader per name (a second `CreateMailslot` of the same name is refused, like the real one), any number
of writers (`CreateFile` on `\\.\mailslot\name`), each `WriteFile` queuing one whole message that a `ReadFile` on the reader's handle takes back whole - never split
or coalesced with another write's bytes, and a buffer too small for the next message leaves it queued (`STATUS_BUFFER_TOO_SMALL`, like the real one's
`ERROR_INSUFFICIENT_BUFFER`) rather than losing it. Closing the reader's handle destroys the mailslot at once; a writer that still has it open then sees it as
gone. Not provided: the domain-wide broadcast a real mailslot can do across machines with the same name (there is only one machine here) - only local processes on
this one machine can write to it, which is the common use anyway (a single-machine notification queue). The read timeout (`SetMailslotInfo`'s millisecond value,
`MAILSLOT_WAIT_FOREVER` included) is converted to this codebase's own relative, negative-100ns convention at the kernel32 layer, the same way `CreateFile`'s
disposition and options are converted from their Win32 constants.

## Memory and sections

| Service | Number |
| --- | --- |
| `NtAllocateVirtualMemory` | 7 |
| `NtFreeVirtualMemory` | 8 |
| `NtProtectVirtualMemory` | 9 |
| `NtQueryVirtualMemory` | 20 |
| `NtCreateSection` | 70 |
| `NtOpenSection` | 71 |
| `NtMapViewOfSection` | 72 |
| `NtUnmapViewOfSection` | 73 |
| `NtFlushVirtualMemory` | 74 |

## Processes

| Service | Number |
| --- | --- |
| `NtTerminateProcess` | 3 |
| `NtQueryInformationProcess` | 21 |
| `NtSetInformationProcess` | 48 |
| `NtCreateUserProcess` | 45 |
| `NtOpenProcess` | 69 |

## Threads

| Service | Number |
| --- | --- |
| `NtCreateThreadEx` | 24 |
| `NtTerminateThread` | 25 |
| `NtResumeThread` | 27 |
| `NtSuspendThread` | 42 |
| `NtQueryInformationThread` | 28 |
| `NtSetInformationThread` | 47 |
| `NtGetContextThread` | 43 |
| `NtSetContextThread` | 44 |
| `NtOpenThread` | 68 |
| `NtYieldExecution` | 4 |
| `NtQueueApcThread` | 58 |
| `NtDequeueApc` | 59 |
| `NtGetCurrentProcessorNumber` | 49 |

## Synchronization and time

| Service | Number |
| --- | --- |
| `NtWaitForSingleObject` | 26 |
| `NtWaitForMultipleObjects` | 40 |
| `NtSignalAndWaitForSingleObject` | 60 |
| `NtDelayExecution` | 5 |
| `NtCreateEvent` | 29 |
| `NtOpenEvent` | 30 |
| `NtSetEvent` | 31 |
| `NtResetEvent` | 32 |
| `NtPulseEvent` | 33 |
| `NtCreateMutant` | 34 |
| `NtOpenMutant` | 35 |
| `NtReleaseMutant` | 36 |
| `NtCreateSemaphore` | 37 |
| `NtOpenSemaphore` | 38 |
| `NtReleaseSemaphore` | 39 |
| `NtCreateTimer` | 61 |
| `NtOpenTimer` | 62 |
| `NtSetTimer` | 63 |
| `NtCancelTimer` | 64 |
| `NtQueryTimer` | 65 |
| `NtDuplicateObject` | 41 |
| `NtQueryObject` | 53 |
| `NtSetInformationObject` | 54 |
| `NtQuerySystemTime` | 6 |
| `NtQueryPerformanceCounter` | 19 |
| `NtQueryTimerResolution` | 66 |
| `NtSetTimerResolution` | 67 |

## Libraries (LoadLibrary)

| Service | Number |
| --- | --- |
| `NtLoadModule` | 50 |
| `NtReleaseModule` | 52 |
| `NtUnloadModule` | 51 |

## The registry

`advapi32.dll` (`src/Windows/advapi32/advapi32.c`) implements the registry entirely over kernel32.dll's file functions, with no NT services of its own: a key is a
directory under `C:\Windows\System32\config\<hive>`, and a value is one file in that directory, holding a small `[type, length, data]` header the value's own bytes follow;
the unnamed "default" value of a key is the file named `(Default)`. The five predefined keys (`HKEY_LOCAL_MACHINE`, `HKEY_CURRENT_USER`, `HKEY_CLASSES_ROOT`, `HKEY_USERS`,
`HKEY_CURRENT_CONFIG`) are fixed pseudo-handles mapping to the hive directories (made, with their parents, the first time something is created under them); there is no
separate per-user hive, so `HKEY_CURRENT_USER` is just another one of the five, and `HKEY_CLASSES_ROOT` does not merge with `HKLM\Software\Classes` the way the real one does.
This makes the registry an ordinary part of the file system, shared by every process like the real one (not per-process), and exactly as persistent as whatever is mounted
there (nothing, on the RAM file system the boot demo uses, like everything else in it). `RegOpenKeyEx`, `RegCreateKeyEx`, `RegCloseKey`, `RegQueryValueEx`/`RegGetValue`,
`RegSetValueEx`, `RegDeleteValue`, `RegDeleteKey` (only on a key with no subkeys, like the real one), `RegEnumKeyEx`, `RegEnumValue`, `RegQueryInfoKey` and `RegFlushKey`
(a no-op: there is no cache) all work, for `REG_SZ`, `REG_EXPAND_SZ` (no `%VAR%` expansion), `REG_MULTI_SZ`, `REG_DWORD`, `REG_QWORD` and `REG_BINARY`. Not provided:
security descriptors and access checks (any access is granted), `REG_DWORD_BIG_ENDIAN`, volatile keys (a key made "volatile" is not treated specially: it persists exactly
like any other, as far as the mounted file system does), remote registries, and registry change notifications. `advapi32.dll` also has `GetUserName` (one fixed user),
`OpenProcessToken`/`GetTokenInformation` (enough to answer "what user" and "is it elevated", always yes: there is no UAC), and `CryptAcquireContext`/`CryptGenRandom`/
`CryptReleaseContext` (bytes that are not predictable in practice but are not a cryptographic guarantee, since there is no entropy driver for programs yet).

## Job objects

| Service | Number |
| --- | --- |
| `NtCreateJobObject` | 77 |
| `NtOpenJobObject` | 78 |
| `NtAssignProcessToJobObject` | 79 |
| `NtTerminateJobObject` | 80 |
| `NtQueryInformationJobObject` | 81 |
| `NtSetInformationJobObject` | 82 |
| `NtIsProcessInJob` | 83 |
| `NtReadVirtualMemory` | 84 |
| `NtWriteVirtualMemory` | 85 |
| `NtCreateSymbolicLink` | 86 |
| `NtCreateSocket` | 87 |
| `NtBindSocket` | 88 |
| `NtConnectSocket` | 89 |
| `NtListenSocket` | 90 |
| `NtAcceptSocket` | 91 |
| `NtSendSocket` | 92 |
| `NtReceiveSocket` | 93 |
| `NtSocketControl` | 94 |
| `NtSelectSockets` | 95 |
| `NtCreateChangeNotification` | 96 |
| `NtNotifyChangeDirectoryFile` | 98 |

## Sockets

`ws2_32.dll` (`src/Windows/ws2_32/ws2_32.c`) is Winsock over `src/Process/Sockets.{hpp,cpp}` and the NT services above - this OS's own, unlike real Windows, where Winsock
talks to a device driver (`afd.sys`) through its own ioctl protocol rather than NT services. IPv4 only: `AF_INET`/`SOCK_STREAM`/`SOCK_DGRAM`, over whatever network
interfaces `src/Network/NetworkStackSet` has (set as the system's sockets at boot once at least one interface exists, right after its service thread starts). A `SOCKET`
is the same kernel `HANDLE` a program's other descriptors are (`DescriptorKind::Socket`), closed and duplicated/inherited exactly like a pipe handle. `WSAStartup`/
`WSACleanup`, `socket`, `bind`, `connect` (blocking retries the handshake itself; non-blocking returns `WSAEWOULDBLOCK` until `select()`'s except/write sets or
`getpeername` show how it resolved), `listen`, `accept`, `send`/`recv`, `sendto`/`recvfrom`, `shutdown`, `ioctlsocket` (`FIONBIO`, `FIONREAD`), `select` (up to 64 sockets
per set), `getsockname`/`getpeername`, `closesocket`, `WSAGetLastError`/`WSASetLastError`, `htons`/`htonl`/`ntohs`/`ntohl`, `inet_addr`/`inet_ntoa` all work. A failing NT
status is mapped to its own WSA error (`WSAECONNREFUSED`, `WSAEWOULDBLOCK` and so on), not a plain Win32 one - `WSAGetLastError` still reads the same per-thread slot as
`GetLastError`, matching the real one. Not done: IPv6, `getaddrinfo`/`gethostbyname` (no DNS from a Windows program yet, though the kernel's own network stack resolves
names for itself), `WSAAsyncSelect`/`WSAEventSelect`, overlapped socket I/O, `SO_RCVTIMEO`/`SO_SNDTIMEO` and other `setsockopt` options, multicast, raw sockets, and a
loopback interface (a program's own address is whichever one network interface it is bound to; nothing short-circuits traffic addressed back to that same address).

## Directory change notifications

`FindFirstChangeNotification`/`FindNextChangeNotification`/`FindCloseChangeNotification` and `ReadDirectoryChangesW` are backed by two services, `NtCreateChangeNotification`
and `NtNotifyChangeDirectoryFile`, this OS's own (real Windows has neither: both families go through the single `NtNotifyChangeDirectoryFile`, an IRP-based service that
completes asynchronously - not a fit for a kernel with no real asynchronous I/O, so both are synchronous here instead, and split into two services because the two
families bind to different kinds of handle - a dedicated notification handle for the first, an already-open directory handle for the second). A change notification
handle is an ordinary `ObjectManager` object (kind `ChangeNotification`) that behaves exactly like a manual-reset event: `WaitForSingleObject`/`WaitForMultipleObjects`
work on it unmodified, and `FindNextChangeNotification` is literally `NtResetEvent` on the same handle (it is not a separate service). It becomes signalled - and, for
`ReadDirectoryChangesW`, queues a `FILE_NOTIFY_INFORMATION`-style action-and-name record - when a file or subdirectory is created, deleted, renamed, hard-linked or
symbolically linked directly under the watched directory, or anywhere under it with `bWatchSubtree`/`WatchTree` true; the directory itself must exist when the handle
or watch is made. The `dwNotifyFilter`/`CompletionFilter` bits are accepted and kept but not matched against - any of those changes signals every watch of its
directory and queues it a record, regardless of which `FILE_NOTIFY_CHANGE_*` bits were asked for (the same kind of permissive simplification as the registry's access
checks). `ReadDirectoryChangesW` works both synchronously and overlapped (its own watch is made and kept on the directory handle's first call, from then on just
drained); an overlapped call's event is set and its completion routine, if any, queued as an ordinary APC, exactly like `ReadFileEx`/`WriteFileEx` already do - there
is no real asynchronous I/O underneath either, the call is always done at once. Only the RAM file system fires any of this (the one backing `C:\` in the demo and the
primary image); a change made through FAT/Ext/NTFS/BeFS is invisible to a watcher, since those are not reachable from a Windows program's `C:\` anyway. Not done: a
file deleted via `FILE_DISPOSITION_INFORMATION`/delete-on-close firing at the point the handle actually closes (only `NtDeleteFile` and an overwriting `CreateFile`
do), watching anything outside the RAM file system, and a `ReadDirectoryChangesW` buffer too small to hold every change already queued when it is called losing the
rest instead of returning them on a later call (each watch keeps only its most recent 32 pending changes anyway, so this is a narrow edge case).

## Exceptions and the machine

| Service | Number |
| --- | --- |
| `NtContinue` | 22 |
| `NtRaiseException` | 23 |
| `NtQuerySystemInformation` | 46 |

## How things behave

- **Threads** of one program run on all the cores, each on the page tables of the program; changes to the address space reach the other cores before the memory is given back
  (`Task::sync_tables`). Affinity masks (`SetThreadAffinityMask`, `SetProcessAffinityMask`) are honoured by the scheduler.
- **Scheduling**: the kernel's ready queue keeps 32 priority levels apart (`kernel::kPriorityLevels`); a core always runs the highest level that has something ready for
  it, round-robin (time-sliced) among threads at the same level, and a thread of a strictly higher level preempts one already running without waiting for its slice.
  `SetThreadPriority`/`SetPriorityClass` compute this absolute level from the Win32 priority class and the thread's relative priority (`windowsabi::absolute_thread_priority`,
  in `src/Process/WindowsProcess.cpp`) the same way real Windows does, and changing a process's class re-levels its existing threads (`repriority_threads`). Not done:
  priority boosting (a starved low-priority thread is never bumped up), and a process's own initial priority class from `CreateProcess`'s creation flags (it always starts
  at `NORMAL_PRIORITY_CLASS`; call `SetPriorityClass` afterwards).
- **Processes**: `CreateProcess` starts another program with its own address space; handles can be inherited (`bInheritHandles`), the standard handles redirected
  (`STARTF_USESTDHANDLES`), the child waited for and terminated. A child ends when the program that started it ends. `OpenProcess` gives access to the exit status and the wait.
- **Libraries**: `LoadLibrary`/`FreeLibrary` map and unmap DLLs after start-up, with their imports, reference counted; `DllMain` and TLS callbacks run. A DLL's own
  `__declspec(thread)` data works (up to 16 modules with thread-local data at once).
- **Files**: a read-only attribute and the times can be set (RAM and FAT file systems); other attributes are accepted and not stored. Byte-range locks are advisory and
  shared by all programs. File mappings of files are read into memory when made and written back when flushed, when the last view is closed and when the mapping is.
- **Hard and symbolic links** (`CreateHardLink`, `CreateSymbolicLink`; service `NtCreateSymbolicLink`, and `NtSetInformationFile`'s `FileLinkInformation` class for hard
  links, the same way `MoveFileEx` goes through `FileRenameInformation`) work on file systems that have a notion of a file apart from its one name — today the RAM file
  system (which is what the demo and the primary image's `C:\` both use), not FAT. A hard link shares its data, size, read-only flag and times with the name it was made
  from; writing or changing attributes through either name is seen through the other, and the data outlives either single name. A symbolic link's target is resolved
  transparently by `CreateFile`/`stat`-style access (reading, writing, traversing into a directory link) and, for a relative target, against the *link's own directory*
  (not the process's current directory), the one place a path here may use ".."; a directory listing (`FindFirstFile`) shows the link itself, with
  `FILE_ATTRIBUTE_REPARSE_POINT` set, rather than following it. There is no way yet to read a link's raw, unresolved target from a Windows program (no
  `DeviceIoControl(FSCTL_GET_REPARSE_POINT)` or `GetFinalPathNameByHandle`), and a target that crosses file systems is not supported, since this OS mounts only one drive
  for Windows programs anyway.
- **Overlapped I/O** is done at the call and the OVERLAPPED filled in; completion routines (`ReadFileEx`, `WriteFileEx`, timers, `QueueUserAPC`) run when the thread waits
  alertably.
- **Locale and strings**: there is one locale, English (United States). `GetLocaleInfo`, `GetNumberFormat`, `GetCurrencyFormat`, `GetDateFormat`, `GetTimeFormat`,
  `LCMapString`, `GetStringType`, `CompareStringOrdinal`, `FindStringOrdinal`, `FoldString` and the enumerations work for it (Latin, Greek and Cyrillic letters have case).
- **Atoms** (local and global) share one table of 256 names in the program. **Completion ports** are queues programs post to (`PostQueuedCompletionStatus`, `GetQueuedCompletionStatus[Ex]`):
  no file I/O is asynchronous, so files associated with a port never post packets. **Process lists**: `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)`, `Process32First/Next` and
  `K32EnumProcesses` read `NtQuerySystemInformation` class 5 (one entry per running program, followed by its `SYSTEM_THREAD_INFORMATION` entries). `ReadProcessMemory`/`WriteProcessMemory` work on the
  program itself only; thread descriptions are kept per thread.
- **The console**: the terminal gives a program lines of text and takes text; on top of that kernel32 keeps what a Windows console has (`console.inc`): screen buffers
  of character/colour cells (`CreateConsoleScreenBuffer`, `SetConsoleActiveScreenBuffer`, the `Read/Write/Fill/ScrollConsoleOutput*` family), cursor, window, text
  attributes, input and output modes, the title, and ANSI sequences when the output mode has `ENABLE_VIRTUAL_TERMINAL_PROCESSING` (colours, cursor movement, erasing, cursor
  visibility, title). Text written with `WriteFile`/`WriteConsole` to the console goes into the model and, without the sequences, to the terminal; cell and cursor
  changes made with the other functions stay in the model, because the terminal cannot show them. Key events (`ReadConsoleInput`, `PeekConsoleInput`,
  `GetNumberOfConsoleInputEvents`, `WriteConsoleInput`, waiting on the input handle) are made from the typed text, a key down and a key up per character; no mouse, focus or resize events.
  `NtQueryInformationFile` class 24 on the console input says how many bytes could be read without waiting. `GenerateConsoleCtrlEvent` runs the program's own control handlers;
  Ctrl+C typed at the terminal still ends the program without running them.
- **Job objects** (`CreateJobObject`, `OpenJobObject`, `AssignProcessToJobObject`, `TerminateJobObject`, `IsProcessInJob`, `QueryInformationJobObject`,
  `SetInformationJobObject`; services 77–83): a process is in one job; the processes it starts join it (not with `CREATE_BREAKAWAY_FROM_JOB`, which needs
  `JOB_OBJECT_LIMIT_BREAKAWAY_OK`; `SILENT_BREAKAWAY_OK` lets children start outside). Enforced: the active process limit (`ERROR_NOT_ENOUGH_QUOTA`, also for a child), kill on
  close and `TerminateJobObject` (members see a flag at their next system call or timer tick, so a member blocked reading the console ends when the read returns).
  Kept and read back but not enforced: CPU time, working set, memory, affinity, priority and UI limits. Accounting has the process counts only (CPU times are 0). Completion
  port notifications (class 7) are refused with `ERROR_NOT_SUPPORTED`; there are no nested jobs.
- **Toolhelp and psapi**: `CreateToolhelp32Snapshot` takes processes, threads (`Thread32First/Next`), modules (`Module32First/Next`, for any running process, from this OS's
  `NtQuerySystemInformation` class 0x80: the process id in the first 8 bytes of the buffer) and, for this process only, heaps (`Heap32ListFirst/Next`, `Heap32First/Next`;
  `HeapWalk` walks the blocks of a heap). psapi: `EnumProcessModules`, `GetModuleBaseName`, `GetModuleFileNameEx`, `GetModuleInformation`, `GetProcessMemoryInfo`,
  `GetProcessImageFileName` (the `K32` names and the plain ones). Module paths of other processes are guessed: DLLs are `C:\Windows\System32\name`, and only the program's name
  is known. Memory counters are the mapped size of the address space; thread priorities and start addresses are as the thread was created.
- **Other processes' memory and threads**: a process handle (from `CreateProcess` or `OpenProcess`) works with `ReadProcessMemory`, `WriteProcessMemory`, `VirtualAllocEx`,
  `VirtualFreeEx`, `VirtualProtectEx`, `VirtualQueryEx` and `CreateRemoteThread[Ex]` (services `NtReadVirtualMemory` 84, `NtWriteVirtualMemory` 85; the other memory services take
  the handle). The work is done on the other program under its lock (`IProcessRuntime::remote_operation`, `windowsabi::run_remote`), and its page tables are rebuilt and given to its
  threads on every core. Reads and writes stop at the first page that is not committed (`ERROR_PARTIAL_COPY`, with the count); writes ignore page protection, as debuggers need.
  A remote thread's handle can be waited for, `GetExitCodeThread`, `GetThreadId`, `ResumeThread` (if created suspended) and `TerminateThread` work on it; other thread calls
  (contexts, priorities, APCs) do not. Code can be injected: write it into memory allocated with `VirtualAllocEx` and start a thread on it. There is no access check beyond having a handle.
- **Not provided**: debugging APIs, and the many exports of the real kernel32 and msvcrt that no program here has needed (`FindFirstChangeNotification`/`ReadDirectoryChangesW` - see "Directory change notifications" - and mailslots - see "Mailslots" - are both done).
  A call to an import that nothing provides stops the program with a message saying which one.
