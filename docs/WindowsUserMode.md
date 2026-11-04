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
| `NtCancelSynchronousIoFile` | 156 |

A write that does not fit the buffer is queued whole, in the pipe's own queue (`ObjectManager::Pipe`, larger than the reported buffer): the other end sees its bytes at
once (`PeekNamedPipe`, `ReadDataAvailable`), and the write is complete when enough was read to make room for all of it. A call that has to wait is an operation of the
pipe with a tag: reads that wait are served in the order they were made, and cancelling one operation (`CancelIoEx`, `CancelIo`, closing the handle, the end of its process)
takes back the bytes of a write that were not read. An overlapped call waits in a thread of its own in the library (`BEGIN_READ/WRITE/LISTEN/TRANSACT`, `AWAIT`, `ACK`,
`CANCEL`, the library's own FSCTLs 0x11FFF0 and up); whoever finishes an operation does not return before that thread has completed the `OVERLAPPED`, so the event is set
when the call that caused it returns. `TransactNamedPipe` writes without waiting and queues the read; `PIPE_NOWAIT` writes take what the waiting reads take and what fits.
A pipe end can be tied to an I/O completion port: when a process ends while a call of it waits on the end, the kernel keeps the packet for the port. Not done: a completion
for a call of another process that finishes normally is still posted by that process's own library (to the port tables of that process only).

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

## The registry and advapi32.dll

The registry is the kernel's (`src/Process/RegistryTree.cpp`, `WindowsRegistry.cpp`): one tree in memory shared by every program, reached by the NT services `NtCreateKey`,
`NtOpenKey`, `NtQueryKey`, `NtEnumerateKey`, `NtSetValueKey` ... and, new, `NtRenameKey`, `NtQueryMultipleValueKey`, `NtNotifyChangeKey`, `NtSaveKey(Ex)`, `NtRestoreKey`,
`NtReplaceKey`, `NtLoadKey*`, `NtUnloadKey*`, `NtQueryOpenSubKeys`, `NtSetInformationKey`. A hive file (`RegSaveKey`) is this OS's own format ("TCRH", see `RegistryTree::save`),
not Windows' regf; saving needs SeBackupPrivilege, restoring/loading SeRestorePrivilege. A loaded hive is written back by `RegFlushKey` and `RegUnLoadKey`. A change to a key signals
the watches of `NtNotifyChangeKey` (an event given with an asynchronous call is signalled by the kernel once). Transacted calls only check for a transaction handle.
`NtShutdownSystem` (SeShutdownPrivilege) asks the desktop loop to shut down or restart; `NtImpersonateAnonymousToken`, the `Nt*AuditAlarm` services and a system random source
(`NtQuerySystemInformation` class 0x1000) are in ntdll.dll / the kernel too.

`advapi32.dll` is a set of files in `src/Windows/advapi32`: `registry.inc`/`registry2.inc` (registry), `security.c`/`kbsec.c`/`secext.c` (security, trustees, audit alarms, logon,
shutdown), `lsa.c`/`policy.c`/`lsa_ns.c` (the LSA: account rights, secrets, private data, audit policy kept in `HKLM\SECURITY\Policy`; trust and forest functions answer
STATUS_NOT_SUPPORTED), `scm.c` (services), `eventlog.c`, `etw.c`, `crypt.c`, `cred.c`, `perf.c`, `misc.c`.

* **Services**: the database is `HKLM\SYSTEM\CurrentControlSet\Services\<name>`; what runs is in `...\Control\TcosServiceState\<name>`. `StartService` starts the services it depends
  on, puts the name in the environment (`TCOS_SERVICE_NAME`) of the new process and creates it from `ImagePath`; its `StartServiceCtrlDispatcher` claims the state and runs
  `ServiceMain`. `ControlService` signals the named event `TcosSvcCtl_<name>`. Drivers cannot be loaded. There is no boot-time SCM: nothing is started automatically.
* **Event log**: files `C:\Windows\System32\winevt\Logs\<Log>.evt` of real `EVENTLOGRECORD`s; `Elf*` are the native forms.
* **ETW**: sessions are registry keys plus a log file; providers hear of sessions through a watched generation counter; `ProcessTrace` replays the file.
* **CryptoAPI**: MD4/MD5/SHA-1/SHA-2/HMAC, RC4, AES-128/192/256 (ECB/CBC, PKCS #5), key derivation, plain-text key blobs. No RSA (CryptSignHash answers NTE_BAD_ALGID).
* **Credentials**: `Cred*` over `HKCU\Software\Microsoft\TcosCredentials`.
* Not provided (they answer an error, never success): EFS, WMI blocks, the RPC stubs (`BaseReg*`, `I_Sc*`), Safer, software installation by policy, the DES password hash helpers
  (`SystemFunction001`..`035`), performance counter queries (providers work), wait chains.

Test: `tests/windows/advapitest.c` (also acts as its own service).

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

## Common dialogs (comdlg32.dll)

`src/Windows/comdlg32/comdlg32.c`. Windows draws the common dialogs as windows of the program; user32/gdi32 are not implemented here, so the dialogs that need a person
are shown by the desktop and the program's thread waits. **`NtTcosCommonDialog`** (service 505, this OS's own; layout in `src/Process/WindowsDialogs.hpp`) takes one 72-byte
request (kind: open file / save file / colour, title, start path as a DOS path, filter list, initial colour, result buffer) and answers *chosen* or *cancelled*:
`WindowsDialogs.cpp` maps the DOS paths to the OS file system through the process's drive C: (`/Windows`) and back, and hands a `process::CommonDialogRequest` to
`IProcessHost::common_dialogs()` (`ICommonDialogs::run`, blocking). `desktop::Terminal` is the implementation for programs started from it: it opens the desktop's shared
`FileDialog` or `ColorDialog` and waits (a `kernel::ConditionVariable`) for the dialog's close handler. Answers: `STATUS_NOT_SUPPORTED` when the program has no screen (a
headless run, or a second dialog while one is open), `STATUS_BUFFER_TOO_SMALL` (with the length) for a short result buffer, `STATUS_OBJECT_PATH_NOT_FOUND` when the user
picked a file outside C:. Host test: `WindowsDialogTest` (fake screen, truncations and bit flips of the request).

In the DLL: `GetOpenFileName`/`GetSaveFileName` (A and W; both structure sizes; filter lists reduced to extensions for the dialog; `lpstrInitialDir`/initial name; the answer
in `lpstrFile` with `nFileOffset`/`nFileExtension`, `lpstrFileTitle`, `nFilterIndex`; default extension rules as Windows (plain extension of the filter wins, wildcard filters
use `lpstrDefExt`, `*.` adds nothing); `OFN_EXTENSIONDIFFERENT`; `OFN_NOCHANGEDIR`; a short `lpstrFile` gets `FNERR_BUFFERTOOSMALL` and the needed size in its first WORD),
`ChooseColor`, `GetFileTitle`, `CommDlgExtendedError` (per thread, reset by every call), and the structure checks of `ChooseFont`, `FindText`/`ReplaceText`, `PrintDlg`,
`PrintDlgEx`, `PageSetupDlg` with Windows' error codes (`CDERR_*`, `FRERR_BUFFERLENGTHZERO`, `E_INVALIDARG`/`E_HANDLE`). Without windows these cannot do more: the font
chooser ends with `CFERR_NOFONTS`, Find/Replace (modeless windows) with `CDERR_DIALOGFAILURE`, the print and page setup dialogs with `PDERR_NODEFAULTPRN` (a Windows
program has no spooler or printers). Not done: hook procedures and templates (accepted, never called), `OFN_ALLOWMULTISELECT` with several files (the file dialog picks one),
creating a new name in an Open dialog (`FileDialog` takes only existing files), the 8-filter limit of `FileDialog`, the COM `IFileDialog` (itemdlg), the data exports
`dwLBSubclass`/`dwOKSubclass`. Own checks in the Wine test harness: `scripts/winetest.sh comdlg32 tcos` (`tests/windows/wine/comdlg32/tcos.c`); Wine's own comdlg32 tests
need real windows.

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
- **Not provided**: debugging APIs, and most of the many exports of a real kernel32.dll that no program here has needed yet (`FindFirstChangeNotification`/
  `ReadDirectoryChangesW` - see "Directory change notifications" - and mailslots - see "Mailslots" - are both done). An ongoing audit (diffing this DLL's export
  table against a real kernel32.dll's) is filling these in a batch at a time - `lstrcat`/`lstrcmp`/`lstrcmpi`/`lstrcpy`/`lstrcpyn`/`lstrlen` (the bare,
  always-ANSI names, distinct from the `A`/`W` ones already there), `RtlCopyMemory`/`RtlMoveMemory`/`RtlFillMemory`/`RtlZeroMemory`/`RtlCompareMemory`,
  `GetEnvironmentStrings` (likewise bare/ANSI), `Beep`, `CancelIo`/`CancelIoEx`/`CancelSynchronousIo`, `GetCompressedFileSizeA`/`W`, `WinExec`, `OpenFile`, and
  `FatalAppExitA`/`W` are the first batch; debugging APIs, transacted file operations, serial port (`COMM`) functions, volume enumeration, and the rest are not
  done yet.
  A call to an import that nothing provides stops the program with a message saying which one.

## The kernel32 export audit (the last batch)

`src/Windows/kernel32/extra4.inc` carries the exports of a real kernel32.dll/kernelbase.dll that were still stubs (about 450 of them; `src/Windows/stubs/kernel32.inc` now lists only
what cannot or should not be implemented, see below). Two system services came with it, and two more were made to serve `VirtualAlloc2`:

- **`NtAlertThreadByThreadId` / `NtWaitForAlertByThreadId`** (118, 491; `WindowsObjects.cpp`): a flag on the thread's record. An alert that arrives before the wait is kept, so the
  wait ends at once. ntdll's `RtlWaitOnAddress` / `RtlWakeAddressSingle` / `RtlWakeAddressAll` (`ntdll/addr.inc`: one list of waiters under a spin lock, each waiter's node on its
  own stack) are made of them, and kernel32's `WaitOnAddress` / `WakeByAddressSingle` / `WakeByAddressAll` of those.
- **`NtAllocateVirtualMemoryEx`** (124; `WindowsMemory.cpp`): address requirements (lowest/highest address, alignment up to any power of two: more than needed is reserved, given back
  and the aligned spot taken at once, under the process lock), NUMA node, partition and attribute parameters (accepted, no effect), another process by handle. `VirtualAlloc2`,
  `VirtualAlloc2FromApp`, `VirtualAllocExNuma`, `VirtualAllocFromApp` use it. A user-physical-pages parameter answers `STATUS_NOT_SUPPORTED`.
- **`NtSetInformationVirtualMemory`** (443): prefetch, page priority and call-target lists are checked (ranges inside the user half, lengths) and accepted.
- **Continue handlers**: `RtlAddVectoredContinueHandler` / `RemoveVectoredContinueHandler` (ntdll `except.inc`) run after any handler that resumed execution.

In kernel32 (all in user mode, over the services above): resource enumeration (`EnumResource{Types,Names,Languages}{A,W,ExA,ExW}`), `GetBinaryType`, the file-API code page flag,
`SetEnvironmentStringsA`, write watch (`GetWriteWatch` reports every page of a `MEM_WRITE_WATCH` region as written: a superset, which is what a collector needs), offer/reclaim/discard,
the NUMA functions (one node), processor groups (one), working set size, shutdown parameters, affinity update mode, DEP and mitigation policies (kept, one-way), `Get/SetProcessInformation`,
`Get/SetThreadInformation` (memory priority, power throttling), ideal processor, CPU sets (ids 256 + processor index) and their masks, packages (a process is never packaged:
`APPMODEL_ERROR_NO_PACKAGE`; the name parsers and formatters work), application recovery and restart registration, the `Wer*` registrations (accepted and dropped),
`RaiseFailFastException`, power requests and `SetThreadExecutionState`, tape and firmware-variable functions (the answers of a machine without them), console aliases (`AddConsoleAlias`,
`GetConsoleAlias*`, per program), `FindFirstFileNameW` / `FindNextFileNameW` (hard links: the volume is walked when the file has more than one), `BackupRead` / `BackupWrite` / `BackupSeek`
(the data stream only), `ReadDirectoryChangesExW` (extended records made from the standard ones, synchronous calls), `GetFileInformationByName`, the transacted file functions (there is
no transaction manager: they fail with `ERROR_INVALID_HANDLE`/`ERROR_TRANSACTIONAL_CONFLICT`), thread pool I/O (`CreateThreadpoolIo` and friends: a port and a thread of their own per
object), `SetTimerQueueTimer`, `RegisterWaitForSingleObjectEx`, private namespaces and boundary descriptors (a namespace is a directory object, so "Alias\\Name" works), memory resource
notifications, psapi (`K32QueryWorkingSet(Ex)`, `K32GetPerformanceInfo`, mapped file names of program images), `GetDurationFormat(Ex)`, `GetUILanguageInfo`, `IsValidUILanguage`,
`EnumSystemGeoNames`, `VerLanguageName`, extended `CONTEXT` (`InitializeContext(2)`, `LocateXStateFeature`, `Get/SetXStateFeaturesMask`, `CopyContext`), enclaves / I/O rings / user-mode
scheduling / thread profiling (`ERROR_NOT_SUPPORTED`).

**Still stubs** (`stubs/kernel32.inc`): the undocumented internals of Windows (`Base*`, `Basep*`, `Quirk*`, `Termsrv*`, `Nls*`, `Reg*`, `Werp*`, VDM and 16-bit DOS support, the `*Worker`
activation-context entry points, calendar helpers such as `GetCalendarDateFormat`), the process snapshot API (`Pss*`), the LZ file compression functions (lz32.dll's, listed here by the
import library), the compiler intrinsics the import library lists (`_InterlockedXxx`, `__readmsr`, ...: not exports of a real kernel32.dll), `CheckTokenMembershipEx`,
`GetStringScripts` / `VerifyScripts`, `GetMappedFileName` of a data view (only program images are named), `SetVolumeLabel` (answers `ERROR_ACCESS_DENIED`: no service sets the label).

## The shell (shell32.dll) and COM (ole32.dll)

`src/Windows/shell32` is a real DLL (the rest of the shell libraries are still stubs): freestanding C over kernel32, advapi32's registry and user32, built against
mingw-w64's headers. One file per part (see the comment at the top of `shell32.c`). What works:

- **Folders.** Known folders and CSIDLs (`folders.inc`): `SHGetFolderPath*`, `SHGetKnownFolderPath`, `SHSetKnownFolderPath` (redirection in `HKCU\...\Explorer\User Shell Folders`),
  `SHGetSpecialFolderPath`, the item-list forms, and `IKnownFolderManager`/`IKnownFolder`. The profile is `%USERPROFILE%` (`C:\Users\Default`); the per-user and common
  folders are made on first use, Windows/System32/Fonts and the like are not. Virtual folders (This PC, Recycle Bin, Network, Control Panel, Printers) have GUID items and no path.
- **Item lists and folders** (`pidl.inc`, `items.inc`): the `IL*` functions, `SHParseDisplayName`, `SHBindToParent`, `SHGetNameFromIDList`, `IShellFolder`/`IShellFolder2`/`IPersistFolder3` for
  the desktop, This PC and file system folders, `IEnumIDList`, `IShellItem`, `IShellItemArray`, and a data object with `CF_HDROP` and "Shell IDList Array". The item formats are this OS's
  own (documented at the top of `pidl.inc`; programs treat them as opaque). The desktop folder is `C:\Users\Default\Desktop` (the public desktop is enumerated too).
- **File operations** (`fileop.inc`): `SHFileOperation` (copy, move, rename, delete with wildcards, several sources, `FOF_RENAMEONCOLLISION`, name mappings, the `DE_*` codes), the
  recycle bin (`C:\$Recycle.Bin\<SID>` with `$I`/`$R` files; `SHQueryRecycleBin`, `SHEmptyRecycleBin`), `SHCreateDirectoryEx`, unique names, `PathResolve`, `PathCleanupSpec`. There are no
  dialogs: where Windows would ask, the answer is yes.
- **ShellExecute** (`exec.inc`): `ShellExecute[Ex]`, `FindExecutable`, associations from `HKEY_CLASSES_ROOT` (extension -> class -> `shell\<verb>\command`, `%1 %L %* %2-%9 %~N`, the shell's own
  rules for splitting parameters), programs started with CreateProcess, URL protocols from the registry. No DDE.
- **Shortcuts** (`link.inc`): `CLSID_ShellLink` with `IShellLinkW/A`, `IPersistFile`, `IPersistStream`, `IShellLinkDataList` (flags only); files are written with a target list and Unicode strings,
  read with or without LinkInfo; hostile input is bounds-checked (test: truncations and bit flips).
- Also: `SHGetFileInfo` (names, attributes, executable type), drag and drop (`DragQueryFile`), change notifications (in-process), notification-icon bookkeeping, shared memory
  (`SHAllocShared`...), `CommandLineToArgvW`, the She* helpers, drive helpers, `SHGetMalloc`.
- **Not provided** (they answer with a failure, never crash): everything that needs a window system or icons (`SHBrowseForFolder`, `ExtractIcon*` pictures, `SHGetImageList`, context menus,
  views, property sheets, `Shell_NotifyIcon` display), `SHGetFileInfo`'s icon, DDE, the property store, `IShellItem2`, libraries, network shares.

`src/Windows/ole32` is the COM runtime of one process: `CoTaskMem*`, apartments (counted per thread), GUID/ProgID strings, `CoGetClassObject`/`CoCreateInstance` for in-process servers (the registry's
`InprocServer32`, `CoRegisterClassObject`, and shell32's own classes without registry entries), bind contexts, `CreateStreamOnHGlobal`, `ReleaseStgMedium`, `PropVariantClear`. There is no RPC,
so out-of-process servers, marshalling, monikers and the clipboard are still stubs. shlwapi gained `StrRetTo*`, `SHStrDup`, the `SH*Value` registry helpers, `SHCreateStreamOnFile`, `AssocQueryString`;
user32 the `Char*`/`IsChar*` functions. No new NT system services were needed: everything above sits on the existing file, registry, section and process services.

Tests: `tests/windows/shell32test.c` (168 checks; run it as `shell32_own.exe` through `scripts/winetest.sh shell32 own`, with `WINETEST_EXTRA=<procchild.exe>` for the ShellExecute checks) and Wine's
`tests/windows/wine/shell32` (status in the table of `tests/windows/wine/README.md`).

## Stubs: every service and every export is in place

Nothing a program links against is missing, even where nothing is implemented yet (`scripts/gen-windows-stubs.py` produced the lists from the mingw-w64 import libraries, which name the exports of the real DLLs).

- **System services.** `src/Windows/tcos_syscalls.h` and `WindowsSyscallNumber` number all 500 `Nt*` services of ntdll.dll (0-98 are the implemented ones, the rest follow in alphabetical order; a number never changes once given). A number below `kWindowsServiceCount` that has no code answers `STATUS_NOT_IMPLEMENTED`; a number above it is `STATUS_INVALID_SYSTEM_SERVICE`. ntdll.dll has the stub of every service (`ntdll/services.inc`) and the `Zw*` twin of each (`ntdll/zw.inc`). The 1540 services of win32k (`NtUser*`, `NtGdi*`, `NtDxgk*`, ...) are a second table numbered from 0x1000 (`kWin32kServiceBase`, as Windows does), named in `scripts/win32k-services.txt` (taken from Wine's `win32u.spec`); their stubs are the exports of `win32u.dll` (`src/Windows/win32u`). gdi32.dll calls the NtGdi* ones (see *gdi32.dll* below).
- **win32k services.** The kernel side lives in `src/Process/Win32k` (framework `Win32k.hpp`: service table, `Call`, system lock, process-exit hooks; modules: GDI, window manager, user-misc, composition/other, and the Direct3D thunks in `Dxg/`, installed by `win32k::install()` / `install_dxg()`). About 720 of the 1539 services have handlers (GDI 168, window manager 146 + 3 private upcall services, user-misc ~119, composition 165, Direct3D 122); the rest still answer `STATUS_NOT_IMPLEMENTED`. The window procedure upcalls are done by `win32u.c` (wrapper exports around renamed raw stubs). Per-module design, gaps and the lists of unimplemented services: `docs/win32k/*.md`. Tests: `tests/run_host_tests.sh Win32kCoreTest Win32kGdiTest Win32kUserTest Win32kMiscTest Win32kCompositionTest Win32kDxgTest` (the misc test runs alone, with fakes for the other modules).
- **Exports.** `src/Windows/stubs/<dll>.inc` lists, per DLL, the exports that are not implemented; `stubs/stubs.c` compiles them into functions that return `STATUS_NOT_IMPLEMENTED` (ntdll.dll) or 0 (all the others). To implement one, write it in the DLL and delete its line from the list (a duplicate is a link error). The five real DLLs link their list in; the other libraries (user32, gdi32, shell32, ole32, ... 71 in all, see `WINDOWS_STUB_LIBRARIES` in `CMakeLists.txt`) are DLLs of nothing but stubs. All of them are files of `/Windows/Windows/System32`.
- **Gaps.** Only functions are stubbed (not data exports such as `_environ`), and names that are not C identifiers (C++ decorated names) are left out. kernelbase.dll and `api-ms-win-*` still fold into kernel32.dll (see `canonical_library_name()`), so their extra exports are not listed.


## dbghelp.dll

`src/Windows/dbghelp` (`dbghelp.c` and the `.inc` files it includes; `dbghelp.h` and `minidumpapiset.h` are Wine's headers, adapted to mingw's `windows.h`) implements the image helpers, the
symbol handler, the x64 stack walker and minidumps in user mode. **No new system call was needed**: it runs over `ReadProcessMemory`, `VirtualQueryEx`, the process snapshot, the
module list (`K32EnumProcessModules`), `NtQueryInformationThread` (the TEB) and the thread context calls, which cross process boundaries already.

- **Image helpers** (`image.inc`): `ImageNtHeader`, `ImageRvaToSection/Va`, `ImageDirectoryEntryToData(Ex)`, `GetTimestampForLoadedLibrary`, `ImagehlpApiVersion(Ex)`.
- **Symbol handler** (`symbols.inc`, `lookup.inc`): one list of modules per process handle (`SymInitialize`, `SymLoadModule(Ex)`, `SymUnloadModule`, `SymRefreshModuleList`, `SymCleanup`,
  options, search path, `SymGetModuleInfo(W)64`, `SymEnumerateModules`). A module is read from the memory of the process or from its file; its symbols are the PE export table and the
  **COFF symbol table** of the file (what mingw's linker leaves in an unstripped image), kept sorted by address. `SymFromAddr/Name/Index(W)`, `SymEnumSymbols(Ex)(W)`, `SymSearch(W)`,
  `SymNext/Prev`, the `IMAGEHLP_SYMBOL64` forms, `SymAddSymbol`/`SymDeleteSymbol` (virtual symbols), `SymMatchString/FileName`. A module is found for any address in a process
  (`SymGetModuleBase64` loads it when the symbol handler does not have it yet). Module loading follows Windows: loading at a base that is taken answers 0 with `ERROR_SUCCESS`, a base
  inside an older module replaces it, a base of 0 takes the file's own (`ERROR_INVALID_ADDRESS` if taken, `ERROR_NO_MORE_FILES` if there is no file), the size the caller gives wins.
- **Unwinding** (`stack.inc`): `StackWalk64`/`StackWalkEx` for AMD64: the UNWIND_INFO interpreter (pushes, allocations, frame register, saved registers, chained and indirect entries,
  machine frames) reading through the caller's memory routine, the function table from `SymFunctionTableAccess64` (a copy of each module's `.pdata`). A pc inside an epilogue is not
  recognised; the walk is right from call sites, bodies and prologs.
- **Minidumps** (`dump.inc`): `MiniDumpWriteDump` of any process the caller can read (thread list with contexts and stacks, module list, system and miscellaneous information, memory list or,
  with `MiniDumpWithFullMemory`, the 64-bit memory list; exception, memory-info, thread-info and thread-name streams; user streams) and `MiniDumpReadDumpStream`.
- **Names and files** (`undname.inc`, `paths.inc`): `UnDecorateSymbolName(W)` for the common MSVC signatures (global and member functions, static data, builtin types, pointers, references,
  classes with scopes, constructors, destructors; anything else falls back to the qualified name), `MakeSureDirectoryPathExists`, `SearchTreeForFile`, `EnumDirTree`, `FindFileInPath`,
  `FindExecutableImage`, `FindDebugInfoFile`, `EnumerateLoadedModules*`.
- **Not done.** PDB, DWARF and CodeView readers (so no lines, types, locals, source files: those calls fail with `ERROR_MOD_NOT_FOUND`/`ERROR_NOT_SUPPORTED`), symbol servers (`SymSrv*`, the
  index functions of Wine's `path.c`), the callback of `MiniDumpWriteDump`, handle data and token streams, 32-bit `StackWalk`, inline frames, the debugger-extension exports.
- **Gotcha (fixed).** mingw's ntdll import library lists some of kernel32.dll's functions (`OpenThread`, `QueueUserAPC`, `IsWow64Process`, ...), which had given ntdll.dll stubs of them that shadowed the real ones in any DLL linking `libntdll.a` first. `scripts/gen-windows-stubs.py` now leaves kernel32's names out of ntdll's list.
- **Tests.** `scripts/winetest.sh dbghelp tcos` runs `tests/windows/wine/dbghelp/tcos.c` (193 checks: image helpers, symbols of kernel32's exports and of the test program's COFF table, the stack
  walk through three frames, name decoding, directories, minidumps of the process itself in both modes, read back). Wine's own tests: `dbghelp` 89 -> 17 failed checks (the rest need files this OS does not have, a 32-bit process, and
  `STATUS_INVALID_CID` from a bogus process handle); `minidump` crashed with an access violation and now runs through (56 failed: the callback, the debug-event exception tests, `kernelbase.dll` as a
  module of its own); `path` is unchanged at 457 (all of it symbol-server and PDB indexing: `SymSrv*`).

## dwmapi.dll

`src/Windows/dwmapi/dwmapi.c` implements the Desktop Window Manager API: `DwmIsCompositionEnabled` (always on), `DwmGetColorizationColor`, `DwmFlush` and `DwmGetCompositionTimingInfo`
(a fixed 60 Hz refresh counted from the performance counter), `DwmSet/GetWindowAttribute` (the attribute table of the SDK: sizes, ranges, read-only attributes, defaults),
`DwmExtendFrameIntoClientArea`, `DwmEnableBlurBehindWindow`, the thumbnail functions, the iconic-bitmap functions, `DwmDefWindowProc` (never handles) and the frame-duration calls.
The state lives in win32k and is reached with one service, **`NtTcosDwm`** (service 506; see `docs/win32k/UserMisc.md`). Still stubs: the MIL/graphics-stream, contact/gesture and
`Dwmp*` exports, `DwmSetPresentParameters`, `DwmTransitionOwnedWindow`, and `SetThreadDpiAwarenessContext`. Nothing composites from the stored values yet. Not run under QEMU with
a Windows program; the kernel half is covered by `test_dwm` in `Win32kMiscTest`.

## imm32.dll

`src/Windows/imm32/imm32.c` implements the Input Method Manager. As in Windows an input context is two objects: the handle (`HIMC`) belongs to win32k (`NtUserCreateInputContext`,
`DestroyInputContext`, `UpdateInputContext`, `QueryInputContext`, `AssociateInputContext`; the association of windows with contexts; see `docs/win32k/UserMisc.md`), and the
`INPUTCONTEXT` with its `HIMCC` components (composition string, candidate info, guide line, private data, message buffer) lives in the process, in a client structure win32k keeps a
pointer to. The default context of a thread is made the first time one of its windows is asked for its context (`ImmGetContext`); `ImmAssociateContext(hwnd, NULL)` switches the IME off
for a window; the contexts of a thread go with the thread. Done: contexts (`ImmCreateContext`/`DestroyContext`/`GetContext`/`AssociateContext(Ex)`/`EnumInputContext`/`LockIMC` ...), the
`HIMCC` functions, open and conversion status, status/composition/candidate window positions, composition font (A and W), `ImmGenerateMessage`, the composition-string, candidate-list and
guide-line getters (A forms convert strings, attributes, clauses and positions), IME information (`ImmIsIME`, `ImmGetProperty`, `ImmGetDescription`, `ImmGetIMEFileName`,
`ImmGetImeInfoEx` over win32k's `IMEINFOEX` table), hot keys, `ImmDisableIME`, `ImmIsUIMessage`, `GetKeyboardLayoutCP`, `SendInput` (the export imm32 re-exports) and the
`CtfImm*` exports that are answers without the text services framework.
No IME module is ever loaded (the one layout win32k has, US English, has none), so the functions that are the IME's answer as Windows does without one: `ImmNotifyIME`,
`ImmSetCompositionString`, `ImmEscape`, `ImmConfigureIME`, `ImmGetConversionList`, the word registration, `ImmProcessKey` and `ImmTranslateMessage` fail or return 0. The composition
getters read what is in the context (empty unless a program wrote into it through `ImmLockIMC`). `ImmGetDefaultIMEWnd` answers NULL: the default IME window ("IME" class) is not made, and
user32 has no window functions yet, so the window-based paths (`ImmGetContext`, `ImmAssociateContext`, the `WM_IME_NOTIFY` sent to the context's window) are covered only by the kernel
half. Still stubs: `CtfImmGetGlobalIMEStatus`, `CtfImmGetGuidAtom`, `CtfImmNotify`, `CtfImmSetDefaultRemoteKeyboardLayout`. The kernel half is covered by `test_keyboard` in
`Win32kMiscTest`; the user-mode half by `tests/windows/imm32test.c` (run with `scripts/winetest.sh imm32 tcos` after putting the built `imm32test.exe` in `build/winetests/imm32_tcos.exe`).

## bcrypt.dll

`src/Windows/bcrypt/bcrypt.c` implements the primitives of Cryptography API: Next Generation. The DLL keeps what CNG keeps in its callers' objects (providers, hash objects, keys, shared
secrets; handles are pointers with a magic number, the pseudo-handles such as `BCRYPT_SHA256_ALG_HANDLE` are static objects) and the arithmetic is done by the kernel with `src/Crypto`, through
one service, **`NtTcosCrypto`** (service 507, `src/Process/WindowsCrypto.cpp`). It takes a pointer to 14 numbers: the operation and its parameters (pointers into the program, lengths, codes).
Operations: 1 hash/HMAC (MD4, MD5, SHA-1/224/256/384/512), 2 PBKDF2, 3 HKDF, 4 AES (ECB, CBC, CFB-128, CFB-8; the chaining value is written back), 5 AES-GCM (12 byte nonce, 16 byte tag),
6 elliptic curves (P-256, P-384: new scalar, public point, ECDH, ECDSA sign and verify), 7 RSA verification (PKCS #1 v1.5, PSS). Every length is bounded, every pointer is checked, and a tag or signature that
does not match is an NTSTATUS (`STATUS_AUTH_TAG_MISMATCH`, `STATUS_INVALID_SIGNATURE`), not output. Random bytes come from `NtQuerySystemInformation` class 0x1000.

Done: `BCryptOpenAlgorithmProvider`/`Close`, `Get`/`SetProperty` (object, hash and block lengths, algorithm name, chaining mode, key length(s), tag lengths, signature length),
`BCryptGenRandom`, hashes (`CreateHash`, `HashData`, `FinishHash`, `DuplicateHash`, `BCryptHash`; HMAC through `BCRYPT_ALG_HANDLE_HMAC_FLAG`; a hash object keeps its input until it is finished),
`GenerateSymmetricKey`, `Import`/`ExportKey` (`KeyDataBlob`, ECC and RSA public blobs), `Encrypt`/`Decrypt` (AES with PKCS #7 padding, GCM with `BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO`, RC4 in the DLL),
`GenerateKeyPair`/`FinalizeKeyPair`/`ImportKeyPair` (ECDSA and ECDH; imported points are checked to be on the curve and a private blob to match its point), `SignHash`/`VerifySignature`, `SecretAgreement`,
`DeriveKey` (HASH, HMAC, TRUNCATE), `DeriveKeyPBKDF2`, `KeyDerivation` (PBKDF2 and HKDF), `EnumAlgorithms`, `FreeBuffer`, `GetFipsAlgorithmMode`.
Not done (they answer `STATUS_NOT_SUPPORTED`, or stay stubs): MD2, DES/3DES/RC2, AES-CCM/CMAC/GMAC, P-521, RSA key generation, signing and encryption, wrapped keys, AES-GCM with a shortened tag
on decryption or with chained calls, SP800-108/TLS PRF/CAPI KDFs, and the context and provider registration functions (`src/Windows/stubs/bcrypt.inc`). Tests: `SyscallTest` (known answers
of every operation, bit flips and bad pointers) and `tests/windows/bcrypttest.c` (the DLL itself, run on the emulator with `tests/emulated_windows.sh tests/windows/bcrypttest.c`).

## shcore.dll

`src/Windows/shcore/shcore.c` implements the shell core library: DPI (`GetDpiForMonitor`, `Get/SetProcessDpiAwareness`, `GetScaleFactorForMonitor/Device`, `GetDpiForShellUIComponent`)
over the existing win32k services `NtUserGetDpiForMonitor`, `NtUserGetSystemDpiForProcess` and `NtUserGet/SetProcessDpiAwarenessContext` (no new service was needed); the scale-change
events and window notifications are signalled by a watcher thread that polls the system DPI every 250 ms while somebody is registered (win32k has no change notification).
Also the truncating string copies (`SHAnsiToUnicode` ...), the registry helpers shlwapi lacked (`SHRegGetValue`, `SHEnumKeyEx`, `SHEnumValue`, `SHQueryInfoKey`, `SHCopyKey`,
`SHRegGetPath/SetPath`, `SHRegGetInt`, ...), memory and registry streams (`SHCreateMemStream`, `SHOpenRegStream*`, `IStream_Copy/ReadStr/WriteStr`), the `IUnknown_*` helpers, the process
reference (shell32's `SHGetInstanceExplorer` reads it), thread references and `SHCreateThread`, `IsOS` (a Windows 10 workstation), the explicit AppUserModelID (kept in the process
parameters; shell32 forwards to it), and feature-state queries (always the default state). The shlwapi/shell32 functions that already exist are forwarders (`shcore.def`).
Still stubs: the `SHTaskPool*` functions; the WinRT stream functions report `E_NOTIMPL` (no HSTRING / IRandomAccessStream). Tested with Wine's `shcore` test (240 checks, 0 failures) and `tests/windows/shcoretest.c` (65 checks; `scripts/winetest.sh shcore own`).
Side changes: `GetProcAddress` now loads the library a forwarder names (it only followed forwarders into libraries already loaded); `SHCreateStreamOnFile` shares read and write by
default and `SetSize` on a read-only stream only moves the position; the registry seeds `HKLM\\...\\CurrentVersion\\Explorer\\Shell Folders` (the Common * folders).

## dwrite.dll

`src/Windows/dwrite` implements DirectWrite (the `IDWriteFactory` interface family of dwrite.h, `DWriteCreateFactory`) over the kernel's font system. The fonts are the kernel's
(`src/Font`: the bundled Liberation/Noto faces and any font a program adds), reached with one service, **`NtTcosFont`** (service 508; request layout and operations in
`src/Process/WindowsFonts.hpp`, implemented by `font::SystemFontService` in `src/Font/SystemFontService.cpp`, installed in `src/main.cpp`): face information and names, code point to
glyph, glyph advances and boxes, kerning, outlines, anti-aliased glyph bitmaps, the bytes of the sfnt file (the DLL reads tables itself) and adding a font file.

What the DLL does itself: the COM objects (font collection, family, font, face, localized strings, font files and their loaders, text format, text layout, text analyzer, glyph run
analysis, GDI interop with the bitmap render target, rendering parameters, typography, number substitution, the ellipsis trimming sign), the CSS-style font matching, and the layout
(clusters, fallback to the wide-coverage face, kerning, line breaking at spaces/hyphens/ideographs, wrapping modes, alignment incl. justification, tab stops, trimming, line
spacing, hit testing, `Draw` through the program's `IDWriteTextRenderer`). The system collection lists the families the bundled faces stand in for (Arial, Segoe UI, Times New Roman,
Courier New, Consolas, Liberation *, TCOS *, ...). A font file a program refers to (path or its own loader) is added to the kernel's fonts once and then is an ordinary face; a
system face's file has a loader that streams the kernel's copy and answers a synthetic path (`C:\Windows\Fonts\tcos-N.ttf`).

Not done: bidirectional reordering and complex-script shaping (clusters are code points plus combining marks, kerning from the `kern` table, no GSUB/GPOS/ligatures; a right-to-left
reading direction only mirrors the alignment), font simulations, colour and variable fonts, CFF outlines, the DirectWrite 1.1+ interfaces (`IDWriteFactory1`..), ClearType sub-pixel
rendering (the 3x1 texture carries grey coverage). Tests: `test_*` in `tests/WindowsFontTest.cpp` (the service, host), `tests/windows/dwritetest.c` (228 checks through the real DLL
in QEMU: `scripts/winetest.sh dwrite own`); Wine's analyzer/font tests are mostly about bidi, scripts and Windows 10 interfaces and still fail (157 / 325 checks).

## gdi32.dll

`src/Windows/gdi32` is a real library: the GDI API of wingdi.h (A and W forms, DCs and their attributes, pens, brushes, bitmaps, DIBs, regions, clipping, palettes, paths, text and
fonts, enumeration, printing entry points, the Direct3D kernel-mode thunks, WMF and EMF recording and playback in `meta.inc`) as a thin layer over the NtGdi* services of the kernel's
GDI module (`docs/win32k/Gdi.md`), the design of Wine's gdi32. Its list of stubs (`stubs/gdi32.inc`) holds what it does not implement: mostly Windows-internal names (`Eng*`, `Script*`, ...).

- The services are called through local system-call stubs (not through win32u.dll). A service the kernel has no handler for answers STATUS_NOT_IMPLEMENTED; the stub turns that into 0 (FALSE / NULL).
- Handles are the kernel's (`generation << 24 | stock << 23 | type << 16 | index`). The stock objects have fixed handles, computed by `GetStockObject` from the order in which the kernel makes
  them (`Win32kGdiTest` checks the table). Metafiles and metafile DCs are client objects of the kernel (type 0x20) that gdi32 keeps the records of.
- The kernel keeps all DC attributes: gdi32 reads and sets them with the methods of `NtGdiGetDCDword` / `NtGdiGetAndSetDCDword` / `NtGdiGetDCPoint` listed in `docs/win32k/Gdi.md`. The mapper flags
  and the pixel format number are remembered by gdi32 (the kernel has no call to read them).
- user32.dll has `GetDC` / `GetDCEx` / `GetWindowDC` / `ReleaseDC` (win32k's NtUserGetDC: a null window is the screen), `GetDesktopWindow`, `GetSysColor` and `GetSysColorBrush`.
- Tests: Wine's gdi32 tests (`scripts/winetest.sh gdi32`). They still fail in many places; most of the failures are in the kernel's GDI module (palettes, system colours, DDB details,
  fonts), some in the stubs: `AddFontResource`, `GetGlyphOutline`, printing and the like are not in the kernel module yet. `SelectPalette` / `RealizePalette` need `NtUserSelectPalette` /
  `NtUserRealizePalette`, which the kernel does not have; a DIB section on a file mapping is refused by the kernel.
