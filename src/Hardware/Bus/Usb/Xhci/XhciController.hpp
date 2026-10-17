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
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Usb/Xhci/IXhciController.hpp"
#include "Hardware/Bus/Usb/Xhci/XhciRing.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/UniquePointer.hpp"
#include "uefi.hpp"

namespace hardware {

// Polling xHCI driver. initialize() takes the controller from the firmware,
// resets and starts it, then for every connected root port: resets the port,
// enables a device slot, addresses the device and reads its device descriptor.
// Devices stay addressed (slot and EP0 ring kept) for later class drivers.
// shutdown() resets the controller and hands it back to the firmware. Interrupts
// are not used; every wait polls the event ring.
class XhciController final : public IXhciController {
 public:
  static constexpr uint32_t kMaxPorts = 32;

  XhciController(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                 efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator), st_(st) {}

  const char* name() const override { return "xHCI controller"; }
  bool initialize() override;
  void shutdown() override;

  uint16_t interface_version() const override { return version_; }
  uint32_t max_device_slots() const override { return max_slots_; }
  bool uses_64_byte_contexts() const override { return context_size_ == 64; }

  uint32_t port_count() const override { return ports_; }
  bool port_info(uint32_t port, UsbPortInfo* out) const override;
  bool device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const override;
  bool configuration_descriptor(uint32_t port, ByteView buffer, uint16_t* length) const override;
  bool set_configuration(uint32_t port, uint8_t configuration) override;
  bool control_out(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                   uint16_t index) override;
  bool control_in(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                  uint16_t index,
                  ByteView buffer) override;
  bool control_out_data(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, ConstByteView data) override;
  bool start_interrupt_in(uint32_t port, uint8_t endpoint_address, uint16_t max_packet,
                          uint8_t interval) override;
  bool poll_interrupt_in(uint32_t port, uint8_t endpoint_address, ByteView buffer, uint16_t* length) override;
  bool start_bulk_endpoint(uint32_t port, uint8_t endpoint_address, uint16_t max_packet) override;
  bool bulk_transfer(uint32_t port, uint8_t endpoint_address, ByteView data, uint32_t* transferred) override;
  bool reset_bulk_endpoint(uint32_t port, uint8_t endpoint_address) override;
  bool supports_hotplug() const override { return true; }
  bool poll_hotplug(UsbHotplugEvent* event) override;
  uint32_t port_generation(uint32_t port) const override;
  bool supports_isochronous() const override { return true; }
  bool start_iso_endpoint(uint32_t port, uint8_t endpoint_address, uint16_t max_packet, uint8_t interval) override;
  void stop_iso_endpoint(uint32_t port, uint8_t endpoint_address) override;
  uint32_t iso_interval_us(uint32_t port, uint8_t endpoint_address) override;
  uint32_t iso_free_packets(uint32_t port, uint8_t endpoint_address) override;
  bool iso_submit(uint32_t port, uint8_t endpoint_address, ConstByteView packet) override;
  uint64_t iso_completed_packets(uint32_t port, uint8_t endpoint_address) override;
  bool iso_receive(uint32_t port, uint8_t endpoint_address, ByteView out, uint16_t* length) override;

 private:
  struct InterruptEndpoint {
    bool active = false;
    uint8_t address = 0;      // USB endpoint address (0x81 = endpoint 1 IN)
    uint8_t context_id = 0;   // device context index used for doorbells
    uint16_t max_packet = 0;
    xhci::ProducerRing ring;
    ByteView buffer;
    uint64_t pending_trb = 0;  // the TRB the controller is working on
    bool ready = false;        // a packet is waiting in buffer
    uint16_t received = 0;
  };

  struct BulkEndpoint {
    bool active = false;
    uint8_t address = 0;
    uint8_t context_id = 0;
    xhci::ProducerRing ring;
  };

  // One isochronous endpoint: a ring of transfer descriptors of one packet each, and a buffer with one slot per descriptor. OUT: slots from head to
  // tail are queued and not yet sent. IN: every slot is armed or holds a packet nobody has taken; head is the oldest.
  struct IsoEndpoint {
    static constexpr uint32_t kSlots = 128;
    bool in = false;
    uint8_t address = 0;
    uint8_t context_id = 0;
    uint16_t max_packet = 0;
    uint32_t interval_us = 0;
    xhci::ProducerRing ring;
    ByteView buffer;           // kSlots * max_packet
    uint16_t length[kSlots] = {};
    uint64_t trb[kSlots] = {};
    bool done[kSlots] = {};
    uint32_t head = 0, tail = 0, count = 0;
    uint64_t completed = 0, errors = 0;
  };

  struct Device {
    static constexpr uint32_t kMaxEndpoints = 4;
    static constexpr uint32_t kMaxBulkEndpoints = 4;
    static constexpr uint32_t kMaxIsoEndpoints = 3;   // audio out, audio in, and a feedback endpoint
    bool present = false;
    uint8_t slot = 0;
    uint32_t speed_id = 0;
    uint16_t max_packet0 = 0;
    ByteView output_context;  // 33 contexts each (input: control + slot + 31 endpoints)
    ByteView input_context;
    xhci::ProducerRing control_ring;
    UsbDeviceDescriptor descriptor{};
    uint8_t config[1024] = {};   // the first configuration (audio devices have long ones)
    uint16_t config_length = 0;
    uint8_t configuration = 0;   // value passed to SET_CONFIGURATION, 0 = none yet
    uint8_t context_entries = 1;  // highest endpoint context index in use
    InterruptEndpoint endpoints[kMaxEndpoints];
    BulkEndpoint bulk[kMaxBulkEndpoints];
    UniquePointer<IsoEndpoint> iso[kMaxIsoEndpoints];
  };

  // Bring-up steps.
  bool take_ownership();
  bool reset();
  bool setup_memory();
  bool start();

  // Command ring / event ring.
  bool run_command(uint64_t parameter, uint32_t control, xhci::Trb* completion);
  bool wait_event(uint32_t type, uint64_t pointer, xhci::Trb* out, uint32_t timeout_ms);
  void ring_doorbell(uint32_t slot, uint32_t target);

  // Device enumeration.
  bool reset_port(uint32_t port);
  bool enumerate_port(uint32_t port);
  bool address_device(uint32_t port, Device& d);
  bool update_max_packet(Device& d, uint16_t max_packet);
  bool transfer(Device& d, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                uint16_t length, bool data_in, const uint8_t* out_data = nullptr);
  IsoEndpoint* find_iso(uint32_t port, uint8_t endpoint_address);
  void arm_iso_slot(Device& d, IsoEndpoint& e, uint32_t slot);
  void handle_iso_event(IsoEndpoint& e, const xhci::Trb& event);
  void release_iso(Device& d, IsoEndpoint& e);
  void handle_event(const xhci::Trb& event);
  void pump_events();
  void arm_endpoint(Device& d, InterruptEndpoint& e);
  void release_device(Device& d);

  bool dma_reachable(const void* p) const;
  uint64_t port_register(uint32_t port) const;
  bool wait_for(uint64_t address, uint32_t mask, uint32_t value, uint32_t timeout_ms);
  void delay_us(uint64_t us) const;
  // Context number `index` of a device or input context, as its 32-bit words (checked).
  ArrayView<uint32_t> context_at(ByteView base, uint32_t index) const {
    return base.subview(uint64_t{index} * context_size_, context_size_).cast<uint32_t>();
  }

  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;

  uint64_t cap_ = 0, op_ = 0, runtime_ = 0, doorbells_ = 0;
  uint16_t version_ = 0;
  uint32_t max_slots_ = 0, ports_ = 0, context_size_ = 32;

  uint64_t* dcbaa_ = nullptr;
  uint64_t* scratchpad_table_ = nullptr;
  uint32_t scratchpad_count_ = 0;
  uint8_t* scratchpad_pages_[32] = {};
  xhci::ProducerRing command_ring_;
  xhci::EventRing event_ring_;
  ByteView buffer_;  // 4 KiB bounce buffer for control transfers
  mutable kernel::Mutex lock_;  // one thread in the controller at a time
  bool running_ = false;
  bool ac64_ = false;
  bool owns_controller_ = false;  // firmware drivers are detached

  Device devices_[kMaxPorts];
  uint32_t generation_[kMaxPorts] = {};     // bumped when a device on the port is enumerated or released
  bool pending_connect_[kMaxPorts] = {};    // enumerate the port on the next poll_hotplug() (a device that came back, or a failed attempt)
  uint8_t attempts_[kMaxPorts] = {};        // failed enumerations since the device appeared
  uint32_t hotplug_cursor_ = 0;
};

}  // namespace hardware
