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

// The IBluetoothAdapter for a controller driven through HCI (HciController and the layers on it): the user's view of the controller, built from discovery, links, pairing and GATT. An
// operation (connect to a device) moves through its phases as poll() is called - about every 100 ms - and the numbers of polls it may take are the timeouts:
//
//   Connecting   the LE connection is created (30 s)
//   Securing     encryption with the bond if there is one; a bond the peripheral no longer honours is deleted and the device is paired again
//   Pairing      the security manager's exchange (90 s; it waits for the user when the method needs one)
//   Setup        the MTU is agreed, the services found, the device's name read if it gave none, and every registered profile that matches is attached
//
// Not thread safe: one thread polls and calls it.
#pragma once
#include "Hardware/Bluetooth/BluetoothProfiles.hpp"
#include "Hardware/Bluetooth/IBluetooth.hpp"
#include "Hardware/Bluetooth/Smp.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class HciBluetoothAdapter final : public IBluetoothAdapter, private ISmUserInterface {
 public:
  static constexpr uint32_t kConnectTicks = 300, kPairTicks = 900, kMaxRemotes = 4, kMaxFailures = 8;
  // `security` may be null (no random source): such an adapter connects only to devices that need no pairing.
  HciBluetoothAdapter(HciController& controller, L2cap& l2cap, AttClient& att, GattClient& gatt, SecurityManager* security, SmBondStore& bonds, BluetoothProfileRegistry& profiles,
                      allocators::IAllocator& allocator);
  ~HciBluetoothAdapter();
  HciBluetoothAdapter(const HciBluetoothAdapter&) = delete;
  HciBluetoothAdapter& operator=(const HciBluetoothAdapter&) = delete;

  // IHardware
  const char* name() const override { return "Bluetooth adapter"; }
  bool initialize() override { return controller_.initialized(); }
  void shutdown() override;

  const hci::BdAddr& address() const override { return controller_.info().address; }
  bool ready() const override { return controller_.initialized() && controller_.present(); }
  BluetoothResult start_discovery() override;
  void stop_discovery() override;
  bool discovering() const override { return controller_.le_scanning() || controller_.inquiring(); }
  uint32_t device_count() const override { return controller_.device_count(); }
  bool device(uint32_t index, BluetoothDeviceInfo* out) const override;
  bool find_device(const hci::BdAddr& address, BluetoothDeviceInfo* out) const override;
  BluetoothResult connect(const hci::BdAddr& address, bool pair = true) override;
  BluetoothResult disconnect(const hci::BdAddr& address) override;
  BluetoothResult forget(const hci::BdAddr& address) override;
  void set_auto_reconnect(bool on) override;
  void set_pairing_ui(IBluetoothPairingUi* ui) override;
  void passkey_entered(const hci::BdAddr& address, uint32_t passkey) override;
  void confirm(const hci::BdAddr& address, bool accepted) override;
  void poll() override;

  // For the tests and the service: the controller underneath, and a remote of this adapter by its id.
  HciController& controller() { return controller_; }
  uint32_t connected_count() const;

 private:
  class Remote;
  enum class Phase : uint8_t { Idle, Connecting, Securing, Pairing, Setup };
  struct Operation {
    Phase phase = Phase::Idle;
    hci::BdAddr address;
    uint8_t type = 0;
    bool pair = true, pairing_started = false;
    uint32_t ticks = 0;
    uint16_t link = 0;
  };
  struct Failure { bool used = false; hci::BdAddr address; const char* text = ""; };

  // ISmUserInterface
  void display_passkey(uint16_t link, uint32_t passkey) override;
  void confirm_numeric(uint16_t link, uint32_t value) override;
  void request_passkey(uint16_t link) override;

  static void on_notification(void* self, uint16_t link, uint16_t handle, ConstByteView value, bool indication);
  void advance();
  void fail(const char* text);
  void remember_failure(const hci::BdAddr& address, const char* text);
  void forget_failure(const hci::BdAddr& address);
  void setup_remote();
  void drop_remote(Remote* remote);
  Remote* remote_for_link(uint16_t link) const;
  Remote* remote_for_address(const hci::BdAddr& address) const;
  void fill(const HciDevice& source, BluetoothDeviceInfo* out) const;
  void reconnect_bonded();
  const hci::BdAddr* address_of_link(uint16_t link) const;

  HciController& controller_;
  L2cap& l2cap_;
  AttClient& att_;
  GattClient& gatt_;
  SecurityManager* security_;
  SmBondStore& bonds_;
  BluetoothProfileRegistry& profiles_;
  allocators::IAllocator& allocator_;
  IBluetoothPairingUi* ui_ = nullptr;
  Operation op_;
  UniquePointer<Remote> remotes_[kMaxRemotes];
  Failure failures_[kMaxFailures];
  bool auto_reconnect_ = false;
  uint32_t next_id_ = 1, reconnect_seen_ = 0;
};

}  // namespace hardware
