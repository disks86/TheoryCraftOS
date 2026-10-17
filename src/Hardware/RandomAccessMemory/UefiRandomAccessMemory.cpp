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

#include "Hardware/RandomAccessMemory/UefiRandomAccessMemory.hpp"
#include "Utility/ArrayView.hpp"

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
  uint64_t best_start = 0, best_pages = 0;      // largest free region anywhere
  uint64_t low_start = 0, low_pages = 0;        // largest free region that ends below 4 GiB
  constexpr uint64_t kFourGiB = 1ull << 32;
  // The firmware's descriptors are desc_size bytes apart (more than sizeof(MemoryDescriptor)
  // on some firmware), so step through the map as bytes and take one entry at a time.
  const ConstByteView map_bytes(reinterpret_cast<const uint8_t*>(map), map_size);
  if (desc_size < sizeof(efi::MemoryDescriptor)) {
    bs->free_pool(buffer);
    return false;
  }
  for (uint64_t off = 0; off + desc_size <= map_bytes.size(); off += desc_size) {
    const ConstByteView entry = map_bytes.subview(off, desc_size);
    const auto* d = reinterpret_cast<const efi::MemoryDescriptor*>(entry.data());
    if (d->type < efi::kFirstRamType || d->type > efi::kLastRamType) continue;
    auto type = static_cast<efi::MemoryType>(d->type);
    total_bytes_ += d->number_of_pages * kPageSize;
    if (type == efi::MemoryType::ConventionalMemory) {
      free_bytes_ += d->number_of_pages * kPageSize;
      if (d->number_of_pages > best_pages) {
        best_pages = d->number_of_pages;
        best_start = d->physical_start;
      }
      if (d->physical_start + d->number_of_pages * kPageSize <= kFourGiB && d->number_of_pages > low_pages) {
        low_pages = d->number_of_pages;
        low_start = d->physical_start;
      }
    }
  }
  bs->free_pool(buffer);

  const bool use_low = low_pages > 0;
  uint64_t pages = use_low ? low_pages : best_pages;
  if (pages == 0) return false;
  // Boot services stay alive, and firmware drivers (the UEFI block and network fallbacks) allocate DMA
  // buffers below 4 GiB while we run: leave them the end of the region, 128 MiB or half of a small one.
  const uint64_t reserve_pages = (pages / 2 < kFirmwareReserve / kPageSize) ? pages / 2 : kFirmwareReserve / kPageSize;
  pages -= reserve_pages;
  if (pages == 0) return false;

  uint64_t address = use_low ? low_start : best_start;
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
