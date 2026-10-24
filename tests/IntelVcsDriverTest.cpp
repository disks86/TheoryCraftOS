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

// Host-side tests for the Intel video engine driver (VcsDriver) against a simulated GPU. The simulated GPU does what the hardware does with the things the driver gives it:
// it grants power requests, resolves the ring, status page and context through the global GTT entries the driver wrote, reads the context image the way a context restore
// does, walks the context's page tables for the batch, runs the ring commands, writes the completion marker and the status entries - and records every access that goes
// to memory that is not (or no longer) the driver's.
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

static void test_init_and_jobs() {
  Rig rig;
  VcsDriver::Config cfg = rig.config();
  {
    VcsDriver drv(rig.mem);
    CHECK(drv.state() == VcsDriver::State::Created);
    CHECK(drv.initialize(cfg) && drv.state() == VcsDriver::State::Ready);
    // Power: both wells were asked for and given back.
    CHECK(rig.gpu.ack(0x130044) == 0 && rig.gpu.ack(0x0D88) == 0 && rig.gpu.regs[0xA188] == 0x00010000 && rig.gpu.regs[0xA270] == 0x00010000);
    // The PAT table was programmed.
    CHECK(rig.gpu.regs[0x40E0] == 0x00030907 && rig.gpu.regs[0x40E4] == 0x3B2B1B0B);
    // The engine was told about the status page; run list mode on.
    CHECK(rig.gpu.regs[kBase + 0x80] == (100u + 4) * 4096 && rig.gpu.regs[kBase + 0x29c] == 0x80008000);
    // Eight global GTT entries changed, the neighbours did not.
    CHECK(rig.ggtt[99] == 0xAAAA'AAAA'AAAA'AAAAull && rig.ggtt[108] == 0xAAAA'AAAA'AAAA'AAAAull);
    for (int i = 100; i < 108; ++i) CHECK(rig.ggtt[i] & 1);
    CHECK(rig.gpu.violations.empty());

    // A job.
    const std::vector<uint32_t> batch = simple_batch();
    CHECK(VcsDriver::batch_is_acceptable(ArrayView<const uint32_t>(batch.data(), batch.size())));
    CHECK(drv.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Done);
    CHECK(rig.gpu.jobs == 1 && rig.gpu.batches.size() == 1 && rig.gpu.batches[0] == batch && rig.gpu.tlb_invalidates == 1);
    CHECK(drv.last_sequence() == 1 && rig.gpu.ack(0x0D88) == 0 && rig.gpu.violations.empty());

    // Many jobs: the ring wraps (a job is 72 bytes, the ring 16 KiB).
    for (int i = 0; i < 400; ++i) CHECK(drv.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Done);
    CHECK(rig.gpu.jobs == 401 && rig.gpu.batches.size() == 401 && drv.last_sequence() == 401 && rig.gpu.violations.empty());
    for (const auto& b : rig.gpu.batches) CHECK(b == batch);

    // Buffers: mapped into the context's address space where the batch can reach them.
    GpuBuffer surface;
    CHECK(rig.mem.allocate(2 * 4096, &surface));
    CHECK(!drv.map_buffer(surface, 0x00010000));      // below the first allowed address
    CHECK(!drv.map_buffer(surface, 0x00100800));      // unaligned
    CHECK(!drv.map_buffer(surface, 0xFFFFF000));      // runs past 4 GiB
    CHECK(drv.map_buffer(surface, 0x00200000));
    CHECK(drv.unmap_buffer(surface, 0x00200000));
    CHECK(!drv.map_buffer(GpuBuffer(), 0x00200000));
    drv.shutdown();
    CHECK(drv.state() == VcsDriver::State::Created);
    rig.mem.free(surface.cpu);
    // The engine was stopped, and the global GTT entries now point at the page that is never freed.
    CHECK(rig.gpu.regs[kBase + 0x9c] & (1u << 8));
    uint64_t first = rig.ggtt[100] & ~0xFFFull;
    for (int i = 100; i < 108; ++i) CHECK((rig.ggtt[i] & ~0xFFFull) == first);
    bool dead = false;
    CHECK(rig.mem.find(first, 4096, &dead) && !dead);
    CHECK(rig.mem.live() == 1);  // only that page is left
    CHECK(rig.gpu.violations.empty());
    // It can start again.
    CHECK(drv.initialize(cfg) && drv.state() == VcsDriver::State::Ready);
    CHECK(drv.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Done);
  }
  CHECK(rig.mem.live() == 2);  // the destructor shut it down; the first scratch page and the second one remain
  CHECK(rig.gpu.violations.empty());
}

static void test_batch_validation() {
  using V = VcsDriver;
  auto ok = [](std::vector<uint32_t> v) { return V::batch_is_acceptable(ArrayView<const uint32_t>(v.data(), v.size())); };
  const uint32_t end = 0x0Au << 23, noop = 0;
  CHECK(ok({end}) && ok({noop, noop, end}));
  CHECK(!ok({}) && !ok({noop}) && !ok({end, noop}) && !ok({end, end}));
  CHECK(ok({0x13000082u, 0, 0, 0, end}));                      // flush
  CHECK(!ok({0x13004082u, 0, 0, 0, end}));                     // a flush that writes memory
  CHECK(!ok({0x13000082u, 0, 0, end}));                        // a flush cut short
  CHECK(ok({0x70000003u, 0, 0, 0, 0, end}));                   // MFX_PIPE_MODE_SELECT, 5 dwords
  CHECK(!ok({0x70000003u, 0, 0, 0, end}));                     // its length runs past the end
  CHECK(!ok({0x70000003u, 0, 0, end, end}));                   // an end marker inside the command's data does not end the batch
  CHECK(!ok({0x18800101u, 0, 0, end}));                        // MI_BATCH_BUFFER_START
  CHECK(!ok({(0x22u << 23) | 1u, 0x2000, 0, end}));            // MI_LOAD_REGISTER_IMM
  CHECK(!ok({(0x20u << 23) | 2u, 0, 0, 0, end}));              // MI_STORE_DWORD_IMM
  CHECK(!ok({0x7B000003u, 0, 0, 0, 0, end}));                  // a command of another pipeline (3D)
  CHECK(!ok({0x60000003u, 0, 0, 0, 0, end}));                  // not the MFX pipeline
  std::vector<uint32_t> big(VcsDriver::kBatchBytes / 4 + 1, noop);
  big.back() = end;
  CHECK(!ok(big));
  std::vector<uint32_t> fits(VcsDriver::kBatchBytes / 4, noop);
  fits.back() = end;
  CHECK(ok(fits));
}

static void test_failures() {
  {
    Rig rig;
    VcsDriver::Config cfg = rig.config();
    cfg.ggtt_first_page = 1020;  // not enough room for eight pages
    VcsDriver d(rig.mem);
    CHECK(!d.initialize(cfg) && d.state() == VcsDriver::State::Failed && rig.mem.blocks.empty() && rig.ggtt_untouched());
    CHECK(!d.initialize(rig.config()));  // a failed driver stays failed
  }
  {
    VcsDriver::Config bad;
    FakeMemory mem;
    VcsDriver d(mem);
    CHECK(!d.initialize(bad) && d.state() == VcsDriver::State::Failed);
    const std::vector<uint32_t> batch = simple_batch();
    CHECK(d.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Rejected);
  }
  for (int which = 0; which < 2; ++which) {
    Rig rig;
    (which == 0 ? rig.gpu.deny_gt : rig.gpu.deny_media) = true;
    VcsDriver d(rig.mem);
    CHECK(!d.initialize(rig.config()) && d.state() == VcsDriver::State::Failed);
    CHECK(rig.mem.blocks.empty() && rig.ggtt_untouched() && rig.gpu.ack(0x130044) == 0 && rig.gpu.ack(0x0D88) == 0);
    CHECK(rig.gpu.regs.count(kBase + 0x80) == 0);  // the engine was never pointed at anything
  }
  // Out of memory at every step: nothing is left allocated, the global GTT is as it was, the engine was never enabled.
  bool succeeded = false;
  for (int budget = 0; budget < 20; ++budget) {
    Rig rig;
    rig.mem.budget = budget;
    {
      VcsDriver d(rig.mem);
      const bool ok = d.initialize(rig.config());
      if (!ok) {
        CHECK(d.state() == VcsDriver::State::Failed && rig.mem.live() == 0 && rig.ggtt_untouched() && rig.gpu.regs.count(kBase + 0x80) == 0);
        CHECK(rig.gpu.ack(0x130044) == 0 && rig.gpu.ack(0x0D88) == 0);
      } else {
        succeeded = true;
        CHECK(d.state() == VcsDriver::State::Ready);
      }
    }
    CHECK(rig.gpu.violations.empty());
  }
  CHECK(succeeded);
}

static void test_hang() {
  Rig rig;
  const std::vector<uint32_t> batch = simple_batch();
  int live_before = 0;
  {
    VcsDriver d(rig.mem);
    CHECK(d.initialize(rig.config()));
    CHECK(d.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Done);
    rig.gpu.hang = true;
    CHECK(d.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Timeout);
    CHECK(d.state() == VcsDriver::State::Wedged);
    CHECK(rig.gpu.regs[kBase + 0x9c] & (1u << 8));  // the ring was told to stop
    CHECK(rig.gpu.ack(0x0D88) == 0);                // power request withdrawn
    CHECK(d.run_batch(ArrayView<const uint32_t>(batch.data(), batch.size())) == VcsDriver::Result::Rejected);
    GpuBuffer b;
    CHECK(rig.mem.allocate(4096, &b));
    CHECK(!d.map_buffer(b, 0x00200000));
    rig.mem.free(b.cpu);
    live_before = rig.mem.live();
  }
  // The driver is gone, but memory the engine might still write to was not given back.
  CHECK(rig.mem.live() == live_before && live_before > 0);
  CHECK(rig.gpu.violations.empty());

  // A stuck engine that never reports idle at shutdown is treated the same.
  Rig rig2;
  {
    VcsDriver d(rig2.mem);
    CHECK(d.initialize(rig2.config()));
    rig2.gpu.never_idle = true;
    const int live = rig2.mem.live();
    d.shutdown();
    CHECK(d.state() == VcsDriver::State::Wedged && rig2.mem.live() == live);
  }
}

int main() {
  test_init_and_jobs();
  test_batch_validation();
  test_failures();
  test_hang();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("intel vcs driver tests passed\n");
  return 0;
}
