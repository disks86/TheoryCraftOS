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


// A simulated interrupt handler (IH 3.0) for the host tests: the registers of the ring (control, base, pointers, write-back address, doorbell), and the hardware side that
// puts 16-byte vectors into the ring when a test posts one, advances the write pointer (register and write-back word), and flags an overflow when the write pointer catches up
// with the read pointer, which the driver clears through the control register. The read pointer is the register's, or the doorbell's when the doorbell is enabled. Written
// from the Linux driver's description; the register numbers and masks are typed in from AMD's oss_3_0_d.h, oss_3_0_sh_mask.h and bif_5_0_d.h.
#pragma once
#include <stdint.h>
#include <map>
#include <vector>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace fakeih {

constexpr uint32_t kRbCntl = 0xe30, kRbBase = 0xe31, kRbRptr = 0xe32, kRbWptr = 0xe33, kWptrAddrHi = 0xe34, kWptrAddrLo = 0xe35, kDoorbellRptr = 0xe42;
constexpr uint32_t kInterruptCntl = 0x151a, kInterruptCntl2 = 0x151b;

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
  std::vector<Window> windows;
  std::vector<std::pair<uint32_t, uint32_t>> writes;   // every register write, in order
  std::vector<uint64_t> write_times;
  const hardware::IClock* clock = nullptr;
  uint8_t* doorbell_host = nullptr;
  uint64_t doorbell_bytes = 0;
  uint32_t posted = 0, dropped = 0;
  bool wptr_register_stuck = false;        // IH_RB_WPTR always reads 0: only the write-back word shows the write pointer
  bool stale_writeback_overflow = false;   // the write-back word keeps an overflow flag the register no longer has

  uint32_t reg(uint32_t r) const { auto at = regs_.find(r); return at == regs_.end() ? 0 : at->second; }
  void set_reg(uint32_t r, uint32_t v) { regs_[r] = v; }
  uint32_t read(uint32_t r) override {
    if (r == kRbWptr) return wptr_register_stuck ? 0 : wptr_ | (overflow_ ? 1u : 0u);
    return reg(r);
  }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    write_times.push_back(clock ? clock->now_us() : 0);
    regs_[r] = v;
    if (r == kRbCntl && (v & 0x80000000u)) {   // WPTR_OVERFLOW_CLEAR
      overflow_ = false;
      stale_writeback_overflow = false;
    }
    if (r == kRbWptr) wptr_ = v & 0x3fffc;
    if (r == kRbRptr) rptr_register_ = v;
  }

  bool enabled() const { return (reg(kRbCntl) & 0x20001) == 0x20001; }
  uint32_t ring_bytes() const { return 4u << ((reg(kRbCntl) >> 1) & 0x1f); }
  uint32_t read_pointer() const {
    if (reg(kDoorbellRptr) & 0x10000000) {
      const uint32_t index = reg(kDoorbellRptr) & 0x1fffff;
      if (doorbell_host && (uint64_t{index} + 1) * 4 <= doorbell_bytes) {
        const uint8_t* p = doorbell_host + uint64_t{index} * 4;
        return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24);
      }
      return 0;
    }
    return rptr_register_;
  }
  uint8_t* host(uint64_t gpu, uint64_t bytes) const {
    for (const Window& w : windows)
      if (gpu >= w.gpu && gpu - w.gpu <= w.size && bytes <= w.size - (gpu - w.gpu)) return w.host + (gpu - w.gpu);
    return nullptr;
  }
  void put32(uint64_t gpu, uint32_t v) const {
    uint8_t* p = host(gpu, 4);
    if (p) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
  }

  // The hardware side: an interrupt from a block.
  void post(uint32_t src_id, uint32_t src_data, uint32_t ring_id, uint32_t vmid, uint32_t pasid) {
    if (!enabled()) { ++dropped; return; }
    const uint64_t base = uint64_t{reg(kRbBase)} << 8;
    const uint32_t dw[4] = {src_id & 0xff, src_data, (ring_id & 0xff) | (vmid & 0xff) << 8 | (pasid & 0xffff) << 16, 0};
    for (int i = 0; i < 4; ++i) put32(base + wptr_ + 4 * i, dw[i]);
    wptr_ = (wptr_ + 16) & (ring_bytes() - 1);
    if (wptr_ == read_pointer()) overflow_ = true;   // caught up with the driver
    ++posted;
    if (reg(kRbCntl) & 0x100) {   // WPTR_WRITEBACK_ENABLE
      const uint64_t address = uint64_t{reg(kWptrAddrHi) & 0xff} << 32 | reg(kWptrAddrLo);
      put32(address, wptr_ | ((overflow_ || stale_writeback_overflow) ? 1u : 0u));
    }
  }

 private:
  std::map<uint32_t, uint32_t> regs_;
  uint32_t wptr_ = 0, rptr_register_ = 0;
  bool overflow_ = false;
};

}  // namespace fakeih
