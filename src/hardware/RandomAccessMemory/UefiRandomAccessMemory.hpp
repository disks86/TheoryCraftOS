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
#include "hardware/RandomAccessMemory/IRandomAccessMemory.hpp"
#include "uefi.hpp"

namespace hardware {

// System memory discovered through UEFI boot services. initialize() reads the
// firmware memory map for the total/usable figures and reserves one contiguous
// pool (up to kPoolBytes, from the largest free region) for the OS allocator.
class UefiRandomAccessMemory final : public IRandomAccessMemory {
 public:
  static constexpr uint64_t kPoolBytes = 64ull * 1024 * 1024;

  explicit constexpr UefiRandomAccessMemory(efi::SystemTable* st) : st_(st) {}

  const char* name() const override { return "UEFI RAM"; }
  bool initialize() override;
  void shutdown() override;

  uint64_t total_bytes() const override { return total_bytes_; }
  uint64_t usable_bytes() const override { return pool_bytes_; }
  void* base() const override { return reinterpret_cast<void*>(pool_base_); }

  // Free (conventional) memory the firmware reported, claimed or not.
  uint64_t free_bytes() const { return free_bytes_; }

 private:
  static constexpr uint64_t kPageSize = 4096;

  efi::SystemTable* st_;
  uint64_t total_bytes_ = 0;
  uint64_t free_bytes_ = 0;
  uint64_t pool_base_ = 0;
  uint64_t pool_bytes_ = 0;
};

}  // namespace hardware
