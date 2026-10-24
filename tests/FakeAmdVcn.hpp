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


// A simulated Video Core Next 1.0 decode ring for the host tests: the registers of the card, and the ring buffer controller that reads register-write packets from
// the ring and from the indirect buffer it points at, the way the hardware does. It is written from the Linux driver's description of the packets, independently of
// the code under test, and the engine firmware behind it is whatever the test plugs in (on_command / on_engine).
#pragma once
#include <stdint.h>
#include <functional>
#include <map>
#include <vector>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace fakevcn {

// UVD register dwords on Raven (segment 1 base 0x7E00 + the register's number in vcn_1_0_offset.h).
constexpr uint32_t kRegGpcomCmd = 0x7E00 + 0x03C3, kRegGpcomData0 = 0x7E00 + 0x03C4, kRegGpcomData1 = 0x7E00 + 0x03C5, kRegEngineCntl = 0x7E00 + 0x03C6;
constexpr uint32_t kRegNoOp = 0x7E00 + 0x03FF, kRegContextId = 0x7E00 + 0x05BD;
constexpr uint32_t kRegIbVmid = 0x7E00 + 0x05A1, kRegIbSize = 0x7E00 + 0x05A2, kRegIbBarLow = 0x7E00 + 0x0467, kRegIbBarHigh = 0x7E00 + 0x0466;
constexpr uint32_t kRegRptr = 0x7E00 + 0x05A4, kRegWptr = 0x7E00 + 0x05A5;

struct FakeClock final : hardware::IClock {
  mutable uint64_t now = 0;
  uint64_t now_us() const override { now += 50; return now; }
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

// Memory the engine can reach: a GPU address range backed by host memory.
struct Window {
  uint64_t gpu = 0;
  uint8_t* host = nullptr;
  uint64_t size = 0;
};

struct Command {
  uint32_t command;
  uint64_t address;
};

class Vcn final : public hardware::IVcnRegisters {
 public:
  std::vector<Window> windows;
  uint64_t ring_gpu = 0;           // where the ring is (the test sets it; the real engine reads it from the base registers)
  uint32_t ring_dwords = 1024;
  std::vector<Command> commands;   // every GPCOM command, in order (the fence and the packet start/end included)
  std::vector<std::pair<uint32_t, uint32_t>> writes;   // every register write
  bool stall_fence = false;        // the engine never finishes
  uint32_t jobs_started = 0;       // IB executions
  uint32_t protocol_errors = 0;    // packets the hardware would not take
  std::function<void(uint32_t command, uint64_t address)> on_command;   // the firmware's view of each command except fence/trap/packet start+end
  std::function<void()> on_engine;                                       // UVD_ENGINE_CNTL written with 1

  uint32_t read(uint32_t dword) override { return dword == kRegRptr ? rptr_ : regs_[dword]; }
  void write(uint32_t dword, uint32_t value) override {
    writes.push_back({dword, value});
    if (dword == kRegWptr) {
      wptr_ = value;
      run_ring();
      return;
    }
    regs_[dword] = value;
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
  // Starts the ring at a position (what AmdVcn1::start leaves).
  void set_pointers(uint32_t position) { rptr_ = wptr_ = position; }

 private:
  void run_ring() {
    while (rptr_ != wptr_) {
      const uint32_t packet = get32(ring_gpu + uint64_t{rptr_} * 4);
      rptr_ = (rptr_ + 1) % ring_dwords;
      if (!execute(packet, /*from_ring=*/true, 0)) { rptr_ = wptr_; return; }
    }
  }
  // A packet whose values follow in the ring (from_ring) or the indirect buffer.
  bool execute(uint32_t packet, bool from_ring, uint64_t ib_next) {
    if ((packet >> 30) != 0) { ++protocol_errors; return false; }
    const uint32_t reg = packet & 0xFFFF, count = (packet >> 16) & 0x3FFF;
    for (uint32_t i = 0; i <= count; ++i) {
      uint32_t value;
      if (from_ring) {
        value = get32(ring_gpu + uint64_t{rptr_} * 4);
        rptr_ = (rptr_ + 1) % ring_dwords;
      } else {
        value = get32(ib_next + uint64_t{i} * 4);
      }
      register_write(reg + i, value, !from_ring);
    }
    return true;
  }
  // `in_ib`: made from the indirect buffer. The command numbers overlap (the message buffer of the IB and the fence of the ring are both 0), so the source says which
  // a command is: the ring carries the framing (start, fence, trap, end), the indirect buffer the buffers.
  void register_write(uint32_t reg, uint32_t value, bool in_ib) {
    writes.push_back({reg, value});
    regs_[reg] = value;
    if (reg == kRegGpcomCmd) {
      const uint32_t command = value >> 1;
      const uint64_t address = regs_[kRegGpcomData0] | (uint64_t{regs_[kRegGpcomData1]} << 32);
      commands.push_back({command, address});
      if (in_ib) {
        if (on_command) on_command(command, address);
      } else if (command == 0 /*fence*/) {
        if (!stall_fence) put32(address, regs_[kRegContextId]);
      }
    } else if (reg == kRegEngineCntl && value == 1 && on_engine) {
      on_engine();
    } else if (reg == kRegIbSize) {
      run_ib(value);
    }
  }
  void run_ib(uint32_t dwords) {
    ++jobs_started;
    const uint64_t base = regs_[kRegIbBarLow] | (uint64_t{regs_[kRegIbBarHigh]} << 32);
    if (regs_[kRegIbVmid] != 0) ++protocol_errors;
    for (uint32_t at = 0; at < dwords;) {
      const uint32_t packet = get32(base + uint64_t{at} * 4);
      const uint32_t count = (packet >> 16) & 0x3FFF;
      if (at + 1 + count + 1 > dwords + 0 && at + 2 > dwords) { ++protocol_errors; return; }
      if (!execute(packet, false, base + uint64_t{at + 1} * 4)) return;
      at += 2 + count;
    }
  }

  std::map<uint32_t, uint32_t> regs_;
  uint32_t rptr_ = 0, wptr_ = 0;
};

}  // namespace fakevcn
