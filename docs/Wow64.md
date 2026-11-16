# 32-bit Windows programs (WoW64)

A 32-bit Windows program (a PE32 image for i386) runs beside the 64-bit ones. This document says how, what works today, and what is still to do.
It is work in progress: see "Status".

## How it works

**The CPU.** An x86-64 CPU runs 32-bit code as it is, in *compatibility mode*: the same long mode, with a code segment whose descriptor says "32-bit".
No emulator is involved on an x86-64 machine. (On ARM and RISC-V machines the x86 emulator would need a 32-bit mode; that is not done.)

- The per-core GDT has a 32-bit user code segment (`kSelectorUserCode32`, src/Kernel/UserEntry.hpp). A thread of a 32-bit program enters ring 3 through
  it.
- Compatibility mode checks the data segments, which long mode ignores: DS, ES and FS are loaded with the flat user data segment once per core.
- FS is the thread's 32-bit TEB (its base comes from the FS base register, switched with the thread, as GS is for a 64-bit program).

**The address space.** `AddressSpace::set_top` gives a 32-bit program a ceiling just below 2 GiB (`kUserTop32`), as Windows does. Nothing is mapped
above it, and a place asked for above it (the kernel's placement hints are made for 64-bit programs) is found low instead. `is_32bit()` is what the
loaders ask.

**System calls.** There is one set of NT services (src/Process/Windows*.cpp), written for 64-bit programs. A 32-bit program reaches them through
`INT 2E`:

- EAX is the service number; EDX points at the arguments, **each a 64-bit value**, and structures are laid out as the 64-bit services read them
  (`decode_x86_32_windows`, src/Process/Syscall.hpp).
- The 32-bit `ntdll.dll` is what makes that so: its `Nt*` functions take what a 32-bit program passes, widen it, and convert the few structures whose
  layout depends on the pointer size. This is the role of `wow64.dll` on Windows, done here inside the 32-bit ntdll.
- So the kernel's services need no second variant. (Planned exception: the three structures nearly every service takes - `UNICODE_STRING`,
  `OBJECT_ATTRIBUTES`, `IO_STATUS_BLOCK` - are read through a few helpers in the kernel, which can take the 32-bit layout from a 32-bit program; an
  I/O status block is written when an operation completes, possibly long after the call, so it cannot be a temporary of a thunk.)

**Two TEBs.** As under WoW64, a thread of a 32-bit program has the TEB the kernel's services use (64-bit layout, the GS base) and, 0x2000 bytes after
it, the 32-bit TEB the program sees (the FS base). The kernel fills in what code may read before ntdll has run: the stack bounds, the ids, the self
pointer, the exception chain's end, and the thread-local storage pointer. The 32-bit PEB, process parameters and loader lists are the 32-bit ntdll's
to make, from the 64-bit ones.

**What a 32-bit program is told about itself.** As under WOW64: `IsWow64Process` says yes (`ProcessWow64Information` is nonzero for the calling
process), `GetSystemInfo` describes a 32-bit processor and `GetNativeSystemInfo` the real one, and turning file-system redirection off and on
succeeds (nothing is redirected here: System32 is System32 for a 32-bit program too, and its libraries are found in SysWOW64 by the loader).

**Where a 32-bit program's system libraries are.** In `C:\Windows\SysWOW64`. A library asked for by the path of `C:\Windows\System32`
(`GetSystemDirectory` answers that for every program) is the one of that name in SysWOW64, and is "not found" when there is none there - the file
in System32 is a 64-bit library. That rule is in the kernel's load service and in kernel32's search (`wow_system_file`); other files are not
redirected. The built-in assemblies a manifest may depend on (Common Controls 6, the old C runtimes) are x86 ones for a 32-bit program, so its
manifest activates - and with it the Windows version it declares support for (`GetVersionEx` answers 6.2 to a program whose manifest does not
activate).

**Loading.** `bind_executable` gives a PE32/i386 program the architecture `UserArch::X86_32`. The image loader and the module loader map PE32 images
in a 32-bit address space: 32-bit import slots, image base, security cookie, TLS directory. A 32-bit program's libraries are the 32-bit ones, from
`C:\Windows\SysWOW64` (`ILibraryProvider::find_32bit`); a library it loads by its path while it runs (a setup program's plug-in in a folder of
temporary files) is that file.

**The libraries.** The sources of the libraries (src/Windows) are built a second time, for i386, into `C:\Windows\SysWOW64`
(scripts/build-wow64-dlls.sh, run by the build when `i686-w64-mingw32-gcc` is there): all of them but eight - ntdll, kernel32, msvcrt, advapi32,
win32u, gdi32, user32, comctl32, version, oleaut32, ole32, shlwapi, shcore and shell32, which are tested, and 66 more that are there so that a
program importing them loads (wtsapi32, userenv, comdlg32, ws2_32, crypt32, uxtheme, shfolder, ...). Of those 66 nothing has been run: the ones
that are stubs or work through kernel32 and advapi32 can be expected to do what their 64-bit forms do; the ones that hand the kernel structures of
their own (comdlg32's dialogs, bcrypt, dwrite, opengl32, the Direct3D libraries, ws2_32's sockets) pass them in a 32-bit layout the kernel does
not read. Not built, because their sources do not compile for i386 yet: winmm, dsound, mmdevapi, xaudio2_8, xaudio2_9, imm32, dbghelp, vulkan-1.

- **Exports that are not implemented** are stubs, as in the 64-bit libraries, but a stdcall stub has to take its arguments off the stack: the
  sizes come from the import libraries i686-w64-mingw32 ships (scripts/gen-wow64-stubs.py). The NT services nothing here has a prototype for are
  exported the same way and answer STATUS_NOT_IMPLEMENTED (src/Windows/ntdll/wow64_unthunked.inc).
- **A library's .def** (forwarders, ordinals) is rewritten with the decorated names of the i386 build (scripts/gen-wow64-def.py).
- **The window system** (win32k's `NtUser*` and `NtGdi*` services) is reached through one piece of code per library
  (src/Windows/tcos_wow64_win32k.h): the stubs are cdecl, and every argument is widened with its sign, which is right for pointers (below 2 GiB),
  for handles like `HWND_TOPMOST`, and for 32-bit values (the kernel takes their low half). user32.dll and gdi32.dll are the same sources for
  both widths and hand a service what the program gave them, so **the structures with handles or pointers in them are converted in the 32-bit
  win32u.dll** (src/Windows/win32u/wow64.inc): `UNICODE_STRING`, `WNDCLASSEX`, `MSG`, `PAINTSTRUCT`, `GUITHREADINFO`, the parameter blocks of
  `NtUserCallHwndParam`, the window list of `NtUserBuildHwndList`, and the structures of the messages that carry one (`CREATESTRUCT`,
  `WINDOWPOS`, `NCCALCSIZE_PARAMS`, `COPYDATASTRUCT`) in both directions: when the kernel has a window procedure called, and when the program
  sends such a message or passes it to `DefWindowProc`. And the rest of the window system: menus (`MENUITEMINFO`, `MENUINFO`), icons and cursors
  (`ICONINFO`, `CURSORINFO`, the frames of a cursor being made), the clipboard's blocks, the lists of keyboard layouts and input contexts, raw
  input (`RAWINPUTDEVICE`, `RAWINPUTDEVICELIST`, `RAWINPUTHEADER`), `INPUT` of `SendInput`, `TRACKMOUSEEVENT`, the pointer-sized actions of
  `SystemParametersInfo`, the device names of the display services, and `OBJECT_ATTRIBUTES` of window stations and desktops.
- **gdi32.dll** calls its services itself, so its three that differ are converted there (src/Windows/gdi32/wow64.inc): `GetObject` of a bitmap,
  a DIB section, a brush or an extended pen (`BITMAP`, `DIBSECTION`, `LOGBRUSH`, `EXTLOGPEN`), the bits' address `CreateDIBSection` gives, and
  the names of `CreateDC`.
- **In user32.dll** a few things follow the pointer size: a dialog's procedure and user data are at 4 and 8 of its window's extra bytes
  (`DWLP_DLGPROC`, `DWLP_USER`), and `Get/SetWindowLongPtr` of the extra bytes reads and writes 4 bytes; the first hook of a chain is asked for
  with bit 30 set where a 64-bit program sets bit 32; a moveable memory handle (`GlobalAlloc`, kernel32) keeps its shape.

- `tcos_win.h` gives the pointer-sized integers, the calling convention (`WINAPI`, `NTAPI`, `CALLBACK` are `__stdcall` on i386), the TEB accessors
  and a few structures their 32-bit form under `__i386__`. The structures are written with the natural pointer types, so the PEB, the process
  parameters and the loader lists come out in their real 32-bit layouts.
- What the libraries keep in the TEB and PEB by raw x64 offsets (their own state, and what the kernel reads) goes through `TEB_RAW` / `PEB_RAW`,
  which name the 64-bit TEB and PEB in a 32-bit program.
- **The `Nt*` functions** of the 32-bit ntdll are C functions generated from the services' prototypes by scripts/gen-wow64-thunks.py
  (src/Windows/ntdll/wow64_services.inc): a handle is sign-extended, an integer or pointer zero-extended, a pointer to a pointer-sized value
  (`PHANDLE`, `PVOID *`, `PSIZE_T`) goes through a 64-bit copy, and `OBJECT_ATTRIBUTES`, `UNICODE_STRING` and `CLIENT_ID` are passed in their
  64-bit layouts. The services whose buffers have a layout of their own are written by hand in src/Windows/ntdll/wow64.inc
  (`NtQueryVirtualMemory`, the basic information of a process and of a thread, the handle array of `NtWaitForMultipleObjects`, the token
  service, and the classes of `NtSetInformationFile` that name a file - a rename or a hard link carries a handle before the name - or a
  completion port). An I/O status
  block is the one structure the kernel writes in the caller's layout, because it may be written long after the call.
- **Start-up**: `RtlUserThreadStart` first makes the 32-bit PEB, process parameters and loader lists from the 64-bit ones the kernel wrote
  (`wow64_start_process`), then goes on as in a 64-bit program. A later thread's `TcosThreadStart` points its 32-bit TEB at that PEB.
- The functions are exported by their plain names (`--kill-at`); a library that imports them links against an import library made from the
  decorated names (`dlltool -k`).
- **Exceptions** (src/Windows/ntdll/except32.inc) are the i386 mechanism, which is not the x64 one: a function with a handler links a record on its
  stack at `FS:[0]`, and there are no unwind tables.
  - The kernel delivers a fault of the processor with an **i386 `CONTEXT`** (716 bytes) and a 32-bit `EXCEPTION_RECORD` on the program's stack, and
    starts `KiUserExceptionDispatcher` with their two addresses on the stack (src/Process/WindowsException.cpp). `NtContinue`, `NtGetContextThread`
    and `NtSetContextThread` read and write the same layout for a 32-bit program, with the same checks as for a 64-bit one (addresses below
    2 GiB, no privileged flags, the usable bits of MXCSR). The x87 state is given both ways a 32-bit `CONTEXT` has it: the FSAVE layout
    (`FloatSave`) and the FXSAVE image (`ExtendedRegisters`).
  - `RtlDispatchException` offers the exception to the vectored handlers, then to each frame's handler from the newest. A handler runs inside a
    frame of the dispatcher's own, so an exception inside a handler is seen and marked `EXCEPTION_NESTED_CALL`. What no frame handles goes to the
    unhandled-exception filter (kernel32's `UnhandledExceptionFilter`), and then the process ends with the exception's code.
  - `RtlUnwind` calls the handlers of the frames above the target with `EXCEPTION_UNWINDING`, takes their records off the list, and returns.
  - `RtlRaiseException` captures the caller's registers, so a handler that says "continue" goes back with all of them.
  - msvcrt.dll has the handlers of the Microsoft compilers' `__try` frames (src/Windows/msvcrt/seh32.inc): `_except_handler3`,
    `_except_handler4_common`, `_local_unwind2/4`, `_global_unwind2`, `_abnormal_termination`. A program that links the C runtime statically
    brings its own and needs only `RtlUnwind`.
  - msvcrt.dll also has the 32-bit compiler's **C++ exceptions**: `_CxxThrowException` and `__CxxFrameHandler` (try blocks, catch by type, by
    reference and `catch (...)`, the destructors of the function's objects, `throw;`), and the operators and data an old C++ library imports
    (`operator new` and `delete`, `type_info`'s destructor, `_adjust_fdiv`; their compiler-made names are in src/Windows/msvcrt/msvcrt32.def).

## Status

Done, and run in QEMU (`tests/wow64_qemu.sh`):

- **The kernel half**: a PE32 program with no imports (tests/windows/hello32.c) is loaded below 2 GiB, runs in compatibility mode, finds its 32-bit
  TEB at FS, calls NT services through INT 2E, and bad argument blocks are refused.
- **A program with a C runtime** (tests/windows/crt32.c, built with `i686-w64-mingw32-gcc` the ordinary way) runs on the 32-bit ntdll.dll,
  kernel32.dll and msvcrt.dll: the runtime's start-up, `printf`, the heap, files, the environment, `VirtualAlloc` / `VirtualQuery`, thread-local
  storage, critical sections, four threads and waiting for them, events, `Sleep`. All 21 of its checks pass.
- **The other libraries** (tests/windows/gui32.c): the registry; the process's token; `wsprintf` and the character functions;
  `InitCommonControls`; `OleInitialize`, the shell's folders, a shortcut made through `IShellLink` and saved through `IPersistFile`; then a
  window class, a window with its creation messages, its text, rectangles and window longs, `SetWindowPos`, sent and posted messages, a timer, a
  device context; a `WH_CALLWNDPROC` hook; `EnumWindows`, the monitors, the keyboard layouts, the display mode, `CreateDC`; a menu with
  `MENUITEMINFO` and `MENUINFO`; `GetObject` of a bitmap, a DIB section and a brush, and the bits of a DIB section; an icon and the cursor; the
  clipboard; `SendInput` and `TrackMouseEvent`; a dialog with its procedure and user data; `DestroyWindow` and `WM_QUIT`. All 70 of its checks
  pass.
- **Exceptions** (tests/windows/seh32.c): faults (bad reads and writes, division by zero, a breakpoint) reach the handler of a frame with the right
  record and registers, and the program goes on from a changed `CONTEXT`; the x87 and SSE registers are in the `CONTEXT` and come back; `RtlUnwind`;
  an exception that cannot be continued; an exception inside a handler; vectored handlers; the unhandled-exception filter; a `__try` frame handled
  by `_except_handler3` (the frame is made by hand, as no compiler here has `__try` for i386); a fault in a second thread; `GetThreadContext` and
  `SetThreadContext`. All 20 of its checks pass.

Not done yet, in the order planned:

1. **What is left of the window system.** Not converted, so not working in a 32-bit program: touch and pointer input (`POINTER_TOUCH_INFO`,
   `TOUCHINPUT`), `GetRawInputBuffer`, the input method's `IMEINFOEX` (imm32 is not built for i386), the DWM thumbnail and composition
   attribute services, `NtGdiPolyTextOutW` and `NtGdiPolyPatBlt` when called through win32u (gdi32 does not use them), and every Direct3D
   service (their structures are full of pointers; no Direct3D library is built for i386). A message with a structure sent between a 32-bit and
   a 64-bit program is not converted. comctl32 has no controls yet (for 64-bit programs either), and comdlg32, imm32, winmm and the other
   libraries are not built for i386.
2. **The long tail in ntdll, kernel32 and msvcrt**: the services ntdll has no prototype for are stubs; information classes with
   pointers in them (`NtQuerySystemInformation`, `NtQueryObject`) are passed unconverted; the old runtime's `_iob` array has the wrong stride
   (a stream is 40 bytes here, 32 in a 32-bit program built against the old runtime).
3. **What exceptions still lack**: C++ exceptions are there for 32-bit programs only (a 64-bit msvcrt.dll has neither `__CxxFrameHandler` nor
   `_CxxThrowException`), without exception specifications and without tests of their own (a real setup program's plug-in is what has run them);
   the stack cookies of `_except_handler4` are not checked; `setjmp`/`longjmp` do not unwind; a 32-bit program is not
   debugged (no debug events for it); a stack overflow ends the program (no guard-page exception on the 32-bit stack).
4. **Setup programs**: Nullsoft (NSIS) and Inno Setup programs run when they install without showing themselves, and the App Manager runs
   them (docs/AppManager.md): Notepad++'s and qBittorrent's (NSIS), WinMerge's (Inno Setup 5) and TreeSize Free's (Inno Setup 6) have installed
   and been removed in QEMU. A 32-bit setup program of no known kind ("exe") is still refused. What an installed 32-bit program then needs is
   another matter: TreeSize Free, started by its own setup program, faulted. oleaut32 parses numbers exactly only up to 2^62 in a 32-bit program
   (i386 has no 128-bit integers).
5. **Other machines**: on ARM and RISC-V the x86 emulator (src/Emulation, src/EmulationJit) would need a 32-bit mode; nothing is done there.

## Finding out why a program gives up

Three things made the setup programs above tractable, and stay in the tree:

- `-DTCOS_TRACE_SERVICE_FAILURES=ON` (a kernel build option): every NT service of a Windows program that answers an error status, and every error a
  window service sets, goes to the debug console with the service's number (src/Windows/tcos_syscalls.h).
- The kernel's log of a program's faults names the function when the fault is a call of a function no library provides
  ("it called a function that is not there: WTSAPI32.DLL!..."), says which library it would not load and why, and shows the bytes of an
  instruction the processor refused.
- `WOW64_EXTRA_FLAGS=-DTCOS_HEAP_CHECK` (an environment variable of the build): the 32-bit ntdll walks a heap at every allocation and release, and
  at the first damage faults on purpose with a write to the address the previous heap call was made from - the damage was done after that call.
  (This found gdi32's font enumeration writing 452 bytes an entry into a buffer sized by a count of entries: a bug for 64-bit programs too.)

A setup program's own log is the other half: the App Manager asks Inno Setup for one and quotes it when the setup program fails.

## Tests

- `tests/run_host_tests.sh SyscallTest LoaderTest`: binding a PE32 program, the INT 2E argument convention, the address-space ceiling.
- `tests/run_host_tests.sh WindowsExceptionTest`: the i386 `CONTEXT` and record the kernel writes and reads, with truncated and bit-flipped
  contexts.
- `tests/wow64_qemu.sh [hello32] [crt32] [seh32] [gui32]`: the four programs above in the real kernel, each in a boot of its own.
