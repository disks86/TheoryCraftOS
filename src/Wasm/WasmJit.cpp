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
#include "Wasm/WasmJit.hpp"
#include "Utility/AllocatedView.hpp"
#include "Wasm/WasmNumeric.hpp"

namespace wasm {

namespace {

using jit::IrBuilder;
using jit::IrCond;
using jit::IrMem;
using jit::IrOp;

constexpr uint64_t kMask32 = 0xFFFFFFFFull;
constexpr uint32_t kMaxFrameValues = 600;     // locals + operand stack positions a compiled function may have (each is a virtual register)
constexpr uint32_t kMaxRegionInstrs = 3000;   // instructions translated for one region; the rest of the function is left to the interpreter
constexpr uint32_t kMaxBrTable = 48;          // targets of a br_table that are compiled to compares
constexpr uint64_t kLoopBudget = 4096;        // iterations of backward branches in compiled code before the host is given a tick (as the interpreter's fuel)

// --- native helpers (the IR has no division, counting or single precision): (state, a, b) -> result, operands and results as the slot holds them. The compiled
// code has tested what would trap before the call.
uint64_t h_div_s32(void*, uint64_t a, uint64_t b) { return static_cast<uint32_t>(static_cast<int32_t>(a) / static_cast<int32_t>(b)); }
uint64_t h_div_u32(void*, uint64_t a, uint64_t b) { return static_cast<uint32_t>(a) / static_cast<uint32_t>(b); }
uint64_t h_rem_s32(void*, uint64_t a, uint64_t b) {
  const int32_t y = static_cast<int32_t>(b);
  return y == -1 ? 0 : static_cast<uint32_t>(static_cast<int32_t>(a) % y);
}
uint64_t h_rem_u32(void*, uint64_t a, uint64_t b) { return static_cast<uint32_t>(a) % static_cast<uint32_t>(b); }
uint64_t h_div_s64(void*, uint64_t a, uint64_t b) { return static_cast<uint64_t>(static_cast<int64_t>(a) / static_cast<int64_t>(b)); }
uint64_t h_div_u64(void*, uint64_t a, uint64_t b) { return a / b; }
uint64_t h_rem_s64(void*, uint64_t a, uint64_t b) {
  const int64_t y = static_cast<int64_t>(b);
  return y == -1 ? 0 : static_cast<uint64_t>(static_cast<int64_t>(a) % y);
}
uint64_t h_rem_u64(void*, uint64_t a, uint64_t b) { return a % b; }
uint64_t h_clz32(void*, uint64_t a, uint64_t) { return num::clz32(static_cast<uint32_t>(a)); }
uint64_t h_ctz32(void*, uint64_t a, uint64_t) { return num::ctz32(static_cast<uint32_t>(a)); }
uint64_t h_popcnt32(void*, uint64_t a, uint64_t) { return num::popcnt32(static_cast<uint32_t>(a)); }
uint64_t h_clz64(void*, uint64_t a, uint64_t) { return num::clz64(a); }
uint64_t h_ctz64(void*, uint64_t a, uint64_t) { return num::ctz64(a); }
uint64_t h_popcnt64(void*, uint64_t a, uint64_t) { return num::popcnt64(a); }

float f32_of(uint64_t v) { return num::f32_from(static_cast<uint32_t>(v)); }
uint64_t h_f32_from_i64_s(void*, uint64_t a, uint64_t) { return num::bits(static_cast<float>(static_cast<int64_t>(a))); }
uint64_t h_f32_from_i64_u(void*, uint64_t a, uint64_t) { return num::bits(static_cast<float>(a)); }
double f64_of(uint64_t v) { return num::f64_from(v); }
uint64_t h_f32_ceil(void*, uint64_t a, uint64_t) { return num::bits(num::f_ceil(f32_of(a))); }
uint64_t h_f32_floor(void*, uint64_t a, uint64_t) { return num::bits(num::f_floor(f32_of(a))); }
uint64_t h_f32_trunc(void*, uint64_t a, uint64_t) { return num::bits(num::f_trunc(f32_of(a))); }
uint64_t h_f32_nearest(void*, uint64_t a, uint64_t) { return num::bits(num::f_nearest(f32_of(a))); }
uint64_t h_f32_sqrt(void*, uint64_t a, uint64_t) { return num::bits(num::f_sqrt(f32_of(a))); }
uint64_t h_f32_min(void*, uint64_t a, uint64_t b) { return num::bits(num::f_min(f32_of(a), f32_of(b))); }
uint64_t h_f32_max(void*, uint64_t a, uint64_t b) { return num::bits(num::f_max(f32_of(a), f32_of(b))); }
uint64_t h_f32_copysign(void*, uint64_t a, uint64_t b) { return num::bits(num::f_copysign(f32_of(a), f32_of(b))); }
uint64_t h_f64_ceil(void*, uint64_t a, uint64_t) { return num::bits(num::f_ceil(f64_of(a))); }
uint64_t h_f64_floor(void*, uint64_t a, uint64_t) { return num::bits(num::f_floor(f64_of(a))); }
uint64_t h_f64_trunc(void*, uint64_t a, uint64_t) { return num::bits(num::f_trunc(f64_of(a))); }
uint64_t h_f64_nearest(void*, uint64_t a, uint64_t) { return num::bits(num::f_nearest(f64_of(a))); }
uint64_t h_f64_sqrt(void*, uint64_t a, uint64_t) { return num::bits(num::f_sqrt(f64_of(a))); }
uint64_t h_f64_min(void*, uint64_t a, uint64_t b) { return num::bits(num::f_min(f64_of(a), f64_of(b))); }
uint64_t h_f64_max(void*, uint64_t a, uint64_t b) { return num::bits(num::f_max(f64_of(a), f64_of(b))); }
uint64_t h_f64_copysign(void*, uint64_t a, uint64_t b) { return num::bits(num::f_copysign(f64_of(a), f64_of(b))); }
// saturating conversions (the non-trapping ones): float to int32 / uint32 / int64 / uint64, from f32 and f64
uint64_t h_sat_i32_f32(void*, uint64_t a, uint64_t) { return static_cast<uint32_t>(num::sat_i32(f32_of(a))); }
uint64_t h_sat_u32_f32(void*, uint64_t a, uint64_t) { return num::sat_u32(f32_of(a)); }
uint64_t h_sat_i32_f64(void*, uint64_t a, uint64_t) { return static_cast<uint32_t>(num::sat_i32(f64_of(a))); }
uint64_t h_sat_u32_f64(void*, uint64_t a, uint64_t) { return num::sat_u32(f64_of(a)); }
uint64_t h_sat_i64_f32(void*, uint64_t a, uint64_t) { return static_cast<uint64_t>(num::sat_i64(f32_of(a))); }
uint64_t h_sat_u64_f32(void*, uint64_t a, uint64_t) { return num::sat_u64(f32_of(a)); }
uint64_t h_sat_i64_f64(void*, uint64_t a, uint64_t) { return static_cast<uint64_t>(num::sat_i64(f64_of(a))); }
uint64_t h_sat_u64_f64(void*, uint64_t a, uint64_t) { return num::sat_u64(f64_of(a)); }
uint64_t h_trunc_u64_f32(void*, uint64_t a, uint64_t) { return static_cast<uint64_t>(f32_of(a)); }   // (range checked by the generated code)
uint64_t h_trunc_u64_f64(void*, uint64_t a, uint64_t) { return static_cast<uint64_t>(f64_of(a)); }
uint64_t h_f64_from_i64_u(void*, uint64_t a, uint64_t) { return num::bits(static_cast<double>(a)); }

// What the translator needs to know about a function before it starts: the instructions that are targets of a branch, and which locals are read and written
// anywhere (a superset of what a region does, which is all the loads and stores at its edges need).
struct Scan {
  ArrayView<uint8_t> target;
  ArrayView<uint8_t> local_use;   // bit 0 read, bit 1 written
  bool uses_memory = false;
};

class Translator {
 public:
  Translator(allocators::IAllocator& allocator, WasmJit& owner, IrBuilder& b, const FuncInst& fn, uint32_t entry_pc, uint32_t entry_height)
      : allocator_(allocator), owner_(owner), b_(b), fn_(fn), code_(*fn.code), instrs_(fn.code->code.view()), entry_pc_(entry_pc), entry_height_(entry_height),
        work_(allocator), stubs_(allocator) {}
  ~Translator() {
    release_view(allocator_, heights_);
    release_view(allocator_, labels_);
    release_view(allocator_, state_);
    release_view(allocator_, target_);
    release_view(allocator_, local_use_);
  }
  bool run();

 private:
  struct Pending {
    uint32_t pc, height;
  };
  struct Stub {
    uint32_t label, pc, height;
    uint64_t flags;
  };
  enum class Flow { Next, Stop, Done };   // Done: the instruction ended the flow itself (a branch); Stop: it is not compiled, the region ends before it

  uint32_t L(uint32_t j) const { return base_ + j; }
  uint32_t S(uint32_t k) const { return base_ + code_.nlocals + k; }
  uint32_t constant(uint64_t v) { return b_.constant(static_cast<int64_t>(v)); }
  uint32_t mask32(uint32_t v) { return b_.binary(IrOp::And, v, constant(kMask32)); }
  uint32_t sext32(uint32_t v) {
    const uint32_t sh = constant(32);
    return b_.binary(IrOp::Sar, b_.binary(IrOp::Shl, v, sh), sh);
  }
  bool scan();
  uint32_t request(uint32_t pc, uint32_t height);
  bool emit_run(uint32_t pc, uint32_t height);
  void sync(uint32_t height);
  void emit_exit(uint32_t pc, uint32_t height, uint64_t flags);
  uint32_t trap_label(uint32_t pc, uint32_t height);
  bool emit_branch(uint32_t target_pc, uint32_t dest_slot, uint32_t arity, uint32_t height, uint32_t here);
  Flow translate_one(const Instr& in, uint32_t pc, uint32_t& h);
  Flow fail() {
    failed_ = true;
    return Flow::Stop;
  }
  void binary_to_top(uint32_t& h, IrOp op, bool mask) {
    b_.emit(op, IrCond::Eq, S(h - 2), S(h - 2), S(h - 1), 0);
    --h;
    if (mask) b_.emit(IrOp::And, IrCond::Eq, S(h - 1), S(h - 1), constant(kMask32), 0);
  }
  void call1(uint32_t h, jit::IrHelper fn) { b_.assign(S(h - 1), b_.helper(fn, S(h - 1), S(h - 1))); }
  void call2(uint32_t& h, jit::IrHelper fn) {
    b_.assign(S(h - 2), b_.helper(fn, S(h - 2), S(h - 1)));
    --h;
  }
  uint32_t dconst(double d) { return constant(__builtin_bit_cast(uint64_t, d)); }
  // f32 arithmetic through double: for + - * / the double result rounded once to single precision is exactly the single precision result.
  void binary_f32(uint32_t& h, IrOp op) {
    const uint32_t x = b_.unary(IrOp::F2D, S(h - 2)), y = b_.unary(IrOp::F2D, S(h - 1));
    b_.emit(IrOp::D2F, IrCond::Eq, S(h - 2), b_.binary(op, x, y), jit::kNoVReg, 0);
    --h;
  }
  void fcompare_f32(uint32_t& h, IrCond cond) {
    b_.emit(IrOp::FSetCond, cond, S(h - 2), b_.unary(IrOp::F2D, S(h - 2)), b_.unary(IrOp::F2D, S(h - 1)), 0);
    --h;
  }
  // A trapping float to integer conversion: the value (a double, or an f32 widened) must lie strictly between `low` and `high` (or at `low` when `low_inclusive`);
  // otherwise the region leaves at this instruction and the interpreter traps.
  Flow trunc_checked(uint32_t& h, uint32_t pc, bool from_f32, double low, bool low_inclusive, double high, bool to_i32) {
    if (h < 1) return fail();
    const uint32_t v = from_f32 ? b_.unary(IrOp::F2D, S(h - 1)) : S(h - 1);
    const uint32_t ok = b_.binary(IrOp::And, b_.fsetcond(low_inclusive ? IrCond::Ge : IrCond::Gt, v, dconst(low)), b_.fsetcond(IrCond::Lt, v, dconst(high)));
    b_.branch_if(IrCond::Eq, ok, constant(0), trap_label(pc, h));
    const uint32_t r = b_.unary(IrOp::F2I, v);
    b_.assign(S(h - 1), to_i32 ? b_.binary(IrOp::And, r, constant(kMask32)) : r);
    return Flow::Next;
  }
  void compare32(uint32_t& h, IrCond cond, bool is_signed) {
    uint32_t a = S(h - 2), b = S(h - 1);
    if (is_signed) {   // (the slots hold zero extended values: flipping the sign bit turns the signed order into the unsigned one)
      const uint32_t bias = constant(0x80000000ull);
      a = b_.binary(IrOp::Xor, a, bias);
      b = b_.binary(IrOp::Xor, b, bias);
    }
    b_.emit(IrOp::SetCond, cond, S(h - 2), a, b, 0);
    --h;
  }
  void compare64(uint32_t& h, IrCond cond) {
    b_.emit(IrOp::SetCond, cond, S(h - 2), S(h - 2), S(h - 1), 0);
    --h;
  }
  void fcompare(uint32_t& h, IrCond cond) {
    b_.emit(IrOp::FSetCond, cond, S(h - 2), S(h - 2), S(h - 1), 0);
    --h;
  }
  Flow emit_call(uint32_t pc, uint32_t& h, FuncInst* callee, const FuncSig* sig, Table* table, bool tail);
  void load_memory_registers();
  Flow emit_memory(const Instr& in, uint32_t pc, uint32_t& h, uint32_t width, IrMem mem, bool store, bool narrow_to_i32);

  allocators::IAllocator& allocator_;
  WasmJit& owner_;
  IrBuilder& b_;
  const FuncInst& fn_;
  const FuncCode& code_;
  ArrayView<const Instr> instrs_;
  uint32_t entry_pc_, entry_height_;
  ArrayView<int32_t> heights_;
  ArrayView<uint32_t> labels_;
  ArrayView<uint8_t> state_;      // bit 0 emitted, bit 1 queued
  ArrayView<uint8_t> target_;
  ArrayView<uint8_t> local_use_;
  List<Pending> work_;
  List<Stub> stubs_;
  uint32_t base_ = 0, counter_ = 0, mem_base_ = 0, mem_size_ = 0;
  bool uses_memory_ = false;
  bool failed_ = false;
  uint32_t translated_ = 0;
};

bool Translator::scan() {
  const uint64_t n = instrs_.size();
  target_ = allocate_view<uint8_t>(allocator_, n + 1);
  if (target_.empty()) return false;
  target_.fill(0);
  local_use_ = allocate_view<uint8_t>(allocator_, code_.nlocals ? code_.nlocals : 1);
  if (local_use_.empty()) return false;
  local_use_.fill(0);
  for (const Instr& in : instrs_) {
    switch (in.op) {
      case kOpJmp: case kOpJmpIfZero: case kOpBr: case kOpBrIf:
        if (in.a < n) target_[in.a] = 1;
        break;
      case kOpBrTable:
        for (uint64_t i = 0; i <= in.b; ++i) {
          const uint64_t at = in.a + i;
          if (at >= code_.targets.count()) return false;
          const uint32_t t = code_.targets[at].pc;
          if (t < n) target_[t] = 1;
        }
        break;
      case 0x20: case 0x22:
        if (in.a >= code_.nlocals) return false;
        local_use_[in.a] |= 1;
        if (in.op == 0x22) local_use_[in.a] |= 2;
        break;
      case 0x21:
        if (in.a >= code_.nlocals) return false;
        local_use_[in.a] |= 2;
        break;
      default:
        if ((in.op >= 0x28 && in.op <= 0x3E) && in.aux == 0) uses_memory_ = true;
        break;
    }
  }
  return true;
}

uint32_t Translator::request(uint32_t pc, uint32_t height) {
  if (pc >= instrs_.size() || height > code_.max_stack) {
    failed_ = true;
    return 0;
  }
  if (heights_[pc] >= 0 && heights_[pc] != static_cast<int32_t>(height)) {   // (validated code has one height at every instruction)
    failed_ = true;
    return 0;
  }
  heights_[pc] = static_cast<int32_t>(height);
  if (!labels_[pc]) labels_[pc] = b_.new_label() + 1;
  if (!(state_[pc] & 3)) {
    state_[pc] |= 2;
    if (!work_.push_back(Pending{pc, height})) failed_ = true;
  }
  return labels_[pc] - 1;
}

// Writes the frame back: the locals the function ever writes and the operand stack.
void Translator::sync(uint32_t height) {
  for (uint32_t j = 0; j < code_.nlocals; ++j)
    if (local_use_[j] & 2) b_.store_state(uint64_t{16} * j, L(j));
  for (uint32_t k = 0; k < height; ++k) b_.store_state(uint64_t{16} * (code_.nlocals + k), S(k));
}

void Translator::emit_exit(uint32_t pc, uint32_t height, uint64_t flags) {
  sync(height);
  b_.exit(static_cast<int64_t>(uint64_t{pc} | (uint64_t{height} << 32) | flags));
}

// A label that leaves the region at `pc` with the stack as it is before that instruction (placed after the main code).
uint32_t Translator::trap_label(uint32_t pc, uint32_t height) {
  const uint32_t label = b_.new_label();
  if (!stubs_.push_back(Stub{label, pc, height, 0})) failed_ = true;
  return label;
}

bool Translator::emit_branch(uint32_t target_pc, uint32_t dest_slot, uint32_t arity, uint32_t height, uint32_t here) {
  if (dest_slot < code_.nlocals || height < arity) return false;
  const uint32_t to = dest_slot - code_.nlocals, from = height - arity;
  if (from != to)
    for (uint32_t i = 0; i < arity; ++i) b_.assign(S(to + i), S(from + i));   // (the destination is below the source: copying upward is safe)
  const uint32_t label = request(target_pc, to + arity);
  if (failed_) return false;
  if (target_pc <= here) {   // a loop: now and then let the host see that the code is still running
    b_.emit(IrOp::Add, IrCond::Eq, counter_, counter_, constant(1), 0);
    const uint32_t stub = b_.new_label();
    if (!stubs_.push_back(Stub{stub, target_pc, to + arity, kJitExitTick})) return false;
    b_.branch_if(IrCond::Geu, counter_, constant(kLoopBudget), stub);
  }
  b_.jump(label);
  return true;
}

void Translator::load_memory_registers() {
  Memory* memory = fn_.instance->memories[0];
  b_.emit(IrOp::LoadPtr, IrCond::Eq, mem_base_, constant(reinterpret_cast<uintptr_t>(&memory->data)), jit::kNoVReg, 0, IrMem::U64);
  b_.emit(IrOp::LoadPtr, IrCond::Eq, mem_size_, constant(reinterpret_cast<uintptr_t>(&memory->size)), jit::kNoVReg, 0, IrMem::U64);
}

// A call (direct: `callee`; call_indirect: `sig` and `table`, the index on top of the stack). The arguments are written to the frame (the callee's locals start
// there), the machine's helper runs the callee, and the results are read back; whatever the callee did to memory is picked up by reloading its base and size.
Translator::Flow Translator::emit_call(uint32_t pc, uint32_t& h, FuncInst* callee, const FuncSig* sig, Table* table, bool tail) {
  if (!owner_.call_helper()) return Flow::Stop;
  const bool indirect = callee == nullptr;
  if (!indirect && callee->sig == nullptr) return Flow::Stop;
  if (!indirect) sig = callee->sig;
  const uint32_t np = sig->nparams, nr = sig->nresults;
  const uint32_t popped = np + (indirect ? 1 : 0);
  if (h < popped) return fail();
  const uint32_t first = h - popped;   // the first argument
  if (first + nr > code_.max_stack) return Flow::Stop;
  JitCallSite* site = owner_.new_call_site();
  if (!site) return fail();
  site->machine = owner_.machine();
  site->callee = callee;
  site->table = table;
  site->sig = sig;
  site->args_slot = code_.nlocals + first;
  for (uint32_t k = first; k < first + np; ++k) b_.store_state(uint64_t{16} * (code_.nlocals + k), S(k));
  const uint32_t index = indirect ? mask32(S(h - 1)) : constant(0);
  const uint32_t r = b_.helper(owner_.call_helper(), constant(reinterpret_cast<uintptr_t>(site)), index);
  const uint32_t ok = b_.new_label();
  b_.branch_if(IrCond::Eq, r, constant(0), ok);
  b_.branch_if(IrCond::Eq, r, constant(1), trap_label(pc, h));   // (left to the interpreter: it does the same call, with the whole stack in the frame)
  const uint32_t fault = b_.new_label();
  if (!stubs_.push_back(Stub{fault, pc + 1, first, kJitExitFault})) return fail();
  b_.jump(fault);
  b_.place_label(ok);
  for (uint32_t i = 0; i < nr; ++i) b_.emit(IrOp::LoadState, IrCond::Eq, S(first + i), jit::kNoVReg, jit::kNoVReg, static_cast<int64_t>(uint64_t{16} * (code_.nlocals + first + i)));
  h = first + nr;
  if (tail) {   // a tail call is the call followed by a return: the interpreter pops the frame (the results are the top of the stack)
    emit_exit(pc, h, kJitExitReturn);
    return Flow::Done;
  }
  if (uses_memory_) load_memory_registers();
  return Flow::Next;
}

Translator::Flow Translator::emit_memory(const Instr& in, uint32_t pc, uint32_t& h, uint32_t width, IrMem mem, bool store, bool narrow_to_i32) {
  if (in.aux != 0 || !uses_memory_) return Flow::Stop;
  const uint32_t addr_at = store ? h - 2 : h - 1;
  const uint32_t trap = trap_label(pc, h);
  // The effective address is at most 2^33 + 8: no wrap. A bounds check against the size loaded when the region was entered is enough because compiled code
  // never calls anything that could change the memory.
  const uint32_t ea = b_.binary(IrOp::Add, S(addr_at), constant(in.a));
  const uint32_t end = b_.binary(IrOp::Add, ea, constant(width));
  b_.branch_if(IrCond::Gtu, end, mem_size_, trap);
  const uint32_t pointer = b_.binary(IrOp::Add, mem_base_, ea);
  if (store) {
    b_.store_ptr(mem, pointer, 0, S(h - 1));
    h -= 2;
  } else {
    b_.emit(IrOp::LoadPtr, IrCond::Eq, S(addr_at), pointer, jit::kNoVReg, 0, mem);
    if (narrow_to_i32) b_.emit(IrOp::And, IrCond::Eq, S(addr_at), S(addr_at), constant(kMask32), 0);
  }
  return Flow::Next;
}

Translator::Flow Translator::translate_one(const Instr& in, uint32_t pc, uint32_t& h) {
  // Anything that is not handled below ends the region at this instruction (the helpers say `Stop` after emitting the exit).
  auto need = [&](uint32_t n) { return h >= n; };
  auto push_room = [&]() { return h < code_.max_stack; };
  switch (in.op) {
    case 0x01: return Flow::Next;

    // --- control
    case kOpJmp: {
      const uint32_t label = request(in.a, h);
      if (failed_) return Flow::Stop;
      if (in.a <= pc) {   // (only a loop's own `br` is backward, but be safe)
        b_.emit(IrOp::Add, IrCond::Eq, counter_, counter_, constant(1), 0);
        const uint32_t stub = b_.new_label();
        if (!stubs_.push_back(Stub{stub, in.a, h, kJitExitTick})) return fail();
        b_.branch_if(IrCond::Geu, counter_, constant(kLoopBudget), stub);
      }
      b_.jump(label);
      return Flow::Done;
    }
    case kOpJmpIfZero: {
      if (!need(1)) return fail();
      const uint32_t cond = mask32(S(h - 1));
      --h;
      const uint32_t label = request(in.a, h);
      if (failed_) return Flow::Stop;
      b_.branch_if(IrCond::Eq, cond, constant(0), label);   // (a backward target is not generated by the translator for conditional jumps)
      return Flow::Next;
    }
    case kOpBr: {
      if (!emit_branch(in.a, static_cast<uint32_t>(in.b), static_cast<uint32_t>(in.b >> 32), h, pc)) return fail();
      return Flow::Done;
    }
    case kOpBrIf: {
      if (!need(1)) return fail();
      const uint32_t cond = mask32(S(h - 1));
      --h;
      const uint32_t skip = b_.new_label();
      b_.branch_if(IrCond::Eq, cond, constant(0), skip);
      if (!emit_branch(in.a, static_cast<uint32_t>(in.b), static_cast<uint32_t>(in.b >> 32), h, pc)) return fail();
      b_.place_label(skip);
      return Flow::Next;
    }
    case kOpBrTable: {
      if (!need(1) || in.b >= kMaxBrTable) return Flow::Stop;
      const uint32_t index = mask32(S(h - 1));
      --h;
      for (uint32_t i = 0; i <= in.b; ++i) {
        const BrTarget& t = code_.targets[in.a + i];
        const bool last = i == in.b;
        uint32_t skip = 0;
        if (!last) {
          skip = b_.new_label();
          b_.branch_if(IrCond::Ne, index, constant(i), skip);
        }
        if (!emit_branch(t.pc, t.height, t.arity, h, pc)) return fail();
        if (!last) b_.place_label(skip);
      }
      return Flow::Done;
    }

    case 0x10: {
      if (in.a >= fn_.instance->funcs.count()) return Flow::Stop;
      return emit_call(pc, h, fn_.instance->funcs[in.a], nullptr, nullptr, false);
    }
    case 0x12: {
      if (in.a >= fn_.instance->funcs.count()) return Flow::Stop;
      return emit_call(pc, h, fn_.instance->funcs[in.a], nullptr, nullptr, true);
    }
    case 0x11: {
      if (in.a >= fn_.instance->type_sigs.count() || in.b >= fn_.instance->tables.count()) return Flow::Stop;
      return emit_call(pc, h, nullptr, fn_.instance->type_sigs[in.a], fn_.instance->tables[in.b], false);
    }
    case 0x13: {
      if (in.a >= fn_.instance->type_sigs.count() || in.b >= fn_.instance->tables.count()) return Flow::Stop;
      return emit_call(pc, h, nullptr, fn_.instance->type_sigs[in.a], fn_.instance->tables[in.b], true);
    }

    // --- parametric and variables
    case 0x1A:
      if (!need(1)) return fail();
      --h;
      return Flow::Next;
    case 0x1B: {
      if (!need(3)) return fail();
      const uint32_t cond = mask32(S(h - 1));
      const uint32_t keep = b_.new_label();
      b_.branch_if(IrCond::Ne, cond, constant(0), keep);
      b_.assign(S(h - 3), S(h - 2));
      b_.place_label(keep);
      h -= 2;
      return Flow::Next;
    }
    case 0x20:
      if (!push_room()) return fail();
      b_.assign(S(h), L(in.a));
      ++h;
      return Flow::Next;
    case 0x21:
      if (!need(1)) return fail();
      b_.assign(L(in.a), S(h - 1));
      --h;
      return Flow::Next;
    case 0x22:
      if (!need(1)) return fail();
      b_.assign(L(in.a), S(h - 1));
      return Flow::Next;
    case 0x23: case 0x24: {
      if (in.a >= fn_.instance->globals.count()) return Flow::Stop;
      Global* g = fn_.instance->globals[in.a];
      if (g->type.type == ValType::V128) return Flow::Stop;
      const uint32_t address = constant(reinterpret_cast<uintptr_t>(&g->value.lo));   // (a Global lives as long as the Store, and so do these blocks)
      if (in.op == 0x23) {
        if (!push_room()) return fail();
        b_.emit(IrOp::LoadPtr, IrCond::Eq, S(h), address, jit::kNoVReg, 0, IrMem::U64);
        ++h;
      } else {
        if (!need(1)) return fail();
        b_.store_ptr(IrMem::U64, address, 0, S(h - 1));
        --h;
      }
      return Flow::Next;
    }

    // --- memory
#define LOADOP(op, bytes, mem, narrow) case op: if (!need(1)) return fail(); return emit_memory(in, pc, h, bytes, IrMem::mem, false, narrow);
    LOADOP(0x28, 4, U32, false)
    LOADOP(0x29, 8, U64, false)
    LOADOP(0x2A, 4, U32, false)
    LOADOP(0x2B, 8, U64, false)
    LOADOP(0x2C, 1, S8, true)
    LOADOP(0x2D, 1, U8, false)
    LOADOP(0x2E, 2, S16, true)
    LOADOP(0x2F, 2, U16, false)
    LOADOP(0x30, 1, S8, false)
    LOADOP(0x31, 1, U8, false)
    LOADOP(0x32, 2, S16, false)
    LOADOP(0x33, 2, U16, false)
    LOADOP(0x34, 4, S32, false)
    LOADOP(0x35, 4, U32, false)
#undef LOADOP
#define STOREOP(op, bytes, mem) case op: if (!need(2)) return fail(); return emit_memory(in, pc, h, bytes, IrMem::mem, true, false);
    STOREOP(0x36, 4, U32)
    STOREOP(0x37, 8, U64)
    STOREOP(0x38, 4, U32)
    STOREOP(0x39, 8, U64)
    STOREOP(0x3A, 1, U8)
    STOREOP(0x3B, 2, U16)
    STOREOP(0x3C, 1, U8)
    STOREOP(0x3D, 2, U16)
    STOREOP(0x3E, 4, U32)
#undef STOREOP

    // --- constants
    case 0x41: case 0x42: case 0x43: case 0x44:
      if (!push_room()) return fail();
      b_.emit(IrOp::Const, IrCond::Eq, S(h), jit::kNoVReg, jit::kNoVReg, static_cast<int64_t>(in.b));
      ++h;
      return Flow::Next;

    // --- comparisons
    case 0x45:
      if (!need(1)) return fail();
      b_.emit(IrOp::SetCond, IrCond::Eq, S(h - 1), mask32(S(h - 1)), constant(0), 0);
      return Flow::Next;
    case 0x46: if (!need(2)) return fail(); compare32(h, IrCond::Eq, false); return Flow::Next;
    case 0x47: if (!need(2)) return fail(); compare32(h, IrCond::Ne, false); return Flow::Next;
    case 0x48: if (!need(2)) return fail(); compare32(h, IrCond::Ltu, true); return Flow::Next;
    case 0x49: if (!need(2)) return fail(); compare32(h, IrCond::Ltu, false); return Flow::Next;
    case 0x4A: if (!need(2)) return fail(); compare32(h, IrCond::Gtu, true); return Flow::Next;
    case 0x4B: if (!need(2)) return fail(); compare32(h, IrCond::Gtu, false); return Flow::Next;
    case 0x4C: if (!need(2)) return fail(); compare32(h, IrCond::Leu, true); return Flow::Next;
    case 0x4D: if (!need(2)) return fail(); compare32(h, IrCond::Leu, false); return Flow::Next;
    case 0x4E: if (!need(2)) return fail(); compare32(h, IrCond::Geu, true); return Flow::Next;
    case 0x4F: if (!need(2)) return fail(); compare32(h, IrCond::Geu, false); return Flow::Next;
    case 0x50:
      if (!need(1)) return fail();
      b_.emit(IrOp::SetCond, IrCond::Eq, S(h - 1), S(h - 1), constant(0), 0);
      return Flow::Next;
    case 0x51: if (!need(2)) return fail(); compare64(h, IrCond::Eq); return Flow::Next;
    case 0x52: if (!need(2)) return fail(); compare64(h, IrCond::Ne); return Flow::Next;
    case 0x53: if (!need(2)) return fail(); compare64(h, IrCond::Lt); return Flow::Next;
    case 0x54: if (!need(2)) return fail(); compare64(h, IrCond::Ltu); return Flow::Next;
    case 0x55: if (!need(2)) return fail(); compare64(h, IrCond::Gt); return Flow::Next;
    case 0x56: if (!need(2)) return fail(); compare64(h, IrCond::Gtu); return Flow::Next;
    case 0x57: if (!need(2)) return fail(); compare64(h, IrCond::Le); return Flow::Next;
    case 0x58: if (!need(2)) return fail(); compare64(h, IrCond::Leu); return Flow::Next;
    case 0x59: if (!need(2)) return fail(); compare64(h, IrCond::Ge); return Flow::Next;
    case 0x5A: if (!need(2)) return fail(); compare64(h, IrCond::Geu); return Flow::Next;
    case 0x5B: if (!need(2)) return fail(); fcompare_f32(h, IrCond::Eq); return Flow::Next;
    case 0x5C: if (!need(2)) return fail(); fcompare_f32(h, IrCond::Ne); return Flow::Next;
    case 0x5D: if (!need(2)) return fail(); fcompare_f32(h, IrCond::Lt); return Flow::Next;
    case 0x5E: if (!need(2)) return fail(); fcompare_f32(h, IrCond::Gt); return Flow::Next;
    case 0x5F: if (!need(2)) return fail(); fcompare_f32(h, IrCond::Le); return Flow::Next;
    case 0x60: if (!need(2)) return fail(); fcompare_f32(h, IrCond::Ge); return Flow::Next;
    case 0x61: if (!need(2)) return fail(); fcompare(h, IrCond::Eq); return Flow::Next;
    case 0x62: if (!need(2)) return fail(); fcompare(h, IrCond::Ne); return Flow::Next;
    case 0x63: if (!need(2)) return fail(); fcompare(h, IrCond::Lt); return Flow::Next;
    case 0x64: if (!need(2)) return fail(); fcompare(h, IrCond::Gt); return Flow::Next;
    case 0x65: if (!need(2)) return fail(); fcompare(h, IrCond::Le); return Flow::Next;
    case 0x66: if (!need(2)) return fail(); fcompare(h, IrCond::Ge); return Flow::Next;

    // --- i32 arithmetic
    case 0x67: if (!need(1)) return fail(); call1(h, h_clz32); return Flow::Next;
    case 0x68: if (!need(1)) return fail(); call1(h, h_ctz32); return Flow::Next;
    case 0x69: if (!need(1)) return fail(); call1(h, h_popcnt32); return Flow::Next;
    case 0x6A: if (!need(2)) return fail(); binary_to_top(h, IrOp::Add, true); return Flow::Next;
    case 0x6B: if (!need(2)) return fail(); binary_to_top(h, IrOp::Sub, true); return Flow::Next;
    case 0x6C: if (!need(2)) return fail(); binary_to_top(h, IrOp::Mul, true); return Flow::Next;
    case 0x6D: case 0x6E: case 0x6F: case 0x70: {
      if (!need(2)) return fail();
      const uint32_t trap = trap_label(pc, h);
      const uint32_t divisor = mask32(S(h - 1));
      b_.branch_if(IrCond::Eq, divisor, constant(0), trap);
      if (in.op == 0x6D) {   // INT32_MIN / -1 overflows
        const uint32_t both = b_.binary(IrOp::Or, b_.binary(IrOp::Xor, S(h - 2), constant(0x80000000ull)), b_.binary(IrOp::Xor, divisor, constant(kMask32)));
        b_.branch_if(IrCond::Eq, both, constant(0), trap);
      }
      b_.assign(S(h - 2), b_.helper(in.op == 0x6D ? h_div_s32 : in.op == 0x6E ? h_div_u32 : in.op == 0x6F ? h_rem_s32 : h_rem_u32, S(h - 2), divisor));
      --h;
      return Flow::Next;
    }
    case 0x71: if (!need(2)) return fail(); binary_to_top(h, IrOp::And, false); return Flow::Next;
    case 0x72: if (!need(2)) return fail(); binary_to_top(h, IrOp::Or, false); return Flow::Next;
    case 0x73: if (!need(2)) return fail(); binary_to_top(h, IrOp::Xor, false); return Flow::Next;
    case 0x74: case 0x75: case 0x76: {
      if (!need(2)) return fail();
      const uint32_t count = b_.binary(IrOp::And, S(h - 1), constant(31));
      const uint32_t value = in.op == 0x75 ? sext32(S(h - 2)) : S(h - 2);
      b_.emit(in.op == 0x74 ? IrOp::Shl : in.op == 0x75 ? IrOp::Sar : IrOp::Shr, IrCond::Eq, S(h - 2), value, count, 0);
      --h;
      if (in.op != 0x76) b_.emit(IrOp::And, IrCond::Eq, S(h - 1), S(h - 1), constant(kMask32), 0);
      return Flow::Next;
    }
    case 0x77: case 0x78: {   // rotl / rotr: both halves shifted, the count 32 giving 0 for a zero extended value
      if (!need(2)) return fail();
      const uint32_t n = b_.binary(IrOp::And, S(h - 1), constant(31));
      const uint32_t other = b_.binary(IrOp::Sub, constant(32), n);
      const uint32_t x = S(h - 2);
      const uint32_t left = b_.binary(IrOp::Shl, x, in.op == 0x77 ? n : other);
      const uint32_t right = b_.binary(IrOp::Shr, x, in.op == 0x77 ? other : n);
      b_.emit(IrOp::Or, IrCond::Eq, S(h - 2), left, right, 0);
      --h;
      b_.emit(IrOp::And, IrCond::Eq, S(h - 1), S(h - 1), constant(kMask32), 0);
      return Flow::Next;
    }

    // --- i64 arithmetic
    case 0x79: if (!need(1)) return fail(); call1(h, h_clz64); return Flow::Next;
    case 0x7A: if (!need(1)) return fail(); call1(h, h_ctz64); return Flow::Next;
    case 0x7B: if (!need(1)) return fail(); call1(h, h_popcnt64); return Flow::Next;
    case 0x7C: if (!need(2)) return fail(); binary_to_top(h, IrOp::Add, false); return Flow::Next;
    case 0x7D: if (!need(2)) return fail(); binary_to_top(h, IrOp::Sub, false); return Flow::Next;
    case 0x7E: if (!need(2)) return fail(); binary_to_top(h, IrOp::Mul, false); return Flow::Next;
    case 0x7F: case 0x80: case 0x81: case 0x82: {
      if (!need(2)) return fail();
      const uint32_t trap = trap_label(pc, h);
      b_.branch_if(IrCond::Eq, S(h - 1), constant(0), trap);
      if (in.op == 0x7F) {   // INT64_MIN / -1 overflows
        const uint32_t both = b_.binary(IrOp::Or, b_.binary(IrOp::Xor, S(h - 2), constant(0x8000000000000000ull)), b_.binary(IrOp::Xor, S(h - 1), constant(~uint64_t{0})));
        b_.branch_if(IrCond::Eq, both, constant(0), trap);
      }
      call2(h, in.op == 0x7F ? h_div_s64 : in.op == 0x80 ? h_div_u64 : in.op == 0x81 ? h_rem_s64 : h_rem_u64);
      return Flow::Next;
    }
    case 0x83: if (!need(2)) return fail(); binary_to_top(h, IrOp::And, false); return Flow::Next;
    case 0x84: if (!need(2)) return fail(); binary_to_top(h, IrOp::Or, false); return Flow::Next;
    case 0x85: if (!need(2)) return fail(); binary_to_top(h, IrOp::Xor, false); return Flow::Next;
    case 0x86: if (!need(2)) return fail(); binary_to_top(h, IrOp::Shl, false); return Flow::Next;   // (the IR takes the count modulo 64, as WebAssembly does)
    case 0x87: if (!need(2)) return fail(); binary_to_top(h, IrOp::Sar, false); return Flow::Next;
    case 0x88: if (!need(2)) return fail(); binary_to_top(h, IrOp::Shr, false); return Flow::Next;
    case 0x89: case 0x8A: {
      if (!need(2)) return fail();
      const uint32_t n = b_.binary(IrOp::And, S(h - 1), constant(63));
      const uint32_t other = b_.binary(IrOp::Sub, constant(64), n);   // (a count of 64 is taken as 0 by the IR, which turns a rotation by 0 into a | a)
      const uint32_t x = S(h - 2);
      const uint32_t left = b_.binary(IrOp::Shl, x, in.op == 0x89 ? n : other);
      const uint32_t right = b_.binary(IrOp::Shr, x, in.op == 0x89 ? other : n);
      b_.emit(IrOp::Or, IrCond::Eq, S(h - 2), left, right, 0);
      --h;
      return Flow::Next;
    }

    // --- f32 (through helpers) and f64 (compiled)
    case 0x8B: if (!need(1)) return fail(); b_.emit(IrOp::And, IrCond::Eq, S(h - 1), S(h - 1), constant(0x7FFFFFFFull), 0); return Flow::Next;
    case 0x8C: if (!need(1)) return fail(); b_.emit(IrOp::Xor, IrCond::Eq, S(h - 1), S(h - 1), constant(0x80000000ull), 0); return Flow::Next;
    case 0x92: if (!need(2)) return fail(); binary_f32(h, IrOp::FAdd); return Flow::Next;
    case 0x93: if (!need(2)) return fail(); binary_f32(h, IrOp::FSub); return Flow::Next;
    case 0x94: if (!need(2)) return fail(); binary_f32(h, IrOp::FMul); return Flow::Next;
    case 0x95: if (!need(2)) return fail(); binary_f32(h, IrOp::FDiv); return Flow::Next;
    case 0x99: if (!need(1)) return fail(); b_.emit(IrOp::And, IrCond::Eq, S(h - 1), S(h - 1), constant(0x7FFFFFFFFFFFFFFFull), 0); return Flow::Next;
    case 0x9A: if (!need(1)) return fail(); b_.emit(IrOp::Xor, IrCond::Eq, S(h - 1), S(h - 1), constant(0x8000000000000000ull), 0); return Flow::Next;
    case 0xA0: if (!need(2)) return fail(); binary_to_top(h, IrOp::FAdd, false); return Flow::Next;
    case 0xA1: if (!need(2)) return fail(); binary_to_top(h, IrOp::FSub, false); return Flow::Next;
    case 0xA2: if (!need(2)) return fail(); binary_to_top(h, IrOp::FMul, false); return Flow::Next;
    case 0xA3: if (!need(2)) return fail(); binary_to_top(h, IrOp::FDiv, false); return Flow::Next;

    // --- the rest of f32 and f64 (helpers), conversions to integers
#define F1(op, fn) case op: if (!need(1)) return fail(); call1(h, fn); return Flow::Next;
#define F2(op, fn) case op: if (!need(2)) return fail(); call2(h, fn); return Flow::Next;
    F1(0x8D, h_f32_ceil) F1(0x8E, h_f32_floor) F1(0x8F, h_f32_trunc) F1(0x90, h_f32_nearest) F1(0x91, h_f32_sqrt)
    F2(0x96, h_f32_min) F2(0x97, h_f32_max) F2(0x98, h_f32_copysign)
    F1(0x9B, h_f64_ceil) F1(0x9C, h_f64_floor) F1(0x9D, h_f64_trunc) F1(0x9E, h_f64_nearest) F1(0x9F, h_f64_sqrt)
    F2(0xA4, h_f64_min) F2(0xA5, h_f64_max) F2(0xA6, h_f64_copysign)
    F1(0x100, h_sat_i32_f32) F1(0x101, h_sat_u32_f32) F1(0x102, h_sat_i32_f64) F1(0x103, h_sat_u32_f64)
    F1(0x104, h_sat_i64_f32) F1(0x105, h_sat_u64_f32) F1(0x106, h_sat_i64_f64) F1(0x107, h_sat_u64_f64)
#undef F1
#undef F2
    case 0xA8: return trunc_checked(h, pc, true, -2147483649.0, false, 2147483648.0, true);
    case 0xA9: return trunc_checked(h, pc, true, -1.0, false, 4294967296.0, true);
    case 0xAA: return trunc_checked(h, pc, false, -2147483649.0, false, 2147483648.0, true);
    case 0xAB: return trunc_checked(h, pc, false, -1.0, false, 4294967296.0, true);
    case 0xAE: return trunc_checked(h, pc, true, -9223372036854775808.0, true, 9223372036854775808.0, false);
    case 0xB0: return trunc_checked(h, pc, false, -9223372036854775808.0, true, 9223372036854775808.0, false);
    case 0xAF: case 0xB1: {   // i64.trunc_f32_u / f64_u: range checked here, the conversion (unsigned) by a helper
      if (!need(1)) return fail();
      const uint32_t v = in.op == 0xAF ? b_.unary(IrOp::F2D, S(h - 1)) : S(h - 1);
      const uint32_t ok = b_.binary(IrOp::And, b_.fsetcond(IrCond::Gt, v, dconst(-1.0)), b_.fsetcond(IrCond::Lt, v, dconst(18446744073709551616.0)));
      b_.branch_if(IrCond::Eq, ok, constant(0), trap_label(pc, h));
      b_.assign(S(h - 1), b_.helper(h_trunc_u64_f64, v, v));
      return Flow::Next;
    }

    // --- conversions
    case 0xA7: if (!need(1)) return fail(); b_.emit(IrOp::And, IrCond::Eq, S(h - 1), S(h - 1), constant(kMask32), 0); return Flow::Next;   // i32.wrap_i64
    case 0xAC: if (!need(1)) return fail(); b_.assign(S(h - 1), sext32(S(h - 1))); return Flow::Next;                                   // i64.extend_i32_s
    case 0xAD: return need(1) ? Flow::Next : fail();                                                                                    // i64.extend_i32_u (already zero extended)
    case 0xB2: if (!need(1)) return fail(); b_.emit(IrOp::D2F, IrCond::Eq, S(h - 1), b_.unary(IrOp::I2F, sext32(S(h - 1))), jit::kNoVReg, 0); return Flow::Next;
    case 0xB3: if (!need(1)) return fail(); b_.emit(IrOp::D2F, IrCond::Eq, S(h - 1), b_.unary(IrOp::I2F, S(h - 1)), jit::kNoVReg, 0); return Flow::Next;
    case 0xB4: if (!need(1)) return fail(); call1(h, h_f32_from_i64_s); return Flow::Next;
    case 0xB5: if (!need(1)) return fail(); call1(h, h_f32_from_i64_u); return Flow::Next;
    case 0xB6: if (!need(1)) return fail(); b_.emit(IrOp::D2F, IrCond::Eq, S(h - 1), S(h - 1), jit::kNoVReg, 0); return Flow::Next;
    case 0xB7: if (!need(1)) return fail(); b_.emit(IrOp::I2F, IrCond::Eq, S(h - 1), sext32(S(h - 1)), jit::kNoVReg, 0); return Flow::Next;   // f64.convert_i32_s
    case 0xB8: if (!need(1)) return fail(); b_.emit(IrOp::I2F, IrCond::Eq, S(h - 1), S(h - 1), jit::kNoVReg, 0); return Flow::Next;          // f64.convert_i32_u
    case 0xB9: if (!need(1)) return fail(); b_.emit(IrOp::I2F, IrCond::Eq, S(h - 1), S(h - 1), jit::kNoVReg, 0); return Flow::Next;          // f64.convert_i64_s
    case 0xBA: if (!need(1)) return fail(); call1(h, h_f64_from_i64_u); return Flow::Next;
    case 0xBB: if (!need(1)) return fail(); b_.emit(IrOp::F2D, IrCond::Eq, S(h - 1), S(h - 1), jit::kNoVReg, 0); return Flow::Next;
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: return need(1) ? Flow::Next : fail();   // reinterpret: the bits are the value
    case 0xC0: if (!need(1)) return fail(); b_.emit(IrOp::And, IrCond::Eq, S(h - 1), b_.binary(IrOp::Sar, b_.binary(IrOp::Shl, S(h - 1), constant(56)), constant(56)), constant(kMask32), 0); return Flow::Next;
    case 0xC1: if (!need(1)) return fail(); b_.emit(IrOp::And, IrCond::Eq, S(h - 1), b_.binary(IrOp::Sar, b_.binary(IrOp::Shl, S(h - 1), constant(48)), constant(48)), constant(kMask32), 0); return Flow::Next;
    case 0xC2: if (!need(1)) return fail(); b_.assign(S(h - 1), b_.binary(IrOp::Sar, b_.binary(IrOp::Shl, S(h - 1), constant(56)), constant(56))); return Flow::Next;
    case 0xC3: if (!need(1)) return fail(); b_.assign(S(h - 1), b_.binary(IrOp::Sar, b_.binary(IrOp::Shl, S(h - 1), constant(48)), constant(48))); return Flow::Next;
    case 0xC4: if (!need(1)) return fail(); b_.assign(S(h - 1), sext32(S(h - 1))); return Flow::Next;
    default: break;
  }
  return Flow::Stop;
}

// Translates the straight run that starts at `pc` until the flow ends (a branch that never falls through, an exit, or code that is already translated).
bool Translator::emit_run(uint32_t pc, uint32_t height) {
  uint32_t h = height;
  for (;;) {
    if (pc >= instrs_.size()) return false;
    if (state_[pc] & 1) {   // arrived at code that is translated already: that is a join, and it is a branch target with a label
      if (heights_[pc] != static_cast<int32_t>(h) || !labels_[pc]) return false;
      b_.jump(labels_[pc] - 1);
      return true;
    }
    if (heights_[pc] >= 0 && heights_[pc] != static_cast<int32_t>(h)) return false;
    heights_[pc] = static_cast<int32_t>(h);
    if (target_[pc] || labels_[pc]) {
      if (!labels_[pc]) labels_[pc] = b_.new_label() + 1;
      b_.place_label(labels_[pc] - 1);
    }
    state_[pc] |= 1;
    if (++translated_ > kMaxRegionInstrs) {   // too big: the rest is left to the interpreter
      emit_exit(pc, h, 0);
      return true;
    }
    const Instr& in = instrs_[pc];
    const uint32_t before = h;
    const Flow flow = translate_one(in, pc, h);
    if (failed_) return false;
    if (flow == Flow::Done) return true;
    if (flow == Flow::Stop) {   // this instruction is not compiled: leave with the stack as it was before it
      emit_exit(pc, before, 0);
      return true;
    }
    ++pc;
  }
}

bool Translator::run() {
  const uint64_t n = instrs_.size();
  if (n == 0 || entry_pc_ >= n || entry_height_ > code_.max_stack) return false;
  const uint32_t values = code_.nlocals + code_.max_stack;
  if (values > kMaxFrameValues) return false;
  heights_ = allocate_view<int32_t>(allocator_, n + 1);
  labels_ = allocate_view<uint32_t>(allocator_, n + 1);
  state_ = allocate_view<uint8_t>(allocator_, n + 1);
  if (heights_.empty() || labels_.empty() || state_.empty()) return false;
  heights_.fill(-1);
  labels_.fill(0);
  state_.fill(0);
  if (!scan()) return false;

  b_.set_state_size(uint64_t{16} * values);
  base_ = b_.new_vreg();
  for (uint32_t i = 1; i < values; ++i) b_.new_vreg();
  counter_ = b_.new_vreg();   // (reads 0 until written, like every register)
  // The frame comes in: the locals that are read, and the operands that are on the stack.
  for (uint32_t j = 0; j < code_.nlocals; ++j)
    if (local_use_[j] & 1) b_.emit(IrOp::LoadState, IrCond::Eq, L(j), jit::kNoVReg, jit::kNoVReg, static_cast<int64_t>(uint64_t{16} * j));
  for (uint32_t k = 0; k < entry_height_; ++k) b_.emit(IrOp::LoadState, IrCond::Eq, S(k), jit::kNoVReg, jit::kNoVReg, static_cast<int64_t>(uint64_t{16} * (code_.nlocals + k)));
  if (uses_memory_) {
    if (fn_.instance->memories.count() == 0) return false;
    Memory* memory = fn_.instance->memories[0];
    (void)memory;
    mem_base_ = b_.new_vreg();
    mem_size_ = b_.new_vreg();
    load_memory_registers();
  }

  request(entry_pc_, entry_height_);
  while (!failed_ && work_.count()) {
    const Pending p = work_[work_.count() - 1];
    work_.remove_at(work_.count() - 1);
    if (state_[p.pc] & 1) continue;
    if (!emit_run(p.pc, p.height)) return false;
  }
  if (failed_) return false;
  for (uint64_t i = 0; i < stubs_.count(); ++i) {   // (emitting a stub adds none)
    const Stub s = stubs_[i];
    b_.place_label(s.label);
    emit_exit(s.pc, s.height, s.flags);
  }
  return !b_.failed();
}

}  // namespace

JitCallSite* WasmJit::new_call_site() {
  UniquePointer<JitCallSite> site = UniquePointer<JitCallSite>::make(allocator_);
  if (!site) return nullptr;
  JitCallSite* raw = site.get();
  if (!sites_.push_back(util::move(site))) return nullptr;
  return raw;
}

bool WasmJit::eligible(FuncInst& fn) {
  if (fn.jit_state) return fn.jit_state == 1;
  fn.jit_state = 2;
  if (!fn.code || !fn.instance || !fn.instance->module) return false;
  const FuncCode& c = *fn.code;
  if (c.nlocals + c.max_stack > kMaxFrameValues || c.code.count() == 0) return false;
  const Module& m = *fn.instance->module;
  // Compiled code moves only the low half of a slot, so nothing that can hold a v128 may be compiled: no type of the module mentions one and no function uses SIMD.
  for (const UniquePointer<FuncSig>& sig : m.types.view()) {
    for (ValType t : sig->params)
      if (t == ValType::V128) return false;
    for (ValType t : sig->results)
      if (t == ValType::V128) return false;
  }
  for (const GlobalType& g : m.globals.view())
    if (g.type == ValType::V128) return false;
  for (const Instr& in : c.code.view())
    if (in.op >= 0x200 && in.op < 0x400) return false;
  fn.jit_state = 1;
  if (!fn.jit_id) fn.jit_id = next_id_++;
  return true;
}

bool WasmJit::enter(FuncInst& fn, uint32_t pc, uint32_t height, Slot* fp, uint64_t* exit) {
  if (!enabled_) return false;
  if (!runtime_) {
    if (tried_) return false;
    tried_ = true;
    runtime_ = jit::JitRuntime::create(allocator_, threshold_);
    if (!runtime_) {
      enabled_ = false;   // (nothing to run generated code from: the interpreter stops offering)
      return false;
    }
  }
  if (!eligible(fn)) return false;
  const FuncCode& c = *fn.code;
  fn_ = &fn;
  height_ = height;
  // The frame is the state block: slot i is at offset 16 * i. This cast is the one place a Slot array becomes bytes (a Slot is two uint64_t, 16 byte aligned
  // at least 8: the block's alignment requirement).
  const uint64_t size = uint64_t{sizeof(Slot)} * (uint64_t{c.nlocals} + c.max_stack);
  ByteView state(reinterpret_cast<uint8_t*>(fp), size);
  return runtime_->try_run(*this, (uint64_t{fn.jit_id} << 32) | pc, state, exit);
}

bool WasmJit::translate(uint64_t key, jit::IrBuilder& builder, uint64_t* guest_size) {
  if (!fn_ || (key >> 32) != fn_->jit_id) return false;
  Translator t(allocator_, *this, builder, *fn_, static_cast<uint32_t>(key), height_);
  *guest_size = 0;
  return t.run() && !builder.failed();
}

}  // namespace wasm
