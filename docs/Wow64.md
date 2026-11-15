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

**The libraries.** The plan is to build the existing sources (src/Windows) a second time, for i686, into SysWOW64. They are written with the natural
pointer types, so the PEB, the loader lists and the like come out in their real 32-bit layouts, and every exported function already names its calling
convention (`WINAPI`, `NTAPI`), which becomes `__stdcall` on i386.

## Status

Done, and run in QEMU (`tests/wow64_qemu.sh`):

- The kernel half: a PE32 program with no imports (tests/windows/hello32.c) is loaded below 2 GiB, runs in compatibility mode, finds its 32-bit TEB
  at FS, calls NT services through INT 2E (writes to the console, allocates memory, ends with an exit code), and bad argument blocks are refused.

Not done yet, in the order planned:

1. **The 32-bit ntdll.dll, kernel32.dll and msvcrt.dll**: the header for both widths, the generated `Nt*` thunks, ntdll's start-up for a 32-bit
   process, 32-bit exception handling (the frame-based SEH of i386 is not the table-based one of x64), the build into SysWOW64. Goal: a program built
   with a C runtime prints and exits.
2. **Threads, exceptions and `NtContinue` for 32-bit programs** in the kernel (a fault in a 32-bit program ends it today; there is no 32-bit
   `CONTEXT`).
3. **The other libraries** (user32, gdi32, advapi32, shell32, ole32, comctl32, ...) and the win32k services' structures; the stub libraries need the
   argument sizes of their functions (stdcall: the callee cleans the stack).
4. **Setup programs**: the App Manager refuses 32-bit setup programs until then (docs/AppManager.md).

## Tests

- `tests/run_host_tests.sh SyscallTest LoaderTest`: binding a PE32 program, the INT 2E argument convention, the address-space ceiling.
- `tests/wow64_qemu.sh`: the program above in the real kernel.
