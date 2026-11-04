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

#pragma once
#include <stdint.h>
#include "Hardware/Mmio.hpp"

namespace hardware {

// The two hardware generations an Intel NPU ("VPU") is built from, named as the Linux ivpu driver names them (drivers/accel/ivpu): the VPU IP block (its
// "37XX", "40XX", ... register layout) and the buttress that sits in front of it and owns clocks, power and the work point.
enum class IntelNpuIp : uint8_t { Unknown, Ip37xx, Ip40xx, Ip50xx, Ip60xx };
enum class IntelNpuButtress : uint8_t { Unknown, Mtl, Lnl };

struct IntelNpuPlatform {
  const char* name = nullptr;  // the platform the device ships in; nullptr for a device that is not a known Intel NPU
  IntelNpuIp ip = IntelNpuIp::Unknown;
  IntelNpuButtress buttress = IntelNpuButtress::Unknown;
  constexpr bool valid() const { return name != nullptr; }
};

constexpr IntelNpuPlatform intel_npu_platform(uint16_t device_id) {
  switch (device_id) {
    case 0x7D1D: return {"Meteor Lake", IntelNpuIp::Ip37xx, IntelNpuButtress::Mtl};
    case 0xAD1D: return {"Arrow Lake", IntelNpuIp::Ip37xx, IntelNpuButtress::Mtl};
    case 0x643E: return {"Lunar Lake", IntelNpuIp::Ip40xx, IntelNpuButtress::Lnl};
    case 0xB03E: return {"Panther Lake", IntelNpuIp::Ip50xx, IntelNpuButtress::Lnl};
    case 0xFD3E: return {"Wildcat Lake", IntelNpuIp::Ip50xx, IntelNpuButtress::Lnl};
    case 0xD71D: return {"Nova Lake", IntelNpuIp::Ip60xx, IntelNpuButtress::Lnl};
    default: return {};
  }
}

// 32/64-bit register access to one BAR of the NPU. The real one is a window onto the BAR; a test supplies a pretend NPU.
class INpuRegisters {
 public:
  virtual uint32_t read32(uint64_t offset) = 0;
  virtual void write32(uint64_t offset, uint32_t value) = 0;
  virtual void write64(uint64_t offset, uint64_t value) = 0;

 protected:
  constexpr INpuRegisters() = default;
  ~INpuRegisters() = default;
};

class MmioNpuRegisters final : public INpuRegisters {
 public:
  explicit MmioNpuRegisters(const MmioRegion& region) : region_(region) {}
  uint32_t read32(uint64_t offset) override { return region_.read32(offset); }
  void write32(uint64_t offset, uint32_t value) override { region_.write32(offset, value); }
  void write64(uint64_t offset, uint64_t value) override { region_.write64(offset, value); }

 private:
  MmioRegion region_;
};

// What a read-only look at the NPU's buttress and identity registers found.
struct IntelNpuInfo {
  bool valid = false;            // the registers answered sensibly (not all-ones, fuses valid, ratios in order)
  uint32_t tile_disable_mask = 0;  // Lunar Lake and newer: bit n set = tile n is fused off; Meteor/Arrow Lake always have both tiles
  uint32_t min_ratio = 0, max_ratio = 0, pn_ratio = 0;  // PLL ratios from the fuses, pn_ratio clamped between the other two
  uint32_t min_mhz = 0, max_mhz = 0, pn_mhz = 0;
  bool ready = false;            // VPU_STATUS.READY: the NPU has a work point and is out of reset
  bool idle = false;
  uint32_t hw_version = 0;       // HOST_SS_HW_VERSION (SoC revision, SoC number, VPU generation)
  uint32_t sw_version = 0;       // HOST_SS_SW_VERSION (Lunar Lake and newer)
};

enum class IntelNpuStep : uint8_t { None, Info, WorkPoint, HostSs, ClockAck, PowerDomain, Axi, TopNoc, CpuNoc };

struct IntelNpuResult {
  bool ok = true;
  IntelNpuStep failed = IntelNpuStep::None;
  bool d0i3_warning = false;  // leaving the D0i3 low-power state did not complete; ivpu goes on regardless, so does this
  static IntelNpuResult fail(IntelNpuStep step) { IntelNpuResult r; r.ok = false; r.failed = step; return r; }
};
const char* intel_npu_step_name(IntelNpuStep step);

// The hardware side of the Linux ivpu driver as far as the host goes: reading the fuses, asking the buttress for a work point, bringing the
// IP block's clocks, reset, power island and NoC queues up, and starting the NPU's CPU at a firmware entry point (ivpu_hw_btrs.c, ivpu_hw_ip.c,
// ivpu_hw.c: ivpu_hw_power_up). The firmware image, its boot parameters and the load-power-up-boot sequence are in IntelNpuFirmware.hpp. The NPU MMU, its page tables and contexts are in IntelNpuMmu.hpp. Not here yet:
// the IPC and job queues (the "JSM" messages and the
// ready message the firmware sends), interrupts, and the power-down/reset path.
// `ip` is BAR 0, `buttress` is BAR 4 (ivpu's regv and regb).
class IntelNpuHw {
 public:
  // Reads of a status register tried before a step gives up (about a microsecond each on PCI Express).
  static constexpr uint32_t kPollAttempts = 200000;
  // How much of BAR 0 / BAR 4 the code touches.
  static uint64_t ip_window(IntelNpuIp ip);
  static constexpr uint64_t kButtressWindow = 0x1000;

  IntelNpuHw(INpuRegisters& ip, INpuRegisters& buttress, const IntelNpuPlatform& platform)
      : ip_(ip), btrs_(buttress), platform_(platform) {}

  // Reads only. Fills and remembers the fuse-derived limits that power_up() uses.
  IntelNpuInfo read_info();
  const IntelNpuInfo& info() const { return info_; }

  // The power-up sequence of ivpu_hw_power_up(): leave D0i3, request the work point, configure the host subsystem, enable the power island, clocks,
  // reset and the AXI and top NoC queues. Writes registers; call read_info() first.
  IntelNpuResult power_up();
  // Starts the NPU's CPU at `entry_point` (a VPU address of the firmware's cold-boot entry), as ivpu_hw_ip_soc_cpu_boot() does. After power_up().
  IntelNpuResult boot(uint64_t entry_point);

  static uint32_t ratio_to_mhz(IntelNpuButtress buttress, uint32_t ratio);

 private:
  bool poll(INpuRegisters& r, uint64_t offset, uint32_t mask, uint32_t expected) const;
  bool d0i3_disable();
  bool work_point(bool enable);
  bool host_ss_configure();
  bool power_domain_enable();
  bool nocs_enable_host_ss();
  bool top_noc_enable();
  uint32_t l2_mask() const;

  INpuRegisters& ip_;
  INpuRegisters& btrs_;
  IntelNpuPlatform platform_;
  IntelNpuInfo info_;
};

}  // namespace hardware
