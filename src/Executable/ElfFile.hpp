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

enum class ElfClass : uint8_t { None, Elf32, Elf64 };
enum class ElfData : uint8_t { None, Little, Big };

// e_type
constexpr uint16_t kElfRelocatable = 1, kElfExecutable = 2, kElfShared = 3, kElfCore = 4;
// p_type
constexpr uint32_t kPtLoad = 1, kPtDynamic = 2, kPtInterp = 3, kPtNote = 4;
// p_flags
constexpr uint32_t kPfExecute = 1, kPfWrite = 2, kPfRead = 4;
// sh_type
constexpr uint32_t kShtNull = 0, kShtProgbits = 1, kShtSymtab = 2, kShtStrtab = 3, kShtNobits = 8, kShtDynsym = 11;
// sh_flags
constexpr uint64_t kShfWrite = 1, kShfAlloc = 2, kShfExecInstr = 4;
// d_tag
constexpr uint64_t kDtNull = 0, kDtNeeded = 1, kDtStrtab = 5, kDtSoname = 14, kDtRpath = 15, kDtRunpath = 29;

struct ElfSegment {
  uint32_t type = 0, flags = 0;
  uint64_t offset = 0, vaddr = 0, paddr = 0, file_size = 0, mem_size = 0, align = 0;
};

struct ElfSection {
  explicit ElfSection(allocators::IAllocator& allocator) : name(allocator) {}
  String name;
  uint32_t type = 0, link = 0, info = 0;
  uint64_t flags = 0, addr = 0, offset = 0, size = 0, addr_align = 0, entry_size = 0;
};

struct ElfSymbol {
  explicit ElfSymbol(allocators::IAllocator& allocator) : name(allocator) {}
  String name;
  uint64_t value = 0, size = 0;
  uint16_t section_index = 0;
  uint8_t info = 0, other = 0;
  bool dynamic = false;      // from .dynsym (else .symtab)
  uint8_t bind() const { return info >> 4; }      // 0 local, 1 global, 2 weak
  uint8_t type() const { return info & 15; }      // 0 none, 1 object, 2 func, 3 section, 4 file
};

// Reads the headers of an ELF file (32 or 64 bit, either byte order): the program headers, the sections with their names, the symbol tables, the
// interpreter, and the names the dynamic section asks for. The data is only read while parse() runs (everything kept is copied), so the buffer may go
// away afterwards. A table that is cut off or points outside the file fails the parse; nothing outside `data` is read.
class ElfFile {
 public:
  explicit ElfFile(allocators::IAllocator& allocator)
      : allocator_(allocator), segments_(allocator), sections_(allocator), symbols_(allocator), needed_(allocator), interpreter_(allocator), soname_(allocator),
        run_path_(allocator) {}
  ElfFile(const ElfFile&) = delete;
  ElfFile& operator=(const ElfFile&) = delete;

  static constexpr uint64_t kMaxEntries = 1u << 16;      // program headers, sections, dynamic entries
  static constexpr uint64_t kMaxSymbols = 1u << 20;

  // Does this look like an ELF file (the four magic bytes)?
  static bool looks_like_elf(ConstByteView data);
  bool parse(ConstByteView data);
  void clear();

  ElfClass elf_class() const { return class_; }
  ElfData data_encoding() const { return data_; }
  bool is_64() const { return class_ == ElfClass::Elf64; }
  uint8_t os_abi() const { return os_abi_; }
  uint16_t type() const { return type_; }
  uint16_t machine() const { return machine_; }
  uint64_t entry() const { return entry_; }
  uint32_t flags() const { return flags_; }
  // Where the program header table sits in the file (a loader needs its address in memory for the auxiliary vector).
  uint64_t program_header_offset() const { return phoff_; }
  uint16_t program_header_size() const { return phentsize_; }
  uint16_t program_header_count() const { return phnum_; }

  const List<ElfSegment>& segments() const { return segments_; }
  const List<ElfSection>& sections() const { return sections_; }
  const List<ElfSymbol>& symbols() const { return symbols_; }
  const List<String>& needed() const { return needed_; }       // DT_NEEDED: the libraries it links against
  StringView interpreter() const { return interpreter_.view(); }     // PT_INTERP, empty if none
  StringView soname() const { return soname_.view(); }
  StringView run_path() const { return run_path_.view(); }          // DT_RUNPATH, else DT_RPATH

  const ElfSection* find_section(StringView name) const;
  const ElfSymbol* find_symbol(StringView name) const;
  // The file bytes of a section (empty for NOBITS); needs the same buffer that was parsed.
  static bool section_data(ConstByteView file, const ElfSection& section, ConstByteView* out);

 private:
  bool parse_all(ConstByteView file);
  bool parse_dynamic(ConstByteView file, const ElfSegment& segment, bool big, bool wide);

  allocators::IAllocator& allocator_;
  ElfClass class_ = ElfClass::None;
  ElfData data_ = ElfData::None;
  uint8_t os_abi_ = 0;
  uint16_t type_ = 0, machine_ = 0;
  uint32_t flags_ = 0;
  uint64_t entry_ = 0, phoff_ = 0;
  uint16_t phentsize_ = 0, phnum_ = 0;
  List<ElfSegment> segments_;
  List<ElfSection> sections_;
  List<ElfSymbol> symbols_;
  List<String> needed_;
  String interpreter_, soname_, run_path_;
};

}  // namespace exe
