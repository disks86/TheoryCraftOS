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

#include "Hardware/Chipset/AMD/AmdNodes.hpp"

namespace hardware {

namespace {
constexpr uint16_t kAmd = 0x1022, kHygon = 0x1D94;
constexpr uint8_t kFirstNodeDevice = 0x18;
constexpr uint32_t kTempShift = 21, kTempRangeSelect = 1u << 19, kTempTjSelect = 3u << 16;
constexpr uint32_t kCcdValid = 1u << 11, kCcdMask = 0x7FF;

bool amd_vendor(uint16_t vendor) { return vendor == kAmd || vendor == kHygon; }
bool host_bridge(const PciDeviceInfo& d) { return d.class_code == 0x06 && d.subclass == 0x00; }
}  // namespace

const char* amd_data_fabric_revision_name(AmdDataFabricRevision revision) {
  switch (revision) {
    case AmdDataFabricRevision::Df2: return "DF2";
    case AmdDataFabricRevision::Df3: return "DF3";
    case AmdDataFabricRevision::Df3p5: return "DF3.5";
    case AmdDataFabricRevision::Df4: return "DF4";
    case AmdDataFabricRevision::Df4p5: return "DF4.5";
    case AmdDataFabricRevision::Later: return "DF5 or later";
    case AmdDataFabricRevision::Unknown: break;
  }
  return "unknown Data Fabric";
}

bool amd_ccd_layout(uint32_t family, uint32_t model, uint32_t* offset, uint32_t* count) {
  *offset = *count = 0;
  if (family == 0x17) {
    if (model == 0x01 || model == 0x08 || model == 0x11 || model == 0x18) *offset = 0x154, *count = 4;
    else if (model == 0x31 || model == 0x47 || model == 0x60 || model == 0x68 || model == 0x71) *offset = 0x154, *count = 8;
    else if (model >= 0xA0 && model <= 0xAF) *offset = 0x300, *count = 8;
  } else if (family == 0x19) {
    if (model <= 0x01 || model == 0x08 || model == 0x21 || (model >= 0x50 && model <= 0x5F)) *offset = 0x154, *count = 8;
    else if (model >= 0x40 && model <= 0x4F) *offset = 0x300, *count = 8;
    else if (model >= 0x60 && model <= 0x7F) *offset = 0x308, *count = 8;
    else if ((model >= 0x10 && model <= 0x1F) || (model >= 0xA0 && model <= 0xAF)) *offset = 0x300, *count = 12;
  } else if (family == 0x1A) {
    if (model <= 0x1F) *offset = 0x1F0, *count = 16;
    else if (model >= 0x40 && model <= 0x4F) *offset = 0x308, *count = 8;
  }
  return *count != 0;
}

int32_t amd_tctl_millicelsius(uint32_t value) {
  int32_t t = static_cast<int32_t>(value >> kTempShift) * 125;
  if ((value & kTempRangeSelect) || (value & kTempTjSelect) == kTempTjSelect) t -= 49000;
  return t;
}

int32_t amd_ccd_millicelsius(uint32_t value) { return static_cast<int32_t>(value & kCcdMask) * 125 - 49000; }

bool amd_df_legacy_ficaa(uint32_t family, uint32_t model) {
  if (family < 0x19) return true;
  if (family > 0x19) return false;
  return model <= 0x0F || (model >= 0x20 && model <= 0x5F);
}

void AmdNodes::discover(const IPciBus& pci, uint32_t family, uint32_t model) {
  pci_ = nullptr;
  nodes_ = roots_ = 0;
  roots_per_node_ = 1;
  family_ = family;
  model_ = model;
  if (family < 0x17) return;
  PciDeviceInfo d;
  for (uint32_t i = 0; pci.pci_device(i, &d); ++i) {
    if (!amd_vendor(d.vendor_id) || !host_bridge(d) || d.address.function != 0) continue;
    if (d.address.bus == 0 && d.address.device >= kFirstNodeDevice && d.address.device < kFirstNodeDevice + kMaxNodes) {
      if (nodes_ < kMaxNodes) node_[nodes_++] = d.address;
    } else if (d.address.device == 0 && roots_ < kMaxRoots) {
      root_[roots_++] = d.address;   // a root complex: device 0 function 0 of its bus
    }
  }
  if (nodes_ == 0 || roots_ == 0) {
    nodes_ = roots_ = 0;
    return;
  }
  // A node may have several roots (EPYC has four per socket); the first of each group carries the node's SMN pair.
  roots_per_node_ = roots_ / nodes_ ? roots_ / nodes_ : 1;
  if (nodes_ * roots_per_node_ > roots_) nodes_ = roots_ / roots_per_node_;
  pci_ = &pci;
}

PciAddress AmdNodes::root_of(uint32_t node) const {
  if (node >= nodes_) return PciAddress{};
  return root_[node * roots_per_node_];
}

bool AmdNodes::smn_read(uint32_t node, uint32_t address, uint32_t* value) const {
  *value = 0;
  if (!pci_ || node >= nodes_) return false;
  const PciAddress root = root_of(node);
  pci_->write32(root, kSmnIndex, address);
  const uint32_t v = pci_->read32(root, kSmnData);
  if (v == 0xFFFFFFFFu) return false;
  *value = v;
  return true;
}

bool AmdNodes::smn_write(uint32_t node, uint32_t address, uint32_t value) const {
  if (!pci_ || node >= nodes_) return false;
  const PciAddress root = root_of(node);
  pci_->write32(root, kSmnIndex, address);
  pci_->write32(root, kSmnData, value);
  return true;
}

bool AmdNodes::df_config(uint32_t node, uint8_t function, uint16_t reg, uint32_t* value) const {
  *value = 0;
  if (!pci_ || node >= nodes_ || function > 7 || reg > 0xFFC || (reg & 3)) return false;
  const PciAddress f{node_[node].bus, node_[node].device, function};
  const uint32_t v = pci_->read32(f, reg);
  if (v == 0xFFFFFFFFu) return false;
  *value = v;
  return true;
}

bool AmdNodes::df_read(uint32_t node, uint8_t function, uint16_t reg, uint32_t* value) const {
  *value = 0;
  if (!pci_ || node >= nodes_ || function > 7 || reg > 0xFFC || (reg & 3)) return false;
  const bool legacy = amd_df_legacy_ficaa(family_, model_);
  const uint16_t ficaa = legacy ? 0x5C : 0x8C, ficad = legacy ? 0x98 : 0xB8;
  const uint32_t reg_field = legacy ? (static_cast<uint32_t>(reg >> 2) << 2) & 0x7FC : (static_cast<uint32_t>(reg >> 2) << 1) & 0x7FE;
  const uint32_t request = reg_field | (static_cast<uint32_t>(function) << 11);   // instance enable clear: broadcast
  const PciAddress f4{node_[node].bus, node_[node].device, 4};
  pci_->write32(f4, ficaa, request);
  const uint32_t v = pci_->read32(f4, ficad);
  if (v == 0xFFFFFFFFu) return false;
  *value = v;
  return true;
}

AmdDataFabricInfo AmdNodes::data_fabric() const {
  AmdDataFabricInfo info;
  uint32_t instance_count = 0;
  if (!df_read(0, 0, 0x40, &instance_count)) return info;
  info.block_instances = static_cast<uint8_t>(instance_count & 0xFF);
  info.major = static_cast<uint8_t>((instance_count >> 24) & 0xF);
  info.minor = static_cast<uint8_t>((instance_count >> 16) & 0xFF);
  if (info.major == 0) {
    // Before DF4 the revision shows in which System Fabric ID mask register is implemented (atl/system.c).
    info.minor = 0;
    uint32_t mask = 0;
    if (df_read(0, 1, 0x150, &mask) && (mask & 0xFFFF)) info.revision = AmdDataFabricRevision::Df3p5;
    else if (df_read(0, 1, 0x208, &mask) && (mask & 0x3FF)) info.revision = AmdDataFabricRevision::Df3;
    else info.revision = AmdDataFabricRevision::Df2;
  } else if (info.major == 4) {
    info.revision = info.minor < 5 ? AmdDataFabricRevision::Df4 : AmdDataFabricRevision::Df4p5;
  } else {
    info.revision = AmdDataFabricRevision::Later;
  }
  // DRAM hole control: D18F0x104 before DF4, D18F7x104 from DF4 on. Bit 0 valid, bits 31:24 the base.
  const uint8_t hole_function = info.revision >= AmdDataFabricRevision::Df4 ? 7 : 0;
  uint32_t hole = 0;
  if (info.revision != AmdDataFabricRevision::Later && df_read(0, hole_function, 0x104, &hole) && (hole & 1)) {
    info.dram_hole = true;
    info.dram_hole_base = hole & 0xFF000000u;
  }
  return info;
}

AmdTemperatures AmdNodes::temperatures(uint32_t node) const {
  AmdTemperatures t;
  uint32_t value = 0;
  if (smn_read(node, kThermalBase, &value)) {
    t.tctl_valid = true;
    t.tctl_mc = amd_tctl_millicelsius(value);
  }
  uint32_t offset = 0, count = 0;
  if (!amd_ccd_layout(family_, model_, &offset, &count)) return t;
  for (uint32_t ccd = 0; ccd < count && t.ccd_count < AmdTemperatures::kMaxCcds; ++ccd) {
    if (!smn_read(node, kThermalBase + offset + ccd * 4, &value) || !(value & kCcdValid)) continue;
    t.ccd_index[t.ccd_count] = static_cast<uint8_t>(ccd);
    t.ccd_mc[t.ccd_count] = amd_ccd_millicelsius(value);
    ++t.ccd_count;
  }
  return t;
}

}  // namespace hardware
