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
#include "Jit/UefiPageProtector.hpp"
#include "Kernel/Firmware.hpp"

namespace jit {

UefiPageProtector::UefiPageProtector(efi::SystemTable* system_table) {
  efi::MemoryAttributeProtocol* protocol = nullptr;
  if (system_table && system_table->boot_services &&
      system_table->boot_services->locate_protocol(&efi::kMemoryAttributeGuid, nullptr, reinterpret_cast<void**>(&protocol)) == efi::kSuccess && protocol &&
      protocol->set_memory_attributes && protocol->clear_memory_attributes)
    protocol_ = protocol;
}

bool UefiPageProtector::protect(uint64_t base, uint64_t length, PageAccess access) {
  if (!protocol_) return true;
  efi::MemoryAttributeProtocol* protocol = protocol_;
  // The order keeps W^X: to make code, "read-only" is set before "no-execute" is cleared; to make data, "no-execute" is set before "read-only" is cleared.
  const uint64_t set = access == PageAccess::ReadExecute ? efi::kMemoryRo : efi::kMemoryXp;
  const uint64_t clear = access == PageAccess::ReadExecute ? efi::kMemoryXp : efi::kMemoryRo;
  efi::Status status = efi::kSuccess;
  kernel::firmware([&] {
    status = protocol->set_memory_attributes(protocol, base, length, set);
    if (status == efi::kSuccess) status = protocol->clear_memory_attributes(protocol, base, length, clear);
  });
  return status == efi::kSuccess;
}

}  // namespace jit
