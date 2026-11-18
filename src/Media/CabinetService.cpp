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
#include "Media/CabinetService.hpp"
#include "Media/Cabinet.hpp"
#include "Process/ProcessFiles.hpp"
#include "Process/WindowsCabinet.hpp"
#include "Utility/ByteStream.hpp"

namespace media {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidParameter = 0xC000000Du, kNoMemory = 0xC0000017u, kBufferTooSmall = 0xC0000023u,
                   kFileCorrupt = 0xC0000102u;
constexpr uint32_t kWords = 8;
enum : uint64_t { kDescribe = 1, kExtract = 2 };

uint32_t to_nt(CabinetServiceStatus s) {
  switch (s) {
    case CabinetServiceStatus::Ok: return kSuccess;
    case CabinetServiceStatus::InvalidParameter: return kInvalidParameter;
    case CabinetServiceStatus::BufferTooSmall: return kBufferTooSmall;
    case CabinetServiceStatus::NoMemory: return kNoMemory;
    case CabinetServiceStatus::BadData: return kFileCorrupt;
  }
  return kInvalidParameter;
}

// Writes word `index` of the service's arguments back to the program. (The words were read from there: the address cannot wrap.)
bool put_word(process::Process& p, uint64_t arguments, uint32_t index, uint64_t value) {
  uint8_t raw[8];
  ByteWriter writer{ByteView(raw, sizeof raw)};
  writer.le64(value);
  return p.write_user(arguments + uint64_t{index} * 8, ConstByteView(raw, sizeof raw));
}

}  // namespace

CabinetServiceStatus describe_cabinet(allocators::IAllocator& allocator, ConstByteView cabinet, uint64_t capacity, ByteBuffer* records, uint64_t* files) {
  *files = 0;
  if (cabinet.size() > kCabinetServiceMaxCabinet) return CabinetServiceStatus::InvalidParameter;
  MemoryZipSource source(cabinet);
  CabinetReader reader(allocator, source);
  if (!reader.open()) return CabinetServiceStatus::BadData;
  const ArrayView<const CabinetEntry> entries = reader.entries();
  *files = entries.size();
  if (entries.size() > capacity / kCabinetRecordSize) return CabinetServiceStatus::BufferTooSmall;      // (at most 100000 entries: the product below cannot wrap)
  const uint64_t start = records->size();
  if (!records->put_zeros(entries.size() * kCabinetRecordSize)) return CabinetServiceStatus::NoMemory;
  ByteWriter writer(records->writable_view().subview(start));
  for (const CabinetEntry& entry : entries) {
    const ConstByteView name = entry.name.view().bytes();
    const uint64_t kept = name.size() < kCabinetRecordName ? name.size() : kCabinetRecordName;
    writer.le64(entry.size);
    writer.le32(uint32_t(kept));
    writer.le16(entry.date);
    writer.le16(entry.time);
    writer.le16(entry.attributes);
    writer.le16(entry.folder);
    writer.le32((entry.name_is_utf8() ? kCabinetRecordUtf8 : 0) | (kept < name.size() ? kCabinetRecordNameCut : 0));
    writer.bytes(name.first(kept));
    writer.zeros(kCabinetRecordName - kept);
  }
  if (!writer.ok()) {      // (cannot happen: the room was made above)
    records->truncate(start);
    return CabinetServiceStatus::NoMemory;
  }
  return CabinetServiceStatus::Ok;
}

CabinetServiceStatus extract_cabinet_file(allocators::IAllocator& allocator, ConstByteView cabinet, uint64_t index, uint64_t capacity, ByteBuffer* out,
                                          uint64_t* size) {
  *size = 0;
  if (cabinet.size() > kCabinetServiceMaxCabinet) return CabinetServiceStatus::InvalidParameter;
  MemoryZipSource source(cabinet);
  CabinetReader reader(allocator, source);
  if (!reader.open()) return CabinetServiceStatus::BadData;
  const CabinetEntry* entry = reader.entries().try_at(index);
  if (entry == nullptr) return CabinetServiceStatus::InvalidParameter;
  *size = entry->size;
  if (entry->size > capacity) return CabinetServiceStatus::BufferTooSmall;
  if (entry->size > kCabinetServiceMaxFile) return CabinetServiceStatus::InvalidParameter;
  // (The size is the cabinet's word, not yet backed by data: extract() grows the buffer block by block as the data really decodes.)
  return reader.extract(*entry, kCabinetServiceMaxFile, out) ? CabinetServiceStatus::Ok : CabinetServiceStatus::BadData;
}

uint32_t windows_cabinet_service(process::Process& p, const process::SyscallRequest& r) {
  const uint64_t arguments = r.args[0];
  uint8_t raw[kWords * 8];
  if (!p.read_user(arguments, ByteView(raw, sizeof raw))) return kAccessViolation;
  uint64_t w[kWords];
  ByteReader words{ConstByteView(raw, sizeof raw)};
  for (uint64_t& word : w) word = words.le64();

  if (w[0] != kDescribe && w[0] != kExtract) return kInvalidParameter;
  // The cabinet, copied: the program may change its memory while the reader works.
  if (w[2] == 0 || w[2] > kCabinetServiceMaxCabinet) return kInvalidParameter;
  allocators::IAllocator& allocator = p.files().allocator();
  ByteBuffer cabinet(allocator);
  if (!cabinet.put_zeros(w[2])) return kNoMemory;
  if (!p.read_user(w[1], cabinet.writable_view())) return kAccessViolation;

  ByteBuffer result(allocator);
  if (w[0] == kDescribe) {
    uint64_t files = 0;
    const CabinetServiceStatus status = describe_cabinet(allocator, cabinet.view(), w[4], &result, &files);
    if (status != CabinetServiceStatus::Ok && status != CabinetServiceStatus::BufferTooSmall) return to_nt(status);
    if (!put_word(p, arguments, 5, files) || !put_word(p, arguments, 6, kCabinetRecordSize)) return kAccessViolation;
    if (status != CabinetServiceStatus::Ok) return to_nt(status);
    return result.size() == 0 || p.write_user(w[3], result.view()) ? kSuccess : kAccessViolation;
  }
  uint64_t size = 0;
  const CabinetServiceStatus status = extract_cabinet_file(allocator, cabinet.view(), w[3], w[5], &result, &size);
  if (status != CabinetServiceStatus::Ok && status != CabinetServiceStatus::BufferTooSmall) return to_nt(status);
  if (!put_word(p, arguments, 6, size)) return kAccessViolation;
  if (status != CabinetServiceStatus::Ok) return to_nt(status);
  return result.size() == 0 || p.write_user(w[4], result.view()) ? kSuccess : kAccessViolation;
}

void install_windows_cabinet() { process::windowsabi::set_cabinet_handler(&windows_cabinet_service); }

}  // namespace media
