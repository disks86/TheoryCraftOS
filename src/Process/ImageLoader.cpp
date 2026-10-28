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
#include "Process/ImageLoader.hpp"

namespace process {

namespace {

// [offset, offset + size) lies inside the file.
bool in_file(ConstByteView file, uint64_t offset, uint64_t size) { return offset <= file.size() && size <= file.size() - offset; }

uint8_t elf_protection(uint32_t flags) {
  return uint8_t(((flags & exe::kPfRead) ? kProtRead : 0) | ((flags & exe::kPfWrite) ? kProtWrite : 0) | ((flags & exe::kPfExecute) ? kProtExecute : 0));
}

bool put_word(AddressSpace& space, uint64_t address, uint64_t value) {
  uint8_t raw[8];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = uint8_t(value >> (8 * i));
  return space.write_privileged(address, ConstByteView(raw, 8));
}

// One page-aligned run of the image and the permissions it needs.
struct Run {
  uint64_t start = 0, end = 0;
  uint8_t protection = 0;
};

}  // namespace

LoadStatus load_elf(const exe::ElfFile& elf, ConstByteView file, AddressSpace& space, LoadedImage* out) {
  if (!elf.is_64() || elf.data_encoding() != exe::ElfData::Little) return LoadStatus::Unsupported;
  if (!elf.interpreter().empty()) return LoadStatus::NeedsLibraries;
  if (elf.type() != exe::kElfExecutable && elf.type() != exe::kElfShared) return LoadStatus::Unsupported;
  const uint64_t bias = elf.type() == exe::kElfShared ? kElfPieBase : 0;

  // Pass 1: check every PT_LOAD and map the runs.
  Run run;
  bool have_run = false;
  uint64_t previous_end = 0, low = ~uint64_t{0}, high = 0;
  for (const exe::ElfSegment& seg : elf.segments().view()) {
    if (seg.type != exe::kPtLoad || seg.mem_size == 0) continue;
    if (seg.file_size > seg.mem_size || !in_file(file, seg.offset, seg.file_size)) return LoadStatus::Malformed;
    if (seg.vaddr > AddressSpace::kUserTop || bias > AddressSpace::kUserTop - seg.vaddr) return LoadStatus::Malformed;
    const uint64_t start = seg.vaddr + bias;
    if (seg.mem_size > AddressSpace::kUserTop - start) return LoadStatus::Malformed;
    const uint64_t end = start + seg.mem_size;
    if (start < previous_end) return LoadStatus::Malformed;      // the table must be in ascending order and not overlap
    previous_end = end;
    uint64_t page_end = 0;
    if (!AddressSpace::round_up(end, &page_end)) return LoadStatus::Malformed;
    const uint64_t page_start = AddressSpace::round_down(start);
    if (page_start == 0) return LoadStatus::Malformed;      // page zero stays unmapped so null pointers fault
    if (have_run && page_start < run.end) {
      if (page_end > run.end) run.end = page_end;
      run.protection |= elf_protection(seg.flags);
    } else {
      if (have_run && !space.map(run.start, run.end - run.start, run.protection)) return LoadStatus::OutOfMemory;
      run = Run{page_start, page_end, elf_protection(seg.flags)};
      have_run = true;
    }
    if (page_start < low) low = page_start;
    if (page_end > high) high = page_end;
  }
  if (!have_run) return LoadStatus::Malformed;
  if (!space.map(run.start, run.end - run.start, run.protection)) return LoadStatus::OutOfMemory;

  // Pass 2: copy the file bytes (the zero tail is already there).
  LoadedImage image;
  image.base = low;
  image.end = high;
  image.entry = elf.entry() + bias;
  image.phent = elf.program_header_size();
  image.phnum = elf.program_header_count();
  for (const exe::ElfSegment& seg : elf.segments().view()) {
    if (seg.type != exe::kPtLoad || seg.mem_size == 0) continue;
    if (seg.file_size && !space.write_privileged(seg.vaddr + bias, file.subview(seg.offset, seg.file_size))) return LoadStatus::Malformed;
    if (image.phdr == 0 && elf.program_header_offset() >= seg.offset && elf.program_header_offset() - seg.offset < seg.file_size) image.phdr = seg.vaddr + bias + (elf.program_header_offset() - seg.offset);
  }
  if (!space.check(image.entry, 1, kProtExecute)) return LoadStatus::Malformed;
  *out = image;
  return LoadStatus::Ok;
}

LoadStatus load_pe(const exe::PeFile& pe, ConstByteView file, AddressSpace& space, LoadedImage* out, bool require_entry) {
  if (!pe.is_pe32_plus() || pe.is_dll()) return LoadStatus::Unsupported;
  return map_pe_image(pe, file, space, pe.image_base(), require_entry, out);
}

void describe_pe_image(const exe::PeFile& pe, uint64_t file_size, uint8_t (&out)[64]) {
  for (uint8_t& b : out) b = 0;
  auto put = [&](uint32_t at, uint64_t value, uint32_t bytes) { for (uint32_t i = 0; i < bytes; ++i) out[at + i] = uint8_t(value >> (8 * i)); };
  put(0, pe.image_base() + pe.entry_rva(), 8);
  put(16, pe.stack_reserve(), 8);
  put(24, pe.stack_commit(), 8);
  put(32, pe.subsystem(), 4);
  put(36, pe.subsystem_minor(), 2);
  put(38, pe.subsystem_major(), 2);
  put(40, pe.os_major(), 2);
  put(42, pe.os_minor(), 2);
  put(44, pe.characteristics(), 2);
  put(46, pe.dll_characteristics(), 2);
  put(48, pe.machine(), 2);
  bool code = pe.entry_rva() != 0;
  for (const exe::PeSection& section : pe.sections().view()) if (section.characteristics & 0x20u) code = true;      // IMAGE_SCN_CNT_CODE
  put(50, code ? 1 : 0, 1);
  put(51, (pe.dll_characteristics() & 0x40) ? 0x04 : 0, 1);      // ImageDynamicallyRelocated
  put(52, pe.loader_flags(), 4);
  put(56, file_size, 4);
  put(60, pe.checksum(), 4);
}

LoadStatus map_pe_image(const exe::PeFile& pe, ConstByteView file, AddressSpace& space, uint64_t base, bool require_entry, LoadedImage* out) {
  if (!pe.is_pe32_plus() || pe.machine() != exe::kPeMachineAmd64) return LoadStatus::Unsupported;
  if (pe.section_alignment() < AddressSpace::kPageSize) return LoadStatus::Unsupported;      // the old 512-byte layout shares pages between sections
  const uint64_t image_size = pe.size_of_image();
  uint64_t image_pages = 0;
  if (base == 0 || base % AddressSpace::kPageSize != 0 || !AddressSpace::round_up(image_size, &image_pages) || image_pages == 0 ||
      base >= AddressSpace::kUserTop || image_pages > AddressSpace::kUserTop - base)
    return LoadStatus::Malformed;

  const uint64_t header_bytes = pe.size_of_headers() < file.size() ? pe.size_of_headers() : file.size();
  if (header_bytes == 0 || header_bytes > image_size) return LoadStatus::Malformed;
  if (!space.map(base, header_bytes, kProtRead)) return LoadStatus::OutOfMemory;
  if (!space.write_privileged(base, file.first(header_bytes))) return LoadStatus::Malformed;

  for (const exe::PeSection& section : pe.sections().view()) {
    const uint64_t span = section.virtual_size ? section.virtual_size : section.raw_size;
    if (span == 0) continue;
    if (section.virtual_address % AddressSpace::kPageSize != 0 || section.virtual_address > image_size || span > image_size - section.virtual_address) return LoadStatus::Malformed;
    const uint8_t protection = uint8_t(((section.characteristics & exe::kPeSectionRead) ? kProtRead : 0) | ((section.characteristics & exe::kPeSectionWrite) ? kProtWrite : 0) |
                                       ((section.characteristics & exe::kPeSectionExecute) ? kProtExecute : 0));
    if (!space.map(base + section.virtual_address, span, protection)) return LoadStatus::Malformed;      // overlaps the headers or another section
    const uint64_t copy = section.raw_size < span ? section.raw_size : span;
    if (copy) {
      if (!in_file(file, section.raw_offset, copy)) return LoadStatus::Malformed;
      if (!space.write_privileged(base + section.virtual_address, file.subview(section.raw_offset, copy))) return LoadStatus::Malformed;
    }
  }
  if (pe.entry_rva() >= image_size || (require_entry && pe.entry_rva() == 0)) return LoadStatus::Malformed;
  LoadedImage image;
  image.base = base;
  image.end = base + image_pages;
  image.entry = pe.entry_rva() ? base + pe.entry_rva() : 0;
  if (image.entry != 0 && !space.check(image.entry, 1, kProtExecute)) return LoadStatus::Malformed;
  *out = image;
  return LoadStatus::Ok;
}

LoadStatus map_stack(AddressSpace& space, uint64_t size, uint64_t* top) {
  uint64_t rounded = 0;
  if (size == 0 || !AddressSpace::round_up(size, &rounded) || rounded > AddressSpace::kUserTop - 2 * AddressSpace::kPageSize) return LoadStatus::Malformed;
  const uint64_t end = AddressSpace::kUserTop - AddressSpace::kPageSize;      // the page above stays unmapped
  if (!space.map(end - rounded, rounded, kProtRead | kProtWrite)) return LoadStatus::OutOfMemory;
  *top = end;
  return LoadStatus::Ok;
}

LoadStatus build_linux_stack(AddressSpace& space, uint64_t stack_top, const LoadedImage& image, ArrayView<const StringView> arguments,
                             ArrayView<const StringView> environment, StringView executable, ConstByteView random, uint64_t* stack_pointer, StringView platform) {
  if (random.size() != 16 || arguments.size() > 4096 || environment.size() > 4096) return LoadStatus::Malformed;
  uint64_t string_bytes = 0;
  for (const StringView& s : arguments) string_bytes += s.size() + 1;      // each is bounded below, so the sum cannot wrap
  for (const StringView& s : environment) string_bytes += s.size() + 1;
  if (executable.size() > kMaxStartupStrings) return LoadStatus::Malformed;
  string_bytes += executable.size() + 1 + platform.size() + 1;
  if (string_bytes > kMaxStartupStrings) return LoadStatus::Malformed;
  if (string_bytes > stack_top) return LoadStatus::Malformed;

  // Strings first (arguments then environment, ascending from `strings`), then the 16 random bytes just below them.
  const uint64_t strings = stack_top - string_bytes;
  uint64_t cursor = strings;
  const uint8_t nul = 0;
  const uint64_t executable_at = strings + (string_bytes - platform.size() - 1 - executable.size() - 1);
  const uint64_t platform_at = strings + (string_bytes - platform.size() - 1);
  for (uint32_t pass = 0; pass < 4; ++pass) {
    ArrayView<const StringView> group = pass == 0 ? arguments : pass == 1 ? environment : ArrayView<const StringView>();
    if (pass == 2) group = ArrayView<const StringView>(&executable, 1);
    if (pass == 3) group = ArrayView<const StringView>(&platform, 1);
    for (const StringView& s : group) {
      if (!space.write_privileged(cursor, s.bytes()) || !space.write_privileged(cursor + s.size(), ConstByteView(&nul, 1)))
        return LoadStatus::OutOfMemory;
      cursor += s.size() + 1;
    }
  }
  const uint64_t random_address = (strings - 16) & ~uint64_t{15};
  if (strings < 16 || !space.write_privileged(random_address, random)) return LoadStatus::OutOfMemory;

  struct Aux { uint64_t key, value; };
  const Aux aux[] = {{3, image.phdr}, {4, image.phent}, {5, image.phnum}, {6, AddressSpace::kPageSize}, {7, 0}, {8, 0}, {9, image.entry}, {11, 0}, {12, 0},
                     {13, 0}, {14, 0}, {15, platform_at}, {16, 0}, {17, 100}, {23, 0}, {25, random_address}, {26, 0}, {31, executable_at}, {0, 0}};
  const uint64_t words = 1 + arguments.size() + 1 + environment.size() + 1 + 2 * (sizeof(aux) / sizeof(aux[0]));
  if (words * 8 > random_address) return LoadStatus::Malformed;
  const uint64_t sp = (random_address - words * 8) & ~uint64_t{15};
  uint64_t at = sp;
  bool ok = put_word(space, at, arguments.size());
  at += 8;
  uint64_t string_at = strings;
  for (const StringView& s : arguments) { ok = ok && put_word(space, at, string_at); at += 8; string_at += s.size() + 1; }
  ok = ok && put_word(space, at, 0); at += 8;
  for (const StringView& s : environment) { ok = ok && put_word(space, at, string_at); at += 8; string_at += s.size() + 1; }
  ok = ok && put_word(space, at, 0); at += 8;
  for (const Aux& a : aux) { ok = ok && put_word(space, at, a.key) && put_word(space, at + 8, a.value); at += 16; }
  if (!ok) return LoadStatus::OutOfMemory;      // the stack mapping is too small
  *stack_pointer = sp;
  return LoadStatus::Ok;
}

uint64_t windows_initial_stack_pointer(uint64_t stack_top) { return ((stack_top - 0x28) & ~uint64_t{15}) | 8; }

}  // namespace process
