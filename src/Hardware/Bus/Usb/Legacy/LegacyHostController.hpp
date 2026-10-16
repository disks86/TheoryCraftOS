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
#include "Kernel/Delay.hpp"
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Kernel/Sync.hpp"
#include "uefi.hpp"

namespace hardware {

// What UHCI and OHCI have in common: both are USB 1.1 controllers (low and full
// speed) whose root ports each carry at most one device, enumerated the same way.
// The base does the enumeration sequence and the control-request API; a derived
// controller supplies the pipe handling and the root-port logic.
class LegacyHostController : public IUsbHostController {
 public:
  bool device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const override;
  bool configuration_descriptor(uint32_t port, ByteView buffer, uint16_t* length) const override;
  bool set_configuration(uint32_t port, uint8_t configuration) override;
  bool control_out(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index) override;
  bool control_in(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                  ByteView buffer) override;

 protected:
  struct DeviceState {
    bool present = false;
    bool low_speed = false;
    uint8_t address = 0;
    uint16_t max_packet0 = 8;
    UsbDeviceDescriptor descriptor{};
    uint8_t config[256] = {};
    uint16_t config_length = 0;
    uint8_t configuration = 0;
  };

  LegacyHostController(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                       efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator), st_(st) {}
  ~LegacyHostController() = default;

  // The device slot of a root port, or null if the port does not exist.
  virtual DeviceState* state(uint32_t port) const = 0;
  // One control transfer on the device's default pipe. With `data_in`, the data
  // stage reads data.size() bytes (at most 4096) into `data`; otherwise there is no
  // data stage and `data` is empty.
  virtual bool control_transfer(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                                ByteView data, bool data_in) = 0;
  // Creates the default pipe at address 0 and the state's max_packet0.
  virtual bool open_control_pipe(uint32_t port) = 0;
  virtual void update_control_pipe(uint32_t port, uint8_t address, uint16_t max_packet) = 0;
  // Throws away everything the controller holds for a device.
  virtual void close_device(uint32_t port) = 0;

  // Address 0 -> descriptor -> SET_ADDRESS -> descriptors. True if the device is usable.
  bool enumerate_device(uint32_t port, bool low_speed);
  void delay_us(uint64_t us) const { hardware::delay_us(us); }

  kernel::Mutex lock_;  // one thread in the controller at a time
  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
};

}  // namespace hardware
