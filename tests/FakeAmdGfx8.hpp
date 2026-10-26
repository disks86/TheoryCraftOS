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


// A simulated Radeon RX Vega M for the host tests of the graphics engine start-up: the registers of the card, the SMC behind its mailbox (the messages that load microcode,
// the table of contents it reads from video memory, the status word it sets) and the command processor, which reads packets from the ring once its microcode is loaded and
// its halt bits are clear. It is written from the Linux driver's description, independently of the code under test: every register number and mask is typed in from AMD's
// gfx_8_0_d.h, gfx_8_0_sh_mask.h and smu_7_1_3_d.h, none comes from the code's own generated file.
#pragma once
#include <stdint.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace fakegfx {

constexpr uint32_t kSmcMessage = 0x94, kSmcResp = 0x95, kSmcArg = 0xa4, kSmcIndex = 0x1AC, kSmcData = 0x1AD, kHdpFlush = 0x1520;
constexpr uint32_t kCpMeCntl = 0x21b6, kCpMecCntl = 0x208d, kRb0Cntl = 0x3041, kRb0Base = 0x3040, kRb0BaseHi = 0x30b1, kRb0Wptr = 0x3045, kRb0Rptr = 0x21c0;
constexpr uint32_t kRb0RptrAddr = 0x3043, kRb0RptrAddrHi = 0x3044, kWptrPollLo = 0x3046, kWptrPollHi = 0x3047;
constexpr uint32_t kScratch0 = 0xc040, kRlcCntl = 0xec00, kGrbmSoftReset = 0x2008, kCpIntCntlRing0 = 0x306a, kSerdesCuBusy = 0xec61, kSerdesNoncuBusy = 0xec62;
constexpr uint32_t kGrbmGfxIndex = 0xc200, kDoorbellControl = 0x3059;
constexpr uint32_t kMeHalt = 0x10000000, kPfpHalt = 0x04000000, kCeHalt = 0x01000000;
constexpr uint32_t kMecMe1Halt = 0x40000000, kMecMe2Halt = 0x10000000;

struct Window {
  uint64_t gpu = 0;
  uint8_t* host = nullptr;
  uint64_t size = 0;
};

struct TocEntry {
  uint16_t id, version;
  uint32_t address_high, address_low, meta_high, meta_low, size;
  uint16_t flags, registers;
  std::vector<uint8_t> image;   // the bytes the SMC read at the address
  bool readable = false;
};

struct Executed {
  uint32_t opcode;
  std::vector<uint32_t> body;
};

class Card final : public hardware::IVcnRegisters {
 public:
  std::vector<Window> windows;
  std::vector<std::pair<uint32_t, uint32_t>> writes;     // every register write the host made, in order
  std::vector<uint64_t> write_times;                     // the clock (microseconds) at each of them, when `clock` is set
  const hardware::IClock* clock = nullptr;
  std::vector<std::pair<uint16_t, uint32_t>> messages;   // every SMC message and its argument
  // The SMC
  bool smc_running = true;
  uint32_t soft_registers = 0x3E000;                     // what the firmware header says; 0 = the header is blank
  uint32_t answer = 1;                                   // the result word of LoadUcodes (1 done, 0xFF failed)
  uint32_t status_polls_before_loaded = 0;               // reads of the status word that still see zero
  uint32_t loadable_mask = 0xFFFFFFFFu;                  // the bits the SMC manages to set
  std::vector<TocEntry> toc;
  uint32_t toc_version = 0, toc_count = 0;
  uint64_t scratch_address = 0, toc_address = 0;
  // The command processor
  bool cp_alive = true;                                  // it runs the packets once its microcode is loaded and not halted
  std::vector<Executed> executed;
  uint32_t protocol_errors = 0;
  uint32_t serdes_busy_reads = 0;                        // reads of the CU busy register that still show busy

  Card() {
    regs_[kSmcResp] = 1;
    regs_[kCpMeCntl] = kMeHalt | kPfpHalt | kCeHalt;
    regs_[kCpMecCntl] = kMecMe1Halt | kMecMe2Halt;
  }

  uint32_t soft_status_address() const { return soft_registers + 108; }
  uint32_t status() const { auto at = sram_.find(soft_status_address()); return at == sram_.end() ? 0 : at->second; }

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
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
  }
  uint32_t reg(uint32_t r) const { auto at = regs_.find(r); return at == regs_.end() ? 0 : at->second; }
  void set_reg(uint32_t r, uint32_t v) { regs_[r] = v; }
  void set_sram(uint32_t address, uint32_t value) { sram_[address] = value; }

  uint32_t read(uint32_t r) override {
    if (r == kSmcData) {
      const uint32_t address = regs_[kSmcIndex];
      if (smc_running && address == soft_status_address() && status_polls_ > 0) {
        --status_polls_;
        return 0;
      }
      auto at = sram_.find(address);
      return at == sram_.end() ? 0 : at->second;
    }
    if (r == kSerdesCuBusy && serdes_busy_reads > 0) {
      --serdes_busy_reads;
      return 1;
    }
    auto it = regs_.find(r);
    return it == regs_.end() ? 0 : it->second;
  }

  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    write_times.push_back(clock ? clock->now_us() : 0);
    if (r == kSmcMessage) {
      handle_message(static_cast<uint16_t>(v));
      return;
    }
    if (r == kSmcData) {
      sram_[regs_[kSmcIndex]] = v;
      return;
    }
    if (r == kRb0Wptr) {
      if (regs_[kRb0Cntl] & 0x80000000u) {   // RB_RPTR_WR_ENA: the pointers are being set
        rptr_ = v;
        regs_[kRb0Rptr] = v;
      }
      regs_[r] = v;
      if (!(regs_[kRb0Cntl] & 0x80000000u)) run_ring();
      return;
    }
    regs_[r] = v;
  }

 private:
  void handle_message(uint16_t message) {
    const uint32_t arg = regs_[kSmcArg];
    messages.push_back({message, arg});
    if (!smc_running) return;
    switch (message) {
      case 0x252: scratch_address = (scratch_address & 0xFFFFFFFFull) | uint64_t{arg} << 32; break;
      case 0x253: scratch_address = (scratch_address & ~0xFFFFFFFFull) | arg; break;
      case 0x250: toc_address = (toc_address & 0xFFFFFFFFull) | uint64_t{arg} << 32; break;
      case 0x251: toc_address = (toc_address & ~0xFFFFFFFFull) | arg; break;
      case 0x254: load_ucodes(arg); return;
      default: break;
    }
    regs_[kSmcResp] = 1;
  }

  void load_ucodes(uint32_t mask) {
    toc.clear();
    toc_version = get32(toc_address);
    toc_count = get32(toc_address + 4);
    uint32_t loaded = 0;
    if (toc_count <= 12) {
      for (uint32_t i = 0; i < toc_count; ++i) {
        const uint64_t at = toc_address + 8 + uint64_t{i} * 28;
        TocEntry e;
        const uint32_t w0 = get32(at);
        e.id = static_cast<uint16_t>(w0);
        e.version = static_cast<uint16_t>(w0 >> 16);
        e.address_high = get32(at + 4);
        e.address_low = get32(at + 8);
        e.meta_high = get32(at + 12);
        e.meta_low = get32(at + 16);
        e.size = get32(at + 20);
        const uint32_t w6 = get32(at + 24);
        e.flags = static_cast<uint16_t>(w6);
        e.registers = static_cast<uint16_t>(w6 >> 16);
        const uint64_t address = uint64_t{e.address_high} << 32 | e.address_low;
        const uint8_t* image = translate(address, e.size);
        if (image) {
          e.image.assign(image, image + e.size);
          e.readable = true;
          if (e.id < 32 && (mask >> e.id & 1)) loaded |= 1u << e.id;
        }
        toc.push_back(e);
      }
    }
    sram_[soft_status_address()] |= loaded & loadable_mask;   // the SMC sets the bits of the images it has loaded
    status_polls_ = status_polls_before_loaded;
    regs_[kSmcResp] = answer;
  }

  // The command processor: packets from the ring between its read and write pointers.
  void run_ring() {
    if (!cp_alive) return;
    if (!ready()) return;
    const uint64_t base = (uint64_t{regs_[kRb0BaseHi]} << 32 | regs_[kRb0Base]) << 8;
    const uint32_t dwords = 2u << (regs_[kRb0Cntl] & 0x3f);   // RB_BUFSZ: 2^n units of 8 bytes
    const uint32_t wptr = regs_[kRb0Wptr] & (dwords - 1);
    while (rptr_ != wptr) {
      const uint32_t header = get32(base + uint64_t{rptr_} * 4);
      if ((header >> 30) != 3) {
        ++protocol_errors;
        rptr_ = wptr;
        break;
      }
      const uint32_t opcode = header >> 8 & 0xFF, count = header >> 16 & 0x3FFF;
      uint32_t body_dwords = count + 1;
      if (opcode == 0x10 && count == 0x3FFF) body_dwords = 0;   // the ring filler: a NOP of count 0x3FFF is one dword
      Executed e;
      e.opcode = opcode;
      uint32_t available = (wptr - rptr_ - 1) & (dwords - 1);
      if (body_dwords > available) {
        ++protocol_errors;
        rptr_ = wptr;
        break;
      }
      for (uint32_t i = 0; i < body_dwords; ++i) e.body.push_back(get32(base + uint64_t{(rptr_ + 1 + i) & (dwords - 1)} * 4));
      rptr_ = (rptr_ + 1 + body_dwords) & (dwords - 1);
      execute(e);
      if (!(opcode == 0x10 && count == 0x3FFF)) executed.push_back(e);
      else ++fillers;
    }
    regs_[kRb0Rptr] = rptr_;
    put32(uint64_t{regs_[kRb0RptrAddrHi]} << 32 | regs_[kRb0RptrAddr], rptr_);
  }

  void execute(const Executed& e) {
    switch (e.opcode) {
      case 0x10: case 0x11: case 0x12: case 0x28: case 0x4A: case 0x69: break;   // NOP, SET_BASE, CLEAR_STATE, CONTEXT_CONTROL, PREAMBLE_CNTL, SET_CONTEXT_REG
      case 0x79:                                                                // SET_UCONFIG_REG
        for (size_t i = 1; i < e.body.size(); ++i) regs_[0xC000 + e.body[0] + static_cast<uint32_t>(i) - 1] = e.body[i];
        break;
      default: ++protocol_errors;
    }
  }

  // The microcode of the three engines is loaded and the halt bits are clear.
  bool ready() const {
    const uint32_t need = (1u << 3) | (1u << 4) | (1u << 5);   // CE, PFP, ME
    if ((status() & need) != need) return false;
    return (reg(kCpMeCntl) & (kMeHalt | kPfpHalt | kCeHalt)) == 0;
  }

  std::map<uint32_t, uint32_t> regs_;
  std::map<uint32_t, uint32_t> sram_;
  uint32_t status_polls_ = 0;
  uint32_t rptr_ = 0;

 public:
  uint32_t fillers = 0;
};

struct FakeClock final : hardware::IClock {
  mutable uint64_t now = 0;
  uint64_t now_us() const override { now += 50; return now; }
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

}  // namespace fakegfx
