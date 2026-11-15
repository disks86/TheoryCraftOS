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
(scripts/build-wow64-dlls.sh, run by the build when `i686-w64-mingw32-gcc` is there): ntdll.dll, kernel32.dll and msvcrt.dll so far.

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
- **Exceptions** (src/Windows/ntdll/except32.inc): only a beginning. An exception raised by software is offered to the vectored handlers and the
  unhandled-exception filter and then ends the process; the frame-based handlers of i386 (`FS:[0]`, `RtlUnwind`) are not written, and a fault of
  the processor ends the program in the kernel.

## Status

Done, and run in QEMU (`tests/wow64_qemu.sh`):

- **The kernel half**: a PE32 program with no imports (tests/windows/hello32.c) is loaded below 2 GiB, runs in compatibility mode, finds its 32-bit
  TEB at FS, calls NT services through INT 2E, and bad argument blocks are refused.
- **A program with a C runtime** (tests/windows/crt32.c, built with `i686-w64-mingw32-gcc` the ordinary way) runs on the 32-bit ntdll.dll,
  kernel32.dll and msvcrt.dll: the runtime's start-up, `printf`, the heap, files, the environment, `VirtualAlloc` / `VirtualQuery`, thread-local
  storage, critical sections, four threads and waiting for them, events, `Sleep`. All 21 of its checks pass.

Not done yet, in the order planned:

1. **Exceptions for 32-bit programs**: frame-based SEH in ntdll (`FS:[0]` handlers, `RtlUnwind`, `_except_handler3/4` in msvcrt), and in the
   kernel a 32-bit `CONTEXT`, the delivery of faults to `KiUserExceptionDispatcher`, `NtContinue`, `Get/SetThreadContext`.
2. **The other libraries** (advapi32, user32, gdi32, win32u, shell32, ole32, comctl32, ...) built for i386, with the win32k services' structures
   converted; the stub libraries need the argument sizes of their functions (stdcall: the function takes its arguments off the stack), which the
   32-bit import libraries of mingw give.
3. **The long tail in the three libraries that are built**: the services ntdll has no prototype for are not exported; information classes with
   pointers in them (`NtQuerySystemInformation`, `NtQueryObject`) are passed unconverted; a 32-bit program cannot start another program
   (`RtlCreateProcessParameters` writes the 64-bit layout); `%p` prints 16 digits; the old runtime's `_iob` array has the wrong stride.
4. **Setup programs**: the App Manager refuses 32-bit setup programs until 1 and 2 are done (docs/AppManager.md). They are GUI programs that use
   user32, gdi32, comctl32, shell32 and ole32.
5. **Other machines**: on ARM and RISC-V the x86 emulator (src/Emulation, src/EmulationJit) would need a 32-bit mode; nothing is done there.

## Tests

- `tests/run_host_tests.sh SyscallTest LoaderTest`: binding a PE32 program, the INT 2E argument convention, the address-space ceiling.
- `tests/wow64_qemu.sh [hello32] [crt32]`: the two programs above in the real kernel, each in a boot of its own.
