#include "hardware/Bus/Pci/FirmwareDriverDetach.hpp"

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
  return for_each_matching_handle(st, address, Action::Disconnect);
}

bool reattach_firmware_drivers(efi::SystemTable* st, PciAddress address) {
  return for_each_matching_handle(st, address, Action::Connect);
}

}  // namespace hardware
