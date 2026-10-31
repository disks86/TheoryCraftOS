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

// A fake Bluetooth controller behind a fake USB host: it answers the commands the host driver sends (the generic bring-up, discovery, connections, and the Intel bootloader), delivers events
// in pieces of the endpoint's packet size the way a USB interrupt endpoint does, and takes ACL data.
#pragma once
#include <string.h>
#include <deque>
#include <vector>
#include "Hardware/Bluetooth/HciPackets.hpp"
#include "Hardware/Bluetooth/UsbHciTransport.hpp"
#include "Hardware/Bus/Usb/IUsbHostController.hpp"

namespace fake_bt {

using namespace hardware;
typedef std::vector<uint8_t> Bytes;

inline ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }

struct Controller {
  // what it is
  uint8_t hci_version = 9, lmp_version = 9;
  uint16_t manufacturer = 2, acl_mtu = 27, acl_packets = 3, le_mtu = 0, le_packets = 0;
  uint8_t features[8] = {0xFF, 0xFF, 0x8F, 0xFE, 0x58, 0x88, 0x0D, 0x80};     // (LE and simple pairing set)
  uint8_t address[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  bool le = true;
  // behaviour
  bool ack_acl = true;                    // answers ACL packets with a Number of Completed Packets event
  bool silent = false;                    // answers nothing
  uint8_t connect_status = 0;
  uint8_t reject_command_status = 0;      // nonzero: the Inquiry command is refused with this status
  bool intel = false, intel_operational = false;
  uint8_t hw_variant = 0x11, hw_revision = 0x0A, fw_revision = 0x02;
  uint32_t payload_expected = 0, payload_received = 0;
  bool intel_fail_download = false, intel_no_bootup = false;
  uint16_t dev_revid = 0x23;
  // what it was asked
  std::vector<Bytes> commands, acl_from_host;
  std::vector<uint16_t> opcodes;
  std::vector<Bytes> intel_fragments;
  uint32_t resets = 0;
  Bytes last_accept, last_reject;
  uint32_t scan_enable_writes = 0;
  // what it will say
  std::deque<Bytes> events, acl_to_host;
  std::vector<Bytes> acl_log;           // every ACL packet that was queued for the host
  // a peripheral behind the controller (see FakePeripheral.hpp): it gets every ACL packet the host sends and the encryption requests
  void (*acl_hook)(void* context, const Bytes& packet) = nullptr;
  bool (*encryption_hook)(void* context, uint16_t handle, const uint8_t random[8], uint16_t ediv, const uint8_t ltk[16]) = nullptr;
  void* hook_context = nullptr;

  void push(uint8_t code, const Bytes& params) {
    Bytes e;
    e.push_back(code);
    e.push_back(static_cast<uint8_t>(params.size()));
    e.insert(e.end(), params.begin(), params.end());
    events.push_back(e);
  }
  void complete(uint16_t op, uint8_t status, const Bytes& more = Bytes()) {
    Bytes p = {1, static_cast<uint8_t>(op), static_cast<uint8_t>(op >> 8), status};
    p.insert(p.end(), more.begin(), more.end());
    push(0x0E, p);
  }
  void status_event(uint16_t op, uint8_t status) { push(0x0F, {status, 1, static_cast<uint8_t>(op), static_cast<uint8_t>(op >> 8)}); }
  static Bytes addr(const uint8_t a[6]) { return Bytes(a, a + 6); }

  void inject_acl(uint16_t handle, uint8_t pb, const Bytes& data) {
    Bytes p = {static_cast<uint8_t>(handle), static_cast<uint8_t>((handle >> 8) | (pb << 4)), static_cast<uint8_t>(data.size()), static_cast<uint8_t>(data.size() >> 8)};
    p.insert(p.end(), data.begin(), data.end());
    acl_to_host.push_back(p);
    acl_log.push_back(p);
  }

  void command(const Bytes& packet) {
    commands.push_back(packet);
    if (packet.size() < 3) return;
    const uint16_t op = static_cast<uint16_t>(packet[0] | (packet[1] << 8));
    const Bytes params(packet.begin() + 3, packet.end());
    opcodes.push_back(op);
    if (silent) return;
    using namespace hci;
    switch (op) {
      case kOpReset: ++resets; complete(op, 0); return;
      case kOpReadLocalVersion: complete(op, 0, {hci_version, 0x34, 0x12, lmp_version, static_cast<uint8_t>(manufacturer), static_cast<uint8_t>(manufacturer >> 8), 0x78, 0x56}); return;
      case kOpReadBdAddr: complete(op, 0, addr(address)); return;
      case kOpReadBufferSize: complete(op, 0, {static_cast<uint8_t>(acl_mtu), static_cast<uint8_t>(acl_mtu >> 8), 60, static_cast<uint8_t>(acl_packets), static_cast<uint8_t>(acl_packets >> 8), 8, 0}); return;
      case kOpReadLocalFeatures: { Bytes f(features, features + 8); if (!le) f[4] &= ~0x40; complete(op, 0, f); return; }
      case kOpSetEventMask: case kOpWriteInquiryMode: case kOpWriteSimplePairingMode: case kOpWriteLeHostSupported: case kOpLeSetEventMask:
      case kOpLeSetScanParameters: case kOpWriteLocalName: complete(op, 0); return;
      case kOpWriteScanEnable: ++scan_enable_writes; complete(op, 0); return;
      case kOpLeReadBufferSize: complete(op, 0, {static_cast<uint8_t>(le_mtu), static_cast<uint8_t>(le_mtu >> 8), static_cast<uint8_t>(le_packets)}); return;
      case kOpLeReadLocalFeatures: complete(op, 0, {1, 0, 0, 0, 0, 0, 0, 0}); return;
      case kOpInquiry: inquiry(); return;
      case kOpInquiryCancel: complete(op, 0); return;
      case kOpRemoteNameRequest: remote_name(params); return;
      case kOpLeSetScanEnable: complete(op, 0); if (params.size() >= 1 && params[0] == 1) advertisements(); return;
      case kOpCreateConnection: {
        status_event(op, connect_status);
        if (connect_status == 0) {
          Bytes p = {0, 0x01, 0x00};
          p.insert(p.end(), params.begin(), params.begin() + 6);
          p.push_back(1);   // ACL link
          p.push_back(0);
          push(kEvConnectionComplete, p);
        }
        return;
      }
      case kOpLeCreateConnection: {
        status_event(op, connect_status);
        if (connect_status == 0) {
          Bytes p = {kLeConnectionComplete, 0, 0x40, 0x00, 0 /* central */, params[5]};
          p.insert(p.end(), params.begin() + 6, params.begin() + 12);
          p.insert(p.end(), {0x18, 0x00, 0x00, 0x00, 0xC8, 0x00, 0x00});
          push(kEvLeMeta, p);
        }
        return;
      }
      case kOpDisconnect: {
        status_event(op, 0);
        push(kEvDisconnectionComplete, {0, params[0], params[1], params[2]});
        return;
      }
      case kOpLeStartEncryption: {
        status_event(op, 0);
        const uint16_t handle = static_cast<uint16_t>(params[0] | (params[1] << 8));
        const bool ok = encryption_hook && encryption_hook(hook_context, handle, params.data() + 2, static_cast<uint16_t>(params[10] | (params[11] << 8)), params.data() + 12);
        push(kEvEncryptionChange, {static_cast<uint8_t>(ok ? 0 : 0x06), params[0], params[1], static_cast<uint8_t>(ok ? 1 : 0)});
        return;
      }
      case kOpAcceptConnection: last_accept = params; status_event(op, 0); return;
      case kOpRejectConnection: last_reject = params; status_event(op, 0); return;
      default: break;
    }
    if (intel) intel_command(op, params);
    else complete(op, hci::kStatusUnknownCommand);
  }

  void intel_command(uint16_t op, const Bytes& params) {
    switch (op) {
      case 0xFC05: complete(op, 0, {0x37, hw_variant, hw_revision, static_cast<uint8_t>(intel_operational ? 0x23 : 0x06), fw_revision, 1, 2, 3, 4}); return;
      case 0xFC0D: {
        Bytes p(22, 0);
        p[3] = static_cast<uint8_t>(dev_revid);
        p[4] = static_cast<uint8_t>(dev_revid >> 8);
        for (int i = 0; i < 6; ++i) p[11 + i] = address[i];
        p[20] = 0;                                    // limited_cce
        complete(op, 0, p);
        return;
      }
      case 0xFC09: {
        intel_fragments.push_back(params);
        complete(op, 0);
        if (!params.empty() && params[0] == 1) payload_received += static_cast<uint32_t>(params.size() - 1);
        if (payload_expected && payload_received >= payload_expected) push(0xFF, {0x06, static_cast<uint8_t>(intel_fail_download ? 1 : 0), 0x09, 0xFC, 0});
        return;
      }
      case 0xFC01: {
        complete(op, 0);
        if (!intel_no_bootup) push(0xFF, {0x02, 0, 0, 0, 0, 0, 0});
        intel_operational = true;
        return;
      }
      default: complete(op, hci::kStatusUnknownCommand); return;
    }
  }

  void inquiry() {
    using namespace hci;
    if (reject_command_status) { status_event(kOpInquiry, reject_command_status); return; }
    status_event(kOpInquiry, 0);
    // two results with the signal strength (the fields grouped by kind), one extended result with a name, one plain
    Bytes p = {2};
    const uint8_t a1[6] = {1, 2, 3, 4, 5, 6}, a2[6] = {7, 8, 9, 10, 11, 12};
    p.insert(p.end(), a1, a1 + 6);
    p.insert(p.end(), a2, a2 + 6);
    p.insert(p.end(), {1, 1});                // page scan repetition modes
    p.insert(p.end(), {0, 0});                // reserved
    p.insert(p.end(), {0x0C, 0x02, 0x5A, 0x10, 0x05, 0x24});          // class of device: 0x5A020C and 0x240510
    p.insert(p.end(), {0x34, 0x12, 0x78, 0x96});                      // clock offsets (bit 15 is a valid flag)
    p.insert(p.end(), {static_cast<uint8_t>(-60), static_cast<uint8_t>(-71)});
    push(kEvInquiryResultWithRssi, p);
    Bytes e = {1};
    const uint8_t a3[6] = {0xA, 0xB, 0xC, 0xD, 0xE, 0xF};
    e.insert(e.end(), a3, a3 + 6);
    e.insert(e.end(), {1, 0});
    e.insert(e.end(), {0x04, 0x04, 0x20});    // class of device
    e.insert(e.end(), {0x01, 0x00});
    e.push_back(static_cast<uint8_t>(-50));
    const char* name = "Headset X";
    e.push_back(static_cast<uint8_t>(strlen(name) + 1));
    e.push_back(0x09);
    e.insert(e.end(), name, name + strlen(name));
    e.push_back(0);
    push(kEvExtendedInquiryResult, e);
    Bytes q = {1};
    const uint8_t a4[6] = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26};
    q.insert(q.end(), a4, a4 + 6);
    q.insert(q.end(), {1, 0, 0, 0x08, 0x05, 0x00, 0x00, 0x00});
    push(kEvInquiryResult, q);
    push(kEvInquiryComplete, {0});
  }

  void remote_name(const Bytes& params) {
    status_event(hci::kOpRemoteNameRequest, 0);
    Bytes p = {0};
    p.insert(p.end(), params.begin(), params.begin() + 6);
    const char* name = "Speaker Y";
    p.insert(p.end(), name, name + strlen(name));
    p.resize(7 + 248, 0);
    push(hci::kEvRemoteNameRequestComplete, p);
  }

  static Bytes report(uint8_t type, uint8_t addr_type, const uint8_t a[6], const Bytes& data, int8_t rssi) {
    Bytes r = {type, addr_type};
    r.insert(r.end(), a, a + 6);
    r.push_back(static_cast<uint8_t>(data.size()));
    r.insert(r.end(), data.begin(), data.end());
    r.push_back(static_cast<uint8_t>(rssi));
    return r;
  }
  void advertisements() {
    using namespace hci;
    const uint8_t a1[6] = {0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6}, a2[6] = {0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6};
    Bytes one = {kLeAdvertisingReport, 1};
    Bytes ad1 = {2, 0x01, 0x06, 4, 0x08, 'K', 'e', 'y', 3, 0x19, 0xC1, 0x03};      // flags, short name "Key", appearance keyboard
    Bytes r1 = report(0, 1, a1, ad1, -55);
    one.insert(one.end(), r1.begin(), r1.end());
    push(kEvLeMeta, one);
    Bytes two = {kLeAdvertisingReport, 2};                                            // the scan response of the first and a beacon
    Bytes ad2 = {9, 0x09, 'K', 'e', 'y', 'b', 'o', 'a', 'r', 'd'};
    Bytes r2 = report(4, 1, a1, ad2, -53);
    Bytes r3 = report(3, 0, a2, Bytes{2, 0x0A, 0x04}, -80);
    two.insert(two.end(), r2.begin(), r2.end());
    two.insert(two.end(), r3.begin(), r3.end());
    push(kEvLeMeta, two);
  }

  void acl(const Bytes& packet) {
    acl_from_host.push_back(packet);
    if (acl_hook) acl_hook(hook_context, packet);
    if (!ack_acl || packet.size() < 4) return;
    const uint16_t handle = static_cast<uint16_t>((packet[0] | (packet[1] << 8)) & 0x0FFF);
    push(hci::kEvNumberOfCompletedPackets, {1, static_cast<uint8_t>(handle), static_cast<uint8_t>(handle >> 8), 1, 0});
  }
};

// A USB configuration with the Bluetooth interface (0xE0/1/1): interrupt IN 0x81 (16 byte packets), bulk IN 0x82 and bulk OUT 0x02 (64), and an isochronous alternate setting on the next interface.
inline Bytes bluetooth_config(bool endpoints = true) {
  Bytes c = {9, 2, 0, 0, 2, 1, 0, 0xE0, 100};
  Bytes i0 = {9, 4, 0, 0, static_cast<uint8_t>(endpoints ? 3 : 1), 0xE0, 0x01, 0x01, 0};
  c.insert(c.end(), i0.begin(), i0.end());
  if (endpoints) {
    const Bytes ep = {7, 5, 0x81, 3, 16, 0, 1, 7, 5, 0x02, 2, 64, 0, 0, 7, 5, 0x82, 2, 64, 0, 0};
    c.insert(c.end(), ep.begin(), ep.end());
  }
  const Bytes i1 = {9, 4, 1, 0, 0, 0xE0, 0x01, 0x01, 0};
  c.insert(c.end(), i1.begin(), i1.end());
  c[2] = static_cast<uint8_t>(c.size());
  return c;
}

struct Host final : IUsbHostController {
  Controller controller;
  Bytes config = bluetooth_config();
  bool present = true;
  uint32_t generation = 1;
  uint16_t vendor = 0x8087, product = 0x0AAA;
  uint32_t event_offset = 0, acl_offset = 0;
  bool acl_stream = true, command_fails = false;
  uint32_t configurations = 0;

  const char* name() const override { return "fake bluetooth host"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return 1; }
  bool port_info(uint32_t, UsbPortInfo* out) const override { out->connected = present; out->speed = UsbSpeed::Full; return true; }
  bool device_descriptor(uint32_t, UsbDeviceDescriptor* out) const override {
    if (!present) return false;
    memset(out, 0, sizeof *out);
    out->length = 18;
    out->descriptor_type = 1;
    out->device_class = 0xE0;
    out->vendor_id = vendor;
    out->product_id = product;
    out->configuration_count = 1;
    return true;
  }
  bool configuration_descriptor(uint32_t, ByteView buffer, uint16_t* length) const override {
    if (!present || config.size() > buffer.size()) return false;
    for (size_t i = 0; i < config.size(); ++i) buffer[i] = config[i];
    *length = static_cast<uint16_t>(config.size());
    return true;
  }
  bool set_configuration(uint32_t, uint8_t) override { ++configurations; return present; }
  bool control_out(uint32_t, uint8_t, uint8_t, uint16_t, uint16_t) override { return present; }
  bool control_in(uint32_t, uint8_t, uint8_t, uint16_t, uint16_t, ByteView) override { return false; }
  bool control_out_data(uint32_t, uint8_t type, uint8_t request, uint16_t, uint16_t, ConstByteView data) override {
    if (!present || command_fails || type != 0x20 || request != 0) return false;
    controller.command(Bytes(data.data(), data.data() + data.size()));
    return true;
  }
  bool start_interrupt_in(uint32_t, uint8_t endpoint, uint16_t, uint8_t) override { return endpoint == 0x81; }
  bool poll_interrupt_in(uint32_t, uint8_t endpoint, ByteView buffer, uint16_t* length) override {
    if (!present) return false;
    std::deque<Bytes>& queue = endpoint == 0x81 ? controller.events : controller.acl_to_host;
    uint32_t& offset = endpoint == 0x81 ? event_offset : acl_offset;
    const uint32_t piece = endpoint == 0x81 ? 16 : 64;
    if ((endpoint != 0x81 && endpoint != 0x82) || queue.empty()) return false;
    const Bytes& front = queue.front();
    const uint32_t n = static_cast<uint32_t>(front.size() - offset < piece ? front.size() - offset : piece);
    for (uint32_t i = 0; i < n && i < buffer.size(); ++i) buffer[i] = front[offset + i];
    *length = static_cast<uint16_t>(n);
    offset += n;
    if (offset >= front.size()) { queue.pop_front(); offset = 0; }
    return true;
  }
  bool start_bulk_in_stream(uint32_t, uint8_t endpoint, uint16_t) override { return acl_stream && endpoint == 0x82; }
  bool start_bulk_endpoint(uint32_t, uint8_t endpoint, uint16_t) override { return endpoint == 0x02 || endpoint == 0x82; }
  bool bulk_transfer(uint32_t, uint8_t endpoint, ByteView data, uint32_t* transferred) override {
    if (!present || endpoint != 0x02) return false;
    controller.acl(Bytes(data.data(), data.data() + data.size()));
    if (transferred) *transferred = static_cast<uint32_t>(data.size());
    return true;
  }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return true; }
  uint32_t port_generation(uint32_t) const override { return generation; }
  void unplug() { present = false; ++generation; }
};

struct Bus final : IUsbBus {
  std::vector<IUsbHostController*> hosts;
  const char* name() const override { return "fake usb"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return static_cast<uint32_t>(hosts.size()); }
  uint32_t controller_count() const override { return static_cast<uint32_t>(hosts.size()); }
  UsbControllerKind controller_kind(uint32_t) const override { return UsbControllerKind::Xhci; }
  uint32_t port_count(uint32_t c) const override { return hosts[c]->port_count(); }
  bool port_info(uint32_t c, uint32_t p, UsbPortInfo* out) const override { return hosts[c]->port_info(p, out); }
  bool device_descriptor(uint32_t c, uint32_t p, UsbDeviceDescriptor* out) const override { return hosts[c]->device_descriptor(p, out); }
  IUsbHostController* host_controller(uint32_t c) override { return c < hosts.size() ? hosts[c] : nullptr; }
};

}  // namespace fake_bt
