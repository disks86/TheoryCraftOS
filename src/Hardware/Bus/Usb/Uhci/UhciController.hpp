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
#include "Hardware/Bus/Usb/Legacy/LegacyHostController.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

// Polling UHCI driver (USB 1.1, Intel's design: I/O-port registers, a frame list
// of 1024 entries, queue heads and transfer descriptors). Every endpoint gets a
// queue head that stays linked into one chain that every frame starts at, so all
// endpoints are serviced each millisecond; transfers run depth-first so a long
// bulk transfer is not limited to one packet per frame. Data toggles are tracked
// in software (UHCI has no hardware toggle). All structures and data buffers
// must sit below 4 GiB; buffers above that are bounced.
//
// Short IN packets: a multi-packet IN transfer that ends early (the device sends
// a short packet before the requested length) is not detected and times out;
// the lengths this OS asks for are exact.
class UhciController final : public LegacyHostController {
 public:
  static constexpr uint32_t kMaxPorts = 8;

  UhciController(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                 efi::SystemTable* st)
      : LegacyHostController(pci, device, allocator, st) {}

  const char* name() const override { return "UHCI controller"; }
  bool initialize() override;
  void shutdown() override;

  UsbControllerKind kind() const override { return UsbControllerKind::Uhci; }
  uint32_t port_count() const override { return ports_; }
  bool port_info(uint32_t port, UsbPortInfo* out) const override;
  bool start_interrupt_in(uint32_t port, uint8_t endpoint_address, uint16_t max_packet, uint8_t interval) override;
  bool poll_interrupt_in(uint32_t port, uint8_t endpoint_address, ByteView buffer, uint16_t* length) override;
  bool start_bulk_endpoint(uint32_t port, uint8_t endpoint_address, uint16_t max_packet) override;
  bool bulk_transfer(uint32_t port, uint8_t endpoint_address, ByteView data, uint32_t* transferred) override;
  bool reset_bulk_endpoint(uint32_t port, uint8_t endpoint_address) override;

 protected:
  DeviceState* state(uint32_t port) const override;
  bool control_transfer(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                        ByteView data, bool data_in) override;
  bool open_control_pipe(uint32_t port) override;
  void update_control_pipe(uint32_t port, uint8_t address, uint16_t max_packet) override;
  void close_device(uint32_t port) override;

 private:
  struct Td {
    uint32_t link, status, token, buffer;
  };
  struct Qh {
    uint32_t link, element, padding[2];
  };
  struct Pipe {
    bool active = false;
    bool interrupt = false;
    uint8_t address = 0;  // USB endpoint address
    uint8_t toggle = 0;
    uint16_t max_packet = 8;
    Qh* qh = nullptr;
    Td* td = nullptr;          // interrupt pipes: the one outstanding transfer
    ByteView buffer;
  };
  struct Device : DeviceState {
    static constexpr uint32_t kPipes = 6;
    Pipe control;
    Pipe pipes[kPipes];
  };

  static constexpr uint32_t kPoolTds = 640;
  static constexpr uint32_t kBatchTds = 512;

  uint16_t read16(uint16_t reg) const;
  void write16(uint16_t reg, uint16_t value) const;
  uint16_t port_status(uint32_t port) const { return read16(static_cast<uint16_t>(0x10 + 2 * port)); }
  void set_port(uint32_t port, uint16_t value) const { write16(static_cast<uint16_t>(0x10 + 2 * port), value); }
  bool reset_port(uint32_t port, bool* low_speed);

  template <class T>
  ArrayView<T> low_allocate(uint64_t count, uint64_t alignment);
  Qh* make_queue_head();
  void link_queue_head(Qh* qh);
  void unlink_queue_head(Qh* qh);
  void free_pipe(Pipe& p);
  void fill_td(Td* td, uint32_t pid, uint8_t device_address, uint8_t endpoint, uint32_t toggle, bool low_speed,
               ConstByteView buffer);
  bool run(Pipe& pipe, ArrayView<Td> descriptors, uint32_t timeout_ms);
  Pipe* find_pipe(Device& d, uint8_t address);

  uint16_t io_ = 0;
  uint32_t ports_ = 0;
  bool owns_controller_ = false;
  bool running_ = false;

  ArrayView<uint32_t> frame_list_;
  Qh* head_ = nullptr;
  ArrayView<Td> pool_;
  ByteView control_buffer_;  // setup packet at 0, data from 64
  ByteView bounce_;          // 64 KiB below 4 GiB
  Device devices_[kMaxPorts];
};

}  // namespace hardware
