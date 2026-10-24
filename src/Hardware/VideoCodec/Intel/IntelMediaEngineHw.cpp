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

#include "Hardware/VideoCodec/Intel/IntelMediaEngineHw.hpp"

namespace hardware {

namespace {
// Forcewake: the GPU powers its engines down on its own; setting the "kernel" bit of a box's forcewake register (a masked register: the upper
// half says which bits the write changes) asks for power, and the acknowledge register shows when it is on.
constexpr uint32_t kForcewakeKernelBit = 1;
constexpr uint32_t kMaskedEnable = (kForcewakeKernelBit << 16) | kForcewakeKernelBit;
constexpr uint32_t kMaskedDisable = kForcewakeKernelBit << 16;

// Gen9.5: one media well for the (single) video box.
constexpr uint32_t kForcewakeMediaGen9 = 0xA270;
constexpr uint32_t kForcewakeAckMediaGen9 = 0x0D88;

// Gen11 and later: a well per video box. The platforms covered here have boxes VCS0 and VCS2 (VCS1 and VCS3 are not built); the disable fuses
// are bits 0-7 of GEN11_GT_VEBOX_VDBOX_DISABLE.
constexpr uint32_t kForcewakeMediaVdbox = 0xA540;  // + 4 * box
constexpr uint32_t kForcewakeAckMediaVdbox = 0x0D50;
constexpr uint32_t kVeboxVdboxDisable = 0x9140;
constexpr uint32_t kVdboxDisableMask = 0xFF;
constexpr uint32_t kVdboxBuilt = (1u << 0) | (1u << 2);

bool wake_and_sleep(const MmioRegion& regs, uint32_t forcewake, uint32_t ack) {
  regs.write32(forcewake, kMaskedEnable);
  const bool awake = regs.wait32(ack, kForcewakeKernelBit, kForcewakeKernelBit, IntelMediaEngineHw::kWakeAttempts);
  regs.write32(forcewake, kMaskedDisable);  // always: a box that never acknowledged must not be left with the request pending
  return awake;
}
}  // namespace

bool IntelMediaEngineHw::supported(IntelGpuArchitecture architecture) {
  switch (architecture) {
    case IntelGpuArchitecture::Gen9_5:
    case IntelGpuArchitecture::Gen11:
    case IntelGpuArchitecture::XeLp:
    case IntelGpuArchitecture::XeHpg:
    case IntelGpuArchitecture::XeLpg:
    case IntelGpuArchitecture::Xe2: return true;
    default: return false;
  }
}

uint64_t IntelMediaEngineHw::register_base(IntelGpuArchitecture architecture) {
  return architecture == IntelGpuArchitecture::XeLpg || architecture == IntelGpuArchitecture::Xe2 ? kMediaTileBase : 0;
}

uint64_t IntelMediaEngineHw::register_window(IntelGpuArchitecture architecture) {
  return supported(architecture) ? register_base(architecture) + kRegisterWindow : 0;
}

IntelMediaEngineStatus IntelMediaEngineHw::probe(const MmioRegion& bar, IntelGpuArchitecture architecture) {
  IntelMediaEngineStatus status;
  if (!supported(architecture) || bar.length() < register_window(architecture)) return status;
  const MmioRegion registers = bar.subregion(register_base(architecture), kRegisterWindow);
  status.probed = true;
  if (architecture == IntelGpuArchitecture::Gen9_5) {
    status.vdbox_present = 1;
    if (wake_and_sleep(registers, kForcewakeMediaGen9, kForcewakeAckMediaGen9)) status.vdbox_awake = 1;
    return status;
  }
  const uint32_t disabled = registers.read32(kVeboxVdboxDisable) & kVdboxDisableMask;
  status.vdbox_present = kVdboxBuilt & ~disabled;
  for (uint32_t box = 0; box < 4; ++box)
    if ((status.vdbox_present & (1u << box)) &&
        wake_and_sleep(registers, kForcewakeMediaVdbox + 4 * box, kForcewakeAckMediaVdbox + 4 * box))
      status.vdbox_awake |= 1u << box;
  return status;
}

}  // namespace hardware
