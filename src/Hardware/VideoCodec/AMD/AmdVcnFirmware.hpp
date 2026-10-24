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
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// The header every AMD GPU microcode file starts with (amdgpu's common_firmware_header; little-endian): the file's own size, where the microcode starts
// in it and how long it is, and a version the driver reads fields out of.
struct AmdFirmwareHeader {
  uint32_t size_bytes = 0;             // the whole file
  uint32_t header_size_bytes = 0;
  uint16_t header_version_major = 0, header_version_minor = 0;
  uint16_t ip_version_major = 0, ip_version_minor = 0;   // the hardware block it is for (VCN 1.0: 1, 0)
  uint32_t ucode_version = 0;
  uint32_t ucode_size_bytes = 0;
  uint32_t ucode_array_offset_bytes = 0;
  uint32_t crc32 = 0;
};
constexpr uint32_t kAmdFirmwareHeaderBytes = 32;

// Checks a firmware image the way the driver does (the size field equals the file's size) and then for what the driver would trust blindly: that the
// header fits, and that the microcode lies inside the file (overflow-safe). Never reads outside `image`.
bool amd_parse_firmware_header(ConstByteView image, AmdFirmwareHeader* out);

// What the VCN firmware's version field says. Firmware from the new naming convention keeps the encoder version, the decoder version and a revision in
// it; older firmware a version number and family ID (amdgpu_vcn.c).
struct AmdVcnFirmwareVersion {
  bool new_naming = false;
  uint8_t enc_major = 0, enc_minor = 0, dec = 0, vep = 0;   // new naming
  uint16_t revision = 0;                                    // new naming
  uint8_t family_id = 0, major = 0, minor = 0;              // old naming
};
AmdVcnFirmwareVersion amd_decode_vcn_firmware_version(uint32_t ucode_version);

// Which firmware file a VCN 1.0 APU (Raven Ridge, Picasso, Raven2/Dali) needs, chosen the way the amdgpu driver does: a PCI revision of 8 or more is Raven2,
// otherwise device 15D8 is Picasso, otherwise (15DD) Raven. nullptr for a device that has no VCN 1.0 (or is not one this knows).
const char* amd_vcn1_firmware_name(uint16_t pci_device_id, uint8_t pci_revision);

enum class AmdFirmwareStatus : uint8_t { Ok, NotApplicable, NotFound, TooLarge, ReadFailed, Invalid, NoMemory };
const char* amd_firmware_status_name(AmdFirmwareStatus status);

// A firmware file read from the OS file system and checked, ready to be put into the GPU's memory. Holds its own copy.
class AmdVcnFirmware {
 public:
  static constexpr const char* kFolder = "/firmware/";
  static constexpr uint64_t kMaxBytes = 4u << 20;   // far above any VCN image (about 360 KiB)

  explicit AmdVcnFirmware(allocators::IAllocator& allocator) : allocator_(allocator), image_(allocator) {}

  // Reads "/firmware/<file_name>". On any failure nothing is kept.
  AmdFirmwareStatus load(filesystem::IFileSystem& file_system, const char* file_name);
  // Takes an image already in memory (the same checks).
  AmdFirmwareStatus adopt(ConstByteView image);
  void clear() { image_.clear(); header_ = AmdFirmwareHeader(); loaded_ = false; }

  bool loaded() const { return loaded_; }
  const AmdFirmwareHeader& header() const { return header_; }
  AmdVcnFirmwareVersion version() const { return amd_decode_vcn_firmware_version(header_.ucode_version); }
  ConstByteView image() const { return image_.view(); }
  // The microcode itself (what goes into the engine's memory), without the header.
  ConstByteView ucode() const { return loaded_ ? image_.view().subview(header_.ucode_array_offset_bytes, header_.ucode_size_bytes) : ConstByteView(); }

 private:
  allocators::IAllocator& allocator_;
  ByteBuffer image_;
  AmdFirmwareHeader header_;
  bool loaded_ = false;
};

}  // namespace hardware
