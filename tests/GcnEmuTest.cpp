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

// The GCN3 emulator: programs assembled with gcn::Code run on a wave against a simulated environment (memory, interpolation, sampling, exports): scalar arithmetic and loops, the
// vector float and integer instructions against the same computation on the CPU, the comparisons including NaN, exec masking with the saveexec idiom, buffer and scalar loads, the
// interpolation, the export and the failures (bad instruction, memory fault, step limit).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnEmu.hpp"

using namespace gcn;

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

static float bits_to_float_bits(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t float_to_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

struct Env final : IEnvironment {
  std::map<uint64_t, uint32_t> memory;
  struct Export { uint32_t target, enable; bool compressed, done; uint32_t values[kWaveSize][4]; uint64_t lanes; };
  std::vector<Export> exports;
  float p0 = 0.25f, p10 = 2.0f, p20 = 3.0f;
  uint32_t samples = 0;
  bool load32(uint64_t a, uint32_t* v) override {
    auto it = memory.find(a);
    if (it == memory.end()) return false;
    *v = it->second;
    return true;
  }
  bool store32(uint64_t a, uint32_t v) override {
    if (a < 0x1000) return false;
    memory[a] = v;
    return true;
  }
  void interpolation(uint32_t attr, uint32_t chan, uint32_t lane, float* a, float* b, float* c) override {
    *a = p0 + static_cast<float>(attr * 10 + chan);
    *b = p10;
    *c = p20 + static_cast<float>(lane);
  }
  void sample(Mimg op, const uint32_t image[8], const uint32_t sampler[4], const float* coords, uint32_t count, float result[4]) override {
    ++samples;
    result[0] = coords[0];
    result[1] = count > 1 ? coords[1] : 0.0f;
    result[2] = static_cast<float>(image[0]);
    result[3] = static_cast<float>(static_cast<uint32_t>(op)) + static_cast<float>(sampler[0]);
  }
  void export_data(uint32_t target, uint32_t enable, bool compressed, const uint32_t values[kWaveSize][4], uint64_t lanes, bool done) override {
    Export e{target, enable, compressed, done, {}, lanes};
    memcpy(e.values, values, sizeof e.values);
    exports.push_back(e);
  }
};

static Status run(Wave& w, Env& env, const Code& c, uint64_t max_steps = 100000) {
  return w.run(c.words(), env, max_steps);
}

static void test_scalar() {
  TestAllocator alloc;
  Env env;
  Wave w;
  // sum of 1..10 with a loop: s0 = 0, s1 = 1; do { s0 += s1; s1 += 1 } while (s1 <= 10)
  Code c(alloc);
  c.sop1(Sop1::s_mov_b32, 0, imm_int(0));
  c.sop1(Sop1::s_mov_b32, 1, imm_int(1));
  const uint32_t top = c.size();
  c.sop2(Sop2::s_add_u32, 0, sgpr(0), sgpr(1));
  c.sop2(Sop2::s_add_u32, 1, sgpr(1), imm_int(1));
  c.sopc(Sopc::s_cmp_le_u32, sgpr(1), imm_int(10));
  c.patch_branch(c.branch(Sopp::s_cbranch_scc1), top);
  c.sop1(Sop1::s_mov_b32, 2, imm_bits(0x12345678u));
  c.sopk(Sopk::s_movk_i32, 3, -2);
  c.sop2(Sop2::s_lshl_b32, 4, sgpr(1), imm_int(4));
  c.sop2(Sop2::s_mul_i32, 5, sgpr(0), imm_int(3));
  c.sop2(Sop2::s_and_b64, 6, imm_int(-1), imm_bits(0xF0F0F0F0u));
  c.sopp(Sopp::s_endpgm);
  CHECK(c.ok() && run(w, env, c) == Status::Done);
  CHECK(w.sgpr[0] == 55 && w.sgpr[1] == 11 && w.sgpr[2] == 0x12345678u && w.sgpr[3] == 0xFFFFFFFEu && w.sgpr[4] == 176 && w.sgpr[5] == 165);
  CHECK(w.sgpr[6] == 0xF0F0F0F0u && w.sgpr[7] == 0);       // (the inline -1 is sign extended to 64 bits, the literal is zero extended)
  // carries: s_add_u32 sets SCC on overflow, s_addc_u32 uses it; s_sub_u32 borrows
  Wave v;
  Code d(alloc);
  d.sop1(Sop1::s_mov_b32, 0, imm_bits(0xFFFFFFFFu));
  d.sop2(Sop2::s_add_u32, 1, sgpr(0), imm_int(1));       // 0, carry
  d.sop2(Sop2::s_addc_u32, 2, imm_int(0), imm_int(0));   // 1
  d.sop2(Sop2::s_sub_u32, 3, imm_int(0), imm_int(1));    // 0xFFFFFFFF, borrow
  d.sop2(Sop2::s_subb_u32, 4, imm_int(5), imm_int(0));   // 4 (borrow)
  d.sop2(Sop2::s_cselect_b32, 5, imm_int(7), imm_int(9)); // scc is 0 after s_subb (5 - 0 - 1 does not borrow): 9
  d.sopp(Sopp::s_endpgm);
  CHECK(run(v, env, d) == Status::Done && v.sgpr[1] == 0 && v.sgpr[2] == 1 && v.sgpr[3] == 0xFFFFFFFFu && v.sgpr[4] == 4 && v.sgpr[5] == 9);
  // branches: execz skips, vccz and vccnz
  Wave u;
  Code e(alloc);
  e.sop1(Sop1::s_mov_b32, 0, imm_int(0));
  e.sop1(Sop1::s_mov_b64, 126, imm_int(0));        // exec = 0
  const uint32_t skip = e.branch(Sopp::s_cbranch_execz);
  e.sop1(Sop1::s_mov_b32, 0, imm_int(99));          // skipped
  e.patch_branch(skip, e.size());
  e.sop1(Sop1::s_mov_b64, 126, imm_int(-1));
  const uint32_t no = e.branch(Sopp::s_cbranch_execz);        // exec is not zero: falls through
  e.sop1(Sop1::s_mov_b32, 1, imm_int(5));
  e.patch_branch(no, e.size());
  e.sopp(Sopp::s_endpgm);
  CHECK(run(u, env, e) == Status::Done && u.sgpr[0] == 0 && u.sgpr[1] == 5);
}

// a float with a few special cases for the lanes: lane n gets values[n % N]
static const float kValues[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.5f, -2.5f, 3.75f, 100.0f, 1e-3f, 0.3f, 8388608.5f, -7.5f, 65504.0f, 1.0e30f, 6.5f};
static const uint32_t kCount = sizeof kValues / sizeof kValues[0];

static void fill(Wave& w, uint32_t reg, float (*f)(uint32_t lane)) {
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) w.set_vgpr_f(reg, lane, f(lane));
}
static float va(uint32_t lane) { return kValues[lane % kCount]; }
static float vb(uint32_t lane) { return kValues[(lane * 7 + 3) % kCount]; }
static float vc(uint32_t lane) { return kValues[(lane * 5 + 1) % kCount]; }

template <class Op, class Ref>
static void unary(Op op, Ref ref, float tolerance = 0.0f) {
  TestAllocator alloc;
  Env env;
  Wave w;
  fill(w, 1, va);
  Code c(alloc);
  op(c);
  c.sopp(Sopp::s_endpgm);
  if (!c.ok() || run(w, env, c) != Status::Done) { ++g_failed; printf("FAILED: a unary program\n"); return; }
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    const float want = ref(va(lane)), got = w.vgpr_f(0, lane);
    const bool both_nan = want != want && got != got;
    const float err = fabsf(got - want);
    if (!both_nan && got != want && !(err <= tolerance * (fabsf(want) > 1.0f ? fabsf(want) : 1.0f))) {
      printf("FAILED unary lane %u: input %g got %g want %g\n", lane, va(lane), got, want);
      ++g_failed;
      return;
    }
  }
}

static void test_vector_float() {
  unary([](Code& c) { c.vop1(Vop1::v_mov_b32, 0, vgpr(1)); }, [](float x) { return x; });
  unary([](Code& c) { c.vop1(Vop1::v_floor_f32, 0, vgpr(1)); }, [](float x) { return floorf(x); });
  unary([](Code& c) { c.vop1(Vop1::v_ceil_f32, 0, vgpr(1)); }, [](float x) { return ceilf(x); });
  unary([](Code& c) { c.vop1(Vop1::v_trunc_f32, 0, vgpr(1)); }, [](float x) { return truncf(x); });
  unary([](Code& c) { c.vop1(Vop1::v_rndne_f32, 0, vgpr(1)); }, [](float x) { return nearbyintf(x); });
  unary([](Code& c) { c.vop1(Vop1::v_fract_f32, 0, vgpr(1)); }, [](float x) { return x - floorf(x); });
  unary([](Code& c) { c.vop1(Vop1::v_sqrt_f32, 0, vgpr(1)); }, [](float x) { return sqrtf(x); }, 1e-6f);
  unary([](Code& c) { c.vop1(Vop1::v_rcp_f32, 0, vgpr(1)); }, [](float x) { return 1.0f / x; }, 1e-6f);
  unary([](Code& c) { c.vop1(Vop1::v_rsq_f32, 0, vgpr(1)); }, [](float x) { return 1.0f / sqrtf(x); }, 1e-6f);
  unary([](Code& c) { c.vop1(Vop1::v_exp_f32, 0, vgpr(1)); }, [](float x) { return exp2f(x); }, 1e-6f);
  unary([](Code& c) { c.vop1(Vop1::v_log_f32, 0, vgpr(1)); }, [](float x) { return log2f(x); }, 1e-6f);
  unary([](Code& c) { c.vop1(Vop1::v_sin_f32, 0, vgpr(1)); }, [](float x) { return sinf((x - floorf(x)) * 6.2831853f); }, 1e-3f);
  unary([](Code& c) { c.vop1(Vop1::v_cos_f32, 0, vgpr(1)); }, [](float x) { return cosf((x - floorf(x)) * 6.2831853f); }, 1e-3f);
  // source modifiers and output modifiers on the VOP3 form (of v_add_f32 with 0): abs, neg, clamp, omod
  unary([](Code& c) { c.vop3_of(Vop2::v_add_f32, 0, vgpr(1), imm_float(0.0f), Mods{0, 1, false, 0}); }, [](float x) { return fabsf(x) + 0.0f; });
  unary([](Code& c) { c.vop3_of(Vop2::v_add_f32, 0, vgpr(1), imm_float(0.0f), Mods{1, 0, false, 0}); }, [](float x) { return -x + 0.0f; });
  unary([](Code& c) { c.vop3_of(Vop2::v_add_f32, 0, vgpr(1), imm_float(0.0f), Mods{1, 1, false, 0}); }, [](float x) { return -fabsf(x) + 0.0f; });
  unary([](Code& c) { c.vop3_of(Vop2::v_add_f32, 0, vgpr(1), imm_float(0.0f), Mods{0, 0, true, 0}); }, [](float x) { return x < 0 ? 0.0f : (x > 1 ? 1.0f : x); });
  unary([](Code& c) { c.vop3_of(Vop2::v_add_f32, 0, vgpr(1), imm_float(0.0f), Mods{0, 0, false, 1}); }, [](float x) { return x * 2.0f; });
  unary([](Code& c) { c.vop3_of(Vop2::v_add_f32, 0, vgpr(1), imm_float(0.0f), Mods{0, 0, false, 2}); }, [](float x) { return x * 4.0f; });
  unary([](Code& c) { c.vop3_of(Vop2::v_add_f32, 0, vgpr(1), imm_float(0.0f), Mods{0, 0, false, 3}); }, [](float x) { return x * 0.5f; });
  unary([](Code& c) { c.vop3_of(Vop2::v_add_f32, 0, vgpr(1), imm_float(0.0f), Mods{1, 0, true, 1}); }, [](float x) { const float y = -x * 2.0f; return y < 0 ? 0.0f : (y > 1 ? 1.0f : y); });
  unary([](Code& c) { c.vop1(Vop1::v_cvt_i32_f32, 0, vgpr(1)); c.vop1(Vop1::v_cvt_f32_i32, 0, vgpr(0)); }, [](float x) { return x >= 2147483648.0f ? 2147483648.0f : truncf(x); });
  // binary and ternary ops over all lanes
  TestAllocator alloc;
  Env env;
  Wave w;
  fill(w, 1, va);
  fill(w, 2, vb);
  fill(w, 3, vc);
  Code c(alloc);
  c.vop2(Vop2::v_add_f32, 10, vgpr(1), 2);
  c.vop2(Vop2::v_sub_f32, 11, vgpr(1), 2);
  c.vop2(Vop2::v_subrev_f32, 12, vgpr(1), 2);
  c.vop2(Vop2::v_mul_f32, 13, vgpr(1), 2);
  c.vop2(Vop2::v_min_f32, 14, vgpr(1), 2);
  c.vop2(Vop2::v_max_f32, 15, vgpr(1), 2);
  c.vop1(Vop1::v_mov_b32, 16, vgpr(3));
  c.vop2(Vop2::v_mac_f32, 16, vgpr(1), 2);                         // v16 = v3 + v1 * v2
  c.vop3(Vop3::v_mad_f32, 17, vgpr(1), vgpr(2), vgpr(3));
  c.vop3(Vop3::v_fma_f32, 18, vgpr(1), vgpr(2), vgpr(3));
  c.vop3(Vop3::v_med3_f32, 19, vgpr(1), vgpr(2), vgpr(3));
  c.vop3(Vop3::v_min3_f32, 20, vgpr(1), vgpr(2), vgpr(3));
  c.vop3(Vop3::v_max3_f32, 21, vgpr(1), vgpr(2), vgpr(3));
  c.vop2(Vop2::v_add_f32, 22, imm_float(1.5f), 1);
  c.vop2(Vop2::v_mul_f32, 23, imm_bits(0x40490fdbu), 1);         // a literal
  c.vop2_literal(Vop2::v_madmk_f32, 24, vgpr(1), 2, float_to_bits(2.0f));       // v1 * 2.0 + v2
  c.vop2_literal(Vop2::v_madak_f32, 25, vgpr(1), 2, float_to_bits(0.5f));       // v1 * v2 + 0.5
  c.vop3_of(Vop2::v_add_f32, 26, vgpr(1), vgpr(2), Mods{1, 2, false, 0});       // -v1 + |v2|
  c.sopp(Sopp::s_endpgm);
  CHECK(c.ok() && run(w, env, c) == Status::Done);
  bool ok = true;
  for (uint32_t lane = 0; lane < kWaveSize && ok; ++lane) {
    const float a = va(lane), b = vb(lane), cc = vc(lane);
    const float want[17] = {a + b, a - b, b - a, a * b, fminf(a, b), fmaxf(a, b), cc + a * b, a * b + cc, static_cast<float>(static_cast<double>(a) * b + cc),
                            fmaxf(fminf(a, b), fminf(fmaxf(a, b), cc)), fminf(fminf(a, b), cc), fmaxf(fmaxf(a, b), cc), 1.5f + a, 3.14159274f * a, a * 2.0f + b, a * b + 0.5f, -a + fabsf(b)};
    for (uint32_t i = 0; i < 17; ++i) {
      const float got = w.vgpr_f(10 + i, lane), tol = i == 7 ? 1e-6f * fabsf(want[i]) : 0.0f;
      if (!(fabsf(got - want[i]) <= tol) && !(got == want[i])) {
        printf("FAILED binary %u lane %u: a %g b %g c %g got %g want %g\n", i, lane, a, b, cc, got, want[i]);
        ++g_failed;
        ok = false;
        break;
      }
    }
  }
  // denormals are flushed: a product that is denormal is zero, a denormal input counts as zero
  Wave d;
  d.set_vgpr_f(1, 0, 1e-30f);
  d.set_vgpr_f(2, 0, 1e-30f);
  d.vgpr[3][0] = 0x00000001u;       // the smallest denormal
  Code e(alloc);
  e.vop2(Vop2::v_mul_f32, 4, vgpr(1), 2);
  e.vop2(Vop2::v_add_f32, 5, vgpr(3), 1);
  e.sopp(Sopp::s_endpgm);
  CHECK(run(d, env, e) == Status::Done && d.vgpr[4][0] == 0 && d.vgpr_f(5, 0) == 1e-30f);
  // NaN: min and max return the number; a comparison with NaN
  Wave n;
  n.vgpr[1][0] = 0x7FC00000u;
  n.set_vgpr_f(2, 0, 3.0f);
  Code f(alloc);
  f.vop2(Vop2::v_min_f32, 3, vgpr(1), 2);
  f.vop2(Vop2::v_max_f32, 4, vgpr(1), 2);
  f.sopp(Sopp::s_endpgm);
  CHECK(run(n, env, f) == Status::Done && n.vgpr_f(3, 0) == 3.0f && n.vgpr_f(4, 0) == 3.0f);
}

static void test_integer_and_conversions() {
  TestAllocator alloc;
  Env env;
  Wave w;
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    w.vgpr[1][lane] = lane * 1000003u + 17;
    w.vgpr[2][lane] = (lane % 7) + 1;
    w.vgpr[3][lane] = 0xFFFFFFF0u + lane;
  }
  Code c(alloc);
  c.vop2(Vop2::v_and_b32, 10, imm_int(0xFF), 1);
  c.vop2(Vop2::v_or_b32, 11, vgpr(1), 2);
  c.vop2(Vop2::v_xor_b32, 12, vgpr(1), 2);
  c.vop2(Vop2::v_lshlrev_b32, 13, imm_int(3), 1);        // v1 << 3
  c.vop2(Vop2::v_lshrrev_b32, 14, imm_int(3), 1);
  c.vop2(Vop2::v_ashrrev_i32, 15, imm_int(3), 3);
  c.vop2(Vop2::v_min_u32, 16, vgpr(1), 3);
  c.vop2(Vop2::v_max_i32, 17, vgpr(1), 3);
  c.vop2(Vop2::v_mul_u32_u24, 18, vgpr(1), 2);
  c.vop1(Vop1::v_not_b32, 19, vgpr(2));
  c.vop3(Vop3::v_bfe_u32, 20, vgpr(1), imm_int(4), imm_int(8));
  c.vop3(Vop3::v_bfi_b32, 21, vgpr(1), vgpr(2), vgpr(3));
  c.vop2(Vop2::v_add_u32, 22, vgpr(1), 3);               // carry into VCC
  c.sop1(Sop1::s_mov_b64, 4, vcc());                     // s[4:5] = vcc
  c.vop1(Vop1::v_cvt_f32_u32, 23, vgpr(2));
  c.vop1(Vop1::v_cvt_u32_f32, 24, vgpr(23));
  c.sopp(Sopp::s_endpgm);
  CHECK(c.ok() && run(w, env, c) == Status::Done);
  uint64_t carry = 0;
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    const uint32_t a = lane * 1000003u + 17, b = (lane % 7) + 1, d = 0xFFFFFFF0u + lane;
    CHECK(w.vgpr[10][lane] == (a & 0xFF) && w.vgpr[11][lane] == (a | b) && w.vgpr[12][lane] == (a ^ b) && w.vgpr[13][lane] == (a << 3) && w.vgpr[14][lane] == (a >> 3));
    CHECK(w.vgpr[15][lane] == static_cast<uint32_t>(static_cast<int32_t>(d) >> 3) && w.vgpr[16][lane] == (a < d ? a : d) && w.vgpr[17][lane] == (static_cast<int32_t>(a) > static_cast<int32_t>(d) ? a : d));
    CHECK(w.vgpr[18][lane] == (a & 0xFFFFFF) * b && w.vgpr[19][lane] == ~b && w.vgpr[20][lane] == ((a >> 4) & 0xFF) && w.vgpr[21][lane] == ((a & b) | (~a & d)));
    CHECK(w.vgpr[22][lane] == a + d && w.vgpr_f(23, lane) == static_cast<float>(b) && w.vgpr[24][lane] == b);
    if (uint64_t{a} + d > 0xFFFFFFFFull) carry |= 1ull << lane;
  }
  CHECK(w.vcc == carry && w.sgpr[4] == static_cast<uint32_t>(carry) && w.sgpr[5] == static_cast<uint32_t>(carry >> 32));
  // float to integer conversions saturate; NaN is 0
  Wave s;
  s.set_vgpr_f(1, 0, 3.99f); s.set_vgpr_f(1, 1, -3.99f); s.set_vgpr_f(1, 2, 3e10f); s.set_vgpr_f(1, 3, -3e10f); s.vgpr[1][4] = 0x7FC00000u;
  Code d(alloc);
  d.vop1(Vop1::v_cvt_i32_f32, 2, vgpr(1));
  d.vop1(Vop1::v_cvt_u32_f32, 3, vgpr(1));
  d.sopp(Sopp::s_endpgm);
  CHECK(run(s, env, d) == Status::Done);
  CHECK(s.vgpr[2][0] == 3 && s.vgpr[2][1] == 0xFFFFFFFDu && s.vgpr[2][2] == 0x7FFFFFFFu && s.vgpr[2][3] == 0x80000000u && s.vgpr[2][4] == 0);
  CHECK(s.vgpr[3][0] == 3 && s.vgpr[3][1] == 0 && s.vgpr[3][2] == 0xFFFFFFFFu && s.vgpr[3][3] == 0 && s.vgpr[3][4] == 0);
  // v_cvt_pkrtz_f16_f32: two halves, rounded toward zero
  Wave h;
  h.set_vgpr_f(1, 0, 1.0f); h.set_vgpr_f(2, 0, -2.5f);
  h.set_vgpr_f(1, 1, 0.1f); h.set_vgpr_f(2, 1, 65520.0f);
  h.set_vgpr_f(1, 2, 1e-9f); h.set_vgpr_f(2, 2, 6e-5f);
  Code p(alloc);
  p.vop3(Vop3::v_cvt_pkrtz_f16_f32, 3, vgpr(1), vgpr(2), Src{});
  p.sopp(Sopp::s_endpgm);
  CHECK(run(h, env, p) == Status::Done);
  CHECK((h.vgpr[3][0] & 0xFFFF) == 0x3C00 && (h.vgpr[3][0] >> 16) == 0xC100);       // 1.0 and -2.5
  CHECK((h.vgpr[3][1] & 0xFFFF) == 0x2E66 && (h.vgpr[3][1] >> 16) == 0x7BFF);       // 0.1 rounded down; 65520 does not round to infinity (round to zero)
  CHECK((h.vgpr[3][2] & 0xFFFF) == 0x0000 && (h.vgpr[3][2] >> 16) == 0x03EE);       // a tiny value is 0; 6e-5 is a half denormal
}

static void test_compares_and_masks() {
  TestAllocator alloc;
  Env env;
  Wave w;
  // lane n compares a = kValues[n % 16] with b = NaN for lanes 16..31, else with 1.0
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    w.set_vgpr_f(1, lane, kValues[lane % kCount]);
    w.vgpr[2][lane] = (lane / 16) % 2 ? 0x7FC00000u : float_to_bits(1.0f);
  }
  const Vopc ops[] = {Vopc::v_cmp_f_f32, Vopc::v_cmp_lt_f32, Vopc::v_cmp_eq_f32, Vopc::v_cmp_le_f32, Vopc::v_cmp_gt_f32, Vopc::v_cmp_lg_f32, Vopc::v_cmp_ge_f32, Vopc::v_cmp_o_f32,
                      Vopc::v_cmp_u_f32, Vopc::v_cmp_nge_f32, Vopc::v_cmp_nlg_f32, Vopc::v_cmp_ngt_f32, Vopc::v_cmp_nle_f32, Vopc::v_cmp_neq_f32, Vopc::v_cmp_nlt_f32, Vopc::v_cmp_tru_f32};
  for (uint32_t k = 0; k < 16; ++k) {
    Code c(alloc);
    c.vopc(ops[k], vgpr(1), 2);
    c.sopp(Sopp::s_endpgm);
    Wave x = w;
    x.pc = 0; x.status = Status::Running; x.steps = 0;
    CHECK(run(x, env, c) == Status::Done);
    uint64_t want = 0;
    for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
      const float a = w.vgpr_f(1, lane), b = w.vgpr_f(2, lane);
      const bool nan = a != a || b != b;
      bool r = false;
      switch (k) {
        case 0: r = false; break;
        case 1: r = a < b; break;
        case 2: r = a == b; break;
        case 3: r = a <= b; break;
        case 4: r = a > b; break;
        case 5: r = !nan && a != b; break;
        case 6: r = a >= b; break;
        case 7: r = !nan; break;
        case 8: r = nan; break;
        case 9: r = !(a >= b); break;
        case 10: r = !(!nan && a != b); break;
        case 11: r = !(a > b); break;
        case 12: r = !(a <= b); break;
        case 13: r = !(a == b); break;
        case 14: r = !(a < b); break;
        default: r = true; break;
      }
      if (r) want |= 1ull << lane;
    }
    if (x.vcc != want) { printf("FAILED compare %u: got %016llx want %016llx\n", k, (unsigned long long)x.vcc, (unsigned long long)want); ++g_failed; }
  }
  // integer compares, signed and unsigned
  Wave i;
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) { i.vgpr[1][lane] = lane; i.vgpr[2][lane] = 32; }
  i.vgpr[1][0] = 0xFFFFFFFFu;
  Code c(alloc);
  c.vopc(Vopc::v_cmp_lt_i32, vgpr(1), 2);
  c.sop1(Sop1::s_mov_b64, 4, vcc());
  c.vopc(Vopc::v_cmp_lt_u32, vgpr(1), 2);
  c.sopp(Sopp::s_endpgm);
  CHECK(run(i, env, c) == Status::Done);
  CHECK(i.sgpr[4] == 0xFFFFFFFFu && i.sgpr[5] == 0 && static_cast<uint32_t>(i.vcc) == 0xFFFFFFFEu && (i.vcc >> 32) == 0);
  // the VOP3 compare writes an SGPR pair and respects exec; v_cndmask selects by a mask; if / else with the saveexec idiom
  Wave m;
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) { m.vgpr[1][lane] = lane; m.vgpr[2][lane] = 100 + lane; m.vgpr[3][lane] = 5; }
  m.exec = 0x00000000FFFFFFFFull;
  Code d(alloc);
  d.vop3_of(Vopc::v_cmp_lt_u32, 10, vgpr(1), imm_int(16));            // s[10:11] = lanes below 16 (within exec)
  d.vop3_of(Vop2::v_cndmask_b32, 4, vgpr(2), vgpr(3), sgpr(10));      // v4 = lane < 16 ? v3 : v2
  // if (lane < 8) v5 = 1 else v5 = 2
  d.vopc(Vopc::v_cmp_gt_u32, imm_int(8), 1);                          // 8 > v1
  d.sop1(Sop1::s_and_saveexec_b64, 12, vcc());                        // s[12:13] = exec; exec &= vcc
  const uint32_t skip_then = d.branch(Sopp::s_cbranch_execz);
  d.vop1(Vop1::v_mov_b32, 5, imm_int(1));
  d.patch_branch(skip_then, d.size());
  d.sop2(Sop2::s_andn2_b64, 14, sgpr(12), exec());                    // s[14:15] = saved exec & ~exec (the else lanes)
  d.sop1(Sop1::s_mov_b64, 126, sgpr(14));
  d.vop1(Vop1::v_mov_b32, 5, imm_int(2));
  d.sop1(Sop1::s_mov_b64, 126, sgpr(12));                             // restore
  d.sopp(Sopp::s_endpgm);
  CHECK(d.ok() && run(m, env, d) == Status::Done);
  CHECK(m.sgpr[10] == 0x0000FFFFu && m.sgpr[11] == 0 && m.exec == 0xFFFFFFFFull);
  for (uint32_t lane = 0; lane < 32; ++lane) CHECK(m.vgpr[4][lane] == (lane < 16 ? 5u : 100 + lane) && m.vgpr[5][lane] == (lane < 8 ? 1u : 2u));
  for (uint32_t lane = 32; lane < 64; ++lane) CHECK(m.vgpr[4][lane] == 0 && m.vgpr[5][lane] == 0);       // (lanes outside exec are untouched)
}

static void test_memory_and_exports() {
  TestAllocator alloc;
  Env env;
  // constants at 0x10000: s_load_dwordx4 through a 64 bit pointer, s_buffer_load through a descriptor; vertex data at 0x20000: buffer_load_format_xyzw by index
  for (uint32_t i = 0; i < 8; ++i) env.memory[0x10000 + i * 4] = 0x1000 + i;
  for (uint32_t v = 0; v < 4; ++v) {
    const float vert[4] = {1.0f + v, 2.0f + v, 3.0f + v, 4.0f + v};
    for (uint32_t i = 0; i < 4; ++i) env.memory[0x20000 + v * 16 + i] = 0, env.memory[0x20000 + v * 16 + i * 4] = float_to_bits(vert[i]);
  }
  Wave w;
  w.sgpr[0] = 0x10000; w.sgpr[1] = 0;                                                       // the pointer s[0:1]
  w.sgpr[4] = 0x10000; w.sgpr[5] = 0; w.sgpr[6] = 32; w.sgpr[7] = 0;                       // a buffer descriptor s[4:7]: base, stride 0, 32 bytes
  w.sgpr[8] = 0x20000; w.sgpr[9] = 16u << 16; w.sgpr[10] = 4; w.sgpr[11] = (7u << 12) | (14u << 15);       // vertex buffer: stride 16, 4 records, 32_32_32_32 float
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) w.vgpr[0][lane] = lane;                // the vertex index
  Code c(alloc);
  c.smem(Smem::s_load_dwordx4, 12, 0, 8);                       // s[12:15] = memory[0x10008 ..]
  c.smem(Smem::s_buffer_load_dword, 16, 4, 4);                  // s16 = memory[0x10004]
  c.smem(Smem::s_buffer_load_dword, 17, 4, 32);                 // out of range: 0
  c.waitcnt(15, 7, 0);
  c.mubuf(Mubuf::buffer_load_format_xyzw, 20, 0, 8, imm_int(0), Code::BufferMode{false, true, false, false, 0});
  c.mubuf(Mubuf::buffer_load_format_x, 24, 0, 8, imm_int(0), Code::BufferMode{false, true, false, false, 8});         // the third component of the vertex
  c.sopp(Sopp::s_endpgm);
  CHECK(c.ok() && run(w, env, c) == Status::Done);
  CHECK(w.sgpr[12] == 0x1002 && w.sgpr[13] == 0x1003 && w.sgpr[14] == 0x1004 && w.sgpr[15] == 0x1005 && w.sgpr[16] == 0x1001 && w.sgpr[17] == 0);
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    const uint32_t v = lane;
    if (v < 4) CHECK(w.vgpr_f(20, lane) == 1.0f + v && w.vgpr_f(21, lane) == 2.0f + v && w.vgpr_f(22, lane) == 3.0f + v && w.vgpr_f(23, lane) == 4.0f + v && w.vgpr_f(24, lane) == 3.0f + v);
    else CHECK(w.vgpr[20][lane] == 0 && w.vgpr[23][lane] == 0x3F800000u);       // out of range: 0, 0, 0, 1
  }
  // a stored dword and a load through a raw buffer with an offset register
  Wave s;
  s.sgpr[8] = 0x30000; s.sgpr[9] = 0; s.sgpr[10] = 0x1000; s.sgpr[11] = 0;
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) { s.vgpr[0][lane] = lane * 4; s.vgpr[1][lane] = 0xAA00 + lane; }
  Code d(alloc);
  d.mubuf(Mubuf::buffer_store_dword, 1, 0, 8, imm_int(0), Code::BufferMode{true, false, false, false, 0});
  d.mubuf(Mubuf::buffer_load_dword, 2, 0, 8, imm_int(0), Code::BufferMode{true, false, false, false, 0});
  d.sopp(Sopp::s_endpgm);
  CHECK(run(s, env, d) == Status::Done && s.vgpr[2][5] == 0xAA05 && env.memory[0x30000 + 4 * 7] == 0xAA07);
  // interpolation: attr 1 chan 2 for lane 3: p0 = 0.25 + 12, p10 = 2, p20 = 3 + 3
  Wave p;
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) { p.set_vgpr_f(0, lane, 0.5f); p.set_vgpr_f(1, lane, 0.25f); }
  Code e(alloc);
  e.vintrp(Vintrp::v_interp_p1_f32, 2, 0, 1, 2);
  e.vintrp(Vintrp::v_interp_p2_f32, 2, 1, 1, 2);
  e.vintrp(Vintrp::v_interp_mov_f32, 3, 2, 0, 0);
  e.sopp(Sopp::s_endpgm);
  CHECK(run(p, env, e) == Status::Done);
  CHECK(p.vgpr_f(2, 3) == (2.0f * 0.5f + 12.25f) + (3.0f + 3.0f) * 0.25f && p.vgpr_f(3, 5) == 0.25f);
  // an export with the exec mask, and a compressed one
  Wave x;
  x.exec = 0xF0F0ull;
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) for (uint32_t r = 0; r < 4; ++r) x.vgpr[r][lane] = lane * 10 + r;
  Code f(alloc);
  f.exp(kExpPos0, 15, 0, 1, 2, 3, false, false);
  f.exp(kExpParam0 + 1, 5, 3, 0, 1, 0, false, false);
  f.exp(kExpMrt0, 15, 0, 1, 0, 0, true, true, true);
  f.sopp(Sopp::s_endpgm);
  CHECK(run(x, env, f) == Status::Done && env.exports.size() == 3);
  CHECK(env.exports[0].target == kExpPos0 && env.exports[0].enable == 15 && env.exports[0].lanes == 0xF0F0ull && env.exports[0].values[4][2] == 4 * 10 + 2 && !env.exports[0].done);
  CHECK(env.exports[1].target == kExpParam0 + 1 && env.exports[1].enable == 5 && env.exports[1].values[4][0] == 43 && env.exports[1].values[4][1] == 0 && env.exports[1].values[4][2] == 41);
  CHECK(env.exports[2].compressed && env.exports[2].done && env.exports[2].values[4][0] == 40 && env.exports[2].values[4][1] == 41);
  // image sample: the environment sees the descriptors, coordinates and the opcode; dmask 5 writes x and z into consecutive registers
  Wave m;
  for (uint32_t i = 0; i < 8; ++i) m.sgpr[16 + i] = 0x100 + i;
  for (uint32_t i = 0; i < 4; ++i) m.sgpr[24 + i] = 0x200 + i;
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) { m.set_vgpr_f(4, lane, 0.5f + lane); m.set_vgpr_f(5, lane, 0.25f); }
  Code g(alloc);
  g.mimg(Mimg::image_sample, 8, 4, 16, 24, 5);
  g.sopp(Sopp::s_endpgm);
  CHECK(run(m, env, g) == Status::Done && env.samples == 64 && m.vgpr_f(8, 2) == 2.5f && m.vgpr_f(9, 2) == 256.0f);
}

static void test_failures() {
  TestAllocator alloc;
  Env env;
  { Wave w; Code c(alloc); const uint32_t word[1] = {0xFC000000u}; CHECK(w.run(ArrayView<const uint32_t>(word, 1), env) == Status::BadInstruction); }
  { Wave w; Code c(alloc); c.smem(Smem::s_load_dword, 0, 0, 0); c.sopp(Sopp::s_endpgm); w.sgpr[0] = 0x999000; CHECK(run(w, env, c) == Status::MemoryFault); }
  { Wave w; Code c(alloc); const uint32_t at = c.size(); c.patch_branch(c.branch(Sopp::s_branch), at); CHECK(run(w, env, c, 1000) == Status::StepLimit && w.steps == 1000); }
  { Wave w; Code c(alloc); c.sopp(Sopp::s_nop, 0); CHECK(run(w, env, c) == Status::BadBranch); }       // runs off the end
  { Wave w; Code c(alloc); c.sopp(Sopp::s_branch, -5); CHECK(run(w, env, c) == Status::BadBranch); }
  { Wave w; Code c(alloc); c.vop1(Vop1::v_mov_b32, 0, vgpr(1)); c.sopp(Sopp::s_endpgm); w.status = Status::Done; CHECK(run(w, env, c) == Status::Done && w.steps == 0); }
  { Wave w; Code c(alloc); c.sop1(Sop1::s_mov_b32, 0, Src{250, false, 0}); c.sopp(Sopp::s_endpgm); CHECK(run(w, env, c) == Status::BadInstruction); }      // an unknown source code
  { Wave w; w.reset(10); CHECK(w.exec == 0x3FFull); w.reset(64); CHECK(w.exec == ~0ull); }
}

// Machine code made by LLVM's AMDGPU back end for pixel shaders (tests/data/gcn/*.ll, scripts/gen_gcn_llvm_shaders.py) run on the emulator and compared with the same computation on the
// CPU: structured control flow with the saveexec idiom, a divergent loop, the exact square root sequence, float classes, comparisons into SGPR pairs, selects, conversions, interpolation
// and exports. A compiler other than ours wrote the instruction sequences, which is the point.
struct LlvmShader {
  const char* name;
  std::vector<uint32_t> words;
};
static const LlvmShader kLlvmShaders[] = {
#include "GcnLlvmShaders.inc"
};

static const std::vector<uint32_t>& llvm_shader(const char* name) {
  for (const LlvmShader& s : kLlvmShaders)
    if (strcmp(s.name, name) == 0) return s.words;
  static const std::vector<uint32_t> none;
  return none;
}

// the interpolation of Env: p0 + attr * 10 + chan, p10, p20 + lane
static float interp_value(float i, float j, uint32_t attr, uint32_t chan, uint32_t lane, const Env& env) {
  const float p0 = env.p0 + static_cast<float>(attr * 10 + chan);
  const float p20 = env.p20 + static_cast<float>(lane);
  float v = env.p10 * i;
  v = v + p0;
  float w = p20 * j;
  return w + v;
}

static Status run_llvm(Wave& w, Env& env, const std::vector<uint32_t>& code) {
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    w.set_vgpr_f(0, lane, static_cast<float>(lane) / 64.0f);
    w.set_vgpr_f(1, lane, static_cast<float>(63 - lane) / 128.0f);
  }
  w.sgpr[0] = 0;
  return w.run(ArrayView<const uint32_t>(code.data(), code.size()), env, 100000);
}

static bool close_f(float got, float want, float ulps) {
  if (got == want || (got != got && want != want)) return true;
  const float scale = fabsf(want) > 1e-30f ? fabsf(want) : 1e-30f;
  return fabsf(got - want) <= ulps * 1.2e-7f * scale;
}

static void test_llvm_interp_math() {
  const std::vector<uint32_t>& code = llvm_shader("interp_math");
  CHECK(!code.empty());
  Env env;
  env.p0 = -2.0f; env.p10 = 6.0f; env.p20 = -40.0f;       // values on both sides of the shaders' thresholds
  Wave w;
  CHECK(run_llvm(w, env, code) == Status::Done && env.exports.size() == 1);
  if (env.exports.empty()) return;
  const Env::Export& e = env.exports[0];
  CHECK(e.target == kExpMrt0 && e.enable == 15 && e.done && e.lanes == ~0ull);
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    const float i = static_cast<float>(lane) / 64.0f, j = static_cast<float>(63 - lane) / 128.0f;
    const float a = interp_value(i, j, 0, 0, lane, env), b = interp_value(i, j, 0, 1, lane, env);
    const float c = a * b, d = fmaf(a, b, 0.5f), ex = sqrtf(d), f = fabsf(c), g = fmaxf(f, 0.25f), h = floorf(c);
    const float sel = a < b ? ex : g;
    const float want[4] = {sel, h, c, 1.0f};
    for (uint32_t k = 0; k < 4; ++k) {
      const float got = bits_to_float_bits(e.values[lane][k]);
      if (!close_f(got, want[k], 1.0f)) { printf("FAILED interp_math lane %u comp %u: got %.9g want %.9g (a %g b %g)\n", lane, k, got, want[k], a, b); ++g_failed; return; }
    }
  }
}

static void test_llvm_flow() {
  const std::vector<uint32_t>& code = llvm_shader("flow");
  CHECK(!code.empty());
  Env env;
  env.p0 = -2.0f; env.p10 = 6.0f; env.p20 = -40.0f;       // values on both sides of the shaders' thresholds
  Wave w;
  CHECK(run_llvm(w, env, code) == Status::Done && env.exports.size() == 1);
  if (env.exports.empty()) return;
  const Env::Export& e = env.exports[0];
  CHECK(e.lanes == ~0ull && w.exec == ~0ull);
  uint32_t loops_min = 1000, loops_max = 0, then_lanes = 0;
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    const float i = static_cast<float>(lane) / 64.0f, j = static_cast<float>(63 - lane) / 128.0f;
    const float a = interp_value(i, j, 0, 0, lane, env), b = interp_value(i, j, 0, 1, lane, env);
    const uint32_t n = b <= 0 ? 0 : static_cast<uint32_t>(b);
    float acc = a > 0.5f ? a * 3.0f : 1.0f - a;
    then_lanes += a > 0.5f;
    uint32_t k = 0;
    do { acc = acc + 0.125f; ++k; } while (k < n);
    loops_min = k < loops_min ? k : loops_min;
    loops_max = k > loops_max ? k : loops_max;
    const float want[4] = {acc, a, b, 1.0f};
    for (uint32_t c = 0; c < 4; ++c) {
      const float got = bits_to_float_bits(e.values[lane][c]);
      if (got != want[c]) { printf("FAILED flow lane %u comp %u: got %.9g want %.9g\n", lane, c, got, want[c]); ++g_failed; return; }
    }
  }
  CHECK(loops_max > loops_min && then_lanes > 0 && then_lanes < kWaveSize);       // (the test really diverges: in the branch and in the loop)
}

// float32 to float16 rounding toward zero, written differently from the emulator's: by scaling
static uint16_t half_rtz_reference(float x) {
  const uint16_t sign = signbit(x) ? 0x8000 : 0;
  const float m = fabsf(x);
  if (m != m) return 0x7E00;
  if (m >= 65520.0f) return sign | 0x7BFF;
  if (m < 6.103515625e-5f) return sign | static_cast<uint16_t>(static_cast<uint32_t>(m * 16777216.0f));        // denormal halves: multiples of 2^-24
  int e;
  const float frac = frexpf(m, &e);                                    // m = frac * 2^e, frac in [0.5, 1)
  const uint32_t mant = static_cast<uint32_t>((frac * 2.0f - 1.0f) * 1024.0f);
  return sign | static_cast<uint16_t>(((e - 1 + 15) << 10) | mant);
}

static void test_llvm_multi_export() {
  const std::vector<uint32_t>& code = llvm_shader("multi_export");
  CHECK(!code.empty());
  Env env;
  env.p0 = -2.0f; env.p10 = 6.0f; env.p20 = -40.0f;       // values on both sides of the shaders' thresholds
  Wave w;
  CHECK(run_llvm(w, env, code) == Status::Done && env.exports.size() == 3);
  if (env.exports.size() != 3) return;
  CHECK(env.exports[0].target == kExpPos0 && env.exports[1].target == kExpParam0 && env.exports[2].target == kExpMrt0 && env.exports[2].compressed && env.exports[2].done && env.exports[2].enable == 15);
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    const float i = static_cast<float>(lane) / 64.0f, j = static_cast<float>(63 - lane) / 128.0f;
    const float r = interp_value(i, j, 0, 0, lane, env), g = interp_value(i, j, 0, 1, lane, env), b = interp_value(i, j, 0, 2, lane, env);
    const uint32_t u = b <= 0 ? 0 : static_cast<uint32_t>(b), x = (u >> 2) ^ u;       // (the hardware conversion saturates)
    const float f = static_cast<float>(x), mn = fminf(r, g), cs = b < 0.0f ? -mn : mn;
    const float pos[4] = {r, g, b, 1.0f}, par[4] = {f, cs, mn, 0.0f};
    for (uint32_t k = 0; k < 4; ++k) {
      if (bits_to_float_bits(env.exports[0].values[lane][k]) != pos[k] || bits_to_float_bits(env.exports[1].values[lane][k]) != par[k]) {
        printf("FAILED multi_export lane %u comp %u: pos got %g want %g, param got %g want %g (u %u)\n", lane, k, bits_to_float_bits(env.exports[0].values[lane][k]), pos[k], bits_to_float_bits(env.exports[1].values[lane][k]), par[k], u);
        ++g_failed;
        return;
      }
    }
    const uint32_t p0 = half_rtz_reference(r) | (static_cast<uint32_t>(half_rtz_reference(g)) << 16), p1 = half_rtz_reference(b) | (static_cast<uint32_t>(half_rtz_reference(cs)) << 16);
    if (env.exports[2].values[lane][0] != p0 || env.exports[2].values[lane][1] != p1) { printf("FAILED multi_export packed lane %u: got %08X %08X want %08X %08X\n", lane, env.exports[2].values[lane][0], env.exports[2].values[lane][1], p0, p1); ++g_failed; return; }
  }
}

int main() {
  test_scalar();
  test_vector_float();
  test_integer_and_conversions();
  test_compares_and_masks();
  test_memory_and_exports();
  test_failures();
  test_llvm_interp_math();
  test_llvm_flow();
  test_llvm_multi_export();
  printf(g_failed ? "GcnEmuTest: %d FAILED\n" : "GcnEmuTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
