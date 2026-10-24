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

// Page-aligned memory at made-up device addresses (above 4 GiB, so nothing can pass for a CPU address). Freed blocks stay allocated but are marked dead, so a later
// access by the simulated GPU is noticed.
struct FakeMemory final : IGpuMemory {
  struct Block {
    uint8_t* bytes;
    uint64_t size, dma;
    bool live;
  };
  std::vector<Block> blocks;
  uint64_t next_dma = 0x2'0000'0000ull;
  int budget = 1 << 30;
  bool allocate(uint64_t size, GpuBuffer* out) override {
    if (budget <= 0 || size == 0 || size % 4096) return false;
    --budget;
    uint8_t* p = static_cast<uint8_t*>(aligned_alloc(4096, size));
    memset(p, 0, size);
    blocks.push_back({p, size, next_dma, true});
    out->cpu = ByteView(p, size);
    out->dma = next_dma;
    next_dma += size + 0x1000;
    return true;
  }
  void free(ByteView cpu) override {
    for (Block& b : blocks)
      if (b.bytes == cpu.data()) { CHECK(b.live); b.live = false; memset(b.bytes, 0xDD, b.size); return; }
    CHECK(false);  // freed something that is not ours
  }
  int live() const { int n = 0; for (const Block& b : blocks) n += b.live; return n; }
  ~FakeMemory() { for (Block& b : blocks) ::free(b.bytes); }
  // The CPU address for a device address, and whether it is live.
  uint8_t* find(uint64_t dma, uint64_t length, bool* dead) const {
    for (const Block& b : blocks)
      if (dma >= b.dma && dma + length <= b.dma + b.size) { *dead = !b.live; return b.bytes + (dma - b.dma); }
    return nullptr;
  }
};

constexpr uint32_t kBase = kVcs0MmioBase;

struct FakeGpu final : IRegisterBank {
  std::vector<uint64_t>* ggtt;
  FakeMemory* mem;
  std::map<uint64_t, uint32_t> regs;
  std::vector<std::string> violations;
  std::vector<std::vector<uint32_t>> batches;
  bool deny_gt = false, deny_media = false, hang = false, never_idle = false;
  uint32_t elsp[4] = {};
  int elsp_count = 0;
  uint32_t csb_wp = 5;
  int tlb_invalidates = 0;
  int jobs = 0;

  explicit FakeGpu(std::vector<uint64_t>* table, FakeMemory* memory) : ggtt(table), mem(memory) {}

  void violation(const std::string& what) { violations.push_back(what); }

  uint32_t ack(uint32_t reg) const { auto it = regs.find(reg); return it == regs.end() ? 0 : it->second; }

  uint32_t read32(uint64_t offset) override {
    uint32_t v = ack(static_cast<uint32_t>(offset));
    if (offset == kBase + ExeclistPort::kRingMiMode && (v & (1u << 8)) && !never_idle) v |= 1u << 9;  // STOP_RING set: the engine reports idle
    return v;
  }

  void write32(uint64_t offset, uint32_t value) override {
    if (offset == 0xA188 || offset == 0xA270) {
      const bool gt = offset == 0xA188;
      const uint32_t ack_reg = gt ? 0x130044 : 0x0D88;
      const bool enable = (value & 1) && (value >> 16);
      if (enable && !(gt ? deny_gt : deny_media)) regs[ack_reg] = 1;
      else if (!enable) regs[ack_reg] = 0;
      regs[offset] = value;
      return;
    }
    if (offset == kBase + ExeclistPort::kRingMiMode) {  // masked register
      const uint32_t mask = value >> 16;
      regs[offset] = (ack(static_cast<uint32_t>(offset)) & ~mask) | (value & mask);
      return;
    }
    regs[offset] = value;
    if (offset == kBase + ExeclistPort::kRingContextStatusPtr) csb_wp = value & 0xFF;
    if (offset == kBase + ExeclistPort::kRingElsp) {
      elsp[elsp_count++] = value;
      if (elsp_count == 4) {
        elsp_count = 0;
        if (elsp[0] != 0 || elsp[1] != 0) violation("element 1 was not empty");
        if (!hang) run((uint64_t{elsp[2]} << 32) | elsp[3]);
      }
    }
  }

  // --- The global GTT as the hardware sees it.
  uint8_t* ggtt_bytes(uint64_t address, uint64_t length, const char* what) {
    const uint64_t index = address >> 12;
    if (index >= ggtt->size() || !((*ggtt)[index] & 1)) { violation(std::string("GGTT miss: ") + what); return nullptr; }
    if ((address & 0xFFF) + length > 4096) { violation(std::string("GGTT access crosses a page: ") + what); return nullptr; }
    bool dead = false;
    uint8_t* p = mem->find(((*ggtt)[index] & ~0xFFFull & 0x7FFFFFFFFFull) + (address & 0xFFF), length, &dead);
    if (!p) { violation(std::string("GGTT entry points nowhere: ") + what); return nullptr; }
    if (dead) { violation(std::string("access to freed memory via GGTT: ") + what); return nullptr; }
    return p;
  }
  uint32_t* ggtt_dword(uint64_t address, const char* what) { return reinterpret_cast<uint32_t*>(ggtt_bytes(address, 4, what)); }

  // --- The context's page tables as the hardware walks them, from the directories in the context image.
  uint64_t pdp[4] = {};
  uint8_t* ppgtt_bytes(uint64_t address, uint64_t length, const char* what, bool* ok) {
    *ok = false;
    bool dead = false;
    uint64_t* pd = reinterpret_cast<uint64_t*>(mem->find(pdp[address >> 30], 4096, &dead));
    if (!pd || dead) { violation(std::string("page directory missing: ") + what); return nullptr; }
    const uint64_t pde = pd[(address >> 21) & 511];
    if (!(pde & 1)) { violation(std::string("PDE not present: ") + what); return nullptr; }
    uint64_t* pt = reinterpret_cast<uint64_t*>(mem->find(pde & ~0xFFFull & 0x7FFFFFFFFFull, 4096, &dead));
    if (!pt || dead) { violation(std::string("page table missing or freed: ") + what); return nullptr; }
    const uint64_t pte = pt[(address >> 12) & 511];
    if (!(pte & 1)) { violation(std::string("PTE not present: ") + what); return nullptr; }
    if ((address & 0xFFF) + length > 4096) { violation(std::string("PPGTT access crosses a page: ") + what); return nullptr; }
    uint8_t* p = mem->find((pte & ~0xFFFull & 0x7FFFFFFFFFull) + (address & 0xFFF), length, &dead);
    if (!p || dead) { violation(std::string("PTE points at freed memory: ") + what); return nullptr; }
    *ok = true;
    return p;
  }

  void run_batch_at(uint64_t address) {
    std::vector<uint32_t> dump;
    for (uint32_t n = 0; n < 8192; ++n) {
      bool ok;
      uint8_t* p = ppgtt_bytes(address + n * 4, 4, "batch", &ok);
      if (!ok) return;
      uint32_t d;
      memcpy(&d, p, 4);
      dump.push_back(d);
      if ((d >> 23) == 0x0A) { batches.push_back(dump); return; }
    }
    violation("batch has no end");
  }

  void run(uint64_t desc) {
    const uint64_t ctx = desc & 0xFFFFF000u;
    const uint32_t id = static_cast<uint32_t>(desc >> 32);
    if (!(desc & 1) || !(desc & (1u << 8)) || ((desc >> 3) & 3) != 1) violation("descriptor flags");
    uint32_t* st = ggtt_dword(ctx + 4096, "context state");
    if (!st) return;
    uint32_t* regs_state = st;
    // The restore: a load header, then offset/value pairs. Check the pairs the driver set are where the engine looks for them.
    if (regs_state[kCtxRingStart - 1] != kBase + 0x38 || regs_state[kCtxRingTail - 1] != kBase + 0x30) violation("context image layout");
    const uint32_t start = regs_state[kCtxRingStart], head = regs_state[kCtxRingHead], tail = regs_state[kCtxRingTail], ctl = regs_state[kCtxRingCtl];
    const uint64_t size = (ctl & ~0xFFFu) + 4096;
    if (!(ctl & 1)) violation("ring not valid");
    const uint32_t udw[4] = {kCtxPdp0Udw, kCtxPdp1Udw, kCtxPdp2Udw, kCtxPdp3Udw}, ldw[4] = {kCtxPdp0Ldw, kCtxPdp1Ldw, kCtxPdp2Ldw, kCtxPdp3Ldw};
    for (int i = 0; i < 4; ++i) pdp[i] = (uint64_t{regs_state[udw[i]]} << 32) | regs_state[ldw[i]];
    uint32_t pos = head;
    uint64_t guard = 0;
    while (pos != tail && guard++ < size) {
      auto at = [&](uint32_t p) -> uint32_t* { return ggtt_dword(start + (p % size), "ring"); };
      uint32_t* hp = at(pos);
      if (!hp) return;
      const uint32_t h = *hp;
      uint32_t len = 1;
      if (h == 0 || (h >> 23) == 0x08 || (h >> 23) == 0x05 || (h >> 23) == 0x02) {
        len = 1;
      } else if ((h >> 23) == 0x31) {
        len = 3;
        uint32_t* lo = at(pos + 4);
        uint32_t* hi = at(pos + 8);
        if (!lo || !hi) return;
        if (!(h & (1u << 8))) violation("batch start is not in the context's address space");
        run_batch_at((uint64_t{*hi} << 32) | *lo);
      } else if ((h >> 23) == 0x26) {
        len = 4;
        uint32_t* a = at(pos + 4);
        uint32_t* v = at(pos + 12);
        if (!a || !v) return;
        if (h & (1u << 18)) ++tlb_invalidates;
        if (h & (1u << 14)) {
          uint32_t* target = nullptr;
          if (h & (1u << 21)) target = ggtt_dword(ctx + (*a & ~7u), "store index");
          else if (*a & 4) target = ggtt_dword(*a & ~7u, "marker");
          else violation("marker write outside the global GTT");
          if (target) *target = *v;
        }
      } else {
        violation("unknown ring command");
        return;
      }
      pos = static_cast<uint32_t>((pos + len * 4) % size);
    }
    ++jobs;
    regs_state[kCtxRingHead] = tail;  // the engine saves its state when the context stops
    // Report to the status page the engine was given.
    uint32_t* wp = ggtt_dword(uint64_t{regs[kBase + ExeclistPort::kRingHwsPga]} + ExeclistPort::kCsbWriteDword * 4, "csb pointer");
    if (!wp) return;
    for (uint32_t status : {ExeclistPort::kStatusIdleToActive, ExeclistPort::kStatusComplete | ExeclistPort::kStatusActiveToIdle}) {
      csb_wp = (csb_wp + 1) % ExeclistPort::kCsbEntries;
      uint32_t* e = ggtt_dword(uint64_t{regs[kBase + ExeclistPort::kRingHwsPga]} + (ExeclistPort::kCsbFirstDword + csb_wp * 2) * 4, "csb entry");
      if (!e) return;
      e[0] = status;
      e[1] = id;
      *wp = 0x100 | csb_wp;
    }
  }
};

static std::vector<uint32_t> simple_batch() {
  std::vector<uint32_t> buf(64);
  intel_mfx::DwordWriter w(ArrayView<uint32_t>(buf.data(), buf.size()));
  intel_mfx::emit_mi_flush(w);
  intel_mfx::emit_pipe_mode_select(w, true, false);
  intel_mfx::emit_batch_end(w);
  buf.resize(w.count());
  return buf;
}

struct Rig {
  FakeMemory mem;
  std::vector<uint64_t> ggtt = std::vector<uint64_t>(1024, 0xAAAA'AAAA'AAAA'AAAAull);
  FakeGpu gpu{&ggtt, &mem};
  VcsDriver::Config config() {
    VcsDriver::Config c;
    c.registers = &gpu;
    c.ggtt_table = MmioRegion(reinterpret_cast<uint64_t>(ggtt.data()), ggtt.size() * 8);
    c.ggtt_first_page = 100;
    c.poll_limit = 1000;
    return c;
  }
  bool ggtt_untouched() const {
    for (uint64_t e : ggtt) if (e != 0xAAAA'AAAA'AAAA'AAAAull) return false;
    return true;
  }
};

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
