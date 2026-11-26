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

// Host-side tests of src/Windows/d3d8/d3d8_shader.h, what d3d8.dll makes of a Direct3D 8 program's shaders for Direct3D 9:
//   - vertex shader declarations (D3DVSD tokens) as Direct3D 9 vertex elements: the registers' usages, streams, offsets, skipped data, the
//     tessellator's outputs, constants and extensions, and every malformed form refused;
//   - vertex shaders 1.0 / 1.1 rewritten with dcl instructions, pixel shaders 1.0 .. 1.4 as Direct3D 9 takes them - and the result translated by
//     d3d9.dll's own translator (sm_to_dxbc.h), which must take it and see each input with its declaration's usage;
//   - the rules of ValidateVertexShader and ValidatePixelShader, with the cases of Wine's conformance test;
//   - every shortened declaration and shader, and thousands with a bit turned (the test runs under ASan and UBSan).
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "../src/Windows/d3d8/d3d8_shader.h"
#include "../src/Windows/d3d9/sm_to_dxbc.h"

static int g_checks = 0, g_failed = 0;
#define CHECK(x) \
  do { \
    ++g_checks; \
    if (!(x)) { \
      ++g_failed; \
      printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } \
  } while (0)

// The D3DVSD_* macros of d3d8types.h.
static uint32_t STREAM(uint32_t n) { return (1u << 29) | n; }
static uint32_t STREAM_TESS() { return (1u << 29) | (1u << 28); }
static uint32_t REG(uint32_t reg, uint32_t type) { return (2u << 29) | (type << 16) | reg; }
static uint32_t SKIP(uint32_t dwords) { return (2u << 29) | 0x10000000u | (dwords << 16); }
static uint32_t SKIPBYTES(uint32_t bytes) { return (2u << 29) | 0x18000000u | (bytes << 16); }
static uint32_t TESSNORMAL(uint32_t in, uint32_t out) { return (3u << 29) | (in << 20) | (2u << 16) | out; }
static uint32_t TESSUV(uint32_t reg) { return (3u << 29) | 0x10000000u | (1u << 16) | reg; }
static uint32_t CONST(uint32_t first, uint32_t count) { return (4u << 29) | (count << 25) | first; }
static uint32_t EXT(uint32_t count) { return (5u << 29) | (count << 24); }
static const uint32_t END = 0xFFFFFFFFu;
enum { FLOAT1, FLOAT2, FLOAT3, FLOAT4, D3DCOLOR, UBYTE4, SHORT2, SHORT4 };
enum { POSITION = 0, BLENDWEIGHT, BLENDINDICES, NORMAL, PSIZE, DIFFUSE, SPECULAR, TEXCOORD0, TEXCOORD1, TEXCOORD2, TEXCOORD3, TEXCOORD4, TEXCOORD5,
       TEXCOORD6, TEXCOORD7, POSITION2, NORMAL2 };

static unsigned g_seed = 4242;
static unsigned random_number() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

static SmShader g_translated;
static SmContext g_context;

static bool element_is(const D8Element& e, unsigned stream, unsigned offset, unsigned type, unsigned method, unsigned usage, unsigned index) {
  return e.stream == stream && e.offset == offset && e.type == type && e.method == method && e.usage == usage && e.usage_index == index;
}

static void test_declarations() {
  D8Declaration d;
  {
    const uint32_t t[] = {STREAM(0), REG(POSITION, FLOAT3), REG(DIFFUSE, D3DCOLOR), END};
    CHECK(d8_declaration_parse(t, 4, &d) == D8_OK);
    CHECK(d.tokens == 4 && d.element_count == 2 && d.declared == ((1u << 0) | (1u << 5)));
    CHECK(element_is(d.elements[0], 0, 0, FLOAT3, D8_METHOD_DEFAULT, D8_USAGE_POSITION, 0));
    CHECK(element_is(d.elements[1], 0, 12, D3DCOLOR, D8_METHOD_DEFAULT, D8_USAGE_COLOR, 0));
  }
  {
    // Every register of D3DVSDE_* with its usage, each type's size, two streams and skipped data.
    const uint32_t t[] = {STREAM(0), REG(POSITION, FLOAT4), REG(BLENDWEIGHT, FLOAT1), REG(BLENDINDICES, UBYTE4), REG(NORMAL, FLOAT3), REG(PSIZE, FLOAT1),
                          SKIP(2), REG(DIFFUSE, D3DCOLOR), REG(SPECULAR, D3DCOLOR), SKIPBYTES(4), REG(TEXCOORD0, FLOAT2), REG(TEXCOORD1, SHORT2),
                          REG(TEXCOORD2, SHORT4), STREAM(3), REG(TEXCOORD3, FLOAT1), REG(TEXCOORD4, FLOAT2), REG(TEXCOORD5, FLOAT3), REG(TEXCOORD6, FLOAT4),
                          REG(TEXCOORD7, FLOAT4), REG(POSITION2, FLOAT3), REG(NORMAL2, FLOAT3), END};
    CHECK(d8_declaration_parse(t, sizeof t / 4, &d) == D8_OK);
    CHECK(d.element_count == 17 && d.declared == 0x1FFFFu && d.tokens == sizeof t / 4);
    CHECK(element_is(d.elements[0], 0, 0, FLOAT4, 0, D8_USAGE_POSITION, 0));
    CHECK(element_is(d.elements[1], 0, 16, FLOAT1, 0, D8_USAGE_BLENDWEIGHT, 0));
    CHECK(element_is(d.elements[2], 0, 20, UBYTE4, 0, D8_USAGE_BLENDINDICES, 0));
    CHECK(element_is(d.elements[3], 0, 24, FLOAT3, 0, D8_USAGE_NORMAL, 0));
    CHECK(element_is(d.elements[4], 0, 36, FLOAT1, 0, D8_USAGE_PSIZE, 0));
    CHECK(element_is(d.elements[5], 0, 48, D3DCOLOR, 0, D8_USAGE_COLOR, 0));      // (36 + 4 + two skipped words)
    CHECK(element_is(d.elements[6], 0, 52, D3DCOLOR, 0, D8_USAGE_COLOR, 1));
    CHECK(element_is(d.elements[7], 0, 60, FLOAT2, 0, D8_USAGE_TEXCOORD, 0));      // (and 4 skipped bytes)
    CHECK(element_is(d.elements[8], 0, 68, SHORT2, 0, D8_USAGE_TEXCOORD, 1));
    CHECK(element_is(d.elements[9], 0, 72, SHORT4, 0, D8_USAGE_TEXCOORD, 2));
    CHECK(element_is(d.elements[10], 3, 0, FLOAT1, 0, D8_USAGE_TEXCOORD, 3));
    CHECK(element_is(d.elements[11], 3, 4, FLOAT2, 0, D8_USAGE_TEXCOORD, 4));
    CHECK(element_is(d.elements[12], 3, 12, FLOAT3, 0, D8_USAGE_TEXCOORD, 5));
    CHECK(element_is(d.elements[13], 3, 24, FLOAT4, 0, D8_USAGE_TEXCOORD, 6));
    CHECK(element_is(d.elements[14], 3, 40, FLOAT4, 0, D8_USAGE_TEXCOORD, 7));
    CHECK(element_is(d.elements[15], 3, 56, FLOAT3, 0, D8_USAGE_POSITION, 1));
    CHECK(element_is(d.elements[16], 3, 68, FLOAT3, 0, D8_USAGE_NORMAL, 1));
    CHECK(d.usage[SPECULAR] == D8_USAGE_COLOR && d.usage_index[SPECULAR] == 1 && d.usage[TEXCOORD7] == D8_USAGE_TEXCOORD && d.usage_index[TEXCOORD7] == 7);
  }
  {
    // Constants (kept by where their numbers are), an extension and no-ops; the tessellator's normal and coordinates.
    const uint32_t t[] = {CONST(4, 2), 1, 2, 3, 4, 5, 6, 7, 8, 0, STREAM(1), REG(POSITION, FLOAT3), EXT(2), 99, 98, STREAM_TESS(), TESSNORMAL(POSITION, NORMAL),
                          TESSUV(TEXCOORD0), CONST(90, 1), 9, 10, 11, 12, END};
    CHECK(d8_declaration_parse(t, sizeof t / 4, &d) == D8_OK);
    CHECK(d.load_count == 2 && d.loads[0].first == 4 && d.loads[0].count == 2 && d.loads[0].token == 1 && t[d.loads[0].token + 7] == 8);
    CHECK(d.loads[1].first == 90 && d.loads[1].count == 1 && t[d.loads[1].token] == 9);
    CHECK(d.element_count == 3);
    CHECK(element_is(d.elements[0], 1, 0, FLOAT3, 0, D8_USAGE_POSITION, 0));
    CHECK(element_is(d.elements[1], 1, 0, D8_TYPE_UNUSED, D8_METHOD_CROSSUV, D8_USAGE_NORMAL, 0));
    CHECK(element_is(d.elements[2], 0, 0, D8_TYPE_UNUSED, D8_METHOD_UV, D8_USAGE_TEXCOORD, 0));
    CHECK(d.declared == ((1u << POSITION) | (1u << NORMAL) | (1u << TEXCOORD0)));
  }
  // What is refused.
  const std::vector<std::vector<uint32_t>> bad = {
    {REG(POSITION, FLOAT3), END},                                  // (no stream)
    {STREAM(0), REG(17, FLOAT3), END},                             // (no register 17)
    {STREAM(0), REG(POSITION, 8), END},                            // (no type 8)
    {STREAM(0), REG(POSITION, FLOAT3), REG(POSITION, FLOAT2), END},   // (a register twice)
    {STREAM_TESS(), TESSNORMAL(POSITION, NORMAL), END},            // (the normal of an input the declaration has not)
    {STREAM(0), TESSUV(TEXCOORD0), END},                           // (the tessellator's data in a stream)
    {STREAM_TESS(), REG(POSITION, FLOAT3), END},                   // (a stream's data in the tessellator's)
    {CONST(0, 0), END},                                            // (no constants)
    {CONST(250, 8), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, END},   // (beyond 256)
    {6u << 29, END},                                               // (no kind 6)
    {0xEFFFFFFFu, END},                                            // (kind 7 that is not the end)
    {0x00000001u, END},                                            // (a no-op with bits)
    {STREAM(16 | 0x100), END},                                     // (bits of no field)
    {STREAM(0), REG(POSITION, FLOAT3)},                            // (no end)
  };
  for (const auto& t : bad) CHECK(d8_declaration_parse(t.data(), t.size(), &d) == D8_INVALID);
  CHECK(d8_declaration_parse(nullptr, 4, &d) == D8_INVALID);
  {
    // Offsets of a stream past 65535 bytes (60 bytes a skip), and a register after them.
    std::vector<uint32_t> t = {STREAM(0)};
    for (int i = 0; i < 1092; ++i) t.push_back(SKIP(15));
    t.push_back(REG(POSITION, FLOAT3));
    t.push_back(END);
    CHECK(d8_declaration_parse(t.data(), t.size(), &d) == D8_OK && d.elements[0].offset == 65520);
    t.insert(t.end() - 2, SKIP(1));
    CHECK(d8_declaration_parse(t.data(), t.size(), &d) == D8_INVALID);
  }

  // Every shortened declaration is refused; with a bit turned it is refused or read, never more than it is long.
  const uint32_t full[] = {CONST(0, 1), 1, 2, 3, 4, STREAM(0), REG(POSITION, FLOAT3), REG(NORMAL, FLOAT3), SKIP(1), REG(DIFFUSE, D3DCOLOR), STREAM(1),
                           REG(TEXCOORD0, FLOAT2), STREAM_TESS(), TESSUV(TEXCOORD1), EXT(1), 7, END};
  const size_t n = sizeof full / 4;
  CHECK(d8_declaration_parse(full, n, &d) == D8_OK && d.tokens == n && d.element_count == 5);
  for (size_t length = 0; length < n; ++length) {
    std::vector<uint32_t> part(full, full + length);      // (a vector of its own: ASan sees a read past its end)
    CHECK(d8_declaration_parse(part.data(), part.size(), &d) == D8_INVALID);
  }
  int read = 0;
  for (int round = 0; round < 20000; ++round) {
    std::vector<uint32_t> t(full, full + n);
    const unsigned flips = 1 + random_number() % 3;
    for (unsigned f = 0; f < flips; ++f) t[random_number() % n] ^= 1u << (random_number() % 32);
    if (d8_declaration_parse(t.data(), t.size(), &d) == D8_OK) {
      ++read;
      CHECK(d.tokens <= n && d.element_count <= D8_MAX_ELEMENTS && d.load_count <= D8_MAX_LOADS);
      for (uint32_t i = 0; i < d.load_count; ++i) CHECK(d.loads[i].token + d.loads[i].count * 4 <= d.tokens);
    }
  }
  CHECK(read > 0);
}

// ---- shaders ----------------------------------------------------------------------------------------------------------------------------------

// Parameter tokens of Shader Model 1.
static uint32_t dst(uint32_t type, uint32_t n, uint32_t mask = 0xF) { return 0x80000000u | (type << 28) | (mask << 16) | n; }
static uint32_t src(uint32_t type, uint32_t n, uint32_t swizzle = 0xE4) { return 0x80000000u | (type << 28) | (swizzle << 16) | n; }
enum { R = 0, V = 1, C = 2, A = 3, T = 3, RASTOUT = 4, ATTROUT = 5, TEXCRDOUT = 6 };

// A vertex shader of Direct3D 8: the position through c0 .. c3, the colour and two sets of coordinates as they come, a light term, a comment.
static std::vector<uint32_t> vertex_shader(uint32_t version = 0xFFFE0101u) {
  return {version,
          0x0000FFFEu | (2u << 16), 0x41414141u, 0x42424242u,                 // (a comment of two tokens)
          0x00000014u, dst(RASTOUT, 0), src(V, POSITION), src(C, 0),          // m4x4 oPos, v0, c0
          0x00000001u, dst(ATTROUT, 0), src(V, DIFFUSE),                      // mov oD0, v5
          0x00000001u, dst(TEXCRDOUT, 0), src(V, TEXCOORD0),                  // mov oT0, v7
          0x00000008u, dst(R, 0, 1), src(V, NORMAL), src(C, 4),               // dp3 r0.x, v3, c4
          0x00000001u, dst(A, 0, 1), src(C, 5),                               // mov a0.x, c5
          0x00000001u, dst(TEXCRDOUT, 1), src(C, 6) | 0x2000u,                // mov oT1, c[a0.x + 6]
          0x00000001u, dst(ATTROUT, 1), src(R, 0, 0x00),                      // mov oD1, r0.x
          0x0000FFFFu};
}

static void test_vertex_shaders() {
  const uint32_t decl[] = {STREAM(0), REG(POSITION, FLOAT3), REG(NORMAL, FLOAT3), REG(DIFFUSE, D3DCOLOR), REG(TEXCOORD0, FLOAT2), END};
  D8Declaration d;
  CHECK(d8_declaration_parse(decl, sizeof decl / 4, &d) == D8_OK);
  for (uint32_t version : {0xFFFE0100u, 0xFFFE0101u}) {
    const std::vector<uint32_t> code = vertex_shader(version);
    D8ShaderInfo info;
    CHECK(d8_shader_scan(code.data(), code.size(), 1, &info) == D8_OK);
    CHECK(info.tokens == code.size() && info.instructions == 7);
    CHECK(info.inputs_read == ((1u << POSITION) | (1u << DIFFUSE) | (1u << TEXCOORD0) | (1u << NORMAL)));
    CHECK(info.constants_used == 6);      // (c0 .. c5 by number; c[a0.x + 6] is not counted)
    const size_t count = d8_vertex_shader_tokens(code.data(), &info, &d, nullptr, 0);
    CHECK(count == code.size() + 3 * 4);
    std::vector<uint32_t> out(count);
    CHECK(d8_vertex_shader_tokens(code.data(), &info, &d, out.data(), out.size() - 1) == count);      // (no room: nothing written)
    CHECK(d8_vertex_shader_tokens(code.data(), &info, &d, out.data(), out.size()) == count);
    CHECK(out[0] == 0xFFFE0101u);
    CHECK(out[1] == 0x1Fu && out[2] == 0x80000000u && out[3] == 0x900F0000u);                    // dcl_position v0
    CHECK(out[4] == 0x1Fu && out[5] == 0x80000003u && out[6] == 0x900F0003u);                    // dcl_normal v3
    CHECK(out[7] == 0x1Fu && out[8] == 0x8000000Au && out[9] == 0x900F0005u);                    // dcl_color v5
    CHECK(out[10] == 0x1Fu && out[11] == 0x80000005u && out[12] == 0x900F0007u);                 // dcl_texcoord v7
    CHECK(memcmp(out.data() + 13, code.data() + 1, (code.size() - 1) * 4) == 0);
    // Direct3D 9's translator takes it, and sees the inputs as the declaration has them.
    CHECK(sm_translate(out.data(), out.size(), &g_translated, &g_context) == SM_OK);
    CHECK(g_translated.vertex && g_translated.input_count == 4);
    CHECK(g_translated.inputs[2].usage == D8_USAGE_COLOR && g_translated.inputs[2].index == 0 && g_translated.inputs[2].reg == DIFFUSE);
    CHECK(g_translated.inputs[3].usage == D8_USAGE_TEXCOORD && g_translated.inputs[3].reg == TEXCOORD0);
  }
  {
    // A register the shader reads that the declaration has not: an input no element fills (binormal <register>).
    const uint32_t only_position[] = {STREAM(0), REG(POSITION, FLOAT3), END};
    CHECK(d8_declaration_parse(only_position, 3, &d) == D8_OK);
    const std::vector<uint32_t> code = vertex_shader();
    D8ShaderInfo info;
    CHECK(d8_shader_scan(code.data(), code.size(), 1, &info) == D8_OK);
    std::vector<uint32_t> out(d8_vertex_shader_tokens(code.data(), &info, &d, nullptr, 0));
    d8_vertex_shader_tokens(code.data(), &info, &d, out.data(), out.size());
    CHECK(out[2] == 0x80000000u && out[5] == (0x80000000u | D8_USAGE_BINORMAL | (3u << 16)) && out[6] == 0x900F0003u);
    CHECK(sm_translate(out.data(), out.size(), &g_translated, &g_context) == SM_OK);
  }
  {
    // Every arithmetic instruction of vs 1.1 the translator is given: lit, dst, expp, logp, exp, log, frc, rcp, rsq, min, max, slt, sge, the matrices, mad, sub.
    std::vector<uint32_t> code = {0xFFFE0101u};
    const uint32_t two[] = {1, 6, 7, 14, 15, 16, 19, 78, 79}, three[] = {2, 3, 5, 8, 9, 10, 11, 12, 13, 17, 20, 21, 22, 23, 24};
    for (uint32_t op : two) { code.push_back(op); code.push_back(dst(R, 1)); code.push_back(src(C, 1)); }
    for (uint32_t op : three) { code.push_back(op); code.push_back(dst(R, 2, op >= 20 ? 0x7 : 0xF)); code.push_back(src(V, 0)); code.push_back(src(C, 2)); }
    code.insert(code.end(), {4u, dst(R, 3), src(R, 1), src(R, 2), src(C, 7), 0u, 0x00000001u, dst(RASTOUT, 0), src(R, 3), 0x0000FFFFu});
    D8ShaderInfo info;
    CHECK(d8_shader_scan(code.data(), code.size(), 1, &info) == D8_OK);
    const uint32_t decl0[] = {STREAM(0), REG(POSITION, FLOAT4), END};
    CHECK(d8_declaration_parse(decl0, 3, &d) == D8_OK);
    std::vector<uint32_t> out(d8_vertex_shader_tokens(code.data(), &info, &d, nullptr, 0));
    d8_vertex_shader_tokens(code.data(), &info, &d, out.data(), out.size());
    CHECK(sm_translate(out.data(), out.size(), &g_translated, &g_context) == SM_OK);
  }
  {
    // A shader with Direct3D 9's dcl and a def (Wine's test_unsupported_shaders passes such ones, Windows takes them): the dcl is dropped for the
    // declaration's, the def's register counts among the constants.
    const std::vector<uint32_t> code = {0xFFFE0101u, 0x0000001Fu, 0x80000000u, 0x900F0000u, 0x00000051u, 0xA00F00FFu, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u,
                                        0x00000002u, 0x800F0000u, 0x90E40000u, 0xA0E400FFu, 0x00000001u, 0xC00F0000u, 0x80E40000u, 0x0000FFFFu};
    const uint32_t position[] = {STREAM(0), REG(POSITION, FLOAT3), END};
    CHECK(d8_declaration_parse(position, 3, &d) == D8_OK);
    D8ShaderInfo info;
    CHECK(d8_shader_scan(code.data(), code.size(), 1, &info) == D8_OK && info.own_dcls == 1 && info.constants_used == 256 && info.inputs_read == 1);
    std::vector<uint32_t> out(d8_vertex_shader_tokens(code.data(), &info, &d, nullptr, 0));
    CHECK(out.size() == code.size());
    CHECK(d8_vertex_shader_tokens(code.data(), &info, &d, out.data(), out.size()) == out.size());
    CHECK(out[1] == 0x1Fu && out[4] == 0x51u && memcmp(out.data() + 4, code.data() + 4, (code.size() - 4) * 4) == 0);
    CHECK(sm_translate(out.data(), out.size(), &g_translated, &g_context) == SM_OK);
    std::vector<uint32_t> c256 = code;
    c256[5] = 0xA00F0100u;
    CHECK(d8_shader_scan(c256.data(), c256.size(), 1, &info) == D8_INVALID);      // (no c256)
    std::vector<uint32_t> dcl_output = code;
    dcl_output[3] = 0xC00F0000u;
    CHECK(d8_shader_scan(dcl_output.data(), dcl_output.size(), 1, &info) == D8_INVALID);      // (a dcl of something not an input)
  }
  // What is refused.
  const std::vector<std::vector<uint32_t>> bad = {
    {0xFFFE0102u, 0x0000FFFFu},                                              // (vs.1.2 is no version of Direct3D 8)
    {0xFFFE0200u, 0x0000FFFFu},
    {0xFFFF0101u, 0x0000FFFFu},                                              // (a pixel shader)
    {0xFFFE0101u, 0x00000012u, dst(R, 0), src(R, 0), src(R, 0), src(R, 0), 0x0000FFFFu},   // (lrp is pixel shaders')
    {0xFFFE0101u, 0x00000001u, dst(R, 12), src(V, 0), 0x0000FFFFu},          // (no r12)
    {0xFFFE0101u, 0x00000001u, dst(TEXCRDOUT, 8), src(V, 0), 0x0000FFFFu},   // (no oT8)
    {0xFFFE0101u, 0x00000001u, dst(R, 0), src(V, 16), 0x0000FFFFu},          // (no v16)
    {0xFFFE0101u, 0x00000001u, dst(R, 0), src(C, 256), 0x0000FFFFu},         // (no c256)
    {0xFFFE0101u, 0x00000001u, dst(V, 0), src(C, 0), 0x0000FFFFu},           // (inputs are not written)
    {0xFFFE0101u, 0x00000001u, dst(R, 0), src(RASTOUT, 0), 0x0000FFFFu},     // (outputs are not read)
    {0xFFFE0101u, 0x00000001u, dst(R, 0), 0x00000000u, 0x0000FFFFu},         // (a parameter without its bit)
    {0xFFFE0101u, 0x80000001u, dst(R, 0), src(C, 0), 0x0000FFFFu},           // (an instruction with it)
    {0xFFFE0101u, 0x40000001u, dst(R, 0), src(C, 0), 0x0000FFFFu},           // (co-issue in a vertex shader)
    {0xFFFE0101u, 0x00000001u, dst(R, 0), src(C, 0) | 0x800u, 0x0000FFFFu},  // (the register type bits of Shader Model 2)
    {0xFFFE0101u, 0x0000FFFEu | (5u << 16), 1, 2, 0x0000FFFFu},             // (a comment longer than the shader)
    {0xFFFE0101u, 0x00000001u, dst(R, 0), src(C, 0)},                       // (no end)
  };
  D8ShaderInfo info;
  for (const auto& c : bad) CHECK(d8_shader_scan(c.data(), c.size(), 1, &info) == D8_INVALID);
  CHECK(d8_shader_scan(nullptr, 4, 1, &info) == D8_INVALID);

  // Shortened and with bits turned: refused, or scanned and rewritten into what the translator may refuse but reads within its tokens.
  const std::vector<uint32_t> full = vertex_shader();
  const uint32_t decl_all[] = {STREAM(0), REG(POSITION, FLOAT3), REG(NORMAL, FLOAT3), REG(DIFFUSE, D3DCOLOR), REG(TEXCOORD0, FLOAT2), END};
  CHECK(d8_declaration_parse(decl_all, 6, &d) == D8_OK);
  for (size_t length = 0; length < full.size(); ++length) {
    std::vector<uint32_t> part(full.begin(), full.begin() + length);
    CHECK(d8_shader_scan(part.data(), part.size(), 1, &info) == D8_INVALID);
  }
  int scanned = 0;
  for (int round = 0; round < 6000; ++round) {
    std::vector<uint32_t> code = full;
    const unsigned flips = 1 + random_number() % 3;
    for (unsigned f = 0; f < flips; ++f) code[random_number() % code.size()] ^= 1u << (random_number() % 32);
    if (d8_shader_scan(code.data(), code.size(), 1, &info) != D8_OK) continue;
    ++scanned;
    CHECK(info.tokens <= code.size());
    std::vector<uint32_t> out(d8_vertex_shader_tokens(code.data(), &info, &d, nullptr, 0));
    CHECK(d8_vertex_shader_tokens(code.data(), &info, &d, out.data(), out.size()) == out.size());
    const int status = sm_translate(out.data(), out.size(), &g_translated, &g_context);
    CHECK(status == SM_OK || status == SM_INVALID || status == SM_UNSUPPORTED);
  }
  CHECK(scanned > 0);
}

static void test_pixel_shaders() {
  struct Case { std::vector<uint32_t> code; bool ok; };
  const std::vector<Case> cases = {
    // ps.1.0 .. 1.3: tex t0, texcoord t1, mul r0, t0, v0, mad, lrp, cnd with r0.a, def, the minor-version ops.
    {{0xFFFF0100u, 0x42u, dst(T, 0), 0x40u, dst(T, 1), 0x5u, dst(R, 0), src(T, 0), src(V, 0), 0x0000FFFFu}, true},
    {{0xFFFF0101u, 0x51u, dst(C, 1), 0x3F800000u, 0, 0, 0x3F800000u, 0x42u, dst(T, 0), 0x4u, dst(R, 0), src(T, 0), src(C, 1), src(V, 1),
      0x12u, dst(R, 1), src(C, 1), src(R, 0), src(V, 0), 0x50u, dst(R, 0), src(R, 0, 0xFF), src(R, 1), src(T, 0), 0x0000FFFFu}, true},
    {{0xFFFF0102u, 0x42u, dst(T, 0), 0x9u, dst(R, 0), src(T, 0), src(C, 0), 0x58u, dst(R, 0), src(R, 0), src(C, 1), src(C, 2), 0x0000FFFFu}, true},
    {{0xFFFF0101u, 0x9u, dst(R, 0), src(C, 0), src(C, 1), 0x0000FFFFu}, false},                   // (dp4 is 1.2's)
    {{0xFFFF0101u, 0x58u, dst(R, 0), src(C, 0), src(C, 1), src(C, 2), 0x0000FFFFu}, false},       // (cmp is 1.2's)
    {{0xFFFF0103u, 0x54u, dst(T, 1), src(T, 0), 0x0000FFFFu}, true},                               // (texm3x2depth: 1.3)
    {{0xFFFF0102u, 0x54u, dst(T, 1), src(T, 0), 0x0000FFFFu}, false},
    // ps.1.4: texld r0, t0; texcrd; phase; texdepth; bem.
    {{0xFFFF0104u, 0x42u, dst(R, 0), src(T, 0), 0x40u, dst(R, 1, 0x7), src(T, 1), 0xFFFDu, 0x42u, dst(R, 2), src(R, 1), 0x5u, dst(R, 0), src(R, 0), src(R, 2),
      0x0000FFFFu}, true},
    {{0xFFFF0104u, 0x42u, dst(T, 0), 0x0000FFFFu}, false},                                          // (texld of 1.4 has two parameters ... and this one runs out)
    {{0xFFFF0104u, 0x43u, dst(T, 1), src(T, 0), 0x0000FFFFu}, false},                               // (texbem is not 1.4's)
    {{0xFFFF0101u, 0x57u, dst(R, 5), 0x0000FFFFu}, false},                                          // (texdepth is 1.4's)
    {{0xFFFF0101u, 0x1u, dst(R, 2), src(C, 0), 0x0000FFFFu}, false},                               // (no r2 below 1.4)
    {{0xFFFF0104u, 0x1u, dst(R, 5), src(C, 7), 0x0000FFFFu}, true},
    {{0xFFFF0104u, 0x1u, dst(R, 6), src(C, 0), 0x0000FFFFu}, false},
    {{0xFFFF0101u, 0x1u, dst(R, 0), src(C, 8), 0x0000FFFFu}, false},                               // (no c8)
    {{0xFFFF0101u, 0x51u, dst(C, 8), 0, 0, 0, 0, 0x0000FFFFu}, false},
    {{0xFFFF0101u, 0x41u, src(T, 0), 0x40000001u, dst(R, 0), src(V, 0), 0x0000FFFFu}, true},        // (texkill t0, a co-issued mov)
    {{0xFFFF0105u, 0x0000FFFFu}, false},
    {{0xFFFF0200u, 0x0000FFFFu}, false},
  };
  for (const auto& c : cases) {
    D8ShaderInfo info;
    const int status = d8_shader_scan(c.code.data(), c.code.size(), 0, &info);
    CHECK((status == D8_OK) == c.ok);
    if (status != D8_OK) continue;
    std::vector<uint32_t> out(d8_pixel_shader_tokens(c.code.data(), &info, nullptr, 0));
    CHECK(d8_pixel_shader_tokens(c.code.data(), &info, out.data(), out.size()) == c.code.size());
    CHECK(out[0] == (c.code[0] == 0xFFFF0100u ? 0xFFFF0101u : c.code[0]));
    CHECK(memcmp(out.data() + 1, c.code.data() + 1, (c.code.size() - 1) * 4) == 0);
    // Direct3D 9's translator takes these (texm3x2depth is one it refuses as unsupported).
    const int translated = sm_translate(out.data(), out.size(), &g_translated, &g_context);
    CHECK(translated == SM_OK || (c.code[1] == 0x54u && translated == SM_UNSUPPORTED));
  }
  // Shortened and with bits turned.
  const std::vector<uint32_t> full = cases[1].code;
  D8ShaderInfo info;
  for (size_t length = 0; length < full.size(); ++length) {
    std::vector<uint32_t> part(full.begin(), full.begin() + length);
    CHECK(d8_shader_scan(part.data(), part.size(), 0, &info) == D8_INVALID);
  }
  for (int round = 0; round < 6000; ++round) {
    std::vector<uint32_t> code = full;
    code[random_number() % code.size()] ^= 1u << (random_number() % 32);
    if (d8_shader_scan(code.data(), code.size(), 0, &info) != D8_OK) continue;
    CHECK(info.tokens <= code.size());
    std::vector<uint32_t> out(d8_pixel_shader_tokens(code.data(), &info, nullptr, 0));
    d8_pixel_shader_tokens(code.data(), &info, out.data(), out.size());
    const int status = sm_translate(out.data(), out.size(), &g_translated, &g_context);
    CHECK(status == SM_OK || status == SM_INVALID || status == SM_UNSUPPORTED);
  }
}

// The cases of Wine's test_validate_vs and test_validate_ps (dlls/d3d8/tests/device.c), the ones Windows answers as Wine marks todo too.
static void test_validation() {
  std::vector<uint32_t> vs = {0xFFFE0101u, 0x00000009u, 0xC0010000u, 0x90E40000u, 0xA0E40000u, 0x00000009u, 0xC0020000u, 0x90E40000u, 0xA0E40001u,
                              0x00000009u, 0xC0040000u, 0x90E40000u, 0xA0E40002u, 0x00000009u, 0xC0080000u, 0x90E40000u, 0xA0E40003u, 0x0000FFFFu};
  const uint32_t valid1[] = {STREAM(0), REG(POSITION, FLOAT4), END}, valid2[] = {STREAM(0), REG(POSITION, FLOAT2), END}, invalid[] = {STREAM(0), REG(NORMAL, FLOAT4), END};
  const size_t big = SIZE_MAX / 8;
  CHECK(d8_validate_vertex_shader(nullptr, 0, nullptr, 0, 0, 0) == D8V_NO_CODE);
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), nullptr, 0, 0, 0) == D8V_OK);
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), valid1, big, 0, 0) == D8V_OK);
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), valid2, big, 0, 0) == D8V_OK);
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), invalid, big, 0, 0) == D8V_INPUT);      // (it reads v0, the declaration has v3 only)
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), nullptr, 0, 0xFFFE0101u, 4) == D8V_OK);
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), nullptr, 0, 0xFFFE0100u, 4) == D8V_CAPS_VERSION);
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), nullptr, 0, 0xFFFE0102u, 4) == D8V_OK);
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), nullptr, 0, 0xFFFE0808u, 4) == D8V_OK);
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), nullptr, 0, 0xFFFE0101u, 3) == D8V_CONSTANTS);      // (it reads c3)
  vs[0] = 0xFFFE0100u;
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), nullptr, 0, 0, 0) == D8V_OK);
  vs[0] = 0xFFFE0102u;
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), nullptr, 0, 0, 0) == D8V_VERSION);
  const uint32_t bad_declaration[] = {REG(POSITION, FLOAT4), END};
  vs[0] = 0xFFFE0101u;
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), bad_declaration, big, 0, 0) == D8V_DECLARATION);
  vs.pop_back();
  CHECK(d8_validate_vertex_shader(vs.data(), vs.size(), nullptr, 0, 0, 0) == D8V_CODE);

  std::vector<uint32_t> ps = {0xFFFF0101u, 0x00000001u, 0x800F0001u, 0xA0E40001u, 0x00000002u, 0x800F0000u, 0x80E40001u, 0xA0E40002u, 0x0000FFFFu};
  const std::vector<uint32_t> ps20 = {0xFFFF0200u, 0x02000001u, 0x800F0001u, 0xA0E40001u, 0x03000002u, 0x800F0000u, 0x80E40001u, 0xA0E40002u,
                                      0x02000001u, 0x800F0800u, 0x80E40000u, 0x0000FFFFu};
  CHECK(d8_validate_pixel_shader(nullptr, 0, 0) == D8V_NO_CODE);
  CHECK(d8_validate_pixel_shader(ps.data(), ps.size(), 0) == D8V_OK);
  CHECK(d8_validate_pixel_shader(ps.data(), ps.size(), 0xFFFF0101u) == D8V_OK);
  CHECK(d8_validate_pixel_shader(ps.data(), ps.size(), 0xFFFF0100u) == D8V_CAPS_VERSION);
  CHECK(d8_validate_pixel_shader(ps.data(), ps.size(), 0xFFFF0102u) == D8V_OK);
  CHECK(d8_validate_pixel_shader(ps.data(), ps.size(), 0xFFFF0808u) == D8V_OK);
  ps[0] = 0xFFFF0100u;
  CHECK(d8_validate_pixel_shader(ps.data(), ps.size(), 0) == D8V_OK);
  ps[0] = 0xFFFF0104u;
  CHECK(d8_validate_pixel_shader(ps.data(), ps.size(), 0) == D8V_OK);
  CHECK(d8_validate_pixel_shader(ps20.data(), ps20.size(), 0) == D8V_VERSION);
  ps[0] = 0xFFFF0105u;
  CHECK(d8_validate_pixel_shader(ps.data(), ps.size(), 0) == D8V_VERSION);
  for (unsigned i = 0; i < sizeof d8_validation_messages / sizeof d8_validation_messages[0]; ++i) CHECK(d8_validation_messages[i] && (i == 0) == (d8_validation_messages[i][0] == 0));
}

int main() {
  test_declarations();
  test_vertex_shaders();
  test_pixel_shaders();
  test_validation();
  printf("D3d8ShaderTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
