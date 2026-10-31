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

// Host-side tests for Bluetooth: the HCI packets (taken apart, truncated, bit flipped), the USB transport (the interface in the descriptors, packets put together from the pieces of the
// endpoints), the controller (bring-up, inquiry and LE scan, connections, ACL data with the controller's flow control) and the Intel firmware load.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Hardware/Bluetooth/BluetoothService.hpp"
#include "Hardware/Bluetooth/HciController.hpp"
#include "Hardware/Bluetooth/IntelBluetooth.hpp"
#include "Hardware/Bluetooth/UsbHciTransport.hpp"
#include "FakeBluetooth.hpp"

namespace hardware {
void delay_us(uint64_t) {}
}  // namespace hardware

using namespace hardware;
using namespace fake_bt;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    ++live;
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// ------------------------------------------------------------------------------------------------ packets

static void test_packets() {
  using namespace hci;
  uint8_t buffer[300];
  {   // commands
    ByteWriter out{ByteView(buffer)};
    const uint8_t params[2] = {1, 2};
    CHECK(build_command(kOpReset, ConstByteView(params, 2), out) && out.written().size() == 5 && buffer[0] == 0x03 && buffer[1] == 0x0C && buffer[2] == 2 && buffer[3] == 1);
    ByteWriter small{ByteView(buffer, 4)};
    CHECK(!build_command(kOpReset, ConstByteView(params, 2), small));
    std::vector<uint8_t> big(256, 0);
    ByteWriter huge{ByteView(buffer)};
    CHECK(!build_command(kOpReset, ConstByteView(big.data(), big.size()), huge));
    CHECK(opcode(kOgfLink, 1) == 0x0401 && kOpLeSetScanEnable == 0x200C && kOpReadBdAddr == 0x1009);
  }
  {   // events and command complete / status
    const uint8_t cc[] = {0x0E, 6, 1, 0x03, 0x0C, 0x00, 0xAA, 0xBB};
    Event e;
    CommandComplete c;
    CHECK(parse_event(ConstByteView(cc, sizeof cc), &e) && e.code == 0x0E && e.params.size() == 6 && parse_command_complete(e, &c) && c.opcode == 0x0C03 && c.returned.size() == 3 && c.returned[0] == 0);
    CHECK(!parse_event(ConstByteView(cc, 7), &e) && !parse_event(ConstByteView(cc, 1), &e) && !parse_event(ConstByteView(), &e));       // truncated
    const uint8_t cs[] = {0x0F, 4, 0x0C, 1, 0x01, 0x04};
    CommandStatus s;
    CHECK(parse_event(ConstByteView(cs, sizeof cs), &e) && parse_command_status(e, &s) && s.status == 0x0C && s.opcode == 0x0401 && !parse_command_complete(e, &c));
  }
  {   // ACL
    ByteWriter out{ByteView(buffer)};
    const uint8_t data[3] = {7, 8, 9};
    CHECK(build_acl(0x0041, kPbFirstFlushable, ConstByteView(data, 3), out) && out.written().size() == 7 && buffer[0] == 0x41 && buffer[1] == 0x20 && buffer[2] == 3 && buffer[3] == 0);
    AclPacket a;
    CHECK(parse_acl(out.written(), &a) && a.handle == 0x41 && a.boundary == 2 && a.data.size() == 3 && a.data[2] == 9);
    CHECK(!parse_acl(out.written().first(6), &a) && !parse_acl(ConstByteView(buffer, 3), &a));
    ByteWriter bad{ByteView(buffer)};
    CHECK(!build_acl(0x1000, 0, ConstByteView(data, 3), bad));
  }
  {   // inquiry results in the three formats
    Controller c;
    c.inquiry();
    InquiryResult results[8];
    int total = 0;
    for (const Bytes& packet : c.events) {
      Event e;
      CHECK(parse_event(view(packet), &e));
      if (e.code == kEvInquiryComplete || e.code == kEvCommandStatus) continue;
      const int n = parse_inquiry_results(e, ArrayView<InquiryResult>(results));
      CHECK(n >= 1);
      if (e.code == kEvInquiryResultWithRssi) {
        CHECK(n == 2 && results[0].address.b[0] == 1 && results[0].class_of_device == 0x5A020C && results[0].rssi == -60 && results[0].clock_offset == 0x1234);
        CHECK(results[1].address.b[0] == 7 && results[1].class_of_device == 0x240510 && results[1].rssi == -71 && results[1].clock_offset == 0x1678);
      } else if (e.code == kEvExtendedInquiryResult) {
        CHECK(n == 1 && results[0].class_of_device == 0x200404 && results[0].rssi == -50 && find_ad_structure(results[0].eir, kAdName).size() == 9);
      } else {
        CHECK(n == 1 && results[0].rssi == 127 && results[0].class_of_device == 0x000508 && results[0].address.b[0] == 0x21);
      }
      total += n;
    }
    CHECK(total == 4);
  }
  {   // advertising reports, the AD structures
    Controller c;
    c.advertisements();
    Event e;
    AdvertisingReport reports[4];
    CHECK(parse_event(view(c.events[0]), &e) && parse_advertising_reports(e, ArrayView<AdvertisingReport>(reports)) == 1 && reports[0].rssi == -55 && reports[0].address.b[0] == 0xC1);
    CHECK(find_ad_structure(reports[0].data, kAdShortName).size() == 3 && find_ad_structure(reports[0].data, kAdFlags)[0] == 6 && find_ad_structure(reports[0].data, kAdTxPower).empty());
    CHECK(parse_event(view(c.events[1]), &e) && parse_advertising_reports(e, ArrayView<AdvertisingReport>(reports)) == 2 && reports[0].event_type == 4 && reports[1].event_type == 3);
    // an AD structure that runs past the data ends the search
    const uint8_t cut[] = {2, 1, 6, 9, 9, 'a', 'b'};
    CHECK(find_ad_structure(ConstByteView(cut, sizeof cut), kAdName).empty() && find_ad_structure(ConstByteView(cut, sizeof cut), kAdFlags).size() == 1);
  }
  {   // every event of the fake controller, cut short at every length and with every bit flipped: nothing read outside, nothing stored beyond the room
    Controller c;
    c.inquiry();
    c.advertisements();
    c.remote_name(Bytes(6, 1));
    c.push(kEvConnectionComplete, {0, 1, 0, 1, 2, 3, 4, 5, 6, 1, 0});
    c.push(kEvNumberOfCompletedPackets, {1, 1, 0, 2, 0});
    for (const Bytes& packet : c.events) {
      for (size_t cut = 0; cut <= packet.size(); ++cut) {
        Event e;
        InquiryResult results[2];
        AdvertisingReport reports[2];
        ConnectionComplete cc;
        if (parse_event(ConstByteView(packet.data(), cut), &e)) {
          parse_inquiry_results(e, ArrayView<InquiryResult>(results));
          parse_advertising_reports(e, ArrayView<AdvertisingReport>(reports));
          parse_connection_complete(e, &cc);
        }
      }
      for (size_t bit = 0; bit < packet.size() * 8 && bit < 400; ++bit) {
        Bytes flipped = packet;
        flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
        Event e;
        InquiryResult results[2];
        AdvertisingReport reports[2];
        LeConnectionComplete lc;
        DisconnectionComplete dc;
        if (parse_event(view(flipped), &e)) {
          const int n = parse_inquiry_results(e, ArrayView<InquiryResult>(results));
          const int m = parse_advertising_reports(e, ArrayView<AdvertisingReport>(reports));
          CHECK(n <= 2 && m <= 2);
          parse_le_connection_complete(e, &lc);
          parse_disconnection_complete(e, &dc);
        }
      }
    }
  }
}

// ------------------------------------------------------------------------------------------------ the USB side

static void test_usb_transport() {
  TestAllocator a;
  {
    UsbBluetoothInterface i;
    const Bytes good = bluetooth_config();
    CHECK(find_bluetooth_interface(view(good), &i) && i.interface_number == 0 && i.event_endpoint == 0x81 && i.acl_in == 0x82 && i.acl_out == 0x02 && i.event_max_packet == 16 && i.acl_max_packet == 64 && i.configuration == 1);
    const Bytes none = bluetooth_config(false);
    CHECK(!find_bluetooth_interface(view(none), &i));                                       // no endpoints: not usable
    for (size_t n = 0; n < good.size(); ++n) find_bluetooth_interface(ConstByteView(good.data(), n), &i);       // (cut at every length)
    for (size_t bit = 0; bit < good.size() * 8; ++bit) {
      Bytes flipped = good;
      flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      find_bluetooth_interface(view(flipped), &i);
    }
    CHECK(bluetooth_vendor(0x8087, 0x0AAA) == BluetoothVendor::Intel && bluetooth_vendor(0x0A5C, 0x21E8) == BluetoothVendor::Generic);
  }
  {   // events of 40 bytes on 16 byte endpoint packets come out whole; ACL packets on 64 byte ones too
    Host host;
    UniquePointer<UsbHciTransport> t = UsbHciTransport::open(host, 0, a);
    CHECK(t && t->present());
    Bytes event(2 + 38, 0x5A);
    event[0] = 0x3E;
    event[1] = 38;
    host.controller.events.push_back(event);
    host.controller.events.push_back(Bytes{0x05, 4, 0, 1, 0, 0x16});
    uint8_t buffer[300];
    uint16_t length = 0;
    CHECK(t->poll_event(ByteView(buffer), &length) && length == 40 && buffer[0] == 0x3E && buffer[39] == 0x5A);
    CHECK(t->poll_event(ByteView(buffer), &length) && length == 6 && buffer[0] == 0x05);
    CHECK(!t->poll_event(ByteView(buffer), &length));
    Bytes data(300, 0x33);
    host.controller.inject_acl(0x0041, 2, data);
    uint8_t acl[1200];
    CHECK(t->poll_acl(ByteView(acl), &length) && length == 304 && acl[0] == 0x41 && acl[1] == 0x20 && acl[2] == 0x2C && acl[3] == 0x01 && acl[303] == 0x33);
    // a command and ACL data go out
    const uint8_t command[3] = {0x03, 0x0C, 0};
    CHECK(t->send_command(ConstByteView(command, 3)) && host.controller.commands.size() == 1);
    CHECK(!t->send_command(ConstByteView(command, 2)));
    const uint8_t packet[6] = {1, 0x20, 2, 0, 9, 9};
    CHECK(t->send_acl(ConstByteView(packet, 6)) && host.controller.acl_from_host.size() == 1);
    // unplugged
    host.unplug();
    CHECK(!t->present() && !t->send_command(ConstByteView(command, 3)) && !t->poll_event(ByteView(buffer), &length));
  }
  {   // a device without the Bluetooth interface is not taken
    Host host;
    host.config = {9, 2, 18, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 8, 6, 0x50, 0};
    CHECK(!UsbHciTransport::open(host, 0, a));
  }
  {   // a piece that does not belong (more than a packet can hold) is dropped, not stored
    uint8_t storage[8];
    HciReassembler r{ArrayView<uint8_t>(storage), 1, 1};
    const uint8_t big[20] = {0x0E, 30};
    CHECK(!r.push(ConstByteView(big, 20)) && r.dropped() == 1);
    const uint8_t ok[4] = {0x0E, 2, 7, 8};
    CHECK(r.push(ConstByteView(ok, 4)) && r.packet().size() == 4);
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------------------------------------ the controller

struct Rig {
  TestAllocator a;
  Host host;
  UniquePointer<UsbHciTransport> transport;
  UniquePointer<HciController> controller;
  Rig() {
    transport = UsbHciTransport::open(host, 0, a);
    controller = UniquePointer<HciController>::make(a, *transport, a);
  }
  HciController& c() { return *controller; }
};

static void test_initialize() {
  {
    Rig rig;
    CHECK(rig.transport && rig.c().initialize() == HciResult::Ok && rig.c().initialized());
    const HciInfo& i = rig.c().info();
    CHECK(i.address.b[0] == 0x11 && i.address.b[5] == 0x66 && i.hci_version == 9 && i.hci_revision == 0x1234 && i.manufacturer == 2 && i.lmp_subversion == 0x5678);
    CHECK(i.acl_mtu == 27 && i.acl_packets == 3 && i.le_supported && i.simple_pairing && i.le_features[0] == 1);
    // the first thing was a reset; the machine is not left discoverable
    CHECK(rig.host.controller.opcodes[0] == hci::kOpReset && rig.host.controller.scan_enable_writes == 1);
  }
  {   // a controller without LE: no LE commands
    Rig rig;
    rig.host.controller.le = false;
    CHECK(rig.c().initialize() == HciResult::Ok && !rig.c().info().le_supported);
    for (uint16_t op : rig.host.controller.opcodes) CHECK((op >> 10) != hci::kOgfLe);
    CHECK(rig.c().start_le_scan() == HciResult::Status);
  }
  {   // a controller that does not answer
    Rig rig;
    rig.host.controller.silent = true;
    CHECK(rig.c().command(hci::kOpReset, ConstByteView(), 20) == HciResult::Timeout);
    CHECK(rig.c().initialize() == HciResult::Timeout && !rig.c().initialized());
  }
  {   // commands that fail, and a transport that fails
    Rig rig;
    CHECK(rig.c().command(0x0FFF) == HciResult::Status && rig.c().last_status() == hci::kStatusUnknownCommand);
    rig.host.command_fails = true;
    CHECK(rig.c().command(hci::kOpReset) == HciResult::Transport);
    rig.host.command_fails = false;
    rig.host.unplug();
    CHECK(rig.c().command(hci::kOpReset) == HciResult::Transport && !rig.c().present());
  }
  {   // events that arrive while a command waits are handled, a stale answer is ignored
    Rig rig;
    CHECK(rig.c().initialize() == HciResult::Ok);
    rig.host.controller.push(hci::kEvCommandComplete, {1, 0x03, 0x0C, 0});                  // an answer to nothing
    rig.host.controller.push(hci::kEvHardwareError, {0x42});
    CHECK(rig.c().command(hci::kOpReadBdAddr) == HciResult::Ok && rig.c().hardware_errors() == 1 && rig.c().reply().size() == 7);
  }
  {   // garbage events are counted, not believed
    Rig rig;
    CHECK(rig.c().initialize() == HciResult::Ok);
    rig.host.controller.push(hci::kEvCommandComplete, {1});                                 // too short to say anything
    rig.host.controller.push(hci::kEvNumberOfCompletedPackets, {200, 1, 0});               // 200 links in three bytes
    rig.c().poll();
    CHECK(rig.c().malformed() >= 1);
  }
}

static void test_discovery() {
  {   // inquiry: three formats, the names, the table
    Rig rig;
    CHECK(rig.c().initialize() == HciResult::Ok && rig.c().start_inquiry() == HciResult::Ok && rig.c().inquiring());
    rig.c().poll();
    CHECK(!rig.c().inquiring() && rig.c().device_count() == 4);
    hci::BdAddr headset;
    const uint8_t h[6] = {0xA, 0xB, 0xC, 0xD, 0xE, 0xF};
    memcpy(headset.b, h, 6);
    const HciDevice* d = rig.c().find_device(headset);
    CHECK(d && !d->le && d->class_of_device == 0x200404 && d->rssi == -50 && strcmp(d->name, "Headset X") == 0);
    CHECK(rig.c().device(0)->class_of_device == 0x5A020C && rig.c().device(0)->rssi == -60 && rig.c().device(3)->rssi == 127 && !rig.c().device(4));
    // the name of a device that did not give one
    CHECK(rig.c().request_name(rig.c().device(0)->address) == HciResult::Ok);
    rig.c().poll();
    CHECK(strcmp(rig.c().device(0)->name, "Speaker Y") == 0 && strlen(rig.c().device(0)->name) == 9);
    // a refused inquiry
    rig.host.controller.reject_command_status = hci::kStatusCommandDisallowed;
    CHECK(rig.c().start_inquiry() == HciResult::Status && !rig.c().inquiring());
    rig.c().clear_devices();
    CHECK(rig.c().device_count() == 0);
  }
  {   // LE scan: the scan response completes the first device, a beacon is a second, repeats do not add rows
    Rig rig;
    CHECK(rig.c().initialize() == HciResult::Ok && rig.c().start_le_scan() == HciResult::Ok && rig.c().le_scanning());
    rig.c().poll();
    CHECK(rig.c().device_count() == 2);
    const HciDevice* k = rig.c().device(0);
    CHECK(k->le && k->address_type == 1 && k->connectable && strcmp(k->name, "Keyboard") == 0 && k->appearance == 0x03C1 && k->rssi == -53);
    CHECK(!rig.c().device(1)->connectable && rig.c().device(1)->rssi == -80 && rig.c().device(1)->name[0] == 0);
    rig.host.controller.advertisements();
    rig.c().poll();
    CHECK(rig.c().device_count() == 2);
    CHECK(rig.c().stop_le_scan() == HciResult::Ok && !rig.c().le_scanning());
    // the table does not overflow
    for (uint32_t i = 0; i < 100; ++i) {
      Bytes one = {hci::kLeAdvertisingReport, 1};
      const uint8_t addr[6] = {static_cast<uint8_t>(i), 9, 9, 9, 9, 9};
      Bytes r = Controller::report(0, 0, addr, Bytes{}, -40);
      one.insert(one.end(), r.begin(), r.end());
      rig.host.controller.push(hci::kEvLeMeta, one);
    }
    rig.c().poll();
    CHECK(rig.c().device_count() == HciController::kMaxDevices);
  }
}

struct Received {
  std::vector<Bytes> packets;
  std::vector<uint8_t> boundaries;
  static void fn(void* c, const HciLink&, uint8_t boundary, ConstByteView data) {
    Received& r = *static_cast<Received*>(c);
    r.packets.push_back(Bytes(data.data(), data.data() + data.size()));
    r.boundaries.push_back(boundary);
  }
};

static void test_links() {
  {   // a classic link up, data both ways, down
    Rig rig;
    CHECK(rig.c().initialize() == HciResult::Ok);
    hci::BdAddr peer;
    const uint8_t p[6] = {1, 2, 3, 4, 5, 6};
    memcpy(peer.b, p, 6);
    CHECK(rig.c().connect(peer) == HciResult::Ok);
    rig.c().poll();
    const HciLink* link = rig.c().find_link(peer);
    CHECK(link && link->handle == 1 && !link->le && rig.c().link_count() == 1);
    Received got;
    rig.c().set_acl_handler(&Received::fn, &got);
    rig.host.controller.inject_acl(1, 2, Bytes{9, 8, 7});
    rig.host.controller.inject_acl(0x99, 2, Bytes{1});                               // a handle nobody has: dropped
    rig.c().poll();
    CHECK(got.packets.size() == 1 && got.packets[0].size() == 3 && got.boundaries[0] == 2 && rig.c().acl_received() == 2);
    // 100 bytes in fragments of the controller's 27, with 3 buffers: the credits come back as the controller acknowledges
    Bytes data(100);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i);
    CHECK(rig.c().send_acl(1, view(data)) == HciResult::Ok);
    const std::vector<Bytes>& sent = rig.host.controller.acl_from_host;
    CHECK(sent.size() == 4 && sent[0].size() == 4 + 27 && sent[3].size() == 4 + 19);
    CHECK(((sent[0][1] >> 4) & 3) == 2 && ((sent[1][1] >> 4) & 3) == 1 && ((sent[3][1] >> 4) & 3) == 1);
    Bytes joined;
    for (const Bytes& s : sent) joined.insert(joined.end(), s.begin() + 4, s.end());
    CHECK(joined == data);
    // no acknowledgements: after the three buffers it waits and then times out
    rig.host.controller.ack_acl = false;
    rig.host.controller.acl_from_host.clear();
    CHECK(rig.c().send_acl(1, view(data), 30) == HciResult::Timeout && rig.host.controller.acl_from_host.size() <= 3);
    CHECK(rig.c().send_acl(7, view(data)) == HciResult::Malformed && rig.c().send_acl(1, ConstByteView()) == HciResult::Malformed);
    // the link goes down
    CHECK(rig.c().disconnect(1) == HciResult::Ok);
    rig.c().poll();
    CHECK(rig.c().link_count() == 0 && !rig.c().find_link(peer));
  }
  {   // LE link; a controller with separate LE buffers
    Rig rig;
    rig.host.controller.le_mtu = 20;
    rig.host.controller.le_packets = 2;
    CHECK(rig.c().initialize() == HciResult::Ok && rig.c().info().le_mtu == 20 && rig.c().info().le_packets == 2);
    hci::BdAddr peer;
    const uint8_t p[6] = {0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6};
    memcpy(peer.b, p, 6);
    CHECK(rig.c().connect_le(peer, 1) == HciResult::Ok);
    rig.c().poll();
    const HciLink* link = rig.c().find_link(peer);
    CHECK(link && link->le && link->handle == 0x40 && link->address_type == 1 && link->central);
    Bytes data(45, 7);
    CHECK(rig.c().send_acl(0x40, view(data)) == HciResult::Ok && rig.host.controller.acl_from_host.size() == 3 && rig.host.controller.acl_from_host[0].size() == 4 + 20);
    CHECK(rig.c().cancel_connect_le() == HciResult::Status);       // (the fake does not know the command)
    rig.host.controller.connect_status = hci::kStatusMemoryFull;
    CHECK(rig.c().connect_le(peer, 1) == HciResult::Status);
  }
  {   // incoming connections are refused unless the host says otherwise
    Rig rig;
    CHECK(rig.c().initialize() == HciResult::Ok);
    rig.host.controller.push(hci::kEvConnectionRequest, {9, 9, 9, 9, 9, 9, 4, 4, 0x20, 1});
    rig.c().poll();
    CHECK(rig.host.controller.last_reject.size() == 7 && rig.host.controller.last_reject[0] == 9 && rig.host.controller.last_reject[6] == 0x0F && rig.c().incoming_refused() == 1 && rig.host.controller.last_accept.empty());
    rig.c().accept_incoming(true);
    rig.host.controller.push(hci::kEvConnectionRequest, {8, 8, 8, 8, 8, 8, 4, 4, 0x20, 1});
    rig.c().poll();
    CHECK(rig.host.controller.last_accept.size() == 7 && rig.host.controller.last_accept[0] == 8 && rig.c().incoming_refused() == 1);
  }
}

// ------------------------------------------------------------------------------------------------ Intel

// A signed image: 644 bytes of header, then a run of commands in fragments that end on 4 byte boundaries (the last of them Write Boot Params with the address 0x00FC0100).
static Bytes intel_image(uint32_t boot_address = 0x00FC0100, bool cut = false) {
  Bytes image(644);
  for (size_t i = 0; i < image.size(); ++i) image[i] = static_cast<uint8_t>(i * 3 + 1);
  auto command = [&](uint16_t op, const Bytes& params) {
    image.push_back(static_cast<uint8_t>(op));
    image.push_back(static_cast<uint8_t>(op >> 8));
    image.push_back(static_cast<uint8_t>(params.size()));
    image.insert(image.end(), params.begin(), params.end());
  };
  command(0xFC8E, Bytes(5, 0x11));              // 3 + 5 = 8 bytes
  command(0xFC8E, Bytes(1, 0x22));              // 4: aligned again
  command(0xFC0E, {static_cast<uint8_t>(boot_address), static_cast<uint8_t>(boot_address >> 8), static_cast<uint8_t>(boot_address >> 16), static_cast<uint8_t>(boot_address >> 24), 5, 6, 7, 8, 9});   // 12 bytes
  command(0xFC8E, Bytes(2, 0x33));              // 5: not aligned...
  command(0xFC8E, Bytes(0));                    // ...3 more: 8
  if (cut) image.resize(image.size() - 2);
  return image;
}

static void test_intel() {
  {   // the bootloader asks for ibt-17-10-2.sfi; the image goes in; the new firmware boots
    Rig rig;
    rig.host.controller.intel = true;
    IntelBluetooth intel(rig.c());
    CHECK(intel.identify() == IntelSetup::NeedFirmware && intel.version().hw_variant == 0x11 && intel.version().fw_variant == 0x06 && strcmp(intel.firmware_name(), "intel/ibt-17-10-2.sfi") == 0);
    const Bytes image = intel_image();
    rig.host.controller.payload_expected = static_cast<uint32_t>(image.size() - 644);
    CHECK(intel.load_firmware(view(image)) == IntelSetup::Ok && intel.operational());
    // the header went in as three fragments (init 128, public key 256, signature 256), then the payload in fragments that end on four bytes
    const std::vector<Bytes>& f = rig.host.controller.intel_fragments;
    CHECK(f.size() >= 6 && f[0][0] == 0x00 && f[0].size() == 1 + 128 && f[1][0] == 0x03 && f[1].size() == 1 + 252 && f[2][0] == 0x03 && f[2].size() == 1 + 4);
    uint32_t sign = 0, data = 0;
    for (const Bytes& x : f) { sign += x[0] == 0x02 ? static_cast<uint32_t>(x.size() - 1) : 0; data += x[0] == 0x01 ? static_cast<uint32_t>(x.size() - 1) : 0; }
    CHECK(sign == 256 && data == image.size() - 644);
    for (const Bytes& x : f) if (x[0] == 0x01) CHECK((x.size() - 1) % 4 == 0);
    // Intel Reset went out with the boot address from the image
    bool reset_seen = false;
    for (const Bytes& c : rig.host.controller.commands)
      if (c.size() == 3 + 8 && c[0] == 0x01 && c[1] == 0xFC) { reset_seen = true; CHECK(c[3 + 4] == 0x00 && c[3 + 5] == 0x01 && c[3 + 6] == 0xFC && c[3 + 7] == 0x00); }
    CHECK(reset_seen);
    // and the generic bring-up works on top
    CHECK(rig.c().initialize() == HciResult::Ok);
  }
  {   // operational firmware, an address that is Intel's default
    Rig rig;
    rig.host.controller.intel = true;
    rig.host.controller.intel_operational = true;
    const uint8_t def[6] = {0x00, 0x8B, 0x9E, 0x19, 0x03, 0x00};
    memcpy(rig.host.controller.address, def, 6);
    IntelBluetooth intel(rig.c());
    CHECK(intel.identify() == IntelSetup::AlreadyRunning && intel.operational() && !intel.address_valid());
    CHECK(intel.load_firmware(view(intel_image())) == IntelSetup::AlreadyRunning);
  }
  {   // refusals: another variant, a newer-format controller, a damaged image, a bootloader that rejects the image, one that never boots
    Rig rig;
    rig.host.controller.intel = true;
    rig.host.controller.hw_variant = 0x17;
    IntelBluetooth other(rig.c());
    CHECK(other.identify() == IntelSetup::UnsupportedVariant);
    Rig plain;
    IntelBluetooth generic(plain.c());
    CHECK(generic.identify() == IntelSetup::Failed);                               // (the command is unknown to it)
    Rig rig2;
    rig2.host.controller.intel = true;
    IntelBluetooth intel(rig2.c());
    CHECK(intel.load_firmware(view(intel_image())) == IntelSetup::Failed);         // (not identified yet)
    CHECK(intel.identify() == IntelSetup::NeedFirmware);
    const Bytes tiny(100, 1);
    CHECK(intel.load_firmware(view(tiny)) == IntelSetup::BadFirmware);
    Bytes cut = intel_image(0x00FC0100, true);
    CHECK(intel.load_firmware(view(cut)) == IntelSetup::BadFirmware);
    Bytes no_boot(644 + 8, 0);
    no_boot[644] = 0x8E; no_boot[645] = 0xFC; no_boot[646] = 5;
    CHECK(intel.load_firmware(view(no_boot)) == IntelSetup::BadFirmware);
    for (size_t n = 0; n < 644 + 40; n += 7) intel.load_firmware(ConstByteView(intel_image().data(), n));         // (cut anywhere: no crash)
    const Bytes image = intel_image();
    rig2.host.controller.payload_expected = static_cast<uint32_t>(image.size() - 644);
    rig2.host.controller.payload_received = 0;
    rig2.host.controller.intel_fail_download = true;
    CHECK(intel.load_firmware(view(image)) == IntelSetup::Failed);
    rig2.host.controller.intel_fail_download = false;
    rig2.host.controller.payload_received = 0;
    rig2.host.controller.intel_no_bootup = true;
    CHECK(intel.load_firmware(view(image)) == IntelSetup::Timeout);
  }
}

struct Files final : IBluetoothFirmwareSource {
  Bytes content;
  std::vector<std::string> asked;
  bool read(const char* name, ByteBuffer& out) override {
    asked.push_back(name);
    if (content.empty()) return false;
    out.clear();
    return out.append(ConstByteView(content.data(), content.size()));
  }
};

static void test_service() {
  TestAllocator a;
  {   // a generic controller is brought up; one that is not Bluetooth is left alone
    Host host;
    host.vendor = 0x0A5C;
    host.product = 0x21E8;
    Host other;
    other.config = {9, 2, 18, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 8, 6, 0x50, 0};
    Bus bus;
    bus.hosts = {&other, &host};
    BluetoothService service(&bus, nullptr, a);
    service.start();
    CHECK(service.count() == 1 && service.ready_count() == 1 && service.added() == 1 && service.entry(0)->state == BluetoothState::Ready && service.entry(0)->bus_controller == 1);
    HciController* c = service.controller(0);
    CHECK(c && c->initialized() && !service.controller(1));
    // events keep flowing through step()
    CHECK(c->start_le_scan() == HciResult::Ok);
    service.step(false);
    CHECK(c->device_count() == 2);
    // unplugged: dropped on the next scan; plugged in again: taken
    host.unplug();
    service.step(true);
    CHECK(service.count() == 0 && service.removed() == 1);
    host.config = bluetooth_config();
    host.present = true;
    ++host.generation;
    service.step(true);
    CHECK(service.count() == 1 && service.ready_count() == 1 && service.added() == 2);
  }
  {   // an Intel controller: the file it asks for is read from the firmware source and loaded
    Host host;
    host.controller.intel = true;
    Files files;
    files.content = intel_image();
    host.controller.payload_expected = static_cast<uint32_t>(files.content.size() - 644);
    Bus bus;
    bus.hosts = {&host};
    BluetoothService service(&bus, &files, a);
    service.start();
    CHECK(service.count() == 1 && service.ready_count() == 1 && files.asked.size() == 1 && files.asked[0] == "intel/ibt-17-10-2.sfi" && service.entry(0)->intel->operational());
  }
  {   // no file: the entry says which one it needs; a damaged file; no source at all
    Host host;
    host.controller.intel = true;
    Files files;
    Bus bus;
    bus.hosts = {&host};
    BluetoothService missing(&bus, &files, a);
    missing.start();
    CHECK(missing.count() == 1 && missing.ready_count() == 0 && missing.entry(0)->state == BluetoothState::FirmwareMissing && strcmp(missing.entry(0)->firmware, "intel/ibt-17-10-2.sfi") == 0 && !missing.controller(0));
    Host host2;
    host2.controller.intel = true;
    Files damaged;
    damaged.content = Bytes(300, 1);
    Bus bus2;
    bus2.hosts = {&host2};
    BluetoothService bad(&bus2, &damaged, a);
    bad.start();
    CHECK(bad.count() == 1 && bad.entry(0)->state == BluetoothState::Failed);
    Host host3;
    host3.controller.intel = true;
    Bus bus3;
    bus3.hosts = {&host3};
    BluetoothService none(&bus3, nullptr, a);
    none.start();
    CHECK(none.count() == 1 && none.entry(0)->state == BluetoothState::FirmwareMissing);
    BluetoothService nothing(nullptr, nullptr, a);
    nothing.start();
    nothing.step(true);
    CHECK(nothing.count() == 0);
  }
  CHECK(a.live == 0);
}

// The real firmware files of the repository (intel/ibt-17-*.sfi of linux-firmware): the load walks them whole - every fragment ends on four bytes, the boot address comes out of the last
// command - and a file cut or damaged anywhere is refused or loaded without a crash. Skipped when the file is not there (the tests run from the repository root).
static void test_intel_real_firmware() {
  const char* names[] = {"firmware/ibt-17-16-1.sfi", "firmware/ibt-17-0-1.sfi"};
  for (const char* name : names) {
    FILE* f = fopen(name, "rb");
    if (!f) continue;
    Bytes image;
    uint8_t chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) image.insert(image.end(), chunk, chunk + n);
    fclose(f);
    CHECK(image.size() > 600000 && image[0] == 6);
    Rig rig;
    rig.host.controller.intel = true;
    IntelBluetooth intel(rig.c());
    CHECK(intel.identify() == IntelSetup::NeedFirmware);
    rig.host.controller.payload_expected = static_cast<uint32_t>(image.size() - 644);
    CHECK(intel.load_firmware(view(image)) == IntelSetup::Ok && intel.operational());
    uint32_t data_fragments = 0, data_bytes = 0;
    for (const Bytes& x : rig.host.controller.intel_fragments)
      if (x[0] == 0x01) { ++data_fragments; data_bytes += static_cast<uint32_t>(x.size() - 1); CHECK((x.size() - 1) % 4 == 0 && x.size() <= 253); }
    CHECK(data_bytes == image.size() - 644 && data_fragments > 2000);
    bool reset_seen = false;
    for (const Bytes& c : rig.host.controller.commands)
      if (c.size() == 3 + 8 && c[0] == 0x01 && c[1] == 0xFC) { reset_seen = true; CHECK(c[3 + 4] == 0x00 && c[3 + 5] == 0x08 && c[3 + 6] == 0x04 && c[3 + 7] == 0x00); }       // 0x00040800
    CHECK(reset_seen);
    // cut at 1000 places and with 300 bits flipped in the header and the first commands: no crash, and a cut file is refused
    for (size_t cut = 0; cut < image.size(); cut += image.size() / 1000 + 1) {
      Rig r;
      r.host.controller.intel = true;
      IntelBluetooth i(r.c());
      i.identify();
      const IntelSetup result = i.load_firmware(ConstByteView(image.data(), cut));
      CHECK(cut >= image.size() - 3 ? true : result == IntelSetup::BadFirmware);
    }
    for (size_t bit = 0; bit < 300 * 8; bit += 8 * 7 + 3) {
      Bytes flipped = image;
      flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      Rig r;
      r.host.controller.intel = true;
      IntelBluetooth i(r.c());
      i.identify();
      i.load_firmware(view(flipped));
    }
  }
}

int main() {
  test_packets();
  test_usb_transport();
  test_initialize();
  test_discovery();
  test_links();
  test_intel();
  test_service();
  test_intel_real_firmware();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("bluetooth tests passed\n");
  return 0;
}
