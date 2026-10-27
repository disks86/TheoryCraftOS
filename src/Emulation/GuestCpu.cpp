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
#include "Emulation/GuestCpu.hpp"
#include "Emulation/AArch64Cpu.hpp"
#include "Emulation/RiscV64Cpu.hpp"
#include "Emulation/X86_64Cpu.hpp"
#include "Process/Process.hpp"

namespace emulation {

uint64_t GuestMemory::fetchable(uint64_t address, uint64_t limit) const {
  constexpr uint64_t kPage = process::AddressSpace::kPageSize;
  const uint64_t to_page_end = kPage - (address & (kPage - 1));
  const uint64_t first = limit < to_page_end ? limit : to_page_end;
  uint8_t probe[1];
  if (!fetch(address, ByteView(probe, 1))) return 0;
  // Regions are whole pages, so the first byte of the next page being fetchable means the rest of the bytes asked for on it are too.
  if (limit > first && fetch(address + first, ByteView(probe, 1))) return limit;
  return first;
}

UniquePointer<IGuestCpu> create_guest_cpu(process::UserArch guest, allocators::IAllocator& allocator) {
  if (guest == process::UserArch::X86_64) return UniquePointer<IGuestCpu>::make_as<X86_64Cpu>(allocator);
  if (guest == process::UserArch::AArch64) return UniquePointer<IGuestCpu>::make_as<AArch64Cpu>(allocator);
  if (guest == process::UserArch::RiscV64) return UniquePointer<IGuestCpu>::make_as<RiscV64Cpu>(allocator);
  return UniquePointer<IGuestCpu>();
}

bool can_emulate(process::UserArch guest) { return guest == process::UserArch::X86_64 || guest == process::UserArch::AArch64 || guest == process::UserArch::RiscV64; }

}  // namespace emulation
