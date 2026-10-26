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


// A simulated memory controller for the host tests of the Vega M GART set-up: the registers of the card (a plain register file with the SRBM busy bits, the host data
// path flush counter, the TLB invalidation request) and a model of VM context 0 that does what the hardware does with it: a translation of a GPU address in the GART's range
// reads the page table entry from video memory, caches the result until the TLB is invalidated, and sends an invalid entry (or an address outside the range) to the
// dummy page. Written from the Linux driver's description, independently of the code under test; the register numbers are typed in from AMD's gmc_8_1_d.h, bif_5_0_d.h
// and oss_3_0_d.h.
#pragma once
#include <stdint.h>
#include <map>
#include <vector>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace fakegmc {

constexpr uint32_t kSrbmStatus = 0x394, kHdpFlush = 0x1520, kInvalidate = 0x51e;
constexpr uint32_t kContext0Cntl = 0x504, kContext1Cntl = 0x505, kCtx0Start = 0x557, kCtx0End = 0x55F, kCtx0Base = 0x54F, kCtx0Fault = 0x546;
constexpr uint32_t kL1TlbCntl = 0x819, kL2Cntl = 0x500, kL2Cntl2 = 0x501, kL2Cntl3 = 0x502, kL2Cntl4 = 0x578;
constexpr uint32_t kApertureLow = 0x80d, kApertureHigh = 0x80e, kApertureDefault = 0x80f, kAgpBase = 0x80c, kAgpTop = 0x80a, kAgpBot = 0x80b;
constexpr uint32_t kBifFbEn = 0x1524, kHdpCoherency = 0x1528, kHdpMisc = 0xbd3, kHdpHostPath = 0xb00;
constexpr uint32_t kBusyBits = 0x100 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x2000;   // VMC, MCB, MCB_NON_DISPLAY, MCC, MCD, VMC1

struct Window {
  uint64_t gpu = 0;
  uint8_t* host = nullptr;
  uint64_t size = 0;
};

struct FakeClock final : hardware::IClock {
  mutable uint64_t now = 0;
  uint64_t now_us() const override { now += 50; return now; }
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

class Card final : public hardware::IVcnRegisters {
 public:
  std::vector<Window> windows;                           // the card's memory (and the system memory it can reach), by GPU address
  std::vector<std::pair<uint32_t, uint32_t>> writes;     // every register write, in order
  uint32_t busy_reads = 0;                               // reads of SRBM_STATUS that still show the memory controller busy
  bool busy_after_agp = false;                           // the controller turns busy once the AGP window is written and stays so
  uint32_t hdp_flushes = 0, invalidations = 0;
  std::map<uint64_t, uint64_t> tlb;                      // GART page -> the entry the hardware cached

  uint32_t reg(uint32_t r) const { auto at = regs_.find(r); return at == regs_.end() ? 0 : at->second; }
  void set_reg(uint32_t r, uint32_t v) { regs_[r] = v; }

  uint32_t read(uint32_t r) override {
    if (r == kSrbmStatus) {
      if (busy_reads > 0) { --busy_reads; return kBusyBits | 0x80000000u; }
      return 0x80000000u;   // (a bit that is not one of the memory controller's)
    }
    return reg(r);
  }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    if (r == kHdpFlush) ++hdp_flushes;
    if (r == kAgpBot && busy_after_agp) busy_reads = 1u << 30;
    if (r == kInvalidate) {
      ++invalidations;
      if (v & 1) tlb.clear();
    }
    regs_[r] = v;
  }

  uint8_t* host(uint64_t gpu, uint64_t bytes) const {
    for (const Window& w : windows)
      if (gpu >= w.gpu && gpu - w.gpu <= w.size && bytes <= w.size - (gpu - w.gpu)) return w.host + (gpu - w.gpu);
    return nullptr;
  }
  uint64_t get64(uint64_t gpu) const {
    const uint8_t* p = host(gpu, 8);
    uint64_t v = 0;
    if (p) for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
  }

  // What the memory controller does with an address of VM context 0. `ok` is false when it would take the fault path (the dummy page); `system` is the entry's system bit.
  struct Translation { bool ok = false; uint64_t address = 0; bool system = false; bool from_tlb = false; };
  Translation translate(uint64_t gpu) {
    Translation t;
    if (!(reg(kContext0Cntl) & 1)) return t;
    const uint64_t start = uint64_t{reg(kCtx0Start)} << 12, end = (uint64_t{reg(kCtx0End)} + 1) << 12;
    if (gpu < start || gpu >= end) return t;
    const uint64_t page = (gpu - start) >> 12;
    uint64_t pte;
    auto cached = tlb.find(page);
    if (cached != tlb.end()) { pte = cached->second; t.from_tlb = true; }
    else { pte = get64((uint64_t{reg(kCtx0Base)} << 12) + page * 8); tlb[page] = pte; }
    if (!(pte & 1)) return t;
    t.ok = true;
    t.address = (pte & 0x000000FFFFFFF000ull) | (gpu & 0xFFF);
    t.system = (pte & 2) != 0;
    return t;
  }

 private:
  std::map<uint32_t, uint32_t> regs_;
};

}  // namespace fakegmc
