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

// Host-side tests of src/Windows/d3d9/sm_to_dxbc.h, which makes Shader Model 4 of a Direct3D 9 shader for d3d9.dll:
//   - shaders a compiler made (tests/data/d3d9, scripts/gen-d3d9-test-shaders.sh) are translated, the kernel's compiler takes the result, and - where
//     the kernel's own compiler for Direct3D 9 bytecode takes the shader too - both programs give the same numbers for the same inputs;
//   - shaders written here token by token, one for each kind of instruction, give the numbers worked out here - pixel shaders 1.x among them, with
//     their modifiers, factors and texture registers;
//   - what is refused is refused, and shaders that end early or have a bit turned (every length, bits all over) are taken or refused without a
//     wrong read (the test runs under ASan and UBSan), and what is taken of them the kernel's compiler takes or refuses the same way.
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <functional>
#include <string>
#include <vector>
#include "Kernel/Dxgkrnl/D3d9Shader.hpp"
#include "../src/Windows/d3d9/sm_to_dxbc.h"

using namespace dxgk;

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

struct HostAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    void* p = nullptr;
    if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size)) return nullptr;
    ++live;
    return p;
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

using Tokens = std::vector<uint32_t>;

// (the two big structures of a translation, once)
SmShader g_shader;
SmContext g_context;

Tokens read_tokens(const char* name) {
  const std::string path = std::string("tests/data/d3d9/") + name;
  FILE* f = fopen(path.c_str(), "rb");
  Tokens out;
  if (!f) return out;
  uint32_t word;
  while (fread(&word, 4, 1, f) == 1) out.push_back(word);
  fclose(f);
  return out;
}

UniquePointer<D3d9Program> compile_translated(HostAllocator& allocator, const SmShader& shader, NtStatus* status_out = nullptr) {
  UniquePointer<D3d9Program> p;
  const NtStatus s = D3d9Program::compile_dxbc(allocator, ConstByteView(shader.dxbc, shader.dxbc_size), &p);
  if (status_out) *status_out = s;
  return p;
}

bool near_value(float a, float b) {
  if (isnan(a) || isnan(b)) return isnan(a) == isnan(b);
  const float scale = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
  return fabsf(a - b) <= 1e-4f * (scale > 1.0f ? scale : 1.0f);
}

// ---- the token streams written here --------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kTemp = 0, kInput = 1, kConst = 2, kAddr = 3, kRastOut = 4, kAttrOut = 5, kTexOut = 6, kColorOut = 8, kSampler = 10, kConstBool = 14;
constexpr uint32_t kXyzw = 0xE4;

uint32_t reg(uint32_t type, uint32_t number) { return 0x80000000u | ((type & 7) << 28) | ((type & 0x18) << 8) | number; }
uint32_t dst(uint32_t type, uint32_t number, uint32_t mask = 0xF, bool saturate = false) { return reg(type, number) | (mask << 16) | (saturate ? 1u << 20 : 0u); }
uint32_t src(uint32_t type, uint32_t number, uint32_t swizzle = kXyzw, uint32_t modifier = 0) { return reg(type, number) | (swizzle << 16) | (modifier << 24); }
constexpr uint32_t swz(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return x | y << 2 | z << 4 | w << 6; }
uint32_t float_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

struct Asm {
  Tokens t;
  bool vertex;
  bool lengths;      // (an instruction says how many parameters it has from Shader Model 2 on)
  Asm(bool vertex_stage, uint32_t major, uint32_t minor) : vertex(vertex_stage), lengths(major >= 2) {
    t.push_back((vertex ? 0xFFFE0000u : 0xFFFF0000u) | (major << 8) | minor);
    if (vertex) {      // v0 the position, v1 a colour, v2 texture coordinates
      dcl(0, 0, reg(kInput, 0));
      dcl(10, 0, reg(kInput, 1));
      dcl(5, 0, reg(kInput, 2));
    }
  }
  Asm& dcl(uint32_t usage, uint32_t index, uint32_t register_token) {
    t.insert(t.end(), {31u | (2u << 24), 0x80000000u | usage | (index << 16), register_token | (0xFu << 16)});
    return *this;
  }
  Asm& def(uint32_t number, float x, float y, float z, float w) {
    t.insert(t.end(), {81u | (5u << 24), dst(kConst, number), float_bits(x), float_bits(y), float_bits(z), float_bits(w)});
    return *this;
  }
  Asm& ins(uint32_t op, std::initializer_list<uint32_t> parameters, uint32_t flags = 0) {
    t.push_back(op | (flags << 16) | (lengths ? static_cast<uint32_t>(parameters.size()) << 24 : 0u));
    t.insert(t.end(), parameters);
    return *this;
  }
  Tokens end() {
    Tokens out = t;
    out.push_back(0x0000FFFF);
    return out;
  }
};

// What the tests give a shader: the inputs v0 .. v2 (a pixel shader: the two colours and two sets of texture coordinates), and the constants.
const float kIn[4][4] = {{0.25f, -0.5f, 0.75f, 1.0f}, {0.3f, -0.6f, 2.4f, 0.5f}, {0.125f, 0.875f, 0.0f, 1.0f}, {1.5f, 0.2f, -0.3f, 0.9f}};
const float kMore[2][4] = {{0.5f, 0.25f, -1.0f, 1.0f}, {2.0f, 0.0f, 0.5f, 1.0f}};
float constant_value(unsigned reg_number, unsigned component) { return 0.1f * static_cast<float>(reg_number + 1) + 0.37f * static_cast<float>(component) - 0.5f; }

struct Result { float position[4]; float colour[4]; bool alive; bool ok; };

// Translates, compiles and runs: a vertex shader's position and COLOR0, a pixel shader's colour. `bools`: the device's bool constants (bit n: b<n>).
Result run_tokens(HostAllocator& allocator, const Tokens& tokens, uint32_t bools = 0, int* translate_status = nullptr) {
  Result r = {};
  const int status = sm_translate(tokens.data(), tokens.size(), &g_shader, &g_context);
  if (translate_status) *translate_status = status;
  if (status != SM_OK) return r;
  UniquePointer<D3d9Program> program = compile_translated(allocator, g_shader);
  if (!program) return r;
  static uint8_t constants[4096];
  memset(constants, 0, sizeof constants);
  for (unsigned n = 0; n < g_shader.float_registers; ++n)
    for (unsigned k = 0; k < 4; ++k) { const float v = constant_value(n, k); memcpy(constants + n * 16 + k * 4, &v, 4); }
  if (g_shader.bools_used)
    for (unsigned n = 0; n < 16; ++n) { const uint32_t v = (bools >> n & 1) ? 0xFFFFFFFFu : 0u; memcpy(constants + (g_shader.float_registers + n) * 16, &v, 4); }
  graphics::Vec4 in[16] = {};
  for (unsigned i = 0; i < 4; ++i) in[i] = {kIn[i][0], kIn[i][1], kIn[i][2], kIn[i][3]};
  if (!g_shader.vertex) {      // (a pixel shader: texture coordinates 2 and 3 as well)
    in[4] = {kMore[0][0], kMore[0][1], kMore[0][2], kMore[0][3]};
    in[5] = {kMore[1][0], kMore[1][1], kMore[1][2], kMore[1][3]};
  }
  if (g_shader.vertex) {
    graphics::ShaderIo io = {};
    program->run_vertex(in, constants, &io);
    const float p[4] = {io.position.x, io.position.y, io.position.z, io.position.w}, c[4] = {io.varying[0].x, io.varying[0].y, io.varying[0].z, io.varying[0].w};
    memcpy(r.position, p, sizeof p);
    memcpy(r.colour, c, sizeof c);
    r.alive = true;
  } else {
    graphics::Vec4 colours[graphics::kMaxColorTargets] = {};
    r.alive = program->run_fragment(in, constants, nullptr, &colours[0]);
    const float c[4] = {colours[0].x, colours[0].y, colours[0].z, colours[0].w};
    memcpy(r.colour, c, sizeof c);
  }
  r.ok = true;
  return r;
}

// A vertex shader of Shader Model 2 whose body leaves its result in r0: the position is v0, the result goes out as COLOR0.
Result run_vertex_body(HostAllocator& allocator, const std::function<void(Asm&)>& body, uint32_t bools = 0) {
  Asm a(true, 2, 0);
  body(a);
  a.ins(1, {dst(kRastOut, 0), src(kInput, 0)});
  a.ins(1, {dst(kAttrOut, 0), src(kTemp, 0)});
  return run_tokens(allocator, a.end(), bools);
}
bool colour_is(const Result& r, float x, float y, float z, float w) {
  const float want[4] = {x, y, z, w};
  bool same = r.ok;
  for (unsigned k = 0; k < 4; ++k) same = same && near_value(r.colour[k], want[k]);
  if (!same) printf("  got %g %g %g %g, wanted %g %g %g %g%s\n", r.colour[0], r.colour[1], r.colour[2], r.colour[3], x, y, z, w, r.ok ? "" : " (not translated or compiled)");
  return same;
}
float cv(unsigned n, unsigned k) { return constant_value(n, k); }

void test_instructions(HostAllocator& allocator) {
  const float* v1 = kIn[1];
  // a constant the shader defines; a swizzle and a negation of it
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.def(5, 1, 2, 3, 4).ins(1, {dst(kTemp, 0), src(kConst, 5, swz(3, 2, 1, 0), 1)}); }), -4, -3, -2, -1));
  // add, sub, mul, mad; a write mask; saturate
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.ins(3, {dst(kTemp, 0), src(kInput, 1), src(kConst, 0)}); }), v1[0] - cv(0, 0), v1[1] - cv(0, 1), v1[2] - cv(0, 2), v1[3] - cv(0, 3)));
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) {
          a.ins(1, {dst(kTemp, 0), src(kConst, 1)});
          a.ins(4, {dst(kTemp, 0, 0x5), src(kInput, 1), src(kConst, 0), src(kConst, 2, swz(3, 3, 3, 3), 1)});
        }), v1[0] * cv(0, 0) - cv(2, 3), cv(1, 1), v1[2] * cv(0, 2) - cv(2, 3), cv(1, 3)));
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.ins(5, {dst(kTemp, 0, 0xF, true), src(kInput, 1), src(kInput, 1)}); }), 0.09f, 0.36f, 1.0f, 0.25f));
  // rcp, rsq (of the absolute value), exp and log to the base 2, frc, abs, min, max
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) {
          a.ins(6, {dst(kTemp, 0, 0x1), src(kInput, 1, swz(3, 3, 3, 3))});
          a.ins(7, {dst(kTemp, 0, 0x2), src(kInput, 1, swz(1, 1, 1, 1))});
          a.ins(14, {dst(kTemp, 0, 0x4), src(kInput, 1, swz(0, 0, 0, 0))});
          a.ins(15, {dst(kTemp, 0, 0x8), src(kInput, 1, swz(2, 2, 2, 2))});
        }), 2.0f, 1.0f / sqrtf(0.6f), powf(2.0f, 0.3f), log2f(2.4f)));
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) {
          a.ins(19, {dst(kTemp, 0, 0x1), src(kInput, 1, swz(2, 2, 2, 2))});
          a.ins(35, {dst(kTemp, 0, 0x2), src(kInput, 1)});
          a.ins(10, {dst(kTemp, 0, 0x4), src(kInput, 1), src(kInput, 1, swz(0, 0, 0, 0))});
          a.ins(11, {dst(kTemp, 0, 0x8), src(kInput, 1), src(kInput, 1, swz(2, 2, 2, 2))});
        }), 2.4f - 2.0f, 0.6f, 0.3f, 2.4f));
  // slt and sge give 1 or 0; sgn gives -1, 0 or 1
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) {
          a.def(9, 0, 0, 0, 0);
          a.ins(12, {dst(kTemp, 0, 0x3), src(kInput, 1), src(kConst, 9)});
          a.ins(13, {dst(kTemp, 0, 0xC), src(kInput, 1), src(kInput, 1, swz(3, 3, 3, 3))});
        }), 0, 1, 1, 1));
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.def(9, 0.3f, 0, 0, 0).ins(3, {dst(kTemp, 1), src(kInput, 1), src(kConst, 9, 0)}).ins(34, {dst(kTemp, 0), src(kTemp, 1), src(kTemp, 2), src(kTemp, 3)}); }), 0, -1, 1, 1));
  // dot products; dst; lit
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) {
          a.ins(8, {dst(kTemp, 0, 0x3), src(kInput, 1), src(kInput, 0)});
          a.ins(9, {dst(kTemp, 0, 0xC), src(kInput, 1), src(kInput, 0)});
        }), 0.3f * 0.25f + 0.6f * 0.5f + 2.4f * 0.75f, 2.175f, 2.175f + 0.5f, 2.675f));
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.ins(17, {dst(kTemp, 0), src(kInput, 1), src(kInput, 0)}); }), 1, -0.6f * -0.5f, 2.4f, 1.0f));
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.ins(16, {dst(kTemp, 0), src(kInput, 0, swz(0, 2, 2, 3))}); }), 1, 0.25f, powf(0.75f, 1.0f), 1));
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.ins(16, {dst(kTemp, 0), src(kInput, 1)}); }), 1, 0.3f, 0, 1));      // (a.y is not above 0: no highlight)
  // cross product, normalize, lrp, pow
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.ins(1, {dst(kTemp, 0), src(kInput, 0)}).ins(33, {dst(kTemp, 0, 0x7), src(kInput, 1), src(kInput, 0)}); }),
                  -0.6f * 0.75f - 2.4f * -0.5f, 2.4f * 0.25f - 0.3f * 0.75f, 0.3f * -0.5f - -0.6f * 0.25f, 1.0f));
  {
    const float length = sqrtf(0.25f * 0.25f + 0.5f * 0.5f + 0.75f * 0.75f);
    CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.ins(36, {dst(kTemp, 0), src(kInput, 0)}); }), 0.25f / length, -0.5f / length, 0.75f / length, 1.0f / length));
  }
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.ins(18, {dst(kTemp, 0), src(kInput, 2), src(kInput, 0), src(kInput, 1)}); }),
                  0.125f * (0.25f - 0.3f) + 0.3f, 0.875f * (-0.5f + 0.6f) - 0.6f, 2.4f, 1.0f));
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.ins(32, {dst(kTemp, 0), src(kInput, 1, swz(1, 1, 1, 1)), src(kInput, 1, swz(2, 2, 2, 2))}); }),
                  powf(0.6f, 2.4f), powf(0.6f, 2.4f), powf(0.6f, 2.4f), powf(0.6f, 2.4f)));
  // sincos: x the cosine, y the sine (Shader Model 2 gives it two constants that are not needed)
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) {
          a.ins(1, {dst(kTemp, 0), src(kInput, 0)});
          a.ins(37, {dst(kTemp, 0, 0x3), src(kInput, 1, swz(0, 0, 0, 0)), src(kConst, 0), src(kConst, 1)});
        }), cosf(0.3f), sinf(0.3f), 0.75f, 1.0f));
  // a matrix of four rows of constants; one of three by three
  {
    float want[4];
    for (unsigned row = 0; row < 4; ++row) want[row] = 0.25f * cv(2 + row, 0) - 0.5f * cv(2 + row, 1) + 0.75f * cv(2 + row, 2) + cv(2 + row, 3);
    CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.ins(20, {dst(kTemp, 0), src(kInput, 0), src(kConst, 2)}); }), want[0], want[1], want[2], want[3]));
    for (unsigned row = 0; row < 3; ++row) want[row] = 0.25f * cv(2 + row, 0) - 0.5f * cv(2 + row, 1) + 0.75f * cv(2 + row, 2);
    CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.ins(1, {dst(kTemp, 0), src(kInput, 0)}).ins(23, {dst(kTemp, 0, 0x7), src(kInput, 0), src(kConst, 2)}); }),
                    want[0], want[1], want[2], 1.0f));
  }
  // a constant far up the block, and constants addressed through a0 (2.4 is nearest 2)
  CHECK(colour_is(run_vertex_body(allocator, [](Asm& a) { a.ins(1, {dst(kTemp, 0), src(kConst, 100)}); }), cv(100, 0), cv(100, 1), cv(100, 2), cv(100, 3)));
  {
    const Result r = run_vertex_body(allocator, [](Asm& a) {
      a.ins(46, {dst(kAddr, 0, 0x1), src(kInput, 1, swz(2, 2, 2, 2))});
      a.t.push_back(1u | (3u << 24));      // mov r0, c[a0.x + 10]
      a.t.push_back(dst(kTemp, 0));
      a.t.push_back(src(kConst, 10) | 0x2000u);
      a.t.push_back(src(kAddr, 0, swz(0, 0, 0, 0)));
    });
    CHECK(colour_is(r, cv(12, 0), cv(12, 1), cv(12, 2), cv(12, 3)));
    CHECK(g_shader.relative && g_shader.float_registers == SM_RELATIVE_REGISTERS);
  }
  // if on a bool the shader defines, on one of the device's, and on a comparison
  auto branch = [](uint32_t which) {
    return [which](Asm& a) {
      if (which == 0) a.t.insert(a.t.end(), {47u | (2u << 24), dst(kConstBool, 3), 1u});      // defb b3, true
      if (which <= 1) a.ins(40, {src(kConstBool, which == 0 ? 3 : 5)});
      else a.ins(41, {src(kInput, 1, swz(0, 0, 0, 0)), src(kInput, 1, swz(3, 3, 3, 3))}, which == 2 ? 4 /* < */ : 1 /* > */);
      a.ins(1, {dst(kTemp, 0), src(kInput, 0)});
      a.ins(42, {});
      a.ins(1, {dst(kTemp, 0), src(kInput, 1)});
      a.ins(43, {});
    };
  };
  CHECK(colour_is(run_vertex_body(allocator, branch(0)), 0.25f, -0.5f, 0.75f, 1.0f));
  CHECK(colour_is(run_vertex_body(allocator, branch(1), 1u << 5), 0.25f, -0.5f, 0.75f, 1.0f));
  CHECK(g_shader.bools_used && g_shader.constant_registers == g_shader.float_registers + SM_BOOL_REGISTERS);
  CHECK(colour_is(run_vertex_body(allocator, branch(1), ~(1u << 5)), 0.3f, -0.6f, 2.4f, 0.5f));
  CHECK(colour_is(run_vertex_body(allocator, branch(2)), 0.25f, -0.5f, 0.75f, 1.0f));      // 0.3 < 0.5
  CHECK(colour_is(run_vertex_body(allocator, branch(3)), 0.3f, -0.6f, 2.4f, 0.5f));        // 0.3 > 0.5 is not so

  // a pixel shader: its inputs by what they are (v0, v1 the colours, t0, t1 the texture coordinates), cmp, dp2add, texkill
  auto pixel = [&](const std::function<void(Asm&)>& body) {
    Asm a(false, 2, 0);
    body(a);
    a.ins(1, {dst(kColorOut, 0), src(kTemp, 0)});
    return run_tokens(allocator, a.end());
  };
  CHECK(colour_is(pixel([](Asm& a) { a.ins(2, {dst(kTemp, 0), src(kInput, 0), src(kAddr, 1)}); }), 0.25f + 1.5f, -0.5f + 0.2f, 0.75f - 0.3f, 1.0f + 0.9f));
  CHECK(colour_is(pixel([](Asm& a) { a.ins(88, {dst(kTemp, 0), src(kInput, 0), src(kInput, 1), src(kAddr, 0)}); }), 0.3f, 0.875f, 2.4f, 0.5f));
  CHECK(colour_is(pixel([](Asm& a) { a.ins(90, {dst(kTemp, 0), src(kInput, 0), src(kInput, 1), src(kAddr, 0, swz(1, 1, 1, 1))}); }),
                  0.25f * 0.3f + 0.5f * 0.6f + 0.875f, 1.25f, 1.25f, 1.25f));
  {
    Result r = pixel([](Asm& a) { a.ins(1, {dst(kTemp, 0), src(kInput, 0)}).ins(65, {dst(kTemp, 0)}); });      // (r0.y is below 0)
    CHECK(r.ok && !r.alive);
    r = pixel([](Asm& a) { a.ins(1, {dst(kTemp, 0), src(kAddr, 0)}).ins(65, {dst(kTemp, 0)}); });
    CHECK(r.ok && r.alive);
  }
  // a vertex shader of Shader Model 3: outputs by their declarations
  {
    Asm a(true, 3, 0);
    a.dcl(0, 0, reg(kTexOut, 0)).dcl(10, 0, reg(kTexOut, 1)).dcl(5, 3, reg(kTexOut, 2));
    a.ins(1, {dst(kTexOut, 0), src(kInput, 0)}).ins(1, {dst(kTexOut, 1), src(kInput, 2)}).ins(1, {dst(kTexOut, 2), src(kInput, 1)});
    const Result r = run_tokens(allocator, a.end());
    CHECK(colour_is(r, 0.125f, 0.875f, 0.0f, 1.0f) && near_value(r.position[0], 0.25f) && near_value(r.position[3], 1.0f));
    CHECK(g_shader.varying_mask == ((1u << SM_SLOT_POSITION) | (1u << SM_SLOT_COLOR) | (1u << (SM_SLOT_TEXCOORD + 3))));
    CHECK(g_shader.input_count == 3 && g_shader.inputs[1].usage == 10 && g_shader.inputs[1].reg == 1 && g_shader.inputs[2].usage == 5);
  }
}

// Pixel shaders 1.x: v0, v1 the colours (kIn[0], kIn[1]); the texture coordinates 0 .. 3 are kIn[2], kIn[3], kMore[0], kMore[1].
void test_pixel_shaders_1(HostAllocator& allocator) {
  const float *v0 = kIn[0], *v1 = kIn[1];
  auto pixel = [&](uint32_t minor, const std::function<void(Asm&)>& body) {
    Asm a(false, 1, minor);
    body(a);
    return run_tokens(allocator, a.end());
  };
  auto shifted = [](uint32_t type, uint32_t number, uint32_t shift, bool saturate = false) { return dst(type, number, 0xF, saturate) | (shift << 24); };
  // the colour is what r0 holds at the end
  CHECK(colour_is(pixel(1, [](Asm& a) { a.ins(1, {dst(kTemp, 0), src(kInput, 1)}); }), v1[0], v1[1], v1[2], v1[3]));
  CHECK(colour_is(pixel(1, [](Asm& a) { a.ins(2, {dst(kTemp, 1), src(kInput, 0), src(kConst, 2)}).ins(5, {dst(kTemp, 0), src(kTemp, 1), src(kInput, 1)}); }),
                  (v0[0] + cv(2, 0)) * v1[0], (v0[1] + cv(2, 1)) * v1[1], (v0[2] + cv(2, 2)) * v1[2], (v0[3] + cv(2, 3)) * v1[3]));
  // the modifiers of a source: _bias, _bx2, 1 - x, _x2, and the negated _bx2
  CHECK(colour_is(pixel(1, [](Asm& a) { a.ins(1, {dst(kTemp, 0), src(kInput, 0, kXyzw, 2)}); }), v0[0] - 0.5f, v0[1] - 0.5f, v0[2] - 0.5f, v0[3] - 0.5f));
  CHECK(colour_is(pixel(1, [](Asm& a) { a.ins(1, {dst(kTemp, 0), src(kInput, 0, kXyzw, 4)}); }), 2 * v0[0] - 1, 2 * v0[1] - 1, 2 * v0[2] - 1, 2 * v0[3] - 1));
  CHECK(colour_is(pixel(1, [](Asm& a) { a.ins(1, {dst(kTemp, 0), src(kInput, 0, kXyzw, 6)}); }), 1 - v0[0], 1 - v0[1], 1 - v0[2], 1 - v0[3]));
  CHECK(colour_is(pixel(4, [](Asm& a) { a.ins(1, {dst(kTemp, 0), src(kInput, 0, swz(2, 2, 2, 3), 7)}); }), 2 * v0[2], 2 * v0[2], 2 * v0[2], 2 * v0[3]));
  CHECK(colour_is(pixel(1, [](Asm& a) { a.ins(2, {dst(kTemp, 0), src(kInput, 0, kXyzw, 5), src(kInput, 1, kXyzw, 4)}); }),
                  -(2 * v0[0] - 1) + 2 * v1[0] - 1, -(2 * v0[1] - 1) + 2 * v1[1] - 1, -(2 * v0[2] - 1) + 2 * v1[2] - 1, -(2 * v0[3] - 1) + 2 * v1[3] - 1));
  // the factor of a destination, and saturation after it
  CHECK(colour_is(pixel(1, [&](Asm& a) { a.ins(2, {shifted(kTemp, 0, 1), src(kInput, 0), src(kInput, 1)}); }), 2 * (v0[0] + v1[0]), 2 * (v0[1] + v1[1]), 2 * (v0[2] + v1[2]), 2 * (v0[3] + v1[3])));
  CHECK(colour_is(pixel(1, [&](Asm& a) { a.ins(1, {shifted(kTemp, 0, 15), src(kInput, 1)}); }), v1[0] / 2, v1[1] / 2, v1[2] / 2, v1[3] / 2));
  CHECK(colour_is(pixel(1, [&](Asm& a) { a.ins(1, {shifted(kTemp, 0, 2, true), src(kInput, 0)}); }), 1, 0, 1, 1));      // (4 x, kept between 0 and 1)
  // lrp, mad, dp3 with a write mask, cnd on r0.a, cmp
  CHECK(colour_is(pixel(1, [](Asm& a) { a.ins(18, {dst(kTemp, 0), src(kInput, 0, swz(3, 3, 3, 3), 2), src(kInput, 0), src(kInput, 1)}); }),
                  0.5f * (v0[0] - v1[0]) + v1[0], 0.5f * (v0[1] - v1[1]) + v1[1], 0.5f * (v0[2] - v1[2]) + v1[2], 0.5f * (v0[3] - v1[3]) + v1[3]));
  {
    const float dot = v0[0] * v1[0] + v0[1] * v1[1] + v0[2] * v1[2];
    CHECK(colour_is(pixel(1, [](Asm& a) { a.ins(1, {dst(kTemp, 0), src(kInput, 0)}).ins(8, {dst(kTemp, 0, 0x7), src(kInput, 0), src(kInput, 1)}); }), dot, dot, dot, v0[3]));
  }
  CHECK(colour_is(pixel(1, [](Asm& a) {      // (r0.a is 1: the first of the two)
          a.ins(1, {dst(kTemp, 0), src(kInput, 0)}).ins(80, {dst(kTemp, 0), src(kTemp, 0, swz(3, 3, 3, 3)), src(kInput, 1), src(kConst, 1)});
        }), v1[0], v1[1], v1[2], v1[3]));
  CHECK(colour_is(pixel(1, [](Asm& a) {      // (r0.a is 0.5, which is not above it: the second)
          a.ins(1, {dst(kTemp, 0), src(kInput, 1)}).ins(80, {dst(kTemp, 0), src(kTemp, 0, swz(3, 3, 3, 3)), src(kInput, 0), src(kConst, 1)});
        }), cv(1, 0), cv(1, 1), cv(1, 2), cv(1, 3)));
  CHECK(colour_is(pixel(2, [](Asm& a) { a.ins(88, {dst(kTemp, 0), src(kInput, 0), src(kInput, 1), src(kConst, 0)}); }), v1[0], cv(0, 1), v1[2], v1[3]));
  // a constant the shader defines
  CHECK(colour_is(pixel(1, [](Asm& a) { a.def(3, 0.25f, 0.5f, 0.75f, 1.0f).ins(1, {dst(kTemp, 0), src(kConst, 3, swz(2, 2, 2, 0))}); }), 0.75f, 0.75f, 0.75f, 0.25f));

  // texcoord: the coordinates between 0 and 1, with 1 for alpha; t<n> is then a register like the others
  CHECK(colour_is(pixel(1, [](Asm& a) { a.ins(64, {dst(kAddr, 1)}).ins(1, {dst(kTemp, 0), src(kAddr, 1)}); }), 1.0f, 0.2f, 0.0f, 1.0f));
  CHECK(colour_is(pixel(1, [](Asm& a) { a.ins(64, {dst(kAddr, 0)}).ins(2, {dst(kAddr, 0), src(kAddr, 0), src(kInput, 0)}).ins(1, {dst(kTemp, 0), src(kAddr, 0)}); }),
                  0.125f + v0[0], 0.875f + v0[1], 0.0f + v0[2], 1.0f + v0[3]));
  // texkill: by the coordinates' first three components
  {
    Result r = pixel(1, [](Asm& a) { a.ins(65, {dst(kAddr, 0)}).ins(1, {dst(kTemp, 0), src(kInput, 0)}); });
    CHECK(r.ok && r.alive);
    r = pixel(1, [](Asm& a) { a.ins(65, {dst(kAddr, 1)}).ins(1, {dst(kTemp, 0), src(kInput, 0)}); });      // (coordinates 1 have z below 0)
    CHECK(r.ok && !r.alive);
    r = pixel(4, [](Asm& a) { a.ins(64, {dst(kTemp, 2, 0x7), src(kAddr, 1)}).ins(65, {dst(kTemp, 2)}).ins(1, {dst(kTemp, 0), src(kInput, 0)}); });
    CHECK(r.ok && !r.alive);
  }
  // texm3x3pad, texm3x3pad, texm3x3: the coordinates 1, 2 and 3 as the rows of a matrix, times a vector; texdp3
  {
    const float x[3] = {0.125f, 0.875f, 0.0f};      // (t0 after texcoord)
    auto row = [&](const float* m) { return m[0] * x[0] + m[1] * x[1] + m[2] * x[2]; };
    CHECK(colour_is(pixel(2, [](Asm& a) {
            a.ins(64, {dst(kAddr, 0)}).ins(73, {dst(kAddr, 1), src(kAddr, 0)}).ins(73, {dst(kAddr, 2), src(kAddr, 0)}).ins(86, {dst(kAddr, 3), src(kAddr, 0)});
            a.ins(1, {dst(kTemp, 0), src(kAddr, 3)});
          }), row(kIn[3]), row(kMore[0]), row(kMore[1]), 1.0f));
    CHECK(colour_is(pixel(2, [](Asm& a) { a.ins(64, {dst(kAddr, 0)}).ins(85, {dst(kAddr, 1), src(kAddr, 0)}).ins(1, {dst(kTemp, 0), src(kAddr, 1)}); }),
                    row(kIn[3]), row(kIn[3]), row(kIn[3]), row(kIn[3])));
  }
  // 1.4: texcrd takes coordinates as they are (and divided by w), r0 .. r5, phase
  CHECK(colour_is(pixel(4, [](Asm& a) {
          a.ins(64, {dst(kTemp, 3, 0x7), src(kAddr, 1)}).ins(1, {dst(kTemp, 3, 0x8), src(kInput, 0)});
          a.ins(0xFFFD, {});
          a.ins(5, {dst(kTemp, 5), src(kTemp, 3), src(kInput, 1)}).ins(1, {dst(kTemp, 0), src(kTemp, 5)});
        }), 1.5f * v1[0], 0.2f * v1[1], -0.3f * v1[2], v0[3] * v1[3]));
  CHECK(colour_is(pixel(4, [](Asm& a) { a.ins(64, {dst(kTemp, 0, 0x3), src(kAddr, 1, kXyzw, 10)}).ins(1, {dst(kTemp, 0, 0xC), src(kInput, 0)}); }),
                  1.5f / 0.9f, 0.2f / 0.9f, v0[2], v0[3]));

  // the instructions that sample are translated and compiled (there is no texture here to look at): stage n at coordinates n, at another register's
  // components, at two rows of a matrix, at a dot product; 1.4's texld from coordinates and from a register
  auto compiles = [&](uint32_t minor, uint32_t samplers, const std::function<void(Asm&)>& body) {
    Asm a(false, 1, minor);
    body(a);
    const Tokens tokens = a.end();
    if (sm_translate(tokens.data(), tokens.size(), &g_shader, &g_context) != SM_OK) return false;
    UniquePointer<D3d9Program> program = compile_translated(allocator, g_shader);
    return program && g_shader.sampler_mask == samplers && program->texture_unit_mask() == samplers;
  };
  CHECK(compiles(1, 0x3, [](Asm& a) { a.ins(66, {dst(kAddr, 0)}).ins(66, {dst(kAddr, 1)}).ins(5, {dst(kTemp, 0), src(kAddr, 0), src(kAddr, 1)}); }));
  CHECK(compiles(1, 0x7, [](Asm& a) { a.ins(66, {dst(kAddr, 0)}).ins(69, {dst(kAddr, 1), src(kAddr, 0)}).ins(70, {dst(kAddr, 2), src(kAddr, 0)}).ins(1, {dst(kTemp, 0), src(kAddr, 2)}); }));
  CHECK(compiles(1, 0x5, [](Asm& a) { a.ins(66, {dst(kAddr, 0)}).ins(71, {dst(kAddr, 1), src(kAddr, 0, kXyzw, 4)}).ins(72, {dst(kAddr, 2), src(kAddr, 0, kXyzw, 4)}).ins(1, {dst(kTemp, 0), src(kAddr, 2)}); }));
  CHECK(compiles(2, 0x3, [](Asm& a) { a.ins(66, {dst(kAddr, 0)}).ins(83, {dst(kAddr, 1), src(kAddr, 0)}).ins(1, {dst(kTemp, 0), src(kAddr, 1)}); }));
  CHECK(compiles(4, 0x3, [](Asm& a) { a.ins(66, {dst(kTemp, 0), src(kAddr, 0)}).ins(0xFFFD, {}).ins(66, {dst(kTemp, 1), src(kTemp, 0)}).ins(5, {dst(kTemp, 0), src(kTemp, 0), src(kTemp, 1)}); }));

  // what is not there, and what is not right
  auto status_of = [](uint32_t minor, const std::function<void(Asm&)>& body) {
    Asm a(false, 1, minor);
    body(a);
    const Tokens tokens = a.end();
    return sm_translate(tokens.data(), tokens.size(), &g_shader, &g_context);
  };
  CHECK(status_of(1, [](Asm& a) { a.ins(66, {dst(kAddr, 0)}).ins(67, {dst(kAddr, 1), src(kAddr, 0)}); }) == SM_UNSUPPORTED);      // texbem
  CHECK(status_of(1, [](Asm& a) { a.ins(73, {dst(kAddr, 1), src(kAddr, 0)}).ins(73, {dst(kAddr, 2), src(kAddr, 0)}).ins(74, {dst(kAddr, 3), src(kAddr, 0)}); }) == SM_UNSUPPORTED);
  CHECK(status_of(4, [](Asm& a) { a.ins(87, {dst(kTemp, 5)}); }) == SM_UNSUPPORTED);                                                // texdepth
  CHECK(status_of(1, [](Asm& a) { a.ins(72, {dst(kAddr, 1), src(kAddr, 0)}); }) == SM_INVALID);                                       // texm3x2tex with no row before it
  CHECK(status_of(1, [](Asm& a) { a.ins(66, {dst(kAddr, 7)}); }) == SM_INVALID);                                                      // a stage that is not there
  CHECK(status_of(1, [](Asm& a) { a.ins(6, {dst(kTemp, 0), src(kInput, 0)}); }) == SM_UNSUPPORTED);                                   // rcp is no instruction of theirs
  CHECK(status_of(5, [](Asm& a) { a.ins(1, {dst(kTemp, 0), src(kInput, 0)}); }) == SM_INVALID);                                       // ps_1_5
  {
    Asm a(true, 1, 1);
    a.ins(1, {dst(kRastOut, 0) | (1u << 24), src(kInput, 0)});      // a factor on a vertex shader's destination
    const Tokens tokens = a.end();
    CHECK(sm_translate(tokens.data(), tokens.size(), &g_shader, &g_context) == SM_INVALID);
  }
}

// A pixel shader 1.1 with a bit of everything, for the tests of damaged shaders.
Tokens pixel_shader_1_sample() {
  Asm a(false, 1, 1);
  a.def(0, 0.5f, 0.5f, 0.5f, 1.0f);
  a.ins(66, {dst(kAddr, 0)}).ins(64, {dst(kAddr, 1)}).ins(71, {dst(kAddr, 2), src(kAddr, 0, kXyzw, 4)}).ins(72, {dst(kAddr, 3), src(kAddr, 0, kXyzw, 4)});
  a.ins(8, {dst(kTemp, 1, 0x7, true) | (1u << 24), src(kAddr, 0, kXyzw, 4), src(kInput, 0, kXyzw, 4)});
  a.ins(18, {dst(kTemp, 0), src(kInput, 1, swz(3, 3, 3, 3)), src(kAddr, 3), src(kAddr, 1, kXyzw, 6)});
  a.ins(4, {dst(kTemp, 0, 0x7), src(kTemp, 0), src(kTemp, 1), src(kConst, 0)}, 0x40 /* (with the instruction before it) */);
  return a.end();
}

void test_refusals() {
  auto status_of = [](const Tokens& t) { return sm_translate(t.data(), t.size(), &g_shader, &g_context); };
  CHECK(status_of({0xFFFF0101, 0x0000FFFF}) == SM_OK);               // a pixel shader 1.1 of no instructions: r0 as it is
  CHECK(status_of({0xFFFE0400, 0x0000FFFF}) == SM_INVALID);          // no such version
  CHECK(status_of({0x12345678, 0x0000FFFF}) == SM_INVALID);
  CHECK(status_of({0xFFFE0200}) == SM_INVALID);                      // no end
  CHECK(sm_translate(nullptr, 0, &g_shader, &g_context) == SM_INVALID);
  CHECK(status_of(Asm(true, 2, 0).end()) == SM_INVALID);             // a vertex shader that writes no position
  {
    Asm a(true, 2, 0);
    a.ins(27, {src(15, 0), src(7, 0)});      // loop aL, i0
    CHECK(status_of(a.end()) == SM_UNSUPPORTED);
  }
  {
    Asm a(false, 2, 0);
    a.dcl(3u << 27, 0, reg(kSampler, 0));      // a cube sampler
    a.ins(66, {dst(kTemp, 0), src(kAddr, 0), src(kSampler, 0)});
    CHECK(status_of(a.end()) == SM_UNSUPPORTED);
  }
  {
    Asm a(false, 2, 0);
    a.ins(1, {dst(kColorOut, 1), src(kInput, 0)});      // a second render target
    CHECK(status_of(a.end()) == SM_UNSUPPORTED);
  }
  {
    Asm a(true, 2, 0);
    a.ins(1, {dst(kTemp, 40), src(kInput, 0)});      // a temporary that is not there
    CHECK(status_of(a.end()) == SM_INVALID);
  }
  {
    Asm a(true, 2, 0);
    a.ins(1, {dst(kRastOut, 0), src(kInput, 0)});
    a.ins(2, {dst(kTemp, 0), src(kInput, 0)});      // add with one source
    CHECK(status_of(a.end()) == SM_INVALID);
  }
}

// ---- the shaders a compiler made ---------------------------------------------------------------------------------------------------------------------

void test_fixtures(HostAllocator& allocator, std::vector<Tokens>* all) {
  static const char* const names[] = {"vs_basic.vs_1_1.bin", "vs_basic.vs_2_0.bin", "vs_basic.vs_3_0.bin", "vs_math.vs_2_0.bin", "vs_math.vs_3_0.bin",
                                      "ps_tex.ps_2_0.bin",   "ps_tex.ps_3_0.bin",   "ps_math.ps_2_0.bin",  "ps_math.ps_3_0.bin"};
  for (const char* name : names) {
    const Tokens tokens = read_tokens(name);
    CHECK(tokens.size() > 8);
    if (tokens.size() <= 8) { printf("  (%s is not there)\n", name); continue; }
    all->push_back(tokens);
    const int status = sm_translate(tokens.data(), tokens.size(), &g_shader, &g_context);
    if (status != SM_OK) printf("  %s: not translated (%d)\n", name, status);
    CHECK(status == SM_OK);
    if (status != SM_OK) continue;
    NtStatus compiled = 0;
    UniquePointer<D3d9Program> translated = compile_translated(allocator, g_shader, &compiled);
    if (!translated) printf("  %s: the kernel does not take the translation (%08x)\n", name, static_cast<unsigned>(compiled));
    CHECK(translated && compiled == status::kSuccess);
    if (!translated) continue;
    CHECK((translated->stage() == graphics::ShaderStage::Vertex) == (g_shader.vertex != 0));
    CHECK(translated->constant_bytes() == g_shader.constant_registers * 16);
    const bool textured = g_shader.sampler_mask != 0;
    CHECK(textured == (strncmp(name, "ps_tex", 6) == 0) && (!textured || translated->texture_unit_mask() == 1));
    // The kernel's own compiler for this bytecode takes the simpler ones: the two programs then agree on every input set.
    UniquePointer<D3d9Program> native;
    const NtStatus native_status = D3d9Program::compile(allocator, ConstByteView(reinterpret_cast<const uint8_t*>(tokens.data()), tokens.size() * 4), &native);
    if (native_status != status::kSuccess || !native || textured) {
      printf("  %s: translated and compiled (%zu tokens to %zu bytes); not compared (%s)\n", name, tokens.size(), g_shader.dxbc_size,
             textured ? "it samples a texture" : "the kernel's Direct3D 9 compiler does not take it");
      continue;
    }
    int compared = 0;
    for (unsigned set = 0; set < 6; ++set) {
      uint8_t constants[4096] = {};
      for (unsigned n = 0; n < 8; ++n)
        for (unsigned k = 0; k < 4; ++k) { const float v = constant_value(n + set, k) * (set & 1 ? -1.3f : 0.8f); memcpy(constants + n * 16 + k * 4, &v, 4); }
      graphics::Vec4 in[16] = {};
      for (unsigned i = 0; i < 4; ++i) in[i] = {kIn[(i + set) % 4][0] + 0.05f * set, kIn[(i + set) % 4][1], kIn[(i + set) % 4][2], kIn[(i + set) % 4][3]};
      bool same = true;
      if (g_shader.vertex) {
        graphics::ShaderIo x = {}, y = {};
        translated->run_vertex(in, constants, &x);
        native->run_vertex(in, constants, &y);
        const graphics::Vec4* a[5] = {&x.position, &x.varying[0], &x.varying[1], &x.varying[2], &x.varying[3]};
        const graphics::Vec4* b[5] = {&y.position, &y.varying[0], &y.varying[1], &y.varying[2], &y.varying[3]};
        for (unsigned o = 0; o < 5; ++o) same = same && near_value(a[o]->x, b[o]->x) && near_value(a[o]->y, b[o]->y) && near_value(a[o]->z, b[o]->z) && near_value(a[o]->w, b[o]->w);
        if (!same) printf("  %s set %u: position %g %g %g %g against %g %g %g %g\n", name, set, x.position.x, x.position.y, x.position.z, x.position.w, y.position.x, y.position.y, y.position.z, y.position.w);
      } else {
        graphics::Vec4 x[graphics::kMaxColorTargets] = {}, y[graphics::kMaxColorTargets] = {};
        const bool alive_x = translated->run_fragment(in, constants, nullptr, &x[0]), alive_y = native->run_fragment(in, constants, nullptr, &y[0]);
        same = alive_x == alive_y && (!alive_x || (near_value(x[0].x, y[0].x) && near_value(x[0].y, y[0].y) && near_value(x[0].z, y[0].z) && near_value(x[0].w, y[0].w)));
        if (!same) printf("  %s set %u: colour %g %g %g %g against %g %g %g %g\n", name, set, x[0].x, x[0].y, x[0].z, x[0].w, y[0].x, y[0].y, y[0].z, y[0].w);
      }
      CHECK(same);
      ++compared;
    }
    printf("  %s: the translation agrees with the kernel's own compiler on %d input sets\n", name, compared);
  }
}

// ---- shaders that are not right ------------------------------------------------------------------------------------------------------------------------

void test_damage(HostAllocator& allocator, const std::vector<Tokens>& shaders) {
  uint64_t truncated = 0, flipped = 0, taken = 0;
  for (const Tokens& whole : shaders) {
    // every length short of the whole: refused (the end is missing), with nothing read past it - the copy is exactly that long
    for (size_t n = 0; n < whole.size(); ++n) {
      const Tokens part(whole.begin(), whole.begin() + static_cast<ptrdiff_t>(n));
      const int status = sm_translate(part.data(), part.size(), &g_shader, &g_context);
      CHECK(status != SM_OK);
      ++truncated;
    }
    // a bit turned, all over the shader: taken or refused; what is taken, the kernel's compiler takes or refuses
    Tokens damaged = whole;
    uint32_t seed = 0x1234567u + static_cast<uint32_t>(whole.size());
    for (unsigned round = 0; round < 3000; ++round) {
      seed = seed * 1664525u + 1013904223u;
      const size_t at = (seed >> 8) % damaged.size();
      const uint32_t bit = 1u << (seed >> 27);
      damaged[at] ^= bit;
      const int status = sm_translate(damaged.data(), damaged.size(), &g_shader, &g_context);
      if (status == SM_OK) {
        CHECK(g_shader.dxbc_size >= 32 && g_shader.dxbc_size <= SM_DXBC_BYTES && g_shader.constant_registers <= 256);
        UniquePointer<D3d9Program> program = compile_translated(allocator, g_shader);
        ++taken;
      }
      damaged[at] ^= bit;
      ++flipped;
    }
  }
  printf("  %llu shortened shaders refused; %llu with a bit turned, of which %llu were still translated\n", static_cast<unsigned long long>(truncated),
         static_cast<unsigned long long>(flipped), static_cast<unsigned long long>(taken));
}

}  // namespace

int main() {
  HostAllocator allocator;
  std::vector<Tokens> shaders;
  test_instructions(allocator);
  test_pixel_shaders_1(allocator);
  test_refusals();
  test_fixtures(allocator, &shaders);
  shaders.push_back(pixel_shader_1_sample());
  CHECK(sm_translate(shaders.back().data(), shaders.back().size(), &g_shader, &g_context) == SM_OK && g_shader.sampler_mask == 0x9);
  test_damage(allocator, shaders);
  CHECK(allocator.live == 0);
  if (g_failed) {
    printf("SmToDxbcTest: %d of %d checks failed\n", g_failed, g_checks);
    return 1;
  }
  printf("SmToDxbcTest: all %d checks passed\n", g_checks);
  return 0;
}
