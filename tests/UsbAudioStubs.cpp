// What the PCI audio drivers (linked in so SystemAudioDevices can be tested as a whole) call into the machine for. The USB audio test has no
// PCI devices, so none of this runs.
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Hardware/Bus/Virtio/VirtioPciDevice.hpp"
#include "Hardware/Bus/Virtio/Virtqueue.hpp"
#include "Hardware/IoPort.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {
uint8_t IoPort::read8() const { return 0; }
void IoPort::write8(uint8_t) const {}
uint16_t IoPort::read16() const { return 0; }
void IoPort::write16(uint16_t) const {}
uint32_t IoPort::read32() const { return 0; }
void IoPort::write32(uint32_t) const {}
void delay_us(uint64_t) {}
bool detach_firmware_drivers(efi::SystemTable*, PciAddress) { return false; }
bool reattach_firmware_drivers(efi::SystemTable*, PciAddress) { return false; }
}
