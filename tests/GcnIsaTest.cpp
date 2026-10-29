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

// The GCN3 assembler against LLVM's: every fixture (tests/GcnIsaFixtures.inc, made by scripts/gen_gcn_fixtures.py with llvm-mc) is built with gcn::Code and the words compared; then the
// operand helpers, the branches, the range checks and instruction_length over all of the fixtures.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnIsa.hpp"

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

struct Fixture {
  const char* assembly;
  std::vector<uint32_t> words;
  void (*build)(gcn::Code&);
};
static const Fixture kFixtures[] = {
#include "GcnIsaFixtures.inc"
};

static void test_fixtures() {
  TestAllocator alloc;
  for (const Fixture& f : kFixtures) {
    gcn::Code c(alloc);
    f.build(c);
    bool same = c.ok() && c.size() == f.words.size();
    for (uint32_t i = 0; same && i < c.size(); ++i) same = c.word(i) == f.words[i];
    if (!same) {
      printf("FAILED %s: got", f.assembly);
      for (uint32_t i = 0; i < c.size(); ++i) printf(" %08X", c.word(i));
      printf(", want");
      for (uint32_t w : f.words) printf(" %08X", w);
      printf("\n");
      ++g_failed;
    }
    // the length of what was made, read back
    CHECK(gcn::instruction_length(c.words()) == f.words.size());
  }
}

static void test_operands() {
  CHECK(gcn::imm_int(0).code == 128 && !gcn::imm_int(0).literal && gcn::imm_int(64).code == 192 && gcn::imm_int(-1).code == 193 && gcn::imm_int(-16).code == 208);
  CHECK(gcn::imm_int(65).literal && gcn::imm_int(65).code == 255 && gcn::imm_int(65).value == 65 && gcn::imm_int(-17).literal && gcn::imm_int(-17).value == 0xFFFFFFEFu);
  CHECK(gcn::imm_float(0.5f).code == 240 && gcn::imm_float(-0.5f).code == 241 && gcn::imm_float(1.0f).code == 242 && gcn::imm_float(-1.0f).code == 243 && gcn::imm_float(2.0f).code == 244 &&
        gcn::imm_float(-2.0f).code == 245 && gcn::imm_float(4.0f).code == 246 && gcn::imm_float(-4.0f).code == 247);
  CHECK(gcn::imm_float(0.0f).code == 128 && gcn::imm_float(3.0f).literal && gcn::imm_float(3.0f).value == 0x40400000u);
  CHECK(gcn::imm_bits(0xFFFFFFFFu).code == 193 && gcn::imm_bits(0xFFFFFFF0u).code == 208 && gcn::imm_bits(0xFFFFFFEFu).literal);
  CHECK(gcn::sgpr(5).code == 5 && gcn::vgpr(5).code == 261 && gcn::vcc().code == 106 && gcn::exec().code == 126 && gcn::m0().code == 124 && gcn::scc().code == 253);
}

static void test_branches() {
  TestAllocator alloc;
  gcn::Code c(alloc);
  c.sopp(gcn::Sopp::s_nop, 0);                                       // 0
  const uint32_t skip = c.branch(gcn::Sopp::s_cbranch_execz);        // 1
  c.sop1(gcn::Sop1::s_mov_b32, 0, gcn::imm_bits(0x12345678u));       // 2, 3
  c.sopp(gcn::Sopp::s_nop, 0);                                       // 4
  c.patch_branch(skip, c.size());                                    // to 5: offset 3
  CHECK(c.word(1) == 0xBF880003u);
  const uint32_t back = c.branch(gcn::Sopp::s_branch);               // 5
  c.patch_branch(back, 0);                                           // offset -6
  CHECK(c.word(5) == (0xBF820000u | 0xFFFAu));
  CHECK(c.ok());
  c.patch_branch(99, 0);
  CHECK(!c.ok());
}

static void test_range_checks() {
  TestAllocator alloc;
  struct Case { const char* what; void (*run)(gcn::Code&); };
  const Case cases[] = {
      {"sdst 128", [](gcn::Code& c) { c.sop1(gcn::Sop1::s_mov_b32, 128, gcn::sgpr(0)); }},
      {"two different literals", [](gcn::Code& c) { c.sop2(gcn::Sop2::s_add_u32, 0, gcn::imm_int(1000), gcn::imm_int(2000)); }},
      {"simm16 too big", [](gcn::Code& c) { c.sopk(gcn::Sopk::s_movk_i32, 0, 70000); }},
      {"waitcnt vm", [](gcn::Code& c) { c.waitcnt(16, 0, 0); }},
      {"odd sbase", [](gcn::Code& c) { c.smem(gcn::Smem::s_load_dwordx4, 0, 3, 0); }},
      {"smem offset", [](gcn::Code& c) { c.smem(gcn::Smem::s_load_dword, 0, 2, 1u << 20); }},
      {"vdst 256", [](gcn::Code& c) { c.vop1(gcn::Vop1::v_mov_b32, 256, gcn::vgpr(0)); }},
      {"vsrc1 256", [](gcn::Code& c) { c.vop2(gcn::Vop2::v_add_f32, 0, gcn::vgpr(0), 256); }},
      {"vop3 literal", [](gcn::Code& c) { c.vop3(gcn::Vop3::v_mad_f32, 0, gcn::imm_bits(0x12345678u), gcn::vgpr(1), gcn::vgpr(2)); }},
      {"vop3 mods", [](gcn::Code& c) { c.vop3(gcn::Vop3::v_mad_f32, 0, gcn::vgpr(0), gcn::vgpr(1), gcn::vgpr(2), gcn::Mods{8, 0, false, 0}); }},
      {"attr 64", [](gcn::Code& c) { c.vintrp(gcn::Vintrp::v_interp_p1_f32, 0, 0, 64, 0); }},
      {"export target", [](gcn::Code& c) { c.exp(64, 15, 0, 0, 0, 0, false, false); }},
      {"buffer offset", [](gcn::Code& c) { c.mubuf(gcn::Mubuf::buffer_load_dword, 0, 0, 8, gcn::imm_int(0), gcn::Code::BufferMode{false, false, false, false, 4096}); }},
      {"unaligned srsrc", [](gcn::Code& c) { c.mubuf(gcn::Mubuf::buffer_load_dword, 0, 0, 9, gcn::imm_int(0), gcn::Code::BufferMode()); }},
      {"image dmask", [](gcn::Code& c) { c.mimg(gcn::Mimg::image_sample, 0, 0, 8, 12, 16); }},
  };
  for (const Case& k : cases) {
    gcn::Code c(alloc);
    k.run(c);
    if (c.ok()) {
      printf("FAILED: %s was accepted\n", k.what);
      ++g_failed;
    }
  }
  // the same literal twice is fine
  gcn::Code c(alloc);
  c.sop2(gcn::Sop2::s_add_u32, 0, gcn::imm_int(1000), gcn::imm_int(1000));
  CHECK(c.ok() && c.size() == 2);
  // an empty or unknown word has no length
  CHECK(gcn::instruction_length(ArrayView<const uint32_t>()) == 0);
  const uint32_t unknown[] = {0xFC000000u};
  CHECK(gcn::instruction_length(ArrayView<const uint32_t>(unknown, 1)) == 0);
}

int main() {
  test_fixtures();
  test_operands();
  test_branches();
  test_range_checks();
  printf(g_failed ? "GcnIsaTest: %d FAILED\n" : "GcnIsaTest: all passed (%d fixtures)\n", g_failed ? g_failed : static_cast<int>(sizeof kFixtures / sizeof kFixtures[0]));
  return g_failed ? 1 : 0;
}
