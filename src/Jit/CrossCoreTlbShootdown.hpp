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
#include "Jit/KernelCodeMemory.hpp"
#include "Kernel/Paging.hpp"

namespace jit {

// The kernel's TLB shootdown (kernel::PageTables::set_shootdown(), or a KernelCodeMemory of its own): runs a flush of the range on every online core through the scheduler's run_on_every_core (an inter-processor
// interrupt to each other core, waiting until each has done it). Small ranges are flushed page by page (INVLPG), large ones with a full flush. Thread
// context only, and not from inside a spin lock; before the scheduler runs it simply flushes this core.
class CrossCoreTlbShootdown final : public ITlbShootdown {
 public:
  explicit CrossCoreTlbShootdown(kernel::IPagingHost& host) : host_(host) {}
  bool invalidate_range(uint64_t address, uint64_t bytes) override;

 private:
  static constexpr uint64_t kPageByPageLimit = 32;   // pages
  kernel::IPagingHost& host_;
};

}  // namespace jit
