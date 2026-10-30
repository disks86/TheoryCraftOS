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


// The Synopsys DesignWare I2C master (DW_apb_i2c) that Intel's low power subsystem (LPSS) puts behind PCI functions on the Sunrise Point-LP PCH of the Dell XPS 15 9575, and that the
// touchpad and the touchscreen hang on. Written from the register map of the DesignWare databook as i2c-designware-*.c of the Linux kernel uses it. The transfer is polled: the
// commands go into the transmit FIFO as the FIFO has room, the bytes come out of the receive FIFO, the abort source is read if the transaction aborts (a device that does not answer).
//
// The LPSS wrapper (intel-lpss.c): the private registers at offset 0x200 of the function's first BAR hold the reset of the function (released by writing 3 to the register at 0x204)
// and the clock of the controller (120 MHz on Sunrise Point, intel-lpss-pci.c); `lpss_release_reset` does the former. Not done: interrupts (polled), 10 bit addresses, high speed
// mode, DMA, the slave role, suspend.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/I2c/II2cMaster.hpp"
#include "Hardware/Mmio.hpp"

namespace hardware {

// The registers of the controller, 32 bit at a byte offset: MMIO in the machine, a model in the tests.
class II2cRegisters {
 public:
  virtual uint32_t read32(uint32_t offset) = 0;
  virtual void write32(uint32_t offset, uint32_t value) = 0;

 protected:
  constexpr II2cRegisters() = default;
  ~II2cRegisters() = default;
};

class MmioI2cRegisters final : public II2cRegisters {
 public:
  explicit MmioI2cRegisters(const MmioRegion& region) : region_(region) {}
  uint32_t read32(uint32_t offset) override { return region_.read32(offset); }
  void write32(uint32_t offset, uint32_t value) override { region_.write32(offset, value); }

 private:
  MmioRegion region_;
};

namespace dw_i2c {
constexpr uint32_t kCon = 0x00, kTar = 0x04, kDataCmd = 0x10, kSsHcnt = 0x14, kSsLcnt = 0x18, kFsHcnt = 0x1C, kFsLcnt = 0x20, kIntrStat = 0x2C, kIntrMask = 0x30, kRawIntrStat = 0x34,
                   kRxTl = 0x38, kTxTl = 0x3C, kClrIntr = 0x40, kClrTxAbrt = 0x54, kEnable = 0x6C, kStatus = 0x70, kTxFlr = 0x74, kRxFlr = 0x78, kSdaHold = 0x7C, kTxAbrtSource = 0x80,
                   kEnableStatus = 0x9C, kCompParam1 = 0xF4, kCompVersion = 0xF8, kCompType = 0xFC;
constexpr uint32_t kCompTypeValue = 0x44570140;                              // "DW" 0x0140
constexpr uint32_t kConMaster = 1u << 0, kConSpeedStandard = 1u << 1, kConSpeedFast = 2u << 1, kConRestartEnable = 1u << 5, kConSlaveDisable = 1u << 6, kConTxEmptyCtrl = 1u << 8;
constexpr uint32_t kCmdRead = 1u << 8, kCmdStop = 1u << 9, kCmdRestart = 1u << 10;
constexpr uint32_t kIntrTxAbrt = 1u << 6, kIntrStopDet = 1u << 9;
constexpr uint32_t kStatusActivity = 1u << 0, kStatusTxNotFull = 1u << 1, kStatusTxEmpty = 1u << 2, kStatusRxNotEmpty = 1u << 3, kStatusMasterActivity = 1u << 5;
}  // namespace dw_i2c

class DesignWareI2c final : public II2cMaster {
 public:
  struct Config {
    uint32_t clock_hz = 120000000;       // the controller's clock (Sunrise Point's LPSS I2C: 120 MHz)
    uint32_t bus_hz = 400000;            // 100000 (standard) or 400000 (fast)
    uint32_t sda_hold_ns = 230;          // intel-lpss-pci.c: "i2c-sda-hold-time-ns" of Sunrise Point
    // Counts the firmware gave (the ACPI methods SSCN / FMCN of the controller: the high and low counts of the clock); 0: worked out from the clock as the kernel does.
    uint32_t ss_hcnt = 0, ss_lcnt = 0, fs_hcnt = 0, fs_lcnt = 0;
    uint32_t sda_hold_register = 0;      // the SDA hold register value the firmware gave (the third element of SSCN / FMCN); 0: from `sda_hold_ns`
  };

  explicit DesignWareI2c(II2cRegisters& regs) : regs_(regs) {}

  // Checks the component type, reads the FIFO depths, programs the clock counts and the speed. False if this is not a DesignWare controller.
  bool initialize(const Config& config);
  bool transfer(uint16_t address, ConstByteView write, ByteView read) override;

  uint32_t tx_depth() const { return tx_depth_; }
  uint32_t rx_depth() const { return rx_depth_; }
  uint32_t last_abort_source() const { return abort_source_; }

  // The LPSS wrapper: takes the function out of reset (private registers at 0x200, reset register at +4). Call before initialize().
  static void lpss_release_reset(II2cRegisters& regs) { regs.write32(kLpssPrivate + kLpssResets, 3); }
  static constexpr uint32_t kLpssPrivate = 0x200, kLpssResets = 0x04;

  // The clock counts of the kernel: IC_CLK in kHz, times in ns.
  static uint32_t high_count(uint32_t clock_khz, uint32_t t_symbol_ns, uint32_t fall_ns);
  static uint32_t low_count(uint32_t clock_khz, uint32_t t_low_ns, uint32_t fall_ns);

 private:
  bool disable();
  bool enable();
  II2cRegisters& regs_;
  uint32_t tx_depth_ = 0, rx_depth_ = 0, abort_source_ = 0;
  bool ready_ = false;
};

}  // namespace hardware
