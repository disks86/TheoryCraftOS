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
#include "Executable/ElfFile.hpp"
#include "Executable/PeFile.hpp"
#include "Process/AddressSpace.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

namespace process {

enum class LoadStatus : uint8_t {
  Ok,
  NotExecutable,    // not an ELF or PE file at all
  Malformed,        // the headers contradict the file or each other (a segment outside the file, overlapping segments, an entry point outside the code)
  Unsupported,      // a valid file this loader does not handle yet (a 32-bit PE)
  NeedsLibraries,   // it links against libraries: a dynamic ELF asks for an interpreter, a PE imports DLLs, and the OS has neither yet
  OutOfMemory,      // the address space's byte budget or the allocator ran out
};

// Where an image ended up.
struct LoadedImage {
  uint64_t base = 0;        // the lowest mapped address of the image
  uint64_t end = 0;         // one past the highest (where a Linux heap begins)
  uint64_t entry = 0;       // the first instruction
  uint64_t phdr = 0;        // ELF: where the program header table is in memory (0 if no segment holds it)
  uint64_t phent = 0, phnum = 0;
  uint64_t program_entry = 0;       // a dynamic program: its own entry (AT_ENTRY), while `entry` is the interpreter's; 0 otherwise
  uint64_t interpreter_base = 0;    // a dynamic program: where its interpreter (ld.so) is (AT_BASE); 0 otherwise
};

// Where a position-independent ELF is put (a fixed address is its own, so the first program is always loaded alike).
constexpr uint64_t kElfPieBase = 0x0000555555554000ull;
// Where a dynamic program's interpreter (the ld.so its PT_INTERP names) is put, and how much room it has: below the mmap area (Process's mmap cursor
// starts at 0x7F0000000000), so the libraries it maps never land on it.
constexpr uint64_t kElfInterpreterBase = 0x00007E0000000000ull;
constexpr uint64_t kElfInterpreterSpan = 0x0000000100000000ull;

// Maps the loadable segments of a static ELF64 program into `space` and copies their file contents; the rest of each segment (.bss) stays zero. A
// program that asks for an interpreter (a dynamically linked one) is NeedsLibraries here: see load_dynamic_elf(). Segments that share a page are mapped
// together with the union of their permissions. On failure `space` may hold part of the image: discard it.
LoadStatus load_elf(const exe::ElfFile& elf, ConstByteView file, AddressSpace& space, LoadedImage* out);
// A dynamically linked program, as Linux starts one: the program is mapped as load_elf() would, its interpreter (`interpreter`, the file its PT_INTERP
// names) at kElfInterpreterBase, and the image's entry is the interpreter's. The interpreter, which runs first, loads the libraries itself with open and
// mmap; it finds the program by the auxiliary vector (AT_PHDR, AT_ENTRY = program_entry, AT_BASE = interpreter_base). Malformed if the interpreter is not
// a shared object for the same CPU, or asks for an interpreter itself.
LoadStatus load_dynamic_elf(const exe::ElfFile& elf, ConstByteView file, const exe::ElfFile& interpreter, ConstByteView interpreter_file, AddressSpace& space,
                            LoadedImage* out);
// Maps the headers and sections of a PE32+ executable at its preferred image base (the space is the program's own, so the base is always free: no
// relocation is needed). Imports are not resolved here. On failure `space` may hold part of the image: discard it.
// `require_entry` false lets a program without an entry point load (it is created; running it would fault, as on Windows).
LoadStatus load_pe(const exe::PeFile& pe, ConstByteView file, AddressSpace& space, LoadedImage* out, bool require_entry = true);

// SECTION_IMAGE_INFORMATION (64 bytes) of a PE image as NtQuerySection and NtQueryInformationProcess(ProcessImageInformation) report it. `file_size` is the size of the file.
// Windows' idea of whether an image has code (SizeOfCode, an entry point, an odd section alignment or a lone executable section). A 32-bit image without any is only data, which a
// 64-bit process may map as a library.
bool pe_image_contains_code(const exe::PeFile& pe);
void describe_pe_image(const exe::PeFile& pe, uint64_t file_size, uint8_t (&out)[64], ConstByteView file = ConstByteView());

// The mapping step of load_pe() at an address of the caller's choosing, for DLLs too (an entry point is then optional unless `require_entry`). Does not apply
// relocations or bind imports (see ModuleLoader.hpp). The headers and sections are mapped at base + their RVAs.
LoadStatus map_pe_image(const exe::PeFile& pe, ConstByteView file, AddressSpace& space, uint64_t base, bool require_entry, LoadedImage* out);
// Where map_pe_image() puts each section that has a size: its RVA and span, its permissions, and the bytes of the file copied to its start (`copy` bytes from `source`;
// the rest is zero). False if a section lies outside the image or the file, or there are more than `capacity`.
struct PeSectionPlacement {
  uint64_t rva = 0, span = 0, source = 0, copy = 0;
  uint8_t protection = 0;
};
bool place_pe_sections(const exe::PeFile& pe, ConstByteView file, uint64_t image_pages, PeSectionPlacement* out, uint32_t capacity, uint32_t* count);

constexpr uint64_t kDefaultStackBytes = 8ull << 20;
// Maps a read-write stack of `size` bytes just under the top of the user space, with an unmapped guard page above it. `*top` is the first address
// past the stack (the initial stack pointer is below it).
LoadStatus map_stack(AddressSpace& space, uint64_t size, uint64_t* top);

constexpr uint64_t kMaxStartupStrings = 128 * 1024;      // bytes of arguments and environment together
// Builds the System V startup block on the stack: argc, the argv and envp pointers, and the auxiliary vector (program headers, entry, page size,
// clock ticks, the program's path and platform name ("x86_64", "aarch64"), random bytes). `random` supplies the 16 bytes of AT_RANDOM. `*stack_pointer` is the value for the program's stack pointer: 16-byte aligned, pointing at argc.
LoadStatus build_linux_stack(AddressSpace& space, uint64_t stack_top, const LoadedImage& image, ArrayView<const StringView> arguments,
                             ArrayView<const StringView> environment, StringView executable, ConstByteView random, uint64_t* stack_pointer,
                             StringView platform = "x86_64");
// The stack pointer a Windows thread starts with: 16-byte aligned plus 8 (as if just called), with the 32-byte home space above it.
uint64_t windows_initial_stack_pointer(uint64_t stack_top);

}  // namespace process
