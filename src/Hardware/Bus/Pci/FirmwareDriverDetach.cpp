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

#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Kernel/Firmware.hpp"

namespace hardware {

namespace {
enum class Action { Disconnect, Connect };

// Runs `action` on every firmware handle that represents this PCI function.
bool for_each_matching_handle(efi::SystemTable* st, PciAddress address, Action action) {
  efi::BootServices* bs = st ? st->boot_services : nullptr;
  if (!bs) return false;

  efi::Uintn count = 0;
  efi::Handle* handles = nullptr;
  if (bs->locate_handle_buffer(efi::LocateSearchType::ByProtocol, &efi::kPciIoGuid, nullptr,
                               &count, &handles) != efi::kSuccess)
    return true;  // no PCI I/O handles at all: nothing is bound

  bool ok = true;
  for (efi::Uintn i = 0; i < count; ++i) {
    efi::PciIo* io = nullptr;
    if (bs->handle_protocol(handles[i], &efi::kPciIoGuid, reinterpret_cast<void**>(&io)) !=
        efi::kSuccess)
      continue;
    efi::Uintn segment, bus, device, function;
    if (io->get_location(io, &segment, &bus, &device, &function) != efi::kSuccess) continue;
    if (segment != 0 || bus != address.bus || device != address.device || function != address.function)
      continue;
    const efi::Status status =
        action == Action::Disconnect
            // Disconnects every driver (and their child handles) from this controller.
            ? bs->disconnect_controller(handles[i], nullptr, nullptr)
            // Lets the firmware pick drivers again, recursively through child devices.
            : bs->connect_controller(handles[i], nullptr, nullptr, 1);
    if (status != efi::kSuccess) ok = false;
  }
  bs->free_pool(handles);
  return ok;
}
}  // namespace

bool detach_firmware_drivers(efi::SystemTable* st, PciAddress address) {
  bool ok = false;
  kernel::firmware([&] { ok = for_each_matching_handle(st, address, Action::Disconnect); });
  return ok;
}

bool reattach_firmware_drivers(efi::SystemTable* st, PciAddress address) {
  bool ok = false;
  kernel::firmware([&] { ok = for_each_matching_handle(st, address, Action::Connect); });
  return ok;
}

}  // namespace hardware
