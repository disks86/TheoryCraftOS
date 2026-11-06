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

// The internal registers of an AMD Zen processor, as Linux reaches them (arch/x86/kernel/amd_node.c, drivers/ras/amd/atl, drivers/hwmon/k10temp.c).
//
// A node is one Data Fabric instance (a die with its own memory and I/O; one per socket on client parts), seen as PCI devices 00:18+n, functions 0..7.
// The System Management Network (SMN) is the address space of the SoC's management blocks (thermal, SMU, PCIe, USB...); each node's root complex
// (a host bridge at device 0 function 0 of its bus) has an index/data pair at config 0x60/0x64 to reach it. Data Fabric registers of other
// functions or instances are read indirectly through function 4: FICAA (0x5C, or 0x8C from Zen 4 on) takes the function and register, FICAD
// (0x98 / 0xB8) gives the value.
//
// Not thread-safe: an index/data pair must not be interleaved, so SystemChipset keeps this in a Synchronized and every user goes through it.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"

namespace hardware {

enum class AmdDataFabricRevision : uint8_t { Unknown, Df2, Df3, Df3p5, Df4, Df4p5, Later };
const char* amd_data_fabric_revision_name(AmdDataFabricRevision revision);

struct AmdDataFabricInfo {
  AmdDataFabricRevision revision = AmdDataFabricRevision::Unknown;
  uint8_t major = 0, minor = 0;     // DF4 and later report these; 0 before
  uint8_t block_instances = 0;      // fabric blocks (CCMs, CSs, IOMs...) behind node 0
  bool dram_hole = false;           // the firmware put an MMIO hole below 4 GiB
  uint32_t dram_hole_base = 0;      // where it starts (DRAM above it is remapped past 4 GiB)
};

// The processor's temperatures as k10temp reads them: Tctl (the control temperature, which on some parts carries an offset) and each core complex die.
struct AmdTemperatures {
  static constexpr uint32_t kMaxCcds = 16;
  bool tctl_valid = false;
  int32_t tctl_mc = 0;
  uint32_t ccd_count = 0;           // how many CCD readings follow (only the dies whose sensor reads valid)
  uint8_t ccd_index[kMaxCcds] = {};
  int32_t ccd_mc[kMaxCcds] = {};
};

// Where a family/model keeps its CCD temperature registers (an offset from SMN 0x59800) and how many to look at; false when k10temp knows none.
bool amd_ccd_layout(uint32_t family, uint32_t model, uint32_t* offset, uint32_t* count);
// The Tctl register (SMN 0x59800) and a CCD register as millidegrees Celsius.
int32_t amd_tctl_millicelsius(uint32_t value);
int32_t amd_ccd_millicelsius(uint32_t value);
// Families before Zen 4 (and Zen 3's 19h models 00-0F and 20-5F) use the old FICAA/FICAD offsets.
bool amd_df_legacy_ficaa(uint32_t family, uint32_t model);

class AmdNodes {
 public:
  static constexpr uint32_t kMaxNodes = 8, kMaxRoots = 32;
  static constexpr uint32_t kSmnIndex = 0x60, kSmnData = 0x64;
  static constexpr uint32_t kThermalBase = 0x59800;

  // Finds the nodes and their root complexes. `family`/`model` select the register layouts; a processor before Zen (family < 0x17) has no SMN.
  void discover(const IPciBus& pci, uint32_t family, uint32_t model);
  uint32_t node_count() const { return nodes_; }
  uint32_t root_count() const { return roots_; }
  PciAddress root_of(uint32_t node) const;

  // SMN access through the node's root complex. False for an unknown node, or a read that comes back all ones (nothing answered).
  bool smn_read(uint32_t node, uint32_t address, uint32_t* value) const;
  bool smn_write(uint32_t node, uint32_t address, uint32_t value) const;

  // A Data Fabric register of the node's own function `function` (direct configuration access).
  bool df_config(uint32_t node, uint8_t function, uint16_t reg, uint32_t* value) const;
  // A Data Fabric register through FICAA/FICAD, as a broadcast read (the value all instances agree on).
  bool df_read(uint32_t node, uint8_t function, uint16_t reg, uint32_t* value) const;
  AmdDataFabricInfo data_fabric() const;

  AmdTemperatures temperatures(uint32_t node) const;

 private:
  const IPciBus* pci_ = nullptr;
  uint32_t family_ = 0, model_ = 0;
  uint32_t nodes_ = 0, roots_ = 0, roots_per_node_ = 1;
  PciAddress node_[kMaxNodes] = {};
  PciAddress root_[kMaxRoots] = {};
};

}  // namespace hardware
