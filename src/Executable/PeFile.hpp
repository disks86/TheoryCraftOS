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
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace exe {

// IMAGE_FILE_HEADER.Machine
constexpr uint16_t kPeMachineI386 = 0x014C, kPeMachineAmd64 = 0x8664, kPeMachineArm = 0x01C4, kPeMachineArm64 = 0xAA64;
// IMAGE_FILE_HEADER.Characteristics
constexpr uint16_t kPeExecutableImage = 0x0002, kPeDll = 0x2000;
// Section characteristics
constexpr uint32_t kPeSectionCode = 0x20, kPeSectionExecute = 0x20000000, kPeSectionRead = 0x40000000, kPeSectionWrite = 0x80000000u;
// Optional header Subsystem
constexpr uint16_t kPeSubsystemNative = 1, kPeSubsystemGui = 2, kPeSubsystemConsole = 3, kPeSubsystemEfiApplication = 10;

struct PeSection {
  explicit PeSection(allocators::IAllocator& allocator) : name(allocator) {}
  String name;
  uint32_t virtual_size = 0, virtual_address = 0, raw_size = 0, raw_offset = 0, characteristics = 0;
};

struct PeExport {
  explicit PeExport(allocators::IAllocator& allocator) : name(allocator), forwarder(allocator) {}
  String name;           // empty when exported by ordinal only
  String forwarder;      // "OTHER.Function" when the export is a forward to another DLL
  uint32_t ordinal = 0;
  uint32_t rva = 0;
};

// One imported function: the DLL it comes from, and its name or ordinal.
struct PeImport {
  explicit PeImport(allocators::IAllocator& allocator) : dll(allocator), name(allocator) {}
  String dll;
  String name;           // empty when imported by ordinal
  uint16_t hint = 0;
  uint16_t ordinal = 0;
  bool by_ordinal = false;
  uint32_t slot_rva = 0;      // where the bound address goes: the import address table entry for this function
};

// One base relocation: the 32- or 64-bit value at `rva` holds an address that moves with the image.
constexpr uint8_t kPeRelocationAbsolute = 0, kPeRelocationHighLow = 3, kPeRelocationDir64 = 10;
struct PeRelocation {
  uint32_t rva = 0;
  uint8_t type = 0;
};

// Reads the headers of a PE/COFF image (PE32 or PE32+): the COFF and optional headers, the data directories, the sections, the export table and the
// import table. Everything kept is copied, so the buffer may go away after parse(). The headers and the section table must be sound or the parse
// fails; a broken export or import directory (packed or hand-made files) does not fail it but clears directories_ok(). Nothing outside `data` is read.
class PeFile {
 public:
  explicit PeFile(allocators::IAllocator& allocator) : allocator_(allocator), sections_(allocator), exports_(allocator), imports_(allocator), relocations_(allocator), export_name_(allocator) {}
  PeFile(const PeFile&) = delete;
  PeFile& operator=(const PeFile&) = delete;

  static constexpr uint32_t kMaxSections = 96;
  static constexpr uint32_t kMaxDirectories = 16;
  static constexpr uint32_t kMaxEntries = 1u << 16;     // exports, imports of one DLL
  static constexpr uint32_t kMaxDlls = 1u << 12;
  static constexpr uint32_t kMaxRelocations = 1u << 20;

  static bool looks_like_pe(ConstByteView data);        // "MZ" with a "PE\0\0" where the DOS header points
  bool parse(ConstByteView data);
  void clear();

  bool is_pe32_plus() const { return plus_; }
  uint16_t machine() const { return machine_; }
  uint16_t characteristics() const { return characteristics_; }
  bool is_dll() const { return (characteristics_ & kPeDll) != 0; }
  uint32_t timestamp() const { return timestamp_; }
  uint32_t entry_rva() const { return entry_rva_; }
  uint32_t size_of_code() const { return size_of_code_; }
  uint64_t image_base() const { return image_base_; }
  uint32_t section_alignment() const { return section_alignment_; }
  uint32_t file_alignment() const { return file_alignment_; }
  uint32_t size_of_image() const { return size_of_image_; }
  uint32_t size_of_headers() const { return size_of_headers_; }
  uint16_t subsystem() const { return subsystem_; }
  uint16_t dll_characteristics() const { return dll_characteristics_; }
  uint32_t checksum() const { return checksum_; }
  uint16_t os_major() const { return os_major_; }
  uint16_t os_minor() const { return os_minor_; }
  uint16_t subsystem_major() const { return subsystem_major_; }
  uint16_t subsystem_minor() const { return subsystem_minor_; }
  uint64_t stack_reserve() const { return stack_reserve_; }
  uint64_t stack_commit() const { return stack_commit_; }
  uint32_t loader_flags() const { return loader_flags_; }

  // The data directories the file has (the index is IMAGE_DIRECTORY_ENTRY_*: 0 exports, 1 imports, 2 resources, ...).
  uint32_t directory_count() const { return directory_count_; }
  bool directory(uint32_t index, uint32_t* rva, uint32_t* size) const;

  const List<PeSection>& sections() const { return sections_; }
  const List<PeExport>& exports() const { return exports_; }
  const List<PeImport>& imports() const { return imports_; }
  // The base relocations (padding entries left out), in file order. Empty when the image has none or its table is damaged (relocations_ok()).
  const List<PeRelocation>& relocations() const { return relocations_; }
  bool relocations_ok() const { return relocations_ok_; }
  StringView export_dll_name() const { return export_name_.view(); }
  bool directories_ok() const { return directories_ok_; }

  const PeSection* find_section(StringView name) const;
  const PeExport* find_export(StringView name) const;
  const PeExport* find_export_by_ordinal(uint32_t ordinal) const;
  // The file offset of a relative virtual address; false if it is in no section (or past the raw data of one).
  bool rva_to_offset(uint32_t rva, uint64_t* offset) const;

 private:
  bool parse_all(ConstByteView file);
  bool parse_exports(ConstByteView file);
  bool parse_imports(ConstByteView file);
  bool parse_relocations(ConstByteView file);
  bool read_string_at(ConstByteView file, uint32_t rva, String* out) const;

  allocators::IAllocator& allocator_;
  bool plus_ = false;
  uint16_t machine_ = 0, characteristics_ = 0, subsystem_ = 0, dll_characteristics_ = 0;
  uint32_t size_of_code_ = 0, timestamp_ = 0, entry_rva_ = 0, section_alignment_ = 0, file_alignment_ = 0, size_of_image_ = 0, size_of_headers_ = 0, checksum_ = 0;
  uint64_t image_base_ = 0, stack_reserve_ = 0, stack_commit_ = 0;
  uint16_t os_major_ = 0, os_minor_ = 0, subsystem_major_ = 0, subsystem_minor_ = 0;
  uint32_t loader_flags_ = 0;
  uint32_t directory_count_ = 0;
  uint32_t directory_rva_[kMaxDirectories] = {};
  uint32_t directory_size_[kMaxDirectories] = {};
  bool directories_ok_ = true;
  bool relocations_ok_ = true;
  List<PeSection> sections_;
  List<PeExport> exports_;
  List<PeImport> imports_;
  List<PeRelocation> relocations_;
  String export_name_;
};

}  // namespace exe
