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

#include "hardware/RandomAccessMemory/UefiRandomAccessMemory.hpp"

namespace hardware {

bool UefiRandomAccessMemory::initialize() {
  efi::BootServices* bs = st_ ? st_->boot_services : nullptr;
  if (!bs) return false;

  // The map size is only known after a failed first call; allocating the
  // buffer can itself grow the map, so leave a few descriptors of slack.
  efi::Uintn map_size = 0, key = 0, desc_size = 0;
  uint32_t desc_version = 0;
  if (bs->get_memory_map(&map_size, nullptr, &key, &desc_size, &desc_version) !=
      efi::kBufferTooSmall)
    return false;
  map_size += 8 * desc_size;

  void* buffer = nullptr;
  if (bs->allocate_pool(efi::MemoryType::LoaderData, map_size, &buffer) != efi::kSuccess)
    return false;
  auto* map = static_cast<efi::MemoryDescriptor*>(buffer);
  if (bs->get_memory_map(&map_size, map, &key, &desc_size, &desc_version) != efi::kSuccess) {
    bs->free_pool(buffer);
    return false;
  }

  total_bytes_ = free_bytes_ = 0;
  uint64_t best_start = 0, best_pages = 0;
  for (efi::Uintn off = 0; off + desc_size <= map_size; off += desc_size) {
    auto* d = reinterpret_cast<efi::MemoryDescriptor*>(reinterpret_cast<uint8_t*>(map) + off);
    if (d->type < efi::kFirstRamType || d->type > efi::kLastRamType) continue;
    auto type = static_cast<efi::MemoryType>(d->type);
    total_bytes_ += d->number_of_pages * kPageSize;
    if (type == efi::MemoryType::ConventionalMemory) {
      free_bytes_ += d->number_of_pages * kPageSize;
      if (d->number_of_pages > best_pages) {
        best_pages = d->number_of_pages;
        best_start = d->physical_start;
      }
    }
  }
  bs->free_pool(buffer);

  uint64_t pages = best_pages;
  if (pages * kPageSize > kPoolBytes) pages = kPoolBytes / kPageSize;
  if (pages == 0) return false;

  uint64_t address = best_start;
  if (bs->allocate_pages(efi::AllocateType::Address, efi::MemoryType::LoaderData, pages,
                         &address) != efi::kSuccess)
    return false;
  pool_base_ = address;
  pool_bytes_ = pages * kPageSize;
  return true;
}

void UefiRandomAccessMemory::shutdown() {
  if (pool_base_ && st_ && st_->boot_services)
    st_->boot_services->free_pages(pool_base_, pool_bytes_ / kPageSize);
  pool_base_ = pool_bytes_ = 0;
}

}  // namespace hardware
