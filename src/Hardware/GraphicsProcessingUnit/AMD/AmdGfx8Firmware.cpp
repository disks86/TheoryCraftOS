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


// The header layouts follow the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): amdgpu_ucode.h.
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Firmware.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
// struct common_firmware_header: size_bytes, header_size_bytes, header_version_major/minor, ip_version_major/minor (16 bits each), ucode_version, ucode_size_bytes,
// ucode_array_offset_bytes, crc32.
constexpr uint32_t kCommonHeaderBytes = 36;   // the common header and the feature version word that follows it in every kind of file but the RLC's
// struct gfx_firmware_header_v1_0 adds ucode_feature_version, jt_offset, jt_size.
constexpr uint32_t kGfxHeaderBytes = 44;
constexpr uint32_t kMaxFirmwareBytes = 4u << 20;   // far above the largest file (the MEC, 257 KiB); a bigger size field is damage

bool is_cp_block(AmdGfx8Block block) {
  return block == AmdGfx8Block::CpPfp || block == AmdGfx8Block::CpMe || block == AmdGfx8Block::CpCe || block == AmdGfx8Block::CpMec || block == AmdGfx8Block::CpMec2;
}
}  // namespace

const char* amd_gfx8_firmware_name(uint16_t pci_device_id, AmdGfx8Block block) {
  if (pci_device_id != 0x694C && pci_device_id != 0x694E && pci_device_id != 0x694F) return nullptr;
  switch (block) {
    case AmdGfx8Block::CpPfp: return "vegam_pfp.bin";
    case AmdGfx8Block::CpMe: return "vegam_me.bin";
    case AmdGfx8Block::CpCe: return "vegam_ce.bin";
    case AmdGfx8Block::CpMec: return "vegam_mec.bin";
    case AmdGfx8Block::CpMec2: return "vegam_mec2.bin";
    case AmdGfx8Block::RlcG: return "vegam_rlc.bin";
    case AmdGfx8Block::Sdma0: return "vegam_sdma.bin";
    case AmdGfx8Block::Sdma1: return "vegam_sdma1.bin";
  }
  return nullptr;
}

bool amd_gfx8_parse_firmware(ConstByteView file, AmdGfx8Block block, AmdGfx8Image* image) {
  ByteReader in(file);
  const uint32_t size_bytes = in.le32();
  const uint32_t header_size = in.le32();
  const uint32_t header_version = in.le32();   // major in the low half
  in.le32();                                   // ip_version_major/minor: the file's name already says which chip it is for
  const uint32_t ucode_version = in.le32();
  const uint32_t ucode_size = in.le32();
  const uint32_t array_offset = in.le32();
  in.le32();                                   // crc32
  if (!in.ok()) return false;
  // amdgpu_ucode_validate: the header says how big the file is, and the file is that big.
  if (size_bytes > file.size() || size_bytes > kMaxFirmwareBytes) return false;
  const uint32_t major = header_version & 0xFFFF;
  if (major != (block == AmdGfx8Block::RlcG ? 2u : 1u)) return false;
  if (header_size < (is_cp_block(block) ? kGfxHeaderBytes : kCommonHeaderBytes) || header_size > size_bytes) return false;
  // The ucode array: whole dwords, inside the file, after the header.
  if (ucode_size == 0 || (ucode_size & 3) != 0 || array_offset < header_size || (array_offset & 3) != 0) return false;
  if (array_offset > size_bytes || ucode_size > size_bytes - array_offset) return false;

  AmdGfx8Image result;
  result.version = ucode_version;
  if (block != AmdGfx8Block::RlcG) {
    in.seek(32);
    result.feature_version = in.le32();
    if (!in.ok()) return false;
  }
  result.ucode = file.subview(array_offset, ucode_size);
  if (block == AmdGfx8Block::CpMec || block == AmdGfx8Block::CpMec2) {
    in.seek(32);
    in.le32();   // ucode_feature_version
    const uint32_t jt_offset = in.le32();
    const uint32_t jt_size = in.le32();
    if (!in.ok()) return false;
    const uint32_t dwords = ucode_size / 4;
    if (jt_size == 0 || jt_offset > dwords || jt_size > dwords - jt_offset) return false;
    result.jump_table_offset_bytes = jt_offset * 4;
    result.jump_table = result.ucode.subview(uint64_t{jt_offset} * 4, uint64_t{jt_size} * 4);
  }
  result.valid = true;
  *image = result;
  return true;
}

}  // namespace hardware
