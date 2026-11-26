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

// The texture stages of d3d9.dll's fixed-function pipeline (src/Windows/d3d9/ff_stages.h): the pixel shader made of a set of stage states is
// translated (sm_to_dxbc.h), compiled by the kernel's Direct3D and run, and what it gives is compared with the stages worked out here, one after
// the other, as Direct3D 9 describes them:
//   - the states a program starts with, with and without a texture, and stages written out by hand: every operation, every kind of argument,
//     the temporary register, eight stages in a row, the specular colour added at the end, coordinates that are divided;
//   - thousands of stage set-ups picked at random, each on several sets of inputs;
//   - the key: stages end at the first that is switched off, a stage with no texture passes on what it is given, and what an operation does not
//     read is not in the key - two set-ups that draw the same have the same key;
//   - keys that ffs_normalize has not seen (every byte anything) and buffers that are too small: tokens are written inside the buffer or not at
//     all, and the translator takes or refuses them (the test runs under ASan and UBSan); shortened shaders are refused.
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Kernel/Dxgkrnl/D3d9Shader.hpp"
#include "../src/Windows/d3d9/sm_to_dxbc.h"
#include "../src/Windows/d3d9/ff_stages.h"

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

SmShader g_shader;
SmContext g_context;

struct Colour { float v[4]; };

// The texture of a unit: a colour that depends on the unit and on where it is looked up (its alpha `g_texel_alpha` where that is not below 0).
float g_texel_alpha = -1.0f;
Colour texel_of(uint32_t unit, float u, float v) {
  return {{0.5f + 0.45f * sinf(u * 3.0f + static_cast<float>(unit)), 0.5f + 0.4f * cosf(v * 2.0f + 0.3f * static_cast<float>(unit)),
           0.15f + 0.1f * static_cast<float>(unit) + 0.05f * sinf(u + v),
           g_texel_alpha >= 0.0f ? g_texel_alpha : 0.3f + 0.08f * static_cast<float>(unit) + 0.1f * cosf(u - v)}};
}
struct Textures final : graphics::ITextureSet {
  graphics::Vec4 sample(uint32_t unit, float u, float v) const override {
    const Colour c = texel_of(unit, u, v);
    return {c.v[0], c.v[1], c.v[2], c.v[3]};
  }
};

// What a pixel is given.
struct Inputs {
  Colour diffuse, specular, factor, constant[FFS_STAGES], coordinate[FFS_STAGES];
};

float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

// ---- the stages as Direct3D 9 describes them ---------------------------------------------------------------------------------------------------------

struct Reference {
  const Inputs& in;
  Colour current, temp, texel;
  unsigned stage = 0;

  Colour argument(unsigned arg, bool alpha) const {
    Colour c;
    switch (arg & FFS_SELECT) {
      case FFS_DIFFUSE: c = in.diffuse; break;
      case FFS_TEXTURE: c = texel; break;
      case FFS_TFACTOR: c = in.factor; break;
      case FFS_SPECULAR: c = in.specular; break;
      case FFS_TEMP: c = temp; break;
      case FFS_CONSTANT: c = in.constant[stage]; break;
      default: c = current; break;
    }
    if (alpha || (arg & FFS_ALPHAREPLICATE)) c = {{c.v[3], c.v[3], c.v[3], c.v[3]}};
    if (arg & FFS_COMPLEMENT) for (float& x : c.v) x = 1.0f - x;
    return c;
  }
  // One half of a stage: three numbers for the colour, or one for the alpha (then the arguments are alphas all through).
  Colour half(unsigned op, const uint8_t arg[3], bool alpha) const {
    const Colour a0 = argument(arg[0], alpha), a1 = argument(arg[1], alpha), a2 = argument(arg[2], alpha);
    Colour r = {};
    float dot = 0.0f;
    for (int k = 0; k < 3; ++k) dot += (a1.v[k] - 0.5f) * (a2.v[k] - 0.5f);
    for (int k = 0; k < 4; ++k) {
      const float x = a1.v[k], y = a2.v[k], z = a0.v[k], xa = a1.v[3];
      float blend = 0.0f;
      switch (op) {
        case FFS_SELECTARG1: r.v[k] = x; break;
        case FFS_SELECTARG2: r.v[k] = y; break;
        case FFS_MODULATE: r.v[k] = x * y; break;
        case FFS_MODULATE2X: r.v[k] = x * y * 2.0f; break;
        case FFS_MODULATE4X: r.v[k] = x * y * 4.0f; break;
        case FFS_ADD: r.v[k] = x + y; break;
        case FFS_ADDSIGNED: r.v[k] = x + y - 0.5f; break;
        case FFS_ADDSIGNED2X: r.v[k] = (x + y - 0.5f) * 2.0f; break;
        case FFS_SUBTRACT: r.v[k] = x - y; break;
        case FFS_ADDSMOOTH: r.v[k] = x + y - x * y; break;
        case FFS_BLENDDIFFUSEALPHA: blend = in.diffuse.v[3]; r.v[k] = x * blend + y * (1.0f - blend); break;
        case FFS_BLENDTEXTUREALPHA: blend = texel.v[3]; r.v[k] = x * blend + y * (1.0f - blend); break;
        case FFS_BLENDFACTORALPHA: blend = in.factor.v[3]; r.v[k] = x * blend + y * (1.0f - blend); break;
        case FFS_BLENDCURRENTALPHA: blend = current.v[3]; r.v[k] = x * blend + y * (1.0f - blend); break;
        case FFS_BLENDTEXTUREALPHAPM: r.v[k] = x + y * (1.0f - texel.v[3]); break;
        case FFS_MODULATEALPHA_ADDCOLOR: r.v[k] = x + xa * y; break;
        case FFS_MODULATECOLOR_ADDALPHA: r.v[k] = x * y + xa; break;
        case FFS_MODULATEINVALPHA_ADDCOLOR: r.v[k] = (1.0f - xa) * y + x; break;
        case FFS_MODULATEINVCOLOR_ADDALPHA: r.v[k] = (1.0f - x) * y + xa; break;
        case FFS_DOTPRODUCT3: r.v[k] = 4.0f * dot; break;
        case FFS_MULTIPLYADD: r.v[k] = z + x * y; break;
        case FFS_LERP: r.v[k] = z * x + (1.0f - z) * y; break;
        default: r.v[k] = x; break;
      }
    }
    return r;
  }
};

Colour reference(const FfsKey& key, const Inputs& in) {
  Reference r{in, in.diffuse, {{0, 0, 0, 0}}, {{0, 0, 0, 0}}};
  for (unsigned n = 0; n < key.stages; ++n) {
    const FfsStage& s = key.stage[n];
    r.stage = n;
    const Colour& at = in.coordinate[n];
    r.texel = texel_of(n, at.v[0] / at.v[3], at.v[1] / at.v[3]);
    const Colour colour = r.half(s.colour_op, s.colour_arg, false), alpha = r.half(s.alpha_op, s.alpha_arg, true);
    const Colour result = {{clamp01(colour.v[0]), clamp01(colour.v[1]), clamp01(colour.v[2]), clamp01(alpha.v[3])}};
    (s.to_temp ? r.temp : r.current) = result;
  }
  if (key.specular) for (int k = 0; k < 3; ++k) r.current.v[k] += in.specular.v[k];
  return r.current;
}

// ---- the shader of a key, run ------------------------------------------------------------------------------------------------------------------------

struct Program {
  UniquePointer<D3d9Program> program;
  unsigned float_registers = 0, state_register = 0, fill_register = 0, samplers = 0;
};
bool make(HostAllocator& allocator, const FfsKey& key, Program* out) {
  uint32_t tokens[FFS_MAX_TOKENS];
  const size_t count = ffs_tokens(&key, tokens, FFS_MAX_TOKENS);
  if (!count || sm_translate(tokens, count, &g_shader, &g_context) != SM_OK || g_shader.vertex || g_shader.major != 2) return false;
  if (g_shader.float_registers > FFS_CONSTANTS || g_shader.tokens != count) return false;
  if (D3d9Program::compile_dxbc(allocator, ConstByteView(g_shader.dxbc, g_shader.dxbc_size), &out->program) != status::kSuccess || !out->program) return false;
  out->float_registers = g_shader.float_registers;
  out->state_register = g_shader.state_register;
  out->fill_register = g_shader.fill_register;
  out->samplers = g_shader.sampler_mask;
  return true;
}
Colour run(const Program& p, const Inputs& in) {
  static uint8_t constants[4096];
  memset(constants, 0, sizeof constants);
  // As draw.inc lays them out: only the float registers the shader reads (the factor c0, the stages' constants c2 ..), then its state registers, then
  // one fill register a sampler (what its texture lacks of the channels read as 1: none, the test's textures have all four). A shader that reads
  // fewer floats has its state and fill registers where the later constants would be, so those must not be written there.
  float floats[FFS_CONSTANTS][4] = {};
  memcpy(floats[0], in.factor.v, 16);
  for (unsigned n = 0; n < FFS_STAGES; ++n) memcpy(floats[2 + n], in.constant[n].v, 16);
  memcpy(constants, floats, (p.float_registers < FFS_CONSTANTS ? p.float_registers : FFS_CONSTANTS) * 16u);
  float state[SM_STATE_REGISTERS][4];
  sm_state_none(state);
  memcpy(constants + p.state_register * 16, state, sizeof state);
  for (unsigned n = 0; p.samplers >> n; ++n) memset(constants + (p.fill_register + n) * 16, 0, 16);
  graphics::Vec4 varyings[graphics::kMaxVaryings] = {};
  varyings[0] = {in.diffuse.v[0], in.diffuse.v[1], in.diffuse.v[2], in.diffuse.v[3]};
  varyings[1] = {in.specular.v[0], in.specular.v[1], in.specular.v[2], in.specular.v[3]};
  for (unsigned n = 0; n < FFS_STAGES; ++n) varyings[2 + n] = {in.coordinate[n].v[0], in.coordinate[n].v[1], in.coordinate[n].v[2], in.coordinate[n].v[3]};
  varyings[10] = {1, 0, 0, 0};      // (the fog factor: none)
  graphics::Vec4 colour[graphics::kMaxColorTargets] = {};
  Textures textures;
  const bool alive = p.program->run_fragment(varyings, constants, &textures, &colour[0]);
  if (!alive) return {{-1, -1, -1, -1}};
  return {{colour[0].x, colour[0].y, colour[0].z, colour[0].w}};
}

uint32_t g_seed = 0x2468ACE1u;
uint32_t next() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }
float unit() { return static_cast<float>(next() & 0xFFFF) / 65535.0f; }
Inputs inputs() {
  Inputs in;
  for (float& x : in.diffuse.v) x = unit();
  for (float& x : in.specular.v) x = unit();
  for (float& x : in.factor.v) x = unit();
  for (Colour& c : in.constant) for (float& x : c.v) x = unit();
  for (Colour& c : in.coordinate) c = {{unit() * 2.0f - 0.5f, unit() * 2.0f - 0.5f, 0.0f, (next() & 1) ? 1.0f : 0.5f + unit() * 2.0f}};
  return in;
}
bool same(const Colour& a, const Colour& b) {
  for (int k = 0; k < 4; ++k)
    if (fabsf(clamp01(a.v[k]) - clamp01(b.v[k])) > 2e-4f) return false;
  return true;
}
// Whether the shader of the key gives what the stages give, on `sets` sets of inputs.
bool agrees(HostAllocator& allocator, const FfsKey& key, unsigned sets = 4) {
  Program p;
  if (!make(allocator, key, &p)) { printf("  a key's shader is not made\n"); return false; }
  for (unsigned n = 0; n < FFS_STAGES; ++n)
    if (((p.samplers >> n & 1) != 0) != (n < key.stages && key.stage[n].sample)) { printf("  samplers %x\n", p.samplers); return false; }
  for (unsigned set = 0; set < sets; ++set) {
    const Inputs in = inputs();
    const Colour got = run(p, in), want = reference(key, in);
    if (!same(got, want)) {
      printf("  got %g %g %g %g, the stages give %g %g %g %g\n", got.v[0], got.v[1], got.v[2], got.v[3], want.v[0], want.v[1], want.v[2], want.v[3]);
      return false;
    }
  }
  return true;
}

// The stage states a device starts with.
FfsKey initial() {
  FfsKey key = {};
  for (unsigned n = 0; n < FFS_STAGES; ++n) {
    FfsStage& s = key.stage[n];
    s.colour_op = n == 0 ? FFS_MODULATE : FFS_DISABLE;
    s.alpha_op = n == 0 ? FFS_SELECTARG1 : FFS_DISABLE;
    s.colour_arg[0] = s.alpha_arg[0] = FFS_CURRENT;
    s.colour_arg[1] = s.alpha_arg[1] = FFS_TEXTURE;
    s.colour_arg[2] = s.alpha_arg[2] = FFS_CURRENT;
  }
  return key;
}
void set_stage(FfsKey* key, unsigned n, unsigned colour_op, unsigned c1, unsigned c2, unsigned alpha_op, unsigned a1, unsigned a2) {
  FfsStage& s = key->stage[n];
  s.colour_op = static_cast<uint8_t>(colour_op); s.colour_arg[1] = static_cast<uint8_t>(c1); s.colour_arg[2] = static_cast<uint8_t>(c2);
  s.alpha_op = static_cast<uint8_t>(alpha_op); s.alpha_arg[1] = static_cast<uint8_t>(a1); s.alpha_arg[2] = static_cast<uint8_t>(a2);
}

void test_written(HostAllocator& allocator) {
  // as a device starts: with a texture the texture times the diffuse colour, with the texture's alpha; without, the diffuse colour
  {
    FfsKey key = initial();
    ffs_normalize(&key, 0x1);
    CHECK(key.stages == 1 && key.stage[0].sample && key.stage[0].colour_op == FFS_MODULATE && agrees(allocator, key));
    const Inputs in = inputs();
    Program p;
    CHECK(make(allocator, key, &p));
    const Colour got = run(p, in), t = texel_of(0, in.coordinate[0].v[0] / in.coordinate[0].v[3], in.coordinate[0].v[1] / in.coordinate[0].v[3]);
    CHECK(same(got, {{t.v[0] * in.diffuse.v[0], t.v[1] * in.diffuse.v[1], t.v[2] * in.diffuse.v[2], t.v[3]}}));
    key = initial();
    ffs_normalize(&key, 0x0);
    CHECK(key.stages == 1 && !key.stage[0].sample && key.stage[0].colour_op == FFS_SELECTARG1 && key.stage[0].colour_arg[1] == FFS_CURRENT);
    CHECK(make(allocator, key, &p) && p.samplers == 0 && same(run(p, in), in.diffuse));
    // ... and the specular colour added
    key = initial();
    key.specular = 7;
    ffs_normalize(&key, 0x0);
    CHECK(key.specular == 1 && make(allocator, key, &p));
    CHECK(same(run(p, in), {{in.diffuse.v[0] + in.specular.v[0], in.diffuse.v[1] + in.specular.v[1], in.diffuse.v[2] + in.specular.v[2], in.diffuse.v[3]}}));
    // no stage at all: the diffuse colour
    key = initial();
    key.stage[0].colour_op = FFS_DISABLE;
    ffs_normalize(&key, 0xFF);
    CHECK(key.stages == 0 && make(allocator, key, &p) && p.samplers == 0 && same(run(p, in), in.diffuse));
  }
  // every operation, for the colour and for the alpha, with every kind of argument
  for (unsigned op = FFS_SELECTARG1; op <= FFS_LERP; ++op) {
    for (unsigned first = FFS_DIFFUSE; first <= FFS_CONSTANT; ++first) {
      FfsKey key = initial();
      set_stage(&key, 0, FFS_SELECTARG1, FFS_TEXTURE, 0, FFS_SELECTARG1, FFS_TEXTURE, 0);
      key.stage[0].to_temp = 1;      // (so that the temporary register holds something)
      set_stage(&key, 1, op, first, FFS_TEXTURE | (first & 1 ? FFS_COMPLEMENT : 0), FFS_MODULATE, FFS_DIFFUSE, FFS_TEXTURE);
      key.stage[1].colour_arg[0] = FFS_TFACTOR | (first & 2 ? FFS_ALPHAREPLICATE : 0);
      set_stage(&key, 2, FFS_ADD, FFS_CURRENT, FFS_SPECULAR, op, first | (first & 2 ? FFS_COMPLEMENT : 0), FFS_TEXTURE);
      key.stage[2].alpha_arg[0] = FFS_CONSTANT;
      ffs_normalize(&key, 0x7);
      const bool passes_on = op == FFS_PREMODULATE || op == FFS_BUMPENVMAP || op == FFS_BUMPENVMAPLUMINANCE;
      CHECK(key.stages == 3 && (passes_on ? key.stage[1].colour_op == FFS_SELECTARG1 : key.stage[1].colour_op == op));
      const bool ok = agrees(allocator, key);
      if (!ok) printf("  operation %u, argument %u\n", op, first);
      CHECK(ok);
    }
  }
  // eight stages, each adding a part of its texture; and the temporary register carried over the stages between
  {
    FfsKey key = initial();
    for (unsigned n = 0; n < FFS_STAGES; ++n) set_stage(&key, n, n & 1 ? FFS_ADDSMOOTH : FFS_BLENDTEXTUREALPHA, FFS_TEXTURE, FFS_CURRENT, FFS_ADDSIGNED, FFS_TEXTURE, FFS_CURRENT);
    ffs_normalize(&key, 0xFF);
    Program p;
    CHECK(key.stages == 8 && make(allocator, key, &p) && p.samplers == 0xFF && agrees(allocator, key, 8));
    key = initial();
    set_stage(&key, 0, FFS_SELECTARG1, FFS_TEXTURE, 0, FFS_SELECTARG1, FFS_TEXTURE, 0);
    key.stage[0].to_temp = 1;
    for (unsigned n = 1; n < 7; ++n) set_stage(&key, n, FFS_MODULATE2X, FFS_TEXTURE, FFS_CURRENT, FFS_SELECTARG2, 0, FFS_CURRENT);
    set_stage(&key, 7, FFS_LERP, FFS_TEMP, FFS_CURRENT, FFS_SELECTARG1, FFS_TEMP, 0);
    key.stage[7].colour_arg[0] = FFS_TFACTOR;
    ffs_normalize(&key, 0x7F);
    CHECK(key.stages == 8 && !key.stage[7].sample && agrees(allocator, key, 8));
    // a temporary register that is read and never written is nothing
    key = initial();
    set_stage(&key, 0, FFS_ADD, FFS_TEMP, FFS_DIFFUSE, FFS_SELECTARG1, FFS_TEMP | FFS_COMPLEMENT, 0);
    ffs_normalize(&key, 0);
    const Inputs in = inputs();
    CHECK(make(allocator, key, &p) && same(run(p, in), {{in.diffuse.v[0], in.diffuse.v[1], in.diffuse.v[2], 1.0f}}));
  }
}

void test_key() {
  // the stages end at the first that is switched off, whatever comes after
  FfsKey a = initial(), b = initial();
  set_stage(&a, 1, FFS_ADD, FFS_TEXTURE, FFS_CURRENT, FFS_ADD, FFS_TEXTURE, FFS_CURRENT);
  set_stage(&a, 3, FFS_ADD, FFS_TEXTURE, FFS_CURRENT, FFS_ADD, FFS_TEXTURE, FFS_CURRENT);
  ffs_normalize(&a, 0xFF);
  CHECK(a.stages == 2 && a.stage[3].colour_op == 0);
  // what an operation does not read is not in the key
  a = initial();
  set_stage(&a, 0, FFS_SELECTARG2, FFS_TEXTURE | FFS_COMPLEMENT, FFS_DIFFUSE, FFS_SELECTARG1, FFS_TFACTOR, FFS_TEXTURE);
  set_stage(&b, 0, FFS_SELECTARG2, FFS_SPECULAR, FFS_DIFFUSE, FFS_SELECTARG1, FFS_TFACTOR, FFS_TEMP | FFS_ALPHAREPLICATE);
  a.stage[0].colour_arg[0] = FFS_TEMP;
  a.stage[5].colour_arg[1] = 99;
  a.specular = 1; b.specular = 0x80;
  ffs_normalize(&a, 0x1);
  ffs_normalize(&b, 0xFE);
  CHECK(memcmp(&a, &b, sizeof a) == 0 && !a.stage[0].sample);
  // a stage with no texture passes on what it is given - each half by itself - and the stages after it go on
  a = initial();
  set_stage(&a, 0, FFS_BLENDTEXTUREALPHA, FFS_DIFFUSE, FFS_TFACTOR, FFS_MODULATE, FFS_DIFFUSE, FFS_TFACTOR);
  set_stage(&a, 1, FFS_ADD, FFS_TEXTURE, FFS_CURRENT, FFS_SELECTARG2, FFS_TEXTURE, FFS_CURRENT);
  ffs_normalize(&a, 0x2);
  CHECK(a.stages == 2 && a.stage[0].colour_op == FFS_SELECTARG1 && a.stage[0].colour_arg[1] == FFS_CURRENT && a.stage[0].alpha_op == FFS_MODULATE && !a.stage[0].sample);
  CHECK(a.stage[1].colour_op == FFS_ADD && a.stage[1].sample && a.stage[1].alpha_arg[1] == 0);
  // an alpha operation that is switched off, an operation that is none, an argument that is none
  a = initial();
  set_stage(&a, 0, FFS_ADD, 9, FFS_DIFFUSE, FFS_DISABLE, FFS_TEXTURE, FFS_TEXTURE);
  set_stage(&a, 1, 200, FFS_TEXTURE, FFS_CURRENT, 77, FFS_TEXTURE, FFS_CURRENT);
  ffs_normalize(&a, 0x3);
  CHECK(a.stages == 1 && a.stage[0].colour_arg[1] == FFS_CURRENT && a.stage[0].alpha_op == FFS_SELECTARG1 && a.stage[0].alpha_arg[1] == FFS_CURRENT && !a.stage[0].sample);
}

void test_random(HostAllocator& allocator) {
  unsigned made = 0, stages = 0;
  for (unsigned round = 0; round < 3000; ++round) {
    FfsKey key = {};
    key.specular = next() & 1;
    const unsigned length = 1 + next() % FFS_STAGES;
    for (unsigned n = 0; n < FFS_STAGES; ++n) {
      FfsStage& s = key.stage[n];
      s.colour_op = static_cast<uint8_t>(n < length ? static_cast<unsigned>(FFS_SELECTARG1) + next() % (FFS_LERP - 1) : static_cast<unsigned>(FFS_DISABLE));
      s.alpha_op = static_cast<uint8_t>(FFS_DISABLE + next() % FFS_LERP);
      for (unsigned k = 0; k < 3; ++k) {
        s.colour_arg[k] = static_cast<uint8_t>(next() % 7 | (next() % 4 == 0 ? FFS_COMPLEMENT : 0) | (next() % 4 == 0 ? FFS_ALPHAREPLICATE : 0));
        s.alpha_arg[k] = static_cast<uint8_t>(next() % 7 | (next() % 4 == 0 ? FFS_COMPLEMENT : 0) | (next() % 4 == 0 ? FFS_ALPHAREPLICATE : 0));
      }
      s.to_temp = next() % 5 == 0;
    }
    const unsigned textured = next() % 8 == 0 ? next() & 0xFF : 0xFF;
    ffs_normalize(&key, textured);
    const FfsKey again = key;
    FfsKey twice = key;
    ffs_normalize(&twice, textured);
    CHECK(memcmp(&again, &twice, sizeof again) == 0);      // (a key is what ffs_normalize leaves alone)
    const bool ok = agrees(allocator, key, 3);
    if (!ok) {
      printf("  round %u: %u stages\n", round, key.stages);
      for (unsigned n = 0; n < key.stages; ++n) {
        const FfsStage& s = key.stage[n];
        printf("    %u: colour %u (%x %x %x) alpha %u (%x %x %x)%s\n", n, s.colour_op, s.colour_arg[0], s.colour_arg[1], s.colour_arg[2], s.alpha_op, s.alpha_arg[0],
               s.alpha_arg[1], s.alpha_arg[2], s.to_temp ? " to the temporary register" : "");
      }
    }
    CHECK(ok);
    ++made;
    stages += key.stages;
  }
  printf("  %u stage set-ups picked at random (%u stages) give what the stages give\n", made, stages);
}

void test_damage(HostAllocator& allocator) {
  unsigned written = 0, taken = 0, refused = 0;
  // keys nothing has looked at: tokens inside the buffer, which the translator takes or refuses
  for (unsigned round = 0; round < 4000; ++round) {
    FfsKey key;
    uint8_t bytes[sizeof key];
    for (uint8_t& b : bytes) b = static_cast<uint8_t>(next());
    memcpy(&key, bytes, sizeof key);
    std::vector<uint32_t> tokens(FFS_MAX_TOKENS);
    const size_t count = ffs_tokens(&key, tokens.data(), tokens.size());
    CHECK(count > 0 && count <= FFS_MAX_TOKENS);
    if (!count) continue;
    ++written;
    const std::vector<uint32_t> exact(tokens.begin(), tokens.begin() + static_cast<ptrdiff_t>(count));      // (exactly that long: nothing is read past it)
    if (sm_translate(exact.data(), exact.size(), &g_shader, &g_context) == SM_OK) {
      UniquePointer<D3d9Program> program;
      (void)D3d9Program::compile_dxbc(allocator, ConstByteView(g_shader.dxbc, g_shader.dxbc_size), &program);
      ++taken;
    }
  }
  // a buffer that is too small: nothing, and nothing written past it; a shortened shader is refused; one with a bit turned is taken or refused
  FfsKey key = initial();
  for (unsigned n = 0; n < FFS_STAGES; ++n) {
    set_stage(&key, n, FFS_DOTPRODUCT3, FFS_TEXTURE | FFS_COMPLEMENT, FFS_CURRENT | FFS_COMPLEMENT, FFS_ADDSIGNED2X, FFS_TEXTURE | FFS_COMPLEMENT, FFS_TFACTOR | FFS_COMPLEMENT);
  }
  ffs_normalize(&key, 0xFF);
  uint32_t whole[FFS_MAX_TOKENS];
  const size_t count = ffs_tokens(&key, whole, FFS_MAX_TOKENS);
  CHECK(count > 100 && count <= FFS_MAX_TOKENS);
  for (size_t capacity = 0; capacity < count; ++capacity) {
    std::vector<uint32_t> small(capacity);
    CHECK(ffs_tokens(&key, small.data(), capacity) == 0);
    const std::vector<uint32_t> part(whole, whole + capacity);
    CHECK(sm_translate(part.data(), part.size(), &g_shader, &g_context) != SM_OK);
    ++refused;
  }
  std::vector<uint32_t> damaged(whole, whole + count);
  for (unsigned round = 0; round < 3000; ++round) {
    const size_t at = next() % damaged.size();
    const uint32_t bit = 1u << (next() & 31);
    damaged[at] ^= bit;
    if (sm_translate(damaged.data(), damaged.size(), &g_shader, &g_context) == SM_OK) {
      UniquePointer<D3d9Program> program;
      (void)D3d9Program::compile_dxbc(allocator, ConstByteView(g_shader.dxbc, g_shader.dxbc_size), &program);
    }
    damaged[at] ^= bit;
  }
  printf("  %u keys of any bytes written (%u translated); the longest shader here has %zu tokens, and %u shorter buffers and shaders are refused\n", written, taken, count, refused);
}

// A colour-keyed texture (the stage's `kill`, ddraw.dll's colour key): a texel of alpha 0 takes the pixel away, whatever the stages make of the alpha;
// a texel of alpha 1/255 (the least a keyed texture's other texels have) does not, even where the alpha the stages give is 0.
void test_kill(HostAllocator& allocator) {
  FfsKey key = initial();
  set_stage(&key, 0, FFS_MODULATE, FFS_TEXTURE, FFS_DIFFUSE, FFS_MODULATE, FFS_TEXTURE, FFS_DIFFUSE);
  key.stage[0].kill = 1;
  FfsKey untextured = key;
  ffs_normalize(&untextured, 0x0);
  CHECK(!untextured.stage[0].kill && !untextured.stage[0].sample);      // (no texture: nothing to key)
  ffs_normalize(&key, 0x1);
  CHECK(key.stages == 1 && key.stage[0].kill && key.stage[0].sample);
  Program p;
  CHECK(make(allocator, key, &p));
  if (!p.program) return;
  for (unsigned set = 0; set < 16; ++set) {
    Inputs in = inputs();
    if (set & 1) in.diffuse.v[3] = 0.0f;      // (MODULATE of the alpha gives 0: Wine's test_colorkey_precision)
    g_texel_alpha = 0.0f;
    CHECK(run(p, in).v[0] == -1.0f);
    g_texel_alpha = 1.0f / 255.0f;
    CHECK(same(run(p, in), reference(key, in)));
    g_texel_alpha = 1.0f;
    CHECK(same(run(p, in), reference(key, in)));
  }
  g_texel_alpha = -1.0f;
  // a key without kill has no texkill: a texel of alpha 0 is drawn
  FfsKey plain = initial();
  ffs_normalize(&plain, 0x1);
  Program q;
  CHECK(make(allocator, plain, &q));
  if (!q.program) return;
  g_texel_alpha = 0.0f;
  const Inputs in = inputs();
  CHECK(run(q, in).v[0] != -1.0f);
  g_texel_alpha = -1.0f;
}

}  // namespace

int main() {
  HostAllocator allocator;
  test_written(allocator);
  test_kill(allocator);
  test_key();
  test_random(allocator);
  test_damage(allocator);
  CHECK(allocator.live == 0);
  if (g_failed) {
    printf("FfStagesTest: %d of %d checks failed\n", g_failed, g_checks);
    return 1;
  }
  printf("FfStagesTest: all %d checks passed\n", g_checks);
  return 0;
}
