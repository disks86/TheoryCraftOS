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

// A simulated Intel GPU for the host tests of the video engine driver and the decode session: memory at made-up device addresses, and an engine that does what the
// hardware does with what the driver gives it (see IntelVcsDriverTest.cpp). The including test defines CHECK and g_failed.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.hpp"

using namespace hardware;
using namespace hardware::intel_gt;

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
  Generation gen = Generation::Gen9;
  uint32_t base = kBase;              // the engine's register base
  uint32_t elsq[4] = {};              // Gen12: the submit queue contents (port 0 low, high, port 1 low, high)
  int tlb_invalidates = 0;
  int jobs = 0;

  explicit FakeGpu(std::vector<uint64_t>* table, FakeMemory* memory) : ggtt(table), mem(memory) {}

  void violation(const std::string& what) { violations.push_back(what); }

  uint32_t ack(uint32_t reg) const { auto it = regs.find(reg); return it == regs.end() ? 0 : it->second; }

  uint32_t read32(uint64_t offset) override {
    uint32_t v = ack(static_cast<uint32_t>(offset));
    if (offset == base + ExeclistPort::kRingMiMode && (v & (1u << 8)) && !never_idle) v |= 1u << 9;  // STOP_RING set: the engine reports idle
    return v;
  }

  void write32(uint64_t offset, uint32_t value) override {
    if (offset == 0xA188 || offset == 0xA270 || offset == 0xA540) {
      const bool gt = offset == 0xA188;
      if ((offset == 0xA270 && gen == Generation::Gen12) || (offset == 0xA540 && gen == Generation::Gen9)) violation("forcewake domain of another generation");
      const uint32_t ack_reg = gt ? 0x130044 : offset == 0xA540 ? 0x0D50 : 0x0D88;
      const bool enable = (value & 1) && (value >> 16);
      if (enable && !(gt ? deny_gt : deny_media)) regs[ack_reg] = 1;
      else if (!enable) regs[ack_reg] = 0;
      regs[offset] = value;
      return;
    }
    if (offset == base + ExeclistPort::kRingMiMode) {  // masked register
      const uint32_t mask = value >> 16;
      regs[offset] = (ack(static_cast<uint32_t>(offset)) & ~mask) | (value & mask);
      return;
    }
    regs[offset] = value;
    if (offset == base + ExeclistPort::kRingContextStatusPtr) csb_wp = value & 0xFF;
    if (gen == Generation::Gen12 && offset >= base + ExeclistPort::kRingExeclistSqContents && offset < base + ExeclistPort::kRingExeclistSqContents + 16) elsq[(offset - base - ExeclistPort::kRingExeclistSqContents) / 4] = value;
    if (gen == Generation::Gen12 && offset == base + ExeclistPort::kRingExeclistControl && (value & 1)) {
      if (elsq[2] != 0 || elsq[3] != 0) violation("port 1 was not empty");
      if (!hang) run((uint64_t{elsq[1]} << 32) | elsq[0]);
    }
    if (gen == Generation::Gen9 && offset == base + ExeclistPort::kRingElsp) {
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
    uint64_t* pd = nullptr;
    if (gen == Generation::Gen12) {
      // Four levels: the page-map level 4 page (pdp[0]) points at a page of directory pointers, which point at the page directories.
      if (address >> 48) { violation(std::string("address beyond 48 bits: ") + what); return nullptr; }
      uint64_t* pml4 = reinterpret_cast<uint64_t*>(mem->find(pdp[0], 4096, &dead));
      if (!pml4 || dead) { violation(std::string("PML4 missing: ") + what); return nullptr; }
      const uint64_t pml4e = pml4[(address >> 39) & 511];
      if (!(pml4e & 1)) { violation(std::string("PML4E not present: ") + what); return nullptr; }
      uint64_t* pdpt = reinterpret_cast<uint64_t*>(mem->find(pml4e & ~0xFFFull & 0x7FFFFFFFFFull, 4096, &dead));
      if (!pdpt || dead) { violation(std::string("page directory pointer table missing: ") + what); return nullptr; }
      const uint64_t pdpe = pdpt[(address >> 30) & 511];
      if (!(pdpe & 1)) { violation(std::string("PDPE not present: ") + what); return nullptr; }
      pd = reinterpret_cast<uint64_t*>(mem->find(pdpe & ~0xFFFull & 0x7FFFFFFFFFull, 4096, &dead));
    } else {
      pd = reinterpret_cast<uint64_t*>(mem->find(pdp[address >> 30], 4096, &dead));
    }
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

  // Called with every batch the engine runs (the dwords, up to and including MI_BATCH_BUFFER_END); a test plays the decode engine here.
  std::function<void(FakeGpu&, const std::vector<uint32_t>&)> on_batch;

  void run_batch_at(uint64_t address) {
    // Walks the commands by their lengths (a data dword may look like the end marker): MFX commands carry their length, a no-op is one dword, the end is the last.
    std::vector<uint32_t> dump;
    uint32_t remaining = 0;
    for (uint32_t n = 0; n < 8192; ++n) {
      bool ok;
      uint8_t* p = ppgtt_bytes(address + n * 4, 4, "batch", &ok);
      if (!ok) return;
      uint32_t d;
      memcpy(&d, p, 4);
      dump.push_back(d);
      if (remaining) { --remaining; continue; }
      if ((d >> 29) == 3) { remaining = (d & 0xFF) + 1; continue; }
      if ((d >> 23) == 0x26) { remaining = 3; continue; }
      if ((d >> 23) == 0x0A) {
        batches.push_back(dump);
        if (on_batch) on_batch(*this, dump);
        return;
      }
    }
    violation("batch has no end");
  }

  void run(uint64_t desc) {
    const uint64_t ctx = desc & 0xFFFFF000u;
    const uint32_t id = static_cast<uint32_t>(desc >> 32);
    if (gen == Generation::Gen12) {
      if (!(desc & 1) || !(desc & (1u << 8)) || ((desc >> 3) & 3) != 3 || (desc >> 61) != 1 || ((desc >> 48) & 0x3F) != 0 || ((desc >> 37) & 0x7FF) == 0x7FF) violation("descriptor flags");
    } else if (!(desc & 1) || !(desc & (1u << 8)) || ((desc >> 3) & 3) != 1) {
      violation("descriptor flags");
    }
    uint32_t* st = ggtt_dword(ctx + 4096, "context state");
    if (!st) return;
    uint32_t* regs_state = st;
    // The restore: a load header, then offset/value pairs. Check the pairs the driver set are where the engine looks for them.
    if (regs_state[kCtxRingStart - 1] != base + 0x38 || regs_state[kCtxRingTail - 1] != base + 0x30) violation("context image layout");
    if (gen == Generation::Gen12) {
      if ((regs_state[1] & (1u << 19)) == 0 || regs_state[1] >> 23 != 0x22 || regs_state[33] >> 23 != 0x22 || regs_state[kCtxPdp0Udw - 1] != base + 0x274) violation("Gen12 context image layout");
      if (regs_state[0x61] != (1u << 24)) violation("ring MI mode in the context image");
    }
    const uint32_t start = regs_state[kCtxRingStart], head = regs_state[kCtxRingHead], tail = regs_state[kCtxRingTail], ctl = regs_state[kCtxRingCtl];
    const uint64_t size = (ctl & ~0xFFFu) + 4096;
    if (!(ctl & 1)) violation("ring not valid");
    const uint32_t udw[4] = {kCtxPdp0Udw, kCtxPdp1Udw, kCtxPdp2Udw, kCtxPdp3Udw}, ldw[4] = {kCtxPdp0Ldw, kCtxPdp1Ldw, kCtxPdp2Ldw, kCtxPdp3Ldw};
    for (int i = 0; i < 4; ++i) pdp[i] = (uint64_t{regs_state[udw[i]]} << 32) | regs_state[ldw[i]];
    if (gen == Generation::Gen12 && (pdp[0] & 0xFFF)) violation("PML4 address");
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
    const uint64_t status_page = regs[base + ExeclistPort::kRingHwsPga];
    if (gen == Generation::Gen12) {
      uint32_t* wp12 = ggtt_dword(status_page + ExeclistPort::kCsb12WriteDword * 4, "csb pointer");
      if (!wp12) return;
      const uint32_t ctx_id = static_cast<uint32_t>((desc >> 37) & 0x7FF), idle = 0x7FF;
      // The context starts (the one switched away from is the idle one), then stops (the one switched to is the idle one).
      const uint32_t entries[2][2] = {{ctx_id << 15 | 1u, idle << 15}, {idle << 15, ctx_id << 15}};
      for (const auto& entry : entries) {
        csb_wp = (csb_wp + 1) % ExeclistPort::kCsb12Entries;
        uint32_t* e = ggtt_dword(status_page + (ExeclistPort::kCsbFirstDword + csb_wp * 2) * 4, "csb entry");
        if (!e) return;
        e[0] = entry[0];
        e[1] = entry[1];
        *wp12 = csb_wp;
      }
      return;
    }
    uint32_t* wp = ggtt_dword(status_page + ExeclistPort::kCsbWriteDword * 4, "csb pointer");
    if (!wp) return;
    for (uint32_t status : {ExeclistPort::kStatusIdleToActive, ExeclistPort::kStatusComplete | ExeclistPort::kStatusActiveToIdle}) {
      csb_wp = (csb_wp + 1) % ExeclistPort::kCsbEntries;
      uint32_t* e = ggtt_dword(status_page + (ExeclistPort::kCsbFirstDword + csb_wp * 2) * 4, "csb entry");
      if (!e) return;
      e[0] = status;
      e[1] = id;
      *wp = 0x100 | csb_wp;
    }
  }
};

struct Rig {
  FakeMemory mem;
  std::vector<uint64_t> ggtt = std::vector<uint64_t>(1024, 0xAAAA'AAAA'AAAA'AAAAull);
  FakeGpu gpu{&ggtt, &mem};
  Generation generation;
  explicit Rig(Generation g = Generation::Gen9) : generation(g) {
    gpu.gen = g;
    gpu.base = g == Generation::Gen12 ? kVd0MmioBase : kBase;
  }
  VcsDriver::Config config() {
    VcsDriver::Config c;
    c.generation = generation;
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

