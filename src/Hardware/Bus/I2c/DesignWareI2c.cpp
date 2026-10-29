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


#include "Hardware/Bus/I2c/DesignWareI2c.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace dw_i2c;

namespace {

constexpr uint32_t kMaxTransfer = 4096;
constexpr uint32_t kSpinLimit = 2000000;             // iterations of a polling loop with a pause between: seconds, not microseconds, but only a device that is stuck gets there

// round(a * b / 1000000)
uint32_t scaled(uint32_t clock_khz, uint32_t ns) { return static_cast<uint32_t>((static_cast<uint64_t>(clock_khz) * ns + 500000) / 1000000); }

}  // namespace

uint32_t DesignWareI2c::high_count(uint32_t clock_khz, uint32_t t_symbol_ns, uint32_t fall_ns) {
  const uint32_t v = scaled(clock_khz, t_symbol_ns + fall_ns);
  return v > 3 ? v - 3 : 1;
}

uint32_t DesignWareI2c::low_count(uint32_t clock_khz, uint32_t t_low_ns, uint32_t fall_ns) {
  const uint32_t v = scaled(clock_khz, t_low_ns + fall_ns);
  return v > 1 ? v - 1 : 1;
}

bool DesignWareI2c::disable() {
  regs_.write32(kEnable, 0);
  for (uint32_t i = 0; i < 1000; ++i) {
    if ((regs_.read32(kEnableStatus) & 1) == 0) return true;
    delay_us(25);
  }
  return false;
}

bool DesignWareI2c::enable() {
  regs_.write32(kEnable, 1);
  for (uint32_t i = 0; i < 1000; ++i) {
    if ((regs_.read32(kEnableStatus) & 1) != 0) return true;
    delay_us(25);
  }
  return false;
}

bool DesignWareI2c::initialize(const Config& config) {
  ready_ = false;
  if (regs_.read32(kCompType) != kCompTypeValue) return false;
  if (config.clock_hz == 0 || (config.bus_hz != 100000 && config.bus_hz != 400000)) return false;
  if (!disable()) return false;
  const uint32_t param = regs_.read32(kCompParam1);
  tx_depth_ = ((param >> 16) & 0xFF) + 1;
  rx_depth_ = ((param >> 8) & 0xFF) + 1;
  if (tx_depth_ < 2 || rx_depth_ < 2) return false;
  const uint32_t khz = config.clock_hz / 1000;
  // standard mode: tHD;STA 4.0 us, tLOW 4.7 us; fast mode: 0.6 us and 1.3 us; the fall time of the signals taken as 0.3 us (i2c-designware-common.c)
  regs_.write32(kSsHcnt, config.ss_hcnt ? config.ss_hcnt : high_count(khz, 4000, 300));
  regs_.write32(kSsLcnt, config.ss_lcnt ? config.ss_lcnt : low_count(khz, 4700, 300));
  regs_.write32(kFsHcnt, config.fs_hcnt ? config.fs_hcnt : high_count(khz, 600, 300));
  regs_.write32(kFsLcnt, config.fs_lcnt ? config.fs_lcnt : low_count(khz, 1300, 300));
  if (config.sda_hold_register != 0) regs_.write32(kSdaHold, config.sda_hold_register);
  else if (config.sda_hold_ns != 0) regs_.write32(kSdaHold, scaled(khz, config.sda_hold_ns));
  regs_.write32(kCon, kConMaster | kConSlaveDisable | kConRestartEnable | kConTxEmptyCtrl | (config.bus_hz >= 400000 ? kConSpeedFast : kConSpeedStandard));
  regs_.write32(kIntrMask, 0);
  regs_.write32(kRxTl, 0);
  regs_.write32(kTxTl, 0);
  ready_ = true;
  return true;
}

bool DesignWareI2c::transfer(uint16_t address, ConstByteView write, ByteView read) {
  abort_source_ = 0;
  if (!ready_ || address > 0x7F || (write.empty() && read.empty()) || write.size() > kMaxTransfer || read.size() > kMaxTransfer) return false;
  if (!disable()) return false;
  regs_.write32(kTar, address);
  regs_.write32(kIntrMask, 0);
  if (!enable()) return false;
  (void)regs_.read32(kClrIntr);

  const uint64_t total = write.size() + read.size();
  uint64_t sent = 0, received = 0;
  uint32_t idle = 0;
  for (uint32_t spin = 0; spin < kSpinLimit; ++spin) {
    if (regs_.read32(kRawIntrStat) & kIntrTxAbrt) {
      abort_source_ = regs_.read32(kTxAbrtSource);
      (void)regs_.read32(kClrTxAbrt);
      disable();
      return false;
    }
    bool progress = false;
    // commands into the transmit FIFO while it has room (and, for reads, while the receive FIFO can take the answers)
    uint32_t room = tx_depth_ - regs_.read32(kTxFlr);
    if (room > tx_depth_) room = 0;
    while (sent < total && room > 0) {
      uint32_t cmd;
      if (sent < write.size()) {
        cmd = write[sent];
        if (sent + 1 == total) cmd |= kCmdStop;
      } else {
        if (sent - write.size() - received >= rx_depth_) break;
        cmd = kCmdRead;
        if (sent == write.size() && !write.empty()) cmd |= kCmdRestart;
        if (sent + 1 == total) cmd |= kCmdStop;
      }
      regs_.write32(kDataCmd, cmd);
      ++sent;
      --room;
      progress = true;
    }
    // the bytes that came in
    uint32_t level = regs_.read32(kRxFlr);
    while (level-- > 0) {
      const uint32_t v = regs_.read32(kDataCmd);
      if (received < read.size()) read[received] = static_cast<uint8_t>(v);
      ++received;
      progress = true;
    }
    if (sent == total && received >= read.size()) {
      // everything went out and came back: done once the stop condition has ended the bus activity
      if ((regs_.read32(kStatus) & kStatusActivity) == 0) {
        if (regs_.read32(kRawIntrStat) & kIntrTxAbrt) continue;           // (an abort that came with the end of the activity)
        return true;
      }
    }
    if (!progress && ++idle > 4) delay_us(5);
    else if (progress) idle = 0;
  }
  disable();
  return false;
}

}  // namespace hardware
