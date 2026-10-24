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

#include "Hardware/VideoCodec/AMD/AmdVcnFirmware.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

bool amd_parse_firmware_header(ConstByteView image, AmdFirmwareHeader* out) {
  if (image.size() < kAmdFirmwareHeaderBytes || image.size() > UINT32_MAX) return false;
  ByteReader in(image);
  AmdFirmwareHeader h;
  h.size_bytes = in.le32();
  h.header_size_bytes = in.le32();
  h.header_version_major = in.le16();
  h.header_version_minor = in.le16();
  h.ip_version_major = in.le16();
  h.ip_version_minor = in.le16();
  h.ucode_version = in.le32();
  h.ucode_size_bytes = in.le32();
  h.ucode_array_offset_bytes = in.le32();
  h.crc32 = in.le32();
  if (!in.ok() || h.size_bytes != image.size()) return false;                    // the driver's own check
  if (h.header_size_bytes < kAmdFirmwareHeaderBytes || h.header_size_bytes > h.size_bytes) return false;
  if (h.ucode_size_bytes == 0 || h.ucode_array_offset_bytes < h.header_size_bytes) return false;   // the microcode does not overlap the header
  if (h.ucode_array_offset_bytes > h.size_bytes || h.ucode_size_bytes > h.size_bytes - h.ucode_array_offset_bytes) return false;
  if (out) *out = h;
  return true;
}

AmdVcnFirmwareVersion amd_decode_vcn_firmware_version(uint32_t v) {
  AmdVcnFirmwareVersion r;
  const uint32_t check = (v >> 20) & 0xF;   // non-zero only with the new naming convention
  if (check) {
    r.new_naming = true;
    r.enc_major = static_cast<uint8_t>(check);
    r.enc_minor = static_cast<uint8_t>((v >> 12) & 0xFF);
    r.dec = static_cast<uint8_t>((v >> 24) & 0xF);
    r.vep = static_cast<uint8_t>((v >> 28) & 0xF);
    r.revision = static_cast<uint16_t>(v & 0xFFF);
  } else {
    r.family_id = static_cast<uint8_t>(v & 0xFF);
    r.major = static_cast<uint8_t>((v >> 24) & 0xFF);
    r.minor = static_cast<uint8_t>((v >> 8) & 0xFF);
  }
  return r;
}

const char* amd_vcn1_firmware_name(uint16_t device_id, uint8_t revision) {
  if (device_id != 0x15DD && device_id != 0x15D8) return nullptr;   // Raven Ridge, Picasso
  if (revision >= 0x8) return "raven2_vcn.bin";
  if (device_id == 0x15D8) return "picasso_vcn.bin";
  return "raven_vcn.bin";
}

const char* amd_firmware_status_name(AmdFirmwareStatus status) {
  switch (status) {
    case AmdFirmwareStatus::Ok: return "loaded";
    case AmdFirmwareStatus::NotApplicable: return "none needed";
    case AmdFirmwareStatus::NotFound: return "not found";
    case AmdFirmwareStatus::TooLarge: return "too large";
    case AmdFirmwareStatus::ReadFailed: return "unreadable";
    case AmdFirmwareStatus::Invalid: return "not a valid firmware image";
    case AmdFirmwareStatus::NoMemory: return "out of memory";
  }
  return "unknown";
}

AmdFirmwareStatus AmdVcnFirmware::adopt(ConstByteView image) {
  clear();
  AmdFirmwareHeader header;
  if (!amd_parse_firmware_header(image, &header)) return AmdFirmwareStatus::Invalid;
  if (!image_.append(image)) return AmdFirmwareStatus::NoMemory;
  header_ = header;
  loaded_ = true;
  return AmdFirmwareStatus::Ok;
}

AmdFirmwareStatus AmdVcnFirmware::load(filesystem::IFileSystem& file_system, const char* file_name) {
  clear();
  if (!file_name || !file_name[0]) return AmdFirmwareStatus::NotApplicable;
  // "/firmware/" + name, built in a buffer of its own (a name with a slash in it would leave the folder: refused).
  ByteBuffer path(allocator_);
  for (const char* c = file_name; *c; ++c)
    if (*c == '/' || *c == '\\') return AmdFirmwareStatus::NotFound;
  if (!path.append(ConstByteView(reinterpret_cast<const uint8_t*>(kFolder), StringView(kFolder).size())) ||
      !path.append(ConstByteView(reinterpret_cast<const uint8_t*>(file_name), StringView(file_name).size())))
    return AmdFirmwareStatus::NoMemory;
  UniquePointer<filesystem::IFile> file;
  const filesystem::Status opened = file_system.open_file(StringView(reinterpret_cast<const char*>(path.view().data()), path.size()), filesystem::OpenMode::Read, &file);
  if (opened == filesystem::Status::NotFound) return AmdFirmwareStatus::NotFound;
  if (opened != filesystem::Status::Ok || !file) return AmdFirmwareStatus::ReadFailed;
  const uint64_t size = file->size();
  if (size > kMaxBytes) return AmdFirmwareStatus::TooLarge;
  if (size < kAmdFirmwareHeaderBytes) return AmdFirmwareStatus::Invalid;
  if (!image_.put_zeros(size)) return AmdFirmwareStatus::NoMemory;
  uint64_t got = 0;
  if (file->read(0, image_.writable_view(), &got) != filesystem::Status::Ok || got != size) {
    image_.clear();
    return AmdFirmwareStatus::ReadFailed;
  }
  AmdFirmwareHeader header;
  if (!amd_parse_firmware_header(image_.view(), &header)) {
    image_.clear();
    return AmdFirmwareStatus::Invalid;
  }
  header_ = header;
  loaded_ = true;
  return AmdFirmwareStatus::Ok;
}

}  // namespace hardware
