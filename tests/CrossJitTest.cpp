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
// Cross test for jit::AArch64Backend and jit::RiscV64Backend, which cannot be run on an x86-64 host directly: "gen" compiles random IR programs for the
// other CPU and writes them with their input state to a cases file; tests/data/jit/cross_runner.c runs them under qemu-aarch64 / qemu-riscv64; "check" interprets the same programs on this machine
// and compares the exit values and final states. tests/run_host_tests.sh drives the three steps.
//   CrossJitTest gen <dir> <helper-address> <aarch64|riscv64> <state-helper-address>      CrossJitTest check <dir> <helper-address> <aarch64|riscv64> <state-helper-address>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Jit/Backend/AArch64Backend.hpp"
#include "Jit/Backend/RiscV64Backend.hpp"
#include "tests/JitRandom.hpp"

using namespace jit;

void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  fprintf(stderr, "BOUNDS VIOLATION: %s (%llu, %llu)\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

namespace {

struct Alloc final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return size ? aligned_alloc(64, (size + 63) / 64 * 64) : nullptr; }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t a = alignment < 64 ? 64 : alignment;
    return size ? aligned_alloc(a, (size + a - 1) / a * a) : nullptr;
  }
  void deallocate(void* p) override { free(p); }
};

uint64_t host_helper_mix(void* state, uint64_t a, uint64_t b) {
  uint64_t word0;
  memcpy(&word0, state, 8);
  return (a * 31) ^ (b + word0);
}

uint64_t host_helper_state(void* state, uint64_t, uint64_t) { return reinterpret_cast<uintptr_t>(state); }

constexpr int kRandomPrograms = 400;
constexpr int kFloatPrograms = 8;   // pointer access and double precision operations, one per register budget
constexpr uint32_t kBigStateSize = 80000;
constexpr int kLoopPrograms = 4;   // the nested-loop program with different trip counts (state word 0), after the random ones

// Pointer access and floating point. The inputs are state words 0..7 (random bytes): integers turned into doubles, so that no NaN or infinity appears except where the
// program makes them from constants (a NaN has a different bit pattern on each CPU, so it only meets comparisons and conversions to integers). The state block's own
// address (from a helper) is what the loads and stores go through.
void build_float(IrBuilder& b, uint64_t state_helper) {
  b.set_state_size(8192);
  const uint32_t p = state_helper ? b.helper_at(state_helper, b.constant(0), b.constant(0)) : b.helper(host_helper_state, b.constant(0), b.constant(0));
  uint32_t x[4];
  for (int i = 0; i < 4; ++i) {
    const uint32_t small = b.binary(IrOp::Sub, b.binary(IrOp::And, b.load_state(8 * i), b.constant(0xFFFFF)), b.constant(0x80000));
    x[i] = b.unary(IrOp::I2F, small);
  }
  const uint32_t half = b.constant(__builtin_bit_cast(int64_t, 0.5));
  uint32_t slot = 8;
  auto out = [&](uint32_t v) { b.store_state(8 * slot++, v); };
  out(b.binary(IrOp::FAdd, x[0], x[1]));
  out(b.binary(IrOp::FSub, x[0], x[1]));
  out(b.binary(IrOp::FMul, x[0], x[1]));
  out(b.binary(IrOp::FDiv, x[2], b.binary(IrOp::FAdd, b.unary(IrOp::I2F, b.binary(IrOp::And, b.load_state(24), b.constant(0xFF))), half)));
  out(b.binary(IrOp::FDiv, x[3], b.constant(0)));   // x / +0: an infinity, or a NaN for 0 / 0 (compared below, never stored raw)
  const uint32_t zero_div = b.binary(IrOp::FDiv, b.constant(__builtin_bit_cast(int64_t, 1.0)), b.constant(0));
  out(zero_div);
  const uint32_t nan = b.constant(0x7FF8000000000000ll), inf = b.constant(0x7FF0000000000000ll), ninf = b.constant(static_cast<int64_t>(0xFFF0000000000000ull));
  const uint32_t operands[] = {x[0], x[1], nan, inf, ninf, b.constant(0)};
  const IrCond conds[] = {IrCond::Eq, IrCond::Ne, IrCond::Lt, IrCond::Le, IrCond::Gt, IrCond::Ge};
  for (uint32_t u : operands)
    for (uint32_t v : operands)
      for (IrCond c : conds) out(b.fsetcond(c, u, v));
  // conversions to integers: in range, fractions, out of range, NaN, infinities
  const uint32_t big = b.constant(__builtin_bit_cast(int64_t, 1e30)), mid = b.constant(__builtin_bit_cast(int64_t, 9.2e18)), ninety = b.constant(__builtin_bit_cast(int64_t, -2.75));
  for (uint32_t v : {x[0], x[1], b.binary(IrOp::FMul, x[0], big), b.binary(IrOp::FMul, x[1], mid), ninety, nan, inf, ninf, b.constant(0), b.constant(static_cast<int64_t>(0x8000000000000000ull))})
    out(b.unary(IrOp::F2I, v));
  out(b.unary(IrOp::I2F, b.load_state(32)));
  out(b.unary(IrOp::I2F, b.constant(-1)));
  // single precision: a few finite floats widened, doubles narrowed (rounding, overflow to infinity, tiny)
  for (int64_t f : {0x3F800000ll, 0xC0490FDBll, 0x00000001ll, 0x7F7FFFFFll, 0x80000000ll, 0x7F800000ll}) out(b.unary(IrOp::F2D, b.constant(f)));
  for (uint32_t v : {x[0], b.binary(IrOp::FDiv, x[1], b.constant(__builtin_bit_cast(int64_t, 3.0))), b.binary(IrOp::FMul, x[0], big), b.constant(__builtin_bit_cast(int64_t, 1e-300)), inf, ninf})
    out(b.unary(IrOp::D2F, v));
  // pointer access: every width and signedness from the (random) input words at odd offsets, stores of every width and what they leave in memory
  const IrMem loads[] = {IrMem::U8, IrMem::U16, IrMem::U32, IrMem::U64, IrMem::S8, IrMem::S16, IrMem::S32};
  for (IrMem m : loads)
    for (int64_t offset : {0, 3, 8, 13}) out(b.load_ptr(m, p, offset));
  const uint32_t v = b.load_state(40);
  b.store_ptr(IrMem::U8, p, 4096, v);
  b.store_ptr(IrMem::U16, p, 4106, v);
  b.store_ptr(IrMem::U32, p, 4116, v);
  b.store_ptr(IrMem::U64, p, 4128, v);
  b.store_ptr(IrMem::U64, p, 4144, b.load_ptr(IrMem::S16, p, 4098));
  for (int64_t offset : {4096, 4104, 4112, 4120, 4128, 4144}) out(b.load_ptr(IrMem::U64, p, offset));
  // an address far from the base (beyond a 12 bit offset): the sum is made first
  const uint32_t far = b.binary(IrOp::Add, p, b.constant(100000));
  b.store_ptr(IrMem::U32, far, -99000, b.constant(0x5A5A1234));
  out(b.load_ptr(IrMem::U64, far, -99000));
  b.exit_value(b.constant(11));
}

// Program `index`: 0..kRandomPrograms-1 random ones (some very long, to force frames above 4 KiB and branches over many instructions), then the
// special cases. `address` is the runner's helper (0: use the host's, for the interpreter).
bool build(Alloc& a, IrBlock& block, int index, uint64_t address, uint64_t state_helper, uint32_t* state_size) {
  IrBuilder b(block);
  if (index >= kRandomPrograms + kLoopPrograms + 1) {
    *state_size = 8192;
    build_float(b, state_helper);
  } else if (index < kRandomPrograms) {
    uint64_t seed = 0x9E3779B97F4A7C15ull * static_cast<uint64_t>(index + 1);
    const uint32_t length = index % 50 == 49 ? 3000 : 4 + index % 70;
    *state_size = 64;
    b.set_state_size(64);
    build_random(b, seed, length, host_helper_mix, address);
  } else if (index < kRandomPrograms + kLoopPrograms) {
    *state_size = 64;
    b.set_state_size(64);
    build_loops(b, host_helper_mix, address);
  } else {
    // state words far from the base register, beyond the 32 KiB reach of a load/store immediate
    *state_size = kBigStateSize;
    b.set_state_size(kBigStateSize);
    const uint32_t x = b.load_state(70000);
    const uint32_t y = b.binary(IrOp::Add, x, b.constant(0x123456789abcdefll));
    b.store_state(79992, y);
    b.store_state(40000, b.constant(77));
    b.exit_value(y);
  }
  (void)a;
  return !b.failed() && validate(a, block);
}

void input_state(std::vector<uint8_t>& state, int index) {
  uint64_t x = 0x2545F4914F6CDD1Dull * static_cast<uint64_t>(index + 7);
  for (size_t i = 0; i < state.size(); ++i) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    state[i] = static_cast<uint8_t>(x >> 24);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) return 2;
  const bool gen = strcmp(argv[1], "gen") == 0;
  std::string dir = argv[2];
  const uint64_t address = strtoull(argv[3], nullptr, 16);
  const bool riscv = strcmp(argv[4], "riscv64") == 0;
  const uint64_t state_helper = strtoull(argv[5], nullptr, 16);
  Alloc a;
  const int total = kRandomPrograms + kLoopPrograms + 1 + kFloatPrograms;
  if (gen) {
    FILE* f = fopen((dir + "/cases.bin").c_str(), "wb");
    if (!f) return 2;
    const uint32_t count = total;
    fwrite(&count, 4, 1, f);
    for (int i = 0; i < total; ++i) {
      IrBlock block(a);
      uint32_t state_size = 0;
      if (!build(a, block, i, address, state_helper, &state_size)) { printf("program %d invalid\n", i); return 1; }
      // (a different register pool size for each program: none at all, one, a few, and the whole pool, to take every path through the allocator)
      static const uint32_t limits[] = {0, 1, 2, 3, 5, 9, 12, 32};
      const uint32_t limit = limits[i % 8];
      AArch64Backend arm(a, limit);
      RiscV64Backend rv(a, limit);
      IJitBackend& backend = riscv ? static_cast<IJitBackend&>(rv) : static_cast<IJitBackend&>(arm);
      CodeBuffer code(a);
      if (!backend.compile(block, code)) { printf("program %d did not compile\n", i); return 1; }
      std::vector<uint8_t> state(state_size);
      input_state(state, i);
      const uint32_t sizes[2] = {static_cast<uint32_t>(code.size()), state_size};
      fwrite(sizes, 4, 2, f);
      fwrite(code.view().data(), 1, code.size(), f);
      fwrite(state.data(), 1, state.size(), f);
    }
    fclose(f);
    printf("CrossJitTest(%s): generated %d programs\n", argv[4], total);
    return 0;
  }
  FILE* f = fopen((dir + "/results.bin").c_str(), "rb");
  if (!f) { printf("no results\n"); return 1; }
  int failed = 0;
  for (int i = 0; i < total; ++i) {
    IrBlock block(a);
    uint32_t state_size = 0;
    if (!build(a, block, i, 0, 0, &state_size)) return 1;
    std::vector<uint8_t> want(state_size), got(state_size);
    input_state(want, i);
    uint64_t want_exit = 0, got_exit = 0;
    if (!interpret(a, block, ByteView(want.data(), want.size()), 1000000, &want_exit)) { printf("program %d did not finish\n", i); return 1; }
    if (fread(&got_exit, 8, 1, f) != 1 || fread(got.data(), 1, state_size, f) != state_size) { printf("results truncated at program %d\n", i); return 1; }
    if (got_exit != want_exit || memcmp(got.data(), want.data(), state_size) != 0) {
      printf("program %d: generated code exit %llx, interpreter %llx\n", i, (unsigned long long)got_exit, (unsigned long long)want_exit);
      if (++failed > 5) break;
    }
  }
  fclose(f);
  if (failed) return 1;
  printf("CrossJitTest(%s): %d programs agree with the interpreter\n", argv[4], total);
  return 0;
}
