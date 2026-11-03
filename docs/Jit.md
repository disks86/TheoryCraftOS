# Just-in-time compilation (`src/Jit`)

Shared infrastructure for the places that will generate machine code at run time: the architecture emulator (`src/Emulation`, x86-64 / AArch64 /
RISC-V guests) and the JavaScript / WebAssembly virtual machines (`src/Script`, `src/Wasm`). Nothing is wired into those yet; this is the
pipeline they plug into.

```
guest code --IJitFrontend--> IrBlock --IJitBackend--> machine code --ICodeMemory--> CompiledBlock --CodeCache--> run
 (per guest)    translate()    (Ir/)     compile()      (CodeBuffer)   W^X, i-cache      (RCU)       JitDispatcher
```

| Folder / file | What it is |
| --- | --- |
| `CodeMemory.hpp` | `IInstructionCacheSync` + `CrossCoreInstructionSync`: make every core's instruction fetch see new code (RISC-V FENCE.I, LoongArch IBAR, Arm ISB; x86 relies on the TLB shootdown IPI). `ICodeMemory`: allocate writable, switch to executable (W^X, i-cache flush), release. `HeapCodeMemory` never executes (cross-compiling, tests). `KernelCodeMemory` is the kernel one: whole 4 KiB pages from the allocator whose permissions are flipped in `kernel::PageTables` (W^X), pages come from a `CodePool` (2 MiB windows taken on first use, so only those windows are split into 4 KiB pages) or straight from the allocator; `CrossCoreTlbShootdown` flushes the other cores through `run_on_every_core` (IPI); the hosted test one is `tests/JitHost.hpp` (mmap). |
| `Emitter/CodeBuffer` | bounds-checked byte emission, labels, fixups (x86 rel32, AArch64 imm26/imm19, RISC-V J/B, LoongArch B/branch16) resolved by `finish()`. |
| `Emitter/X86_64Assembler`, `AArch64Assembler`, `RiscV64Assembler`, `LoongArch64Assembler` | integer-core encoders, every encoding checked against `llvm-mc` output in `JitTest`. No SIMD/FP yet. |
| `Ir/Ir` | small 64-bit integer IR on virtual registers (not SSA), guest state in a memory block, helpers for guest memory access, in-block labels and branches, `Exit` values. `validate()` for untrusted front ends, `interpret()` as the reference and the tier-below-JIT. |
| `Ir/RegisterAllocator` | linear-scan register allocation over the IR: intervals widened over loops, zero-at-entry for registers read before a certain write, call-crossing values only in callee-saved registers, furthest-end spilling, spill slots reused once their interval ends, and coalescing hints (a result takes the register of an operand that dies there, so a Move costs nothing and x86 computes in place). Shared by all three back ends (each supplies its register pool). |
| `Backend/IJitBackend` | IR -> machine code for one host CPU. `X86_64Backend` (r12-r15, r10-r11 for values; SysV and Win64, the kernel is a PE image: Win64). `AArch64Backend` for AArch64 (AAPCS64; x20-x28 and x9-x15 for values, x19 = state, x16 = helper address; up to 4095 spill slots per block), `RiscV64Backend` likewise for RV64IM (s2-s11, t3-t6, a3-a7 for values, s1 = state, t2 = helper address, jumps within ±1 MiB), and `LoongArch64Backend` for LA64 (s1-s8, t3-t8, a3-a7 for values, s0 = state, t2 = helper address, jumps within ±128 MiB). All three are run for real under qemu-aarch64 / qemu-riscv64 / qemu-loongarch64 by `AArch64JitTest` / `RiscV64JitTest` / `LoongArch64JitTest`. The LoongArch image has no page protector that can enforce W^X (UEFI runs it in direct address translation), so its code memory reports protections as not enforced. |
| `CodeCache` | direct-mapped guest-address -> `CompiledBlock` table published with RCU (`lookup` never blocks; replaced/invalidated code is freed after a grace period). `HotnessTracker` decides what is worth compiling. |
| `ProtectedCodeMemory` + `IPageProtector` | `ICodeMemory` for the images without page-table code of their own: code-pool pages, W^X switched by a protector. `UefiPageProtector` uses the firmware's EFI_MEMORY_ATTRIBUTE_PROTOCOL (AArch64); `arch_riscv64::PageProtector` edits the Sv39/48/57 tables the firmware left (RISC-V firmware keeps paging on and data no-execute, and has no such protocol): splits superpages, one PTE store per page so no entry is ever W+X, SFENCE.VMA on every hart. |
| `JitSelfTest` | `jit_self_test()`: compile a loop with a helper call, run it from the given code memory, compare with the interpreter. Every image runs it at boot once the cores are up and logs `JIT: self test passed`. |
| `KernelJit` | `kernel_code_memory()`: what `main.cpp` registers (pool of up to 16 MiB, shootdown, NX from the CPU); null if paging is unusable, then callers keep interpreting. |
| `JitCompiler` | runs front end -> validate -> back end -> code memory -> publish. `JitDispatcher::try_run` is the call an interpreter loop makes at each block boundary. |

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
* Front ends for the emulator and the VMs.

Boot tests: `tests/jit_boot_test.sh [x86_64] [aarch64] [riscv64] [loongarch64]` builds each image, boots it under QEMU with the edk2 firmware and checks the `JIT: self test passed` line (real page tables, real NX, SMP).

Tests: `tests/run_host_tests.sh JitTest ProtectedCodeMemoryTest KernelCodeMemoryTest AArch64JitTest RiscV64JitTest LoongArch64JitTest` (the last three compile 405 IR programs for the other CPU on the host, run them with `tests/data/jit/cross_runner.c` under qemu user mode, and compare with the interpreter; they skip themselves without qemu, clang and rust's ld.lld). `JitTest` runs with ASan/UBSan, then ThreadSanitizer, and runs generated code natively on an x86-64 host.
