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

// The Bluetooth window's view of the machine: the adapters of a Bluetooth manager, numbered by their position, with the devices they found. It is the only piece that knows both the
// desktop's IBluetoothDetails and the hardware's IBluetoothManager / IBluetoothAdapter, and it works with whatever adapters the manager has (HCI over USB today). It also takes the
// pairing questions of every adapter (IBluetoothPairingUi) and keeps the one being asked for the window to show. The manager must outlive it.
#pragma once
#include "Desktop/BluetoothDetails.hpp"
#include "Hardware/Bluetooth/IBluetooth.hpp"

class SystemBluetoothDetails final : public desktop::IBluetoothDetails {
 public:
  explicit SystemBluetoothDetails(hardware::IBluetoothManager& manager);

  bool is_bluetooth(uint32_t id) override;
  bool adapter(uint32_t id, desktop::BluetoothAdapterInfo* out) override;
  void set_auto_reconnect(uint32_t id, bool on) override;
  bool set_discovery(uint32_t id, bool on) override;
  uint32_t devices(uint32_t id, desktop::BluetoothDeviceRow* out, uint32_t capacity) override;
  desktop::BluetoothResult connect(uint32_t id, const uint8_t key[6]) override;
  void disconnect(uint32_t id, const uint8_t key[6]) override;
  void forget(uint32_t id, const uint8_t key[6]) override;
  bool prompt(uint32_t id, desktop::BluetoothPrompt* out) override;
  void answer_confirm(uint32_t id, bool accepted) override;
  void answer_passkey(uint32_t id, uint32_t passkey) override;

  // Makes sure every adapter of the manager sends its pairing questions here. Done by every call that reaches an adapter; the system can also call it once adapters appear.
  void attach_adapters();

 private:
  static constexpr uint32_t kMaxAdapters = 4;
  struct Question {
    desktop::BluetoothPromptKind kind = desktop::BluetoothPromptKind::None;
    hardware::hci::BdAddr device;
    uint32_t number = 0;
  };
  class Ui final : public hardware::IBluetoothPairingUi {
   public:
    void display_passkey(const hardware::hci::BdAddr& device, uint32_t passkey) override;
    void confirm_numeric(const hardware::hci::BdAddr& device, uint32_t value) override;
    void request_passkey(const hardware::hci::BdAddr& device) override;
    hardware::IBluetoothAdapter* adapter = nullptr;
    Question question;
  };
  hardware::IBluetoothAdapter* find(uint32_t id);
  hardware::IBluetoothManager& manager_;
  Ui uis_[kMaxAdapters];
};
