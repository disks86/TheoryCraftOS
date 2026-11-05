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


// Host-side tests for the Gen11 (Ice Lake) form of the Intel video engine driver (VcsDriver with Generation::Gen11) against a simulated GPU (FakeIntelGpu.hpp). Gen11 sits
// between the generations the driver knew: the video box's own power well, the submit queue and twelve-entry status buffer of Gen12 (but with Gen8-format entries), the
// 32-bit address space, Gen9 register list and context image of Gen9 (with Gen11's context control), the eight single-register PAT, the MOCS table, and a descriptor with
// the context ID, instance and class fields where Gen12 has them.
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
  Rig rig(Generation::Gen11);
  VcsDriver::Config cfg = rig.config();
  {
    VcsDriver drv(rig.mem);
    CHECK(drv.initialize(cfg) && drv.state() == VcsDriver::State::Ready && drv.generation() == Generation::Gen11);
    // Power: the GT and the first video box's wells; not the Gen9 media well.
    CHECK(rig.gpu.ack(0x130044) == 0 && rig.gpu.ack(0x0D50) == 0 && rig.gpu.regs[0xA188] == 0x00010000 && rig.gpu.regs[0xA540] == 0x00010000 && rig.gpu.regs.count(0xA270) == 0);
    // The PAT, one register per index (icl_setup_private_ppat): write-back + LLC, write-combining, write-back with the eLLC override, uncached, then four write-back ages.
    const uint32_t pat[8] = {0x07, 0x09, 0x03, 0x00, 0x0B, 0x1B, 0x2B, 0x3B};
    for (uint32_t i = 0; i < 8; ++i) CHECK(rig.gpu.regs[0x40E0 + 4 * i] == pat[i]);
    CHECK(rig.gpu.regs.count(0x4800) == 0);   // not the Gen12 registers
    // The video engine's MOCS table: entry 1 is "whatever the page table says" (what the decode commands select), 0 uncached.
    CHECK(rig.gpu.regs.count(0xC900) == 1 && rig.gpu.regs.count(0xC904) == 1 && rig.gpu.regs[0xC904] != rig.gpu.regs[0xC900] && rig.gpu.regs.count(0xC900 + 63 * 4) == 1);
    // The engine: status page, legacy mode off (the queue is used), ring running.
    CHECK(rig.gpu.regs[kB + 0x80] == (100u + 4) * 4096 && rig.gpu.regs[kB + 0x29c] == 0x00080008);
    CHECK(rig.gpu.regs[kB + 0x3a0] == (0xFFFFu << 16 | 11u << 8 | 11u));
    CHECK(rig.ggtt[99] == 0xAAAA'AAAA'AAAA'AAAAull && rig.ggtt[108] == 0xAAAA'AAAA'AAAA'AAAAull);
    CHECK(rig.gpu.violations.empty());

    const std::vector<uint32_t> batch = simple_batch();
    CHECK(drv.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Done);
    CHECK(rig.gpu.jobs == 1 && rig.gpu.batches.size() == 1 && rig.gpu.batches[0] == batch && rig.gpu.tlb_invalidates == 1);
    CHECK(drv.last_sequence() == 1 && rig.gpu.ack(0x0D50) == 0 && rig.gpu.violations.empty());
    // The queue was loaded: port 0 with the context (valid, privileged, 32-bit mode 1, context ID 1 at bit 37, instance 0, class 1), port 1 empty.
    const uint64_t desc = uint64_t{rig.gpu.elsq[1]} << 32 | rig.gpu.elsq[0];
    CHECK(rig.gpu.elsq[2] == 0 && rig.gpu.elsq[3] == 0 && (desc & 1) && (desc & 0x100) && ((desc >> 3) & 3) == 1 && (desc >> 61) == 1 && ((desc >> 37) & 0x7FF) == 1 &&
          ((desc >> 48) & 0x3F) == 0 && (desc & 4));   // (bit 2: force restore)

    // Many jobs: the ring wraps and the twelve-entry status buffer wraps with it.
    for (int i = 0; i < 400; ++i) CHECK(drv.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Done);
    CHECK(rig.gpu.jobs == 401 && rig.gpu.batches.size() == 401 && drv.last_sequence() == 401 && rig.gpu.violations.empty());

    // Buffers in the 32-bit space.
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
    Rig rig(Generation::Gen11);
    (which == 0 ? rig.gpu.deny_gt : rig.gpu.deny_media) = true;
    VcsDriver d(rig.mem);
    CHECK(!d.initialize(rig.config()) && d.state() == VcsDriver::State::Failed);
    CHECK(rig.mem.blocks.empty() && rig.ggtt_untouched() && rig.gpu.ack(0x130044) == 0 && rig.gpu.ack(0x0D50) == 0 && rig.gpu.regs.count(kB + 0x80) == 0);
  }
  bool succeeded = false;
  for (int budget = 0; budget < 30; ++budget) {
    Rig rig(Generation::Gen11);
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
  // Gen11 has the video engine only: the render engine path is Gen9's.
  Rig rig(Generation::Gen11, VcsDriver::Engine::Render);
  VcsDriver d(rig.mem);
  CHECK(!d.initialize(rig.config()));
}

static void test_hang() {
  Rig rig(Generation::Gen11);
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
  printf("intel gen11 driver tests passed\n");
  return 0;
}
