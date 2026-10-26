# Windows programs: libraries and system services


Windows (PE) programs run in ring 3 on `ntdll.dll`, `kernel32.dll` and `msvcrt.dll`, freestanding DLLs built from `src/Windows/` and put on the primary disk image in
`C:\Windows\System32` (see [FileSystemLayout.md](FileSystemLayout.md)). ntdll turns the NT services into `SYSCALL`s. The service numbers are this OS's own (real Windows
renumbers its services in nearly every build, which is why programs call ntdll); `src/Windows/tcos_syscalls.h` and `src/Process/WindowsSyscalls.hpp` hold the numbers and
`tests/SyscallTest.cpp` checks that they agree. The services behind the system calls are in `src/Process/Windows*.cpp`; the kernel side is `src/Kernel/UserMode.cpp`.

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

## Exceptions and the machine

| Service | Number |
| --- | --- |
| `NtContinue` | 22 |
| `NtRaiseException` | 23 |
| `NtQuerySystemInformation` | 46 |

## How things behave

- **Threads** of one program run on all the cores, each on the page tables of the program; changes to the address space reach the other cores before the memory is given back
  (`Task::sync_tables`). Affinity masks (`SetThreadAffinityMask`, `SetProcessAffinityMask`) are honoured by the scheduler. Priorities and priority classes are kept and read
  back but do not change the scheduling (it is round-robin).
- **Processes**: `CreateProcess` starts another program with its own address space; handles can be inherited (`bInheritHandles`), the standard handles redirected
  (`STARTF_USESTDHANDLES`), the child waited for and terminated. A child ends when the program that started it ends. `OpenProcess` gives access to the exit status and the wait.
- **Libraries**: `LoadLibrary`/`FreeLibrary` map and unmap DLLs after start-up, with their imports, reference counted; `DllMain` and TLS callbacks run. A DLL's own
  `__declspec(thread)` data works (up to 16 modules with thread-local data at once).
- **Files**: a read-only attribute and the times can be set (RAM and FAT file systems); other attributes are accepted and not stored. Byte-range locks are advisory and
  shared by all programs. File mappings of files are read into memory when made and written back when flushed, when the last view is closed and when the mapping is.
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
- **Not provided**: debugging APIs, memory of
  other processes, remote threads, mailslots, change notifications, hard and symbolic links, and the many exports of the real kernel32 and msvcrt that no program here has needed.
  A call to an import that nothing provides stops the program with a message saying which one.
