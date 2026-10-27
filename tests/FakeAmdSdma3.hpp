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


// Simulated SDMA engines for the host tests: the register files of the two engines (SDMA1's registers are SDMA0's plus 0x200) and the engines themselves, which poll their
// doorbell in the doorbell memory the test supplies (from the fake clock's tick, since the code under test waits on memory words, not registers), read packets from the ring
// between their read pointer and the doorbell's write pointer, and execute them: NOP (with the burst count), WRITE linear, COPY linear, CONSTANT_FILL, FENCE and TRAP. An engine
// runs only when the SMC loaded its microcode (the status bit), it is not halted and its ring is enabled. Written from the Linux driver's description; the register numbers and
// masks are typed in from AMD's oss_3_0_d.h, oss_3_0_sh_mask.h and tonga_sdma_pkt_open.h.
#pragma once
#include <stdint.h>
#include <string.h>
#include <functional>
#include <map>
#include <vector>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace fakesdma {

constexpr uint32_t kSmcMessage = 0x94, kSmcResp = 0x95, kSmcIndex = 0x1AC, kSmcData = 0x1AD, kHdpFlush = 0x1520;   // (only the HDP flush is used here)
constexpr uint32_t kF32Cntl = 0x3412, kRbCntl = 0x3480, kRbBase = 0x3481, kRbBaseHi = 0x3482, kRbRptr = 0x3483, kRbWptr = 0x3484, kPollCntl = 0x3485;
constexpr uint32_t kRptrAddrHi = 0x3488, kRptrAddrLo = 0x3489, kIbCntl = 0x348a, kDoorbell = 0x3492, kSdmaCntl = 0x3404, kTilingConfig = 0x3406;
constexpr uint32_t kSrbmGfxCntl = 0x391;

struct Window {
  uint64_t gpu = 0;
  uint8_t* host = nullptr;
  uint64_t size = 0;
};

struct FakeClock final : hardware::IClock {
  mutable uint64_t now = 0;
  std::function<void()> on_tick;
  uint64_t now_us() const override {
    now += 50;
    if (on_tick) on_tick();
    return now;
  }
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

class Card final : public hardware::IVcnRegisters {
 public:
  std::vector<Window> windows;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  uint8_t* doorbell_host = nullptr;
  uint64_t doorbell_bytes = 0;
  bool loaded[2] = {true, true};       // the SMC loaded the microcode of the engine
  bool stall[2] = {false, false};      // the engine never processes anything
  uint32_t executed[2] = {0, 0}, traps[2] = {0, 0}, protocol_errors = 0, copies = 0, fills = 0, writes_done = 0, fences = 0;
  std::vector<uint32_t> copy_sizes;
  struct IbRun { uint64_t address; uint32_t dwords; };
  std::vector<IbRun> ib_runs;     // every indirect buffer the engines ran
  uint32_t ib_errors = 0;         // indirect packets the engine refused
  bool ignore_fences = false;     // the engine runs everything but never writes a fence

  uint32_t reg(uint32_t r) const { auto at = regs_.find(r); return at == regs_.end() ? 0 : at->second; }
  void set_reg(uint32_t r, uint32_t v) { regs_[r] = v; }
  uint32_t read(uint32_t r) override { return reg(r); }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    regs_[r] = v;
    if (r == kRbRptr) rptr_[0] = v;            // the driver sets the read pointer (to 0) when it programs the ring
    if (r == kRbRptr + 0x200) rptr_[1] = v;
  }

  uint8_t* host(uint64_t gpu, uint64_t bytes) const {
    for (const Window& w : windows)
      if (gpu >= w.gpu && gpu - w.gpu <= w.size && bytes <= w.size - (gpu - w.gpu)) return w.host + (gpu - w.gpu);
    return nullptr;
  }
  uint32_t get32(uint64_t gpu) const {
    const uint8_t* p = host(gpu, 4);
    return p ? p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24) : 0xDEADBEEF;
  }
  void put32(uint64_t gpu, uint32_t v) const {
    uint8_t* p = host(gpu, 4);
    if (p) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
  }

  // The engines look at their doorbells.
  void poll() {
    for (uint32_t i = 0; i < 2; ++i) {
      const uint32_t off = 0x200 * i;
      if (!loaded[i] || stall[i]) continue;
      if (reg(kF32Cntl + off) & 1) continue;                 // halted
      if (!(reg(kRbCntl + off) & 1)) continue;               // ring disabled
      const uint32_t db = reg(kDoorbell + off);
      if (!(db & 0x10000000)) continue;
      const uint32_t index = db & 0x1fffff;
      if (!doorbell_host || (uint64_t{index} + 1) * 4 > doorbell_bytes) continue;
      const uint8_t* p = doorbell_host + uint64_t{index} * 4;
      const uint32_t wptr = (p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24));
      run(i, off, wptr);
    }
  }

 private:
  void run(uint32_t instance, uint32_t off, uint32_t wptr_bytes) {
    const uint64_t base = (uint64_t{reg(kRbBaseHi + off)} << 32 | reg(kRbBase + off)) << 8;
    const uint32_t ring_bytes = 4u << ((reg(kRbCntl + off) >> 1) & 0x1f);
    process(instance, base, &rptr_[instance], wptr_bytes & (ring_bytes - 1), ring_bytes, 0);
    if (reg(kRbCntl + off) & 0x1000) put32(uint64_t{reg(kRptrAddrHi + off)} << 32 | reg(kRptrAddrLo + off), rptr_[instance]);
  }

  // Packets from `*rptr` to `wptr` (bytes) of a buffer at `base`, `size` bytes long and a power of two (the ring wraps; an indirect buffer is processed with a size too big to wrap).
  void process(uint32_t instance, uint64_t base, uint32_t* rptr_ref, uint32_t wptr, uint32_t size, int depth) {
    uint32_t& rptr = *rptr_ref;
    while (rptr != wptr) {
      const uint32_t h = get32(base + rptr);
      const uint32_t op = h & 0xFF;
      auto dword = [&](uint32_t k) { return get32(base + ((rptr + 4 * k) & (size - 1))); };
      uint32_t length = 1;   // dwords of the packet
      switch (op) {
        case 0: length = 1 + (h >> 16 & 0x3FFF); break;   // NOP: a burst count skips that many following dwords
        case 2: length = 4 + dword(3); break;             // WRITE linear: header, address (2), count, data
        case 1: length = 7; break;
        case 4: length = 6; break;                        // INDIRECT: header, address lo, hi, size, two more
        case 5: length = 4; break;
        case 6: length = 2; break;
        case 11: length = 5; break;
        default: ++protocol_errors; rptr = wptr; return;
      }
      if (length * 4 > ((wptr - rptr) & (size - 1)) && !(wptr == rptr)) {
        // (the whole packet must be there: the driver commits whole packets)
        ++protocol_errors;
        rptr = wptr;
        return;
      }
      switch (op) {
        case 2: {
          const uint64_t address = uint64_t{dword(2)} << 32 | dword(1);
          for (uint32_t k = 0; k < dword(3); ++k) put32(address + 4 * k, dword(4 + k));
          ++writes_done;
          break;
        }
        case 1: {
          const uint32_t count = dword(1) & 0x3FFFFF;
          const uint64_t src = uint64_t{dword(4)} << 32 | dword(3), dst = uint64_t{dword(6)} << 32 | dword(5);
          uint8_t* from = host(src, count);
          uint8_t* to = host(dst, count);
          if (!from || !to || dword(2) != 0) { ++protocol_errors; break; }
          memmove(to, from, count);
          ++copies;
          copy_sizes.push_back(count);
          break;
        }
        case 4: {
          // The IB must end on an 8-dword boundary of the ring, start 32-byte aligned, and has no VMID (0).
          const uint64_t address = uint64_t{dword(2)} << 32 | dword(1);
          const uint32_t dwords = dword(3) & 0xFFFFF;
          if (depth > 0 || ((rptr / 4 + 6) & 7) != 0 || (address & 31) != 0 || (h >> 16 & 0xF) != 0 || dwords == 0 || !host(address, uint64_t{dwords} * 4)) { ++ib_errors; break; }
          uint32_t pos = 0;
          ib_runs.push_back({address, dwords});
          process(instance, address, &pos, dwords * 4, 0x80000000u, depth + 1);
          break;
        }
        case 11: {
          const uint64_t dst = uint64_t{dword(2)} << 32 | dword(1);
          const uint32_t value = dword(3), count = dword(4);
          if ((count & 3) || !host(dst, count)) { ++protocol_errors; break; }
          for (uint32_t k = 0; k < count; k += 4) put32(dst + k, value);
          ++fills;
          break;
        }
        case 5: if (!ignore_fences) put32(uint64_t{dword(2)} << 32 | dword(1), dword(3)); ++fences; break;
        case 6: ++traps[instance]; break;
        default: break;
      }
      ++executed[instance];
      rptr = (rptr + 4 * length) & (size - 1);
    }
  }

  std::map<uint32_t, uint32_t> regs_;
  uint32_t rptr_[2] = {0, 0};
};

}  // namespace fakesdma
