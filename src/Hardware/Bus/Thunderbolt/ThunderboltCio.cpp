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

#include "Hardware/Bus/Thunderbolt/ThunderboltCio.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
constexpr uint32_t kStepUs = 1000;

// (nhi.h: PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_*_BRIDGE)
bool is_alpine_ridge_bridge(uint16_t device) {
  switch (device) {
    case 0x1576: case 0x1578: case 0x15C0: case 0x15D3: case 0x15DA: return true;
    default: return false;
  }
}
}  // namespace

bool TbPcie2Cio::wait(uint32_t timeout_us) {
  for (uint32_t waited = 0;; waited += kStepUs) {
    const uint32_t cmd = config_.read32(capability_ + tb_cio::kCmd);
    if (!(cmd & tb_cio::kCmdStart)) return !(cmd & tb_cio::kCmdTimeout);
    if (waited >= timeout_us) return false;
    delay_us(kStepUs);
  }
}

bool TbPcie2Cio::read(TbConfigSpace space, uint32_t port, uint32_t index, uint32_t* value, uint32_t timeout_us) {
  if (capability_ == 0 || port > 0x3F || index > tb_cio::kCmdIndexMask) return false;
  const uint32_t cmd = index | ((port << tb_cio::kCmdPortShift) & tb_cio::kCmdPortMask) | ((static_cast<uint32_t>(space) << tb_cio::kCmdSpaceShift) & tb_cio::kCmdSpaceMask) | tb_cio::kCmdStart;
  config_.write32(capability_ + tb_cio::kCmd, cmd);
  if (!wait(timeout_us)) return false;
  *value = config_.read32(capability_ + tb_cio::kReadData);
  return true;
}

bool TbPcie2Cio::write(TbConfigSpace space, uint32_t port, uint32_t index, uint32_t value, uint32_t timeout_us) {
  if (capability_ == 0 || port > 0x3F || index > tb_cio::kCmdIndexMask) return false;
  config_.write32(capability_ + tb_cio::kWriteData, value);
  const uint32_t cmd = index | ((port << tb_cio::kCmdPortShift) & tb_cio::kCmdPortMask) | ((static_cast<uint32_t>(space) << tb_cio::kCmdSpaceShift) & tb_cio::kCmdSpaceMask) | tb_cio::kCmdWrite |
                       tb_cio::kCmdStart;
  config_.write32(capability_ + tb_cio::kCmd, cmd);
  return wait(timeout_us);
}

uint16_t tb_find_vendor_capability(ITbConfigSpace& config) {
  uint32_t offset = 0x100;
  for (int guard = 0; guard < 64 && offset >= 0x100 && offset < 0x1000; ++guard) {
    const uint32_t header = config.read32(static_cast<uint16_t>(offset));
    if (header == 0 || header == 0xFFFFFFFFu) return 0;
    if ((header & 0xFFFF) == tb_cio::kVendorExtendedCapability) return static_cast<uint16_t>(offset);
    offset = (header >> 20) & 0xFFC;
  }
  return 0;
}

bool tb_find_upstream_port(const IPciBus& bus, const PciDeviceInfo& nhi, PciAddress* port) {
  bool found = false;
  uint32_t best_span = 0;
  for (uint32_t i = 0; i < bus.device_count(); ++i) {
    PciDeviceInfo d;
    if (!bus.pci_device(i, &d) || d.vendor_id != 0x8086 || !is_alpine_ridge_bridge(d.device_id)) continue;
    // a PCI-to-PCI bridge (header type 1) whose secondary..subordinate bus numbers hold the NHI's bus
    if (((bus.read32(d.address, 0x0C) >> 16) & 0x7F) != 1) continue;
    const uint32_t buses = bus.read32(d.address, 0x18);
    const uint32_t secondary = (buses >> 8) & 0xFF, subordinate = (buses >> 16) & 0xFF;
    if (secondary == 0 || nhi.address.bus < secondary || nhi.address.bus > subordinate) continue;
    // a PCI Express upstream port (device/port type 5 in the PCIe capability's flags)
    const uint16_t cap = bus.find_capability(d.address, 0x10);
    if (cap == 0 || ((bus.read32(d.address, cap) >> 20) & 0xF) != 5) continue;
    const uint32_t span = subordinate - secondary;
    if (!found || span < best_span) {
      found = true;
      best_span = span;
      *port = d.address;
    }
  }
  return found;
}

}  // namespace hardware
