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

// Host-side tests of the lane model of Shader Model 6's wave operations (D3d9Shader.cpp WaveRunner, the DXIL translator's wave operations, D3d9Dxbc.cpp's private opcodes):
//   - compute shaders dxc made (tests/data/dxbc/dxil_cs_wave*.hlsl) run a group at a time give what tests/windows/wave_model.h works out: every operation of 6.0 on 32-bit
//     integers, floats, doubles and 64-bit integers, the 16-bit types, 6.5's match and multi-prefix operations, in divergent branches, a loop the lanes leave at different rounds,
//     after an early return, in a wave that is not full, and with a barrier between;
//   - a pixel shader (dxil_ps_wave.hlsl) run as the quads of a primitive: helpers (uncovered corners, discarded pixels) are not active for the wave operations but are read by the
//     quad operations; without a quad the pixel is the one lane;
//   - the virgl path refuses the shaders (no TGSI for them) and the programs carry kFeature3WaveOps / kFeature3WaveMatch;
//   - every prefix and every bit flipped of the fixtures compile or are refused without a read outside them (ASan, UBSan).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Kernel/Dxgkrnl/D3d9Shader.hpp"
#include "windows/wave_model.h"

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

UniquePointer<D3d9Program> compile(HostAllocator& allocator, const char* name, NtStatus* status) {
  const std::string path = std::string("tests/data/dxbc/") + name + ".dxil";
  const std::vector<uint8_t> blob = read_file(path.c_str());
  UniquePointer<D3d9Program> program;
  *status = blob.empty() ? status::kInvalidParameter : D3d9Program::compile_dxil(allocator, ConstByteView(blob.data(), blob.size()), &program);
  if (*status != status::kSuccess) printf("%s: status %x\n", name, static_cast<unsigned>(*status));
  return program;
}

// Runs a compute program's group 0 with buffer u0 = `words` words of WAVE_UNWRITTEN; the words.
std::vector<uint32_t> dispatch(const D3d9Program& program, uint32_t words) {
  std::vector<uint32_t> out(words, WAVE_UNWRITTEN);
  graphics::ComputeContext context;
  context.buffers[0] = ByteView(reinterpret_cast<uint8_t*>(out.data()), out.size() * 4);
  program.run_compute_group(context, nullptr);
  return out;
}

int compare(const char* what, const std::vector<uint32_t>& got, const uint32_t* want, uint32_t words, uint32_t per_thread) {
  int bad = 0;
  for (uint32_t k = 0; k < words; ++k)
    if (got[k] != want[k] && bad++ < 12) printf("%s: thread %u word %u: %08x, wanted %08x\n", what, k / per_thread, k % per_thread, got[k], want[k]);
  return bad;
}

void test_compute_integers(HostAllocator& allocator) {
  NtStatus s;
  UniquePointer<D3d9Program> p = compile(allocator, "dxil_cs_wave", &s);
  CHECK(s == status::kSuccess && p);
  if (!p) return;
  CHECK(p->uses_wave() && p->needs_group() && !p->keeps_group());
  CHECK((p->required_features3() & graphics::kFeature3WaveOps) != 0 && (p->required_features3() & graphics::kFeature3WaveMatch) == 0);
  String tgsi(allocator);
  CHECK(p->emit_tgsi(&tgsi) == status::kNotSupported);   // (virgl: TGSI has no wave operations)
  const uint32_t words = WAVE_CS_THREADS * WAVE_CS_WORDS;
  std::vector<uint32_t> want(words);
  wave_model_cs(want.data());
  const std::vector<uint32_t> got = dispatch(*p, words);
  CHECK(compare("dxil_cs_wave", got, want.data(), words, WAVE_CS_WORDS) == 0);
}

template <typename Model>
void test_compute(HostAllocator& allocator, const char* name, uint32_t threads, uint32_t per_thread, Model model, bool later, bool keeps_group) {
  NtStatus s;
  UniquePointer<D3d9Program> p = compile(allocator, name, &s);
  CHECK(s == status::kSuccess && p);
  if (!p) return;
  CHECK(p->uses_wave() && p->needs_group() && p->keeps_group() == keeps_group);
  CHECK((p->required_features3() & graphics::kFeature3WaveOps) != 0 && ((p->required_features3() & graphics::kFeature3WaveMatch) != 0) == later);
  const uint32_t words = threads * per_thread;
  std::vector<uint32_t> want(words);
  model(want.data());
  const std::vector<uint32_t> got = dispatch(*p, words);
  const int bad = compare(name, got, want.data(), words, per_thread);
  CHECK(bad == 0);
}

// The pixel shader on quads of a primitive that covers `covered` of their corners: each covered pixel that does not discard gets the colour wave_model_ps says.
struct QuadUser {
  uint32_t qx, qy, covered;
};
void test_pixel(HostAllocator& allocator) {
  NtStatus s;
  UniquePointer<D3d9Program> p = compile(allocator, "dxil_ps_wave", &s);
  CHECK(s == status::kSuccess && p);
  if (!p) return;
  CHECK(p->uses_wave() && p->uses_system_values() && (p->required_features3() & graphics::kFeature3WaveOps) != 0);
  int bad = 0, drawn = 0;
  for (uint32_t qy = 0; qy < 8; qy += 2)
    for (uint32_t qx = 0; qx < 8; qx += 2)
      for (uint32_t covered = 1; covered < 16; ++covered)
        for (uint32_t corner = 0; corner < 4; ++corner) {
          if (!(covered >> corner & 1)) continue;
          QuadUser user = {qx, qy, covered};
          graphics::FragmentContext context;
          context.position = {static_cast<float>(qx + (corner & 1)) + 0.5f, static_cast<float>(qy + (corner >> 1)) + 0.5f, 0.5f, 1.0f};
          context.quad_fn = [](const void* u, uint32_t k, graphics::Vec4* varyings, graphics::Vec4* position) {
            const QuadUser& q = *static_cast<const QuadUser*>(u);
            for (uint32_t n = 0; n < graphics::kMaxVaryings; ++n) varyings[n] = {0, 0, 0, 0};
            *position = {static_cast<float>(q.qx + (k & 1)) + 0.5f, static_cast<float>(q.qy + (k >> 1)) + 0.5f, 0.5f, 1.0f};
          };
          context.quad_covered_fn = [](const void* u, uint32_t k) { return (static_cast<const QuadUser*>(u)->covered >> k & 1) != 0; };
          context.quad_user = &user;
          context.quad_corner = corner;
          graphics::Vec4 varyings[graphics::kMaxVaryings] = {};
          graphics::Vec4 color[graphics::kMaxColorTargets] = {};
          const bool kept = p->run_fragment_ex(varyings, context, nullptr, nullptr, color);
          const uint32_t x = qx + (corner & 1), y = qy + (corner >> 1);
          const bool discards = (x & 3) == 3 && (y & 3) == 3;
          if (kept == discards) {
            if (bad++ < 12) printf("ps (%u, %u) covered %x: kept %d\n", x, y, covered, kept);
            continue;
          }
          if (!kept) continue;
          ++drawn;
          uint32_t want[4];
          wave_model_ps(qx, qy, covered, corner, want);
          const float got[4] = {color[0].x, color[0].y, color[0].z, color[0].w};
          for (uint32_t c = 0; c < 4; ++c) {
            const float g = got[c] * 255.0f;
            if (g < static_cast<float>(want[c]) - 0.01f || g > static_cast<float>(want[c]) + 0.01f) {
              if (bad++ < 12) printf("ps (%u, %u) covered %x channel %u: %g, wanted %u\n", x, y, covered, c, static_cast<double>(g), want[c]);
            }
          }
        }
  CHECK(bad == 0 && drawn > 100);
  // (no quad - a point or a line -: the pixel is the one lane of a wave of four)
  graphics::FragmentContext alone;
  alone.position = {4.5f, 4.5f, 0.5f, 1.0f};
  graphics::Vec4 varyings[graphics::kMaxVaryings] = {};
  graphics::Vec4 color[graphics::kMaxColorTargets] = {};
  CHECK(p->run_fragment_ex(varyings, alone, nullptr, nullptr, color));
  CHECK(color[0].x * 255.0f > 9.9f && color[0].x * 255.0f < 10.1f);    // (one active lane; ddx 0, so no 5)
  CHECK(color[0].z * 255.0f > 12.9f && color[0].z * 255.0f < 13.1f);    // (the first lane, of four)
  CHECK(color[0].w * 255.0f > 7.9f && color[0].w * 255.0f < 8.1f);      // (lane 0)
}

// The resources of 6.0 that the DXIL translator takes as the DXBC front end does (dxil_cs_typed: typed views, GetDimensions, compare-exchange, the counter, group-shared atomics):
// the DXIL and the DXBC of the same HLSL compile, both say the same storage and images, and both have TGSI (they run on virgl too). d3d12test runs the two and compares.
void test_resources(HostAllocator& allocator) {
  NtStatus s;
  UniquePointer<D3d9Program> dxil = compile(allocator, "dxil_cs_typed", &s);
  CHECK(s == status::kSuccess && dxil);
  const std::vector<uint8_t> blob = read_file("tests/data/dxbc/dxil_cs_typed.dxbc");
  UniquePointer<D3d9Program> dxbc;
  CHECK(!blob.empty() && D3d9Program::compile_dxbc(allocator, ConstByteView(blob.data(), blob.size()), &dxbc) == status::kSuccess && dxbc);
  if (!dxil || !dxbc) return;
  CHECK(!dxil->uses_wave() && dxil->required_features3() == 0 && dxil->needs_group() && dxbc->needs_group());
  for (uint32_t slot = 0; slot < 8; ++slot) {
    CHECK(dxil->storage_kind(slot) == dxbc->storage_kind(slot));
    CHECK(dxil->storage_stride(slot) == dxbc->storage_stride(slot));
    CHECK(dxil->image_dim(slot) == dxbc->image_dim(slot));
    CHECK(dxil->image_return(slot) == dxbc->image_return(slot));
    if (dxil->image_dim(slot) != dxbc->image_dim(slot) || dxil->storage_kind(slot) != dxbc->storage_kind(slot) || dxil->image_return(slot) != dxbc->image_return(slot))
      printf("dxil_cs_typed: slot %u differs: dim %d %d, return %d %d\n", slot, static_cast<int>(dxil->image_dim(slot)), static_cast<int>(dxbc->image_dim(slot)),
             static_cast<int>(dxil->image_return(slot)), static_cast<int>(dxbc->image_return(slot)));
  }
  CHECK(dxil->image_dim(graphics::kTexelBufferSlotBase) == dxbc->image_dim(graphics::kTexelBufferSlotBase) && dxil->image_dim(graphics::kTexelBufferSlotBase) == D3d9Program::ImageDim::Buffer);
  CHECK(dxil->required_features() == dxbc->required_features() && dxil->required_features2() == dxbc->required_features2());
  String tgsi_dxil(allocator), tgsi_dxbc(allocator);
  const NtStatus t1 = dxil->emit_tgsi(&tgsi_dxil), t2 = dxbc->emit_tgsi(&tgsi_dxbc);
  CHECK(t1 == status::kSuccess && t2 == status::kSuccess);
  if (t1 != status::kSuccess || t2 != status::kSuccess) printf("dxil_cs_typed: TGSI %x %x\n", static_cast<unsigned>(t1), static_cast<unsigned>(t2));
}

// Every prefix of a fixture and every bit of it turned: compiled or refused, and run when compiled, without a read outside (the sanitizers).
void damaged(HostAllocator& allocator, const char* name, uint32_t words) {
  const std::string path = std::string("tests/data/dxbc/") + name + ".dxil";
  const std::vector<uint8_t> blob = read_file(path.c_str());
  CHECK(!blob.empty());
  uint32_t compiled = 0;
  for (size_t cut = 0; cut < blob.size(); cut += 7) {
    UniquePointer<D3d9Program> program;
    if (D3d9Program::compile_dxil(allocator, ConstByteView(blob.data(), cut), &program) == status::kSuccess) ++compiled;
  }
  for (size_t bit = 0; bit < blob.size() * 8; bit += 29) {
    std::vector<uint8_t> copy = blob;
    copy[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    UniquePointer<D3d9Program> program;
    if (D3d9Program::compile_dxil(allocator, ConstByteView(copy.data(), copy.size()), &program) == status::kSuccess && program && program->stage() == graphics::ShaderStage::Compute) {
      ++compiled;
      dispatch(*program, words);
    }
  }
  printf("%s: %u damaged copies compiled\n", name, compiled);
}

}  // namespace

int main() {
  HostAllocator allocator;
  test_compute_integers(allocator);
  test_compute(allocator, "dxil_cs_wave_types", WAVE_TYPES_THREADS, WAVE_TYPES_WORDS, wave_model_cs_types, false, false);
  test_compute(allocator, "dxil_cs_wave16", WAVE16_THREADS, WAVE16_WORDS, wave_model_cs16, false, false);
  test_compute(allocator, "dxil_cs_wave_match", WAVE_MATCH_THREADS, WAVE_MATCH_WORDS, wave_model_cs_match, true, true);
  test_pixel(allocator);
  test_resources(allocator);
  damaged(allocator, "dxil_cs_wave", WAVE_CS_THREADS * WAVE_CS_WORDS);
  damaged(allocator, "dxil_cs_wave_match", WAVE_MATCH_THREADS * WAVE_MATCH_WORDS);
  damaged(allocator, "dxil_cs_typed", 8 * 16);
  printf("%d of %d checks failed\n", g_failed, g_checks);
  return g_failed ? 1 : 0;
}
