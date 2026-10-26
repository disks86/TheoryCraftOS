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
};

// Where a position-independent ELF is put (a fixed address is its own, so the first program is always loaded alike).
constexpr uint64_t kElfPieBase = 0x0000555555554000ull;

// Maps the loadable segments of a static ELF64 program into `space` and copies their file contents; the rest of each segment (.bss) stays zero. A
// program that asks for an interpreter (a dynamically linked one) is Unsupported until the dynamic loader exists. Segments that share a page are mapped
// together with the union of their permissions. On failure `space` may hold part of the image: discard it.
LoadStatus load_elf(const exe::ElfFile& elf, ConstByteView file, AddressSpace& space, LoadedImage* out);
// Maps the headers and sections of a PE32+ executable at its preferred image base (the space is the program's own, so the base is always free: no
// relocation is needed). Imports are not resolved here. On failure `space` may hold part of the image: discard it.
LoadStatus load_pe(const exe::PeFile& pe, ConstByteView file, AddressSpace& space, LoadedImage* out);

// The mapping step of load_pe() at an address of the caller's choosing, for DLLs too (an entry point is then optional unless `require_entry`). Does not apply
// relocations or bind imports (see ModuleLoader.hpp). The headers and sections are mapped at base + their RVAs.
LoadStatus map_pe_image(const exe::PeFile& pe, ConstByteView file, AddressSpace& space, uint64_t base, bool require_entry, LoadedImage* out);

constexpr uint64_t kDefaultStackBytes = 8ull << 20;
// Maps a read-write stack of `size` bytes just under the top of the user space, with an unmapped guard page above it. `*top` is the first address
// past the stack (the initial stack pointer is below it).
LoadStatus map_stack(AddressSpace& space, uint64_t size, uint64_t* top);

constexpr uint64_t kMaxStartupStrings = 128 * 1024;      // bytes of arguments and environment together
// Builds the System V startup block on the stack: argc, the argv and envp pointers, and the auxiliary vector (program headers, entry, page size,
// clock ticks, the program's path and platform name, random bytes). `random` supplies the 16 bytes of AT_RANDOM. `*stack_pointer` is the value for the program's stack pointer: 16-byte aligned, pointing at argc.
LoadStatus build_linux_stack(AddressSpace& space, uint64_t stack_top, const LoadedImage& image, ArrayView<const StringView> arguments,
                             ArrayView<const StringView> environment, StringView executable, ConstByteView random, uint64_t* stack_pointer);
// The stack pointer a Windows thread starts with: 16-byte aligned plus 8 (as if just called), with the 32-byte home space above it.
uint64_t windows_initial_stack_pointer(uint64_t stack_top);

}  // namespace process
