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
#include "Jit/CodeMemory.hpp"
#include "Jit/CodePageSource.hpp"
#include "Jit/PageProtector.hpp"

namespace jit {

// ICodeMemory for the images that have no page-table code of their own (AArch64, RISC-V): pages from a code pool, W^X switched by an IPageProtector. A
// region is "writable, not executable" while it is filled and "read-only, executable" afterwards; the protector's ordering guarantee means it is never both.
// make_executable() synchronises the instruction caches of every core (flush_instruction_cache() on this one, then the optional IInstructionCacheSync).
//
// If the protector is not `enforced()` the regions are plain memory and nothing keeps data from being executed or code from being written.
class ProtectedCodeMemory final : public ICodeMemory {
 public:
  ProtectedCodeMemory(ICodePageSource& pages, IPageProtector& protector, IInstructionCacheSync* sync = nullptr) : pages_(pages), protector_(protector), sync_(sync) {}

  uint64_t page_size() const override { return kCodePage; }
  bool allocate(uint64_t size, CodeRegion* out) override;
  bool make_executable(const CodeRegion& region) override;
  bool make_writable(const CodeRegion& region) override;
  void release(const CodeRegion& region) override;
  bool can_execute() const override { return true; }
  bool protections_enforced() const { return protector_.enforced(); }

 private:
  bool protect(const CodeRegion& region, PageAccess access) {
    return protector_.protect(reinterpret_cast<uint64_t>(region.bytes.data()), region.bytes.size(), access);   // (identity mapped: the address is the physical address)
  }

  ICodePageSource& pages_;
  IPageProtector& protector_;
  IInstructionCacheSync* sync_;
};

}  // namespace jit
