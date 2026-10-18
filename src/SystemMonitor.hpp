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

// The computer manager's view of the running machine: the kernel's threads and cores and the allocator's memory.
#pragma once
#include "Allocators/Allocator.hpp"
#include "Allocators/ThreadSafeAllocator.hpp"
#include "Desktop/ComputerManager.hpp"

class SystemMonitor final : public desktop::ISystemMonitor {
 public:
  // `shared` is the thread-safe front of `heap`; reading the heap's statistics takes its lock.
  SystemMonitor(allocators::Allocator& heap, allocators::ThreadSafeAllocator& shared) : heap_(heap), shared_(shared) {}

  uint32_t ticks_per_second() const override;
  uint32_t core_count() const override;
  bool core(uint32_t index, desktop::TaskCore* out) override;
  uint32_t threads(desktop::TaskThread* out, uint32_t capacity) override;
  desktop::TaskMemory memory() override;

 private:
  allocators::Allocator& heap_;
  allocators::ThreadSafeAllocator& shared_;
};
