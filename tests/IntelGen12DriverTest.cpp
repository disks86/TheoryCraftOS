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

// Host-side tests for the Gen12 (Xe-LP) form of the Intel video engine driver (VcsDriver with Generation::Gen12) against a simulated GPU (FakeIntelGpu.hpp): the video
// box's own power well, the Gen12 PAT registers, a four-level address space for the context (the simulated engine walks the page-map level 4 page), the Gen12 context
// image (its register list and the ring MI mode), the descriptor with the context ID, engine instance and class, the submit queue (ELSQ) and the twelve-entry context
// status buffer in the Gen12 format. Exactly the checks IntelVcsDriverTest makes of the Gen9 form, for the parts that are the same.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.hpp"
#include "Hardware/VideoCodec/Intel/IntelMfxCommands.hpp"

using namespace hardware;
using namespace hardware::intel_gt;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "FakeIntelGpu.hpp"

static std::vector<uint32_t> simple_batch() {
  std::vector<uint32_t> buf(64);
  intel_mfx::DwordWriter w(ArrayView<uint32_t>(buf.data(), buf.size()));
  intel_mfx::emit_mi_flush(w);
  intel_mfx::emit_pipe_mode_select(w, true, false);
  intel_mfx::emit_batch_end(w);
  buf.resize(w.count());
  return buf;
}

constexpr uint32_t kB = kVd0MmioBase;

static void test_init_and_jobs() {
  Rig rig(Generation::Gen12);
  VcsDriver::Config cfg = rig.config();
  {
    VcsDriver drv(rig.mem);
    CHECK(drv.initialize(cfg) && drv.state() == VcsDriver::State::Ready);
    // Power: the GT and the first video box's wells were asked for and given back; the media well of Gen9 was not touched.
    CHECK(rig.gpu.ack(0x130044) == 0 && rig.gpu.ack(0x0D50) == 0 && rig.gpu.regs[0xA188] == 0x00010000 && rig.gpu.regs[0xA540] == 0x00010000 && rig.gpu.regs.count(0xA270) == 0);
    // The PAT: write-back, write-combining, write-through, uncached, then write-back.
    const uint32_t pat[8] = {3, 1, 2, 0, 3, 3, 3, 3};
    for (uint32_t i = 0; i < 8; ++i) CHECK(rig.gpu.regs[0x4800 + 4 * i] == pat[i]);
    CHECK(rig.gpu.regs.count(0x40E0) == 0);
    // The engine: status page, legacy mode off, ring running.
    CHECK(rig.gpu.regs[kB + 0x80] == (100u + 4) * 4096 && rig.gpu.regs[kB + 0x29c] == 0x00080008);
    CHECK(rig.gpu.regs[kB + 0x3a0] == (0xFFFFu << 16 | 11u << 8 | 11u));
    CHECK(rig.ggtt[99] == 0xAAAA'AAAA'AAAA'AAAAull && rig.ggtt[108] == 0xAAAA'AAAA'AAAA'AAAAull);
    CHECK(rig.gpu.violations.empty());

    const std::vector<uint32_t> batch = simple_batch();
    CHECK(drv.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Done);
    CHECK(rig.gpu.jobs == 1 && rig.gpu.batches.size() == 1 && rig.gpu.batches[0] == batch && rig.gpu.tlb_invalidates == 1);
    CHECK(drv.last_sequence() == 1 && rig.gpu.ack(0x0D50) == 0 && rig.gpu.violations.empty());
    // The queue was loaded, port 0 with the context: valid, privileged, four-level mode, context ID 1, instance 0, class 1.
    CHECK(rig.gpu.elsq[2] == 0 && rig.gpu.elsq[3] == 0 && (rig.gpu.elsq[0] & 0x1) && (rig.gpu.elsq[0] & 0x100) && ((rig.gpu.elsq[0] >> 3) & 3) == 3);
    CHECK(((uint64_t{rig.gpu.elsq[1]} << 32 | rig.gpu.elsq[0]) >> 61) == 1 && ((rig.gpu.elsq[1] >> 5) & 0x7FF) == 1);

    // Many jobs: the ring wraps and the twelve-entry status buffer wraps with it.
    for (int i = 0; i < 400; ++i) CHECK(drv.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Done);
    CHECK(rig.gpu.jobs == 401 && rig.gpu.batches.size() == 401 && drv.last_sequence() == 401 && rig.gpu.violations.empty());

    // Buffers in the four-level space, reachable by the batch.
    GpuBuffer surface;
    CHECK(rig.mem.allocate(2 * 4096, &surface));
    CHECK(!drv.map_buffer(surface, 0x00010000) && !drv.map_buffer(surface, 0x00100800) && !drv.map_buffer(surface, 0xFFFFF000));
    CHECK(drv.map_buffer(surface, 0x00200000));
    CHECK(drv.unmap_buffer(surface, 0x00200000));
    drv.shutdown();
    CHECK(drv.state() == VcsDriver::State::Created);
    rig.mem.free(surface.cpu);
    CHECK(rig.gpu.regs[kB + 0x9c] & (1u << 8));
    CHECK(rig.mem.live() == 1 && rig.gpu.violations.empty());
    CHECK(drv.initialize(cfg) && drv.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Done);
  }
  CHECK(rig.mem.live() == 2 && rig.gpu.violations.empty());
}

static void test_failures() {
  for (int which = 0; which < 2; ++which) {
    Rig rig(Generation::Gen12);
    (which == 0 ? rig.gpu.deny_gt : rig.gpu.deny_media) = true;
    VcsDriver d(rig.mem);
    CHECK(!d.initialize(rig.config()) && d.state() == VcsDriver::State::Failed);
    CHECK(rig.mem.blocks.empty() && rig.ggtt_untouched() && rig.gpu.ack(0x130044) == 0 && rig.gpu.ack(0x0D50) == 0 && rig.gpu.regs.count(kB + 0x80) == 0);
  }
  // Out of memory at every step (the four-level space needs more pages).
  bool succeeded = false;
  for (int budget = 0; budget < 30; ++budget) {
    Rig rig(Generation::Gen12);
    rig.mem.budget = budget;
    {
      VcsDriver d(rig.mem);
      const bool ok = d.initialize(rig.config());
      if (!ok) CHECK(d.state() == VcsDriver::State::Failed && rig.mem.live() == 0 && rig.ggtt_untouched() && rig.gpu.regs.count(kB + 0x80) == 0 && rig.gpu.ack(0x0D50) == 0);
      else succeeded = true;
    }
    CHECK(rig.gpu.violations.empty());
  }
  CHECK(succeeded);
}

static void test_hang() {
  Rig rig(Generation::Gen12);
  const std::vector<uint32_t> batch = simple_batch();
  int live_before = 0;
  {
    VcsDriver d(rig.mem);
    CHECK(d.initialize(rig.config()));
    CHECK(d.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Done);
    rig.gpu.hang = true;
    CHECK(d.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Timeout);
    CHECK(d.state() == VcsDriver::State::Wedged && (rig.gpu.regs[kB + 0x9c] & (1u << 8)) && rig.gpu.ack(0x0D50) == 0);
    CHECK(d.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Rejected);
    live_before = rig.mem.live();
  }
  CHECK(rig.mem.live() == live_before && live_before > 0 && rig.gpu.violations.empty());
}

int main() {
  test_init_and_jobs();
  test_failures();
  test_hang();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("intel gen12 driver tests passed\n");
  return 0;
}
