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

// Host-side tests of src/Windows/d3d11/dxbc_link.h, the dynamic shader linkage of d3d11.dll:
//   - the IFCE part of a shader Microsoft's compiler made (tests/data/dxbc/ps_iface.hlsl) is read as its disassembly lists it;
//   - the shader specialised for instances bound to its slots - static and dynamically indexed slots, bodies that return from inside an if and from inside
//     a loop, an instance array's elements - is taken by the kernel's DXBC front end and gives what the HLSL says for the constants given;
//   - a class the slot does not take is refused, an unbound slot calls nothing;
//   - containers cut short and with a bit turned are read and specialised without a read outside them (ASan, UBSan), and what comes out is compiled
//     by the kernel's front end without one either.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <string>
#include <vector>
#include "Kernel/Dxgkrnl/D3d9Shader.hpp"
#include "../src/Windows/d3d11/dxbc_link.h"

using namespace dxgk;

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

struct HostAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    void* p = nullptr;
    if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size)) return nullptr;
    return p;
  }
  void deallocate(void* p) override { free(p); }
};

std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> out;
  FILE* f = fopen(path, "rb");
  if (!f) return out;
  uint8_t buffer[4096];
  size_t n;
  while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) out.insert(out.end(), buffer, buffer + n);
  fclose(f);
  return out;
}

// The constants of ps_iface: cb0 (6 vectors: red, greens[2], halved (colour, scale), loop) then cb1 (which).
struct Constants {
  float v[7][4] = {};
  void set(uint32_t reg, float x, float y, float z, float w) { v[reg][0] = x, v[reg][1] = y, v[reg][2] = z, v[reg][3] = w; }
  void which(uint32_t n) { memcpy(&v[6][0], &n, 4); }
};

DlBinding bound(const DlInfo& info, const char* type, uint32_t cb, uint32_t offset, uint32_t texture = 0xFFFF, uint32_t sampler = 0xFFFF) {
  DlBinding b;
  b.bound = 1;
  b.type = dl_type_named(&info, type);
  b.this_values[0] = cb, b.this_values[1] = offset, b.this_values[2] = texture, b.this_values[3] = sampler;
  return b;
}

struct Run {
  bool ok = false;
  float colour[4] = {};
};

Run specialise_and_run(HostAllocator& allocator, const std::vector<uint8_t>& dxbc, const DlInfo& info, const DlBinding* bind, uint32_t count, const Constants& c,
                       DlWork* work) {
  Run r;
  if (dl_specialise(dxbc.data(), dxbc.size(), &info, bind, count, work) != DL_OK) return r;
  UniquePointer<D3d9Program> program;
  const NtStatus s = D3d9Program::compile_dxbc(allocator, ConstByteView(work->dxbc, work->dxbc_size), &program);
  if (s != status::kSuccess) {
    printf("  the kernel refused the specialised shader: %x\n", static_cast<unsigned>(s));
    return r;
  }
  graphics::Vec4 varyings[graphics::kMaxVaryings] = {};
  graphics::Vec4 colour[graphics::kMaxColorTargets] = {};
  if (!program->run_fragment(varyings, reinterpret_cast<const uint8_t*>(c.v), nullptr, colour)) return r;
  r.ok = true;
  r.colour[0] = colour[0].x, r.colour[1] = colour[0].y, r.colour[2] = colour[0].z, r.colour[3] = colour[0].w;
  return r;
}

bool near4(const Run& r, float x, float y, float z, float w) {
  const float e[4] = {x, y, z, w};
  for (int i = 0; i < 4; ++i)
    if (!(fabsf(r.colour[i] - e[i]) < 1e-5f)) {
      printf("  got %g %g %g %g, want %g %g %g %g\n", r.colour[0], r.colour[1], r.colour[2], r.colour[3], x, y, z, w);
      return false;
    }
  return r.ok;
}

void test_interfaces_read(const std::vector<uint8_t>& dxbc, const DlInfo& info) {
  // (as Microsoft's disassembler lists them)
  CHECK(info.type_count == 4);
  CHECK(strcmp(info.types[0].name, "cLoop") == 0 && info.types[0].cb_stride == 1);
  CHECK(strcmp(info.types[1].name, "cTex") == 0 && info.types[1].cb_stride == 0 && info.types[1].textures == 1 && info.types[1].samplers == 1);
  CHECK(strcmp(info.types[2].name, "cScaled") == 0 && info.types[2].cb_stride == 2);
  CHECK(strcmp(info.types[3].name, "cConst") == 0 && info.types[3].cb_stride == 1);
  CHECK(info.instance_count == 5);
  CHECK(strcmp(info.instances[0].name, "texture_colour") == 0 && info.instances[0].type == 1 && info.instances[0].cb == 0xFFFF && info.instances[0].texture == 0 &&
        info.instances[0].sampler == 0);
  CHECK(strcmp(info.instances[2].name, "greens") == 0 && info.instances[2].count == 2 && info.instances[2].cb == 0 && info.instances[2].cb_offset == 1);
  CHECK(strcmp(info.instances[4].name, "loop") == 0 && info.instances[4].type == 0 && info.instances[4].cb_offset == 5);
  CHECK(info.slot_count == 3);
  CHECK(info.slot_first[1] - info.slot_first[0] == 4 && info.slot_first[3] - info.slot_first[2] == 4);
  CHECK(info.entry_table[info.slot_first[0] + 3] == 3 && info.entry_table[info.slot_first[1] + 2] == 6 && info.entry_table[info.slot_first[2]] == 4);
  CHECK(dl_type_named(&info, "cConst") == 3 && dl_type_named(&info, "nothing") == DL_NONE);
  (void)dxbc;
}

void test_specialised(HostAllocator& allocator, const std::vector<uint8_t>& dxbc, const DlInfo& info, DlWork* work) {
  Constants c;
  c.set(0, 0.5f, 0, 0, 0.25f);         // red
  c.set(1, 0, 0.125f, 0, 0);           // greens[0]
  c.set(2, 0, 0.25f, 0, 0);            // greens[1]
  c.set(3, 0.5f, 0.5f, 0.5f, 0.5f);    // halved.colour
  c.set(4, 0.5f, 0, 0, 0);             // halved.scale
  c.set(5, 0.25f, 0.125f, 0, 0.6f);    // loop.step (stops once x reaches 0.6)
  // g_first = red, g_list = {halved, greens[1]}
  DlBinding b[3] = {bound(info, "cConst", 0, 0), bound(info, "cScaled", 0, 3), bound(info, "cConst", 0, 2)};
  c.which(0);
  CHECK(near4(specialise_and_run(allocator, dxbc, info, b, 3, c, work), 0.75f, 0.25f, 0.25f, 0.5f));      // red + halved.colour * 0.5
  c.which(1);
  CHECK(near4(specialise_and_run(allocator, dxbc, info, b, 3, c, work), 0.5f, 0.25f, 0, 0.25f));          // red + greens[1]
  // a return from inside an if: halved with a negative scale gives (1, 0, 1, 1)
  c.set(4, -1, 0, 0, 0);
  c.which(0);
  CHECK(near4(specialise_and_run(allocator, dxbc, info, b, 3, c, work), 1.5f, 0, 1, 1.25f));
  c.set(4, 0.5f, 0, 0, 0);
  // a return from inside a loop: loop sums step until x is 0.6 or more: 3 steps
  DlBinding l[3] = {bound(info, "cLoop", 0, 5), bound(info, "cConst", 0, 1), bound(info, "cLoop", 0, 5)};
  c.which(0);
  CHECK(near4(specialise_and_run(allocator, dxbc, info, l, 3, c, work), 0.75f, 0.375f + 0.125f, 0, 1.8f));      // 3 steps + greens[0]
  c.which(1);
  CHECK(near4(specialise_and_run(allocator, dxbc, info, l, 3, c, work), 1.5f, 0.75f, 0, 3.6f));               // 3 steps twice
  // a step that never gets there: the loop ends and the body returns (0, 0, 0, 1)
  c.set(5, 0, 0, 0, 0.6f);
  CHECK(near4(specialise_and_run(allocator, dxbc, info, l, 3, c, work), 0, 0, 0, 2));
  // a created instance with its data elsewhere in the buffer: cConst at vector 4 (halved.scale's: 0.5, 0, 0, 0)
  DlBinding k[3] = {bound(info, "cConst", 0, 4), bound(info, "cConst", 0, 4), bound(info, "cConst", 0, 4)};
  CHECK(near4(specialise_and_run(allocator, dxbc, info, k, 3, c, work), 1, 0, 0, 0));
  // the constants in another buffer (cb1: `which` is 1)
  DlBinding o[3] = {bound(info, "cConst", 1, 0), bound(info, "cConst", 0, 0), bound(info, "cConst", 0, 0)};
  c.which(1);
  {
    const Run r = specialise_and_run(allocator, dxbc, info, o, 3, c, work);
    uint32_t bits;
    memcpy(&bits, &r.colour[0], 4);
    float one;
    const uint32_t one_bits = 1;
    memcpy(&one, &one_bits, 4);
    CHECK(r.ok && fabsf(r.colour[0] - (one + 0.5f)) < 1e-6f);      // (the integer 1 read as a float, plus red.x)
  }
  // a slot with nothing bound calls nothing; the other still adds
  DlBinding u[3] = {bound(info, "cConst", 0, 0), bound(info, "cConst", 0, 0), bound(info, "cConst", 0, 1)};
  u[0].bound = 0;
  c.which(1);
  CHECK(specialise_and_run(allocator, dxbc, info, u, 3, c, work).ok);
  // a class the slot does not take; a type number out of range
  DlBinding bad[3] = {bound(info, "cConst", 0, 0), bound(info, "cConst", 0, 0), bound(info, "cConst", 0, 0)};
  bad[2].type = 77;
  CHECK(dl_specialise(dxbc.data(), dxbc.size(), &info, bad, 3, work) == DL_UNSUPPORTED);
  // fewer bindings than slots: the others are unbound
  CHECK(dl_specialise(dxbc.data(), dxbc.size(), &info, b, 1, work) == DL_OK);
  // the specialised shader has no interfaces left
  CHECK(dl_specialise(dxbc.data(), dxbc.size(), &info, b, 3, work) == DL_OK);
  DlInfo again;
  CHECK(dl_read_interfaces(work->dxbc, work->dxbc_size, &again) == DL_OK && again.slot_count == 0);
  for (uint32_t i = 2; i < work->out_count;) {
    const uint32_t op = work->out[i] & 0x7FF, length = dl_ins_length(work->out, work->out_count, i);
    CHECK(length != 0);
    if (!length) break;
    CHECK(op != DL_OP_FCALL && op != DL_OP_LABEL && op != DL_OP_DCL_INTERFACE && op != DL_OP_DCL_FUNCTION_BODY && op != DL_OP_DCL_FUNCTION_TABLE);
    i += length;
  }
}

// Every prefix, and every bit turned: no read outside, and what is made the kernel's front end reads without one.
void test_damaged(HostAllocator& allocator, const std::vector<uint8_t>& dxbc, DlWork* work, const char* const* types, uint32_t slots) {
  std::unique_ptr<DlInfo> info(new DlInfo);
  DlBinding b[3];
  int specialised = 0;
  for (size_t n = 0; n < dxbc.size(); n += (n < 256 ? 1 : 7)) {
    std::vector<uint8_t> cut(dxbc.begin(), dxbc.begin() + static_cast<long>(n));
    if (dl_read_interfaces(cut.data(), cut.size(), info.get()) != DL_OK) continue;
    for (uint32_t s = 0; s < slots; ++s) b[s] = bound(*info, types[s], 0, s, 0, 0);
    if (dl_specialise(cut.data(), cut.size(), info.get(), b, slots, work) == DL_OK) ++specialised;
  }
  int taken = 0;
  for (size_t bit = 0; bit < dxbc.size() * 8; ++bit) {
    std::vector<uint8_t> turned = dxbc;
    turned[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    if (dl_read_interfaces(turned.data(), turned.size(), info.get()) != DL_OK) continue;
    for (uint32_t s = 0; s < slots; ++s) b[s] = bound(*info, types[s], 0, s, 0, 0);
    if (dl_specialise(turned.data(), turned.size(), info.get(), b, slots, work) != DL_OK) continue;
    ++taken;
    UniquePointer<D3d9Program> program;
    if (D3d9Program::compile_dxbc(allocator, ConstByteView(work->dxbc, work->dxbc_size), &program) == status::kSuccess) {
      graphics::Vec4 varyings[graphics::kMaxVaryings] = {};
      graphics::Vec4 colour[graphics::kMaxColorTargets] = {};
      std::vector<uint8_t> constants(program->constant_bytes() + 16, 0);      // (a damaged shader may declare larger buffers)
      if (!program->uses_textures()) program->run_fragment(varyings, constants.data(), nullptr, colour);
    }
  }
  printf("  damaged: %d prefixes specialised, %d of %zu turned bits\n", specialised, taken, dxbc.size() * 8);
  CHECK(taken > 0);
}

// A texture set whose texel colour says the texture register it read (through the program's unit -> register mapping) and the sampler register.
struct RegisterTextures final : graphics::ITextureSet {
  const D3d9Program* program = nullptr;
  graphics::Vec4 sample(uint32_t unit, float u, float v) const override {
    (void)u, (void)v;
    return {0.125f * static_cast<float>(program->texture_register(unit)), 0.125f * static_cast<float>(program->sampler_register(unit)), 0, 1};
  }
};

// ps_iface_dyn: a body that samples t[this.texture + pick] (pick read from the instance's data): the index is a register the kernel cannot take, so the sample
// is written once for each declared texture inside ifs; the call goes through an array indexed by a register (which).
void test_dynamic_index(HostAllocator& allocator, DlWork* work) {
  const std::vector<uint8_t> dxbc = read_file("tests/data/dxbc/ps_iface_dyn.dxbc");
  CHECK(dxbc.size() > 100);
  std::unique_ptr<DlInfo> info(new DlInfo);
  CHECK(dl_read_interfaces(dxbc.data(), dxbc.size(), info.get()) == DL_OK && info->slot_count == 2);
  // g_list = {picker (cb0[0], textures 0 and 1, sampler 0), picker_b (cb2[0], textures 2 and 3, sampler 1)}
  DlBinding b[2] = {bound(*info, "cPick", 0, 0, 0, 0), bound(*info, "cPick", 2, 0, 2, 1)};
  // the constants: cb0 (picker.pick, flat.colour), cb1 (which), cb2 (picker_b.pick)
  struct {
    uint32_t pick0[4], flat[4], which[4], pick1[4];
  } c = {};
  int ran = 0;
  for (uint32_t which = 0; which < 2; ++which)
    for (uint32_t pick = 0; pick < 2; ++pick) {
      c.which[0] = which;
      c.pick0[0] = pick, c.pick1[0] = pick;
      if (dl_specialise(dxbc.data(), dxbc.size(), info.get(), b, 2, work) != DL_OK) continue;
      UniquePointer<D3d9Program> program;
      const NtStatus s = D3d9Program::compile_dxbc(allocator, ConstByteView(work->dxbc, work->dxbc_size), &program);
      CHECK(s == status::kSuccess);
      if (s != status::kSuccess) {
        printf("  the kernel refused the specialised shader: %x\n", static_cast<unsigned>(s));
        continue;
      }
      RegisterTextures textures;
      textures.program = program.get();
      graphics::Vec4 varyings[graphics::kMaxVaryings] = {};
      graphics::Vec4 colour[graphics::kMaxColorTargets] = {};
      CHECK(program->run_fragment(varyings, reinterpret_cast<const uint8_t*>(&c), &textures, colour));
      const float texture = static_cast<float>(which * 2 + pick), sampler = static_cast<float>(which);
      CHECK(fabsf(colour[0].x - 0.125f * texture) < 1e-6f && fabsf(colour[0].y - 0.125f * sampler) < 1e-6f);
      if (!(fabsf(colour[0].x - 0.125f * texture) < 1e-6f)) printf("  which %u pick %u: texture %g sampler %g\n", which, pick, colour[0].x * 8, colour[0].y * 8);
      ++ran;
    }
  CHECK(ran == 4);
  const char* const types[3] = {"cPick", "cFlat", "cPick"};
  test_damaged(allocator, dxbc, work, types, 2);
}

// Subroutines of call / label (a [call] switch: tests/data/dxbc/ps_call_switch.hlsl) are inlined by the specialisation itself, so that a body's subroutine
// would have its `this`: the shader made has no call or label left and gives what the HLSL says.
void test_subroutines(HostAllocator& allocator, DlWork* work) {
  const std::vector<uint8_t> dxbc = read_file("tests/data/dxbc/ps_call_switch.dxbc");
  CHECK(dxbc.size() > 100);
  DlInfo none;
  CHECK(dl_read_interfaces(dxbc.data(), dxbc.size(), &none) == DL_OK && none.slot_count == 0);
  CHECK(dl_specialise(dxbc.data(), dxbc.size(), &none, nullptr, 0, work) == DL_OK);
  for (uint32_t i = 2; i < work->out_count;) {
    const uint32_t op = work->out[i] & 0x7FF, length = dl_ins_length(work->out, work->out_count, i);
    CHECK(length != 0);
    if (!length) break;
    CHECK(op != DL_OP_CALL && op != DL_OP_CALLC && op != DL_OP_LABEL);
    i += length;
  }
  UniquePointer<D3d9Program> program;
  CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(work->dxbc, work->dxbc_size), &program) == status::kSuccess);
  if (!program) return;
  int bad = 0;
  for (int32_t first = -1; first <= 4; ++first)
    for (int32_t n = 0; n <= 4; ++n) {
      struct { int32_t sel[4]; float a[4]; } c = {{first, n, 0, 0}, {0.25f, 0.6f, 0.125f, 0.75f}};
      float want[4] = {0, 0, 0, 0};
      for (int32_t i = 0; i < n; ++i) {
        switch (first + i) {
          case 0: want[0] += 1; break;
          case 1: { float s = 0; for (int q = 0; q < 4; ++q) { s += c.a[0]; if (s > c.a[1]) break; } want[1] += s; break; }
          case 2: case 3: want[2] += c.a[2]; if (c.a[3] > 0.5f) { want[3] += 1; break; } want[3] += 2; break;
          default: for (float& v : want) v += 0.5f; break;
        }
      }
      graphics::Vec4 varyings[graphics::kMaxVaryings] = {};
      graphics::Vec4 colour[graphics::kMaxColorTargets] = {};
      if (!program->run_fragment(varyings, reinterpret_cast<const uint8_t*>(&c), nullptr, colour)) ++bad;
      const float got[4] = {colour[0].x, colour[0].y, colour[0].z, colour[0].w};
      for (int k = 0; k < 4; ++k)
        if (!(fabsf(got[k] - want[k]) < 1e-5f)) ++bad;
    }
  CHECK(bad == 0);
}

}  // namespace

int main() {
  HostAllocator allocator;
  const std::vector<uint8_t> dxbc = read_file("tests/data/dxbc/ps_iface.dxbc");
  CHECK(dxbc.size() > 100);
  if (dxbc.size() <= 100) { printf("tests/data/dxbc/ps_iface.dxbc missing (git lfs checkout tests/data)\n"); return 1; }
  std::unique_ptr<DlInfo> info(new DlInfo);
  std::unique_ptr<DlWork> work(new DlWork);
  CHECK(dl_read_interfaces(dxbc.data(), dxbc.size(), info.get()) == DL_OK);
  test_interfaces_read(dxbc, *info);
  test_specialised(allocator, dxbc, *info, work.get());
  {
    const char* const types[3] = {"cLoop", "cScaled", "cConst"};
    test_damaged(allocator, dxbc, work.get(), types, 3);
  }
  test_dynamic_index(allocator, work.get());
  test_subroutines(allocator, work.get());
  // a shader without interfaces: nothing to read
  {
    const std::vector<uint8_t> plain = read_file("tests/data/dxbc/ps_tex.dxbc");
    DlInfo none;
    if (!plain.empty()) CHECK(dl_read_interfaces(plain.data(), plain.size(), &none) == DL_OK && none.slot_count == 0);
  }
  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
