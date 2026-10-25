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
#include "Executable/ElfFile.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/ByteText.hpp"

namespace exe {

namespace {

// A ByteReader with the byte order and the word size of the file.
class ElfReader {
 public:
  ElfReader(ConstByteView data, bool big, bool wide) : in_(data), big_(big), wide_(wide) {}
  ByteReader& in() { return in_; }
  bool big_endian() const { return big_; }
  bool ok() const { return in_.ok(); }
  uint16_t u16() {
    const uint16_t v = big_ ? in_.be16() : in_.le16();
    return v;
  }
  uint32_t u32() { return big_ ? in_.be32() : in_.le32(); }
  uint64_t u64() {
    if (!big_) return in_.le64();
    const uint64_t hi = in_.be32();
    return hi << 32 | in_.be32();
  }
  // An address or offset: 8 bytes in a 64-bit file, 4 in a 32-bit one.
  uint64_t word() { return wide_ ? u64() : u32(); }

 private:
  ByteReader in_;
  bool big_, wide_;
};

// The NUL-terminated string at `offset` of a string table, appended to `out`; false if it runs off the end or `offset` is outside.
bool read_name(ConstByteView table, uint64_t offset, String* out) {
  if (offset >= table.size()) return false;
  ConstByteView rest = table.subview(offset);
  uint64_t length = 0;
  while (length < rest.size() && rest[length] != 0) ++length;
  if (length == rest.size()) return false;
  return out->append(bytes_as_text(rest.first(length)));
}

// A range [offset, offset + size) of the file, checked without adding the two.
bool file_range(ConstByteView file, uint64_t offset, uint64_t size, ConstByteView* out) {
  if (offset > file.size() || size > file.size() - offset) return false;
  *out = file.subview(offset, size);
  return true;
}

}  // namespace

bool ElfFile::looks_like_elf(ConstByteView data) {
  return data.size() >= 4 && data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F';
}

void ElfFile::clear() {
  class_ = ElfClass::None;
  data_ = ElfData::None;
  os_abi_ = 0;
  type_ = machine_ = 0;
  flags_ = 0;
  entry_ = 0;
  segments_.clear();
  sections_.clear();
  symbols_.clear();
  needed_.clear();
  interpreter_.clear();
  soname_.clear();
  run_path_.clear();
}

bool ElfFile::section_data(ConstByteView file, const ElfSection& section, ConstByteView* out) {
  if (section.type == kShtNobits || section.type == kShtNull) {
    *out = ConstByteView();
    return true;
  }
  return file_range(file, section.offset, section.size, out);
}

const ElfSection* ElfFile::find_section(StringView name) const {
  for (const ElfSection& s : sections_.view())
    if (s.name.view().equals(name)) return &s;
  return nullptr;
}

const ElfSymbol* ElfFile::find_symbol(StringView name) const {
  for (const ElfSymbol& s : symbols_.view())
    if (s.name.view().equals(name)) return &s;
  return nullptr;
}

bool ElfFile::parse(ConstByteView file) {
  clear();
  if (parse_all(file)) return true;
  clear();
  return false;
}

// The file offset of a virtual address, through the loadable segments.
static bool address_to_offset(const List<ElfSegment>& segments, uint64_t address, uint64_t* out) {
  for (const ElfSegment& s : segments.view()) {
    if (s.type != kPtLoad || address < s.vaddr || address - s.vaddr >= s.file_size) continue;
    const uint64_t delta = address - s.vaddr;
    if (delta > ~uint64_t{0} - s.offset) return false;
    *out = s.offset + delta;
    return true;
  }
  return false;
}

bool ElfFile::parse_all(ConstByteView file) {
  if (!looks_like_elf(file) || file.size() < 16) return false;
  const uint8_t cls = file[4], enc = file[5];
  if ((cls != 1 && cls != 2) || (enc != 1 && enc != 2) || file[6] != 1) return false;
  class_ = cls == 1 ? ElfClass::Elf32 : ElfClass::Elf64;
  data_ = enc == 1 ? ElfData::Little : ElfData::Big;
  os_abi_ = file[7];
  const bool wide = cls == 2;

  ElfReader r(file, enc == 2, wide);
  r.in().skip(16);
  type_ = r.u16();
  machine_ = r.u16();
  r.u32();      // e_version
  entry_ = r.word();
  const uint64_t phoff = r.word(), shoff = r.word();
  flags_ = r.u32();
  r.u16();      // e_ehsize
  const uint16_t phentsize = r.u16(), phnum = r.u16();
  const uint16_t shentsize = r.u16();
  uint64_t shnum = r.u16();
  uint64_t shstrndx = r.u16();
  if (!r.ok()) return false;

  // --- program headers
  if (phnum != 0) {
    const uint64_t minimum = wide ? 56 : 32;
    ConstByteView table;
    if (phentsize < minimum || phnum > kMaxEntries || !file_range(file, phoff, uint64_t{phnum} * phentsize, &table)) return false;
    for (uint64_t i = 0; i < phnum; ++i) {
      ElfReader p(table.subview(i * phentsize, phentsize), enc == 2, wide);
      ElfSegment seg;
      seg.type = p.u32();
      if (wide) seg.flags = p.u32();
      seg.offset = p.word();
      seg.vaddr = p.word();
      seg.paddr = p.word();
      seg.file_size = p.word();
      seg.mem_size = p.word();
      if (!wide) seg.flags = p.u32();
      seg.align = p.word();
      if (!p.ok() || !segments_.push_back(seg)) return false;
    }
  }

  // --- section headers (an extended count or name index lives in the first one)
  if (shoff != 0) {
    const uint64_t minimum = wide ? 64 : 40;
    if (shentsize < minimum) return false;
    ConstByteView first_entry;
    if (!file_range(file, shoff, shentsize, &first_entry)) return false;
    ElfReader f(first_entry, enc == 2, wide);
    f.u32(); f.u32(); f.word(); f.word(); f.word();
    const uint64_t extended_count = f.word();
    const uint32_t extended_link = f.u32();
    if (!f.ok()) return false;
    if (shnum == 0) shnum = extended_count;
    if (shstrndx == 0xFFFF) shstrndx = extended_link;
  } else {
    shnum = 0;
  }
  if (shnum > kMaxEntries) return false;

  ConstByteView table;
  if (shnum != 0 && !file_range(file, shoff, shnum * shentsize, &table)) return false;
  List<uint32_t> name_offsets(allocator_);
  for (uint64_t i = 0; i < shnum; ++i) {
    ElfReader s(table.subview(i * shentsize, shentsize), enc == 2, wide);
    ElfSection sec(allocator_);
    const uint32_t name_offset = s.u32();
    sec.type = s.u32();
    sec.flags = s.word();
    sec.addr = s.word();
    sec.offset = s.word();
    sec.size = s.word();
    sec.link = s.u32();
    sec.info = s.u32();
    sec.addr_align = s.word();
    sec.entry_size = s.word();
    if (!s.ok() || !name_offsets.push_back(name_offset) || !sections_.push_back(static_cast<ElfSection&&>(sec))) return false;
  }

  // Section names; a name that is outside the table stays empty.
  if (shnum != 0) {
    if (shstrndx >= shnum) return false;
    ConstByteView names;
    if (!section_data(file, sections_[shstrndx], &names)) return false;
    for (uint64_t i = 0; i < shnum; ++i) read_name(names, name_offsets[i], &sections_[i].name);
  }

  // --- symbol tables
  for (uint64_t i = 0; i < shnum; ++i) {
    const ElfSection& sec = sections_[i];
    if ((sec.type != kShtSymtab && sec.type != kShtDynsym) || sec.link >= shnum) continue;
    const uint64_t minimum = wide ? 24 : 16;
    const uint64_t entry_size = sec.entry_size ? sec.entry_size : minimum;
    ConstByteView symbols, strings;
    if (entry_size < minimum || !section_data(file, sec, &symbols) || !section_data(file, sections_[sec.link], &strings)) return false;
    const uint64_t count = symbols.size() / entry_size;
    if (count > kMaxSymbols - symbols_.count()) return false;
    for (uint64_t k = 1; k < count; ++k) {      // symbol 0 is the null symbol
      ElfReader y(symbols.subview(k * entry_size, entry_size), enc == 2, wide);
      ElfSymbol sym(allocator_);
      const uint32_t name_offset = y.u32();
      if (wide) {
        sym.info = y.in().u8();
        sym.other = y.in().u8();
        sym.section_index = y.u16();
        sym.value = y.u64();
        sym.size = y.u64();
      } else {
        sym.value = y.u32();
        sym.size = y.u32();
        sym.info = y.in().u8();
        sym.other = y.in().u8();
        sym.section_index = y.u16();
      }
      sym.dynamic = sec.type == kShtDynsym;
      if (!y.ok()) return false;
      if (name_offset != 0) read_name(strings, name_offset, &sym.name);
      if (!symbols_.push_back(static_cast<ElfSymbol&&>(sym))) return false;
    }
  }

  // --- interpreter and dynamic section
  for (const ElfSegment& seg : segments_.view()) {
    if (seg.type == kPtInterp) {
      ConstByteView text;
      if (file_range(file, seg.offset, seg.file_size, &text)) read_name(text, 0, &interpreter_);
    } else if (seg.type == kPtDynamic) {
      if (!parse_dynamic(file, seg, r.big_endian(), wide)) return false;
    }
  }
  return true;
}

bool ElfFile::parse_dynamic(ConstByteView file, const ElfSegment& seg, bool big, bool wide) {
  ConstByteView dynamic;
  if (!file_range(file, seg.offset, seg.file_size, &dynamic)) return false;
  const uint64_t entry_size = wide ? 16 : 8;
  uint64_t strtab = 0, strsz = 0;
  bool have_strtab = false;
  List<uint64_t> needed(allocator_);
  uint64_t soname = 0, rpath = 0, runpath = 0;
  bool have_soname = false, have_rpath = false, have_runpath = false;
  ElfReader d(dynamic, big, wide);
  for (uint64_t i = 0; i < dynamic.size() / entry_size && i < kMaxEntries; ++i) {
    const uint64_t tag = d.word(), value = d.word();
    if (!d.ok() || tag == kDtNull) break;
    switch (tag) {
      case kDtNeeded: if (!needed.push_back(value)) return false; break;
      case kDtStrtab: strtab = value; have_strtab = true; break;
      case 10: strsz = value; break;      // DT_STRSZ
      case kDtSoname: soname = value; have_soname = true; break;
      case kDtRpath: rpath = value; have_rpath = true; break;
      case kDtRunpath: runpath = value; have_runpath = true; break;
      default: break;
    }
  }
  if (!have_strtab) return true;      // no names to resolve
  uint64_t offset = 0;
  ConstByteView strings;
  if (!address_to_offset(segments_, strtab, &offset) || offset > file.size()) return true;
  const uint64_t available = file.size() - offset;
  if (!file_range(file, offset, strsz && strsz < available ? strsz : available, &strings)) return true;
  for (uint64_t n : needed.view()) {
    String name(allocator_);
    if (read_name(strings, n, &name) && !needed_.push_back(static_cast<String&&>(name))) return false;
  }
  if (have_soname) read_name(strings, soname, &soname_);
  if (have_runpath) read_name(strings, runpath, &run_path_);
  else if (have_rpath) read_name(strings, rpath, &run_path_);
  return true;
}

}  // namespace exe
