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
#include "Executable/PeFile.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/ByteText.hpp"

namespace exe {

namespace {

bool file_range(ConstByteView file, uint64_t offset, uint64_t size, ConstByteView* out) {
  if (offset > file.size() || size > file.size() - offset) return false;
  *out = file.subview(offset, size);
  return true;
}

// The NUL-terminated string at `offset` of the file, appended to `out`; false if the NUL is missing.
bool read_c_string(ConstByteView file, uint64_t offset, String* out) {
  if (offset >= file.size()) return false;
  ConstByteView rest = file.subview(offset);
  uint64_t length = 0;
  while (length < rest.size() && rest[length] != 0) ++length;
  if (length == rest.size()) return false;
  return out->append(bytes_as_text(rest.first(length)));
}

}  // namespace

bool PeFile::looks_like_pe(ConstByteView data) {
  if (data.size() < 0x40 || data[0] != 'M' || data[1] != 'Z') return false;
  ByteReader r(data);
  r.seek(0x3C);
  const uint32_t lfanew = r.le32();
  ConstByteView signature;
  return r.ok() && file_range(data, lfanew, 4, &signature) && signature[0] == 'P' && signature[1] == 'E' && signature[2] == 0 && signature[3] == 0;
}

void PeFile::clear() {
  plus_ = false;
  machine_ = characteristics_ = subsystem_ = dll_characteristics_ = 0;
  size_of_code_ = timestamp_ = entry_rva_ = section_alignment_ = file_alignment_ = size_of_image_ = size_of_headers_ = checksum_ = 0;
  image_base_ = 0;
  directory_count_ = 0;
  for (uint32_t i = 0; i < kMaxDirectories; ++i) directory_rva_[i] = directory_size_[i] = 0;
  directories_ok_ = true;
  sections_.clear();
  exports_.clear();
  imports_.clear();
  relocations_.clear();
  relocations_ok_ = true;
  export_name_.clear();
}

bool PeFile::directory(uint32_t index, uint32_t* rva, uint32_t* size) const {
  if (index >= directory_count_) return false;
  *rva = directory_rva_[index];
  *size = directory_size_[index];
  return true;
}

const PeSection* PeFile::find_section(StringView name) const {
  for (const PeSection& s : sections_.view())
    if (s.name.view().equals(name)) return &s;
  return nullptr;
}

const PeExport* PeFile::find_export_by_ordinal(uint32_t ordinal) const {
  for (const PeExport& e : exports_.view())
    if (e.ordinal == ordinal) return &e;
  return nullptr;
}

const PeExport* PeFile::find_export(StringView name) const {
  for (const PeExport& e : exports_.view())
    if (e.name.view().equals(name)) return &e;
  return nullptr;
}

bool PeFile::rva_to_offset(uint32_t rva, uint64_t* offset) const {
  for (const PeSection& s : sections_.view()) {
    if (rva < s.virtual_address) continue;
    const uint32_t delta = rva - s.virtual_address;
    const uint32_t extent = s.virtual_size > s.raw_size ? s.virtual_size : s.raw_size;
    if (delta >= extent) continue;
    if (delta >= s.raw_size) return false;      // in the zero-filled tail of the section: no bytes in the file
    *offset = uint64_t{s.raw_offset} + delta;
    return true;
  }
  if (rva < size_of_headers_) {      // the headers themselves
    *offset = rva;
    return true;
  }
  return false;
}

bool PeFile::read_string_at(ConstByteView file, uint32_t rva, String* out) const {
  uint64_t offset = 0;
  return rva_to_offset(rva, &offset) && read_c_string(file, offset, out);
}

bool PeFile::parse(ConstByteView file) {
  clear();
  if (parse_all(file)) return true;
  clear();
  return false;
}

bool PeFile::parse_all(ConstByteView file) {
  if (!looks_like_pe(file)) return false;
  ByteReader r(file);
  r.seek(0x3C);
  const uint32_t lfanew = r.le32();
  r.seek(uint64_t{lfanew} + 4);

  // --- COFF file header
  machine_ = r.le16();
  const uint16_t section_count = r.le16();
  timestamp_ = r.le32();
  r.le32();      // PointerToSymbolTable
  r.le32();      // NumberOfSymbols
  const uint16_t optional_size = r.le16();
  characteristics_ = r.le16();
  if (!r.ok() || section_count > kMaxSections) return false;

  // --- optional header, read from its own view so a short SizeOfOptionalHeader cannot lead into the section table
  ConstByteView optional;
  if (!file_range(file, r.position(), optional_size, &optional)) return false;
  const uint64_t section_table = r.position() + optional_size;
  if (optional_size != 0) {
    ByteReader o(optional);
    const uint16_t magic = o.le16();
    if (magic != 0x10B && magic != 0x20B) return false;
    plus_ = magic == 0x20B;
    o.skip(2);      // linker version
    size_of_code_ = o.le32();
    o.skip(4 + 4);      // SizeOfInitializedData, SizeOfUninitializedData
    entry_rva_ = o.le32();
    o.le32();      // BaseOfCode
    if (plus_) {
      image_base_ = o.le64();
    } else {
      o.le32();      // BaseOfData
      image_base_ = o.le32();
    }
    section_alignment_ = o.le32();
    file_alignment_ = o.le32();
    os_major_ = o.le16();
    os_minor_ = o.le16();
    o.skip(2 * 2);      // image version
    subsystem_major_ = o.le16();
    subsystem_minor_ = o.le16();
    o.skip(4);      // Win32VersionValue
    size_of_image_ = o.le32();
    size_of_headers_ = o.le32();
    checksum_ = o.le32();
    subsystem_ = o.le16();
    dll_characteristics_ = o.le16();
    if (plus_) {
      stack_reserve_ = o.le64();
      stack_commit_ = o.le64();
      o.skip(2 * 8);      // heap reserve and commit
    } else {
      stack_reserve_ = o.le32();
      stack_commit_ = o.le32();
      o.skip(2 * 4);
    }
    loader_flags_ = o.le32();
    const uint32_t declared = o.le32();
    if (!o.ok()) return false;
    // Only the directories that fit in the header count (a lying NumberOfRvaAndSizes is clipped, not trusted).
    const uint64_t fit = o.remaining() / 8;
    directory_count_ = declared < fit ? declared : static_cast<uint32_t>(fit);
    if (directory_count_ > kMaxDirectories) directory_count_ = kMaxDirectories;
    for (uint32_t i = 0; i < directory_count_; ++i) {
      directory_rva_[i] = o.le32();
      directory_size_[i] = o.le32();
    }
    if (!o.ok()) return false;
  }

  // --- section table
  ConstByteView table;
  if (!file_range(file, section_table, uint64_t{section_count} * 40, &table)) return false;
  ByteReader s(table);
  for (uint32_t i = 0; i < section_count; ++i) {
    PeSection section(allocator_);
    ConstByteView raw_name = s.take(8);
    uint64_t name_length = 0;
    while (name_length < raw_name.size() && raw_name[name_length] != 0) ++name_length;
    if (!s.ok() || !section.name.append(bytes_as_text(raw_name.first(name_length)))) return false;
    section.virtual_size = s.le32();
    section.virtual_address = s.le32();
    section.raw_size = s.le32();
    section.raw_offset = s.le32();
    s.skip(4 + 4 + 2 + 2);      // relocations and line numbers
    section.characteristics = s.le32();
    if (!s.ok() || !sections_.push_back(static_cast<PeSection&&>(section))) return false;
  }

  // The raw data of a section must lie inside the file: a header that says otherwise is damaged.
  for (const PeSection& section : sections_.view()) {
    ConstByteView raw;
    if (section.raw_size != 0 && !file_range(file, section.raw_offset, section.raw_size, &raw)) return false;
  }

  if (!parse_exports(file) || !parse_imports(file)) {
    exports_.clear();
    imports_.clear();
    export_name_.clear();
    directories_ok_ = false;
  }
  if (!parse_relocations(file)) {
    relocations_.clear();
    relocations_ok_ = false;
  }
  return true;
}

bool PeFile::parse_exports(ConstByteView file) {
  uint32_t dir_rva = 0, dir_size = 0;
  if (!directory(0, &dir_rva, &dir_size) || dir_rva == 0 || dir_size == 0) return true;
  uint64_t offset = 0;
  ConstByteView dir;
  if (!rva_to_offset(dir_rva, &offset) || !file_range(file, offset, 40, &dir)) return false;
  ByteReader d(dir);
  d.skip(12);
  const uint32_t name_rva = d.le32(), base = d.le32(), function_count = d.le32(), name_count = d.le32();
  const uint32_t functions_rva = d.le32(), names_rva = d.le32(), ordinals_rva = d.le32();
  if (!d.ok() || function_count > kMaxEntries || name_count > kMaxEntries) return false;
  if (name_rva != 0 && !read_string_at(file, name_rva, &export_name_)) return false;

  ConstByteView functions, names, ordinals;
  if (function_count != 0 && (!rva_to_offset(functions_rva, &offset) || !file_range(file, offset, uint64_t{function_count} * 4, &functions))) return false;
  if (name_count != 0) {
    if (!rva_to_offset(names_rva, &offset) || !file_range(file, offset, uint64_t{name_count} * 4, &names)) return false;
    if (!rva_to_offset(ordinals_rva, &offset) || !file_range(file, offset, uint64_t{name_count} * 2, &ordinals)) return false;
  }

  ByteReader f(functions);
  for (uint32_t i = 0; i < function_count; ++i) {
    PeExport e(allocator_);
    e.rva = f.le32();
    e.ordinal = base + i;      // base is 32-bit and function_count is capped, so this cannot reach 2^33
    if (!f.ok()) return false;
    if (e.rva == 0) continue;      // an unused slot
    // An address inside the export directory is a forwarder string, not code.
    if (e.rva >= dir_rva && e.rva - dir_rva < dir_size && !read_string_at(file, e.rva, &e.forwarder)) return false;
    if (!exports_.push_back(static_cast<PeExport&&>(e))) return false;
  }
  ByteReader n(names), o(ordinals);
  for (uint32_t i = 0; i < name_count; ++i) {
    const uint32_t string_rva = n.le32();
    const uint16_t index = o.le16();
    if (!n.ok() || !o.ok() || index >= function_count) return false;
    const uint32_t ordinal = base + index;
    // The entries are in ordinal order, so the one for this name is found by bisection (a quadratic scan would let a crafted table stall us).
    uint64_t low = 0, high = exports_.count();
    while (low < high) {
      const uint64_t middle = low + (high - low) / 2;
      if (exports_[middle].ordinal < ordinal) low = middle + 1;
      else high = middle;
    }
    if (low < exports_.count() && exports_[low].ordinal == ordinal && !read_string_at(file, string_rva, &exports_[low].name)) return false;
  }
  return true;
}

bool PeFile::parse_imports(ConstByteView file) {
  uint32_t dir_rva = 0, dir_size = 0;
  if (!directory(1, &dir_rva, &dir_size) || dir_rva == 0 || dir_size == 0) return true;
  uint64_t offset = 0;
  if (!rva_to_offset(dir_rva, &offset)) return false;
  const uint64_t thunk_size = plus_ ? 8 : 4;
  for (uint32_t dll = 0; dll < kMaxDlls; ++dll) {
    ConstByteView entry;
    if (!file_range(file, offset + uint64_t{dll} * 20, 20, &entry)) return false;
    ByteReader d(entry);
    const uint32_t lookup_rva = d.le32();
    d.skip(8);
    const uint32_t name_rva = d.le32(), address_rva = d.le32();
    if (lookup_rva == 0 && name_rva == 0 && address_rva == 0) return true;      // the all-zero terminator
    String dll_name(allocator_);
    if (!read_string_at(file, name_rva, &dll_name)) return false;
    uint64_t thunks_offset = 0;
    if (!rva_to_offset(lookup_rva != 0 ? lookup_rva : address_rva, &thunks_offset)) return false;
    for (uint32_t i = 0; i < kMaxEntries; ++i) {
      ConstByteView thunk_bytes;
      if (!file_range(file, thunks_offset + uint64_t{i} * thunk_size, thunk_size, &thunk_bytes)) return false;
      ByteReader t(thunk_bytes);
      const uint64_t thunk = plus_ ? t.le64() : t.le32();
      if (thunk == 0) break;
      PeImport imp(allocator_);
      if (!imp.dll.append(dll_name.view())) return false;
      if (address_rva > 0xFFFFFFFFu - i * thunk_size) return false;
      imp.slot_rva = address_rva + static_cast<uint32_t>(i * thunk_size);
      const uint64_t ordinal_flag = plus_ ? (uint64_t{1} << 63) : (uint64_t{1} << 31);
      if (thunk & ordinal_flag) {
        imp.by_ordinal = true;
        imp.ordinal = static_cast<uint16_t>(thunk & 0xFFFF);
      } else {
        uint64_t name_offset = 0;
        ConstByteView hint;
        if (thunk > 0xFFFFFFFFu || !rva_to_offset(static_cast<uint32_t>(thunk), &name_offset) || !file_range(file, name_offset, 2, &hint)) return false;
        imp.hint = static_cast<uint16_t>(hint[0] | hint[1] << 8);
        if (!read_c_string(file, name_offset + 2, &imp.name)) return false;
      }
      if (!imports_.push_back(static_cast<PeImport&&>(imp))) return false;
    }
  }
  return false;      // more DLLs than any real file has: not terminated
}

bool PeFile::parse_relocations(ConstByteView file) {
  uint32_t dir_rva = 0, dir_size = 0;
  if (!directory(5, &dir_rva, &dir_size) || dir_rva == 0 || dir_size == 0) return true;
  uint64_t offset = 0;
  ConstByteView table;
  if (!rva_to_offset(dir_rva, &offset) || !file_range(file, offset, dir_size, &table)) return false;
  uint64_t at = 0;
  while (at + 8 <= table.size()) {
    ByteReader h(table.subview(at, 8));
    const uint32_t page = h.le32(), block = h.le32();
    if (page == 0 && block == 0) break;      // padding at the end of the directory
    if (block < 8 || block % 2 != 0 || block > table.size() - at) return false;
    ByteReader entries(table.subview(at + 8, block - 8));
    for (uint64_t i = 0; i < (block - 8) / 2; ++i) {
      const uint16_t entry = entries.le16();
      const uint8_t type = static_cast<uint8_t>(entry >> 12);
      if (type == kPeRelocationAbsolute) continue;
      if (relocations_.count() >= kMaxRelocations) return false;
      if (type != kPeRelocationDir64 && type != kPeRelocationHighLow) return false;      // (the other kinds do not occur on x86-64 images)
      PeRelocation r;
      r.rva = page + (entry & 0xFFF);
      r.type = type;
      if (r.rva < page || !relocations_.push_back(r)) return false;
    }
    at += block;
  }
  return true;
}

}  // namespace exe
