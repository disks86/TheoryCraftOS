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

// The microcode files of the graphics engine (GFX 8), the compute engine and the DMA engines of a Radeon RX Vega M (amdgpu's VEGAM), and how to take one apart: the common
// firmware header of amdgpu_ucode.h (common_firmware_header) and the pieces the three kinds add (gfx_firmware_header_v1_0 for the command processor's PFP, ME, CE and MEC,
// rlc_firmware_header_v2_0 for the run list controller, sdma_firmware_header_v1_0 for the DMA engines). All fields are little-endian.
//
// Nothing here trusts the file: every size is checked against the file's own length and the arithmetic cannot wrap, so a truncated or damaged file is refused, not
// followed out of its buffer.
enum class AmdGfx8Block : uint8_t { CpPfp, CpMe, CpCe, CpMec, CpMec2, RlcG, Sdma0, Sdma1 };

// The file amdgpu_ucode_request is asked for, as linux-firmware names it (gfx_v8_0_init_microcode: "amdgpu/%s_pfp.bin" ... with chip_name "vegam"; sdma_v3_0_init_microcode:
// "amdgpu/%s_sdma.bin" and "_sdma1.bin"). nullptr for a device that is not a Vega M (PCI 1002:694C, 694E, 694F).
const char* amd_gfx8_firmware_name(uint16_t pci_device_id, AmdGfx8Block block);

// What a parsed file holds. The views point into the file's memory, which must outlive them.
struct AmdGfx8Image {
  bool valid = false;
  uint32_t version = 0;               // common header: ucode_version
  uint32_t feature_version = 0;       // ucode_feature_version (the word after the common header; not for the RLC file, which has others there)
  ConstByteView ucode;                // ucode_size_bytes at ucode_array_offset_bytes
  // The command processor's compute microcode (MEC) ends in a jump table the SMU is told about separately (cgs_get_firmware_info): jt_size dwords at dword jt_offset of
  // the ucode array. Empty for every other file (the PFP, ME and CE headers carry the same two fields, but the driver only uses them for the MEC).
  ConstByteView jump_table;
  uint32_t jump_table_offset_bytes = 0;
};

// The file's kind is that of its block. False (and `image` untouched) for anything amdgpu_ucode_validate or the code that reads the header would refuse, and for what the
// reader here cannot follow: sizes that do not fit the file, an unknown header version, a ucode array that is not whole dwords, a jump table outside the ucode array.
bool amd_gfx8_parse_firmware(ConstByteView file, AmdGfx8Block block, AmdGfx8Image* image);

}  // namespace hardware
