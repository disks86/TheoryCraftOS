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
#include "Utility/ArrayView.hpp"

namespace hardware {

// What an AMD GPU says about itself in its IP discovery table: the list of hardware blocks (graphics, video, DMA...) on the chip with their
// versions and instance counts, which the driver reads instead of guessing from the PCI ID. Present on Navi 10 (RDNA 1) and newer.
struct AmdIpVersion {
  uint8_t major = 0, minor = 0, revision = 0;
};

// One instance of a block as the table lists it: which instance it is and where its registers start. The base addresses are the "segments" the driver adds
// a register's offset to (a register's address is base[segment] + offset, in dwords); how many there are and what each is for depends on the block's version.
constexpr uint32_t kAmdMaxIpBases = 8;
struct AmdIpInstance {
  bool present = false;
  uint8_t instance = 0;                 // the table's instance number (VCN 0, VCN 1)
  uint8_t base_count = 0;               // base addresses stored (at most kAmdMaxIpBases; the table may list more)
  uint64_t base[kAmdMaxIpBases] = {};
};

constexpr uint32_t kAmdMaxVcnInstances = 4;

struct AmdIpDiscoveryInfo {
  bool valid = false;            // a table with the right signatures and a layout that holds together was found
  bool checksum_ok = false;      // the table's own checksum matches (a mismatch is reported, not fatal: the layout checks above still apply)
  uint16_t binary_version = 0;   // major << 8 | minor of the table container
  uint16_t table_version = 0;    // version of the IP list inside it
  uint32_t ip_count = 0;         // IP blocks listed, all dies
  uint32_t gc_instances = 0;     // graphics and compute
  AmdIpVersion gc;
  uint32_t vcn_instances = 0;    // video core next (decode/encode) engines
  AmdIpVersion vcn;              // version of the first instance
  AmdIpInstance vcn_engine[kAmdMaxVcnInstances];   // the first kAmdMaxVcnInstances of them, in table order: where each one's registers are
  uint32_t sdma_instances = 0;   // DMA engines
};

// The hardware IDs of the blocks the probe counts (as the amdgpu driver names them).
constexpr uint16_t kAmdHwIdGc = 11;
constexpr uint16_t kAmdHwIdVcn = 12;   // UVD and VCN share the ID
constexpr uint16_t kAmdHwIdSdma0 = 42;
constexpr uint16_t kAmdHwIdSdma1 = 43;
constexpr uint16_t kAmdHwIdSdma2 = 68;
constexpr uint16_t kAmdHwIdSdma3 = 69;

// Parses an IP discovery table (the bytes at the end of the GPU's video memory). Every field is bounds-checked against `table`; a truncated,
// corrupted or foreign table gives valid == false and never reads outside it. Never allocates.
AmdIpDiscoveryInfo amd_parse_ip_discovery(ConstByteView table);

// Reads the size the table declares for itself from its first bytes (so that no more than that need be fetched), or 0 if the signature is wrong.
uint32_t amd_ip_discovery_declared_size(ConstByteView header);
constexpr uint32_t kAmdIpDiscoveryHeaderBytes = 16;

}  // namespace hardware
