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

**Loading.** `bind_executable` gives a PE32/i386 program the architecture `UserArch::X86_32`. The image loader and the module loader map PE32 images
in a 32-bit address space: 32-bit import slots, image base, security cookie, TLS directory. A 32-bit program's libraries are the 32-bit ones, from
`C:\Windows\SysWOW64` (`ILibraryProvider::find_32bit`).

**The libraries.** The sources of the libraries (src/Windows) are built a second time, for i386, into `C:\Windows\SysWOW64`
(scripts/build-wow64-dlls.sh, run by the build when `i686-w64-mingw32-gcc` is there): ntdll, kernel32, msvcrt, advapi32, win32u, gdi32, user32,
comctl32, version, oleaut32, ole32, shlwapi, shcore and shell32.

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
  sends such a message or passes it to `DefWindowProc`.

- `tcos_win.h` gives the pointer-sized integers, the calling convention (`WINAPI`, `NTAPI`, `CALLBACK` are `__stdcall` on i386), the TEB accessors
  and a few structures their 32-bit form under `__i386__`. The structures are written with the natural pointer types, so the PEB, the process
  parameters and the loader lists come out in their real 32-bit layouts.
- What the libraries keep in the TEB and PEB by raw x64 offsets (their own state, and what the kernel reads) goes through `TEB_RAW` / `PEB_RAW`,
  which name the 64-bit TEB and PEB in a 32-bit program.
- **The `Nt*` functions** of the 32-bit ntdll are C functions generated from the services' prototypes by scripts/gen-wow64-thunks.py
  (src/Windows/ntdll/wow64_services.inc): a handle is sign-extended, an integer or pointer zero-extended, a pointer to a pointer-sized value
  (`PHANDLE`, `PVOID *`, `PSIZE_T`) goes through a 64-bit copy, and `OBJECT_ATTRIBUTES`, `UNICODE_STRING` and `CLIENT_ID` are passed in their
  64-bit layouts. The services whose buffers have a layout of their own are written by hand in src/Windows/ntdll/wow64.inc
  (`NtQueryVirtualMemory`, the basic information of a process and of a thread, the handle array of `NtWaitForMultipleObjects`). An I/O status
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

## Status

Done, and run in QEMU (`tests/wow64_qemu.sh`):

- **The kernel half**: a PE32 program with no imports (tests/windows/hello32.c) is loaded below 2 GiB, runs in compatibility mode, finds its 32-bit
  TEB at FS, calls NT services through INT 2E, and bad argument blocks are refused.
- **A program with a C runtime** (tests/windows/crt32.c, built with `i686-w64-mingw32-gcc` the ordinary way) runs on the 32-bit ntdll.dll,
  kernel32.dll and msvcrt.dll: the runtime's start-up, `printf`, the heap, files, the environment, `VirtualAlloc` / `VirtualQuery`, thread-local
  storage, critical sections, four threads and waiting for them, events, `Sleep`. All 21 of its checks pass.
- **The other libraries** (tests/windows/gui32.c): the registry; `wsprintf` and the character functions; `InitCommonControls`; `OleInitialize`,
  the shell's folders, a shortcut made through `IShellLink` and saved through `IPersistFile`; then a window class, a window with its creation
  messages, its text, rectangles and window longs, `SetWindowPos`, sent and posted messages, a timer, a device context, `DestroyWindow` and
  `WM_QUIT`. All 37 of its checks pass.
- **Exceptions** (tests/windows/seh32.c): faults (bad reads and writes, division by zero, a breakpoint) reach the handler of a frame with the right
  record and registers, and the program goes on from a changed `CONTEXT`; the x87 and SSE registers are in the `CONTEXT` and come back; `RtlUnwind`;
  an exception that cannot be continued; an exception inside a handler; vectored handlers; the unhandled-exception filter; a `__try` frame handled
  by `_except_handler3` (the frame is made by hand, as no compiler here has `__try` for i386); a fault in a second thread; `GetThreadContext` and
  `SetThreadContext`. All 20 of its checks pass.

Not done yet, in the order planned:

1. **The rest of the window system's structures.** These are still passed to the kernel in the 32-bit layout, so the services that take them
   do not work in a 32-bit program yet: menus (`MENUITEMINFO`, `MENUINFO`), icons and cursors (`ICONINFO`), `SendInput` (`INPUT`),
   `TrackMouseEvent`, hooks (the hook's module name, and the structures a hook procedure is given), the clipboard, keyboard layouts, monitor
   enumeration, `SetScrollInfo`'s block, and in GDI `GetObject` of a bitmap (`BITMAP`, `DIBSECTION`). Dialogs keep their procedure at the 64-bit
   offset of the window's extra bytes (`DWLP_DLGPROC` is 4, not 8, in a 32-bit program), and comctl32 has no controls yet (for 64-bit programs
   either). A message with a structure sent between a 32-bit and a 64-bit program is not converted.
2. **The long tail in ntdll, kernel32 and msvcrt**: the services ntdll has no prototype for are stubs; information classes with
   pointers in them (`NtQuerySystemInformation`, `NtQueryObject`) are passed unconverted; a 32-bit program cannot start another program
   (`RtlCreateProcessParameters` writes the 64-bit layout); `%p` prints 16 digits; the old runtime's `_iob` array has the wrong stride.
3. **What exceptions still lack**: C++ exceptions of the Microsoft runtime (`__CxxFrameHandler`, `_CxxThrowException`: msvcrt.dll has neither, for
   64-bit programs either); the stack cookies of `_except_handler4` are not checked; `setjmp`/`longjmp` do not unwind; a 32-bit program is not
   debugged (no debug events for it); a stack overflow ends the program (no guard-page exception on the 32-bit stack).
4. **Setup programs**: the App Manager still refuses 32-bit setup programs (docs/AppManager.md). The libraries they import are there and a
   program like tests/windows/gui32.c runs; no real setup program has been run yet. oleaut32 parses numbers exactly only up to 2^62 in a 32-bit
   program (i386 has no 128-bit integers).
5. **Other machines**: on ARM and RISC-V the x86 emulator (src/Emulation, src/EmulationJit) would need a 32-bit mode; nothing is done there.

## Tests

- `tests/run_host_tests.sh SyscallTest LoaderTest`: binding a PE32 program, the INT 2E argument convention, the address-space ceiling.
- `tests/run_host_tests.sh WindowsExceptionTest`: the i386 `CONTEXT` and record the kernel writes and reads, with truncated and bit-flipped
  contexts.
- `tests/wow64_qemu.sh [hello32] [crt32] [seh32] [gui32]`: the four programs above in the real kernel, each in a boot of its own.
