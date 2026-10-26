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

// A simulated UVD 6 decode ring for the host tests: the registers of the card the ring uses, and the ring buffer controller that reads register-write packets from the
// ring and from the indirect buffer it points at, the way the hardware does. It is written from the Linux driver's description of the packets, independently of the
// code under test, and the engine firmware behind it is whatever the test plugs in (on_command / on_engine). The register numbers are typed in from AMD's
// uvd_6_0_d.h and bif_5_0_d.h.
#pragma once
#include <stdint.h>
#include <functional>
#include <map>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdUvd6.hpp"

namespace fakeuvd {

constexpr uint32_t kIbVmid = 0x3da1, kIbBarLow = 0x3c67, kIbBarHigh = 0x3c66, kIbSize = 0x3da2, kContextId = 0x3dbd;
constexpr uint32_t kData0 = 0x3bc4, kData1 = 0x3bc5, kCmd = 0x3bc3, kEngineCntl = 0x3bc6, kNoOp = 0x3bff, kWptr = 0x3da5, kRptr = 0x3da4, kHdpFlush = 0x1520;

struct Window {
  uint64_t gpu = 0;
  uint8_t* host = nullptr;
  uint64_t size = 0;
};

struct Command {
  uint32_t command;
  uint64_t address;
  bool in_ib;
};

class Ring final : public hardware::IVcnRegisters {
 public:
  std::vector<Window> windows;
  uint64_t ring_gpu = 0;
  uint32_t ring_dwords = 1024;
  std::vector<Command> commands;                          // every GPCOM command, in order
  std::vector<std::pair<uint32_t, uint32_t>> writes;      // every register write the host made
  bool stall_fence = false, dead = false;
  uint32_t engine_starts = 0, protocol_errors = 0, ib_runs = 0;
  std::function<void(uint32_t command, uint64_t address, bool in_ib)> on_command;   // the firmware's view of each command except the fence and trap of the ring
  std::function<void()> on_engine;                                                  // UVD_ENGINE_CNTL written with 1

  uint32_t read(uint32_t r) override { return r == kRptr ? rptr_ : regs_[r]; }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    if (r == kWptr) {
      wptr_ = v;
      if (!dead) run_ring();
      return;
    }
    regs_[r] = v;
  }
  uint8_t* translate(uint64_t gpu, uint64_t bytes) const {
    for (const Window& w : windows)
      if (gpu >= w.gpu && gpu - w.gpu <= w.size && bytes <= w.size - (gpu - w.gpu)) return w.host + (gpu - w.gpu);
    return nullptr;
  }
  uint32_t get32(uint64_t gpu) const {
    const uint8_t* p = translate(gpu, 4);
    return p ? p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24) : 0xDEADBEEF;
  }
  void put32(uint64_t gpu, uint32_t v) const {
    uint8_t* p = translate(gpu, 4);
    if (!p) return;
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
  }
  void set_pointers(uint32_t position) { rptr_ = wptr_ = position; }

 private:
  void run_ring() {
    while (rptr_ != wptr_) {
      const uint32_t packet = get32(ring_gpu + uint64_t{rptr_} * 4);
      rptr_ = (rptr_ + 1) % ring_dwords;
      if (!execute(packet, true, 0)) { rptr_ = wptr_; return; }
    }
  }
  bool execute(uint32_t packet, bool from_ring, uint64_t ib_at) {
    if ((packet >> 30) != 0) { ++protocol_errors; return false; }
    const uint32_t r = packet & 0xFFFF, count = (packet >> 16) & 0x3FFF;
    for (uint32_t i = 0; i <= count; ++i) {
      uint32_t value;
      if (from_ring) {
        value = get32(ring_gpu + uint64_t{rptr_} * 4);
        rptr_ = (rptr_ + 1) % ring_dwords;
      } else {
        value = get32(ib_at + uint64_t{i} * 4);
      }
      register_write(r + i, value, !from_ring);
    }
    return true;
  }
  void register_write(uint32_t r, uint32_t value, bool in_ib) {
    regs_[r] = value;
    if (r == kCmd) {
      const uint32_t command = value >> 1;
      const uint64_t address = regs_[kData0] | (uint64_t{regs_[kData1]} << 32);
      commands.push_back({command, address, in_ib});
      if (!in_ib && command == 0) {
        if (!stall_fence) put32(address, regs_[kContextId]);
      } else if (on_command) {
        on_command(command, address, in_ib);
      }
    } else if (r == kEngineCntl && value == 1) {
      ++engine_starts;
      if (on_engine) on_engine();
    } else if (r == kIbSize) {
      const uint64_t ib = regs_[kIbBarLow] | (uint64_t{regs_[kIbBarHigh]} << 32);
      ++ib_runs;
      if (regs_[kIbVmid] != 0) ++protocol_errors;
      uint32_t at = 0;
      while (at < value) {
        const uint32_t p = get32(ib + uint64_t{at} * 4);
        at += 1;
        if ((p >> 30) != 0 || ((p >> 16) & 0x3FFF) != 0) { ++protocol_errors; return; }
        register_write(p & 0xFFFF, get32(ib + uint64_t{at} * 4), true);
        at += 1;
      }
    }
  }
  uint32_t rptr_ = 0, wptr_ = 0;
  std::map<uint32_t, uint32_t> regs_;
};

}  // namespace fakeuvd
