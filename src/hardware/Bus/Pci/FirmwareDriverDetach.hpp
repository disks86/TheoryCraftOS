#pragma once
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "uefi.hpp"

namespace hardware {

// Asks the firmware to disconnect its driver(s) from a PCI function, so the OS
// can program the device without the firmware's driver (e.g. its USB stack)
// changing things underneath it. Returns true when no firmware driver is left
// bound to the function (including when none ever was).
bool detach_firmware_drivers(efi::SystemTable* st, PciAddress address);

// Lets the firmware bind its drivers to the function again (e.g. so its USB
// keyboard works for firmware console input once the OS is done with the
// controller). The device must be idle and reset.
bool reattach_firmware_drivers(efi::SystemTable* st, PciAddress address);

}  // namespace hardware
