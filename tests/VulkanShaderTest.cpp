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

// Host-side tests for the SPIR-V to DXBC translator of vulkan-1.dll (src/Vulkan/spirv.inc): shaders compiled by glslc (tests/data/spirv, the GLSL next to the SPIR-V) are
// translated, taken by the kernel's DXBC front end, run by its shader interpreter and compared with a C++ model of the same GLSL. Truncated and bit-flipped modules must be refused
// or translated, never crash (this runs under ASan and UBSan).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Kernel/Dxgkrnl/D3d9Shader.hpp"
#include "Utility/ByteStream.hpp"

extern "C" {
#include "spirv.h"
}

using namespace dxgk;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

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

void* sp_alloc_zero(void*, unsigned long long n) { return calloc(1, n); }
void sp_release(void*, void* p) { free(p); }

std::vector<uint32_t> read_spirv(const char* name) {
  char path[256];
  snprintf(path, sizeof path, "tests/data/spirv/%s", name);
  FILE* f = fopen(path, "rb");
  std::vector<uint32_t> words;
  if (!f) {
    printf("cannot open %s\n", path);
    ++g_failed;
    return words;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  words.resize(static_cast<size_t>(n) / 4);
  if (fread(words.data(), 4, words.size(), f) != words.size()) words.clear();
  fclose(f);
  return words;
}

// One translated shader: the DXBC, and the layout of the constant buffers it was given (slot n at the sum of the sizes before it). Storage buffers are numbered as vulkan-1.dll does:
// the read-only ones t0.. (the kernel's slots after the unordered access ones), the others u0..; `storage_slot` is the kernel's slot of each binding.
struct Translated {
  std::vector<uint8_t> dxbc;
  std::vector<uint32_t> cb_vec4;
  std::vector<uint32_t> binding_of_slot;
  uint32_t storage_slot[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
  uint32_t local_size[3] = {1, 1, 1};
  std::string error;
  bool ok = false;
};

Translated translate(const std::vector<uint32_t>& words, uint32_t stage, const char* entry = nullptr, const uint32_t* spec_ids = nullptr, const uint32_t* spec_values = nullptr,
                     uint32_t spec_count = 0) {
  Translated t;
  SpvAllocator a = {sp_alloc_zero, sp_release, nullptr};
  SpvModule* m = spv_open(words.data(), static_cast<uint32_t>(words.size()), &a);
  if (!m) {
    t.error = "open";
    return t;
  }
  if (spv_reflect(m, stage, entry, spec_ids, spec_values, spec_count)) {
    t.error = spv_error(m);
    spv_close(m);
    return t;
  }
  uint32_t slot = 0, unit = 0, uav = 0, srv = 0;
  for (uint32_t i = 0; i < spv_resource_count(m); ++i) {
    SpvResource* r = spv_resource(m, i);
    if (r->kind == SPV_RES_STORAGE) {
      r->uav = !r->readonly;
      r->slot = r->uav ? uav++ : srv++;
      if (r->binding < 8) t.storage_slot[r->binding] = r->uav ? r->slot : graphics::kMaxUavBuffers + r->slot;
    } else if (r->kind == SPV_RES_UNIFORM || r->kind == SPV_RES_PUSH) {
      r->slot = slot++;
      r->cb_vec4 = (r->bytes + 15) / 16;
      spv_declare_cb(m, r->slot, r->cb_vec4);
      t.cb_vec4.push_back(r->cb_vec4);
      t.binding_of_slot.push_back(r->binding);
    } else {
      r->slot = unit++;
    }
  }
  spv_local_size(m, t.local_size);
  const uint8_t* out = nullptr;
  uint32_t size = 0;
  if (spv_emit(m, &out, &size)) t.error = spv_error(m);
  else t.dxbc.assign(out, out + size), t.ok = true;
  spv_close(m);
  return t;
}

UniquePointer<D3d9Program> compile(HostAllocator& allocator, const Translated& t, NtStatus* status_out = nullptr) {
  UniquePointer<D3d9Program> p;
  NtStatus s = D3d9Program::compile_dxbc(allocator, ConstByteView(t.dxbc.data(), t.dxbc.size()), &p);
  if (status_out) *status_out = s;
  return p;
}

struct V4 {
  float x, y, z, w;
};

// A fake texture unit 0: the colour is a function of the coordinates.
class FakeTextures final : public graphics::ITextureSet {
 public:
  graphics::Vec4 sample(uint32_t unit, float u, float v) const override { return {u, v, 0.5f, 0.75f}; }
};

void put_floats(std::vector<uint8_t>& block, uint32_t offset, const float* f, uint32_t n) {
  if (block.size() < offset + n * 4) block.resize(offset + n * 4);
  memcpy(block.data() + offset, f, n * 4);
}
void put_u32(std::vector<uint8_t>& block, uint32_t offset, uint32_t v) {
  if (block.size() < offset + 4) block.resize(offset + 4);
  memcpy(block.data() + offset, &v, 4);
}

float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
float smoothstep(float e0, float e1, float x) {
  float t = clampf((x - e0) / (e1 - e0), 0, 1);
  return t * t * (3 - 2 * t);
}

void test_basic() {
  HostAllocator allocator;
  const std::vector<uint32_t> vs_words = read_spirv("basic.vert.spv"), fs_words = read_spirv("basic.frag.spv");
  Translated vs = translate(vs_words, SPV_STAGE_VERTEX), fs = translate(fs_words, SPV_STAGE_FRAGMENT);
  if (!vs.ok) printf("  vertex shader: %s\n", vs.error.c_str());
  if (!fs.ok) printf("  fragment shader: %s\n", fs.error.c_str());
  CHECK(vs.ok && fs.ok);
  if (!vs.ok || !fs.ok) return;
  NtStatus s = 0;
  UniquePointer<D3d9Program> pv = compile(allocator, vs, &s);
  CHECK(s == status::kSuccess && pv && pv->stage() == graphics::ShaderStage::Vertex);
  UniquePointer<D3d9Program> pf = compile(allocator, fs, &s);
  CHECK(s == status::kSuccess && pf && pf->stage() == graphics::ShaderStage::Fragment);
  if (!pv || !pf) return;

  // The vertex shader: gl_Position = mvp * (pos, 1); the kernel flips Y for the device and the translated shader has flipped it before, so the position comes out as GLSL wrote it.
  const float mvp[16] = {1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0.5f, 0, 0.25f, -0.5f, 0.125f, 1};   // columns
  const float tint[4] = {1, 0.5f, 0.25f, 2};
  std::vector<uint8_t> cb;
  put_floats(cb, 0, mvp, 16);
  put_floats(cb, 64, tint, 4);
  cb.resize(vs.cb_vec4[0] * 16);
  graphics::Vec4 attributes[4] = {{1, 2, 3, 1}, {0.5f, 0.25f, 0.125f, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
  graphics::ShaderIo io = {};
  pv->run_vertex(attributes, cb.data(), &io);
  const float px = 1 * 1 + 0 * 2 + 0 * 3 + 0.25f, py = 2 * 2 - 0.5f, pz = 0.5f * 3 + 0.125f, pw = 1;
  CHECK(fabsf(io.position.x - px) < 1e-5f && fabsf(io.position.y - py) < 1e-5f && fabsf(io.position.z - pz) < 1e-5f && fabsf(io.position.w - pw) < 1e-5f);
  CHECK(fabsf(io.varying[0].x - 0.5f) < 1e-6f && fabsf(io.varying[0].y - 0.125f) < 1e-6f && fabsf(io.varying[0].z - 0.03125f) < 1e-6f && fabsf(io.varying[0].w - 2) < 1e-6f);
  CHECK(fabsf(io.varying[1].x - 1.0f) < 1e-6f && fabsf(io.varying[1].y - 1.5f) < 1e-6f);

  // The fragment shader: a texture lookup at uv, discard under a threshold, colour times texture times scale. cb slot 0 is the push constant block.
  CHECK(fs.cb_vec4.size() == 1 && fs.cb_vec4[0] == 2);
  std::vector<uint8_t> pc(32);
  const float scale[4] = {2, 1, 0.5f, 1};
  put_floats(pc, 0, scale, 4);
  const float cut = 0.5f;
  put_floats(pc, 16, &cut, 1);
  FakeTextures textures;
  graphics::Vec4 varyings[4] = {{0.5f, 0.25f, 1, 1}, {0.25f, 0.5f, 0, 0}, {}, {}};
  graphics::Vec4 color[4] = {};
  const bool kept = pf->run_fragment(varyings, pc.data(), &textures, color);
  CHECK(kept);
  CHECK(fabsf(color[0].x - 0.5f * 0.25f * 2) < 1e-5f && fabsf(color[0].y - 0.25f * 0.5f * 1) < 1e-5f && fabsf(color[0].z - 1 * 0.5f * 0.5f) < 1e-5f && fabsf(color[0].w - 0.75f) < 1e-5f);
  const float high_cut = 0.9f;
  put_floats(pc, 16, &high_cut, 1);
  CHECK(!pf->run_fragment(varyings, pc.data(), &textures, color));   // alpha 0.75 < 0.9: discarded
}

struct MathInputs {
  V4 v;
  V4 a, b[4];
  float m[9];
  float f;
  int n;
};

float helper(float x) {
  if (x > 1) return x * 2;
  if (x < -1) return -x;
  return x + 0.5f;
}

V4 math_model(const MathInputs& u) {
  float nx = u.v.x + u.a.x, ny = u.v.y + u.a.y, nz = u.v.z + u.a.z;
  float len = sqrtf(nx * nx + ny * ny + nz * nz);
  nx /= len, ny /= len, nz /= len;
  float s = 0;
  for (int i = 0; i < 4; ++i) {
    if (i == 2) continue;
    s += u.b[i].x * u.v.x + u.b[i].y * u.v.y + u.b[i].z * u.v.z + u.b[i].w * u.v.w;
  }
  float h = helper(s);
  float w[3];
  for (int r = 0; r < 3; ++r) w[r] = u.m[0 * 3 + r] * nx + u.m[1 * 3 + r] * ny + u.m[2 * 3 + r] * nz;
  float t = u.v.w > 0.5f ? h : -h;
  unsigned q = static_cast<unsigned>(u.n) % 3u;
  float sel = q == 0 ? 1.0f : q == 1 ? 2.0f : 3.0f;
  int idx = static_cast<int>(u.v.x) & 3;
  float dyn = (&u.b[idx].x)[1];
  float acc = 0;
  for (int j = 0; j < 8; ++j) {
    acc += static_cast<float>(j) * 0.25f;
    if (acc > u.f) break;
  }
  float c = clampf(t, -4, 4);
  float a = 0.25f + (0.75f - 0.25f) * smoothstep(0, 1, u.v.w);
  return {w[0] * c, w[1] * c, w[2] * c, a + sel * 0.01f + dyn * 0.001f + acc * 0.0001f + powf(fabsf(u.v.y) + 0.5f, 2.0f) * 0.00001f};
}

void run_math(const char* what, D3d9Program& p, const Translated& tr) {
  uint32_t wrong = 0, total = 0;
  for (int set = 0; set < 24; ++set) {
    MathInputs u = {};
    float seed = static_cast<float>(set);
    u.v = {fmodf(seed * 0.37f, 3.0f) - 0.4f, fmodf(seed * 0.61f, 2.0f) - 0.9f, fmodf(seed * 0.17f, 1.0f) + 0.1f, set % 3 == 0 ? 0.2f : set % 3 == 1 ? 0.8f : 0.5f};
    u.a = {0.3f, -0.2f, 0.9f, 0};
    for (int i = 0; i < 4; ++i) u.b[i] = {0.1f * (i + 1), -0.2f * i, 0.3f, 0.05f * seed};
    for (int i = 0; i < 9; ++i) u.m[i] = 0.1f * (i + 1) - (i % 2 ? 0.3f : 0);
    u.f = 0.4f + 0.3f * (set % 5);
    u.n = set;
    std::vector<uint8_t> cb;
    put_floats(cb, 0, &u.a.x, 4);
    for (int i = 0; i < 4; ++i) put_floats(cb, 16 + 16 * i, &u.b[i].x, 4);
    for (int c = 0; c < 3; ++c) put_floats(cb, 80 + 16 * c, &u.m[c * 3], 3);
    put_floats(cb, 128, &u.f, 1);
    put_u32(cb, 132, static_cast<uint32_t>(u.n));
    cb.resize(tr.cb_vec4[0] * 16);
    graphics::Vec4 varyings[4] = {{u.v.x, u.v.y, u.v.z, u.v.w}, {}, {}, {}};
    graphics::Vec4 color[4] = {};
    const bool kept = p.run_fragment(varyings, cb.data(), nullptr, color);
    const V4 expected = math_model(u);
    ++total;
    const float tol = 2e-3f;
    if (!kept || fabsf(color[0].x - expected.x) > tol * (1 + fabsf(expected.x)) || fabsf(color[0].y - expected.y) > tol * (1 + fabsf(expected.y)) ||
        fabsf(color[0].z - expected.z) > tol * (1 + fabsf(expected.z)) || fabsf(color[0].w - expected.w) > tol * (1 + fabsf(expected.w))) {
      if (wrong == 0)
        printf("  %s set %d: got %g %g %g %g, expected %g %g %g %g\n", what, set, color[0].x, color[0].y, color[0].z, color[0].w, expected.x, expected.y, expected.z, expected.w);
      ++wrong;
    }
  }
  printf("  %s: %u of %u wrong\n", what, wrong, total);
  CHECK(wrong == 0);
}

void test_math() {
  HostAllocator allocator;
  for (const char* name : {"math.frag.spv", "math_opt.frag.spv"}) {
    Translated t = translate(read_spirv(name), SPV_STAGE_FRAGMENT);
    if (!t.ok) printf("  %s: %s\n", name, t.error.c_str());
    CHECK(t.ok);
    if (!t.ok) continue;
    NtStatus s = 0;
    UniquePointer<D3d9Program> p = compile(allocator, t, &s);
    if (s != status::kSuccess) printf("  %s: the kernel refused the DXBC (0x%x)\n", name, s);
    CHECK(s == status::kSuccess && p);
    if (p) run_math(name, *p, t);
  }
}

// Modules cut short or with bits flipped: refused or translated, never a crash; whatever comes out is a container the kernel can take or refuse.
void test_fuzz() {
  HostAllocator allocator;
  uint32_t translated = 0, refused = 0;
  uint32_t seed = 12345;
  auto next = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  for (const char* name : {"basic.vert.spv", "basic.frag.spv", "math.frag.spv", "cs_add.comp.spv", "cs_shared_opt.comp.spv", "vk_ssbo.vert.spv", "cs_dyn_opt.comp.spv", "vk_cube_opt.frag.spv"}) {
    const std::vector<uint32_t> words = read_spirv(name);
    const uint32_t stage = strstr(name, ".vert") ? SPV_STAGE_VERTEX : strstr(name, ".comp") ? SPV_STAGE_COMPUTE : SPV_STAGE_FRAGMENT;
    for (uint32_t cut = 5; cut < words.size(); cut += 7) {
      std::vector<uint32_t> w(words.begin(), words.begin() + cut);
      Translated t = translate(w, stage);
      (t.ok ? translated : refused)++;
    }
    for (int round = 0; round < 400; ++round) {
      std::vector<uint32_t> w = words;
      const uint32_t flips = 1 + next() % 3;
      for (uint32_t k = 0; k < flips; ++k) w[5 + next() % (w.size() - 5)] ^= 1u << (next() % 32);
      Translated t = translate(w, stage);
      if (t.ok) {
        ++translated;
        UniquePointer<D3d9Program> p;
        D3d9Program::compile_dxbc(allocator, ConstByteView(t.dxbc.data(), t.dxbc.size()), &p);
      } else {
        ++refused;
      }
    }
  }
  printf("  fuzz: %u translated, %u refused\n", translated, refused);
  CHECK(translated + refused > 1000);
}

// ---- compute shaders -------------------------------------------------------------------------------------------------------------------------------------------------------

// Runs groups_x x 1 x 1 work groups the way the kernel's software device does: a thread at a time, or a whole group at a time for a program with barriers or group-shared memory.
void dispatch(const D3d9Program& p, uint32_t groups_x, std::vector<uint8_t>* const* buffers, const uint8_t* constants, const graphics::ITextureSet* textures = nullptr) {
  graphics::ComputeContext c;
  c.textures = textures;
  for (uint32_t k = 0; k < graphics::kMaxStorageBuffers; ++k)
    if (buffers[k]) c.buffers[k] = ByteView(buffers[k]->data(), buffers[k]->size());
  for (uint32_t g = 0; g < groups_x; ++g) {
    c.group_id[0] = g, c.group_id[1] = c.group_id[2] = 0;
    if (p.needs_group()) {
      p.run_compute_group(c, constants);
      continue;
    }
    uint32_t index = 0;
    for (uint32_t z = 0; z < p.thread_group(2); ++z)
      for (uint32_t y = 0; y < p.thread_group(1); ++y)
        for (uint32_t x = 0; x < p.thread_group(0); ++x, ++index) {
          c.thread_id[0] = x, c.thread_id[1] = y, c.thread_id[2] = z;
          c.dispatch_id[0] = g * p.thread_group(0) + x, c.dispatch_id[1] = y, c.dispatch_id[2] = z;
          c.group_index = index;
          p.run_compute(c, constants);
        }
  }
}

float get_float(const std::vector<uint8_t>& b, uint32_t offset) {
  float f;
  memcpy(&f, b.data() + offset, 4);
  return f;
}
uint32_t get_u32(const std::vector<uint8_t>& b, uint32_t offset) {
  uint32_t v;
  memcpy(&v, b.data() + offset, 4);
  return v;
}

UniquePointer<D3d9Program> compile_compute(HostAllocator& allocator, const char* name, Translated* t, const uint32_t* spec_ids = nullptr, const uint32_t* spec_values = nullptr,
                                           uint32_t spec_count = 0) {
  *t = translate(read_spirv(name), SPV_STAGE_COMPUTE, nullptr, spec_ids, spec_values, spec_count);
  if (!t->ok) printf("  %s: %s\n", name, t->error.c_str());
  CHECK(t->ok);
  if (!t->ok) return {};
  NtStatus s = 0;
  UniquePointer<D3d9Program> p = compile(allocator, *t, &s);
  if (s != status::kSuccess) printf("  %s: the kernel refused the DXBC (0x%x)\n", name, s);
  CHECK(s == status::kSuccess && p && p->stage() == graphics::ShaderStage::Compute);
  // What virgl gets: the program as TGSI text (VULKAN_SHADER_TGSI=1 prints it).
  if (p) {
    String tgsi(allocator);
    const NtStatus ts = p->emit_tgsi(&tgsi);
    CHECK(ts == status::kSuccess);
    if (getenv("VULKAN_SHADER_TGSI")) printf("--- %s TGSI ---\n%.*s\n", name, static_cast<int>(tgsi.size()), tgsi.data());
  }
  return p;
}

// Storage buffers read only and read-write with runtime arrays, push constants, a 4 x 2 work group, atomicAdd, signed division and modulo.
void test_compute_buffers() {
  HostAllocator allocator;
  for (const char* name : {"cs_add.comp.spv", "cs_add_opt.comp.spv"}) {
    Translated t;
    UniquePointer<D3d9Program> p = compile_compute(allocator, name, &t);
    if (!p) continue;
    CHECK(t.local_size[0] == 4 && t.local_size[1] == 2 && t.local_size[2] == 1);
    CHECK(t.storage_slot[0] == graphics::kMaxUavBuffers + 0 && t.storage_slot[1] == 0);   // (the input is NonWritable: a read-only view)
    std::vector<uint8_t> src(8 + 16 * 4), dst(36 + 16 * 4);
    const float offset[2] = {0.25f, 0.5f};
    put_floats(src, 0, offset, 2);
    for (uint32_t i = 0; i < 16; ++i) {
      const float a = i * 1.5f - 3.0f, junk = -7.0f;
      put_floats(src, 8 + 4 * i, &a, 1);
      put_floats(dst, 36 + 4 * i, &junk, 1);
    }
    std::vector<uint8_t> constants(16);
    const float scale = 2.0f;
    put_floats(constants, 0, &scale, 1);
    put_u32(constants, 4, 13);
    std::vector<uint8_t>* buffers[graphics::kMaxStorageBuffers] = {};
    buffers[t.storage_slot[0]] = &src, buffers[t.storage_slot[1]] = &dst;
    dispatch(*p, 2, buffers, constants.data());
    uint32_t wrong = 0;
    for (uint32_t i = 0; i < 16; ++i) {
      const float expected = i < 13 ? (i * 1.5f - 3.0f) * 2.0f + 0.5f : -7.0f;
      if (fabsf(get_float(dst, 36 + 4 * i) - expected) > 1e-5f) {
        if (!wrong) printf("  %s: b[%u] = %g, expected %g\n", name, i, get_float(dst, 36 + 4 * i), expected);
        ++wrong;
      }
    }
    for (uint32_t i = 0; i < 8; ++i) {
      const int v = static_cast<int>(i) - 5;
      const int expected = (v / 3) * 100 + ((v % 3) + 3) % 3;   // (OpSDiv rounds towards zero; GLSL's % is OpSMod: the sign of the divisor)
      if (static_cast<int>(get_u32(dst, 4 + 4 * i)) != expected) {
        if (!wrong) printf("  %s: q[%u] = %d, expected %d\n", name, i, static_cast<int>(get_u32(dst, 4 + 4 * i)), expected);
        ++wrong;
      }
    }
    CHECK(wrong == 0);
    CHECK(get_u32(dst, 0) == 136);   // the sum of i + 1 over the 16 threads
  }
}

// Group-shared memory with a barrier, a std430 struct (vec3, float, a mat2 of 8-byte columns), the local index; two groups of 16.
void test_compute_shared() {
  HostAllocator allocator;
  for (const char* name : {"cs_shared.comp.spv", "cs_shared_opt.comp.spv"}) {
    Translated t;
    UniquePointer<D3d9Program> p = compile_compute(allocator, name, &t);
    if (!p) continue;
    CHECK(p->needs_group());
    std::vector<uint8_t> items(32 * 32), sums(2 * 4);
    float expected_sum[2] = {0, 0};
    for (uint32_t i = 0; i < 32; ++i) {
      const float f[8] = {0.5f * i, 1.0f + 0.25f * (i % 5), 0.1f * (i % 3), 0.01f * i, 1.0f, 2.0f + i, 3.0f, 0.5f * (i % 4)};   // pos, w, m columns (x, y), (x, y)
      put_floats(items, 32 * i, f, 8);
      expected_sum[i / 16] += f[0] + f[1] * f[2] + f[3] + (f[5] * 1.0f + f[7] * 2.0f);
    }
    std::vector<uint8_t>* buffers[graphics::kMaxStorageBuffers] = {};
    buffers[t.storage_slot[0]] = &items, buffers[t.storage_slot[1]] = &sums;
    std::vector<uint8_t> constants(16);
    dispatch(*p, 2, buffers, constants.data());
    for (uint32_t g = 0; g < 2; ++g) {
      if (fabsf(get_float(sums, 4 * g) - expected_sum[g]) > 1e-3f) printf("  %s: sums[%u] = %g, expected %g\n", name, g, get_float(sums, 4 * g), expected_sum[g]);
      CHECK(fabsf(get_float(sums, 4 * g) - expected_sum[g]) <= 1e-3f);
    }
    uint32_t wrong = 0;
    for (uint32_t i = 0; i < 32; ++i) wrong += get_float(items, 32 * i + 12) != static_cast<float>(i % 16);
    CHECK(wrong == 0);
  }
}

// The work group size of a specialization constant (gl_WorkGroupSize), a uniform block, booleans in a buffer (any non-zero word is true).
void test_compute_spec() {
  HostAllocator allocator;
  for (const char* name : {"cs_spec.comp.spv", "cs_spec_opt.comp.spv"}) {
    const uint32_t ids[1] = {0}, values[1] = {8};
    Translated t;
    UniquePointer<D3d9Program> p = compile_compute(allocator, name, &t, ids, values, 1);
    if (!p) continue;
    CHECK(t.local_size[0] == 8 && p->thread_group(0) == 8);
    std::vector<uint8_t> flags(16 * 4), words(4 + 16 * 4);
    for (uint32_t i = 0; i < 16; ++i) {
      put_u32(flags, 4 * i, i % 3 == 0 ? (i == 3 ? 5u : 1u) : 0u);
      put_u32(words, 4 + 4 * i, 10 * i);
    }
    std::vector<uint8_t> constants(16);
    put_u32(constants, 0, 7);
    std::vector<uint8_t>* buffers[graphics::kMaxStorageBuffers] = {};
    buffers[t.storage_slot[1]] = &flags, buffers[t.storage_slot[2]] = &words;
    dispatch(*p, 2, buffers, constants.data());
    uint32_t wrong = 0;
    for (uint32_t i = 0; i < 16; ++i) wrong += get_u32(words, 4 + 4 * i) != 10 * i + 7 + (i % 3 == 0 ? 1000 : 0);
    CHECK(wrong == 0);
    CHECK(get_u32(words, 0) == 8);
  }
}

// The number of temporaries a DXBC container declares (dcl_temps), 0 if none.
uint32_t declared_temps(const std::vector<uint8_t>& dxbc) {
  for (size_t at = 0; at + 8 <= dxbc.size(); at += 4) {
    uint32_t token, count;
    memcpy(&token, dxbc.data() + at, 4);
    memcpy(&count, dxbc.data() + at + 4, 4);
    if (token == (104u | (2u << 24))) return count;
  }
  return 0;
}

// A shader far past the old limits of 32 temporaries and about 1100 instructions (cs_big.comp, generated: 80 values all live through 30 rounds of updates), against the same
// arithmetic in C++; and its TGSI (what virgl gets) is made too.
void test_compute_big() {
  HostAllocator allocator;
  constexpr int kValues = 80, kRounds = 30;
  for (const char* name : {"cs_big.comp.spv", "cs_big_opt.comp.spv"}) {
    Translated t;
    UniquePointer<D3d9Program> p = compile_compute(allocator, name, &t);
    if (!p) continue;
    const uint32_t temps = declared_temps(t.dxbc);
    printf("  %s: %u bytes of DXBC, %u temporaries\n", name, static_cast<uint32_t>(t.dxbc.size()), temps);
    CHECK(temps > 32 && t.dxbc.size() > 1100 * 8);
    std::vector<uint8_t> values(4 * 8);
    for (uint32_t i = 0; i < 8; ++i) {
      const float x = 0.25f * i - 1.0f;
      put_floats(values, 4 * i, &x, 1);
    }
    std::vector<uint8_t>* buffers[graphics::kMaxStorageBuffers] = {};
    buffers[t.storage_slot[0]] = &values;
    std::vector<uint8_t> constants(16);
    dispatch(*p, 2, buffers, constants.data());
    uint32_t wrong = 0;
    for (uint32_t i = 0; i < 8; ++i) {
      const float x = 0.25f * i - 1.0f;
      float a[kValues];
      for (int k = 0; k < kValues; ++k) a[k] = x * static_cast<float>(k % 7 + 1) + static_cast<float>(k);
      for (int r = 0; r < kRounds; ++r)
        for (int k = 0; k < kValues; ++k) a[k] = a[k] * 0.5f + a[(k + 7) % kValues] * 0.25f + static_cast<float>(r % 5);
      float sum = 0;
      for (int k = 0; k < kValues; ++k) sum = sum + a[k] * static_cast<float>(k % 3 + 1);
      const float got = get_float(values, 4 * i);
      if (fabsf(got - sum) > 1e-4f * (1 + fabsf(sum))) {
        if (!wrong) printf("  %s: v[%u] = %g, expected %g\n", name, i, got, sum);
        ++wrong;
      }
    }
    CHECK(wrong == 0);
  }
}

// Dynamic indices into registers (cs_dyn.comp): local arrays read and written in loops, a 2D local array, a constant array, a vector's component, a member array of a struct array.
void test_compute_dynamic() {
  HostAllocator allocator;
  for (const char* name : {"cs_dyn.comp.spv", "cs_dyn_opt.comp.spv"}) {
    Translated t;
    UniquePointer<D3d9Program> p = compile_compute(allocator, name, &t);
    if (!p) continue;
    std::vector<uint8_t> values(4 * 16);
    for (uint32_t i = 0; i < 16; ++i) {
      const float x = 0.5f * i - 2.0f;
      put_floats(values, 4 * i, &x, 1);
    }
    std::vector<uint8_t>* buffers[graphics::kMaxStorageBuffers] = {};
    buffers[t.storage_slot[0]] = &values;
    std::vector<uint8_t> constants(16);
    dispatch(*p, 2, buffers, constants.data());
    uint32_t wrong = 0;
    for (int i = 0; i < 16; ++i) {
      const float x = 0.5f * i - 2.0f;
      float a[6];
      for (int k = 0; k < 6; ++k) a[k] = x * static_cast<float>(k + 1);
      a[i % 6] = a[i % 6] + 100.0f;
      float g[2][3];
      for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c) g[r][c] = static_cast<float>(r * 10 + c) + x;
      const float table[5] = {1.5f, 2.5f, 3.5f, 4.5f, 5.5f};
      float q[4] = {x, x + 1.0f, x + 2.0f, x + 3.0f};
      q[i & 3] = -1.0f;
      float w[2][3];
      for (int k = 0; k < 3; ++k) w[0][k] = static_cast<float>(k), w[1][k] = static_cast<float>(k) * 2.0f;
      w[i & 1][i % 3] = 50.0f;
      float sum = 0;
      for (int k = 0; k < 6; ++k) sum += a[k] * static_cast<float>(k + 1);
      sum += g[i & 1][i % 3] * 1000.0f;
      sum += table[i % 5] * 7.0f;
      sum += q[(i + 1) & 3] * 13.0f + q[i & 3];
      sum += w[(i + 1) & 1][i % 3] * 3.0f + w[i & 1][i % 3];
      const float got = get_float(values, 4 * static_cast<uint32_t>(i));
      if (fabsf(got - sum) > 1e-3f * (1 + fabsf(sum))) {
        if (!wrong) printf("  %s: v[%d] = %g, expected %g\n", name, i, got, sum);
        ++wrong;
      }
    }
    CHECK(wrong == 0);
  }
}

// A texture set that says what was asked of it: (u, v, w, unit), w the layer of a 2D array.
class EchoTextures final : public graphics::ITextureSet {
 public:
  graphics::Vec4 sample(uint32_t unit, float u, float v) const override { return {u, v, -1.0f, static_cast<float>(unit)}; }
  graphics::Vec4 sample_volume(uint32_t unit, float u, float v, float w) const override { return {u, v, w, static_cast<float>(unit)}; }
};

// Cube maps and cube map arrays (vk_cube.frag) are 2D arrays of six layers a cube: the shader picks the face by Vulkan's table and the point on it, and the layer.
void test_cube_arrays() {
  HostAllocator allocator;
  struct Case {
    float x, y, z, cube;
  };
  const Case cases[] = {{1.0f, 0.25f, -0.5f, 0}, {-0.8f, 0.3f, 0.1f, 1}, {0.2f, 0.9f, -0.4f, 2}, {0.3f, -0.7f, 0.2f, 0}, {-0.1f, 0.2f, 0.95f, 3}, {0.4f, -0.3f, -0.6f, 1.4f}};
  for (const char* name : {"vk_cube.frag.spv", "vk_cube_opt.frag.spv"}) {
    Translated t = translate(read_spirv(name), SPV_STAGE_FRAGMENT);
    if (!t.ok) printf("  %s: %s\n", name, t.error.c_str());
    CHECK(t.ok);
    if (!t.ok) continue;
    NtStatus s = 0;
    UniquePointer<D3d9Program> p = compile(allocator, t, &s);
    CHECK(s == status::kSuccess && p);
    if (!p) continue;
    EchoTextures textures;
    uint32_t wrong = 0;
    for (const Case& c : cases) {
      // Vulkan's table: the major axis and its sign give the face, sc and tc
      const float ax = fabsf(c.x), ay = fabsf(c.y), az = fabsf(c.z);
      float sc, tc, ma, face;
      if (ax >= ay && ax >= az) face = c.x >= 0 ? 0 : 1, sc = c.x >= 0 ? -c.z : c.z, tc = -c.y, ma = ax;
      else if (ay >= az) face = c.y >= 0 ? 2 : 3, sc = c.x, tc = c.y >= 0 ? c.z : -c.z, ma = ay;
      else face = c.z >= 0 ? 4 : 5, sc = c.z >= 0 ? c.x : -c.x, tc = -c.y, ma = az;
      const float u = 0.5f * (sc / ma) + 0.5f, v = 0.5f * (tc / ma) + 0.5f;
      graphics::Vec4 varyings[4] = {{c.x, c.y, c.z, c.cube}, {}, {}, {}};
      graphics::Vec4 color[graphics::kMaxColorTargets] = {};
      const std::vector<uint8_t> constants(16);
      p->run_fragment(varyings, constants.data(), &textures, color);
      const float layer = face + 6.0f * rintf(c.cube);
      const bool ok = fabsf(color[0].x - u) < 1e-5f && fabsf(color[0].y - v) < 1e-5f && fabsf(color[0].z - layer) < 1e-5f && color[0].w == 0.0f &&
                      fabsf(color[1].x - u) < 1e-5f && fabsf(color[1].y - v) < 1e-5f && fabsf(color[1].z - face) < 1e-5f && color[1].w == 1.0f;
      if (!ok && !wrong)
        printf("  %s: (%g %g %g, cube %g): array %g %g %g unit %g, cube %g %g %g unit %g; want %g %g layer %g face %g\n", name, c.x, c.y, c.z, c.cube, color[0].x, color[0].y, color[0].z,
               color[0].w, color[1].x, color[1].y, color[1].z, color[1].w, u, v, layer, face);
      wrong += !ok;
    }
    CHECK(wrong == 0);
  }
}

// A texture set that says what it was asked: a lookup at a level (u, v, lod, 10 + unit), in a volume or array (u, v, w, 20 + unit), a fetch (x, y, level, 30 + unit); 16 x 8.
class EchoTexels final : public graphics::ITextureSet {
 public:
  graphics::Vec4 sample(uint32_t unit, float u, float v) const override { return {u, v, -1.0f, static_cast<float>(unit)}; }
  graphics::Vec4 sample_lod(uint32_t unit, float u, float v, float lod) const override { return {u, v, lod, 10.0f + unit}; }
  graphics::Vec4 sample_lod_volume(uint32_t unit, float u, float v, float w, float) const override { return {u, v, w, 20.0f + unit}; }
  graphics::Vec4 fetch(uint32_t unit, int32_t x, int32_t y, uint32_t level) const override { return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(level), 30.0f + unit}; }
  bool size(uint32_t, uint32_t level, uint32_t* width, uint32_t* height, uint32_t* levels) const override {
    *width = 16 >> level, *height = 8 >> level, *levels = 1;
    return true;
  }
};

// Textures in a compute shader (cs_tex.comp) and a vertex shader (vk_vtex.vert): lookups at a level, texel fetches, sizes; the kernel used to refuse them outside pixel shaders.
void test_textures_outside_pixels() {
  HostAllocator allocator;
  EchoTexels textures;
  for (const char* name : {"cs_tex.comp.spv", "cs_tex_opt.comp.spv"}) {
    Translated t;
    UniquePointer<D3d9Program> p = compile_compute(allocator, name, &t);
    if (!p) continue;
    std::vector<uint8_t> out(4 * 16 * 4);
    std::vector<uint8_t>* buffers[graphics::kMaxStorageBuffers] = {};
    buffers[t.storage_slot[2]] = &out;
    std::vector<uint8_t> constants(16);
    dispatch(*p, 1, buffers, constants.data(), &textures);
    uint32_t wrong = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      const float* o = reinterpret_cast<const float*>(out.data()) + i * 16;
      const float u = static_cast<float>(i) * 0.125f;
      wrong += !(o[0] == u && o[1] == 0.5f && o[2] == 0.0f && o[3] >= 10.0f && o[3] < 12.0f);                                     // textureLod: unit 0 or 1
      wrong += !(o[4] == static_cast<float>(i) && o[5] == 1.0f && o[6] == 0.0f && o[7] == o[3] + 20.0f);                         // texelFetch, the same unit
      wrong += !(o[8] == u && o[9] == 0.5f && o[10] == 2.0f && o[11] >= 20.0f && o[11] < 22.0f && o[11] - 20.0f != o[3] - 10.0f);   // the array: the other unit
      wrong += !(o[12] == 16.0f && o[13] == 8.0f);                                                                                // textureSize
      if (wrong && i == 0) printf("  %s: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g\n", name, o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8], o[9], o[10], o[11], o[12], o[13]);
    }
    CHECK(wrong == 0);
  }
  Translated v = translate(read_spirv("vk_vtex.vert.spv"), SPV_STAGE_VERTEX);
  if (!v.ok) printf("  vk_vtex.vert: %s\n", v.error.c_str());
  CHECK(v.ok);
  if (!v.ok) return;
  NtStatus s = 0;
  UniquePointer<D3d9Program> p = compile(allocator, v, &s);
  if (s != status::kSuccess) printf("  vk_vtex.vert: the kernel refused the DXBC (0x%x)\n", s);
  CHECK(s == status::kSuccess && p);
  if (!p) return;
  graphics::VertexContext context;
  context.textures = &textures;
  graphics::Vec4 attributes[4] = {};
  graphics::ShaderIo io = {};
  std::vector<uint8_t> constants(16);
  p->run_vertex_ex(attributes, constants.data(), context, &io);
  CHECK(io.position.x == 0.25f && io.position.y == 0.75f && io.position.z == 3.0f && io.position.w == 2.0f);   // (texture(): u, v at level 0; texelFetch: x, y)
  graphics::VertexContext none;   // (without a texture set: zeros, not a crash)
  p->run_vertex_ex(attributes, constants.data(), none, &io);
  // The shaders of the QEMU test (tests/windows/vulkantest.c): taken, and their TGSI made (VULKAN_SHADER_TGSI=1 prints it)
  Translated c;
  CHECK(compile_compute(allocator, "cs_ctex_opt.comp.spv", &c));
  Translated d = translate(read_spirv("vk_vtexdraw.vert.spv"), SPV_STAGE_VERTEX);
  CHECK(d.ok);
  UniquePointer<D3d9Program> q = d.ok ? compile(allocator, d) : UniquePointer<D3d9Program>();
  CHECK(q);
  if (q) {
    String tgsi(allocator);
    CHECK(q->emit_tgsi(&tgsi) == status::kSuccess);
    if (getenv("VULKAN_SHADER_TGSI")) printf("--- vk_vtexdraw.vert TGSI ---\n%.*s\n", static_cast<int>(tgsi.size()), tgsi.data());
  }
}

// Sixteen vertex attributes and fifteen varyings (vk_many.vert / .frag), with the system values above them (gl_VertexIndex, gl_FrontFacing): the kernel took four of each.
void test_many_inputs() {
  HostAllocator allocator;
  Translated vt = translate(read_spirv("vk_many.vert.spv"), SPV_STAGE_VERTEX), ft = translate(read_spirv("vk_many.frag.spv"), SPV_STAGE_FRAGMENT);
  if (!vt.ok) printf("  vk_many.vert: %s\n", vt.error.c_str());
  if (!ft.ok) printf("  vk_many.frag: %s\n", ft.error.c_str());
  CHECK(vt.ok && ft.ok);
  if (!vt.ok || !ft.ok) return;
  NtStatus s = 0;
  UniquePointer<D3d9Program> vs = compile(allocator, vt, &s);
  if (s != status::kSuccess) printf("  vk_many.vert: the kernel refused the DXBC (0x%x)\n", s);
  UniquePointer<D3d9Program> fs = compile(allocator, ft, &s);
  if (s != status::kSuccess) printf("  vk_many.frag: the kernel refused the DXBC (0x%x)\n", s);
  CHECK(vs && fs);
  if (!vs || !fs) return;
  graphics::Vec4 attributes[graphics::kMaxVertexAttributes];
  for (uint32_t k = 0; k < 16; ++k) attributes[k] = {1.0f * k, 2.0f * k, 0.5f * k, 1.0f};
  graphics::VertexContext context;
  context.vertex_id = 7;
  graphics::ShaderIo io = {};
  std::vector<uint8_t> constants(16);
  vs->run_vertex_ex(attributes, constants.data(), context, &io);
  uint32_t wrong = 0;
  for (uint32_t k = 0; k < 15; ++k) {
    const graphics::Vec4 want = {attributes[k].x + attributes[k + 1].x * 0.5f, attributes[k].y + attributes[k + 1].y * 0.5f, attributes[k].z + attributes[k + 1].z * 0.5f, 1.5f};
    if (io.varying[k].x != want.x || io.varying[k].y != want.y || io.varying[k].z != want.z || io.varying[k].w != want.w) {
      if (!wrong) printf("  vk_many.vert: varying %u is %g %g %g %g\n", k, io.varying[k].x, io.varying[k].y, io.varying[k].z, io.varying[k].w);
      ++wrong;
    }
  }
  CHECK(wrong == 0);
  CHECK(io.position.x == 15.0f + 7.0f && io.position.y == 30.0f + 7.0f && io.position.z == 7.5f + 7.0f && io.position.w == 8.0f);
  graphics::FragmentContext fc;
  fc.front_facing = true;
  graphics::Vec4 color[graphics::kMaxColorTargets] = {};
  CHECK(fs->run_fragment_ex(io.varying, fc, constants.data(), nullptr, color));
  float want[4] = {1000, 1000, 1000, 1000};
  for (uint32_t k = 0; k < 15; ++k) want[0] += io.varying[k].x * (k + 1), want[1] += io.varying[k].y * (k + 1), want[2] += io.varying[k].z * (k + 1), want[3] += io.varying[k].w * (k + 1);
  if (fabsf(color[0].x - want[0]) > 1e-2f || fabsf(color[0].w - want[3]) > 1e-2f) printf("  vk_many.frag: %g %g %g %g, want %g %g %g %g\n", color[0].x, color[0].y, color[0].z, color[0].w, want[0], want[1], want[2], want[3]);
  CHECK(fabsf(color[0].x - want[0]) <= 1e-2f && fabsf(color[0].y - want[1]) <= 1e-2f && fabsf(color[0].z - want[2]) <= 1e-2f && fabsf(color[0].w - want[3]) <= 1e-2f);
  String tgsi(allocator);
  CHECK(vs->emit_tgsi(&tgsi, 15) == status::kSuccess && fs->emit_tgsi(&tgsi, 15) == status::kSuccess);   // (what virgl gets, with its fifteen varyings)
}

// A vertex shader reading a read-only storage buffer (the kernel's storage in draws).
void test_vertex_storage() {
  HostAllocator allocator;
  Translated t = translate(read_spirv("vk_ssbo.vert.spv"), SPV_STAGE_VERTEX);
  if (!t.ok) printf("  vk_ssbo.vert: %s\n", t.error.c_str());
  CHECK(t.ok && t.storage_slot[0] == graphics::kMaxUavBuffers);
  if (!t.ok) return;
  NtStatus s = 0;
  UniquePointer<D3d9Program> p = compile(allocator, t, &s);
  CHECK(s == status::kSuccess && p);
  if (!p) return;
  std::vector<uint8_t> positions(4 * 16);
  for (uint32_t i = 0; i < 4; ++i) {
    const float v[4] = {1.0f * i, 2.0f * i, 0.5f, 1.0f};
    put_floats(positions, 16 * i, v, 4);
  }
  ByteView views[graphics::kMaxStorageBuffers] = {};
  views[graphics::kMaxUavBuffers] = ByteView(positions.data(), positions.size());
  graphics::VertexContext context;
  context.storage = views;
  context.vertex_id = 3;
  graphics::Vec4 attributes[4] = {};
  graphics::ShaderIo io = {};
  std::vector<uint8_t> constants(16);
  p->run_vertex_ex(attributes, constants.data(), context, &io);
  CHECK(io.position.x == 3.0f && io.position.y == 6.0f && io.position.z == 0.5f && io.position.w == 1.0f);
}

// Things the translator names as not taken must say so.
void test_refusals() {
  const std::vector<uint32_t> words = read_spirv("basic.frag.spv");
  Translated wrong_stage = translate(words, SPV_STAGE_VERTEX);
  CHECK(!wrong_stage.ok && !wrong_stage.error.empty());
  Translated compute = translate(words, SPV_STAGE_COMPUTE);
  CHECK(!compute.ok);
  Translated not_there = translate(words, SPV_STAGE_FRAGMENT, "nope");
  CHECK(!not_there.ok);
  CHECK(spv_open(nullptr, 0, nullptr) == nullptr);
  const uint32_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  SpvAllocator a = {sp_alloc_zero, sp_release, nullptr};
  CHECK(spv_open(junk, 8, &a) == nullptr);
}

}  // namespace

int main() {
  printf("basic\n");
  test_basic();
  printf("math\n");
  test_math();
  printf("compute\n");
  test_compute_buffers();
  test_compute_shared();
  test_compute_spec();
  test_compute_big();
  test_compute_dynamic();
  test_cube_arrays();
  test_textures_outside_pixels();
  test_many_inputs();
  test_vertex_storage();
  printf("refusals\n");
  test_refusals();
  printf("fuzz\n");
  test_fuzz();
  printf(g_failed ? "FAILED: %d\n" : "all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
