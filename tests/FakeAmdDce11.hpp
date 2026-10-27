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



// A simulated DCE 11.2 display block for the host tests: six CRTCs with their register blocks (offsets typed in from vid.h, register numbers from AMD's dce_11_2_d.h), a
// graphics surface address that is latched after a number of reads of GRPH_UPDATE (or never), and the system aperture.
#pragma once
#include <stdint.h>
#include <map>
#include <vector>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace fakedce {

using hardware::IClock;
using hardware::IVcnRegisters;

constexpr uint32_t kCrtcControl = 0x1b9c, kGrphEnable = 0x1a00, kGrphControl = 0x1a01, kGrphSwap = 0x1a03, kGrphPrimary = 0x1a04, kGrphPitch = 0x1a06, kGrphPrimaryHigh = 0x1a07;
constexpr uint32_t kGrphUpdate = 0x1a11, kGrphFlipControl = 0x1a12, kApertureLow = 0x80d, kApertureHigh = 0x80e;
constexpr uint32_t kOffsets[6] = {0x0, 0x200, 0x400, 0x2600, 0x2800, 0x2a00};
constexpr uint64_t kSurface = 0x1'2000'0000ull;

struct FakeClock final : IClock {
  mutable uint64_t now = 0;
  uint64_t now_us() const override { now += 50; return now; }
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

class FakeDisplay final : public IVcnRegisters {
 public:
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  uint32_t latency_reads = 2;       // reads of GRPH_UPDATE that still show the update pending
  bool never_take = false;          // the engine never latches the new address
  bool all_ones = false;            // the registers are not there
  bool drop_address = false;        // the pending flag clears but the engine keeps showing the old address

  void set(uint32_t r, uint32_t v) { regs_[r] = v; }
  uint32_t get(uint32_t r) const { auto at = regs_.find(r); return at == regs_.end() ? 0 : at->second; }
  // A CRTC in the state the firmware leaves it in: on, 32-bit ARGB8888 linear, showing `surface`.
  void crtc(uint32_t index, uint64_t surface, uint32_t pitch) {
    const uint32_t off = kOffsets[index];
    set(kCrtcControl + off, 0x1);
    set(kGrphEnable + off, 0x1);
    set(kGrphControl + off, 0x2);          // GRPH_DEPTH 2, GRPH_FORMAT 0, GRPH_ARRAY_MODE 0
    set(kGrphSwap + off, 0);
    set(kGrphPitch + off, pitch);
    set(kGrphPrimaryHigh + off, static_cast<uint32_t>(surface >> 32));
    set(kGrphPrimary + off, static_cast<uint32_t>(surface));
    set(kGrphFlipControl + off, 0x0);
  }
  uint32_t read(uint32_t r) override {
    if (all_ones) return 0xFFFFFFFFu;
    for (uint32_t off : kOffsets)
      if (r == kGrphUpdate + off) {
        if (pending_reads_ > 0) {
          --pending_reads_;
          return 0x4;   // GRPH_SURFACE_UPDATE_PENDING
        }
        if (!never_take && have_pending_) {
          if (!drop_address) {
            set(kGrphPrimary + off_, pending_low_);
            set(kGrphPrimaryHigh + off_, pending_high_);
          }
          have_pending_ = false;
        }
        return never_take && have_pending_ ? 0x4 : 0;
      }
    return get(r);
  }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    for (uint32_t off : kOffsets) {
      if (r == kGrphPrimaryHigh + off) { pending_high_ = v; return; }
      if (r == kGrphPrimary + off) {
        // the write of the low half triggers the update (taken at once with the immediate flip, never with a vsync flip)
        pending_low_ = v;
        off_ = off;
        const bool immediate = get(kGrphFlipControl + off) & 0x10;
        if (immediate) { have_pending_ = true; pending_reads_ = latency_reads; }
        else { have_pending_ = true; never_take = true; }
        return;
      }
    }
    set(r, v);
  }

 private:
  std::map<uint32_t, uint32_t> regs_;
  bool have_pending_ = false;
  uint32_t pending_reads_ = 0, pending_low_ = 0, pending_high_ = 0, off_ = 0;
};

inline void setup_aperture(FakeDisplay& d) {
  d.set(kApertureLow, 0x100000);        // 0x1_0000_0000
  d.set(kApertureHigh, 0x1FFFFF);       // up to 0x1_FFFF_FFFF
}


}  // namespace fakedce
