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
#include "Allocators/IAllocator.hpp"
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/Bus/Usb/Ehci/EhciRegisters.hpp"
#include "hardware/Bus/Usb/IUsbHostController.hpp"
#include "uefi.hpp"

namespace hardware {

// Polling EHCI driver for USB 2.0 controllers. initialize() takes the controller
// from the firmware, resets and starts it, routes every port to it and, for each
// high-speed device on a root port, resets the port, assigns an address and reads
// its descriptors. Low- and full-speed devices are handed to the companion
// controller (which this OS has no driver for). Devices behind hubs are not
// enumerated.
//
// Control and bulk transfers use the asynchronous schedule (one queue head per
// endpoint, linked in once and left there); interrupt endpoints use the
// periodic schedule, polled once per frame. Everything is polled, interrupts
// stay off. shutdown() resets the controller and hands it back to the firmware.
class EhciController final : public IUsbHostController {
 public:
  static constexpr uint32_t kMaxPorts = 15;

  EhciController(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                 efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator), st_(st) {}

  const char* name() const override { return "EHCI controller"; }
  bool initialize() override;
  void shutdown() override;

  UsbControllerKind kind() const override { return UsbControllerKind::Ehci; }
  uint32_t port_count() const override { return ports_; }
  bool port_info(uint32_t port, UsbPortInfo* out) const override;
  bool device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const override;
  bool configuration_descriptor(uint32_t port, uint8_t* buffer, uint16_t capacity, uint16_t* length) const override;
  bool set_configuration(uint32_t port, uint8_t configuration) override;
  bool control_out(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index) override;
  bool control_in(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                  void* buffer, uint16_t length) override;
  bool start_interrupt_in(uint32_t port, uint8_t endpoint_address, uint16_t max_packet, uint8_t interval) override;
  bool poll_interrupt_in(uint32_t port, uint8_t endpoint_address, void* buffer, uint16_t capacity,
                         uint16_t* length) override;
  bool start_bulk_endpoint(uint32_t port, uint8_t endpoint_address, uint16_t max_packet) override;
  bool bulk_transfer(uint32_t port, uint8_t endpoint_address, void* data, uint32_t length,
                     uint32_t* transferred) override;
  bool reset_bulk_endpoint(uint32_t port, uint8_t endpoint_address) override;

 private:
  static constexpr uint32_t kMaxEndpoints = 8;
  static constexpr uint32_t kPoolQtds = 8;
  static constexpr uint32_t kFrameListEntries = 1024;
  static constexpr uint32_t kChunkBytes = 16384;  // always fits five pages

  struct Endpoint {
    bool active = false;
    bool interrupt = false;
    uint8_t address = 0;  // USB endpoint address
    uint16_t max_packet = 0;
    ehci::Qh* qh = nullptr;
    ehci::Qtd* qtd = nullptr;  // interrupt endpoints: the one outstanding transfer
    uint8_t* buffer = nullptr;
  };

  struct Device {
    bool present = false;
    uint8_t address = 0;
    ehci::Qh* control_qh = nullptr;
    UsbDeviceDescriptor descriptor{};
    uint8_t config[256] = {};
    uint16_t config_length = 0;
    uint8_t configuration = 0;
    Endpoint endpoints[kMaxEndpoints];
  };

  // Bring-up.
  bool take_ownership();
  bool reset();
  bool setup_memory();
  bool start();

  // Ports and devices.
  uint64_t port_register(uint32_t port) const { return op_ + ehci::kOpPortSc + uint64_t{port} * 4; }
  bool reset_port(uint32_t port);
  bool enumerate_port(uint32_t port);
  void release_device(Device& d);

  // Queue heads and transfer descriptors.
  ehci::Qh* make_queue_head(uint8_t address, uint8_t endpoint, uint16_t max_packet, bool control);
  void link_async(ehci::Qh* qh);
  void link_periodic(ehci::Qh* qh);
  void free_queue_head(ehci::Qh* qh);
  void recover_queue_head(ehci::Qh* qh);
  void fill_qtd(ehci::Qtd* qtd, uint32_t pid, uint64_t data, uint32_t bytes, uint32_t toggle, bool interrupt_on_complete);
  bool run(ehci::Qh* qh, ehci::Qtd* first, uint32_t count, uint32_t timeout_ms, uint32_t* remaining);
  bool control(Device& d, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint16_t length,
               bool data_in);
  Endpoint* find_endpoint(Device& d, uint8_t address);

  bool reachable(const void* p, uint64_t length) const;
  void delay_us(uint64_t us) const;
  bool wait_for(uint64_t address, uint32_t mask, uint32_t value, uint32_t timeout_ms);

  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;

  uint64_t cap_ = 0, op_ = 0;
  uint32_t ports_ = 0;
  bool address64_ = false;
  bool owns_controller_ = false;
  bool running_ = false;

  ehci::Qh* async_head_ = nullptr;
  ehci::Qtd* pool_ = nullptr;  // kPoolQtds descriptors for the one synchronous transfer in flight
  uint32_t* frame_list_ = nullptr;
  ehci::Qh* periodic_first_ = nullptr;
  uint8_t* buffer_ = nullptr;  // control transfers: setup packet at 0, data from 64
  Device devices_[kMaxPorts];
};

}  // namespace hardware
