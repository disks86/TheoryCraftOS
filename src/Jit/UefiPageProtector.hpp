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
#include "Jit/PageProtector.hpp"
#include "uefi.hpp"

namespace jit {

// IPageProtector through the firmware's EFI_MEMORY_ATTRIBUTE_PROTOCOL (UEFI 2.10), which changes the read-only / no-execute attributes in the firmware's own
// page tables. Not enforced if the firmware lacks the protocol. Firmware calls go through kernel::firmware(): on the boot core, one at a time.
class UefiPageProtector final : public IPageProtector {
 public:
  explicit UefiPageProtector(efi::SystemTable* system_table);
  bool enforced() const override { return protocol_ != nullptr; }
  bool protect(uint64_t base, uint64_t length, PageAccess access) override;

 private:
  efi::MemoryAttributeProtocol* protocol_ = nullptr;
};

}  // namespace jit
