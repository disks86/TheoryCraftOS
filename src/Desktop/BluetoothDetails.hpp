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

// What the Bluetooth window shows and does: the devices a Bluetooth adapter has found, which of them are paired or connected, connecting to one and leaving it, and the questions the
// pairing asks the user. The desktop knows nothing about drivers: whoever builds the system hands it an IBluetoothDetails (SystemBluetoothDetails, in the real system, built on the
// Bluetooth manager's interfaces). Adapters are numbered as in the hardware tree (HardwareEntry::id).
#pragma once
#include <stdint.h>
#include "Utility/StringView.hpp"

namespace desktop {

enum class BluetoothDeviceState : uint8_t { Disconnected, Connecting, Pairing, Setup, Connected, Failed };
enum class BluetoothResult : uint8_t { Ok, Busy, NoSuchDevice, Unsupported, NotReady };
// What the pairing waits for from the user: nothing; a number to show (the device's user types it); two numbers to compare; the number the device shows.
enum class BluetoothPromptKind : uint8_t { None, ShowPasskey, ConfirmNumber, EnterPasskey };

struct BluetoothDeviceRow {
  char name[40] = {};                 // empty when the device gave none
  char address[18] = {};              // "AA:BB:CC:DD:EE:FF"
  uint8_t key[6] = {};                // the address as the adapter knows it; what connect() takes
  char kind[16] = {};                 // "Keyboard", "Mouse", "Headset", "Phone", ... or "Device"
  int8_t signal_percent = -1;         // -1: not known
  BluetoothDeviceState state = BluetoothDeviceState::Disconnected;
  bool paired = false;
  bool supported = true;              // false: a kind of device this system cannot connect to yet (classic Bluetooth)
  char failure[64] = {};              // why the last attempt failed (state Failed)
  char profiles[48] = {};             // what serves it once connected ("HID over GATT")
};

struct BluetoothAdapterInfo {
  char name[64] = {};
  char address[18] = {};
  bool ready = false;
  bool discovering = false;
};

struct BluetoothPrompt {
  BluetoothPromptKind kind = BluetoothPromptKind::None;
  char device[40] = {};
  uint32_t number = 0;                // ShowPasskey and ConfirmNumber: the six digits
};

class IBluetoothDetails {
 public:
  static constexpr uint32_t kMaxDevices = 64;

  // True if the adapter with this number is a Bluetooth adapter.
  virtual bool is_bluetooth(uint32_t id) = 0;
  // The adapter's state. False if there is no such adapter (any more).
  virtual bool adapter(uint32_t id, BluetoothAdapterInfo* out) = 0;
  // Starts or stops the search for devices.
  virtual bool set_discovery(uint32_t id, bool on) = 0;
  // The devices found so far and the ones connected: connected first, then paired, then by signal. Writes up to `capacity`; returns how many.
  virtual uint32_t devices(uint32_t id, BluetoothDeviceRow* out, uint32_t capacity) = 0;
  // Starts connecting (pairing first if the device is not paired); the state of the row says how far it got.
  virtual BluetoothResult connect(uint32_t id, const uint8_t key[6]) = 0;
  virtual void disconnect(uint32_t id, const uint8_t key[6]) = 0;
  // Disconnects and deletes the pairing.
  virtual void forget(uint32_t id, const uint8_t key[6]) = 0;
  // The question the pairing is asking now, if there is one; answered with answer_confirm() or answer_passkey().
  virtual bool prompt(uint32_t id, BluetoothPrompt* out) = 0;
  virtual void answer_confirm(uint32_t id, bool accepted) = 0;
  virtual void answer_passkey(uint32_t id, uint32_t passkey) = 0;

 protected:
  constexpr IBluetoothDetails() = default;
  ~IBluetoothDetails() = default;
};

// "Connected", "Not connected", "Connecting...", "Pairing...", ...
const char* bluetooth_state_name(BluetoothDeviceState state);
const char* bluetooth_result_text(BluetoothResult result);

}  // namespace desktop
