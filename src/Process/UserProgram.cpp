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
#include "Process/UserProgram.hpp"
#include "Executable/ElfFile.hpp"
#include "Executable/ExecutableKind.hpp"
#include "Executable/PeFile.hpp"

namespace process {

LoadStatus UserProgram::load(ConstByteView file, ArrayView<const StringView> arguments, ArrayView<const StringView> environment, ConstByteView random) {
  if (process_) return LoadStatus::Malformed;      // already loaded
  const exe::ExecutableKind kind = exe::identify_executable(file.at_most(exe::kIdentifyBytes));
  ProcessBinding binding;
  LoadedImage image;
  uint64_t stack_top = 0, stack_pointer = 0, heap_start = 0;
  if (kind == exe::ExecutableKind::Elf) {
    exe::ElfFile elf(allocator_);
    if (!elf.parse(file)) return LoadStatus::Malformed;
    if (!bind_executable(elf, &binding)) return LoadStatus::Unsupported;
    LoadStatus status = load_elf(elf, file, space_, &image);
    if (status == LoadStatus::Ok) status = map_stack(space_, kDefaultStackBytes, &stack_top);
    if (status == LoadStatus::Ok) status = build_linux_stack(space_, stack_top, image, arguments, environment, random, &stack_pointer);
    if (status != LoadStatus::Ok) return status;
    if (!AddressSpace::round_up(image.end, &heap_start)) return LoadStatus::Malformed;
  } else if (kind == exe::ExecutableKind::Pe) {
    exe::PeFile pe(allocator_);
    if (!pe.parse(file)) return LoadStatus::Malformed;
    if (!bind_executable(pe, &binding)) return LoadStatus::Unsupported;
    LoadStatus status = load_pe(pe, file, space_, &image);
    if (status == LoadStatus::Ok) status = map_stack(space_, kDefaultStackBytes, &stack_top);
    if (status != LoadStatus::Ok) return status;
    stack_pointer = windows_initial_stack_pointer(stack_top);
  } else {
    return LoadStatus::NotExecutable;
  }
  UniquePointer<Process> process = UniquePointer<Process>::make(allocator_, id_, binding, space_, host_);
  if (!process) return LoadStatus::OutOfMemory;
  process->attach_address_space(space_, heap_start);
  start_ = ProgramStart{binding.arch, image.entry, stack_pointer, 0};
  image_ = image;
  process_ = static_cast<UniquePointer<Process>&&>(process);
  return LoadStatus::Ok;
}

}  // namespace process
