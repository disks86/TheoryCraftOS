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

// Every Bluetooth controller on the USB ports: finds them (those present at start-up, and ones plugged in later), loads the firmware an Intel one needs, brings it up and keeps its events
// flowing. step() does one round of that (polling every controller; with `scan` also looking for controllers plugged in or pulled out); whoever owns the service calls it now and then.
// Not thread safe by itself.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Crypto/Random.hpp"
#include "Hardware/Bluetooth/BondFile.hpp"
#include "Hardware/Bluetooth/Gatt.hpp"
#include "Hardware/Bluetooth/HciBluetoothAdapter.hpp"
#include "Hardware/Bluetooth/HciController.hpp"
#include "Hardware/Bluetooth/L2cap.hpp"
#include "Hardware/Bluetooth/IntelBluetooth.hpp"
#include "Hardware/Bluetooth/Smp.hpp"
#include "Hardware/Bluetooth/UsbHciTransport.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Bus/Usb/UsbPortWatcher.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Where firmware files come from (the system's is the "/firmware" folder). `name` is the name the controller asks for, with its directory ("intel/ibt-17-16-1.sfi").
class IBluetoothFirmwareSource {
 public:
  virtual bool read(const char* name, ByteBuffer& out) = 0;

 protected:
  constexpr IBluetoothFirmwareSource() = default;
  ~IBluetoothFirmwareSource() = default;
};

enum class BluetoothState : uint8_t { Ready, FirmwareMissing, Failed };

class BluetoothService final : public IBluetoothManager {
 public:
  struct Entry {
    UniquePointer<UsbHciTransport> transport;
    UniquePointer<HciController> controller;
    UniquePointer<IntelBluetooth> intel;
    UniquePointer<L2cap> l2cap;                        // the upper layers, once the controller is ready (the security manager only with a random source)
    UniquePointer<AttClient> att;
    UniquePointer<GattClient> gatt;
    UniquePointer<SecurityManager> security;
    UniquePointer<HciBluetoothAdapter> adapter;         // the user's view of the controller (IBluetoothAdapter)
    BluetoothState state = BluetoothState::Failed;
    char message[96] = {};
    char firmware[64] = {};            // the file an Intel controller is waiting for
    uint32_t port = 0, generation = 0, bus_controller = 0;
  };

  // `usb` and `firmware` may be null: without a bus there is nothing to find, without a firmware source an Intel controller stays unstarted.
  BluetoothService(IUsbBus* usb, IBluetoothFirmwareSource* firmware, allocators::IAllocator& allocator, crypto::IRandom* random = nullptr, IBluetoothBondStorage* bonds = nullptr);
  BluetoothService(const BluetoothService&) = delete;
  BluetoothService& operator=(const BluetoothService&) = delete;

  // Takes over the controllers present now.
  void start();
  void step(bool scan);

  uint32_t count() const { return static_cast<uint32_t>(entries_.count()); }
  const Entry* entry(uint32_t index) const { return index < entries_.count() ? entries_[index].get() : nullptr; }
  HciController* controller(uint32_t index) { return index < entries_.count() && entries_[index]->state == BluetoothState::Ready ? entries_[index]->controller.get() : nullptr; }
  uint32_t ready_count() const;

  // IBluetoothManager: the adapters of the controllers that are ready, and the profiles offered to the devices that connect to them.
  uint32_t adapter_count() const override;
  IBluetoothAdapter* adapter(uint32_t index) override;
  bool add_profile(IBluetoothProfile* profile) override { return profiles_.add(profile); }
  bool remove_profile(IBluetoothProfile* profile) override { return profiles_.remove(profile); }
  // The bonds of every controller (one table: a device bonded through one is known to the others). They are loaded when the service starts and saved by save_bonds().
  SmBondStore& bonds() { return bonds_; }
  bool save_bonds();
  uint32_t added() const { return added_; }
  uint32_t removed() const { return removed_; }

 private:
  void scan_ports();
  void open(uint32_t bus_controller, IUsbHostController& host, uint32_t port);
  void drop_gone();

  IUsbBus* usb_;
  IBluetoothFirmwareSource* firmware_;
  allocators::IAllocator& allocator_;
  crypto::IRandom* random_;
  IBluetoothBondStorage* bond_storage_;
  SmBondStore bonds_;
  BluetoothProfileRegistry profiles_;
  UsbPortWatcher watcher_;
  List<UniquePointer<Entry>> entries_;
  uint32_t added_ = 0, removed_ = 0;
};

}  // namespace hardware
