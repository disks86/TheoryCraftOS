# Just-in-time compilation (`src/Jit`)

Shared infrastructure for the places that generate machine code at run time: the architecture emulator (`src/Emulation`, x86-64 / AArch64 /
RISC-V guests, not wired in yet) and the JavaScript / WebAssembly virtual machines of the browser (`src/Script`, `src/Wasm`, wired in: see
"The browser's VMs" below). This is the pipeline they plug into.

```
guest code --IJitFrontend--> IrBlock --IJitBackend--> machine code --ICodeMemory--> CompiledBlock --CodeCache--> run
 (per guest)    translate()    (Ir/)     compile()      (CodeBuffer)   W^X, i-cache      (RCU)       JitDispatcher
```

| Folder / file | What it is |
| --- | --- |
| `CodeMemory.hpp` | `IInstructionCacheSync` + `CrossCoreInstructionSync`: make every core's instruction fetch see new code (RISC-V FENCE.I, Arm ISB; x86 relies on the TLB shootdown IPI). `ICodeMemory`: allocate writable, switch to executable (W^X, i-cache flush), release. `HeapCodeMemory` never executes (cross-compiling, tests). `KernelCodeMemory` is the kernel one: whole 4 KiB pages from the allocator whose permissions are flipped in `kernel::PageTables` (W^X), pages come from a `CodePool` (2 MiB windows taken on first use, so only those windows are split into 4 KiB pages) or straight from the allocator; `CrossCoreTlbShootdown` flushes the other cores through `run_on_every_core` (IPI); the hosted test one is `tests/JitHost.hpp` (mmap). |
| `Emitter/CodeBuffer` | bounds-checked byte emission, labels, fixups (x86 rel32, AArch64 imm26/imm19, RISC-V J/B) resolved by `finish()`. |
| `Emitter/X86_64Assembler`, `AArch64Assembler`, `RiscV64Assembler` | integer-core encoders, every encoding checked against `llvm-mc` output in `JitTest`. No SIMD/FP yet. |
| `Ir/Ir` | small 64-bit integer IR on virtual registers (not SSA), guest state in a memory block, helpers for guest memory access, in-block labels and branches, `Exit` values. `LoadPtr` / `StorePtr` read and write memory through an address in a register (8/16/32/64 bits, zero or sign extended; the front end bounds checks first), and double precision `FAdd` / `FSub` / `FMul` / `FDiv` / `FSetCond` / `I2F` / `F2I` work on the bit pattern of a double held in a register, `F2D` / `D2F` widen and narrow a single precision float (the low 32 bits of a register). `F2I` gives INT64_MIN for a NaN and for anything that does not fit; every back end makes it so (x86 does it natively; AArch64 turns a NaN into 0 and saturates, RISC-V gives INT64_MAX, and their lowering fixes that up). A NaN produced by arithmetic has a different bit pattern on each CPU (x86 negative quiet, AArch64 positive quiet, RISC-V the canonical one): front ends must not depend on it. `validate()` for untrusted front ends, `interpret()` as the reference and the tier-below-JIT. |
| `Ir/RegisterAllocator` | linear-scan register allocation over the IR: intervals widened over loops, zero-at-entry for registers read before a certain write, call-crossing values only in callee-saved registers, furthest-end spilling, spill slots reused once their interval ends, and coalescing hints (a result takes the register of an operand that dies there, so a Move costs nothing and x86 computes in place). Shared by all three back ends (each supplies its register pool). |
| `Backend/IJitBackend` | IR -> machine code for one host CPU. All three back ends lower pointer access and double precision floating point (SSE2, the AArch64 scalar FP instructions, the RISC-V D extension); the AArch64 and RISC-V ones take a `floating_point` flag, and `JitRuntime` turns it off for them because those kernels neither enable nor save the floating point registers across a task switch (pointer access has no such need). `X86_64Backend` (r12-r15, r10-r11 for values; SysV and Win64, the kernel is a PE image: Win64). `AArch64Backend` for AArch64 (AAPCS64; x20-x28 and x9-x15 for values, x19 = state, x16 = helper address; up to 4095 spill slots per block), `RiscV64Backend` likewise for RV64IM (s2-s11, t3-t6, a3-a7 for values, s1 = state, t2 = helper address, jumps within ±1 MiB). Both are run for real under qemu-aarch64 / qemu-riscv64 by `AArch64JitTest` / `RiscV64JitTest`. |
| `CodeCache` | direct-mapped guest-address -> `CompiledBlock` table published with RCU (`lookup` never blocks; replaced/invalidated code is freed after a grace period). `HotnessTracker` decides what is worth compiling. |
| `ProtectedCodeMemory` + `IPageProtector` | `ICodeMemory` for the images without page-table code of their own: code-pool pages, W^X switched by a protector. `UefiPageProtector` uses the firmware's EFI_MEMORY_ATTRIBUTE_PROTOCOL (AArch64); `arch_riscv64::PageProtector` edits the Sv39/48/57 tables the firmware left (RISC-V firmware keeps paging on and data no-execute, and has no such protocol): splits superpages, one PTE store per page so no entry is ever W+X, SFENCE.VMA on every hart. |
| `JitSelfTest` | `jit_self_test()`: compile a loop with a helper call, run it from the given code memory, compare with the interpreter. Every image runs it at boot once the cores are up and logs `JIT: self test passed`. |
| `KernelJit` | `kernel_code_memory()`: what `main.cpp` registers (pool of up to 16 MiB, shootdown, NX from the CPU); null if paging is unusable, then callers keep interpreting. |
| `JitRuntime` | what one VM needs to run generated code: the back end for this CPU, a code cache (4096 slots), a hotness tracker and the compiler over the kernel's code memory. `try_run(frontend, key, state, &exit)` is the call a VM makes at a point where it could enter compiled code; the front end is passed per call (the VM sets which function the key belongs to first). `create()` is null when the kernel has no executable code memory, then the VM keeps interpreting. |
| `JitCompiler` | runs front end -> validate -> back end -> code memory -> publish. `JitDispatcher::try_run` is the call an interpreter loop makes at each block boundary. |

## The browser's VMs

Both VMs keep a `JitRuntime`, offer compiled code at a few points of their interpreter loop (a function's entry, after a call returned, a backward branch, the instruction after
one the compiled code does not do), and take the exit value to continue where the compiled code stopped. A region is not a basic block: it is everything reachable from the
entry point, with the loops inside it, and it ends only at an instruction the front end does not compile (the interpreter runs that one, and offers the next point). Compiled code
never throws and never calls back into the VM's loop: a trap or an exception is the interpreter re-running the instruction it left at, on the same stack. Both are on by default
whenever the kernel has registered executable code memory, and a host test registers `HostCodeMemory` (`tests/JitHost.hpp`). `Store::set_jit` / `Vm::set_jit` switch them and set
the number of visits after which a point is compiled (1 compiles at once).

* **WebAssembly** (`src/Wasm/WasmJit.cpp`): the translated instruction array of a function. Locals and operand stack positions are virtual registers, loaded from the frame when a
  region is entered and stored back when it is left. Compiled: i32 / i64 arithmetic, comparison, shifts and rotates, f64 arithmetic and comparison, conversions, loads and stores of
  memory 0 (bounds checked in the generated code against the size read when the region was entered), globals, locals, select, every branch including `br_table`. f32 arithmetic and
  comparison, the conversions between f32, f64 and integers and the trapping float to integer conversions are inline (f32 + - * / is done in double and rounded once, which is exact);
  division, bit counting, rounding, square root, minimum, maximum, copysign, the saturating and the unsigned 64 bit conversions go through native helpers after the generated code
  tested for the trap. `call` and `call_indirect` are made from compiled code: the arguments are written to the frame and the machine runs the callee in place
  (`Machine::call_in_place`, a nested interpreter loop that offers the callee's own compiled code first); the results come back through the frame and the memory registers are
  reloaded. A callee that traps, fails in the host or throws leaves the region with `kJitExitFault` and the interpreter carries on as if it had made the call (a handler in the
  caller catches an exception as usual). A call to a host function is made the same way (the machine calls `IHost::call` with the arguments where they are). A tail call is the call followed by a return: the region leaves with `kJitExitReturn` and the interpreter pops the frame, so long tail recursion nests natively only up to the limit below and then runs in the interpreter's flat frames. Native nesting is limited to 20 levels; beyond that, or when a table entry does not match, the call is left to the
  interpreter. Not compiled: SIMD (no function that could hold a v128 is compiled at all, since compiled code moves only the low half of a slot), tables, bulk memory, exception
  handling, `memory.grow`.
  A loop counts its back edges and leaves after 4096 so that the host's tick (the time limit) still runs.
* **JavaScript** (`src/Script/VmJit.cpp`): the bytecode of a function. The operand stack stays in the VM's stack (so the collector sees all of it and a helper can run anywhere),
  the environment is reached by pointer. Numbers are handled inline (arithmetic, comparison, `& | ^ << >> >>>` through a truncating conversion that leaves anything out of range to
  the slow path), dense arrays inline for an in-range integer index and for `.length`; everything else (other operand types, `in`, `instanceof`, property and global reads and
  writes, method lookup) goes through helpers that call the same `Vm` functions as the interpreter. Calls, returns, literals, closures, `try`, iteration and `await` end the region.
  A region that would leave after fewer than three instructions is not compiled. Async functions and generators are never compiled. Long loops charge the instruction budget.

Tests: `WasmTest` runs all 38767 calls of the reference vector file once without and once with the JIT; `WasmJitTest` compares thousands of random functions (integer, double and
memory operations, ifs, loops, calls, traps) between interpreter and JIT, results and memory; `ScriptTest` runs the whole suite twice (second time every region compiled at its first
visit) plus `test_jit_scripts` (hot loops, arrays, bit operations, exceptions inside loops, the budget stopping a compiled infinite loop).

## Using it

A guest implements `IJitFrontend::translate(pc, builder, &size)` for one basic block (state registers via `load_state`/`store_state`, guest memory
through `helper`s that use the checked `GuestMemory`, end with `exit(next_pc)`), and the interpreter loop becomes:

```cpp
if (dispatcher.try_run(pc, state_view, &next_pc)) { pc = next_pc; continue; }
/* interpret one block as before */
```

When the guest writes to translated code (self-modifying code, `munmap`, `mprotect`), call `dispatcher.invalidate(start, size)`.

## Rules for code that runs generated code

* Hold a `kernel::RcuReader` from `lookup()` until the block returns (`JitDispatcher` does). Blocks must be short, since a running block holds up the grace period.
* The state block passed to a block is 8-byte aligned; helpers have the signature `uint64_t(void* state, uint64_t a, uint64_t b)` and must not unwind.
* Registers not yet written read 0 (interpreter and back ends agree); the differential test in `JitTest` depends on it.

## Not done

* Real-hardware validation of `KernelCodeMemory` (host-tested against a fake CPU only); `PageTables` is not thread safe against other users (the MMIO mapper), only against itself through `KernelCodeMemory`.
* Allocator: interval splitting (a value lives in one place for its whole life, so a long-lived value in a busy block ends up in memory), operands taken straight from memory or as immediates (x86 `op reg, [mem]` / `op reg, imm`; constants are materialised into registers), real-hardware validation of the RISC-V/Arm instruction synchronisation. Splitting needs liveness holes and split-point moves on a non-SSA IR with loops; it is worth doing together with the first real front end, which will show which blocks suffer.
* A `ProtectedCodeMemory` page protector for x86 is not needed (`KernelCodeMemory`); AArch64 relies on the firmware offering the memory attribute protocol (if it does not, regions are plain memory and `protections_enforced()` says so; a firmware that also enforces NX on data would then fault, which the boot self test shows). Boot-services time only: after `ExitBootServices` the protocol is gone.
* A front end for the emulator.
* Floating point on AArch64 and RISC-V is lowered and tested under qemu (`AArch64JitTest`, `RiscV64JitTest`) but switched off in `JitRuntime`: those kernels would need to enable the
  floating point unit and save its registers on a task switch first. Vector operations are not in the IR (they go through helpers); register allocation across a region's loops keeps every wasm local / stack position in a register or
  a spill slot for the whole region, so large functions spill a lot; the JS stack lives in memory (a register-resident stack with the collector told about it would be faster).
* Wasm: calls nest natively only 20 levels deep (deeper recursion, and so long tail recursion, runs in the interpreter's flat frames).

Boot tests: `tests/jit_boot_test.sh [x86_64] [aarch64] [riscv64]` builds each image, boots it under QEMU with the edk2 firmware and checks the `JIT: self test passed` line (real page tables, real NX, SMP).

Tests: `tests/run_host_tests.sh JitTest ProtectedCodeMemoryTest KernelCodeMemoryTest AArch64JitTest RiscV64JitTest` (the last two compile 401 IR programs for the other CPU on the host, run them with `tests/data/jit/cross_runner.c` under qemu user mode, and compare with the interpreter; they skip themselves without qemu, clang and rust's ld.lld). `JitTest` runs with ASan/UBSan, then ThreadSanitizer, and runs generated code natively on an x86-64 host.
