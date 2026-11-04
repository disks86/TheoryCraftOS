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

#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"

namespace hardware {

namespace {
// Register offsets and bits, from the Linux ivpu driver (ivpu_hw_btrs_reg.h and ivpu_hw_ip_reg.h). BAR 4, the buttress:
constexpr uint64_t kMtlFminFuse = 0x24, kMtlFmaxFuse = 0x28, kMtlPllStatus = 0x40, kMtlVpuStatus = 0x44, kMtlD0i3 = 0x60;
constexpr uint64_t kMtlWpPayload0 = 0x08, kMtlWpPayload1 = 0x0C, kMtlWpPayload2 = 0x10, kMtlWpCmd = 0x14;
constexpr uint64_t kLnlWpPayload0 = 0x130, kLnlWpPayload1 = 0x134, kLnlWpPayload2 = 0x138, kLnlWpCmd = 0x13C;
constexpr uint64_t kLnlCdyn = 0x14C, kLnlTileFuse = 0x150, kLnlVpuStatus = 0x154, kLnlD0i3 = 0x164, kLnlFminFuse = 0x174, kLnlFmaxFuse = 0x178;
constexpr uint64_t kLnlPortWeightsAts = 0x24, kLnlPortWeights = 0x54;
constexpr uint32_t kWpSend = 1u << 0;
constexpr uint32_t kPllLock = 1u << 1;
constexpr uint32_t kStatusReady = 1u << 0, kStatusIdle = 1u << 1, kStatusClockOwnAck = 1u << 6;
constexpr uint32_t kD0i3InProgress = 1u << 0, kD0i3I3 = 1u << 2;
constexpr uint32_t kPortWeights = 0xF711F711u, kPortWeightsAts = 0x0000F711u;
constexpr uint32_t kPllEppDefault = 0x80, kPllCdynDefault = 0x80;
constexpr uint32_t kMtlWorkPointConfig = (2u << 8) | 2u;  // two tiles, PLL ratio 4/3
constexpr uint32_t kPllRefMhz = 50;

// BAR 0, the IP block:
constexpr uint64_t kCprClkEn40 = 0x80, kCprClkSet37 = 0x84, kCprRstEn40 = 0x90, kCprRstSet37 = 0x94, kCprRstClr37 = 0x98;
constexpr uint64_t kHwVersion = 0x108, kSwVersion = 0x10C;
constexpr uint64_t kHostNocQreqn = 0x154, kHostNocQacceptn = 0x158, kHostNocQdeny = 0x15C;
constexpr uint64_t kTopNocQreqn = 0x160, kTopNocQacceptn = 0x164, kTopNocQdeny = 0x168;
constexpr uint64_t kAonPwrIso = 0x30020, kAonPwrIslandEn = 0x30024, kAonPwrIslandTrickle = 0x30028, kAonPwrIslandStatus = 0x3002C;
constexpr uint64_t kAonPwrIslandEnPostDly = 0x30068, kAonPwrIslandStatusDly = 0x3006C;
constexpr uint64_t kAonIdleGen = 0x30200, kAonDpuActive = 0x30204;
constexpr uint64_t kVerificationAddressLo = 0x40040, kLoadingAddressLo = 0x41040;
constexpr uint64_t kCpuNocQreqn40 = 0x01010030, kCpuNocQacceptn40 = 0x01010034, kCpuNocQdeny40 = 0x01010038;
constexpr uint64_t kLeonRtVec37 = 0x06010040;
constexpr uint32_t kClkTopNoc = 1u << 1, kClkDss = 1u << 10, kClkCssMss = 1u << 11, kClkAll = kClkTopNoc | kClkDss | kClkCssMss;
constexpr uint32_t kRstClrAon = 1u << 0;
constexpr uint32_t kHostNocTopSocMmio = 1u << 0;
constexpr uint32_t kTopNocCpuCtrl = 1u << 0, kTopNocL2Cache37 = 1u << 1, kTopNocL2Cache40 = 1u << 2;
constexpr uint32_t kPwrIslandMssCpu = 1u << 3;
constexpr uint32_t kIdleGenEnable = 1u << 0, kDpuActive = 1u << 0;
constexpr uint32_t kLoadingDone = 1u << 0;
constexpr uint32_t kCpuNocTopMmio = 1u << 0;
constexpr uint32_t kLeonRstRun0 = 1u << 0, kLeonResume0 = 1u << 1, kLeonRstVec = 0xFFFFFFF0u;
constexpr uint32_t kTileFuseValid = 1u << 0;
constexpr uint32_t kTileFuseConfigShift = 1, kTileFuseConfigMask = 0x7Fu;
constexpr uint32_t kAllOnes = 0xFFFFFFFFu;
}  // namespace

const char* intel_npu_step_name(IntelNpuStep step) {
  switch (step) {
    case IntelNpuStep::None: return "none";
    case IntelNpuStep::Info: return "fuses";
    case IntelNpuStep::WorkPoint: return "work point";
    case IntelNpuStep::HostSs: return "host subsystem";
    case IntelNpuStep::ClockAck: return "clock resource";
    case IntelNpuStep::PowerDomain: return "power island";
    case IntelNpuStep::Axi: return "host AXI";
    case IntelNpuStep::TopNoc: return "top NoC";
    case IntelNpuStep::CpuNoc: return "CPU NoC";
  }
  return "unknown";
}

uint64_t IntelNpuHw::ip_window(IntelNpuIp ip) {
  switch (ip) {
    case IntelNpuIp::Ip37xx: return 0x06020000;  // up to LEON_RT_VEC at 0x06010040
    case IntelNpuIp::Ip40xx:
    case IntelNpuIp::Ip50xx:
    case IntelNpuIp::Ip60xx: return 0x01011000;  // up to the CPU NoC registers at 0x01010038
    default: return 0;
  }
}

uint64_t IntelNpuHw::operational_window(IntelNpuIp ip) {
  switch (ip) {
    case IntelNpuIp::Ip37xx: return 0x06400000;  // doorbells 0x06300000 + 255 * 0x1000
    case IntelNpuIp::Ip40xx:
    case IntelNpuIp::Ip50xx:
    case IntelNpuIp::Ip60xx: return 0x01400000;  // doorbells 0x01300000 + 255 * 0x1000
    default: return 0;
  }
}

// The reference clock is 50 MHz; the ratio means 2/3 of it per step on Meteor/Arrow Lake and 1/2 on the later parts.
uint32_t IntelNpuHw::ratio_to_mhz(IntelNpuButtress buttress, uint32_t ratio) {
  return buttress == IntelNpuButtress::Mtl ? ratio * kPllRefMhz * 2 / 3 : ratio * kPllRefMhz / 2;
}

bool IntelNpuHw::poll(INpuRegisters& r, uint64_t offset, uint32_t mask, uint32_t expected) const {
  for (uint32_t i = 0; i < kPollAttempts; ++i)
    if ((r.read32(offset) & mask) == expected) return true;
  return false;
}

uint32_t IntelNpuHw::l2_mask() const { return platform_.ip == IntelNpuIp::Ip37xx ? kTopNocL2Cache37 : kTopNocL2Cache40; }

IntelNpuInfo IntelNpuHw::read_info() {
  IntelNpuInfo info;
  info_ = info;
  if (!platform_.valid() || platform_.buttress == IntelNpuButtress::Unknown) return info_;
  const bool mtl = platform_.buttress == IntelNpuButtress::Mtl;
  const uint32_t fmin = btrs_.read32(mtl ? kMtlFminFuse : kLnlFminFuse);
  const uint32_t fmax = btrs_.read32(mtl ? kMtlFmaxFuse : kLnlFmaxFuse);
  const uint32_t status = btrs_.read32(mtl ? kMtlVpuStatus : kLnlVpuStatus);
  if (fmin == kAllOnes || fmax == kAllOnes || status == kAllOnes) return info_;  // a device that is off the bus (or powered down) reads all-ones
  if (!mtl) {
    const uint32_t tile_fuse = btrs_.read32(kLnlTileFuse);
    if (tile_fuse == kAllOnes || !(tile_fuse & kTileFuseValid)) return info_;
    info.tile_disable_mask = (tile_fuse >> kTileFuseConfigShift) & kTileFuseConfigMask;
  }
  info.min_ratio = fmin & 0xFF;
  info.pn_ratio = (fmin >> 8) & 0xFF;
  info.max_ratio = fmax & 0xFF;
  if (info.max_ratio == 0 || info.min_ratio > info.max_ratio) return info_;
  if (info.pn_ratio < info.min_ratio) info.pn_ratio = info.min_ratio;
  if (info.pn_ratio > info.max_ratio) info.pn_ratio = info.max_ratio;
  info.min_mhz = ratio_to_mhz(platform_.buttress, info.min_ratio);
  info.max_mhz = ratio_to_mhz(platform_.buttress, info.max_ratio);
  info.pn_mhz = ratio_to_mhz(platform_.buttress, info.pn_ratio);
  info.ready = (status & kStatusReady) != 0;
  info.idle = (status & kStatusIdle) != 0;
  info.hw_version = ip_.read32(kHwVersion);
  if (platform_.ip != IntelNpuIp::Ip37xx) info.sw_version = ip_.read32(kSwVersion);
  info.valid = true;
  info_ = info;
  return info_;
}

bool IntelNpuHw::d0i3_disable() {
  const uint64_t reg = platform_.buttress == IntelNpuButtress::Mtl ? kMtlD0i3 : kLnlD0i3;
  if (!poll(btrs_, reg, kD0i3InProgress, 0)) return false;
  btrs_.write32(reg, btrs_.read32(reg) & ~kD0i3I3);
  return poll(btrs_, reg, kD0i3InProgress, 0);
}

// ivpu_hw_btrs_wp_drive: wait for the previous request to finish, write the payload, set SEND, wait for it to be taken, then wait for the PLL to lock
// (Meteor/Arrow Lake) and the NPU to report ready.
bool IntelNpuHw::work_point(bool enable) {
  const bool mtl = platform_.buttress == IntelNpuButtress::Mtl;
  const uint64_t p0 = mtl ? kMtlWpPayload0 : kLnlWpPayload0, p1 = mtl ? kMtlWpPayload1 : kLnlWpPayload1;
  const uint64_t p2 = mtl ? kMtlWpPayload2 : kLnlWpPayload2, cmd = mtl ? kMtlWpCmd : kLnlWpCmd;
  const uint32_t target = mtl ? (enable ? info_.pn_ratio : 0) : info_.pn_ratio;
  const uint32_t cfg = mtl ? (enable ? kMtlWorkPointConfig : 0) : 0;
  const uint32_t epp = mtl ? kPllEppDefault : (enable ? kPllEppDefault : 0);
  const uint32_t cdyn = mtl ? 0 : (enable ? kPllCdynDefault : 0);
  if (!poll(btrs_, cmd, kWpSend, 0)) return false;
  btrs_.write32(p0, (info_.max_ratio << 16) | info_.min_ratio);
  btrs_.write32(p1, (epp << 16) | target);
  // Payload 2 holds the configuration in its low half and, from Lunar Lake on, the dynamic capacitance in its high half (left alone before).
  btrs_.write32(p2, mtl ? (btrs_.read32(p2) & 0xFFFF0000u) | cfg : (cdyn << 16) | cfg);
  btrs_.write32(cmd, btrs_.read32(cmd) | kWpSend);
  if (!poll(btrs_, cmd, kWpSend, 0)) return false;
  if (mtl && !poll(btrs_, kMtlPllStatus, kPllLock, enable ? kPllLock : 0)) return false;
  if (!poll(btrs_, mtl ? kMtlVpuStatus : kLnlVpuStatus, kStatusReady, enable ? kStatusReady : 0)) return false;
  if (!mtl && !enable && !poll(btrs_, kLnlCdyn, 0xFFFF, 0)) return false;
  return true;
}

bool IntelNpuHw::host_ss_configure() {
  if (platform_.ip == IntelNpuIp::Ip37xx) {
    if (!poll(ip_, kCprRstClr37, kRstClrAon, 0)) return false;  // the IP's BAR is usable once AON comes out of reset
    ip_.write32(kCprRstClr37, kClkAll);                          // (the same three bits release TOP_NOC, DSS_MAS and MSS_MAS from reset)
  }
  return !(ip_.read32(kHostNocQreqn) & kHostNocTopSocMmio) && !(ip_.read32(kHostNocQacceptn) & kHostNocTopSocMmio) &&
         !(ip_.read32(kHostNocQdeny) & kHostNocTopSocMmio);
}

bool IntelNpuHw::power_domain_enable() {
  // Power-island delays (Panther/Wildcat Lake and Nova Lake), for the default (not high) profiling frequency.
  if (platform_.ip == IntelNpuIp::Ip50xx) {
    ip_.write32(kAonPwrIslandEnPostDly, 0);
    ip_.write32(kAonPwrIslandStatusDly, 3);
  } else if (platform_.ip == IntelNpuIp::Ip60xx) {
    ip_.write32(kAonPwrIslandEnPostDly, 17u | (17u << 16));
    ip_.write32(kAonPwrIslandStatusDly, 0);
  }
  ip_.write32(kAonPwrIslandTrickle, ip_.read32(kAonPwrIslandTrickle) | kPwrIslandMssCpu);
  (void)ip_.read32(kAonPwrIslandTrickle);  // ivpu waits 500 ns here; the read-back is a posted-write flush that takes at least that long on PCIe
  ip_.write32(kAonPwrIslandEn, ip_.read32(kAonPwrIslandEn) | kPwrIslandMssCpu);
  if (!poll(ip_, kAonPwrIslandStatus, kPwrIslandMssCpu, kPwrIslandMssCpu)) return false;
  const uint32_t qreqn = ip_.read32(kTopNocQreqn);
  if ((qreqn & kTopNocCpuCtrl) || (qreqn & l2_mask())) return false;
  const bool legacy = platform_.ip == IntelNpuIp::Ip37xx;
  const uint64_t clk = legacy ? kCprClkSet37 : kCprClkEn40, rst = legacy ? kCprRstSet37 : kCprRstEn40;
  ip_.write32(clk, ip_.read32(clk) | kClkAll);
  ip_.write32(kAonPwrIso, ip_.read32(kAonPwrIso) & ~kPwrIslandMssCpu);
  ip_.write32(rst, ip_.read32(rst) | kClkAll);
  if (legacy) ip_.write32(kAonDpuActive, ip_.read32(kAonDpuActive) | kDpuActive);
  return true;
}

bool IntelNpuHw::nocs_enable_host_ss() {
  ip_.write32(kHostNocQreqn, ip_.read32(kHostNocQreqn) | kHostNocTopSocMmio);
  return (ip_.read32(kHostNocQacceptn) & kHostNocTopSocMmio) && !(ip_.read32(kHostNocQdeny) & kHostNocTopSocMmio);
}

bool IntelNpuHw::top_noc_enable() {
  const uint32_t bits = kTopNocCpuCtrl | l2_mask();
  ip_.write32(kTopNocQreqn, ip_.read32(kTopNocQreqn) | bits);
  return (ip_.read32(kTopNocQacceptn) & bits) == bits && !(ip_.read32(kTopNocQdeny) & bits);
}

IntelNpuResult IntelNpuHw::power_up() {
  if (!info_.valid) return IntelNpuResult::fail(IntelNpuStep::Info);
  IntelNpuResult result;
  result.d0i3_warning = !d0i3_disable();
  if (!work_point(true)) return IntelNpuResult::fail(IntelNpuStep::WorkPoint);
  if (!host_ss_configure()) return IntelNpuResult::fail(IntelNpuStep::HostSs);
  ip_.write32(kAonIdleGen, ip_.read32(kAonIdleGen) & ~kIdleGenEnable);
  if (platform_.buttress != IntelNpuButtress::Mtl && !poll(btrs_, kLnlVpuStatus, kStatusClockOwnAck, kStatusClockOwnAck))
    return IntelNpuResult::fail(IntelNpuStep::ClockAck);
  if (!power_domain_enable()) return IntelNpuResult::fail(IntelNpuStep::PowerDomain);
  if (!nocs_enable_host_ss()) return IntelNpuResult::fail(IntelNpuStep::Axi);
  if (platform_.buttress == IntelNpuButtress::Lnl) {
    btrs_.write32(kLnlPortWeights, kPortWeights);
    btrs_.write32(kLnlPortWeightsAts, kPortWeightsAts);
  }
  if (!top_noc_enable()) return IntelNpuResult::fail(IntelNpuStep::TopNoc);
  return result;
}

IntelNpuResult IntelNpuHw::boot(uint64_t entry_point) {
  switch (platform_.ip) {
    case IntelNpuIp::Ip37xx: {
      if (entry_point > 0xFFFFFFFFull << 9) return IntelNpuResult::fail(IntelNpuStep::CpuNoc);  // the register holds entry_point >> 9 in 32 bits
      uint32_t vec = ip_.read32(kLeonRtVec37) | kLeonRstRun0;
      vec &= ~kLeonRstVec;
      ip_.write32(kLeonRtVec37, vec);
      vec |= kLeonResume0;
      ip_.write32(kLeonRtVec37, vec);
      vec &= ~kLeonResume0;
      ip_.write32(kLeonRtVec37, vec);
      const uint32_t addr = static_cast<uint32_t>(entry_point >> 9);
      ip_.write32(kLoadingAddressLo, addr);
      ip_.write32(kLoadingAddressLo, addr | kLoadingDone);
      return {};
    }
    case IntelNpuIp::Ip40xx:
    case IntelNpuIp::Ip50xx:
    case IntelNpuIp::Ip60xx: {
      if (platform_.ip != IntelNpuIp::Ip60xx) {  // Nova Lake needs no CPU NoC request
        ip_.write32(kCpuNocQreqn40, ip_.read32(kCpuNocQreqn40) | kCpuNocTopMmio);
        if (!(ip_.read32(kCpuNocQacceptn40) & kCpuNocTopMmio) || (ip_.read32(kCpuNocQdeny40) & kCpuNocTopMmio))
          return IntelNpuResult::fail(IntelNpuStep::CpuNoc);
      }
      if (entry_point > (0xFFFFFFFFFFFFFFFFull >> 3)) return IntelNpuResult::fail(IntelNpuStep::CpuNoc);
      ip_.write64(kVerificationAddressLo, entry_point << 3);  // the entry point sits in bits 31:3 (and up) of the register pair
      ip_.write32(kVerificationAddressLo, ip_.read32(kVerificationAddressLo) | kLoadingDone);
      return {};
    }
    default: return IntelNpuResult::fail(IntelNpuStep::Info);
  }
}

}  // namespace hardware
