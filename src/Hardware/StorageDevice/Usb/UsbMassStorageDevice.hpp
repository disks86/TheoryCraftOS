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
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/StorageDevice/StorageDeviceBase.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// A USB mass-storage device (QEMU's usb-storage) speaking the Bulk-Only
// Transport: every SCSI command is a 31-byte command block wrapper out, an
// optional data stage, and a 13-byte status wrapper in. Runs on any host
// controller that offers bulk endpoints (xHCI here).
class UsbMassStorageDevice final : public StorageDeviceBase {
 public:
  UsbMassStorageDevice(IUsbHostController& host, uint32_t port, uint8_t interface_number, uint8_t in_endpoint,
                       uint16_t in_max_packet, uint8_t out_endpoint, uint16_t out_max_packet,
                       allocators::IAllocator& allocator, efi::SystemTable* st)
      : StorageDeviceBase(allocator, st), host_(host), port_(port), interface_(interface_number),
        in_(in_endpoint), out_(out_endpoint), in_max_packet_(in_max_packet), out_max_packet_(out_max_packet),
        generation_(host.port_generation(port)) {}

  // Adds a device for every mass-storage interface on every enumerated USB device.
  static void discover(IUsbBus& usb, allocators::IAllocator& allocator, efi::SystemTable* st,
                       List<UniquePointer<IStorageDevice>>& out);

  // The same for the one device on a root port (a stick that was just plugged in).
  static void discover_port(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator, efi::SystemTable* st,
                            List<UniquePointer<IStorageDevice>>& out);

  // False once the device was unplugged (the port's generation moved on) or shut down: requests then fail at once.
  bool present() const override {
    return !__atomic_load_n(&shut_down_, __ATOMIC_ACQUIRE) && !__atomic_load_n(&ejected_, __ATOMIC_ACQUIRE) && host_.port_generation(port_) == generation_;
  }
  // Flushes the write cache (a device that does not know the command is let off), allows removal and stops the unit (opening the tray of an optical
  // drive). False, with the device untouched, if the cache could not be flushed.
  bool can_eject() const override { return true; }
  bool eject() override;
  bool ejected() const override { return __atomic_load_n(&ejected_, __ATOMIC_ACQUIRE); }
  const char* name() const override { return optical_ ? "USB optical drive" : "USB mass storage"; }
  bool initialize() override;
  void shutdown() override;

 protected:
  bool transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) override;
  bool flush_cache() override;

 private:
  // Bulk-Only Transport wrappers (all fields little-endian), built and parsed with ByteWriter / ByteReader.
  static constexpr uint32_t kCbwSize = 31, kCswSize = 13;

  enum class Result { Good, Failed, Error };  // command failed (sense available) / transport broke
  Result command(ConstByteView cdb, ByteView data, bool data_in);
  bool run(ConstByteView cdb, ByteView data, bool data_in);
  void recover();

  IUsbHostController& host_;
  uint32_t port_;
  uint8_t interface_, in_, out_;
  uint16_t in_max_packet_, out_max_packet_;
  uint32_t generation_;     // the port's generation when the device was taken over
  bool shut_down_ = false;
  bool ejected_ = false;
  uint8_t last_sense_key_ = 0;   // of the last command that failed with CHECK CONDITION (0: none, or it has not failed since)
  ByteView wire_;  // command and status wrappers, sense and identify data: one 64 KiB-aligned page
  uint32_t tag_ = 1;
  bool optical_ = false;
};

}  // namespace hardware
