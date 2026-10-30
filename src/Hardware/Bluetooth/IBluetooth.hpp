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

// The interfaces the rest of the system uses for Bluetooth, whichever controller and transport is underneath (USB today; a PCIe or UART controller would implement the same):
//
//   IBluetoothManager   the adapters the machine has, and the profiles that serve remote devices. The desktop's Bluetooth window and anything else that wants a device list talks to this.
//   IBluetoothAdapter   one controller as the user sees it: find devices, connect to one (pairing it first if it must be), disconnect, forget.
//   IBluetoothProfile   a driver for a kind of remote device (HID over GATT, a battery service, a heart rate sensor...). Registered with the manager once; offered every device that
//                       connects, it attaches to those it understands. Adding a profile changes nothing else: no adapter, no desktop code, and the devices a profile makes (a keyboard)
//                       reach the system through their own interface (IInputDeviceSource for input).
//   IBluetoothRemote    what a profile may do with a connected device: read and write its characteristics, subscribe to notifications.
//
// Operations that take time (connect, pair) start and return; the adapter's poll() carries them on and `device()` says how far they got. Nothing here blocks except what is said to.
#pragma once
#include <stdint.h>
#include "Hardware/Bluetooth/Gatt.hpp"
#include "Hardware/Bluetooth/HciTypes.hpp"
#include "Hardware/IHardware.hpp"

namespace hardware {

enum class BluetoothLinkState : uint8_t { Disconnected, Connecting, Pairing, Setup, Connected, Failed };

struct BluetoothDeviceInfo {
  hci::BdAddr address;
  uint8_t address_type = 0;         // LE: public (0) or random (1)
  bool le = true;                   // false: a classic device (found by inquiry; this system cannot connect to them yet)
  char name[40] = {};
  int8_t rssi = 127;                // 127: not known
  uint32_t class_of_device = 0;     // classic
  uint16_t appearance = 0;          // LE: the GAP appearance the device advertises
  bool connectable = false;
  bool paired = false;              // a bond is stored for it
  bool authenticated = false;       // the bond was made with a method that protects against a man in the middle
  BluetoothLinkState state = BluetoothLinkState::Disconnected;
  const char* failure = "";         // why the last attempt failed (state Failed)
  char profiles[48] = {};           // the names of the profiles serving it, comma separated
};

enum class BluetoothResult : uint8_t { Ok, Busy, NoSuchDevice, Unsupported, NotReady };

// What the user is asked while a device pairs. The answers go back through IBluetoothAdapter::confirm() / passkey_entered().
class IBluetoothPairingUi {
 public:
  virtual void display_passkey(const hci::BdAddr& device, uint32_t passkey) = 0;       // show it; the device's user types it
  virtual void confirm_numeric(const hci::BdAddr& device, uint32_t value) = 0;         // show it and ask whether the device shows the same
  virtual void request_passkey(const hci::BdAddr& device) = 0;                         // ask for the passkey the device shows

 protected:
  IBluetoothPairingUi() = default;
  ~IBluetoothPairingUi() = default;
};

class IBluetoothRemote;
class IBluetoothNotificationSink {
 public:
  virtual void on_notification(IBluetoothRemote& remote, uint16_t value_handle, ConstByteView value, bool indication) = 0;

 protected:
  IBluetoothNotificationSink() = default;
  ~IBluetoothNotificationSink() = default;
};

class IBluetoothRemote {
 public:
  virtual const BluetoothDeviceInfo& info() const = 0;
  virtual uint32_t id() const = 0;                         // different for every connection; 0 is never used
  virtual bool connected() const = 0;
  virtual bool encrypted() const = 0;
  virtual const GattDatabase& database() const = 0;
  // `characteristic` and `descriptor` are indices and handles from database(). These run the controller meanwhile and wait for the answer (a few milliseconds to a few seconds).
  virtual AttStatus read(int characteristic, ByteView out, uint32_t* length) = 0;
  virtual AttStatus read_descriptor(uint16_t handle, ByteView out, uint32_t* length) = 0;
  virtual AttStatus write(int characteristic, ConstByteView value, bool with_response) = 0;
  // Switches notifications (or indications) on for a characteristic and routes them to `sink` (null: off).
  virtual AttStatus subscribe(int characteristic, bool indications, IBluetoothNotificationSink* sink) = 0;

 protected:
  IBluetoothRemote() = default;
  ~IBluetoothRemote() = default;
};

class IBluetoothProfile {
 public:
  virtual const char* name() const = 0;
  // A cheap look at the device's services: does this profile serve it?
  virtual bool matches(const BluetoothDeviceInfo& info, const GattDatabase& database) const = 0;
  // Sets the device up (reads what it needs, subscribes). False: not served after all. Called from the adapter's poll().
  virtual bool attach(IBluetoothRemote& remote) = 0;
  // The device is gone (its link dropped, or it was disconnected): let go of everything about it. `remote` can no longer be used.
  virtual void detach(IBluetoothRemote& remote) = 0;

 protected:
  IBluetoothProfile() = default;
  ~IBluetoothProfile() = default;
};

class IBluetoothAdapter : public IHardware {
 public:
  virtual const hci::BdAddr& address() const = 0;
  virtual bool ready() const = 0;

  // Discovery: LE advertisers (and classic devices by inquiry) accumulate in the device list until stopped or cleared.
  virtual BluetoothResult start_discovery() = 0;
  virtual void stop_discovery() = 0;
  virtual bool discovering() const = 0;
  virtual uint32_t device_count() const = 0;
  virtual bool device(uint32_t index, BluetoothDeviceInfo* out) const = 0;
  virtual bool find_device(const hci::BdAddr& address, BluetoothDeviceInfo* out) const = 0;

  // Starts connecting to a device from the list: link, pairing if it is not bonded (and `pair`), service discovery, then the profiles. One at a time (Busy otherwise).
  virtual BluetoothResult connect(const hci::BdAddr& address, bool pair = true) = 0;
  virtual BluetoothResult disconnect(const hci::BdAddr& address) = 0;
  // Disconnects and deletes the bond.
  virtual BluetoothResult forget(const hci::BdAddr& address) = 0;
  // Connect again to bonded devices that start advertising (a keyboard that was switched off and on).
  virtual void set_auto_reconnect(bool on) = 0;

  virtual void set_pairing_ui(IBluetoothPairingUi* ui) = 0;
  virtual void passkey_entered(const hci::BdAddr& address, uint32_t passkey) = 0;
  virtual void confirm(const hci::BdAddr& address, bool accepted) = 0;

  // Carries operations on and reads events. Called regularly by whoever owns the adapter (the manager).
  virtual void poll() = 0;

 protected:
  IBluetoothAdapter() = default;
  ~IBluetoothAdapter() = default;
};

class IBluetoothManager {
 public:
  virtual uint32_t adapter_count() const = 0;
  virtual IBluetoothAdapter* adapter(uint32_t index) = 0;
  // Profiles are offered every device that connects through any adapter, those registered before and after the adapter came up alike. The profile must outlive the manager or be removed.
  virtual bool add_profile(IBluetoothProfile* profile) = 0;
  virtual bool remove_profile(IBluetoothProfile* profile) = 0;

 protected:
  IBluetoothManager() = default;
  ~IBluetoothManager() = default;
};

}  // namespace hardware
