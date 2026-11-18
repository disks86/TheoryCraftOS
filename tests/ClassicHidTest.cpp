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

// Classic Bluetooth HID: the adapter's BR/EDR connections and pairing (Secure Simple Pairing, legacy PIN, stored link keys), the SDP client and its data element reader, and
// the HIDP host (channels, input, output, feature reports, virtual cable unplug), with keyboards going to the input system and pads to the game controller service. The other
// side is tests/FakeBluetooth.hpp's controller with a ClassicLmp and tests/FakeClassicDevice.hpp.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "FakeClassicDevice.hpp"
#include "Hardware/Bluetooth/BluetoothService.hpp"
#include "Hardware/Bluetooth/HciBluetoothAdapter.hpp"
#include "Hardware/Bluetooth/HidpHost.hpp"
#include "Hardware/Bluetooth/Sdp.hpp"
#include "Hardware/GameController/GameControllerService.hpp"
#include "Hardware/Keyboard/CompositeKeyboard.hpp"
#include "Hardware/Mouse/CompositeMouse.hpp"

namespace hardware {
void delay_us(uint64_t) {}
}  // namespace hardware

using namespace hardware;
using namespace fake_bt;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
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

// A keyboard (report id 1) and a mouse (report id 2).
static const uint8_t kKeyboardMouseMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0};
// A boot-style keyboard without report ids.
static const uint8_t kPlainKeyboardMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0};
// A gamepad collection (the DualShock 4 is recognised by its ids; this map is what an unknown pad would be judged by).
static const uint8_t kGamepadMap[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04,
    0x81, 0x02, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0E, 0x81, 0x02, 0x75, 0x02, 0x95, 0x01, 0x81, 0x01, 0xC0};

struct TestSink final : IInputSink {
  CompositeKeyboard keyboards;
  CompositeMouse mice;
  std::vector<std::string> notes;
  bool add_keyboard(IKeyboard* k) override { return keyboards.add(k); }
  bool remove_keyboard(IKeyboard* k) override { return keyboards.remove(k); }
  bool add_mouse(IMouse* m) override { return mice.add(m); }
  bool remove_mouse(IMouse* m) override { return mice.remove(m); }
  void notify(StringView text) override { notes.push_back(std::string(text.data(), text.size())); }
  bool noted(const char* what) const { for (const std::string& n : notes) if (n.find(what) != std::string::npos) return true; return false; }
  std::vector<KeyEvent> drain_keys() { std::vector<KeyEvent> v; KeyEvent e; while (keyboards.poll(&e)) v.push_back(e); return v; }
  std::vector<MouseEvent> drain_mouse() { std::vector<MouseEvent> v; MouseEvent e; while (mice.poll(&e)) v.push_back(e); return v; }
};

struct Ui final : IBluetoothPairingUi {
  uint32_t compared = 0, displayed = 0, asked = 0, comparisons = 0;
  void display_passkey(const hci::BdAddr&, uint32_t passkey) override { displayed = passkey; }
  void confirm_numeric(const hci::BdAddr&, uint32_t value) override { compared = value; ++comparisons; }
  void request_passkey(const hci::BdAddr&) override { ++asked; }
};

struct Rig {
  TestAllocator a;
  Host host;
  ClassicLmp lmp;
  UniquePointer<UsbHciTransport> transport;
  UniquePointer<HciController> c;
  UniquePointer<L2cap> l2;
  UniquePointer<AttClient> att;
  UniquePointer<GattClient> gatt;
  SmBondStore bonds;
  BluetoothProfileRegistry profiles;
  UniquePointer<HidpHost> hidp;
  UniquePointer<GameControllerService> pads;
  UniquePointer<HciBluetoothAdapter> adapter;
  UniquePointer<ClassicDevice> dev;
  TestSink sink;
  Ui ui;

  static void acl_hook(void* self, const Bytes& packet) { static_cast<Rig*>(self)->dev->on_acl(packet); }

  explicit Rig(const uint8_t* map, size_t map_size, bool with_pads = true) {
    transport = UsbHciTransport::open(host, 0, a);
    c = UniquePointer<HciController>::make(a, *transport, a);
    CHECK(c->initialize() == HciResult::Ok);
    l2 = UniquePointer<L2cap>::make(a, *c);
    att = UniquePointer<AttClient>::make(a, *c, *l2);
    gatt = UniquePointer<GattClient>::make(a, *att);
    hidp = UniquePointer<HidpHost>::make(a, a);
    if (with_pads) {
      pads = UniquePointer<GameControllerService>::make(a, nullptr, a);
      hidp->set_game_controllers(pads.get());
    }
    profiles.add(hidp.get());
    adapter = UniquePointer<HciBluetoothAdapter>::make(a, *c, *l2, *att, *gatt, nullptr, bonds, profiles, a);
    dev = UniquePointer<ClassicDevice>::make(a, host.controller, lmp.handle);
    dev->report_map = Bytes(map, map + map_size);
    host.controller.lmp = &lmp;
    host.controller.remote_name_text = "Wireless Controller";
    host.controller.acl_hook = &Rig::acl_hook;
    host.controller.hook_context = this;
  }
  ~Rig() {
    adapter.reset();
    hidp.reset();
    pads.reset();
  }
  hci::BdAddr peer() const { hci::BdAddr b; memcpy(b.b, lmp.address, 6); return b; }
  // The device answers an inquiry (it is in pairing mode).
  void discover(uint32_t class_of_device = 0x002508) {
    Bytes p = {1};
    p.insert(p.end(), lmp.address, lmp.address + 6);
    p.insert(p.end(), {1, 0, 0, static_cast<uint8_t>(class_of_device), static_cast<uint8_t>(class_of_device >> 8), static_cast<uint8_t>(class_of_device >> 16), 0, 0});
    host.controller.push(hci::kEvInquiryResult, p);
    c->poll();
  }
  void step() {
    adapter->poll();
    hidp->poll(sink);
  }
  bool run_until(BluetoothLinkState wanted, int limit = 400) {
    for (int i = 0; i < limit; ++i) {
      step();
      BluetoothDeviceInfo info;
      if (adapter->find_device(peer(), &info) && info.state == wanted) return true;
    }
    return false;
  }
  template <typename F> bool run_until(F done, int limit = 400) {
    for (int i = 0; i < limit; ++i) {
      if (done()) return true;
      step();
    }
    return done();
  }
  BluetoothDeviceInfo info() { BluetoothDeviceInfo i; adapter->find_device(peer(), &i); return i; }
  uint32_t opcode_count(uint16_t op) const { uint32_t n = 0; for (uint16_t o : host.controller.opcodes) n += o == op; return n; }
  // Runs the game controller service's round on its own thread while this one polls the adapter (a pad's first round reads a feature report, which the adapter's thread answers).
  void pads_step() {
    std::atomic<bool> done{false};
    std::thread t([&] { pads->step(true); done = true; });   // (with the scan: pads that left are dropped)
    for (int i = 0; i < 2000 && !done; ++i) step();
    t.join();
  }
};

// ------------------------------------------------------------------------------------------------ SDP data elements

static void test_sdp_elements() {
  using namespace sdp;
  // a record: { 0x0001: {uuid 0x1124}, 0x0206: {{0x22, "descriptor"}}, 0x0201: 0x054C }
  Bytes descriptor(kGamepadMap, kGamepadMap + sizeof kGamepadMap);
  Bytes text;
  ClassicDevice::element_header(text, 4, descriptor.size());
  ClassicDevice::append(text, descriptor);
  Bytes entry = ClassicDevice::u8e(0x22);
  ClassicDevice::append(entry, text);
  Bytes record;
  ClassicDevice::append(record, ClassicDevice::u16e(0x0001));
  ClassicDevice::append(record, ClassicDevice::seq(ClassicDevice::uuid16(0x1124)));
  ClassicDevice::append(record, ClassicDevice::u16e(0x0206));
  ClassicDevice::append(record, ClassicDevice::seq(ClassicDevice::seq(entry)));
  ClassicDevice::append(record, ClassicDevice::u16e(0x0201));
  ClassicDevice::append(record, ClassicDevice::u16e(0x054C));
  const Bytes lists = ClassicDevice::seq(ClassicDevice::seq(record));
  CHECK(valid(view(lists)));
  Element value;
  ConstByteView map;
  uint32_t number = 0;
  CHECK(find_in_records(view(lists), kAttributeHidDescriptorList, &value) && hid_report_descriptor(value, &map) && map.size() == sizeof kGamepadMap && memcmp(map.data(), kGamepadMap, map.size()) == 0);
  CHECK(find_in_records(view(lists), kAttributeVendorId, &value) && to_uint(value, &number) && number == 0x054C);
  CHECK(!find_in_records(view(lists), kAttributeProductId, &value));
  // sizes: each kind with the sizes it may have, and those it may not
  {
    const Bytes ok[] = {{0x00}, {0x08, 1}, {0x09, 1, 2}, {0x0A, 1, 2, 3, 4}, {0x0B, 1, 2, 3, 4, 5, 6, 7, 8}, {0x19, 1, 2}, {0x1A, 1, 2, 3, 4}, {0x28, 1}, {0x25, 2, 'h', 'i'},
                        {0x26, 0, 1, 'x'}, {0x27, 0, 0, 0, 0}, {0x35, 0}, {0x3D, 0}, {0x45, 0}};
    for (const Bytes& b : ok) { ByteReader in(view(b)); Element e; CHECK(read_element(in, &e) && in.at_end()); }
    const Bytes bad[] = {{0x01}, {0x1B, 1, 2, 3, 4, 5, 6, 7, 8}, {0x29, 1, 2}, {0x20, 'x'}, {0x30}, {0x0D, 1}, {0x48, 0}, {0x25, 3, 'h', 'i'}, {0x26, 0}, {0x09, 1}, {0xF8}, {}};
    for (const Bytes& b : bad) { ByteReader in(view(b)); Element e; CHECK(!read_element(in, &e)); }
  }
  // nesting: deeper than the limit is refused, the limit itself is fine
  {
    Bytes deep = ClassicDevice::u8e(1);
    for (uint32_t i = 0; i < kMaxDepth; ++i) deep = ClassicDevice::seq(deep);
    CHECK(valid(view(deep)));
    deep = ClassicDevice::seq(deep);
    CHECK(!valid(view(deep)));
  }
  // every truncation of the good answer is refused or read safely, and so is every bit flipped (the sanitizers watch the reads)
  for (size_t n = 0; n < lists.size(); ++n) {
    const Bytes cut(lists.begin(), lists.begin() + n);
    CHECK(!valid(view(cut)) || n == 0);
    Element e;
    ConstByteView m;
    if (find_in_records(view(cut), kAttributeHidDescriptorList, &e)) (void)hid_report_descriptor(e, &m);
  }
  uint32_t still_valid = 0;
  for (size_t byte = 0; byte < lists.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      Bytes flipped = lists;
      flipped[byte] ^= static_cast<uint8_t>(1 << bit);
      still_valid += valid(view(flipped));
      Element e;
      ConstByteView m;
      uint32_t v = 0;
      if (find_in_records(view(flipped), kAttributeHidDescriptorList, &e) && hid_report_descriptor(e, &m)) CHECK(m.size() <= flipped.size());
      if (find_in_records(view(flipped), kAttributeVendorId, &e)) (void)to_uint(e, &v);
    }
  }
  CHECK(still_valid > 0 && still_valid < lists.size() * 8);
  // an attribute id that is not a 16-bit integer ends the walk
  {
    Bytes r = {0x08, 0x06};
    ClassicDevice::append(r, ClassicDevice::u8e(1));
    const Bytes l = ClassicDevice::seq(r);
    Element list;
    ByteReader in(view(l));
    CHECK(read_element(in, &list) && !find_attribute(list, 6, &value));
  }
}

// ------------------------------------------------------------------------------------------------ the bond file

struct MemoryBonds final : IBluetoothBondStorage {
  Bytes stored;
  bool load(ByteBuffer& out) override { out.clear(); if (stored.empty()) return false; return out.append(view(stored)); }
  bool save(ConstByteView bytes) override { stored.assign(bytes.data(), bytes.data() + bytes.size()); return true; }
};


static void test_bond_file() {
  TestAllocator a;
  SmBondStore store;
  ClassicBond k;
  for (int i = 0; i < 6; ++i) k.address.b[i] = static_cast<uint8_t>(0x10 + i);
  for (int i = 0; i < 16; ++i) k.key[i] = static_cast<uint8_t>(i * 7);
  k.key_type = 5;
  k.authenticated = true;
  const uint32_t before = store.revision();
  CHECK(store.store_classic(k) && store.classic_count() == 1 && store.revision() != before && store.count() == 0);
  ClassicBond k2 = k;
  k2.address.b[0] = 0x99;
  k2.authenticated = false;
  k2.key_type = 4;
  CHECK(store.store_classic(k2) && store.store_classic(k2) && store.classic_count() == 2);
  SmBond le;
  le.address.b[0] = 0xC1;
  le.has_ltk = true;
  CHECK(store.store(le));
  ByteBuffer bytes(a);
  CHECK(store.serialize(bytes) && bytes.put_tag("BTST") && bytes.put8(1) && store.serialize_classic(bytes));
  // a reader that knows only the LE bonds and the settings takes those and stops
  SmBondStore old;
  CHECK(old.parse(bytes.view()) == 1 && old.classic_count() == 0);
  const uint64_t at = SmBondStore::kHeaderBytes + SmBondStore::kRecordBytes + 5;
  const ConstByteView section = bytes.view().subview(at);
  SmBondStore again;
  CHECK(again.parse_classic(section) == 2);
  const ClassicBond* got = again.find_classic(k.address);
  CHECK(got && got->authenticated && got->key_type == 5 && memcmp(got->key, k.key, 16) == 0);
  got = again.find_classic(k2.address);
  CHECK(got && !got->authenticated && got->key_type == 4);
  // truncated: only whole records are taken; flipped bits: never a crash, never more keys than records
  for (uint64_t n = 0; n < section.size(); ++n) {
    SmBondStore cut;
    const uint32_t taken = cut.parse_classic(section.first(n));
    CHECK(n < 7 ? taken == 0 : taken == (n - 7) / SmBondStore::kClassicRecordBytes);
  }
  for (uint64_t byte = 0; byte < section.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      Bytes flipped(section.data(), section.data() + section.size());
      flipped[byte] ^= static_cast<uint8_t>(1 << bit);
      SmBondStore f;
      CHECK(f.parse_classic(view(flipped)) <= 2);
    }
  }
  // a later version with longer records is read as far as this one knows them; records shorter than this version's are refused
  {
    Bytes v2 = {'B', 'T', 'L', 'K', 2, 30, 1};
    v2.insert(v2.end(), k.address.b, k.address.b + 6);
    v2.insert(v2.end(), k.key, k.key + 16);
    v2.insert(v2.end(), {5, 1, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE});
    SmBondStore v;
    CHECK(v.parse_classic(view(v2)) == 1 && v.find_classic(k.address) && v.find_classic(k.address)->authenticated);
    v2[5] = 20;
    SmBondStore w;
    CHECK(w.parse_classic(view(v2)) == 0);
  }
  // the service loads the section with the rest and writes it back
  MemoryBonds storage;
  storage.stored.assign(bytes.view().data(), bytes.view().data() + bytes.size());
  {
    BluetoothService service(nullptr, nullptr, a, nullptr, &storage);
    service.start();
    CHECK(service.auto_reconnect() && service.bonds().count() == 1 && service.bonds().classic_count() == 2);
    CHECK(service.bonds().remove_classic(k2.address) && service.save_bonds());
  }
  {
    BluetoothService service(nullptr, nullptr, a, nullptr, &storage);
    service.start();
    CHECK(service.bonds().count() == 1 && service.bonds().classic_count() == 1 && service.bonds().find_classic(k.address) && service.auto_reconnect());
  }
  // an old file without the section: no classic keys, the rest as before
  storage.stored.resize(at);
  {
    BluetoothService service(nullptr, nullptr, a, nullptr, &storage);
    service.start();
    CHECK(service.bonds().count() == 1 && service.bonds().classic_count() == 0 && service.auto_reconnect());
  }
}

// ------------------------------------------------------------------------------------------------ keyboards and mice

static void test_keyboard_just_works() {
  Rig r(kKeyboardMouseMap, sizeof kKeyboardMouseMap, false);
  r.host.controller.remote_name_text = "Keyboard K";
  r.dev->pnp_record = false;
  r.discover(0x002540);
  BluetoothDeviceInfo info = r.info();
  CHECK(!info.le && info.class_of_device == 0x002540 && !info.paired && info.state == BluetoothLinkState::Disconnected);
  CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.adapter->connect(r.peer()) == BluetoothResult::Busy);
  CHECK(r.run_until(BluetoothLinkState::Connected));
  info = r.info();
  CHECK(r.lmp.host_io == 3 && r.lmp.host_authentication == 0x04 && r.lmp.pairings == 1 && r.lmp.confirmations == 1 && r.lmp.encryptions == 1 && r.lmp.negative_key_replies == 1);
  const ClassicBond* bond = r.bonds.find_classic(r.peer());
  CHECK(bond && !bond->authenticated && bond->key_type == 4 && memcmp(bond->key, r.lmp.key, 16) == 0);
  CHECK(info.paired && !info.authenticated && strcmp(info.profiles, "HID") == 0 && strcmp(info.name, "Keyboard K") == 0);
  CHECK(r.run_until([&] { return r.hidp->sessions() == 1 && r.sink.keyboards.count() == 1 && r.sink.mice.count() == 1; }));
  CHECK(r.sink.noted("Keyboard K: connected") && r.hidp->pads() == 0);
  CHECK(r.dev->sdp_requests.size() > 2);                                           // (the answer came in pieces: the continuation state was followed)
  CHECK(r.host.controller.scan_enable == 0x02);                                    // (a classic bond: page scan, for it to come back)
  CHECK(r.opcode_count(hci::kOpWriteDefaultLinkPolicy) == 1);
  // keys and the pointer
  r.dev->input({1, 0x02, 0, 0x04, 0, 0, 0, 0, 0});
  r.step();
  std::vector<KeyEvent> keys = r.sink.drain_keys();
  CHECK(keys.size() == 2 && keys[0].usage == 0xE1 && keys[0].pressed && keys[1].usage == 4 && keys[1].pressed);
  r.dev->input({2, 0x01, 5, 0xFD, 1});
  r.dev->input({7, 1, 2, 3});                                                       // (an id the map does not have)
  r.dev->input({});                                                                 // (nothing after the header)
  r.dev->send_on(0x13, {0xA2, 1});                                                  // (not an input report)
  r.step();
  std::vector<MouseEvent> mouse = r.sink.drain_mouse();
  CHECK(mouse.size() == 1 && mouse[0].buttons == 1 && mouse[0].dx == 5 && mouse[0].dy == -3 && mouse[0].wheel == 1);
  CHECK(r.hidp->malformed() == 2 && r.hidp->input_reports() == 4);
  // a held key is released when the device goes; the bond stays
  r.dev->input({1, 0, 0, 0x07, 0, 0, 0, 0, 0});
  r.step();
  r.sink.drain_keys();
  CHECK(r.adapter->disconnect(r.peer()) == BluetoothResult::Ok);
  r.step();
  keys = r.sink.drain_keys();
  CHECK(keys.size() >= 1 && keys.back().usage == 7 && !keys.back().pressed && r.sink.keyboards.count() == 0 && r.sink.mice.count() == 0 && r.sink.noted("disconnected"));
  CHECK(r.info().state == BluetoothLinkState::Disconnected && r.info().paired && r.hidp->sessions() == 0);

  // again: the stored key is given, nothing is paired
  CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until(BluetoothLinkState::Connected));
  CHECK(r.lmp.key_replies == 1 && r.lmp.pairings == 1 && r.lmp.encryptions == 2);
  CHECK(r.run_until([&] { return r.hidp->sessions() == 1 && r.sink.keyboards.count() == 1; }));
  // the device closes its interrupt channel (it is switching off): the session ends and the link is dropped
  r.dev->close_channel(0x13);
  CHECK(r.run_until(BluetoothLinkState::Disconnected) && r.hidp->sessions() == 0);
  r.step();
  CHECK(r.sink.keyboards.count() == 0);

  // the device forgot the key (it was reset): the stale key is refused, deleted, and the device paired again
  r.lmp.has_key = false;
  CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until(BluetoothLinkState::Connected));
  CHECK(r.lmp.key_replies == 2 && r.lmp.negative_key_replies == 2 && r.lmp.pairings == 2);
  bond = r.bonds.find_classic(r.peer());
  CHECK(bond && memcmp(bond->key, r.lmp.key, 16) == 0);
  CHECK(r.adapter->disconnect(r.peer()) == BluetoothResult::Ok);
  r.step();

  // it reconnects by itself and does not open the HID channels within the wait: the host opens them
  r.host.controller.complete_accepted = true;
  r.host.controller.incoming_request(0x002540);
  r.step();
  CHECK(r.host.controller.last_accept.size() == 7 && r.host.controller.last_accept[6] == 0x00);    // (accepted with a role switch)
  CHECK(r.run_until(BluetoothLinkState::Connected));
  CHECK(r.run_until([&] { return r.hidp->sessions() == 1; }, 200) && r.dev->open(0x11) && r.dev->open(0x13));
  CHECK(r.lmp.pairings == 2);
  // forgetting it: the key goes, page scan stops
  CHECK(r.adapter->forget(r.peer()) == BluetoothResult::Ok && !r.bonds.find_classic(r.peer()));
  r.step();
  r.step();
  CHECK(r.host.controller.scan_enable == 0x00 && r.hidp->sessions() == 0);
  // and a connection request from it is now refused
  r.host.controller.incoming_request(0x002540);
  r.step();
  CHECK(r.host.controller.last_reject.size() == 7 && r.host.controller.last_reject[6] == 0x0F);
}

static void test_pairing_methods() {
  {   // numeric comparison with a UI: the number is shown and the answer given
    Rig r(kPlainKeyboardMap, sizeof kPlainKeyboardMap, false);
    r.lmp.io_capability = 1;
    r.adapter->set_pairing_ui(&r.ui);
    r.discover(0x002540);
    CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok);
    CHECK(r.run_until([&] { return r.ui.comparisons == 1; }) && r.ui.compared == 123456 && r.lmp.host_io == 1);
    CHECK(r.info().state == BluetoothLinkState::Pairing);
    r.adapter->confirm(r.peer(), true);
    CHECK(r.run_until(BluetoothLinkState::Connected));
    const ClassicBond* bond = r.bonds.find_classic(r.peer());
    CHECK(bond && bond->authenticated && bond->key_type == 5 && r.info().authenticated);
    CHECK(r.run_until([&] { return r.sink.keyboards.count() == 1; }));
    r.dev->input({0, 0, 0x05, 0, 0, 0, 0, 0});                                     // (no report ids: the report follows the header)
    r.step();
    std::vector<KeyEvent> keys = r.sink.drain_keys();
    CHECK(keys.size() == 1 && keys[0].usage == 5 && keys[0].pressed);
  }
  {   // the user says the numbers differ
    Rig r(kPlainKeyboardMap, sizeof kPlainKeyboardMap, false);
    r.lmp.io_capability = 1;
    r.adapter->set_pairing_ui(&r.ui);
    r.discover(0x002540);
    CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until([&] { return r.ui.comparisons == 1; }));
    r.adapter->confirm(r.peer(), false);
    CHECK(r.run_until(BluetoothLinkState::Failed) && r.lmp.rejections == 1 && !r.bonds.find_classic(r.peer()) && strlen(r.info().failure) > 0);
    CHECK(r.c->link_count() == 0 || r.run_until([&] { return r.c->link_count() == 0; }));
  }
  {   // a pad with no input or output, with a UI set all the same: Just Works, nobody is asked
    Rig r(kPlainKeyboardMap, sizeof kPlainKeyboardMap, false);
    r.adapter->set_pairing_ui(&r.ui);
    r.discover(0x002540);
    CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until(BluetoothLinkState::Connected));
    CHECK(r.ui.comparisons == 0 && r.lmp.host_io == 1 && r.lmp.confirmations == 1 && !r.bonds.find_classic(r.peer())->authenticated);
  }
  {   // a keyboard: the host shows the passkey
    Rig r(kPlainKeyboardMap, sizeof kPlainKeyboardMap, false);
    r.lmp.io_capability = 2;
    r.adapter->set_pairing_ui(&r.ui);
    r.discover(0x002540);
    CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until(BluetoothLinkState::Connected));
    CHECK(r.ui.displayed == 654321 && r.bonds.find_classic(r.peer())->authenticated);
  }
  {   // legacy pairing without a UI: PIN 0000
    Rig r(kPlainKeyboardMap, sizeof kPlainKeyboardMap, false);
    r.lmp.ssp = false;
    r.discover(0x002540);
    CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until(BluetoothLinkState::Connected));
    CHECK(r.lmp.last_pin == (Bytes{'0', '0', '0', '0'}) && r.bonds.find_classic(r.peer()) && r.bonds.find_classic(r.peer())->key_type == 0);
  }
  {   // legacy pairing with a PIN the device does not take
    Rig r(kPlainKeyboardMap, sizeof kPlainKeyboardMap, false);
    r.lmp.ssp = false;
    r.lmp.pin = {'1', '2', '3', '4'};
    r.discover(0x002540);
    CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until(BluetoothLinkState::Failed));
    CHECK(!r.bonds.find_classic(r.peer()) && strstr(r.info().failure, "refused"));
  }
  {   // legacy pairing with a UI: the PIN the user types
    Rig r(kPlainKeyboardMap, sizeof kPlainKeyboardMap, false);
    r.lmp.ssp = false;
    r.lmp.pin = {'0', '0', '4', '2'};
    r.adapter->set_pairing_ui(&r.ui);
    r.discover(0x002540);
    CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until([&] { return r.ui.asked == 1; }));
    r.adapter->passkey_entered(r.peer(), 42);
    CHECK(r.run_until(BluetoothLinkState::Connected) && r.lmp.last_pin == r.lmp.pin);
  }
  {   // the device refuses to pair; and one that does not answer the page
    Rig r(kPlainKeyboardMap, sizeof kPlainKeyboardMap, false);
    r.lmp.refuse_pairing = true;
    r.discover(0x002540);
    CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until(BluetoothLinkState::Failed) && strstr(r.info().failure, "does not allow"));
    r.host.controller.connect_status = hci::kStatusPageTimeout;
    CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until(BluetoothLinkState::Failed));
  }
  {   // an incoming connection from a device that is not bonded is refused
    Rig r(kPlainKeyboardMap, sizeof kPlainKeyboardMap, false);
    r.host.controller.complete_accepted = true;
    r.host.controller.incoming_request();
    r.step();
    CHECK(r.host.controller.last_accept.empty() && r.host.controller.last_reject.size() == 7 && r.c->link_count() == 0 && r.host.controller.scan_enable == 0);
  }
}

// ------------------------------------------------------------------------------------------------ SDP over the air

static void test_sdp_answers() {
  const ClassicDevice::SdpMode modes[] = {ClassicDevice::SdpMode::Truncated, ClassicDevice::SdpMode::Deep, ClassicDevice::SdpMode::BadContinuation,
                                          ClassicDevice::SdpMode::BadLength, ClassicDevice::SdpMode::Error, ClassicDevice::SdpMode::Silent,
                                          ClassicDevice::SdpMode::BadAttributeId};
  for (const ClassicDevice::SdpMode mode : modes) {
    Rig r(kPlainKeyboardMap, sizeof kPlainKeyboardMap, false);
    r.dev->sdp_mode = mode;
    r.discover(0x002540);
    CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until(BluetoothLinkState::Connected));
    for (int i = 0; i < 20; ++i) r.step();
    CHECK(r.info().profiles[0] == 0 && r.hidp->sessions() == 0 && r.sink.keyboards.count() == 0 && !r.dev->open(0x11));
  }
  {   // one answer in a single piece (no continuation) works as well as many
    Rig r(kPlainKeyboardMap, sizeof kPlainKeyboardMap, false);
    r.dev->sdp_chunk = 400;
    r.discover(0x002540);
    CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until([&] { return r.hidp->sessions() == 1; }) && r.dev->sdp_requests.size() == 2);
  }
  {   // the device refuses the HID channels: the link is let go
    Rig r(kPlainKeyboardMap, sizeof kPlainKeyboardMap, false);
    r.dev->refuse_hid = true;
    r.discover(0x002540);
    CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until(BluetoothLinkState::Connected));
    CHECK(r.run_until(BluetoothLinkState::Disconnected) && r.hidp->sessions() == 0);
  }
}

// ------------------------------------------------------------------------------------------------ pads

static bool feature_on_thread(Rig& r, HidpReportLink* link, uint8_t id, Bytes* got, int polls = 2000) {
  std::atomic<bool> done{false};
  bool ok = false;
  uint8_t buffer[64];
  uint32_t length = 0;
  std::thread t([&] { ok = link->get_feature(id, ByteView(buffer), &length); done = true; });
  for (int i = 0; i < polls && !done; ++i) r.step();
  t.join();
  if (got) got->assign(buffer, buffer + (ok ? length : 0));
  return ok;
}

static void test_pad() {
  Rig r(kGamepadMap, sizeof kGamepadMap);
  r.discover(0x002508);
  CHECK(r.adapter->connect(r.peer()) == BluetoothResult::Ok && r.run_until(BluetoothLinkState::Connected));
  CHECK(r.run_until([&] { return r.hidp->sessions() == 1; }) && r.hidp->pads() == 1 && r.pads->count() == 1);
  gamepad::Info pad_info;
  CHECK(r.pads->info(0, &pad_info) && pad_info.protocol == gamepad::Protocol::DualShock4);
  r.step();
  CHECK(!r.dev->outputs.empty() && r.dev->outputs[0][0] == 0x11);                  // (the player's light, set when the pad took its slot: output report 0x11)
  // the pad's first round reads feature 0x02 (which switches a DualShock 4 to its full reports), from the service's thread
  r.pads_step();
  CHECK(r.dev->get_reports.size() == 1 && r.dev->get_reports[0] == (Bytes{0x43, 0x02}));
  // input
  r.dev->input({0x01, 0x00, 0x80, 0x80, 0x80, 0x28, 0, 0, 0, 0});
  r.step();
  r.pads_step();
  gamepad::State state;
  uint32_t packet = 0;
  CHECK(r.pads->snapshot(0, &state, &packet) && state.left_x < -30000 && state.buttons != 0);
  // rumble goes out from the adapter's thread
  const size_t outputs = r.dev->outputs.size();
  CHECK(r.pads->set_vibration(0, 65535, 0));
  r.pads_step();
  r.step();
  CHECK(r.dev->outputs.size() > outputs && r.dev->outputs.back()[0] == 0x11);
  // feature reports: answered, refused (an id it does not have: a handshake), and unanswered (given up after kControlTicks polls)
  HidpReportLink* link = r.hidp->link_at(0);
  CHECK(link != nullptr);
  Bytes got;
  r.dev->feature = {0x05, 1, 2, 3};
  CHECK(feature_on_thread(r, link, 0x05, &got) && got == r.dev->feature && r.dev->get_reports.back() == (Bytes{0x43, 0x05}));
  CHECK(!feature_on_thread(r, link, 0x09, &got));
  r.dev->answer_feature = false;
  const size_t asked = r.dev->get_reports.size();
  CHECK(!feature_on_thread(r, link, 0x05, &got) && r.dev->get_reports.size() == asked + 1);
  r.dev->answer_feature = true;
  CHECK(feature_on_thread(r, link, 0x05, &got) && got.size() == 4);
  // SET_REPORT on the control channel
  const uint8_t set[2] = {0x05, 0x09};
  CHECK(link->set_report(3, ConstByteView(set, 2)) && !link->set_report(3, ConstByteView(set, 1)));
  r.step();
  r.step();
  CHECK(!r.dev->set_reports.empty() && r.dev->set_reports.back() == (Bytes{0x53, 0x05, 0x09}) && link->set_reports_failed() == 0);
  // many threads queue output while the adapter's thread sends it
  {
    std::vector<std::thread> threads;
    std::atomic<uint32_t> queued{0};
    for (int t = 0; t < 4; ++t)
      threads.emplace_back([&, t] {
        for (int i = 0; i < 200; ++i) {
          const uint8_t report[3] = {0x11, static_cast<uint8_t>(t), static_cast<uint8_t>(i)};
          if (link->send_output(ConstByteView(report, 3))) ++queued;
        }
      });
    const size_t start = r.dev->outputs.size();
    for (int i = 0; i < 400; ++i) r.step();
    for (std::thread& th : threads) th.join();
    for (int i = 0; i < 20; ++i) r.step();
    CHECK(queued == 800 && r.dev->outputs.size() > start && r.dev->outputs.size() - start <= 800);
  }
  // the pad switches off while a feature read waits: the read returns, the pad leaves its slot
  r.dev->answer_feature = false;
  {
    std::atomic<bool> done{false};
    bool ok = true;
    uint8_t buffer[16];
    uint32_t length = 0;
    std::thread t([&] { ok = link->get_feature(0x05, ByteView(buffer), &length); done = true; });
    r.step();
    r.step();
    r.host.controller.remote_disconnect();
    // (the controller's round only: the link goes down, the session ends and the read is let go; the adapter's round would free the device, and this test calls the link
    // directly, not through a pad's channel, whose detach() waits for a call in progress)
    for (int i = 0; i < 2000 && !done; ++i) r.c->poll();
    t.join();
    CHECK(!ok && length == 0);
  }
  r.dev->answer_feature = true;
  CHECK(r.run_until(BluetoothLinkState::Disconnected) && r.hidp->sessions() == 0 && r.hidp->pads() == 0);
  r.pads_step();
  CHECK(r.pads->count() == 0);
  // its PS button: it reconnects by itself and opens the channels itself; the bond is used, nothing paired
  const uint32_t replies = r.lmp.key_replies;
  r.host.controller.complete_accepted = true;
  r.host.controller.incoming_request();
  CHECK(r.run_until(BluetoothLinkState::Connected) && r.lmp.key_replies == replies + 1 && r.lmp.pairings == 1);
  r.dev->connect_channel(0x11);
  r.dev->connect_channel(0x13);
  CHECK(r.run_until([&] { return r.hidp->sessions() == 1; }, 20) && r.hidp->pads() == 1);
  r.pads_step();
  CHECK(r.pads->count() == 1);
  // it asks to be forgotten (virtual cable unplug): the bond goes, the link is dropped
  r.dev->unplug();
  CHECK(r.run_until(BluetoothLinkState::Disconnected) && r.hidp->unplugged() == 1 && !r.bonds.find_classic(r.peer()) && r.hidp->sessions() == 0);
  r.step();
  CHECK(r.host.controller.scan_enable == 0);
  r.pads_step();
  CHECK(r.pads->count() == 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_sdp_elements();
  test_bond_file();
  test_keyboard_just_works();
  test_pairing_methods();
  test_sdp_answers();
  test_pad();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("classic HID tests passed\n");
  return 0;
}
