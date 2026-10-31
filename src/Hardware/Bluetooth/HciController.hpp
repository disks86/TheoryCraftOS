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

// The host side of a Bluetooth controller: commands sent and answered, events taken apart into state (devices found, links up, packet credits), ACL data in and out. Polled: nothing runs
// by itself, whoever owns the controller calls poll() now and then (and every wait inside a command polls too). Not thread safe.
//
//   Bring-up:      initialize() resets the controller and reads what it is (version, address, buffers, features) and switches on the events the host handles.
//   Finding:       start_inquiry() (Bluetooth classic) and start_le_scan() fill a table of devices; request_name() asks a classic device for its name.
//   Links:         connect() / connect_le() / disconnect(); incoming classic connections are refused unless accept_incoming(true), nothing is paired or encrypted here.
//   Data:          send_acl() splits a packet to the controller's buffer size and waits for a free buffer (flow control by the credits the controller hands out); what arrives goes to the
//                  ACL handler, whole packets with the link they came on.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bluetooth/HciPackets.hpp"
#include "Hardware/Bluetooth/IHciTransport.hpp"

namespace hardware {

enum class HciResult : uint8_t { Ok, Timeout, Transport, Status, Malformed, NoRoom };

struct HciInfo {
  hci::BdAddr address;
  uint8_t hci_version = 0, lmp_version = 0;
  uint16_t hci_revision = 0, lmp_subversion = 0, manufacturer = 0;
  uint8_t features[8] = {};
  uint8_t le_features[8] = {};
  uint16_t acl_mtu = 0, acl_packets = 0, le_mtu = 0, le_packets = 0;      // the controller's buffers (the LE ones are 0 when LE data shares the ACL buffers)
  uint8_t sco_mtu = 0;
  uint16_t sco_packets = 0;
  bool le_supported = false, classic_supported = true, simple_pairing = false;
};

struct HciDevice {
  hci::BdAddr address;
  bool le = false;                // seen by an LE scan (else by an inquiry)
  uint8_t address_type = 0;       // LE: public or random
  uint32_t class_of_device = 0;
  int8_t rssi = 127;
  char name[40] = {};
  uint32_t last_seen = 0;         // the poll count of the last report
  uint16_t appearance = 0;
  bool connectable = false;       // LE: the advertisement said it accepts a connection
};

struct HciLink {
  bool used = false;
  bool encrypted = false;
  uint16_t handle = 0;
  hci::BdAddr address;
  bool le = false;
  uint8_t address_type = 0;
  bool central = true;            // we initiated it
};

using HciAclHandler = void (*)(void* context, const HciLink& link, uint8_t boundary, ConstByteView data);
// A link came up (`up`) or went down.
using HciLinkHandler = void (*)(void* context, const HciLink& link, bool up);
// Encryption on a link changed (status 0: it did; else the controller refused or the key was wrong).
using HciEncryptionHandler = void (*)(void* context, uint16_t handle, uint8_t status, bool enabled);
// The controller (as the peripheral's side of a bond, or ours) asks for the long term key of a link to start encryption: fill `ltk` and return true, or false to refuse.
using HciKeyRequestHandler = bool (*)(void* context, uint16_t handle, const uint8_t random[8], uint16_t ediv, uint8_t ltk[16]);
// Sees every event first; true takes it (the vendor events of firmware loading are handled this way).
using HciEventHook = bool (*)(void* context, const hci::Event& event);

class HciController {
 public:
  static constexpr uint32_t kMaxDevices = 32, kMaxLinks = 8;
  HciController(IHciTransport& transport, allocators::IAllocator& allocator) : transport_(transport), allocator_(allocator) {}
  HciController(const HciController&) = delete;
  HciController& operator=(const HciController&) = delete;

  // --- commands
  // Sends a command and waits for its Command Complete (or Command Status). Events that arrive meanwhile are handled. Ok means the controller answered with status 0; Status that it answered
  // with another one (last_status()).
  HciResult command(uint16_t opcode, ConstByteView params = ConstByteView(), uint32_t timeout_ms = 2000);
  uint8_t last_status() const { return status_; }
  // The return parameters of the last command that completed (after the status byte, which they start with for nearly every command).
  ConstByteView reply() const { return ConstByteView(reply_).first(reply_size_); }

  // --- running
  // Handles what the controller has sent: events (state, then hooks) and ACL data. Returns how many packets there were.
  uint32_t poll();
  // Waits up to `timeout_ms`, polling, until `done(context)` is true.
  bool wait_for(bool (*done)(void*), void* context, uint32_t timeout_ms);
  bool present() const { return transport_.present(); }

  // --- bring-up
  HciResult initialize();
  const HciInfo& info() const { return info_; }
  bool initialized() const { return initialized_; }

  // --- finding devices
  HciResult start_inquiry(uint8_t length_1_28s = 4);              // classic: length in units of 1.28 s
  HciResult cancel_inquiry();
  bool inquiring() const { return inquiring_; }
  HciResult start_le_scan(bool active = true);
  HciResult stop_le_scan();
  bool le_scanning() const { return le_scanning_; }
  HciResult request_name(const hci::BdAddr& address);
  uint32_t device_count() const { return device_count_; }
  const HciDevice* device(uint32_t index) const { return index < device_count_ ? &devices_[index] : nullptr; }
  const HciDevice* find_device(const hci::BdAddr& address) const;
  void clear_devices() { device_count_ = 0; }

  // --- links
  HciResult connect(const hci::BdAddr& address);
  HciResult connect_le(const hci::BdAddr& address, uint8_t address_type);
  HciResult cancel_connect_le() { return command(hci::kOpLeCreateConnectionCancel); }
  HciResult disconnect(uint16_t handle, uint8_t reason = hci::kStatusRemoteUserTerminated);
  void accept_incoming(bool accept) { accept_incoming_ = accept; }
  const HciLink* link(uint16_t handle) const;
  const HciLink* find_link(const hci::BdAddr& address) const;
  uint32_t link_count() const;
  const HciLink* link_at(uint32_t index) const { return index < kMaxLinks && links_[index].used ? &links_[index] : nullptr; }

  // --- data
  void set_acl_handler(HciAclHandler handler, void* context) { acl_handler_ = handler; acl_context_ = context; }
  void set_event_hook(HciEventHook hook, void* context) { hook_ = hook; hook_context_ = context; }
  void set_link_handler(HciLinkHandler handler, void* context) { link_handler_ = handler; link_context_ = context; }
  void set_encryption_handler(HciEncryptionHandler handler, void* context) { encryption_handler_ = handler; encryption_context_ = context; }
  void set_key_request_handler(HciKeyRequestHandler handler, void* context) { key_handler_ = handler; key_context_ = context; }
  // LE: starts encryption of a link with a key we hold (LE Start Encryption; the result arrives as an encryption change).
  HciResult start_encryption(uint16_t handle, const uint8_t random[8], uint16_t ediv, const uint8_t ltk[16]);
  // Classic: switches encryption on a link on (needs a link key the controller holds).
  HciResult set_connection_encryption(uint16_t handle, bool on);
  // Sends one L2CAP packet (or any ACL payload) on a link in as many fragments as the controller's buffers need.
  HciResult send_acl(uint16_t handle, ConstByteView data, uint32_t timeout_ms = 2000);

  // Counters for logs and tests.
  uint32_t events_seen() const { return events_seen_; }
  uint32_t malformed() const { return malformed_; }
  uint32_t hardware_errors() const { return hardware_errors_; }
  uint32_t acl_received() const { return acl_received_; }
  uint32_t incoming_refused() const { return incoming_refused_; }

 private:
  void handle_event(const hci::Event& event);
  void handle_le_meta(const hci::Event& event);
  void note_device(const hci::BdAddr& address, bool le, uint8_t address_type, uint32_t class_of_device, int8_t rssi, ConstByteView data, uint8_t advertising_type);
  HciLink* add_link(uint16_t handle, const hci::BdAddr& address, bool le, uint8_t address_type, bool central);
  void remove_link(uint16_t handle);
  void give_credits(uint16_t handle, uint16_t count);
  uint16_t* credits_for(const HciLink& link);
  void process_incoming();
  uint32_t drain_events();
  HciResult simple(uint16_t opcode, ConstByteView params) { return command(opcode, params); }

  IHciTransport& transport_;
  allocators::IAllocator& allocator_;
  HciInfo info_;
  bool initialized_ = false;
  uint8_t status_ = 0;
  uint8_t reply_[hci::kMaxEventParams] = {};
  uint32_t reply_size_ = 0;
  // the command in flight
  uint16_t waiting_opcode_ = 0;
  bool waiting_ = false, answered_ = false;
  HciResult answer_ = HciResult::Ok;

  HciDevice devices_[kMaxDevices];
  uint32_t device_count_ = 0, ticks_ = 0;
  HciLink links_[kMaxLinks];
  uint16_t acl_credits_ = 0, le_credits_ = 0;
  bool inquiring_ = false, le_scanning_ = false, accept_incoming_ = false;
  struct Incoming { bool used = false; hci::BdAddr address; };
  Incoming incoming_[4];
  struct KeyRequest { bool used = false; uint16_t handle = 0; uint8_t random[8] = {}; uint16_t ediv = 0; };
  KeyRequest key_requests_[2];
  HciLinkHandler link_handler_ = nullptr;
  void* link_context_ = nullptr;
  HciEncryptionHandler encryption_handler_ = nullptr;
  void* encryption_context_ = nullptr;
  HciKeyRequestHandler key_handler_ = nullptr;
  void* key_context_ = nullptr;

  HciAclHandler acl_handler_ = nullptr;
  void* acl_context_ = nullptr;
  HciEventHook hook_ = nullptr;
  void* hook_context_ = nullptr;
  uint32_t events_seen_ = 0, malformed_ = 0, hardware_errors_ = 0, acl_received_ = 0, incoming_refused_ = 0;
};

}  // namespace hardware
