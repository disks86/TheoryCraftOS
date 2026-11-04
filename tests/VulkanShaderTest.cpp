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

// Host-side tests for the SPIR-V to DXBC translator of vulkan-1.dll (src/Windows/vulkan-1/spirv.inc): shaders compiled by glslc (tests/data/spirv, the GLSL next to the SPIR-V) are
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

// One translated shader: the DXBC, and the layout of the constant buffers it was given (slot n at the sum of the sizes before it).
struct Translated {
  std::vector<uint8_t> dxbc;
  std::vector<uint32_t> cb_vec4;
  std::vector<uint32_t> binding_of_slot;
  std::string error;
  bool ok = false;
};

Translated translate(const std::vector<uint32_t>& words, uint32_t stage, const char* entry = nullptr) {
  Translated t;
  SpvAllocator a = {sp_alloc_zero, sp_release, nullptr};
  SpvModule* m = spv_open(words.data(), static_cast<uint32_t>(words.size()), &a);
  if (!m) {
    t.error = "open";
    return t;
  }
  if (spv_reflect(m, stage, entry, nullptr, nullptr, 0)) {
    t.error = spv_error(m);
    spv_close(m);
    return t;
  }
  uint32_t slot = 0, unit = 0;
  for (uint32_t i = 0; i < spv_resource_count(m); ++i) {
    SpvResource* r = spv_resource(m, i);
    if (r->kind == SPV_RES_UNIFORM || r->kind == SPV_RES_PUSH) {
      r->slot = slot++;
      r->cb_vec4 = (r->bytes + 15) / 16;
      spv_declare_cb(m, r->slot, r->cb_vec4);
      t.cb_vec4.push_back(r->cb_vec4);
      t.binding_of_slot.push_back(r->binding);
    } else {
      r->slot = unit++;
    }
  }
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
  for (const char* name : {"basic.vert.spv", "basic.frag.spv", "math.frag.spv"}) {
    const std::vector<uint32_t> words = read_spirv(name);
    const uint32_t stage = strstr(name, ".vert") ? SPV_STAGE_VERTEX : SPV_STAGE_FRAGMENT;
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
  printf("refusals\n");
  test_refusals();
  printf("fuzz\n");
  test_fuzz();
  printf(g_failed ? "FAILED: %d\n" : "all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
