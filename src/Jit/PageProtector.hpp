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

namespace jit {

enum class PageAccess : uint8_t {
  ReadWrite,      // data: readable, writable, not executable
  ReadExecute,    // code: readable, executable, not writable
};

// Changes what the CPU allows on a range of identity-mapped memory, whatever the platform's way of doing that is (the firmware's memory attribute protocol
// on AArch64, the page tables the firmware left on RISC-V). A change from one access to the other must never pass through a state that is both writable and
// executable. Used by ProtectedCodeMemory for the images that have no page-table code of their own.
class IPageProtector {
 public:
  // True if protect() really changes the CPU's permissions. A protector that cannot (no paging, no firmware protocol) reports false and accepts every call:
  // the memory then simply has whatever access it has.
  virtual bool enforced() const = 0;
  // `base` and `length` are multiples of the 4 KiB page size. False if the range cannot be changed.
  virtual bool protect(uint64_t base, uint64_t length, PageAccess access) = 0;

 protected:
  constexpr IPageProtector() = default;
  ~IPageProtector() = default;
};

}  // namespace jit
