# Running programs built for another CPU

A program whose ELF or PE header names a CPU other than the machine's own is run by an **in-kernel emulator** (`src/Emulation`) instead of
in the CPU's user mode. The goal is the FEX-style case: an x86-64 program on an Arm or RISC-V machine.

## How a program is dispatched

1. `exe::identify_architecture()` reads the machine field of the header (ELF `e_machine`, PE `Machine`) from the first 4 KiB of the file.
2. `process::native_arch()` is the CPU the kernel was built for; `process::needs_emulation(guest)` is true for any other guest.
3. `desktop::run_program()` (`src/Program/ProgramRunner.cpp`) chooses a plan: **Native** (ring 3, x86-64 only today), **Emulated**
   (`emulation::create_guest_cpu()` + `desktop::EmulatedTask`), or **Impossible** (a 32-bit or unknown CPU, or a guest with no emulator),
   which ends in a message naming the CPU (`this program is for AArch64, which this OS can neither run directly nor emulate on this machine`).
   `ProgramRun::force_emulation` runs a native-CPU program on the emulator anyway (to compare the two).

## Why it lives in the kernel and what that buys

The guest never executes on the host CPU. Its memory is the program's `AddressSpace`, reached only through the bounds-checked accessors
(`GuestMemory`); a bad guest pointer is a guest fault, never a kernel one. Its system calls go straight to the same `process::Process` a native
program uses, through the same ABI tables (`decode_x86_64` for the registers, then the Linux or Windows interface), so files, pipes, the console,
`mmap` and the loader are shared with native programs. Consequences:

- no hardware user mode is needed, so it works on the AArch64 and RISC-V builds, which have none;
- the guest's CPU and the host's are independent: `emulation::IGuestCpu` has an x86-64, an AArch64 and a RISC-V implementation, so an Arm or RISC-V Linux
  program runs on x86-64;
- the interpreter runs with kernel privilege, so it is written to the memory-safety rules in `CLAUDE.md` (no raw guest pointers, checked
  arithmetic, decoder tested with truncated and bit-flipped code under ASan/UBSan).

## The x86-64 guest (`X86_64Cpu`)

An interpreter, one instruction per decode, flags computed eagerly.

- **Integer ISA**: all legacy prefixes (operand/address size, REP/REPNE, FS/GS), REX, ModRM/SIB/RIP-relative addressing, the arithmetic and logic
  groups, shifts and rotates (not RCL/RCR), MUL/IMUL/DIV/IDIV at every width (128-bit divide done without a compiler runtime call), string
  instructions with REP (4096 iterations per step, resumable), CMOVcc/SETcc/Jcc, BT*/BSF/BSR/POPCNT/TZCNT/LZCNT, SHLD/SHRD, XCHG/CMPXCHG/XADD,
  BSWAP, PUSH/POP/CALL/RET/LEAVE, PUSHF/POPF, CPUID (a plain x86-64 baseline), RDTSC (instruction count), SYSCALL.
- **SSE2 integer and moves**: MOVUPS/MOVAPS/MOVDQA/MOVDQU, MOVSS/MOVSD *as bit moves*, MOVD/MOVQ, MOVLPS/MOVHPS, the bitwise ops, PADD/PSUB (plain
  and saturating), PCMPEQ/PCMPGT, PMOVMSKB, PUNPCK*, PACK*, PSHUFD/PSHUFLW/PSHUFHW, PMUL*, PMADDWD, PSADBW, PMIN/PMAX, PAVG, all PSLL/PSRL/PSRA
  forms. These are what compilers emit for `memcpy`/`strlen`-style and auto-vectorized integer code.
- **Floating point (SSE/SSE2, soft-float)**: ADD/SUB/MUL/DIV/SQRT/MIN/MAX in scalar and packed single and double forms, CMPcc, COMIS/UCOMIS, CVTSS2SD / CVTSD2SS /
  CVTPS2PD / CVTPD2PS, CVTSI2SS/SD, CVT(T)SS/SD2SI, CVTDQ2PS/PD, CVT(T)PS/PD2DQ, UNPCK*, SHUFPS/PD, MOVMSKPS/PD, LDMXCSR/STMXCSR. The arithmetic is
  `emulation::softfloat` (`SoftFloat.cpp`): IEEE 754 binary32/64 done in integer operations (the kernel cannot assume an FPU and the host's float would not be
  bit-exact), with x86 semantics: the default NaN, first-NaN propagation, all four MXCSR rounding modes, and the MXCSR exception flags (exceptions are always masked, so
  none traps). MXCSR.DAZ and FTZ are modelled (see below). `RCPPS`/`RSQRTPS` (and the scalar forms) return the correctly rounded value, within the architecture's 1.5 * 2^-12 but not the hardware's table values.
- **Faults** are reported as x86 vectors (0, 3, 6, 13, 14 with the address), restartable (`rip` stays at the instruction).
- **Segments and far transfers**: the six segment registers hold selectors (`mov Sreg`, `lss` / `lfs` / `lgs`, `push` / `pop fs` / `gs`; a selector is valid when null or RPL 3, and a bad one is #GP; loading FS or GS does not change the base), `call far` / `jmp far` through memory (FF /3, /5), `retf`, `iretq` (rip, cs, flags, rsp, ss; only the user-changeable flags are taken) and `int n`, where `int3` is a breakpoint and any other vector a #GP (there is no interrupt table; Linux `int 0x80` and Windows `int 0x29` / `0x2e` are not emulated). CS 0x33 is the only code segment, so a transfer to a compatibility-mode selector is #GP. See `src/Emulation/X86_64Segment.cpp` and `tests/SegmentTest.cpp`.
- **Not implemented** (each is an invalid-opcode fault, never a wrong result): the system instructions (privileged ones, `rdmsr`, the control and debug registers, `lgdt`, ...). See the x86-64 section for the full list of what exists beyond the base set.

## The AArch64 guest (`AArch64Cpu`)

Linux programs only (there is no AArch64 Windows interface). Files: `AArch64Cpu.cpp` (integer, branches, loads/stores, system), `AArch64Fp.cpp`
(scalar FP), `AArch64Simd.cpp` (Advanced SIMD), `AArch64Sve*.cpp` and `AArch64Sve2.cpp` (SVE and SVE2).

- **Integer ISA**: add/sub (immediate, shifted, extended, with carry), logical (immediate and shifted), move wide, bitfield moves and `extr`, conditional
  select and compare, `madd`/`msub`/`smaddl`/`umulh`..., `udiv`/`sdiv`, shifts, `rbit`/`rev*`/`clz`/`cls`, `adr`/`adrp`, every branch form (`ret`, `br`,
  `cbz`, `tbz`, `b.cond`), `svc`, hints and barriers, `mrs`/`msr` of TPIDR_EL0, NZCV, FPCR, FPSR, the counter and a few ID registers.
- **Loads and stores**: all single-register addressing modes (unsigned, unscaled, pre/post-index, register offset with extend), literal loads, pairs
  (`ldp`/`stp`/`ldpsw`), exclusives (`ldxr`/`stxr`, `ldar`/`stlr`), `cas*`, and the LSE atomics (`ldadd` ... `swp`). SIMD/FP registers load and store at
  every width; Advanced SIMD structure loads and stores (`ld1`-`ld4`, `st1`-`st4`, single-lane and replicate forms, post-index).
- **Scalar floating point**: soft-float with the AArch64 rules (positive default NaN, signaling NaN first, FPCR rounding mode and FPSR flags): arithmetic,
  fused `fmadd` family, `fsqrt`, `fmax`/`fmin`/`fmaxnm`/`fminnm`, `fcmp`/`fccmp`/`fcsel`, `fcvt`, int/fixed-point conversions with saturation, every `frint*`.
- **Advanced SIMD** (vector and scalar forms): copy, three-same (integer, saturating, rounding, pairwise, polynomial; floating point in half, single and double precision; `famax`/`famin`), the "extra"
  group (`sqrdmlah`/`sqrdmlsh`, `sdot`/`udot`/`usdot`, `smmla`/`ummla`/`usmmla`, `fcadd`, `fcmla`, the half-to-single `fmlal` family), three-different (all widening and saturating-doubling
  multiplies, `addhn` and friends), two-register miscellaneous (reversals, widening adds, saturating and counting forms, compares with zero, narrowing, every `fcvt*`, `frint*` including `frint32z..64x`, the FP and
  integer reciprocal and reciprocal-square-root estimates), across lanes, permutes, `ext`, `tbl`/`tbx`, modified immediates, shifts by immediate (rounding, saturating, narrowing, widening and fixed-point
  conversions), by-element forms (integer, long, FP in three precisions, `fcmla`, dot products). Not implemented: BFloat16 dot products and matrix multiplies (`bfdot`, `bfmmla`, `bfmlalb/t`; `bfcvt` and
  `bfcvtn` are), the Armv9.5 FP8 conversions and multiplies, `luti2/4`, `fscale`, `fmov` vector half-precision immediates, FEAT_FPRCVT. The NaN sign of rotated `fcmla` / `fcadd` results differs from qemu
  in rare NaN cases (it follows the pseudocode's `FPNeg`).
- **Cryptography**: AES (`aese aesd aesmc aesimc`), SHA-1, SHA-256, SHA-512, the SHA-3 set (`eor3 bcax rax1 xar`), SM3, SM4 and `pmull`/`pmull2` (8x8 and 64x64 bit polynomial), sharing the block-cipher
  primitives with the x86-64 guest (`CryptoShared.hpp`).
- **SVE and SVE2** at a fixed vector length of **128 bits** (`rdvl x0, #1` is 16; Z0..Z31 are the vector registers themselves, a predicate is 16 bits, FFR is 16 bits; the streaming length is the same). Integer arithmetic
  (predicated, unpredicated, immediate; halving, saturating and rounding forms; shifts by vector, wide element and immediate; reductions; multiply-add; `index`; the element-count `cnt*`/`inc*`/`dec*` and saturating
  forms), predicates (logic, `brk*`, `ptrue`/`pfirst`/`pnext`/`ptest`, `while*`, `cntp`, the first-fault register), compares, permutes (`zip`/`uzp`/`trn`, `tbl`/`tbx`, `ext`, `splice`, `compact`, `clast*`, `last*`,
  `rev*`, `insr`, `dup`), floating point (arithmetic with immediates, `fmla` family, `fcmla`/`fcadd`, reductions, `fadda`, conversions, `frint*`, `fscale`, `ftmad`/`fexpa`/`ftsmul`/`ftssel`, compares, half,
  single, double and BFloat16 arithmetic of FEAT_B16B16), SVE2 widening and narrowing arithmetic, `sqdmull` and friends, complex integer arithmetic (`cmla`, `sqrdcmlah`, `cdot`, `cadd`), dot products and
  8-bit matrix multiplies, bit permutes (`bext`/`bdep`/`bgrp`), `histcnt`/`histseg`/`match`, `pmull`, AES / SM4 / `rax1` on Z registers, and every load and store: contiguous (one to four registers, sign and
  zero extending, non-temporal, `ld1rq`/`ld1r*`), first-fault and non-fault (a fault that is not on the first active element is suppressed and clears FFR from there), gathers and scatters with vector bases
  or offsets, `ldr`/`str` of Z and P registers, and prefetches (accepted, no effect). Also present from the newer extensions: BFloat16 (`bfdot`, `bfmmla`, `bfmlalb/t`, `bfmlslb/t`, `bfmla`/`bfmls`/`bfmul` by element; the dot
  products and the matrix multiply round to odd at each step, flush denormals, produce the default NaN and report no exceptions, as qemu does), the half-to-single `fmlalb/t`, `fmlslb/t`, `fdot`, `fmmla` (single),
  `fclamp`/`bfclamp`, the quadword reductions (`addqv`, `smaxqv`, `faddqv`, ...), `fcmla` by element. SVE2.1 is there too: predicate-as-counter registers (`ptrue pn`, `pext`, `psel`, paired
  `while*`, `cntp`, `pmov`), the two-way dot products by element, paired narrowing shifts and `sqcvtn`-style conversions, `ld1q`/`st1q` and the quadword structure loads and stores.
  **Not implemented**: the SVE2.2 zeroing-predicated forms, the FP8 conversions, dot products and multiplies, `firstp`/`lastp`, and `ld1ro*` (undefined at this length, as in qemu).
- **SME** (`AArch64Sme.cpp`) at a streaming vector length of 128 bits (the same as the SVE length): `smstart`/`smstop` (`SM`, `ZA`, or both; a mode change zeroes Z, P and FFR and sets FPSR as the
  architecture says), `mrs svcr`, `ZERO`, `MOVA`/`MOVAZ` between a tile slice and a vector (all element sizes, horizontal and vertical, and the two- and four-vector forms and ZA array vector groups of SME2),
  `ld1`/`st1` of tile slices, `ldr`/`str` of a ZA row, `addha`/`addva`, the outer products (`fmopa`/`fmops` single, double and widening half and BFloat16, `bmopa`, `smopa`/`umopa`/`sumopa`/`usmopa` into 32 and
  64 bit tiles), ZT0 (`ldr`/`str`/`zero`, `movt` to and from a general register, `luti2`/`luti4` with one, two or four destination vectors) and the SME2 two- and four-vector `ld1`/`ldnt1`/`st1`/`stnt1`
  with a predicate-as-counter, contiguous or strided. The rest of SME2 is there too: the ZA array vector groups (`vgx2`/`vgx4`: `add`, `sub`, `fadd`, `fsub`, `fmla`, `fmls`, the integer, `fdot` and `bfdot`
  dot products, the vertical dots `svdot`/`uvdot`/`suvdot`/`usvdot`/`fvdot`/`bfvdot`, and the widening multiply-accumulates `smlal`/`umlal`/`fmlal`/`bfmlal` (+ the `sl` forms) and `smlall`/`umlall`/...)
  with a single vector, a group or an element as the second operand, also on half precision tiles; the multi-vector `smax`/`smin`/`umax`/`umin`/`fmax`/`fmin`/`fmaxnm`/`fminnm`/`famax`/`famin`
  (and the BFloat16 ones), `add`, `sqdmulh`, `srshl`/`urshl`, `fscale`, `sclamp`/`uclamp`/`fclamp`/`bfclamp`, `sel`, `zip`/`uzp` (also of quadwords), `sunpk`/`uunpk`, `fcvt`/`fcvtn`/`bfcvt`/`bfcvtn`,
  `scvtf`/`ucvtf`/`fcvtzs`/`fcvtzu`, `frint*`, the narrowing conversions `sqcvt`... and shifts `sqrshr`...; the MOP4 outer products (`smop4a`/`s`, `umop4*`, `sumop4*`, `usmop4*`, `fmop4*`, `bfmop4*`),
  half precision `fmopa`/`bfmopa` into `za.h`, and `movt` of a vector. The multi-vector arithmetic reuses the single-vector SVE implementation, so both agree. ZA is 16 rows of 16 bytes (`State::za`), ZT0
  64 bytes (`State::zt0`). A system call leaves streaming mode (Linux does too). The tables that decode the SME2 forms (`src/Emulation/AArch64Sme2*Forms.inc`) are generated from LLVM's encodings by
  `tests/aarch64_diff/sme2gen`. **FP8** (FEAT_FP8, `AArch64Fp8*.cpp`, `State::fpmr` with `msr/mrs fpmr`; cleared by a streaming-mode change): the E5M2 and E4M3 formats, `f1cvt`/`f2cvt`/`bf1cvt`/`bf2cvt` (and the `l`/`lt` and `2` forms, scaled by
  2^-LSCALE or 2^-LSCALE2: four bits for halves, six or seven for BFloat16 and single), `fcvtn`/`fcvt`/`bfcvtn` to bytes (rounding to nearest even, scaled by 2^NSCALE, with FPMR.OSC saturation), `fdot` (two- and four-way,
  by element too), `fmlalb`/`fmlalt`, `fmlallbb`..`fmlalltt`, `fmmla`, in Advanced SIMD, SVE and SME (`fdot`, `fmlal`, `fmlall` of ZA vector groups, the vertical `fvdot`/`fvdotb`/`fvdott`, `fmopa`/`fmops`,
  `fmop4a`/`fmop4s`). The sums of products are computed exactly and rounded once; FPMR.OSM turns an overflow into the largest finite value; the dot products report no exceptions (qemu's behaviour, which the
  tests were fitted to: for `fmopa` an operand that is zeroed by its own predicate still counts as a NaN). Also LUT (`luti2`/`luti4` in Advanced SIMD and SVE) and `fscale` (Advanced SIMD). **Not implemented**: `luti6`,
  `ftmopa`/`stmopa`, `bfmul`/`fmul`/`bfscale` of vector groups, the LUTv2 `movt`/`luti4` four-register forms, and the FP8 matrix-multiply `fmmla` of SME. qemu reads one register past `z31` in the vertical dots
  of `{z28, z29}`; those few encodings are not matched (a register past `z31` reads as zero here). SME2 is reported to programs (HWCAP2_SME2, ID_AA64PFR1_EL1.SME = 2), and so are FPMR, LUT, FAMINMAX and the FP8
  features (HWCAP2 bits 48 to 56, 58 and 59, ID_AA64PFR2_EL1, ID_AA64FPFR0_EL1).
- **Advertised to programs**: `AT_HWCAP`/`AT_HWCAP2` of an AArch64 program list what is implemented (FP, ASIMD, crypto, CRC32, atomics, FP16, dot product, SVE, SVE2 and its sub-features, SME2 and its outer products, I8MM, BF16), ID_AA64PFR0/PFR1/ZFR0/SMFR0/ISAR0/ISAR1_EL1 report the same, and `prctl(PR_SVE_GET_VL / PR_SVE_SET_VL / PR_SME_GET_VL / PR_SME_SET_VL)` answer 16 bytes.
- **Other user-mode extensions**: scalar half precision (`FEAT_FP16`), `bfcvt`, `fjcvtzs`, `frint32/64`, exclusive pairs (`ldxp`/`stxp`), `casp*`, LRCPC2/3 (`ldapur`, `stlur`, `ldapr`/`stlr` with writeback),
  CSSC (`smax`/`smin`/`umax`/`umin` with registers and immediates, `abs`, `cnt`, `ctz`), `crc32*`, and pointer tags: the top byte of a user address is ignored (TBI), as Linux runs user code.
- **Not implemented** (an undefined-instruction fault, never a wrong result): the rest of SME2 and the newest SVE extensions (above), pointer authentication (`ldraa`, `pacga`, ...), MTE (`irg`, `stg`, `ldg`, ...), FEAT_MOPS (`cpy*`, `set*`),
  LSE128, RCW and LSFE atomics, FEAT_LSUI (`ldtp`, `ldt*`, `swpt`, ...: EL1 uses of unprivileged accesses), FEAT_CPA (`addpt`, ...), FEAT_LS64. The instruction decoder is also more permissive than the
  architecture for some reserved encodings (about 5% of random words LLVM rejects still execute).
- **Faults** use the same x86 vectors as the other guest (an undefined encoding is 6, a bad address 14 with the address, `brk` 3, a misaligned `pc` 13).

How it was checked: besides `EmulationTest` (with known answers for CRC32, AES, PMULL and the newer memory instructions), `tests/aarch64_diff.sh` assembles tens of thousands of instructions with random register
state, runs them under `qemu-aarch64 -cpu max` and under the emulator, and compares every register, NZCV, FPSR (including QC) and a scratch buffer. The `int`, `int2`, `fp`, `fp16`, `neon` and `mem`
modes use hand-written generators; the `words` mode (`SVE=1` for the SVE space, with qemu limited to a 128-bit vector length) and the `svemem` mode (SVE loads and stores, whose addressing registers are
rewritten to land in the scratch buffer) and the `sme` mode (streaming mode with ZA and ZT0 initialised and dumped; qemu with `sme128=on`) need none: it draws random 32-bit words, keeps those `llvm-mc` (all features) decodes, re-assembles them to the same bits (so no should-be-zero
field is set) and runs a few per mnemonic and operand shape, e.g. `tests/aarch64_diff.sh words --seeds 3 --filter '^(sq|uq)' --perkey 8`. It also reports which sampled forms the emulator lacks
(`tests/aarch64_diff/probe.cpp`), which is the decode-coverage measure: of about 594,000 canonical base-ISA words in a 2-million-word sample the emulator now lacks 8.7% (mostly the Armv9.4+ features listed
above); of the SVE forms qemu implements, about 90% by mnemonic and operand shape are present (the rest are the SVE2.1 / SME2 additions and the FP8 forms), and all of those it has agree with qemu. Two qemu differences are known: it does not load a first-fault or non-fault element that is the first active one but sits in the upper half of the register (the architecture requires the load), and
the NaN sign of some rotated `fcmla` results. `qprobe.c` / `qstart.s` filter the words qemu itself rejects. It needs clang's AArch64 target, `llvm-mc`, an AArch64-capable `ld.lld` (rustup's works) and `qemu-aarch64`, so it is not in `run_host_tests.sh`.
`tests/data/emulation/guest-aarch64.elf` is the checked-in build of the test guest (`clang --target=aarch64-linux-gnu -O2 -static -nostdlib -ffreestanding
-fno-builtin -fno-stack-protector -ffp-contract=off -fuse-ld=lld tests/data/emulation/guest.c`); its output must equal the x86-64 build's.

## The RISC-V guest (`RiscV64Cpu`)

An RV64GC user-mode interpreter (`RiscV64Cpu.cpp`: core, integer, memory, atomics, compressed; `RiscV64Fp.cpp`: F and D). A RISC-V Linux ELF (`e_machine` 243) is
recognized by `identify_architecture` and its system calls (`ecall`, number in a7, arguments a0..a5) are decoded by `decode_riscv64_linux` into the same "generic" Linux table
AArch64 uses, so files, pipes, `mmap` and `clone` are shared with every other kind of program.

- **Integer**: RV64I, M (multiply/divide with the architected division-by-zero and overflow results; the 128-bit high products come from `Wide.hpp`, not `__int128`),
  A (LR/SC with a reservation, all nine AMOs, `.w` and `.d`; misaligned atomics fault), Zicsr for `fflags`/`frm`/`fcsr` and the read-only `cycle`/`time`/`instret`
  (the instruction count stands in for a clock), `fence`/`fence.i` as no-ops. Misaligned ordinary loads and stores work (Linux emulates them for user programs).
- **C**: every RV64C instruction is expanded to the 32-bit encoding it stands for (`expand_compressed`) and executed as that, so there is one implementation of each operation.
  Reserved encodings (the all-zero halfword, `c.addi4spn` with a zero immediate, `c.lui` with zero, `c.lwsp` into x0, ...) are invalid instructions.
- **F and D** (soft-float, so it works on kernels without an FPU): the RISC-V rules on top of `SoftFloat`: every NaN result is the canonical NaN, singles are NaN-boxed (an
  improperly boxed operand reads as the canonical NaN), `fmin`/`fmax` return the number when only one operand is a NaN and order -0 below +0, NV is raised by a signaling NaN
  even when the result is not a NaN, conversions to integers saturate with a NaN converting to the largest value and sign-extend the 32-bit forms, fused multiply-add raises NV
  for inf*0 even with a quiet NaN addend, `fclass`, the five rounding modes (RMM, round to nearest ties away, also for arithmetic) and the dynamic mode from `frm`
  (a reserved mode is an invalid instruction). Underflow is detected **after rounding**, as RISC-V requires (x86 and AArch64 detect before): `Env::tininess_after` in `SoftFloat`.
- **Registers** at program start: `sp` = the stack, `tp` = the thread pointer (`set_thread_pointers`, `clone` with `CLONE_SETTLS`), everything else 0.
- **B and friends**: Zba, Zbb, Zbs, Zbc and Zicond (`bit_manipulation()` in `RiscV64Cpu.cpp`), plus the cache-block operations Zicbom/Zicboz (`cbo.zero` clears its 64-byte block, the others do nothing) and the Zihintpause hint (a fence), so RVA22U64 binaries run.
- **V (vector, 1.0)**: VLEN = 128, ELEN = 64, in `RiscV64Vector.cpp` (configuration, loads and stores), `RiscV64VectorInt.cpp` (integer, fixed-point, mask, reduction and permutation
  instructions) and `RiscV64VectorFp.cpp` (floating point), around `VectorUnit` in `RiscV64VectorUnit.hpp`. The 32 registers are one flat byte array, so a register group is a plain
  slice and LMUL 1/8..8, every SEW, widening/narrowing and segments need no special cases. Covered: `vsetvl*`; unit-stride, strided, indexed (ordered and not), segment,
  whole-register, mask (`vlm`/`vsm`) and fault-only-first loads and stores (a fault leaves `vstart` at the element, like a real trap); all of OPIVV/OPIVX/OPIVI and OPMVV/OPMVX
  (add/sub/shift/logic/min/max, `vadc`/`vmadc`, merge, compares, saturating and averaging adds with the four `vxrm` rounding modes, `vsmul`, `vnclip*`, multiply/divide, the
  multiply-add family, widening and narrowing forms, reductions, mask logic, `vmsbf`/`vmsif`/`vmsof`/`viota`/`vid`/`vcpop`/`vfirst`, slides, `vrgather`/`vrgatherei16`,
  `vcompress`, `vzext`/`vsext`, `vmv<nr>r`); and the floating-point set at SEW 32 and 64 through the same soft-float as the scalar F/D (arithmetic, fused multiply-add, min/max,
  sign injection, compares, `vfsqrt`, `vfclass`, `vfrec7`/`vfrsqrt7` (the 7-bit tables were read out of qemu), all `vfcvt`/`vfwcvt`/`vfncvt` including round-to-odd, widening
  arithmetic, ordered and unordered reductions). Tail and masked-off elements are written with all ones (the "agnostic" choice; qemu does the same with `rvv_ta_all_1s`), and a
  mask destination's tail is always ones. An instruction with `vstart >= vl` changes nothing. `vtype` is `vill` until the first `vsetvl`; an invalid configuration, a register
  group not aligned to EMUL, or a masked instruction writing v0 is an invalid instruction. The emulator does not check every register-overlap rule (an assembler or compiler never
  emits a violation, and the result of one is unspecified anyway).
- **Not implemented** (an invalid-instruction fault, never a wrong result): half-precision vectors (Zvfh, `SEW=16` floats), Zvbb/Zvbc/Zvk\*, Zfh/Zfhmin, Zcb, Zbkb/Zbkx crypto bit ops, H, RV32, privileged instructions.

## AVX, AVX2, FMA, BMI, F16C

The x86-64 guest implements the VEX-encoded instruction sets and reports them through `cpuid`: AVX, AVX2, FMA3, BMI1, BMI2, F16C, MOVBE, LZCNT, POPCNT, XSAVE/OSXSAVE (with
`xgetbv` returning XCR0 = x87|SSE|AVX, and `xsave`/`xrstor`). A dispatcher such as a C library's `memcpy`/`strlen` selection therefore picks its AVX2 paths, and
programs built with `-march=haswell` (or x86-64-v3) run. `X86_64Vex.cpp` holds all of it.

- **State**: YMM0..15 are the XMM registers plus `ymm_high`; legacy SSE instructions leave the upper halves alone and VEX.128 forms clear them, as on hardware. The upper
  halves travel in the `X86Context` so exceptions keep them.
- **How the arithmetic is shared**: a VEX instruction on 128-bit lanes is "the SSE instruction on each 128-bit half with three operands". `vex_simd()` copies the first
  source into the destination, the second source into a scratch register, runs the existing SSE code once per half, and merges the halves; shifts by an XMM count use the
  low 128 bits for both halves, and the width-changing conversions (`vcvtps2pd`, `vcvtpd2ps`, `vcvtdq2pd`, `vcvt(t)pd2dq`) and the mask extractions are written out.
  So every SSE/SSE2 operation (float, integer, moves, conversions, compares) exists with a VEX prefix in 128 and 256 bits.
- **VEX-only instructions**: broadcasts (`vbroadcastss/sd/f128/i128`, `vpbroadcastb/w/d/q`), `vinsert/vextract[fi]128`, `vperm2[fi]128`, `vpermq/pd/d/ps`, `vpermilps/pd`
  (both forms), `vpblendw/d`, `vblendps/pd`, `vblendv*`, `vpblendvb`, the variable shifts (`vpsllv/vpsrlv/vpsravd/q`), the SSSE3/SSE4.1 integer set in VEX form (`vpshufb`,
  `vpalignr`, `vphadd/sub`, `vpmaddubsw`, `vpmulhrsw`, `vpsign`, `vpabs`, `vpmovsx/zx`, `vpmulld/dq`, `vpmin/max[su]*`, `vpackusdw`, `vpcmpeqq/gtq`, `vptest`, `vtestps/pd`,
  `vroundps/pd/ss/sd`, `vpextr*`, `vpinsr*`, `vinsertps`), masked moves (`vmaskmovps/pd`, `vpmaskmovd/q`), gathers (all eight `vgather`/`vpgather` forms), F16C
  (`vcvtph2ps`, `vcvtps2ph`), `vzeroupper`/`vzeroall`.
- **FMA3**: the 132/213/231 forms of `fmadd`, `fmsub`, `fnmadd`, `fnmsub` (packed and scalar, single and double) and `fmaddsub`/`fmsubadd`, with one rounding (the soft-float
  fused multiply-add) and the hardware's NaN selection order (multiplicand, multiplicand, addend).
- **BMI1/BMI2**: `andn`, `bextr`, `blsi`, `blsmsk`, `blsr`, `bzhi`, `mulx`, `pdep`, `pext`, `rorx`, `sarx`, `shlx`, `shrx` (and `tzcnt`/`lzcnt`/`popcnt` that already existed).
- **AES-NI, PCLMULQDQ, SHA** (`X86_64Crypto.cpp`): `aesenc/enclast/dec/declast`, `aesimc`, `aeskeygenassist`, `pclmulqdq` (all immediates), SHA-1 (`sha1rnds4`, `sha1nexte`, `sha1msg1/2`) and SHA-256 (`sha256rnds2`, `sha256msg1/2`), plus the VEX forms and the 256-bit VAES / VPCLMULQDQ. The S-box is computed at compile time. cpuid reports AES, PCLMULQDQ, SHA, VAES and VPCLMULQDQ. Tested by 125 more `AvxTest` cases against the host CPU (bit-exact), and by `tests/data/emulation/crypto.c` (FIPS-197 vector, key schedule, CTR, GHASH, SHA-256 and SHA-1 of "abc" with the published digests).
- **SHA-512, SM3 and SM4** (`X86_64Crypto.cpp`): `vsha512rnds2`/`msg1`/`msg2`, `vsm3msg1`/`msg2`/`rnds2` and `vsm4key4`/`vsm4rnds4` (cpuid leaf 7 sub-leaf 1). No available CPU or emulator runs them (qemu's TCG does not), so
  `tests/data/emulation/crypto2.c` checks them against the published SHA-512("abc") digest, the SM4 standard test vector and SM3("abc") (the full 64-round compression built from the three instructions, equal to OpenSSL's digest) instead. The SM3 operand layout was taken from the ASM-DUDE reference of the Intel pseudocode; a correct digest is only possible if that layout is right, so the digest check covers it.
- **AVX-512** (`X86_64Evex*.cpp`, table in `kEvexOps`): the EVEX prefix, 32 vector registers of 512 bits (`State::evex_lo/evex_hi` for 16..31, the upper halves in `YmmHigh::upper`; assigning an `Xmm`
  to a `YmmHigh`, which is what every VEX write does, clears the upper halves like the hardware), the opmask registers `k0..k7` (the VEX-encoded `kmov`/`kand`/`kshift`/`kortest`/... in
  `kmask_instruction`), merge and zeroing masking per element, embedded broadcast, embedded rounding and SAE, compressed disp8*N, and fault suppression for masked-off memory elements.
  Covered: F, VL, BW, DQ, CD, VBMI, VBMI2, IFMA, VNNI, BITALG, VPOPCNTDQ, GFNI (legacy, VEX and EVEX forms), VAES and VPCLMULQDQ at 512 bits, and BF16: moves, integer arithmetic and logic,
  shifts and rotates, compares into masks, `vpternlog`, packs/unpacks/shuffles/permutes (`vpermi2/t2`, `vshuf*x*`, `valign*`, `vpalignr`), insert/extract, blends, compress/expand,
  broadcasts, zero/sign extension and the narrowing `vpmov*`, `vpmovm2*`/`vpmov*2m`, popcnt/lzcnt/conflict, `vdbpsadbw`, `vpmultishiftqb`, `vpshufbitqmb`, the double shifts, all the FP
  arithmetic with SAE/rounding, FMA, compares, `vrndscale`, `vgetexp`/`vgetmant`/`vscalef`/`vfixupimm`/`vreduce`/`vrange`/`vfpclass`, every conversion between 32/64-bit integers and
  single/double (and `vcvtph2ps`/`vcvtps2ph`), gathers and scatters, and `vucomis*`. XCR0 is 0xE7 and `xsave`/`xrstor` handle the opmask, ZMM_Hi256 and Hi16_ZMM components (standard
  format, 2688 bytes). `vrcp14*`/`vrsqrt14*` return a correctly rounded value truncated to 14 bits: the architecture only promises a relative error below 2^-14, and the real tables differ
  between Intel and AMD, so they are not compared with the host.
- **The rest of the base set and its extensions** (all in `X86_64Misc.cpp`, `X86_64Mmx.cpp` and the main switch, each compared with the host by `tests/X86IntTest.cpp`, 590 instruction forms over 236,000
  random runs: registers, defined flags, memory, MM and XMM state, MXCSR): `rcl`/`rcr` (every size, by 1, immediate or CL; OF only for a count of one), `cmpxchg8b`/`cmpxchg16b` (16-byte alignment, the
  always-write), `adcx`/`adox`, `sahf`/`lahf`, `movnti`, `enter`, `xlat`, `loop`/`loope`/`loopne` (also with the address-size prefix), `push`/`pop fs`/`gs` (a zero selector; the base is not reloaded). With no
  oracle, checked against the architecture's rules: `rdrand`/`rdseed` (a counter-seeded generator, not cryptographic; CF=1 always), `rdpid`/`rdtscp` (TSC_AUX is 0), `rdfsbase`/`wrfsbase`/`rdgsbase`/`wrgsbase`
  (a non-canonical base is #GP), `movdiri`, `movdir64b`, `xtest`, `xend`.
- **Transactional memory (RTM)**: `xbegin` always aborts at once (EAX=0, jump to the fallback), `xabort` outside a transaction is a no-op, `xtest` says "not in a transaction" and `xend` outside one is #GP. That is
  legal (a processor may abort any transaction), and cpuid says so with RTM_ALWAYS_ABORT (leaf 7 EDX bit 11) rather than the RTM bit, so software takes its fallback. The HLE prefixes were already ignored.
- **XSAVE**: `xsave`, `xsaveopt` (which writes everything, as `xsave`), `xsavec` (the compacted format: XCOMP_BV bit 63, the present components packed from offset 576) and `xrstor` of both formats, for x87, SSE,
  AVX, the opmask registers and ZMM_Hi256 / Hi16_ZMM. Checked by saving, clearing and restoring all vector state in each variant and by the layout of the compacted image.
- **MMX**: MM0..MM7 are the significands of the x87 registers. Any MMX instruction makes every x87 register valid and TOP zero, a written register gets an all-ones sign and exponent, and `emms` empties the
  tags. All the prefix-less 0F integer opcodes, `pshufw`, `pextrw`/`pinsrw`, `pmovmskb`, `movntq`, `maskmovq`, `movd`/`movq`, `movq2dq`/`movdq2q`, the SSSE3 forms (`0F 38`/`0F 3A` without a prefix) and
  `cvtpi2ps`/`cvtpi2pd`/`cvt(t)ps2pi`/`cvt(t)pd2pi` exist. The integer operations are the SSE ones (`sse_integer_core`) on a 64-bit value in the low half; `punpckh*` shifts both operands right by 32 and
  runs `punpckl*`, and the pack instructions put the second source in the high half. About 400 MMX forms are compared with the host (generic opcodes in register and memory form, every SSSE3 operation, the shift immediates, `palignr`, `pshufw`, conversions); a mutation of `pmulhrsw` fails it.
- **SSE4a** (`extrq`, `insertq` in the immediate and register forms, `movntss`, `movntsd`): compared with the host where the CPU has it; the upper quadword of the destination is cleared as on Zen.
- **Packed decimal**: `fbld` and `fbstp` (eighteen digits and a sign; anything larger stores the packed-decimal indefinite and raises invalid), compared with the host by `X87Test` in every rounding mode.
- **CET** (`src/Emulation/X86_64Cet.cpp`, `tests/CetTest.cpp`): `endbr64`/`endbr32` are nops. Shadow stacks are off until the host calls `enable_shadow_stack(ssp, allow_wrss)`; while off, `rdsspd`/`rdsspq` and
  `incsspd`/`incsspq` do nothing and `wrss`/`wruss` are #UD, as on a processor with CET disabled. While on, every near `call` also pushes the return address on the shadow stack (ordinary guest memory,
  8 bytes down) and every near `ret` compares its target with the shadow stack top and pops it, raising #CP (vector 21, error code 1) on a mismatch with the data stack left as it was; `rdssp` reads the
  pointer, `incssp` adds count x 8 (Q) or count x 4 (D, unverified) after reading the last entry skipped, `wrss` stores to an operand-size aligned address (#GP(0) otherwise; #UD unless WRSS was allowed),
  and `wruss` is #GP(0) at CPL 3. Far calls push CS and the return address on the shadow stack, `retf` and `iretq` compare both (#CP, error code 2; the layout is from memory of the
  SDM and unverified). `rstorssp` / `saveprevssp` switch shadow stacks through restore tokens (#CP error code 4 for a bad token; #GP for a misaligned address or a token without bit 1). `setssbsy` and
  `clrssbsy` act on supervisor shadow stacks, which the emulator does not have, so they are #UD. Not modelled: the shadow stack page type, the privilege-changing and compatibility-mode cases, interrupts and indirect branch tracking. The map 4 forms of
  `wrss`, `wruss`, `enqcmd` and `enqcmds` are there too.
- **ENQCMD / ENQCMDS** (cpuid 7.0 ECX bit 29): `enqcmd` reads the 64-byte command at the memory operand, replaces its first dword with the PASID, writes it to the 64-byte aligned address in the register
  and clears ZF (ordinary memory always accepts). It is #GP(0) until the host sets a PASID with `set_pasid()` (IA32_PASID valid bit), as on a machine whose OS has not; `enqcmds` is #GP(0) at CPL 3.
- **AVX-VNNI, AVX-IFMA, AVX-VNNI-INT8, AVX-VNNI-INT16, AVX-NE-CONVERT** (`tests/VexNewTest.cpp`, against models written from the definitions, 16 dot-product instructions at 300 cases each,
  both vector lengths, memory and register): the VEX encodings of `vpdpbusd[s]`/`vpdpwssd[s]`/`vpmadd52luq`/`vpmadd52huq` and `vcvtneps2bf16` are run by the EVEX code from a context with no mask and no broadcast
  (`vex_via_evex`); the INT8 / INT16 dot products (`kDot`: signed, unsigned or mixed operands; the all-unsigned forms accumulate and saturate as unsigned) exist in both encodings; `vbcstnebf162ps`,
  `vbcstnesh2ps`, `vcvtne[eo]bf162ps` and `vcvtne[eo]ph2ps` convert exactly (bfloat16 by a shift, half precision to single with a quieted signaling NaN). The unsigned-accumulator rule and the exactness of
  the bfloat16 conversion are not checked against hardware.
- **AVX-512 ER and PF** (Knights Landing; `tests/ErTest.cpp`): `vexp2ps/pd` (2^x in binary64 by its Taylor series), `vrcp28ps/pd/ss/sd` and `vrsqrt28ps/pd/ss/sd` (correctly rounded; the architecture promises 2^-23 and
  2^-28), with denormal inputs as zeros, denormal results flushed, round to nearest, and `{sae}` suppressing the flags. The gather / scatter prefetches (`vgatherpf0/1d/qps/pd`, `vscatterpf0/1`) decode and do nothing.
- **AVX10.1** is advertised (cpuid 7.1 EDX bit 19, leaf 0x24: version 1, 128 / 256 / 512-bit vectors): the emulator has every AVX-512 instruction set it requires. AVX10.2 is implemented too (below), so leaf 0x24 reports version 2.
- **MXCSR.DAZ / FTZ** (`Env::daz` / `Env::ftz` in `SoftFloat`, set from MXCSR bits 6 and 15 by every x86 float handler): with DAZ a denormal operand is a zero of its sign and raises no denormal flag (also for
  `min`/`max`, `sqrt`, rounding, comparisons, conversions and the single-to-half conversion); with FTZ a tiny result (detected before rounding, even when exact) becomes a zero of its sign and raises underflow
  and inexact. A sum with a zero and an FMA with a zero product hand the other operand back, so those paths flush it too. The AVX-512 special functions follow the hardware: `vscalef` flushes only the final
  result, `vreduce` flushes the final difference with the inexact flag but no underflow, `vrndscale` and `vreduce` scale in unflushed intermediate steps, `vfixupimm` classifies the DAZ'd operand but leaves its integer
  table alone, and the bfloat16 `vdpbf16ps` accumulates each pair with one fused multiply-add. Half precision has neither. `AvxTest` and `Avx512Test` run half their cases with DAZ and FTZ set on denormal-rich data
  against the host (`NO_DAZFTZ=1` turns that off, `AVX_SEED=<n>` varies the data), and `AvxTest` has hand-written checks. One known difference: tininess is detected before rounding (Intel), an AMD host detects
  it after rounding, so a result that rounds up to the smallest normal number can differ in the underflow flag.
- **AVX512_4FMAPS** (Xeon Phi; cpuid.7.0:EDX bit 3; `kFma4` in `X86_64EvexFp.cpp`): `v4fmaddps/v4fnmaddps zmm1{k}, zmm2+3, m128` and the scalar `v4fmaddss/v4fnmaddss`. Per lane the accumulator
  starts at the destination and takes four fused multiply-adds, `acc = fma(group[j][lane], mem[j], acc)` (product negated for the fnmadd forms), where the group is the four registers from
  `vvvv & ~3`. The operand must be memory (no register form, no broadcast, packed form 512-bit only: #UD). Rounding comes from MXCSR (no embedded rounding); the flags of all the
  steps accumulate. Whether the hardware rounds each step (as here) is not documented in anything the emulator was checked against. `Fma4Test` compares with the host's own `vfmadd` chain
  (values, NaNs, flags, masks, all four rounding modes), except underflow: the host being AMD detects tininess after rounding, Intel and the emulator before.
- **AVX512_4VNNIW** (Xeon Phi; cpuid.7.0:EDX bit 2; `kVnni4` in `X86_64Evex.cpp`): `vp4dpwssd/vp4dpwssds zmm1{k}, zmm2+3, m128`: four chained `vpdpwssd` (wrapping) or `vpdpwssds` (saturating) steps over the
  register group at `vvvv & ~3`, step j using dword j of the memory operand as the word pair. Memory operand only, 512-bit only (else #UD). `Vnni4Test` compares with four host `vpdpwssd[s]`.
- **AVX512_VP2INTERSECT** (cpuid.7.0:EDX bit 8; `kVp2Intersect` in `X86_64Evex.cpp`): `vp2intersectd/q k, zmm, zmm/m/m{1toN}` writes the mask pair (`reg & 6`, `reg | 1`): bit i of the first when element i of
  the first source equals any element of the second, bit j of the second when element j equals any element of the first; every other bit is cleared. A write mask, zeroing or embedded
  rounding is #UD. `Vp2IntersectTest` checks it against a model written from that definition (the host has no VP2INTERSECT) and runs every truncation and bit flip of the encodings.
- **AVX-512 FP16** (cpuid.7.0:EDX bit 23; `kEvexFp16Ops` in `X86_64EvexFp16.cpp`, EVEX maps 3, 5 and 6): half-precision `add/sub/mul/div/min/max/sqrt`, `fmadd/fmsub/fnmadd/fnmsub`
  in all three operand orders plus `fmaddsub/fmsubadd`, `vcmpph`/`vcomish`, `vrndscale`/`vreduce`/`vgetexp`/`vgetmant`/`vscalef`/`vfpclass`, `vrcpph`/`vrsqrtph`, every conversion to and from
  16/32/64-bit integers, `float`, `double` (packed and scalar, `vcvtsi2sh`/`vcvtsh2si` and the unsigned forms), `vmovsh`, `vmovw`, and the complex-number instructions
  (`vfmulcph`/`vfcmulcph`/`vfmaddcph`/`vfcmaddcph` and the scalar forms, which raise #UD when the destination is also a source). The elements are handled by the same handler as the single
  and double forms (`evex_fp`), which was generalized to 16-bit elements, on top of a new `softfloat::Format::F16` (IEEE binary16: the generic packing code of `SoftFloat.cpp` needed only
  the format description). Half precision has no denormal exception and no DAZ/FTZ, so those never show. Two details are not known and are documented guesses: the complex instructions
  round as chained fused multiply-adds (`a.re*b.re`, then `-a.im*b.im` added with one rounding), and `vrcpph`/`vrsqrtph` return the correctly rounded value, which is inside the
  architecture's 2^-11 bound but is not what the hardware's table gives bit for bit.
- **Intel AMX**: palette 1 (eight tiles of up to 16 rows of 64 bytes) in `src/Emulation/X86_64Amx.cpp`. `ldtilecfg` / `sttilecfg` / `tilerelease` (a bad configuration is #GP: palette above 1, reserved bytes, more than 64
  bytes or 16 rows, rows and bytes not both zero or both non-zero), `tilezero`, `tileloadd` / `tileloaddt1` / `tilestored` (the SIB index register is the row stride; a fault saves `start_row` and the restarted
  instruction continues from it), `tdpbssd` / `sud` / `usd` / `uud` (wrapping 32-bit sums), `tdpbf16ps`, `tdpfp16ps` and the AMX-COMPLEX `tcmmimfp16ps` / `tcmmrlfp16ps`. Tiles that are not configured, repeated tiles,
  shapes that do not fit, VEX.L / VEX.W and a SIB-less tile load are #UD. The floating-point forms accumulate each product with one rounding (round to nearest even, MXCSR ignored, bfloat16 and binary32
  denormals flushed; fp16 denormal inputs are kept). XCR0 (`xgetbv`) gains bits 17 and 18, `xsave` / `xsavec` / `xrstor` handle TILECFG (offset 2752) and TILEDATA (2816) and keep them out of XSTATE_BV while
  unconfigured, and cpuid reports AMX-TILE, INT8, BF16, FP16 and COMPLEX plus leaves 0xD (17, 18), 0x1D and 0x1E. There is no hardware to compare with here: the tests (`tests/AmxTest.cpp`) check against models
  written from the definition, so the operand order of the complex forms, the fp16 denormal rule and the #GP-vs-#UD choices for odd configurations are unverified.
- **AMD FMA4, XOP and TBM** (`src/Emulation/X86_64Xop.cpp`; cpuid 0x80000001 ECX bits 16, 11, 21): all twenty FMA4 forms (VEX map 3; VEX.W swaps the r/m operand and the register in imm8[7:4]; a scalar result
  zeroes the rest of the destination, which is unverified), and from the 8F prefix: the integer multiply-accumulates (`vpmacs*`, `vpmadcs*`), `vpcmov`, `vpperm`, `vprot`, `vpshl`, `vpsha`, `vpcom` / `vpcomu`,
  the horizontal `vphadd*` / `vphsub*` and `vfrcz`, plus TBM (`blcfill`, `blci`, `blcic`, `blcmsk`, `blcs`, `blsfill`, `blsic`, `t1mskc`, `tzmsk`, `bextr` with an immediate). `8F /0` is still `pop`. Checked against
  models only (`tests/XopTest.cpp`). `vpermil2ps` / `vpermil2pd` (VEX map 3, 66 48 / 49; selector bits [1:0] or [1] pick the element, bit 2 the source, bit 3 the match bit, m2z in imm8[1:0]
  as the Microsoft intrinsic documentation gives it; the operand order for VEX.W is from the same source) are there too, and so is LWP (below). The cpuid XOP / FMA4 / TBM / LWP bits are set.
- **AMD Lightweight Profiling** (`src/Emulation/X86_64Lwp.cpp`, from AMD64 APM volumes 2 and 3; cpuid 0x80000001 ECX bit 15 and leaf 0x8000001C; XCR0 bit 62): `llwpcb`, `slwpcb`, `lwpval`, `lwpins` (XOP
  map 9 / A, opcode 12). The control block, the ring buffer in guest memory with its head, the software-owned tail and MissedEvents, synchronized and continuous mode, the Random bits, the performance time
  stamp (the emulator's time stamp counter), SLWPCB's flush of the head, counters and MissedEvents, and LWPINS's CF are modelled, and so are the events: the programmed value sample (1) and programmed
  event (255), instructions retired (2), branches retired (3: conditional, relative and register-indirect branches, `call`, `ret`; the NAB / NCB / NRB filters; TKN set when taken; no prediction
  information, so PRV = 0, and no fused branches), and CPU clocks and reference clocks not halted (5, 6: one clock per retired instruction), with the IP range filter (IPF / IPI) for all of them. The
  DCache misses (4) come from a small cache model that is switched on only while a session with that event runs: a 32 KiB 8-way L1 and a 128 KiB 16-way second level (64-byte lines, LRU), loads that miss the L1 are
  events (a local L3 hit, 48 cycles, source 1, or DRAM, 256 cycles, source 3), stores allocate lines silently, the lowest missed address is reported, and MinLatency and the cache level filters apply. The
  threshold interrupt is a host-visible flag: when the ring fill reaches the threshold with Flags.INT set, `take_lwp_interrupt()` returns true once. Not modelled: branch prediction filters, instruction fetch in the cache,
  and the interrupt delivery itself. The session is part of the CPU object (like the AMX tiles) and XSAVE /
  XRSTOR carry it as component 62 (offset 832, 128 bytes) in a layout of this emulator's own, since AMD's is not public. The minimum interval of a hardware event is 1. `tests/XopTest.cpp` has the
  tests; the semantics are from the manuals and unverified on hardware.
- **Intel APX, part 1** (`D5` REX2 prefix): the payload bits M0, R4, X4, B4, W, R3, X3, B3 select the 0F map and name registers R16..R31 in modrm, SIB (an index field of 100 with X4 is R20, not "no index") and
  opcode-embedded fields; `jmpabs` (`D5 00 A1 imm64`); state in XCR0 bit 19 and xsave component 19 (offset 11008, 128 bytes, in the initial state while R16..R31 are all zero); cpuid 7.1 EDX bit 21. REX2 on an SSE,
  x87, MMX or other vector instruction that names a high register is #UD. `tests/ApxTest.cpp` runs integer instructions on R16 / R17 and on RAX / RCX and compares.
- **Intel APX, part 2** (`src/Emulation/X86_64Apx.cpp`, EVEX map 4, from Intel's architecture specification 355828): the promoted legacy integer instructions `add or adc sbb and sub xor` (all forms, the immediate
  groups 80 / 81 / 83), `rol ror rcl rcr shl shr sal sar` (by 1, CL and imm8), `not neg inc dec`, `mul imul div idiv`, `imul` (0F AF, 69, 6B), `shld` / `shrd`, `tzcnt` / `lzcnt` / `popcnt`, with the new data
  destination (EVEX.ND: the result goes to the register in EVEX.vvvv, zero-extended, and the sources stay), flag suppression (EVEX.NF) and zero-upper (ZU, for `imul` 69 / 6B and `setcc`); `cmovcc` and
  `cfcmovcc` in the four ND / NF forms (a conditionally faulting form does not touch memory when the condition is false); `setcc` (F2, map 4 40..4F); `ccmpscc` / `ctestscc` (the condition in the low four bits
  of the third payload byte, SCC 0xA always true, 0xB always false; the default flags in bits 6:3 of the second, taken as not inverted and PF set from CF as the specification states, both unverified); `push2`
  / `pop2` (16-byte aligned, neither register RSP). Encoding errors (reserved payload bits, ND or NF on an instruction that does not take them, a non-zero vvvv without ND, a clear U with a register operand,
  a 66 or REX prefix before EVEX) are #UD. The promoted instructions run the legacy implementation (a REX2-like decoder state, the same arithmetic and flags) and then rearrange the result, so they cannot differ
  from the legacy forms; `tests/ApxEvexTest.cpp` runs every form against its legacy instruction. Also promoted into map 4: `adcx` / `adox` (66 / F3, opcode 66, with the new
  destination), `crc32` (F0 / F1), `movbe` (60 / 61, with the new register-register forms), `movdiri` (F9), `movdir64b` (66 F8) and `aadd` / `aand` / `aor` / `axor` (FC; RAO-INT, which the legacy 0F 38 FC
  encodings gained too: cpuid 7.1 EAX bit 3). The extended EVEX prefix on the existing EVEX instructions gives them R16..R31 as memory base (P0[3] = B4) and index (P1[2] = ~X4), as the r/m operand of the
  GPR-operand forms (`vmovd` / `vmovq`, `vpbroadcast` from a GPR, `vpinsr`, `vpextr`, `vcvtsi2s*`) and as the destination of `vcvt[t]s*2si`; unused B4 / X4 / R4 are ignored, and U clear with a register
  operand is #UD. The VEX instructions that APX promotes (BMI: `andn bextr blsi blsmsk blsr bzhi mulx pdep pext rorx sarx shlx shrx`; `kmov`; `cmpccxadd`; the AMX tile loads and stores) have the
  EVEX form with the same map and opcode (NF for the six BMI instructions that take it). `cmpccxadd` is implemented in its VEX form as well (cpuid 7.1 EAX bit 7). Not promoted: `enqcmd`, `wrss` /
  `wruss`, `invept` / `invvpid` / `invpcid` and `urdmsr` / `uwrmsr` (privileged or absent hardware: #UD). The PPX hints have no effect.
- **AVX10.2, part 1: bfloat16 arithmetic** (`src/Emulation/X86_64EvexBf16.cpp`; SoftFloat gained a `BF16` format): `vadd` / `vsub` / `vmul` / `vdiv` / `vmin` / `vmax` / `vsqrt` / `vfmadd` / `vfmsub` / `vfnmadd` /
  `vfnmsub` (132, 213, 231) / `vscalef` / `vrcp` / `vrsqrt` / `vgetexp` / `vgetmant` / `vrndscale` / `vreduce` `bf16`, `vcmpbf16`, `vfpclassbf16` and `vcomisbf16`, with the architecture's rules: no MXCSR use and no
  exception flags, denormal inputs are zeros, denormal results are flushed, round to nearest even. `vrcp` and `vrsqrt` are correctly rounded (inside the architectural error bound). `vgetexpbf16` of -INF gives +INF
  (the specification's pseudocode would give 128). `tests/Bf16Test.cpp` checks against the host's double arithmetic.
- **AVX10.2, part 2**: `vminmax{ps,pd,ph,bf16}` and `vminmax{ss,sd,sh}` (all eight operations of the immediate, sign control, NaN propagation and `Number` forms, from the specification's tables; DE / IE flags in MXCSR, no
  flags for bf16), `vcomxs{s,d,h}` / `vucomxs{s,d,h}` (OF SF ZF PF CF as the specification gives, invalid for any NaN in the comx form and only for a signaling NaN in the ucomx form), `vmovd` (F3 0F 7E and
  66 0F D6 with W0), `vdpphps` (two FP16 products accumulated into binary32, RNE, DAZ / FTZ, the first NaN in low, high, destination order), `vmpsadbw` (EVEX form, 512-bit lanes reuse the control bits of the
  first two) and the saturating conversions `vcvtt{ps,pd}2{dqs,udqs,qqs,uqqs}`, `vcvtts{s,d}2{sis,usis}` and `vcvt[t]{ph,ps,bf16}2i[u]bs` (NaN gives 0, out of range saturates, IE / PE in MXCSR except for bf16;
  the byte result sits in the low byte of a zeroed element). 
- **AVX10.2, part 3: FP8 conversions** (revision 7.0 of the specification, 361050): `vcvtph2bf8` / `hf8` (E5M2 / E4M3) and their saturating `s` forms, `vcvt2ph2bf8` / `hf8` (two sources into one),
  `vcvtbiasph2bf8` / `hf8` (an 8-bit rounding bias per element, the stochastic-rounding hook), `vcvthf82ph` (exact) and `vcvt2ps2phx` (MXCSR or embedded rounding, DAZ from MXCSR, no FTZ). The FP8
  conversions use round to nearest even (or the bias), no MXCSR and no flags; overflow gives infinity (E5M2) or NaN (E4M3), saturated forms the maximum. `tests/Bf16Test.cpp` runs every one of the 65536
  fp16 inputs through all the forms against a value-based model written independently of the pseudocode. The embedded-rounding forms of the 256-bit instructions were removed from AVX10.2 in revision 4.0
  and are not implemented. cpuid reports AVX10 version 2.
- **xgetbv with ECX=1** returns XINUSE (the components that are not in their initial state; cpuid 0xD.1 EAX bit 2). `xsaves` / `xrstors` are #GP(0): they are supervisor-state instructions and the emulator only
  runs CPL 3 code.
- **Not implemented** (an invalid-instruction fault, never a wrong result): `invept`, `invvpid` and `invpcid` (VMX and the INVPCID feature, which cpuid does not advertise, so #UD is the architectural answer; they are privileged instructions of a kernel the emulator does not run), supervisor shadow stacks, and delivery of LWP's threshold interrupt (there is no interrupt descriptor table; the flag is the interface).
- **User MSRs** (`urdmsr` / `uwrmsr`, cpuid 7.1 EDX bit 15): the register forms (F2 / F3 REX.W 0F 38 F8, mod 3), the VEX map 7 immediate forms and their EVEX / APX map 4 and 7 forms. The host lists the MSRs a program
  may touch with `set_user_msr(index, value, writable)` (16 entries); any other index, or a write to a read-only one, is #GP(0). IA32_USER_MSR_CTL and its permission bitmap are not modelled.
  Exceptions delivered to a Windows program save the XMM halves of vector state in the `CONTEXT` in memory, not the upper halves (the emulator keeps those itself).
- **Checked against the real thing**: `tests/AvxTest.cpp` assembles about 1,400 instruction forms with `llvm-mc` (`tests/data/emulation/avx_cases_gen.py`), runs each on the host
  and on the emulator from the same random state (80,000+ comparisons per run: all vector registers, written general registers, flags, MXCSR flags except DE, the memory operand)
  and requires them to be identical; truncations and bit flips of every encoding run under ASan/UBSan. `tests/data/emulation/avx.c` (auto-vectorized loops, intrinsics, gathers, BMI,
  feature detection through `cpuid` and `xgetbv`; 440 ymm instructions) prints the same text natively and on the emulator.

## SSE3, SSSE3, SSE4.1, SSE4.2 (legacy encodings)

`cpuid` reports SSE3, SSSE3, SSE4.1, SSE4.2 and POPCNT, so a program built for `-march=x86-64-v2` (or a library that dispatches on those bits) runs.

- **SSE3** (`X86_64Cpu.cpp`): `haddps/pd`, `hsubps/pd`, `addsubps/pd` in `step_float` (the same soft-float add/sub, per-element exception flags), `movddup`, `movsldup`,
  `movshdup` and `lddqu` in `step_sse`. The VEX forms (`vhaddps`, `vmovddup ymm`, ...) reuse them through the per-half scheme (`classify_map1`).
- **SSSE3 and SSE4.1/4.2 with the `0F 38` / `0F 3A` escapes** (`legacy_sse_map` in `X86_64Vex.cpp`): these are the same instructions as the VEX ones with the destination
  also the first source, so `0F 38 xx` and `0F 3A xx` hand the opcode to `vex_map2`/`vex_map3` as a 128-bit operation with `vvvv = reg`, and the upper halves of the YMM registers
  are put back afterwards (legacy SSE leaves them alone). An allow list names the opcodes that exist without a prefix, so a VEX-only opcode (`vbroadcastss`, `vinserti128`,
  ...) is still an invalid instruction without one. The 66-prefixed (XMM) forms go through this path; the prefix-less ones are the MMX forms (`X86_64Mmx.cpp`, below).
- **What was written new** (shared by the VEX and legacy forms): `dpps`/`dppd` (products rounded one by one, summed as (p0+p1)+(p2+p3)), `mpsadbw`, `phminposuw`,
  `pcmpestri/m` and `pcmpistri/m` (all four aggregations, both polarities, bytes or words, signed or unsigned; the flags and ECX/XMM0 results as the SDM defines them; explicit
  lengths from RAX/RDX honoring REX.W), `crc32` (CRC-32C, 8/16/32/64-bit sources), and the legacy-only `blendvps/pd` and `pblendvb` (the selector is XMM0).
- **Checked**: `AvxTest` now holds about 2,150 forms (the legacy encodings of every instruction above, the 128 immediates of `pcmpistri`, every `crc32` width, the VEX versions of
  the new instructions) and compares each with the host CPU like the AVX ones; a mutation of the `dpps` sum order, the CRC polynomial, the negate-masked polarity and the equal-ordered
  rule of `pcmpistri` each fail it. Hand-written checks cover the CRC-32C check value of "123456789", strlen with `pcmpistri`, the MMX forms and VEX-only opcodes being invalid, and the
  `cpuid` bits. `tests/data/emulation/sse4.c` (built with `-O2 -msse4.2 -mno-avx`: CRC-32C, strlen/span/substring searches with `pcmpistri`, auto-vectorized `pmulld`/`pminsd`/
  `pmaxud`/`pmovsx`, `haddps`, `dpps`, `pshufb`, `mpsadbw`, ...) prints the same text natively and on the emulator.

## x87 (80-bit extended precision)

The x87 unit is emulated in full except packed decimal (`fbld`/`fbstp`), which faults. `SoftFloatExt.cpp` is the arithmetic on the 80-bit format (64-bit significand with an
explicit integer bit, 15-bit exponent), `SoftFloatExtMath.cpp` the transcendental functions, `X86_64X87.cpp` the register stack and the decoding of `D8`..`DF`.

- **State**: eight 80-bit registers used as a stack with TOP in the status word, the control word (precision control 24/53/64 bits, all four rounding modes), the status word
  (C0..C3, the sticky exception flags, stack fault), and which registers hold a value. All exceptions are masked, so nothing traps; an empty-register read gives the real
  indefinite and sets the stack-fault flags, a push onto a full stack too. The images `fxsave`/`fxrstor` (ST registers in stack order, abbreviated tags), `fnsave`/`frstor`
  and `fnstenv`/`fldenv` (32-bit layouts) are real, and the x87 state is part of the `X86Context` that exceptions save and `NtContinue` restores.
- **Instructions**: loads and stores in all formats (`fld`/`fst`/`fstp` of 32, 64 and 80 bits, `fild`/`fist`/`fistp`/`fisttp` of 16, 32 and 64), `fadd`/`fsub`/`fsubr`/
  `fmul`/`fdiv`/`fdivr` in every register and memory form (`faddp`, `fiadd`...), `fcom`/`fucom`/`fcomi`/`fucomi` and `ftst`/`fxam`, `fsqrt`, `frndint`, `fscale`,
  `fprem`/`fprem1` (with the hardware's partial-remainder behaviour: C2 and a reduction by a multiple of 32 bits per round), `fxtract`, `fchs`/`fabs`, `fxch`, `fcmov`, the
  constants, `fincstp`/`fdecstp`/`ffree`, the control instructions, and the transcendentals `f2xm1`, `fyl2x`, `fyl2xp1`, `fpatan`, `fsin`, `fcos`, `fsincos`, `fptan`.
- **Exactness**: the arithmetic, conversions, comparisons, remainder, `fscale`, `frndint` and `fsqrt` are bit-exact with a real x87 (results, flags, C1 "rounded up", the
  condition codes), checked against the host's x87 with random operands in every rounding mode and precision (`tests/X87Test.cpp`: 200k+ arithmetic and 100k+ instruction
  cases per run). The transcendentals are series in the extended format with 32-bit-piece Cody-Waite argument reduction: within about 7 units in the last place of the hardware
  (the hardware does not round them exactly either; it uses a 66-bit pi, so its sin/cos of huge arguments are not the true values and ours are closer). `fsin`, `fcos` and
  `fptan` give C2 = 1 for |x| >= 2^63. Denormals are handled as the hardware does (flags, precision-control rounding of denormal results).
- **What it unlocked**: the C runtime's `printf("%f")` uses it, so the Windows `crttest` and `cxxtest` programs now run on the emulator, as does `tests/data/emulation/x87.c`
  (series for e and pi, square roots, conversions, 64-bit significand checks).

## Windows structured exceptions

A fault in an emulated x86-64 Windows program is delivered the way the hardware path does it. `EmulatedProcess::deliver_exception` takes the CPU's `GuestFault` (vector,
address, and for a page fault whether it was a read, a write or an instruction fetch), turns it into an NT exception with the shared `process::fault_to_exception`
(`STATUS_ACCESS_VIOLATION` with the right information, divide error, breakpoint reported at the `int3`, illegal instruction...), and `process::dispatch_exception` pushes
the machine frame, `CONTEXT` and `EXCEPTION_RECORD` on the faulting thread's own stack; the thread continues in ntdll's `KiUserExceptionDispatcher`, whose unwinder finds the
`__try`/`__except` and C++ handlers. `NtContinue` (what the unwinder ends with) goes through `process::apply_context`, which sanitizes the registers, into the CPU.
`IGuestCpu::get_x86_context`/`set_x86_context` convert between the interpreter's registers and the `X86Context` image (general registers, `rip`, flags, and an FXSAVE image
holding `MXCSR` and XMM0-15). A fault that cannot be delivered (no dispatcher, no stack left) ends the program as before. For this the interpreter gained `fxsave`/`fxrstor` and
`mov` to and from the segment registers (user selectors), which `RtlCaptureContext`/`RtlRestoreContext` use.

`tests/emulated_windows.sh` builds the OS's own DLLs and runs `emuthreads.c`, `sehtest.c` (`__try`/`__except`/`__finally`, `RaiseException`, a deep recursion) and
`threadtest.c` (the same inside worker threads) on the emulator: all pass. The other Windows test programs mostly do too, except where they need things an emulated process does not
have (other processes, several processors).

## Threads

An emulated program can have threads, for both ABIs. `EmulatedProcess` (`src/Emulation/EmulatedProcess.cpp`) is the program's `process::IProcessRuntime`:

- Every guest thread is a system thread of its own (a kernel thread; a `std::thread` in the host tests) with its own CPU state, but **all of a program's threads take
  turns on one process lock**: a thread runs a slice of instructions (20000) or a system call with the lock held, lets go of it between slices and while a call waits
  (`Process::block_*`, `IProcessRuntime::wait`). So the guest sees a sequentially consistent machine, its atomic instructions are atomic for free, and the system-call
  code (written for "one lock held by every call") needs no changes. The price: no parallel speed-up, whatever the number of cores.
- AArch64 `ldxr`/`stxr`: a turn never ends inside an exclusive sequence (a reservation still open after 64 extra instructions is dropped, which the architecture allows),
  otherwise another thread's store would not clear it.
- Linux: `clone` (the thread flag set a threading library uses: `CLONE_VM|FS|FILES|SIGHAND|THREAD`, plus `SETTLS`, `PARENT_SETTID`, `CHILD_SETTID`, `CHILD_CLEARTID`;
  anything else, `fork` included, is `-ENOSYS`), `futex` (`WAIT`, `WAKE`, `REQUEUE`, `CMP_REQUEUE`, `WAIT_BITSET`, `WAKE_BITSET`, with timeouts; `WAKE_OP` and the PI
  forms are `-ENOSYS`), `exit` (a thread; the last one takes the process with it), `exit_group`, `gettid`, `set_tid_address` (`CHILD_CLEARTID` zeroes the word and wakes it
  on exit, which is what `pthread_join` waits for), `tgkill`/`tkill` (accepted, no signals), `sched_getaffinity`, `prctl`. The code is in `src/Process/LinuxThreads.cpp`
  and is shared with the native path, which does not support it yet (`supports_linux_threads()` is false there).
- Windows: `NtCreateThread`, `CreateThread`, `ExitThread`, `TerminateThread`, waits on threads and objects, critical sections, TLS, events all work through the same
  runtime (`start_thread`, `resume_thread`, `request_terminate`). `SuspendThread`, `Get/SetThreadContext` and thread-targeted APCs are not available.
- At most 64 threads at a time per program.
- Tests: `threads.c` (raw `clone` + futex mutex, atomics, join through `CLONE_CHILD_CLEARTID`, a futex hand-off and an `exit_group` that must stop a spinning thread) for
  x86-64 and AArch64, run on real host threads at several slice lengths, clean under ASan/UBSan and ThreadSanitizer. `tests/emulated_windows.sh` runs
  `tests/windows/emuthreads.c` (threads, critical sections, events, TLS, `TerminateThread`) on the emulator with the OS's own ntdll/kernel32.

## What an emulated program cannot do yet
- Windows exceptions only work for x86-64 Windows programs (the only kind there is), and `SuspendThread`, `Get/SetThreadContext` and thread-targeted APCs are missing.
- Speed: a plain interpreter, tens of times slower than native. The seam for a translator (decode once into blocks, cache by address, invalidate on
  `AddressSpace::generation()`) is `IGuestCpu::run()`.
- Only x86-64, AArch64 and RISC-V (RV64GC) guests exist; a 32-bit guest (x86, Arm, RV32) needs its own `IGuestCpu`. AArch64 and RISC-V Windows programs are refused with a clear message.
- The AArch64 and RISC-V kernel builds do not yet contain the shell, `ProgramRunner` or the process layer, so nothing launches programs there yet;
  the emulator core compiles for both targets (checked with clang `--target=aarch64-linux-gnu` / `riscv64-linux-gnu`) and `ProgramRunner` already
  falls back to emulation on any non-x86-64 build.
- Nothing has been run in QEMU yet: the host test and the x86-64 kernel build are what exist.

## Tests

`tests/run_host_tests.sh EmulationTest` builds a freestanding Linux program (`tests/data/emulation/guest.c`) with the host compiler, runs it natively
for the expected output, and checks the emulator prints the same for the x86-64 build and for the checked-in AArch64 build, also when run in tiny slices. It also covers instruction semantics
against hand-assembled bytes, the soft-float against the host FPU (random and edge-case operands, all four rounding modes, results and flags compared bit for bit; the
guest program also does double and float work), faults, architecture detection, and the truncation / bit-flip / random-byte robustness cases.

`tests/riscv_diff.sh [int|mem|fp|comp|bit|vint|vmem|vfp] [--seeds N]` is the differential test of the RISC-V guest against `qemu-riscv64` (it needs clang with the riscv64 target, the rustup
`ld.lld` and qemu, so it is not part of `run_host_tests.sh`): `tests/riscv_diff/gen.py` assembles thousands of random instructions with random register contents (x1..x30, f0..f31,
fcsr and a 256-byte buffer) into one program that dumps the whole state after each, the program runs under qemu and under `tests/EmulationRun.cpp`, and `compare.py` requires
identical state. About 105,000 cases (4 seeds of each mode: the integer set including M and the branches, loads/stores/LR/SC/AMOs, F and D with every rounding mode, the
compressed set) match. The generator includes targeted cases for what random operands almost never reach (products that round up to the smallest normal number, `fmin`/`fmax`/compares/
sign injection of +0 and -0); without them a wrong tininess rule and a wrong zero ordering both went unnoticed, and each is now caught. `EmulationTest` also runs the RISC-V builds of
`guest.c` and `threads.c` (checked-in `guest-riscv64.elf`, `threads-riscv64.elf`; built with `clang --target=riscv64-linux-gnu -march=rv64gc -O2 -ffreestanding -nostdlib` and the
rustup `ld.lld`) and has hand-assembled checks, fault vectors and truncation / bit-flip / random-word robustness cases for the RISC-V decoder.
The `bit` mode covers Zba/Zbb/Zbs/Zbc/Zicond (qemu `-cpu max`); the `vint`, `vmem` and `vfp` modes (qemu `-cpu max,vlen=128,rvv_ta_all_1s=true,rvv_ma_all_1s=true`) start every case
with a random legal `vsetvli` (SEW, LMUL 1/8..8, tail and mask policy, `vl` from 0 to past VLMAX), initialize all 32 vector registers with random, special or float-like lanes, run
one instruction with legal register groups, and compare every vector register, `vl`, `vtype`, `vxsat`, `vxrm`, `fcsr`, the scalar registers and a 1 KiB buffer. 4,000 to 5,000 cases
per seed; several seeds of each match with no mismatch. `tests/data/emulation/rvv.c` (checked in as `rvv-riscv64.elf` with the qemu output in `rvv-expected.txt`) is a program of
intrinsics loops and auto-vectorized code that `EmulationTest` runs. `EmulationTest` also feeds 80,000 random vector instructions under random valid configurations to the
decoder under ASan/UBSan.

`tests/run_host_tests.sh Avx512Test` runs the AVX-512 cases (`tests/data/emulation/avx512_cases_gen.py`, about 12,600 instruction forms assembled with llvm-mc: every vector length, masking,
zeroing, broadcast, disp8*N, embedded rounding, registers 16..31) on the host CPU and on the emulator from the same random zmm, mask and general registers, and requires all 640 bytes of
vector state, the mask registers, flags, MXCSR and the memory operand to match; it needs an AVX-512 host and skips otherwise (the truncation / bit-flip robustness cases always run, under
the sanitizers). `avx512.c` is a program of intrinsics and auto-vectorized loops built with every AVX-512 -m flag and run natively and on the emulator.

No CPU that runs the tests has AVX512_FP16, so `Fp16Test` checks against models written in the test instead: add, subtract, multiply, divide, square root and fused multiply-add are
computed exactly in 128-bit integers and rounded in every rounding mode with all the exception flags; the rest uses the host's `_Float16`/float/double. Every instruction runs with
random registers 0..31, all vector lengths, merge/zero masking, memory (compressed disp8), broadcast and embedded rounding/SAE, and all 32 vector registers, the mask registers, flags
and MXCSR are compared. The soft-float half arithmetic was also compared with `_Float16` on 180 million random operand sets, and the unary instructions (`vrndscalesh`, `vreducesh`,
`vgetexpsh`, `vgetmantsh`, `vrcpsh`, `vrsqrtsh`, `vsqrtsh`) run on all 65,536 half values (`FP16_SCAN=1`). `tests/data/emulation/fp16_check_encodings.py` checks the encodings the test uses
against llvm-mc (`FP16_DUMP=1 Fp16Test | python3 -I fp16_check_encodings.py`). `fp16.c` is a program built twice, with `-mavx512fp16` (run on the emulator) and without (the compiler's own
half arithmetic, run natively): the printed hashes of its add/sub/mul/div/min/max/sqrt/conversion/compare loops must be identical, and its intrinsics section (fma, roundscale, getexp, scalef,
masked add, complex multiply, fpclass, sqrt, cvtph2w) counts disagreements with scalar computations, which must be zero.
