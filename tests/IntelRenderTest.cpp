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

// Host-side tests for the Gen9 render engine path: the context image of the render engine, the 3D command set and its checks, the batch builder (decoded again the way the
// hardware reads it, and executed by a small model of the windower for solid fills), and the driver running 3D batches against the simulated GPU of FakeIntelGpu.hpp.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRenderBatch.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.hpp"

using namespace hardware;
using namespace hardware::intel_gt;
using namespace hardware::intel_gt::gen9_3d;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "FakeIntelGpu.hpp"

static float as_float(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }

// --- The context image ---------------------------------------------------------------------------------------------------------------------

static void test_context_image() {
  std::vector<uint32_t> rcs(kLrcStateDwords), xcs(kLrcStateDwords);
  CHECK(lrc_init_gen9_rcs(ArrayView<uint32_t>(rcs.data(), rcs.size()), kRcs0MmioBase, true));
  CHECK(lrc_init_gen9_xcs(ArrayView<uint32_t>(xcs.data(), xcs.size()), kRcs0MmioBase, true));
  // gen9_rcs_offsets: the same runs as the video engine's, except that the third (one register) is R_PWR_CLK_STATE and is not posted.
  const uint32_t lri = 0x22u << 23, posted = 1u << 12;
  CHECK(rcs[1] == (lri | 27 | posted) && rcs[2] == kRcs0MmioBase + 0x244);
  CHECK(rcs[65] == (lri | 1) && rcs[66] == kRcs0MmioBase + 0x0c8);
  CHECK(xcs[65] == (lri | 1 | posted) && xcs[66] == kRcs0MmioBase + 0x200);
  int differing = 0;
  for (uint32_t i = 0; i < kLrcStateDwords; ++i) differing += rcs[i] != xcs[i];
  CHECK(differing == 2 && rcs[kCtxRingStart - 1] == kRcs0MmioBase + 0x38);
  CHECK(kLrcRenderPages == 22 && VcsDriver::kRenderGgttPagesNeeded == 28);
  std::vector<uint32_t> wrong(100);
  CHECK(!lrc_init_gen9_rcs(ArrayView<uint32_t>(wrong.data(), wrong.size()), kRcs0MmioBase, true));
}

// --- The command set -----------------------------------------------------------------------------------------------------------------------

static uint32_t length_of(std::vector<uint32_t> c) { return render_command_length(ArrayView<const uint32_t>(c.data(), c.size())); }

static void test_command_set() {
  CHECK(length_of({kMiNoop}) == 1 && length_of({kMiBatchBufferEnd}) == 1 && length_of({}) == 0);
  CHECK(length_of({kPipelineSelect3d}) == 1 && length_of({kPipelineSelect | 2}) == 0 && length_of({kPipelineSelect}) == 0);
  CHECK(length_of({kVf | 0, 0}) == 2 && length_of({kVf | 0}) == 0);          // truncated
  CHECK(length_of({k3dPrimitive | 5, 0, 3, 0, 1, 0, 0}) == 7);
  CHECK(length_of({kStateBaseAddress | 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) == 18);
  CHECK(length_of({kStateBaseAddress | 15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) == 0);   // not a length the command has
  // Commands that are not 3D state: register writes, stores, flushes, a nested batch start, an unknown 3D command.
  CHECK(length_of({(0x22u << 23) | 1, 0x2244, 0}) == 0);
  CHECK(length_of({(0x20u << 23) | 2, 0, 0, 0}) == 0);
  CHECK(length_of({(0x31u << 23) | 1, 0, 0}) == 0);
  CHECK(length_of({cmd(3, 0, 0x7F), 0}) == 0);
  // PIPE_CONTROL: six dwords, may write a value to the batch's address space, may not write a register or use the global GTT.
  CHECK(length_of({kPipeControl | 4, kPcRenderTargetCacheFlush | kPcCsStall, 0, 0, 0, 0}) == 6);
  CHECK(length_of({kPipeControl | 4, kPcPostSyncWriteImmediate, 0x1000, 0, 7, 0}) == 6);
  CHECK(length_of({kPipeControl | 4, kPcLriPostSync, 0, 0, 0, 0}) == 0);
  CHECK(length_of({kPipeControl | 4, kPcPostSyncWriteImmediate | kPcGlobalGtt, 0, 0, 0, 0}) == 0);
  CHECK(length_of({kPipeControl | 3, 0, 0, 0, 0}) == 0);
  // Whole batches.
  uint32_t end = 0;
  const std::vector<uint32_t> good = {kPipelineSelect3d, kMiNoop, kVf, 0, kMiBatchBufferEnd, 0xFFFFFFFF, 0x12345678};   // data after the end marker is not looked at
  CHECK(render_batch_is_acceptable(ArrayView<const uint32_t>(good.data(), good.size()), &end) && end == 5);
  const std::vector<uint32_t> no_end = {kPipelineSelect3d, kMiNoop};
  CHECK(!render_batch_is_acceptable(ArrayView<const uint32_t>(no_end.data(), no_end.size())));
  const std::vector<uint32_t> bad_middle = {kPipelineSelect3d, (0x22u << 23) | 1, 0x2244, 0, kMiBatchBufferEnd};
  CHECK(!render_batch_is_acceptable(ArrayView<const uint32_t>(bad_middle.data(), bad_middle.size())));
  // A data dword that looks like the end marker inside a command is not the end.
  const std::vector<uint32_t> disguised = {kVf | 0, kMiBatchBufferEnd, kMiNoop};
  CHECK(!render_batch_is_acceptable(ArrayView<const uint32_t>(disguised.data(), disguised.size())));
  // Bit flips and truncations of a real batch never get past the walker with something it does not allow, and never read out of bounds (run under the sanitizers).
  std::vector<uint32_t> image(16384);
  RenderBatch b(ArrayView<uint32_t>(image.data(), image.size()), 0x10000);
  const float red[4] = {1, 0, 0, 1};
  DrawState draw;
  draw.target = {0x100000, 16, 16, 64, SurfaceFormat::B8G8R8A8Unorm};
  CHECK(b.fill_rect(draw, {0, 0, 16, 16}, red));
  uint32_t commands_end = 0;
  CHECK(render_batch_is_acceptable(ArrayView<const uint32_t>(image.data(), b.dwords()), &commands_end) && commands_end < kStateOffset / 4);
  for (uint32_t n = 0; n < commands_end; ++n) {
    for (int bit = 0; bit < 32; ++bit) {
      std::vector<uint32_t> copy(image.begin(), image.begin() + commands_end);
      copy[n] ^= 1u << bit;
      (void)render_batch_is_acceptable(ArrayView<const uint32_t>(copy.data(), copy.size()));
    }
    (void)render_batch_is_acceptable(ArrayView<const uint32_t>(image.data(), n));
  }
}

// --- The kernels ---------------------------------------------------------------------------------------------------------------------------

static void test_kernels() {
  // The copy kernel is the intel-gpu-tools one, word for word.
  static const uint32_t reference[16] = {0x0080005a, 0x2f403ae8, 0x3a0000c0, 0x008d0040, 0x0080005a, 0x2f803ae8, 0x3a0000d0, 0x008d0040,
                                         0x02800031, 0x2e203a48, 0x0e8d0f40, 0x08840001, 0x05800031, 0x20003a40, 0x0e8d0e20, 0x90031000};
  ArrayView<const uint32_t> copy = copy_kernel();
  CHECK(copy.size() == 16);
  for (uint32_t i = 0; i < 16 && i < copy.size(); ++i) CHECK(copy[i] == reference[i]);

  // The colour kernel keeps the shape of the test's constant-colour kernel: eight moves of an immediate into consecutive registers, two per component, then the write.
  std::vector<uint32_t> flat(kFlatKernelDwords);
  const float rgba[4] = {1.0f, 0.5f, 0.25f, 0.75f};
  CHECK(flat_kernel(ArrayView<uint32_t>(flat.data(), flat.size()), rgba));
  for (uint32_t n = 0; n < 8; ++n) {
    CHECK(flat[n * 4] == 0x00600001 && flat[n * 4 + 2] == 0);
    CHECK(((flat[n * 4 + 1] >> 21) & 0xFF) == 0x70 + n);                 // g112 + n
    CHECK(as_float(flat[n * 4 + 3]) == rgba[n / 2]);
  }
  CHECK(flat[32] == 0x05800031 && flat[33] == 0x200022e0 && flat[34] == 0x0e000e00 && flat[35] == 0x90031000);
  std::vector<uint32_t> wrong_size(10);
  CHECK(!flat_kernel(ArrayView<uint32_t>(wrong_size.data(), wrong_size.size()), rgba));

  // The interpolating kernel: the copy kernel's plane-equation instruction with other registers. Each destination is a pair of registers from g112; each source is a
  // component of the attribute set up from g6; the write sends g112..g119.
  ArrayView<const uint32_t> g = gouraud_kernel();
  CHECK(g.size() == 20);
  const uint32_t src[4] = {0xc0, 0xd0, 0xe0, 0xf0};
  for (uint32_t k = 0; k < 4 && g.size() == 20; ++k) {
    CHECK(g[k * 4] == reference[0]);
    CHECK(((g[k * 4 + 1] >> 21) & 0xFF) == 112 + 2 * k && (g[k * 4 + 1] & ~(0xFFu << 21)) == (reference[1] & ~(0xFFu << 21)));
    CHECK(g[k * 4 + 2] == (0x3a000000u | src[k]) && g[k * 4 + 3] == reference[3]);
  }
  CHECK(g.size() == 20 && g[16] == 0x05800031 && ((g[18] >> 5) & 0xFF) == 112 && g[19] == 0x90031000);
  CHECK(float_bits(1.0f) == 0x3f800000);
}

#include "FakeIntelRender.hpp"

struct Pixels {
  const GpuBuffer& buffer;
  uint32_t pitch;
  uint32_t at(uint32_t x, uint32_t y) const {
    uint32_t v;
    memcpy(&v, buffer.cpu.data() + y * pitch + x * 4, 4);
    return v;
  }
};

static void test_batches_on_driver() {
  Rig rig(Generation::Gen9, VcsDriver::Engine::Render);
  rig.gpu.on_batch = execute;
  VcsDriver drv(rig.mem);
  CHECK(drv.initialize(rig.config()) && drv.state() == VcsDriver::State::Ready);
  CHECK(rig.gpu.violations.empty());

  // A 16 x 16 render target in the context's address space.
  GpuBuffer target;
  CHECK(rig.mem.allocate(4096, &target));
  const uint32_t target_address = 0x200000;
  CHECK(drv.map_buffer(target, target_address));
  Surface surface = {target_address, 16, 16, 64, SurfaceFormat::B8G8R8A8Unorm};
  CHECK(surface.valid());

  std::vector<uint32_t> image(VcsDriver::kBatchBytes / 4);
  RenderBatch batch(ArrayView<uint32_t>(image.data(), image.size()), VcsDriver::kBatchGpuAddress);
  auto run = [&]() { return drv.run_batch(ArrayView<const uint32_t>(image.data(), batch.dwords())); };
  Pixels px{target, 64};

  // A clear, then a rectangle inside it.
  const float blue[4] = {0, 0, 1, 1}, red[4] = {1, 0, 0, 1};
  DrawState draw;
  draw.target = surface;
  CHECK(batch.fill_rect(draw, {0, 0, 16, 16}, blue));
  CHECK(run() == VcsDriver::Result::Done);
  CHECK(rig.gpu.jobs == 1 && g_executed == 1 && rig.gpu.violations.empty());
  CHECK(px.at(0, 0) == 0xFF0000FF && px.at(15, 15) == 0xFF0000FF && px.at(8, 8) == 0xFF0000FF);   // B8G8R8A8: blue is the low byte
  CHECK(batch.fill_rect(draw, {4, 4, 12, 8}, red));
  CHECK(run() == VcsDriver::Result::Done);
  CHECK(px.at(5, 5) == 0xFFFF0000 && px.at(11, 7) == 0xFFFF0000 && px.at(5, 9) == 0xFF0000FF && px.at(3, 5) == 0xFF0000FF && px.at(12, 5) == 0xFF0000FF);
  // The same, clipped to a smaller window: nothing outside it changes.
  const float green[4] = {0, 1, 0, 1};
  draw.clip = {8, 8, 12, 12};
  CHECK(batch.fill_rect(draw, {0, 0, 16, 16}, green));
  CHECK(run() == VcsDriver::Result::Done);
  CHECK(px.at(9, 9) == 0xFF00FF00 && px.at(11, 11) == 0xFF00FF00 && px.at(7, 9) == 0xFF0000FF && px.at(12, 9) == 0xFF0000FF && px.at(9, 12) == 0xFF0000FF &&
        px.at(9, 7) != 0xFF00FF00);
  draw.clip = Rect();

  // A triangle list: a right triangle with the legs along the left and bottom edges, in the other winding too.
  const float white[4] = {1, 1, 1, 1};
  Vertex tri[3] = {{{0, 0, 0, 1}, {0, 0, 0, 1}}, {{0, 16, 0, 1}, {0, 0, 0, 1}}, {{16, 16, 0, 1}, {0, 0, 0, 1}}};
  CHECK(batch.triangles(draw, ArrayView<const Vertex>(tri, 3), false, white));
  CHECK(run() == VcsDriver::Result::Done);
  CHECK(px.at(2, 13) == 0xFFFFFFFF && px.at(13, 2) != 0xFFFFFFFF);
  for (int i = 0; i < 3; ++i) tri[i].color[3] = 1;
  CHECK(batch.triangles(draw, ArrayView<const Vertex>(tri, 3), true, nullptr) && batch.dwords() > 0);   // the interpolating kernel builds too (not run by the model)

  // Source over: the blend state is chosen by the draw, the render target write is the same.
  draw.blend = Blend::SourceOver;
  CHECK(batch.fill_rect(draw, {0, 0, 4, 4}, red));
  Decoded decoded(std::vector<uint32_t>(image.begin(), image.begin() + batch.dwords()));
  const uint32_t psb = decoded.one(kPsBlend);
  CHECK(psb != 0xFFFFFFFFu && (image[psb + 1] & (1u << 30)) && (image[psb + 1] & (1u << 29)) && ((image[psb + 1] >> 14) & 0x1F) == 0x03 && ((image[psb + 1] >> 9) & 0x1F) == 0x13);
  draw.blend = Blend::None;
  CHECK(batch.fill_rect(draw, {0, 0, 4, 4}, red));
  Decoded plain(std::vector<uint32_t>(image.begin(), image.begin() + batch.dwords()));
  CHECK(!(image[plain.one(kPsBlend) + 1] & (1u << 29)) && (image[plain.one(kPsBlend) + 1] & (1u << 30)));

  // The render engine's ring: every job is two PIPE_CONTROLs that write (the invalidating one and the marker), and many jobs wrap the ring.
  rig.gpu.pipe_control_writes = 0;
  const int jobs_before = rig.gpu.jobs;
  CHECK(batch.fill_rect(draw, {0, 0, 16, 16}, blue));
  for (int i = 0; i < 300; ++i) CHECK(run() == VcsDriver::Result::Done);
  CHECK(rig.gpu.jobs == jobs_before + 300 && rig.gpu.pipe_control_writes == 600 && rig.gpu.violations.empty());
  CHECK(rig.gpu.tlb_invalidates >= 300);

  // Batches the engine must not run: a video batch, a register write, a batch without an end, and one that is too big.
  std::vector<uint32_t> video = {0x70000000u | 0, 0};   // an MFX command
  CHECK(drv.run_batch(ArrayView<const uint32_t>(video.data(), video.size())) == VcsDriver::Result::Rejected);
  std::vector<uint32_t> lri = {(0x22u << 23) | 1, 0x2244, 0, kMiBatchBufferEnd};
  CHECK(drv.run_batch(ArrayView<const uint32_t>(lri.data(), lri.size())) == VcsDriver::Result::Rejected);
  std::vector<uint32_t> no_end = {kPipelineSelect3d};
  CHECK(drv.run_batch(ArrayView<const uint32_t>(no_end.data(), no_end.size())) == VcsDriver::Result::Rejected);
  std::vector<uint32_t> big(VcsDriver::kBatchBytes / 4 + 1, kMiNoop);
  big.back() = kMiBatchBufferEnd;
  CHECK(drv.run_batch(ArrayView<const uint32_t>(big.data(), big.size())) == VcsDriver::Result::Rejected);
  CHECK(drv.run_batch(ArrayView<const uint32_t>()) == VcsDriver::Result::Rejected);
  CHECK(rig.gpu.violations.empty());

  // Shutdown gives everything back and puts the global GTT the way it was.
  CHECK(drv.unmap_buffer(target, target_address));
  drv.shutdown();
  CHECK(drv.state() == VcsDriver::State::Created);
  rig.mem.free(target.cpu);
  // The engine was stopped, all 28 global GTT entries now point at the page that is never freed, and only that page is left.
  CHECK(rig.gpu.regs[kRcs0MmioBase + 0x9c] & (1u << 8));
  const uint64_t first = rig.ggtt[100] & ~0xFFFull;
  for (uint64_t i = 100; i < 100 + VcsDriver::kRenderGgttPagesNeeded; ++i) CHECK((rig.ggtt[i] & ~0xFFFull) == first);
  CHECK(rig.ggtt[100 + VcsDriver::kRenderGgttPagesNeeded] == 0xAAAA'AAAA'AAAA'AAAAull && rig.ggtt[99] == 0xAAAA'AAAA'AAAA'AAAAull);
  CHECK(rig.mem.live() == 1 && rig.gpu.violations.empty());
}

static void test_builder_arguments() {
  std::vector<uint32_t> image(VcsDriver::kBatchBytes / 4);
  RenderBatch b(ArrayView<uint32_t>(image.data(), image.size()), VcsDriver::kBatchGpuAddress);
  DrawState draw;
  draw.target = {0x200000, 16, 16, 64, SurfaceFormat::B8G8R8A8Unorm};
  const float c[4] = {1, 1, 1, 1};
  CHECK(b.fill_rect(draw, {0, 0, 16, 16}, c) && b.dwords() > kStateOffset / 4 && b.dwords() <= image.size());
  // Surfaces the hardware cannot use.
  Surface s = draw.target;
  CHECK(s.valid());
  s.pitch = 60;  CHECK(!s.valid());
  s = draw.target; s.pitch = 32;  CHECK(!s.valid());          // narrower than a row
  s = draw.target; s.width = 0;  CHECK(!s.valid());
  s = draw.target; s.width = 16385;  CHECK(!s.valid());
  s = draw.target; s.gpu_address = 0x200020;  CHECK(!s.valid());
  s = draw.target; s.gpu_address = 0;  CHECK(!s.valid());
  DrawState bad = draw;
  bad.target.pitch = 63;
  CHECK(!b.fill_rect(bad, {0, 0, 16, 16}, c) && b.dwords() == 0);
  CHECK(!b.fill_rect(draw, {4, 4, 4, 9}, c));                  // empty rectangle
  CHECK(!b.fill_rect(draw, {0, 0, 1 << 20, 16}, c));           // beyond the window coordinates
  const float nan = as_float(0x7FC00000);
  const float with_nan[4] = {nan, 0, 0, 1};
  CHECK(!b.fill_rect(draw, {0, 0, 16, 16}, with_nan));
  draw.clip = {20, 20, 30, 30};
  CHECK(!b.fill_rect(draw, {0, 0, 16, 16}, c));                // clipped away entirely
  draw.clip = Rect();
  // Triangles: whole triangles only, finite positions with w = 1, as many as fit and not more.
  Vertex tri[3] = {{{0, 0, 0, 1}, {1, 1, 1, 1}}, {{0, 16, 0, 1}, {1, 1, 1, 1}}, {{16, 16, 0, 1}, {1, 1, 1, 1}}};
  CHECK(b.triangles(draw, ArrayView<const Vertex>(tri, 3), false, c));
  CHECK(!b.triangles(draw, ArrayView<const Vertex>(tri, 2), false, c));
  CHECK(!b.triangles(draw, ArrayView<const Vertex>(), false, c));
  // The fourth component is 1/w: any positive finite number is a vertex, zero, negative and not-a-number are not.
  Vertex w2[3] = {tri[0], tri[1], tri[2]};
  w2[1].position[3] = 2.0f;
  CHECK(b.triangles(draw, ArrayView<const Vertex>(w2, 3), false, c));
  for (const float bad_w : {0.0f, -1.0f, nan, 1.0e31f}) {
    w2[1].position[3] = bad_w;
    CHECK(!b.triangles(draw, ArrayView<const Vertex>(w2, 3), false, c));
  }
  Vertex far[3] = {tri[0], tri[1], tri[2]};
  far[2].position[0] = 1.0e9f;
  CHECK(!b.triangles(draw, ArrayView<const Vertex>(far, 3), false, c));
  far[2].position[0] = nan;
  CHECK(!b.triangles(draw, ArrayView<const Vertex>(far, 3), false, c));
  CHECK(!b.triangles(draw, ArrayView<const Vertex>(tri, 3), false, with_nan));
  const uint32_t most = b.max_vertices() / 3 * 3;
  CHECK(most > 1000);
  std::vector<Vertex> many(most, tri[0]);
  CHECK(b.triangles(draw, ArrayView<const Vertex>(many.data(), many.size()), true, nullptr));
  CHECK(b.dwords() <= image.size());
  std::vector<Vertex> too_many(most + 3, tri[0]);
  CHECK(!b.triangles(draw, ArrayView<const Vertex>(too_many.data(), too_many.size()), true, nullptr));
  // Depth: a buffer the size of the target, aligned; the clear value 0 to 1.
  DrawState deep = draw;
  deep.depth = {0x400000, 16, 16};
  CHECK(deep.depth.valid() && DepthBuffer::pitch_for(16) == 128 && DepthBuffer::bytes_for(16, 16) == 128 * 32 && DepthBuffer::pitch_for(33) == 256);
  CHECK(b.clear_depth(deep, 0.5f) && b.dwords() > kStateOffset / 4);
  CHECK(b.clear_depth(deep, 0.0f) && b.clear_depth(deep, 1.0f));
  CHECK(!b.clear_depth(deep, 1.5f) && !b.clear_depth(deep, -0.1f) && !b.clear_depth(deep, nan) && !b.clear_depth(draw, 0.5f));   // (no buffer attached)
  DrawState bad_depth = deep;
  bad_depth.depth.width = 15;
  CHECK(bad_depth.depth.valid() && !b.clear_depth(bad_depth, 0.5f) && !b.triangles(bad_depth, ArrayView<const Vertex>(tri, 3), false, c) && !b.fill_rect(bad_depth, {0, 0, 4, 4}, c));   // (a valid buffer of another size than the target)
  bad_depth = deep;
  bad_depth.depth.gpu_address = 0x400010;
  CHECK(!bad_depth.depth.valid() && !b.clear_depth(bad_depth, 0.5f));   // (not 4 KiB aligned)
  bad_depth.depth.gpu_address = 0;
  CHECK(!b.clear_depth(bad_depth, 0.5f));
  deep.depth_test = deep.depth_write = true;
  CHECK(b.triangles(deep, ArrayView<const Vertex>(tri, 3), false, c));
  // The fields of the depth buffer packet and the depth state.
  {
    const std::vector<uint32_t> job(image.begin(), image.begin() + b.dwords());
    Decoded d(job);
    const uint32_t db = d.one(kDepthBuffer), ds = d.one(kWmDepthStencil);
    CHECK(db != 0xFFFFFFFFu && ds != 0xFFFFFFFFu);
    if (db != 0xFFFFFFFFu && ds != 0xFFFFFFFFu) {
      CHECK(job[db + 1] == (127u | (1u << 18) | (1u << 28) | (1u << 29)));           // pitch less one, D32_FLOAT, write, 2D
      CHECK(job[db + 2] == 0x400000 && job[db + 3] == 0 && job[db + 4] == ((15u << 4) | (15u << 18)) && job[db + 7] == 4);
      CHECK(job[ds + 1] == (1u | (1u << 1) | (3u << 5)));                              // write, test, less or equal
    }
  }
  CHECK(b.clear_depth(deep, 0.25f));
  {
    const std::vector<uint32_t> job(image.begin(), image.begin() + b.dwords());
    Decoded d(job);
    const uint32_t ds = d.one(kWmDepthStencil), vb = d.one(kVertexBuffers);
    CHECK(ds != 0xFFFFFFFFu && job[ds + 1] == (1u | (1u << 1) | (7u << 5)));          // a clear: write, test, always
    // The vertices carry the clear depth, and the colour write is masked off in every blend state entry.
    const uint32_t base = VcsDriver::kBatchGpuAddress, at = (job[vb + 2] - base) / 4;
    CHECK(as_float(image[at + 2]) == 0.25f && as_float(image[at + 8 + 2]) == 0.25f && as_float(image[at + 16 + 2]) == 0.25f);
    const uint32_t bs = d.one(kBlendStatePointers);
    CHECK(bs != 0xFFFFFFFFu && (image[(job[bs + 1] & ~1u) / 4 + 1] & 0xF) == 0xF);
  }
  // Without a depth buffer attached, a test or write in the state is left out.
  DrawState test_only = draw;
  test_only.depth_test = test_only.depth_write = true;
  CHECK(b.triangles(test_only, ArrayView<const Vertex>(tri, 3), false, c));
  {
    const std::vector<uint32_t> job(image.begin(), image.begin() + b.dwords());
    Decoded d(job);
    const uint32_t ds = d.one(kWmDepthStencil), db = d.one(kDepthBuffer);
    CHECK(job[ds + 1] == (3u << 5) && job[db + 1] == 0);
  }
  // Copy: the rectangle lies inside the source, and what falls outside the destination is clipped.
  Surface src = {0x300000, 8, 8, 64, SurfaceFormat::R8G8B8A8Unorm};
  CHECK(b.copy(draw.target, src, {0, 0, 8, 8}, 2, 2));
  CHECK(b.copy(draw.target, src, {0, 0, 8, 8}, 12, 12));          // partly outside
  CHECK(b.copy(draw.target, src, {0, 0, 8, 8}, -4, -4));
  CHECK(!b.copy(draw.target, src, {0, 0, 9, 8}, 0, 0));           // beyond the source
  CHECK(!b.copy(draw.target, src, {0, 0, 8, 8}, 16, 0));          // entirely outside the destination
  CHECK(!b.copy(draw.target, src, {2, 2, 2, 2}, 0, 0));
  Surface unaligned = src; unaligned.gpu_address = 0x300001;
  CHECK(!b.copy(draw.target, unaligned, {0, 0, 8, 8}, 0, 0));
  // Textured triangles: the copy kernel with a sampler, two surfaces in the binding table (the target, then the texture), and the vertices as they are for the other triangle jobs
  // with the texture coordinates in the colour slot.
  {
    Surface texture = {0x300000, 8, 8, 64 * 1, SurfaceFormat::B8G8R8A8Unorm};
    texture.pitch = 64;
    Vertex uv[3] = {{{1, 1, 0.5f, 1}, {0.25f, 0.5f, 7, 7}}, {{1, 14, 0.5f, 1}, {0.25f, 0.75f, 7, 7}}, {{14, 14, 0.5f, 1}, {0.5f, 0.75f, 7, 7}}};
    Sampler nearest_clamp;
    CHECK(b.textured_triangles(draw, ArrayView<const Vertex>(uv, 3), texture, nearest_clamp));
    std::vector<uint32_t> job(image.begin(), image.begin() + b.dwords());
    uint32_t end = 0;
    CHECK(render_batch_is_acceptable(ArrayView<const uint32_t>(job.data(), job.size()), &end));
    Decoded d(job);
    const uint32_t ps = d.one(kPs), bt = d.one(kBindingTablePointersPs), sp = d.one(kSamplerStatePointersPs), vb = d.one(kVertexBuffers);
    CHECK(ps != 0xFFFFFFFFu && bt != 0xFFFFFFFFu && sp != 0xFFFFFFFFu && vb != 0xFFFFFFFFu);
    if (ps != 0xFFFFFFFFu && bt != 0xFFFFFFFFu && sp != 0xFFFFFFFFu && vb != 0xFFFFFFFFu) {
      const uint32_t base = VcsDriver::kBatchGpuAddress;
      CHECK(((job[ps + 3] >> 18) & 0xFF) == 2 && (job[ps + 3] & (1u << 27)));           // two binding table entries, a sampler
      const uint32_t kernel = job[ps + 1] / 4;
      for (uint32_t i = 0; i < 16; ++i) CHECK(image[kernel + i] == copy_kernel()[i]);   // the intel-gpu-tools copy kernel, unchanged
      const uint32_t table = job[bt + 1] / 4, target_state = image[table], texture_state = image[table + 1];
      CHECK(target_state != texture_state && image[texture_state / 4 + 8] == 0x300000 && image[target_state / 4 + 8] == static_cast<uint32_t>(draw.target.gpu_address));
      CHECK(((image[texture_state / 4 + 2]) & 0x3FFF) == 7 && ((image[texture_state / 4 + 2] >> 16) & 0x3FFF) == 7 && image[texture_state / 4 + 3] == 63);
      const uint32_t sampler = job[sp + 1] / 4;
      CHECK(image[sampler] == 0 && image[sampler + 3] == (2u | (2u << 3) | (2u << 6)));   // nearest, clamped
      // The vertices: position then (u, v, 7, 7) at the offsets of the other triangle jobs.
      const uint32_t at = (job[vb + 2] - base) / 4;
      CHECK(as_float(image[at + 4]) == 0.25f && as_float(image[at + 5]) == 0.5f && as_float(image[at + 8 + 4 + 1]) == 0.75f && job[vb + 1] == (32u | (1u << 14)));
    }
    // Linear filtering and wrapping change only the sampler state.
    Sampler wrap_linear;
    wrap_linear.linear = true;
    wrap_linear.repeat_u = wrap_linear.repeat_v = true;
    CHECK(b.textured_triangles(draw, ArrayView<const Vertex>(uv, 3), texture, wrap_linear));
    {
      std::vector<uint32_t> second(image.begin(), image.begin() + b.dwords());
      Decoded d2(second);
      const uint32_t sp2 = d2.one(kSamplerStatePointersPs);
      CHECK(sp2 != 0xFFFFFFFFu && image[second[sp2 + 1] / 4] == ((1u << 14) | (1u << 17)) && image[second[sp2 + 1] / 4 + 3] == 2u);   // r clamped; s and t wrap
    }
    Sampler mixed;
    mixed.repeat_u = true;
    CHECK(b.textured_triangles(draw, ArrayView<const Vertex>(uv, 3), texture, mixed));
    {
      std::vector<uint32_t> third(image.begin(), image.begin() + b.dwords());
      Decoded d3(third);
      const uint32_t sp3 = d3.one(kSamplerStatePointersPs);
      CHECK(sp3 != 0xFFFFFFFFu && image[third[sp3 + 1] / 4 + 3] == (2u | (2u << 3)));    // s (u) wraps, t (v) clamps
    }
    // Refused: the texture is the target (or overlaps it), is not a valid surface, a vertex coordinate is not a number, the vertex count is not whole triangles.
    Surface same = draw.target;
    CHECK(!b.textured_triangles(draw, ArrayView<const Vertex>(uv, 3), same, nearest_clamp));
    Surface overlapping = texture;
    overlapping.gpu_address = draw.target.gpu_address + 64;
    CHECK(!b.textured_triangles(draw, ArrayView<const Vertex>(uv, 3), overlapping, nearest_clamp));
    Surface bad_texture = texture;
    bad_texture.gpu_address = 0x300001;
    CHECK(!b.textured_triangles(draw, ArrayView<const Vertex>(uv, 3), bad_texture, nearest_clamp));
    Vertex nan_uv[3] = {uv[0], uv[1], uv[2]};
    nan_uv[1].color[0] = nan_uv[1].color[0] / 0.0f * 0.0f;
    CHECK(!b.textured_triangles(draw, ArrayView<const Vertex>(nan_uv, 3), texture, nearest_clamp));
    CHECK(!b.textured_triangles(draw, ArrayView<const Vertex>(uv, 2), texture, nearest_clamp));
    CHECK(!b.textured_triangles(draw, ArrayView<const Vertex>(), texture, nearest_clamp));
  }
  // Images that are too small, or not whole pages, are refused.
  std::vector<uint32_t> small(1024);
  RenderBatch tiny(ArrayView<uint32_t>(small.data(), small.size()), VcsDriver::kBatchGpuAddress);
  CHECK(!tiny.fill_rect(draw, {0, 0, 16, 16}, c) && tiny.max_vertices() == 0);
  std::vector<uint32_t> odd(kStateOffset / 4 * 2 + 1);
  RenderBatch crooked(ArrayView<uint32_t>(odd.data(), odd.size()), VcsDriver::kBatchGpuAddress);
  CHECK(!crooked.fill_rect(draw, {0, 0, 16, 16}, c));
  RenderBatch misplaced(ArrayView<uint32_t>(image.data(), image.size()), VcsDriver::kBatchGpuAddress + 4);
  CHECK(!misplaced.fill_rect(draw, {0, 0, 16, 16}, c));
}

static void test_driver_failures() {
  // The render engine is Gen9 only.
  {
    Rig rig(Generation::Gen12, VcsDriver::Engine::Render);
    VcsDriver d(rig.mem);
    CHECK(!d.initialize(rig.config()) && d.state() == VcsDriver::State::Failed && rig.mem.blocks.empty() && rig.ggtt_untouched());
  }
  // The render power well is not granted: nothing is left behind.
  {
    Rig rig(Generation::Gen9, VcsDriver::Engine::Render);
    rig.gpu.deny_render = true;
    VcsDriver d(rig.mem);
    CHECK(!d.initialize(rig.config()) && d.state() == VcsDriver::State::Failed && rig.mem.live() == 0 && rig.ggtt_untouched());
  }
  // Not enough room in the global GTT for the 28 pages.
  {
    Rig rig(Generation::Gen9, VcsDriver::Engine::Render);
    VcsDriver::Config cfg = rig.config();
    cfg.ggtt_first_page = rig.ggtt.size() - 27;
    VcsDriver d(rig.mem);
    CHECK(!d.initialize(cfg) && d.state() == VcsDriver::State::Failed && rig.ggtt_untouched());
  }
  // Running out of memory at any allocation leaves nothing behind.
  for (int budget = 0; budget < 7; ++budget) {
    Rig rig(Generation::Gen9, VcsDriver::Engine::Render);
    rig.mem.budget = budget;
    VcsDriver d(rig.mem);
    CHECK(!d.initialize(rig.config()) && d.state() == VcsDriver::State::Failed && rig.mem.live() == 0 && rig.ggtt_untouched());
  }
  // A hang wedges the engine, and the memory it might still write to is not freed.
  {
    Rig rig(Generation::Gen9, VcsDriver::Engine::Render);
    VcsDriver d(rig.mem);
    CHECK(d.initialize(rig.config()));
    std::vector<uint32_t> image(VcsDriver::kBatchBytes / 4);
    RenderBatch b(ArrayView<uint32_t>(image.data(), image.size()), VcsDriver::kBatchGpuAddress);
    DrawState draw;
    draw.target = {0x200000, 16, 16, 64, SurfaceFormat::B8G8R8A8Unorm};
    const float c[4] = {1, 1, 1, 1};
    GpuBuffer target;
    CHECK(rig.mem.allocate(4096, &target) && d.map_buffer(target, 0x200000));
    CHECK(b.fill_rect(draw, {0, 0, 16, 16}, c));
    rig.gpu.hang = true;
    const int live = rig.mem.live();
    CHECK(d.run_batch(ArrayView<const uint32_t>(image.data(), b.dwords())) == VcsDriver::Result::Timeout);
    CHECK(d.state() == VcsDriver::State::Wedged && rig.mem.live() == live);
    CHECK(d.run_batch(ArrayView<const uint32_t>(image.data(), b.dwords())) == VcsDriver::Result::Rejected);
  }
}

int main() {
  test_context_image();
  test_command_set();
  test_kernels();
  test_builder_arguments();
  test_batches_on_driver();
  test_driver_failures();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("intel render tests passed\n");
  return 0;
}
