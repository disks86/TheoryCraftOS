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
//   CrossJitTest gen <dir> <helper-address> <aarch64|riscv64>      CrossJitTest check <dir> <helper-address> <aarch64|riscv64>
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

constexpr int kRandomPrograms = 400;
constexpr uint32_t kBigStateSize = 80000;
constexpr int kLoopPrograms = 4;   // the nested-loop program with different trip counts (state word 0), after the random ones

// Program `index`: 0..kRandomPrograms-1 random ones (some very long, to force frames above 4 KiB and branches over many instructions), then the
// special cases. `address` is the runner's helper (0: use the host's, for the interpreter).
bool build(Alloc& a, IrBlock& block, int index, uint64_t address, uint32_t* state_size) {
  IrBuilder b(block);
  if (index < kRandomPrograms) {
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
  if (argc != 5) return 2;
  const bool gen = strcmp(argv[1], "gen") == 0;
  std::string dir = argv[2];
  const uint64_t address = strtoull(argv[3], nullptr, 16);
  const bool riscv = strcmp(argv[4], "riscv64") == 0;
  Alloc a;
  const int total = kRandomPrograms + kLoopPrograms + 1;
  if (gen) {
    FILE* f = fopen((dir + "/cases.bin").c_str(), "wb");
    if (!f) return 2;
    const uint32_t count = total;
    fwrite(&count, 4, 1, f);
    for (int i = 0; i < total; ++i) {
      IrBlock block(a);
      uint32_t state_size = 0;
      if (!build(a, block, i, address, &state_size)) { printf("program %d invalid\n", i); return 1; }
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
    if (!build(a, block, i, 0, &state_size)) return 1;
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
