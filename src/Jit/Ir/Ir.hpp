/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

// The intermediate representation every JIT front end produces and every back end consumes: a straight-line list of 64-bit integer operations on
// virtual registers, with labels and conditional jumps inside the block, and an exit that tells the dispatcher what to run next. A guest
// instruction set (src/Emulation) or a bytecode (src/Script, src/Wasm) is translated into it once; each host CPU needs one lowering of
// this small set instead of one per guest.
//
// Model:
//  - A virtual register is a 64-bit value, written any number of times (this is not SSA), numbered from 0 up to IrBlock::vreg_count. A register
//    that has not been written yet reads as 0, in the interpreter and in every back end.
//  - Guest state (the emulated registers, the VM's frame) lives in a block of memory the compiled function is handed a pointer to. LoadState and
//    StoreState move 8-byte words between it and virtual registers; the offset is a multiple of 8 below IrBlock::state_size.
//  - Guest memory is never touched by generated code directly: it goes through a Helper, a native function that does the checked access
//    (GuestMemory) and may report a fault. This keeps the generated code free of any bounds logic of its own.
//  - Control flow stays inside the block (Label, Jump, BranchIf); leaving it is Exit / ExitValue, whose value is returned to the dispatcher.
//  - Guest memory that the front end has checked itself (a bounds test in IR against a base and size it loaded) is read and written with LoadPtr /
//    StorePtr through an address held in a virtual register; anything the front end cannot check goes through a Helper. A virtual register that holds an
//    address is a plain 64-bit number to every back end.
//  - Double precision floating point (FAdd .. F2I) works on the IEEE bit pattern of the double held in a virtual register, so the registers stay plain 64-bit
//    values. Only the x86-64 back end lowers these so far: the others report that they cannot encode the block (the caller keeps interpreting it). Single
//    precision and vector operations go through a Helper.
namespace jit {

enum class IrOp : uint8_t {
  Const,        // dst = imm
  Move,         // dst = a
  Add, Sub, Mul, And, Or, Xor,      // dst = a op b
  Shl, Shr, Sar,                    // dst = a shifted by (b & 63)
  Not, Neg,                         // dst = op a
  SetCond,      // dst = (a cond b) ? 1 : 0
  LoadState,    // dst = state[imm]   (8 bytes)
  StoreState,   // state[imm] = a
  Helper,       // dst = helper(state, a, b); imm is the helper's address (see IrBuilder::helper)
  Label,        // a jump target; imm is the label number
  Jump,         // continue at label imm
  BranchIf,     // if (a cond b) continue at label imm
  Exit,         // leave the block returning the constant imm
  ExitValue,    // leave the block returning a
  LoadPtr,      // dst = the `mem` sized value at address a + imm, zero or sign extended to 64 bits (the front end checked the address)
  StorePtr,     // the low `mem` bytes of b are stored at address a + imm (mem is U8 .. U64)
  FAdd, FSub, FMul, FDiv,   // dst = a op b on doubles (bit patterns)
  FSetCond,     // dst = (a cond b) ? 1 : 0 comparing doubles: Eq, Ne, Lt, Le, Gt, Ge; false when either is a NaN, except Ne, which is then true
  I2F,          // dst = (double) the signed integer a
  F2D,          // dst = the single precision float in the low 32 bits of a, as a double (bits)
  D2F,          // dst = the double a rounded to single precision, in the low 32 bits (the upper half is zero)
  F2I,          // dst = the double a truncated toward zero as a signed 64-bit integer; INT64_MIN when it is a NaN or does not fit (the CPU's "integer indefinite")
};

// The width and signedness of a LoadPtr / StorePtr.
enum class IrMem : uint8_t { U8, U16, U32, U64, S8, S16, S32 };

enum class IrCond : uint8_t { Eq, Ne, Lt, Le, Gt, Ge, Ltu, Leu, Gtu, Geu };   // (Lt.. are signed, Ltu.. unsigned)

constexpr uint32_t kNoVReg = 0xFFFFFFFFu;

struct IrInst {
  IrOp op;
  IrCond cond;
  uint32_t dst;
  uint32_t a;
  uint32_t b;
  int64_t imm;
  IrMem mem = IrMem::U64;
};

// The signature of a helper: the state pointer the block was entered with, two arguments, one result. A helper must not unwind (no exceptions in
// this kernel), and may be called from any thread that runs compiled code.
using IrHelper = uint64_t (*)(void* state, uint64_t a, uint64_t b);

// One block of IR: what is compiled as a unit.
struct IrBlock {
  explicit IrBlock(allocators::IAllocator& allocator) : insts(allocator) {}
  List<IrInst> insts;
  uint32_t vreg_count = 0;
  uint32_t label_count = 0;
  uint64_t state_size = 0;     // bytes of guest state the block may touch
};

// Appends instructions to a block, handing out virtual registers and labels. Every method returns false (the builder is then failed() and the
// block must be dropped) when memory ran out; a front end can call freely and check once.
class IrBuilder {
 public:
  explicit IrBuilder(IrBlock& block) : block_(block) {}
  bool failed() const { return failed_; }

  uint32_t new_vreg() { return block_.vreg_count++; }
  // Appends one instruction writing the register `dst` (which may be one that was written before: a guest register, a stack slot of a VM). The helpers below
  // are this with a fresh register; a front end that keeps its guest state in fixed registers calls this directly.
  uint32_t emit(IrOp op, IrCond cond, uint32_t dst, uint32_t a, uint32_t b, int64_t imm, IrMem mem = IrMem::U64) {
    if (!block_.insts.push_back(IrInst{op, cond, dst, a, b, imm, mem})) failed_ = true;
    return dst;
  }
  uint32_t new_label() { return block_.label_count++; }
  void set_state_size(uint64_t bytes) { block_.state_size = bytes; }

  uint32_t constant(int64_t value) { return emit(IrOp::Const, IrCond::Eq, new_vreg(), kNoVReg, kNoVReg, value); }
  // dst = a, for a register that is written more than once (a loop variable, a guest register).
  void assign(uint32_t dst, uint32_t a) { emit(IrOp::Move, IrCond::Eq, dst, a, kNoVReg, 0); }
  uint32_t move(uint32_t a) { return emit(IrOp::Move, IrCond::Eq, new_vreg(), a, kNoVReg, 0); }
  uint32_t binary(IrOp op, uint32_t a, uint32_t b) { return emit(op, IrCond::Eq, new_vreg(), a, b, 0); }
  uint32_t unary(IrOp op, uint32_t a) { return emit(op, IrCond::Eq, new_vreg(), a, kNoVReg, 0); }
  uint32_t set_cond(IrCond cond, uint32_t a, uint32_t b) { return emit(IrOp::SetCond, cond, new_vreg(), a, b, 0); }
  uint32_t load_state(uint64_t offset) { return emit(IrOp::LoadState, IrCond::Eq, new_vreg(), kNoVReg, kNoVReg, static_cast<int64_t>(offset)); }
  void store_state(uint64_t offset, uint32_t a) { emit(IrOp::StoreState, IrCond::Eq, kNoVReg, a, kNoVReg, static_cast<int64_t>(offset)); }
  uint32_t helper(IrHelper fn, uint32_t a, uint32_t b) { return emit(IrOp::Helper, IrCond::Eq, new_vreg(), a, b, helper_address(fn)); }
  // A helper by address, for code generated for another CPU than the one running the compiler (its addresses are not ours to call).
  uint32_t helper_at(uint64_t address, uint32_t a, uint32_t b) { return emit(IrOp::Helper, IrCond::Eq, new_vreg(), a, b, static_cast<int64_t>(address)); }
  void place_label(uint32_t label) { emit(IrOp::Label, IrCond::Eq, kNoVReg, kNoVReg, kNoVReg, label); }
  void jump(uint32_t label) { emit(IrOp::Jump, IrCond::Eq, kNoVReg, kNoVReg, kNoVReg, label); }
  void branch_if(IrCond cond, uint32_t a, uint32_t b, uint32_t label) { emit(IrOp::BranchIf, cond, kNoVReg, a, b, label); }
  uint32_t load_ptr(IrMem mem, uint32_t address, int64_t offset) { return emit(IrOp::LoadPtr, IrCond::Eq, new_vreg(), address, kNoVReg, offset, mem); }
  void store_ptr(IrMem mem, uint32_t address, int64_t offset, uint32_t value) { emit(IrOp::StorePtr, IrCond::Eq, kNoVReg, address, value, offset, mem); }
  uint32_t fsetcond(IrCond cond, uint32_t a, uint32_t b) { return emit(IrOp::FSetCond, cond, new_vreg(), a, b, 0); }
  void exit(int64_t value) { emit(IrOp::Exit, IrCond::Eq, kNoVReg, kNoVReg, kNoVReg, value); }
  void exit_value(uint32_t a) { emit(IrOp::ExitValue, IrCond::Eq, kNoVReg, a, kNoVReg, 0); }

 private:
  // A function address is a plain number in the IR, which is what makes the IR independent of where the helper lives. This is the one place
  // that converts (valid for the lifetime of the program: helpers are ordinary functions).
  static int64_t helper_address(IrHelper fn) { return static_cast<int64_t>(reinterpret_cast<uintptr_t>(fn)); }
  IrBlock& block_;
  bool failed_ = false;
};

// Checks a block a front end built before any back end sees it: operands in range, labels placed once and every jump to a placed label,
// the block ends in an exit or a jump, state offsets aligned and inside state_size. A back end may assume a validated block.
bool validate(allocators::IAllocator& allocator, const IrBlock& block);

// Runs the block directly, instruction by instruction: the reference every back end is checked against, and the fallback for a tier that
// has not been compiled yet. `state` points at block.state_size bytes. Returns the exit value, or false if `max_steps` instructions
// ran without leaving the block (a guard against a loop that never ends).
bool interpret(allocators::IAllocator& allocator, const IrBlock& block, ByteView state, uint64_t max_steps, uint64_t* exit_value);

// Which operands an instruction reads and writes (for the register allocator and the back ends).
bool ir_reads_a(IrOp op);
bool ir_reads_b(IrOp op);
bool ir_writes_dst(IrOp op);

// Evaluates a comparison.
bool eval_cond(IrCond cond, uint64_t a, uint64_t b);

}  // namespace jit
