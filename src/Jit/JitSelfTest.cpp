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
#include "Jit/JitSelfTest.hpp"
#include "Jit/Backend/AArch64Backend.hpp"
#include "Jit/Backend/RiscV64Backend.hpp"
#include "Jit/Backend/X86_64Backend.hpp"
#include "Jit/JitCompiler.hpp"

namespace jit {

namespace {

uint64_t helper(void* state, uint64_t a, uint64_t b) {
  uint64_t word0;
  bytes_of(word0).copy_from(ConstByteView(static_cast<const uint8_t*>(state), 8));   // (the state block is at least 8 bytes: see the program)
  return (a * 31) ^ (b + word0);
}

// state[0] = trip count (masked to 0..63), state[1] = initial accumulator; state[2] = result.
struct SelfTestFrontend final : IJitFrontend {
  bool translate(uint64_t, IrBuilder& b, uint64_t* guest_size) override {
    b.set_state_size(24);
    const uint32_t n = b.binary(IrOp::And, b.load_state(0), b.constant(63));
    const uint32_t acc = b.new_vreg(), i = b.new_vreg(), one = b.constant(1);
    b.assign(acc, b.load_state(8));
    b.assign(i, b.constant(0));
    const uint32_t top = b.new_label(), done = b.new_label();
    b.branch_if(IrCond::Geu, i, n, done);
    b.place_label(top);
    const uint32_t t = b.helper(helper, acc, i);
    b.assign(acc, b.binary(IrOp::Add, b.binary(IrOp::Xor, acc, t), i));
    b.assign(i, b.binary(IrOp::Add, i, one));
    b.branch_if(IrCond::Ltu, i, n, top);
    b.place_label(done);
    b.store_state(16, acc);
    b.exit_value(b.binary(IrOp::Shr, acc, b.constant(3)));
    *guest_size = 16;
    return !b.failed();
  }
};

}  // namespace

SelfTestResult jit_self_test(allocators::IAllocator& allocator, ICodeMemory& memory, uint64_t* compiled_bytes) {
  SelfTestFrontend frontend;
  X86_64Backend x86(allocator);
  AArch64Backend arm(allocator);
  RiscV64Backend riscv(allocator);
  IJitBackend* backend = native_host_arch() == HostArch::X86_64 ? static_cast<IJitBackend*>(&x86)
                         : native_host_arch() == HostArch::AArch64 ? static_cast<IJitBackend*>(&arm) : static_cast<IJitBackend*>(&riscv);

  CodeCache cache(allocator, 16);
  if (!cache.ok()) return SelfTestResult::CompileFailed;
  JitCompiler compiler(allocator, memory, *backend, cache);
  const CompileResult compiled = compiler.compile(frontend, 0x1000);
  if (compiled == CompileResult::OutOfMemory || compiled == CompileResult::NotExecutable) return SelfTestResult::NoCodeMemory;
  if (compiled != CompileResult::Compiled) return SelfTestResult::CompileFailed;

  // The interpreter's answer for the same program and input.
  IrBlock block(allocator);
  IrBuilder builder(block);
  uint64_t guest_size = 0;
  frontend.translate(0x1000, builder, &guest_size);
  alignas(8) uint8_t native[24], reference[24];
  const uint64_t input[3] = {45, 0x1234567, 0};
  ByteView(native, 24).copy_from(bytes_of(input));
  ByteView(reference, 24).copy_from(bytes_of(input));
  uint64_t want = 0, got = 0;
  if (!interpret(allocator, block, ByteView(reference, 24), 100000, &want)) return SelfTestResult::CompileFailed;
  {
    kernel::RcuReader reader;
    const CompiledBlock* compiled_block = cache.lookup(0x1000);
    if (!compiled_block) return SelfTestResult::CompileFailed;
    if (compiled_bytes) *compiled_bytes = compiled_block->code_size;
    got = run_block(*compiled_block, ByteView(native, 24));
  }
  return got == want && ByteView(native, 24).equals(ConstByteView(reference, 24)) ? SelfTestResult::Passed : SelfTestResult::Mismatch;
}

const char* self_test_result_name(SelfTestResult result) {
  switch (result) {
    case SelfTestResult::Passed: return "passed";
    case SelfTestResult::NoCodeMemory: return "no executable code memory";
    case SelfTestResult::CompileFailed: return "compile failed";
    case SelfTestResult::Mismatch: return "result differs from the interpreter";
  }
  return "?";
}

}  // namespace jit
