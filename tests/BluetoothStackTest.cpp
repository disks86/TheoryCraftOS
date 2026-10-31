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

// Host-side tests for the Bluetooth upper layers: the security manager's cryptography against the specification's sample data (the vectors are those in the Linux kernel's SMP self
// tests), L2CAP, ATT, GATT, and pairing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "FakePeripheral.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Hardware/Bluetooth/BluetoothService.hpp"
#include "Hardware/Bluetooth/HciBluetoothAdapter.hpp"
#include "Hardware/Bluetooth/HidOverGatt.hpp"
#include "Hardware/Hid/HidDescriptor.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "SystemBluetoothDetails.hpp"
#include "Hardware/Keyboard/CompositeKeyboard.hpp"
#include "Hardware/Mouse/CompositeMouse.hpp"
#include "Hardware/Bluetooth/Smp.hpp"

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

static const uint8_t kDebugPk[64] = {
		0xe6, 0x9d, 0x35, 0x0e, 0x48, 0x01, 0x03, 0xcc,
		0xdb, 0xfd, 0xf4, 0xac, 0x11, 0x91, 0xf4, 0xef,
		0xb9, 0xa5, 0xf9, 0xe9, 0xa7, 0x83, 0x2c, 0x5e,
		0x2c, 0xbe, 0x97, 0xf2, 0xd2, 0x03, 0xb0, 0x20,

		0x8b, 0xd2, 0x89, 0x15, 0xd0, 0x8e, 0x1c, 0x74,
		0x24, 0x30, 0xed, 0x8f, 0xc2, 0x45, 0x63, 0x76,
		0x5c, 0x15, 0x52, 0x5a, 0xbf, 0x9a, 0x32, 0x63,
		0x6d, 0xeb, 0x2a, 0x65, 0x49, 0x9c, 0x80, 0xdc,
};
static const uint8_t kDebugSk[32] = {
		0xbd, 0x1a, 0x3c, 0xcd, 0xa6, 0xb8, 0x99, 0x58,
		0x99, 0xb7, 0x40, 0xeb, 0x7b, 0x60, 0xff, 0x4a,
		0x50, 0x3f, 0x10, 0xd2, 0xe3, 0xb3, 0xc9, 0x74,
		0x38, 0x5f, 0xc5, 0xa3, 0xd4, 0xf6, 0x49, 0x3f,
};

static void test_crypto() {
  using namespace bt;
  uint8_t pk[64];
  CHECK(sm_public_from_private(kDebugSk, pk) && memcmp(pk, kDebugPk, 64) == 0);
  {   // ah
    const uint8_t irk[16] = {0x9b, 0x7d, 0x39, 0x0a, 0xa6, 0x10, 0x10, 0x34, 0x05, 0xad, 0xc8, 0x57, 0xa3, 0x34, 0x02, 0xec};
    const uint8_t r[3] = {0x94, 0x81, 0x70}, exp[3] = {0xaa, 0xfb, 0x0d};
    uint8_t res[3];
    CHECK(sm_ah(irk, r, res) && memcmp(res, exp, 3) == 0);
    const uint8_t address[6] = {0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70};      // (0x70 has the top bits 01: a resolvable address)
    CHECK(sm_irk_matches(irk, address));
    uint8_t other[6];
    memcpy(other, address, 6);
    other[0] ^= 1;
    CHECK(!sm_irk_matches(irk, other));
    other[0] = address[0];
    other[5] = 0xB0;                                                     // not a resolvable address
    CHECK(!sm_irk_matches(irk, other));
  }
  {   // c1
    const uint8_t k[16] = {};
    const uint8_t r[16] = {0xe0, 0x2e, 0x70, 0xc6, 0x4e, 0x27, 0x88, 0x63, 0x0e, 0x6f, 0xad, 0x56, 0x21, 0xd5, 0x83, 0x57};
    const uint8_t preq[7] = {0x01, 0x01, 0x00, 0x00, 0x10, 0x07, 0x07}, pres[7] = {0x02, 0x03, 0x00, 0x00, 0x08, 0x00, 0x05};
    const uint8_t ra[6] = {0xb6, 0xb5, 0xb4, 0xb3, 0xb2, 0xb1}, ia[6] = {0xa6, 0xa5, 0xa4, 0xa3, 0xa2, 0xa1};
    const uint8_t exp[16] = {0x86, 0x3b, 0xf1, 0xbe, 0xc5, 0x4d, 0xa7, 0xd2, 0xea, 0x88, 0x89, 0x87, 0xef, 0x3f, 0x1e, 0x1e};
    uint8_t res[16];
    CHECK(sm_c1(k, r, preq, pres, 1, ia, 0, ra, res) && memcmp(res, exp, 16) == 0);
  }
  {   // s1
    const uint8_t k[16] = {};
    const uint8_t r1[16] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, r2[16] = {0x00, 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99};
    const uint8_t exp[16] = {0x62, 0xa0, 0x6d, 0x79, 0xae, 0x16, 0x42, 0x5b, 0x9b, 0xf4, 0xb0, 0xe8, 0xf0, 0xe1, 0x1f, 0x9a};
    uint8_t res[16];
    CHECK(sm_s1(k, r1, r2, res) && memcmp(res, exp, 16) == 0);
  }
  {   // f4, f5, f6, g2, h6
    const uint8_t u[32] = {0xe6, 0x9d, 0x35, 0x0e, 0x48, 0x01, 0x03, 0xcc, 0xdb, 0xfd, 0xf4, 0xac, 0x11, 0x91, 0xf4, 0xef, 0xb9, 0xa5, 0xf9, 0xe9, 0xa7, 0x83, 0x2c, 0x5e, 0x2c, 0xbe, 0x97, 0xf2, 0xd2, 0x03, 0xb0, 0x20};
    const uint8_t v[32] = {0xfd, 0xc5, 0x7f, 0xf4, 0x49, 0xdd, 0x4f, 0x6b, 0xfb, 0x7c, 0x9d, 0xf1, 0xc2, 0x9a, 0xcb, 0x59, 0x2a, 0xe7, 0xd4, 0xee, 0xfb, 0xfc, 0x0a, 0x90, 0x9a, 0xbb, 0xf6, 0x32, 0x3d, 0x8b, 0x18, 0x55};
    const uint8_t x[16] = {0xab, 0xae, 0x2b, 0x71, 0xec, 0xb2, 0xff, 0xff, 0x3e, 0x73, 0x77, 0xd1, 0x54, 0x84, 0xcb, 0xd5};
    const uint8_t y[16] = {0xcf, 0xc4, 0x3d, 0xff, 0xf7, 0x83, 0x65, 0x21, 0x6e, 0x5f, 0xa7, 0x25, 0xcc, 0xe7, 0xe8, 0xa6};
    const uint8_t exp4[16] = {0x2d, 0x87, 0x74, 0xa9, 0xbe, 0xa1, 0xed, 0xf1, 0x1c, 0xbd, 0xa9, 0x07, 0xf1, 0x16, 0xc9, 0xf2};
    uint8_t res[16];
    CHECK(sm_f4(u, v, x, 0, res) && memcmp(res, exp4, 16) == 0);
    uint32_t value = 0;
    CHECK(sm_g2(u, v, x, y, &value) && value == 0x2f9ed5ba % 1000000);
    const uint8_t w[32] = {0x98, 0xa6, 0xbf, 0x73, 0xf3, 0x34, 0x8d, 0x86, 0xf1, 0x66, 0xf8, 0xb4, 0x13, 0x6b, 0x79, 0x99, 0x9b, 0x7d, 0x39, 0x0a, 0xa6, 0x10, 0x10, 0x34, 0x05, 0xad, 0xc8, 0x57, 0xa3, 0x34, 0x02, 0xec};
    const uint8_t a1[7] = {0xce, 0xbf, 0x37, 0x37, 0x12, 0x56, 0x00}, a2[7] = {0xc1, 0xcf, 0x2d, 0x70, 0x13, 0xa7, 0x00};
    const uint8_t n1[16] = {0xab, 0xae, 0x2b, 0x71, 0xec, 0xb2, 0xff, 0xff, 0x3e, 0x73, 0x77, 0xd1, 0x54, 0x84, 0xcb, 0xd5};
    const uint8_t n2[16] = {0xcf, 0xc4, 0x3d, 0xff, 0xf7, 0x83, 0x65, 0x21, 0x6e, 0x5f, 0xa7, 0x25, 0xcc, 0xe7, 0xe8, 0xa6};
    const uint8_t exp_ltk[16] = {0x38, 0x0a, 0x75, 0x94, 0xb5, 0x22, 0x05, 0x98, 0x23, 0xcd, 0xd7, 0x69, 0x11, 0x79, 0x86, 0x69};
    const uint8_t exp_mackey[16] = {0x20, 0x6e, 0x63, 0xce, 0x20, 0x6a, 0x3f, 0xfd, 0x02, 0x4a, 0x08, 0xa1, 0x76, 0xf1, 0x65, 0x29};
    uint8_t mackey[16], ltk[16];
    CHECK(sm_f5(w, n1, n2, a1, a2, mackey, ltk) && memcmp(mackey, exp_mackey, 16) == 0 && memcmp(ltk, exp_ltk, 16) == 0);
    const uint8_t w6[16] = {0x20, 0x6e, 0x63, 0xce, 0x20, 0x6a, 0x3f, 0xfd, 0x02, 0x4a, 0x08, 0xa1, 0x76, 0xf1, 0x65, 0x29};
    const uint8_t r6[16] = {0xc8, 0x0f, 0x2d, 0x0c, 0xd2, 0x42, 0xda, 0x08, 0x54, 0xbb, 0x53, 0xb4, 0x3b, 0x34, 0xa3, 0x12};
    const uint8_t io_cap[3] = {0x02, 0x01, 0x01};
    const uint8_t exp6[16] = {0x61, 0x8f, 0x95, 0xda, 0x09, 0x0b, 0x6c, 0xd2, 0xc5, 0xe8, 0xd0, 0x9c, 0x98, 0x73, 0xc4, 0xe3};
    CHECK(sm_f6(w6, n1, n2, r6, io_cap, a1, a2, res) && memcmp(res, exp6, 16) == 0);
    const uint8_t wh[16] = {0x9b, 0x7d, 0x39, 0x0a, 0xa6, 0x10, 0x10, 0x34, 0x05, 0xad, 0xc8, 0x57, 0xa3, 0x34, 0x02, 0xec};
    const uint8_t key_id[4] = {0x72, 0x62, 0x65, 0x6c};
    const uint8_t exph[16] = {0x99, 0x63, 0xb1, 0x80, 0xe2, 0xa9, 0xd3, 0xe8, 0x1c, 0xc9, 0x6d, 0xe7, 0x02, 0xe1, 0x9a, 0x2d};
    CHECK(sm_h6(wh, key_id, res) && memcmp(res, exph, 16) == 0);
  }
  {   // two key pairs agree on the secret; a point that is not on the curve is refused
    struct Fixed final : crypto::IRandom {
      uint8_t seed = 3;
      bool fill(ByteView out) override { for (uint64_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(seed * 31 + i * 7 + 1); ++seed; return true; }
    } random;
    SmKeyPair a, b;
    CHECK(sm_generate_key_pair(random, &a) && sm_generate_key_pair(random, &b));
    uint8_t ka[32], kb[32];
    CHECK(sm_dhkey(a.private_key, b.public_key, ka) && sm_dhkey(b.private_key, a.public_key, kb) && memcmp(ka, kb, 32) == 0);
    uint8_t bad[64];
    memcpy(bad, b.public_key, 64);
    bad[0] ^= 1;
    CHECK(!sm_dhkey(a.private_key, bad, ka));
  }
}

// ------------------------------------------------------------------------------------------------ the world: host, controller, a peripheral

struct TestRandom final : crypto::IRandom {
  uint64_t state = 0x1234567887654321ull;
  bool fill(ByteView out) override {
    for (uint64_t i = 0; i < out.size(); ++i) {
      state ^= state << 13; state ^= state >> 7; state ^= state << 17;
      out[i] = static_cast<uint8_t>(state >> 24);
    }
    return true;
  }
};

struct TestUi final : ISmUserInterface {
  SecurityManager* sm = nullptr;
  Peripheral* peripheral = nullptr;
  bool answer_confirm = true;
  bool auto_answer = true;
  uint32_t displayed = 0, compared = 0, typed = 0;
  uint32_t displays = 0, compares = 0, requests = 0;
  void display_passkey(uint16_t, uint32_t passkey) override { displayed = passkey; ++displays; if (peripheral) peripheral->passkey = passkey; }      // the user types what is shown
  void confirm_numeric(uint16_t link, uint32_t value) override { compared = value; ++compares; if (auto_answer) sm->confirm(link, answer_confirm); }
  void request_passkey(uint16_t link) override { ++requests; if (auto_answer) sm->passkey_entered(link, typed); }
};

struct World {
  TestAllocator a;
  Host host;
  UniquePointer<UsbHciTransport> transport;
  UniquePointer<HciController> c;
  UniquePointer<L2cap> l2;
  UniquePointer<AttClient> att;
  UniquePointer<GattClient> gatt;
  SmBondStore bonds;
  TestRandom random;
  UniquePointer<SecurityManager> sm;
  TestUi ui;
  UniquePointer<Peripheral> p;
  uint16_t handle = 0;

  static void acl_hook(void* self, const Bytes& packet) { static_cast<World*>(self)->p->on_acl(packet); }
  static bool encryption_hook(void* self, uint16_t h, const uint8_t rand[8], uint16_t ediv, const uint8_t ltk[16]) {
    World& w = *static_cast<World*>(self);
    if (h != w.handle) return false;
    const bool ok = w.p->encryption_request(rand, ediv, ltk);
    if (ok) w.p->after_encryption();
    return ok;
  }

  World() {
    transport = UsbHciTransport::open(host, 0, a);
    c = UniquePointer<HciController>::make(a, *transport, a);
    CHECK(c->initialize() == HciResult::Ok);
    l2 = UniquePointer<L2cap>::make(a, *c);
    att = UniquePointer<AttClient>::make(a, *c, *l2);
    gatt = UniquePointer<GattClient>::make(a, *att);
    sm = UniquePointer<SecurityManager>::make(a, *c, *l2, random, bonds);
    att->set_security_handler(&SecurityManager::att_security, sm.get());
    ui.sm = sm.get();
    sm->set_user_interface(&ui);
    host.controller.acl_hook = &World::acl_hook;
    host.controller.encryption_hook = &World::encryption_hook;
    host.controller.hook_context = this;
  }
  // An LE link to a peripheral with a small GATT database.
  bool connect_le() {
    handle = 0x40;
    p = UniquePointer<Peripheral>::make(a, host.controller, handle, true);
    p->random = &random;
    build_database(*p);
    hci::BdAddr peer;
    memcpy(peer.b, p->address, 6);
    if (c->connect_le(peer, 1) != HciResult::Ok) return false;
    c->poll();
    return c->link(handle) != nullptr;
  }
  static void build_database(Peripheral& per) {
    // 1-4: generic access with a long device name; 6-10: battery with notifications; 11-15: a service whose value needs encryption; 20-22: a service with 128 bit UUIDs
    per.add_service(1, 4, Uuid::from16(0x1800));
    Bytes name;
    for (int i = 0; i < 60; ++i) name.push_back(static_cast<uint8_t>('a' + i % 26));
    per.add_characteristic(2, gatt::kPropRead, Uuid::from16(0x2A00), name);
    per.add_service(6, 10, Uuid::from16(0x180F));
    per.add_characteristic(7, gatt::kPropRead | gatt::kPropNotify | gatt::kPropIndicate, Uuid::from16(0x2A19), Bytes{87});
    per.add_cccd(9);
    per.add_characteristic(10, gatt::kPropWrite | gatt::kPropWriteWithoutResponse, Uuid::from16(0x2A1A), Bytes{0}, false, true);
    per.add_service(11, 15, Uuid::from16(0x1812));
    per.add_characteristic(12, gatt::kPropRead | gatt::kPropNotify, Uuid::from16(0x2A4D), Bytes{1, 2, 3, 4}, true);
    per.add_cccd(14);
    Uuid custom;
    for (int i = 0; i < 16; ++i) custom.b[i] = static_cast<uint8_t>(0xA0 + i);
    per.add_service(20, 22, custom);
    Uuid chr = custom;
    chr.b[0] ^= 0x55;
    per.add_characteristic(21, gatt::kPropRead, chr, Bytes{9, 9});
  }
  void classic_link() {
    handle = 1;
    p = UniquePointer<Peripheral>::make(a, host.controller, handle, false);
    hci::BdAddr peer;
    const uint8_t addr[6] = {1, 2, 3, 4, 5, 6};
    memcpy(peer.b, addr, 6);
    CHECK(c->connect(peer) == HciResult::Ok);
    c->poll();
    CHECK(c->link(1) != nullptr);
    host.controller.acl_hook = &World::acl_hook;
  }
};

struct Frames final : IL2capFixedHandler {
  std::vector<Bytes> got;
  uint32_t ups = 0, downs = 0;
  void on_data(const HciLink&, ConstByteView sdu) override { got.push_back(Bytes(sdu.data(), sdu.data() + sdu.size())); }
  void on_link(const HciLink&, bool up) override { (up ? ups : downs)++; }
};

static void test_l2cap() {
  {   // frames in fragments, both ways; two links at once; junk
    World w;
    CHECK(w.connect_le());
    Frames fixed;
    CHECK(w.l2->register_fixed(0x0008, &fixed) && !w.l2->register_fixed(0x0008, &fixed));
    Bytes big(300);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i);
    w.p->send_frame(8, big);                                    // 304 bytes in 27 byte fragments
    w.p->send_frame(8, Bytes{1, 2, 3});
    w.c->poll();
    CHECK(fixed.got.size() == 2 && fixed.got[0] == big && fixed.got[1].size() == 3 && fixed.ups == 0);
    // to the peripheral: 100 bytes, a frame the peripheral keeps because nothing listens on that CID
    Bytes out(100, 0x5A);
    CHECK(w.l2->send_fixed(w.handle, 0x0009, ConstByteView(out.data(), out.size())) == HciResult::Ok);
    CHECK(w.p->frames.size() == 1 && w.p->frames[0].first == 9 && w.p->frames[0].second == out);
    // a frame to a channel nobody registered is dropped and counted
    w.p->send_frame(0x0099, Bytes{1});
    w.c->poll();
    CHECK(w.l2->frames_dropped() == 1);
    // a continuation with no start, a start that never finishes, a length too big for us, a length longer than the fragments
    w.host.controller.inject_acl(w.handle, 1, Bytes{1, 2, 3});
    w.host.controller.inject_acl(w.handle, 2, Bytes{50, 0, 8, 0, 1, 2, 3});                  // 50 bytes promised, 3 delivered...
    w.host.controller.inject_acl(w.handle, 2, Bytes{2, 0, 8, 0, 7, 7});                      // ...then a new start: the old one is dropped, this one is whole
    w.host.controller.inject_acl(w.handle, 2, Bytes{0xFF, 0x7F, 8, 0, 1});                   // 32 KiB: refused
    w.host.controller.inject_acl(w.handle, 1, Bytes(20, 1));                                 // (its continuation is skipped)
    w.host.controller.inject_acl(w.handle, 2, Bytes{2, 0, 8, 0, 1, 2, 3, 4, 5});             // more bytes than the length: damaged
    w.c->poll();
    CHECK(fixed.got.size() == 3 && fixed.got[2] == (Bytes{7, 7}));
    // a link going down tells the handlers
    CHECK(w.c->disconnect(w.handle) == HciResult::Ok);
    w.c->poll();
    CHECK(fixed.downs == 1 && w.c->link_count() == 0);
  }
  {   // signalling on an LE link
    World w;
    CHECK(w.connect_le());
    w.p->signal(0x08, 5, Bytes{});                      // an echo request on the LE channel: LE has no echo, the command is rejected
    w.c->poll();
    CHECK(w.p->signal_responses.size() == 1 && w.p->signal_responses[0][0] == 0x01);
    w.p->signal_responses.clear();
    w.p->signal(0x12, 6, Bytes{0x18, 0, 0x28, 0, 0, 0, 0xC8, 0});    // connection parameter update request: refused
    w.p->signal(0x14, 7, Bytes(10, 0));                               // credit based connection: not supported
    w.c->poll();
    CHECK(w.p->signal_responses.size() == 2 && w.p->signal_responses[0][0] == 0x13 && w.p->signal_responses[1][0] == 0x15);
  }
  {   // classic channels: the peer opens one to our listener, we open one to it, data, close, refusals
    World w;
    w.classic_link();
    struct Handler final : IL2capChannelHandler {
      uint32_t opens = 0, closes = 0;
      std::vector<Bytes> got;
      void on_open(L2capChannel&) override { ++opens; }
      void on_data(L2capChannel&, ConstByteView d) override { got.push_back(Bytes(d.data(), d.data() + d.size())); }
      void on_close(L2capChannel&) override { ++closes; }
    } handler;
    CHECK(w.l2->listen(0x0011, &handler) && !w.l2->listen(0x0011, &handler));
    // the peer connects to PSM 0x11 (ours) and 0x13 (nobody's)
    w.p->open_to_host(0x11, 0x50);
    w.p->signal(0x02, 2, Bytes{0x13, 0, 0x51, 0});
    w.p->signal(0x02, 3, Bytes{0x11, 0, 0x03, 0});            // a source CID that is not dynamic
    w.c->poll();
    w.c->poll();
    CHECK(handler.opens == 1);
    L2capChannel* ch = w.l2->channel(0);
    CHECK(ch && ch->state == L2capState::Open && ch->remote_cid == 0x50 && ch->mtu_out == 672 && ch->psm == 0x11);
    // data both ways
    w.p->send_frame(ch->local_cid, Bytes{1, 2, 3, 4});
    w.c->poll();
    CHECK(handler.got.size() == 1 && handler.got[0].size() == 4);
    CHECK(w.l2->send(*ch, ConstByteView(handler.got[0].data(), 4)) == HciResult::Ok && w.p->frames.size() == 1 && w.p->frames[0].first == 0x50);
    Bytes toobig(700, 1);
    CHECK(w.l2->send(*ch, ConstByteView(toobig.data(), toobig.size())) == HciResult::NoRoom);
    w.p->send_frame(ch->local_cid, toobig);                   // more than the MTU we announced: dropped
    w.c->poll();
    CHECK(handler.got.size() == 1);
    // the peer closes it
    w.p->signal(0x06, 9, Bytes{static_cast<uint8_t>(ch->local_cid), 0, 0x50, 0});
    w.c->poll();
    CHECK(handler.closes == 1 && !w.l2->channel(0));
    // we open one to the peer's PSM 0x11
    L2capChannel* mine = w.l2->connect(1, 0x0011, &handler);
    CHECK(mine && mine->state == L2capState::Connecting);
    w.c->poll();
    w.c->poll();
    w.c->poll();
    CHECK(handler.opens == 2 && w.l2->channel(0) && w.l2->channel(0)->state == L2capState::Open);
    // and a PSM it does not have is refused: the channel goes away at once
    w.p->accept_psm_0x11 = false;
    L2capChannel* refused = w.l2->connect(1, 0x0011, &handler);
    CHECK(refused);
    w.c->poll();
    w.c->poll();
    CHECK(handler.closes == 2);
    // echo and information requests from the peer
    w.p->signal_responses.clear();
    w.p->signal(0x08, 20, Bytes{9, 8, 7});
    w.p->signal(0x0A, 21, Bytes{2, 0});
    w.p->signal(0x0A, 22, Bytes{3, 0});
    w.p->signal(0x0A, 23, Bytes{9, 0});
    w.c->poll();
    CHECK(w.p->signal_responses.size() == 4 && w.p->signal_responses[0][0] == 0x09 && w.p->echoed == (Bytes{9, 8, 7}) && w.p->signal_responses[1][0] == 0x0B);
    // link loss closes the open channel
    const uint32_t closes = handler.closes;
    CHECK(w.c->disconnect(1) == HciResult::Ok);
    w.c->poll();
    CHECK(handler.closes == closes + 1);
  }
}

static void test_att_gatt() {
  World w;
  CHECK(w.connect_le());
  CHECK(w.att->mtu(w.handle) == 23);
  CHECK(w.att->exchange_mtu(w.handle, 247) == AttStatus::Ok && w.att->mtu(w.handle) == 185 && w.p->mtu == 185);
  GattDatabase db;
  CHECK(w.gatt->discover(w.handle, &db) == AttStatus::Ok && !db.truncated);
  CHECK(db.service_count == 4 && db.services[0].uuid == Uuid::from16(0x1800) && db.services[1].start == 6 && db.services[1].end == 10 && !db.services[3].uuid.is16());
  CHECK(db.characteristic_count == 5 && db.characteristics[0].value_handle == 3 && db.characteristics[1].properties == (gatt::kPropRead | gatt::kPropNotify | gatt::kPropIndicate));
  CHECK(db.characteristics[1].cccd == 9 && db.characteristics[1].end_handle == 9 && db.characteristics[2].cccd == 0 && db.characteristics[3].cccd == 14);
  CHECK(db.descriptor_count == 2 && db.descriptors[0].handle == 9);
  const int battery = db.find_characteristic(Uuid::from16(0x2A19));
  const int report = db.find_characteristic(Uuid::from16(0x2A4D), Uuid::from16(0x1812));
  CHECK(battery == 1 && report == 3 && db.find_characteristic(Uuid::from16(0x2A19), Uuid::from16(0x1812)) < 0 && db.find_service(Uuid::from16(0x180F)) == 1 && db.find_service(Uuid::from16(0x1234)) < 0);
  // reads: short, and long (60 bytes in 184 byte answers: one read)
  uint8_t value[200];
  uint32_t length = 0;
  CHECK(w.gatt->read(w.handle, db, battery, ByteView(value), &length) == AttStatus::Ok && length == 1 && value[0] == 87);
  CHECK(w.gatt->read(w.handle, db, 0, ByteView(value), &length) == AttStatus::Ok && length == 60 && value[0] == 'a' && value[59] == 'h');
  // the same with the default MTU of 23: blob reads, 22 bytes at a time
  World small;
  CHECK(small.connect_le());
  GattDatabase db2;
  CHECK(small.gatt->discover(small.handle, &db2) == AttStatus::Ok && db2.service_count == 4);
  CHECK(small.gatt->read(small.handle, db2, 0, ByteView(value), &length) == AttStatus::Ok && length == 60 && value[22] == 'w' && value[59] == 'h');
  uint8_t cut[10];
  CHECK(small.gatt->read(small.handle, db2, 0, ByteView(cut), &length) == AttStatus::Ok && length == 10);          // (cut to the room there is)
  // writes, with and without a response; a read-only attribute refuses; a write command to one is ignored
  const uint8_t one = 1;
  CHECK(w.gatt->write(w.handle, db, 2, ConstByteView(&one, 1)) == AttStatus::Ok && w.p->attr(11)->value == Bytes{1});
  CHECK(w.gatt->write(w.handle, db, 2, ConstByteView(&one, 1), false) == AttStatus::Ok);
  CHECK(w.gatt->write(w.handle, db, 0, ConstByteView(&one, 1)) == AttStatus::Error && w.att->error_code() == 3 && w.att->error_handle() == 3);
  CHECK(w.att->write(w.handle, 99, ConstByteView(&one, 1)) == AttStatus::Error && w.att->error_code() == 1);
  Bytes huge(300, 1);
  CHECK(w.att->write(w.handle, 11, ConstByteView(huge.data(), huge.size())) == AttStatus::Malformed);
  CHECK(w.gatt->read(w.handle, db, 77, ByteView(value), &length) == AttStatus::Malformed && w.gatt->subscribe(w.handle, db, 77, true) == AttStatus::Malformed);
  // notifications and indications
  struct Seen { std::vector<Bytes> values; std::vector<bool> indications; } seen;
  w.att->set_notification_handler([](void* c, uint16_t, uint16_t handle, ConstByteView v, bool indication) {
    Seen& s = *static_cast<Seen*>(c);
    s.values.push_back(Bytes{static_cast<uint8_t>(handle)});
    s.values.back().insert(s.values.back().end(), v.data(), v.data() + v.size());
    s.indications.push_back(indication);
  }, &seen);
  CHECK(w.gatt->subscribe(w.handle, db, battery, true, true) == AttStatus::Ok && w.p->attr(9)->value[0] == 3);
  CHECK(w.gatt->subscribe(w.handle, db, 0, true) == AttStatus::Malformed && w.gatt->subscribe(w.handle, db, 2, true) == AttStatus::Malformed);
  w.p->notify(8, Bytes{80});
  w.p->notify(8, Bytes{70}, true);
  w.c->poll();
  CHECK(seen.values.size() == 2 && seen.values[0] == (Bytes{8, 80}) && !seen.indications[0] && seen.indications[1] && w.p->indication_confirms == 1);
  // errors: a silent peer times out, a link that is gone, a request while one is out, damaged discovery answers
  w.p->silent_att = true;
  CHECK(w.att->request(w.handle, ConstByteView(&one, 1), 0x0B, 20) == AttStatus::Timeout);
  w.p->silent_att = false;
  CHECK(w.att->request(0x77, ConstByteView(&one, 1), 0x0B) == AttStatus::NoLink);
  World damaged;
  CHECK(damaged.connect_le());
  damaged.p->malformed_discovery = true;
  GattDatabase bad;
  CHECK(damaged.gatt->discover(damaged.handle, &bad) == AttStatus::Malformed);
  // a link that drops during a request ends it
  World dropped;
  CHECK(dropped.connect_le());
  dropped.p->silent_att = true;
  const uint8_t read_request[3] = {0x0A, 3, 0};
  CHECK(dropped.host.controller.commands.size() > 0);
  dropped.host.controller.push(hci::kEvDisconnectionComplete, {0, 0x40, 0, 0x13});
  CHECK(dropped.att->request(dropped.handle, ConstByteView(read_request, 3), 0x0B, 200) == AttStatus::NoLink);
}

// ------------------------------------------------------------------------------------------------ pairing

struct PairCase {
  uint8_t central_io, peripheral_io;
  bool central_sc, peripheral_sc, central_mitm, peripheral_mitm;
  int method;                 // what the peripheral should see: 0 just works, 1 numeric, 2 the peripheral displays, 3 the peripheral inputs
};

static void configure(World& w, const PairCase& pc) {
  SmConfig cfg;
  cfg.io_capability = pc.central_io;
  cfg.secure_connections = pc.central_sc;
  cfg.mitm = pc.central_mitm;
  w.sm->set_config(cfg);
  w.p->io_capability = pc.peripheral_io;
  w.p->want_sc = pc.peripheral_sc;
  w.p->want_mitm = pc.peripheral_mitm;
  w.p->method = pc.method;
  w.ui.peripheral = w.p.get();
  if (pc.method == 2) { w.p->passkey = 123456; w.ui.typed = 123456; }
}

static void test_pairing() {
  const PairCase cases[] = {
      {3, 3, false, false, false, false, 0},     // legacy, just works
      {3, 3, true, true, false, false, 0},       // secure connections, just works
      {0, 2, false, false, true, true, 3},       // legacy: the host displays, the keyboard types
      {2, 0, false, false, true, true, 2},       // legacy: the peripheral displays, the host types
      {0, 2, true, true, true, true, 3},         // secure connections passkey, the host displays
      {2, 0, true, true, true, true, 2},         // secure connections passkey, the host types
      {1, 1, true, true, true, true, 1},         // numeric comparison
      {1, 1, true, false, true, true, 0},        // the peripheral has no secure connections: legacy (and a yes/no pair is just works there)
      {4, 2, true, true, true, true, 3},         // keyboard and display with a keyboard: the host displays
  };
  for (const PairCase& pc : cases) {
    World w;
    CHECK(w.connect_le());
    configure(w, pc);
    const SmResult r = w.sm->pair(w.handle, 5000);
    CHECK(r == SmResult::Ok);
    if (r != SmResult::Ok) { printf("  (case io %u/%u sc %d/%d mitm %d/%d method %d: failure %u)\n", pc.central_io, pc.peripheral_io, pc.central_sc, pc.peripheral_sc, pc.central_mitm, pc.peripheral_mitm, pc.method, w.sm->failure_reason()); continue; }
    CHECK(w.c->link(w.handle)->encrypted && w.p->encrypted && w.p->pairings_completed == 1);
    CHECK(w.sm->last_secure_connections() == (pc.central_sc && pc.peripheral_sc) && w.sm->last_authenticated() == (pc.method != 0));
    const SmBond* bond = w.bonds.find(w.c->link(w.handle)->address, 1);
    CHECK(bond && bond->has_ltk && bond->has_irk && bond->has_identity && bond->key_size == 16 && bond->authenticated == (pc.method != 0) && bond->secure_connections == (pc.central_sc && pc.peripheral_sc));
    CHECK(memcmp(bond->ltk, w.p->stored_ltk, 16) == 0 && bond->ediv == w.p->stored_ediv && memcmp(bond->rand, w.p->stored_rand, 8) == 0);
    if (pc.method == 1) CHECK(w.ui.compares == 1 && w.ui.compared == w.p->numeric_value);
    if (pc.method == 3) CHECK(w.ui.displays == 1 && w.ui.displayed < 1000000);
    if (pc.method == 2) CHECK(w.ui.requests == 1);
    // the link now reads what needs encryption, and a second connection can use the bond
    GattDatabase db;
    CHECK(w.gatt->discover(w.handle, &db) == AttStatus::Ok);
    uint8_t value[16];
    uint32_t length = 0;
    CHECK(w.gatt->read(w.handle, db, db.find_characteristic(Uuid::from16(0x2A4D)), ByteView(value), &length) == AttStatus::Ok && length == 4);
  }
}

static void test_pairing_failures() {
  {   // the user says the numbers differ
    World w;
    CHECK(w.connect_le());
    configure(w, {1, 1, true, true, true, true, 1});
    w.ui.answer_confirm = false;
    CHECK(w.sm->pair(w.handle, 3000) == SmResult::Failed && w.sm->failure_reason() == 0x0C && w.bonds.count() == 0 && !w.c->link(w.handle)->encrypted);
  }
  {   // the wrong passkey typed: the first round's confirm values disagree (secure connections) or the confirm value does not match (legacy)
    World w;
    CHECK(w.connect_le());
    configure(w, {2, 0, true, true, true, true, 2});
    w.ui.typed = 654321;
    CHECK(w.sm->pair(w.handle, 3000) == SmResult::Failed && w.bonds.count() == 0);
    World legacy;
    CHECK(legacy.connect_le());
    configure(legacy, {2, 0, false, false, true, true, 2});
    legacy.ui.typed = 111111;
    CHECK(legacy.sm->pair(legacy.handle, 3000) == SmResult::Failed && legacy.bonds.count() == 0);
  }
  {   // the peripheral refuses, wants OOB, offers a key that is too short, asks the host to pair
    World w;
    CHECK(w.connect_le());
    configure(w, {3, 3, true, true, false, false, 0});
    w.p->reject_pairing = true;
    CHECK(w.sm->pair(w.handle, 2000) == SmResult::Failed && w.sm->failure_reason() == 5);
    World oob;
    CHECK(oob.connect_le());
    configure(oob, {3, 3, true, true, false, false, 0});
    oob.p->oob = 1;
    CHECK(oob.sm->pair(oob.handle, 2000) == SmResult::Failed && oob.sm->failure_reason() == 2);
    World shortkey;
    CHECK(shortkey.connect_le());
    configure(shortkey, {3, 3, true, true, false, false, 0});
    shortkey.p->max_key = 6;
    CHECK(shortkey.sm->pair(shortkey.handle, 2000) == SmResult::Failed && shortkey.sm->failure_reason() == 6);
    World reversed;
    CHECK(reversed.connect_le());
    reversed.p->smp_send(0x01, Bytes{3, 0, 1, 16, 0, 3});                       // a pairing request from the peripheral
    reversed.c->poll();
    CHECK(!reversed.p->smp_log.empty() && reversed.p->failed_with == 5);
  }
  {   // a method that needs a user interface the host does not have; the peripheral that does not answer; a link that drops mid-pairing
    World w;
    CHECK(w.connect_le());
    configure(w, {2, 0, true, true, true, true, 2});
    w.sm->set_user_interface(nullptr);
    CHECK(w.sm->pair(w.handle, 2000) == SmResult::Failed && w.sm->failure_reason() == 3);
    World silent;
    CHECK(silent.connect_le());
    configure(silent, {3, 3, true, true, false, false, 0});
    silent.host.controller.acl_hook = [](void*, const Bytes&) {};
    CHECK(silent.sm->pair(silent.handle, 50) == SmResult::Timeout && !silent.sm->pairing_active());
    World gone;
    CHECK(gone.connect_le());
    CHECK(gone.sm->pair(0x55, 100) == SmResult::NoLink);
    configure(gone, {3, 3, true, true, false, false, 0});
    gone.host.controller.acl_hook = [](void* self, const Bytes&) { static_cast<World*>(self)->host.controller.push(hci::kEvDisconnectionComplete, {0, 0x40, 0, 0x08}); };
    CHECK(gone.sm->pair(gone.handle, 500) == SmResult::Failed);
  }
  {   // damaged messages from the peripheral: a confirm of the wrong size, a command out of order
    World w;
    CHECK(w.connect_le());
    configure(w, {3, 3, false, false, false, false, 0});
    w.host.controller.acl_hook = [](void* self, const Bytes& packet) {
      World& world = *static_cast<World*>(self);
      if (packet.size() > 8 && packet[8] == 0x01) world.p->smp_send(0x03, Bytes(5, 1));          // the pairing request is answered with a short confirm
    };
    CHECK(w.sm->pair(w.handle, 500) == SmResult::Failed);
  }
}

static void test_bonds_and_encryption() {
  World w;
  CHECK(w.connect_le());
  configure(w, {3, 3, true, true, false, false, 0});
  CHECK(w.sm->encrypt(w.handle) == SmResult::NoBond);
  CHECK(w.sm->pair(w.handle, 5000) == SmResult::Ok && w.bonds.count() == 1);
  // the link drops and comes back: the bond encrypts it without pairing again
  CHECK(w.c->disconnect(w.handle) == HciResult::Ok);
  w.c->poll();
  w.p->encrypted = false;
  hci::BdAddr peer;
  memcpy(peer.b, w.p->address, 6);
  CHECK(w.c->connect_le(peer, 1) == HciResult::Ok);
  w.c->poll();
  CHECK(w.c->link(0x40) && !w.c->link(0x40)->encrypted);
  CHECK(w.sm->encrypt(0x40) == SmResult::Ok && w.c->link(0x40)->encrypted && w.p->encrypted && w.p->pairings_completed == 1);
  CHECK(w.sm->encrypt(0x40) == SmResult::Ok);                                  // (already)
  // a bond whose key the peripheral has forgotten is refused by the controller
  CHECK(w.c->disconnect(0x40) == HciResult::Ok);
  w.c->poll();
  w.p->encrypted = false;
  w.p->have_bond = false;
  CHECK(w.c->connect_le(peer, 1) == HciResult::Ok);
  w.c->poll();
  CHECK(w.sm->encrypt(0x40) == SmResult::Failed && !w.c->link(0x40)->encrypted);
  // the ATT layer asks for security when an attribute needs it: with the bond gone it pairs
  w.p->have_bond = true;
  CHECK(w.bonds.remove(w.c->link(0x40)->address, 1) && w.bonds.count() == 0);
  GattDatabase db;
  CHECK(w.gatt->discover(0x40, &db) == AttStatus::Ok);
  uint8_t value[8];
  uint32_t length = 0;
  CHECK(w.gatt->read(0x40, db, 3, ByteView(value), &length) == AttStatus::Ok && length == 4 && w.bonds.count() == 1 && w.c->link(0x40)->encrypted);
  // a security request from the peripheral encrypts with the bond, or is remembered when there is none
  CHECK(w.c->disconnect(0x40) == HciResult::Ok);
  w.c->poll();
  w.p->encrypted = false;
  CHECK(w.c->connect_le(peer, 1) == HciResult::Ok);
  w.c->poll();
  w.p->smp_send(0x0B, Bytes{0x05});
  w.c->poll();
  CHECK(w.c->link(0x40)->encrypted && w.p->encrypted);
  CHECK(w.c->disconnect(0x40) == HciResult::Ok);
  w.c->poll();
  w.bonds.remove(*reinterpret_cast<const hci::BdAddr*>(w.p->address), 1);
  w.p->encrypted = false;
  CHECK(w.c->connect_le(peer, 1) == HciResult::Ok);
  w.c->poll();
  w.p->smp_send(0x0B, Bytes{0x05});
  w.c->poll();
  uint16_t requested = 0;
  CHECK(w.sm->take_security_request(&requested) && requested == 0x40 && !w.sm->take_security_request(&requested));
}

static void test_bond_store() {
  SmBondStore store;
  SmBond b;
  b.used = true;
  const uint8_t addr[6] = {1, 2, 3, 4, 5, 6};
  memcpy(b.address.b, addr, 6);
  b.address_type = 1;
  b.has_ltk = b.has_irk = true;
  for (int i = 0; i < 16; ++i) { b.ltk[i] = static_cast<uint8_t>(i); b.irk[i] = static_cast<uint8_t>(0x40 + i); }
  b.ediv = 0x4321;
  b.rand[3] = 9;
  b.authenticated = true;
  b.key_size = 12;
  CHECK(store.store(b) && store.count() == 1 && store.find(b.address, 1) && !store.find(b.address, 0));
  // another device, a replaced one, a full table
  SmBond c = b;
  c.address.b[0] = 9;
  CHECK(store.store(c) && store.count() == 2);
  b.key_size = 8;
  CHECK(store.store(b) && store.count() == 2 && store.find(b.address, 1)->key_size == 8);
  for (uint32_t i = 0; i < 40; ++i) { SmBond x = c; x.address.b[1] = static_cast<uint8_t>(i + 10); store.store(x); }
  CHECK(store.count() == SmBondStore::kMaxBonds);
  // a private address made from an IRK resolves to its bond
  SmBondStore resolving;
  SmBond r = b;
  const uint8_t prand[3] = {0x94, 0x81, 0x70};
  uint8_t hash[3];
  CHECK(bt::sm_ah(r.irk, prand, hash));
  hci::BdAddr rpa;
  memcpy(rpa.b, hash, 3);
  memcpy(rpa.b + 3, prand, 3);
  CHECK(resolving.store(r) && resolving.find(rpa, 1) && resolving.find(rpa, 1)->address == r.address && !resolving.find(rpa, 0));
  // the identity address finds it too
  SmBond id = b;
  id.has_identity = true;
  id.identity.b[0] = 0x77;
  id.identity_type = 0;
  SmBondStore ids;
  CHECK(ids.store(id));
  hci::BdAddr identity = id.identity;
  CHECK(ids.find(identity, 0));
  // serialisation: round trip, every truncation, every flipped bit
  TestAllocator a;
  {
    ByteBuffer out(a);
    CHECK(store.serialize(out) && out.size() == 5 + SmBondStore::kMaxBonds * 58);
    SmBondStore back;
    CHECK(back.parse(out.view()) == SmBondStore::kMaxBonds && back.count() == SmBondStore::kMaxBonds);
    const SmBond* got = back.find(b.address, 1);
    CHECK(got && memcmp(got->ltk, b.ltk, 16) == 0 && memcmp(got->irk, b.irk, 16) == 0 && got->ediv == 0x4321 && got->key_size == 8 && got->authenticated && got->rand[3] == 9);
    for (uint64_t n = 0; n < out.size(); n += 7) {                               // cut anywhere: a whole number of records is taken, nothing else
      SmBondStore part;
      const uint32_t taken = part.parse(ConstByteView(out.view().data(), n));
      CHECK(taken <= SmBondStore::kMaxBonds && taken == part.count() && (n < 5 ? taken == 0 : taken <= (n - 5) / 58));
    }
    for (uint64_t bit = 0; bit < 64 * 8; ++bit) {                                // a bit flipped in the header and the first records: refused or still sane
      Bytes flipped(out.view().data(), out.view().data() + out.size());
      flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      SmBondStore damaged;
      const uint32_t taken = damaged.parse(ConstByteView(flipped.data(), flipped.size()));
      CHECK(taken <= SmBondStore::kMaxBonds);
      for (uint32_t i = 0; i < SmBondStore::kMaxBonds; ++i)
        if (const SmBond* x = damaged.at(i)) CHECK(x->key_size >= 7 && x->key_size <= 16 && x->address_type <= 1);
    }
    SmBondStore wrong_tag;
    Bytes bad(out.view().data(), out.view().data() + out.size());
    bad[0] = 'X';
    CHECK(wrong_tag.parse(ConstByteView(bad.data(), bad.size())) == 0);
  }
  CHECK(a.live == 0);
}

// What the peripheral sent during a whole session (MTU, discovery, a secure connections pairing with a passkey) is replayed with one packet cut at every length and with bits flipped: the
// host must neither crash nor read outside what it was given (the sanitizers watch), and must still be able to do something afterwards.
static void test_garbage() {
  std::vector<Bytes> recorded;
  {
    World w;
    CHECK(w.connect_le());
    configure(w, {0, 2, true, true, true, true, 3});
    GattDatabase db;
    CHECK(w.att->exchange_mtu(w.handle, 247) == AttStatus::Ok && w.gatt->discover(w.handle, &db) == AttStatus::Ok && w.sm->pair(w.handle, 5000) == SmResult::Ok);
    w.p->notify(8, Bytes{1, 2, 3});
    w.p->signal(0x12, 3, Bytes(8, 0));
    recorded = w.host.controller.acl_log;
  }
  CHECK(recorded.size() > 20);
  auto replay = [&](const std::vector<Bytes>& packets) {
    World w;
    if (!w.connect_le()) return;
    w.host.controller.acl_hook = [](void*, const Bytes&) {};          // (nobody answers: the host sees only the replay)
    w.p->silent_att = true;
    for (const Bytes& packet : packets) {
      Bytes p = packet;
      if (p.size() >= 2) { p[0] = 0x40; p[1] = static_cast<uint8_t>((p[1] & 0xF0) | 0); }
      w.host.controller.acl_to_host.push_back(p);
    }
    for (int i = 0; i < 4; ++i) w.c->poll();
    CHECK(w.c->link_count() == 1);
  };
  for (size_t k = 0; k < recorded.size(); ++k) {
    for (size_t cut = 0; cut < recorded[k].size(); cut += (recorded[k].size() > 40 ? 5 : 1)) {
      std::vector<Bytes> packets = recorded;
      packets[k].resize(cut);
      replay(packets);
    }
    for (size_t bit = 0; bit < recorded[k].size() * 8 && bit < 320; bit += 3) {
      std::vector<Bytes> packets = recorded;
      packets[k][bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      replay(packets);
    }
  }
  // random packets in a connected session, with a pairing and a discovery going on
  TestRandom r;
  for (int round = 0; round < 60; ++round) {
    World w;
    CHECK(w.connect_le());
    configure(w, {3, 3, true, true, false, false, 0});
    for (int i = 0; i < 12; ++i) {
      Bytes frame(1 + (r.state >> 5) % 40);
      r.fill(ByteView(frame.data(), frame.size()));
      static const uint16_t cids[] = {1, 4, 5, 6, 7, 0x40, 0x41, 0x99};
      uint8_t pick[1];
      r.fill(ByteView(pick, 1));
      w.p->send_frame(cids[pick[0] % 8], frame);
    }
    w.c->poll();
    GattDatabase db;
    w.gatt->discover(w.handle, &db);
    w.sm->pair(w.handle, 200);
    CHECK(w.c->link_count() == 1);
  }
}

// The service builds the upper layers for a controller that comes up, and keeps the bonds in a file.
static void test_service_stack() {
  TestAllocator a;
  filesystem::RamFileSystem files(a);
  FileBluetoothBonds storage(files, a);
  TestRandom random;
  const int base = a.live;
  struct Fw final : IBluetoothFirmwareSource { bool read(const char*, ByteBuffer&) override { return false; } } fw;
  uint8_t before[4];
  {
    Host host;
    host.vendor = 0x0A5C;
    Bus bus;
    bus.hosts = {&host};
    BluetoothService service(&bus, &fw, a, &random, &storage);
    service.start();
    CHECK(service.ready_count() == 1 && service.bonds().count() == 0);
    BluetoothService::Entry* e = const_cast<BluetoothService::Entry*>(service.entry(0));
    CHECK(e && e->l2cap && e->att && e->gatt && e->security);
    // a peripheral on an LE link; pair through the entry's security manager
    Peripheral per(host.controller, 0x40, true);
    per.random = &random;
    World::build_database(per);
    host.controller.hook_context = &per;
    host.controller.acl_hook = [](void* c, const Bytes& p) { static_cast<Peripheral*>(c)->on_acl(p); };
    host.controller.encryption_hook = [](void* c, uint16_t, const uint8_t r[8], uint16_t ediv, const uint8_t k[16]) { Peripheral& x = *static_cast<Peripheral*>(c); const bool ok = x.encryption_request(r, ediv, k); if (ok) x.after_encryption(); return ok; };
    hci::BdAddr peer;
    memcpy(peer.b, per.address, 6);
    HciController* c = service.controller(0);
    CHECK(c && c->connect_le(peer, 1) == HciResult::Ok);
    c->poll();
    per.want_sc = true;
    CHECK(e->security->pair(0x40, 5000) == SmResult::Ok && service.bonds().count() == 1 && service.save_bonds());
    (void)before;
  }
  {   // a new service reads the bond back
    Host host;
    host.vendor = 0x0A5C;
    Bus bus;
    bus.hosts = {&host};
    BluetoothService service(&bus, &fw, a, &random, &storage);
    service.start();
    CHECK(service.bonds().count() == 1);
    // without a random source there is no security manager; without storage nothing is kept
    BluetoothService plain(&bus, &fw, a);
    plain.start();
    CHECK(plain.ready_count() == 1 && plain.entry(0)->l2cap && !plain.entry(0)->security && !plain.save_bonds() && plain.bonds().count() == 0);
  }
  CHECK(a.live == base || a.live <= base + 8);   // (the file system keeps what it was given for the bond file)
}

// ------------------------------------------------------------------------------------------------ HID

// A keyboard with report id 1 and a mouse with report id 2, as one descriptor.
static const uint8_t kKeyboardMouseMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0};

static void test_key_maps() {
  namespace key = hardware::key;
  CHECK(hardware::usage_from_linux_key(113) == key::kMute && hardware::usage_from_linux_key(114) == key::kVolumeDown && hardware::usage_from_linux_key(115) == key::kVolumeUp &&
        hardware::usage_from_linux_key(163) == key::kMediaNext && hardware::usage_from_linux_key(164) == key::kMediaPlayPause && hardware::usage_from_linux_key(165) == key::kMediaPrevious &&
        hardware::usage_from_linux_key(166) == key::kMediaStop && hardware::usage_from_linux_key(224) == key::kBrightnessDown && hardware::usage_from_linux_key(225) == key::kBrightnessUp);
}

static void test_ps2_media_keys() {
  namespace key = hardware::key;
  CHECK(hardware::usage_from_scan_code_set1(0x30, true) == key::kVolumeUp && hardware::usage_from_scan_code_set1(0x2E, true) == key::kVolumeDown && hardware::usage_from_scan_code_set1(0x20, true) == key::kMute &&
        hardware::usage_from_scan_code_set1(0x22, true) == key::kMediaPlayPause && hardware::usage_from_scan_code_set1(0x19, true) == key::kMediaNext && hardware::usage_from_scan_code_set1(0x10, true) == key::kMediaPrevious &&
        hardware::usage_from_scan_code_set1(0x24, true) == key::kMediaStop);
  CHECK(hardware::usage_from_scan_code_set1(0x30, false) != key::kVolumeUp);                                  // (the same code without the E0 prefix is B)
}

static void test_hid_descriptor() {
  using namespace hid;
  ReportMap map;
  CHECK(parse_report_map(ConstByteView(kKeyboardMouseMap, sizeof kKeyboardMouseMap), &map) && map.report_count == 2 && !map.truncated);
  const InputReport* kb = map.find(1);
  const InputReport* ms = map.find(2);
  CHECK(kb && ms && kb->app_page == 1 && kb->app_usage == 6 && ms->app_usage == 2 && kb->bits == 64 && ms->bits == 32 && !map.find(3) && map.find_application(1, 2) == ms);
  KeyboardDecoder kd;
  CHECK(kd.init(*kb) && kd.valid());
  KeyboardState st;
  const uint8_t report[8] = {0x02, 0, 0x04, 0x05, 0, 0, 0, 0};
  CHECK(kd.decode(ConstByteView(report, 8), &st) && st.modifiers == 0x02 && st.key_count == 2 && st.keys[0] == 4 && st.keys[1] == 5);
  const uint8_t rollover[8] = {0, 0, 1, 1, 1, 1, 1, 1};                                // a rollover error is not a key
  CHECK(kd.decode(ConstByteView(rollover, 8), &st) && st.key_count == 0);
  CHECK(!kd.decode(ConstByteView(report, 5), &st));
  KeyboardDiff diff;
  std::vector<std::pair<int, bool>> events;
  auto emit = [&](uint8_t usage, bool pressed) { events.push_back({usage, pressed}); };
  diff.update(st, emit);
  KeyboardState a;
  CHECK(kd.decode(ConstByteView(report, 8), &a));
  diff.update(a, emit);
  CHECK(events.size() == 3 && events[0] == std::make_pair(0xE1, true) && events[1] == std::make_pair(4, true) && events[2] == std::make_pair(5, true));
  events.clear();
  const uint8_t next[8] = {0, 0, 0x05, 0x06, 0, 0, 0, 0};
  KeyboardState b;
  CHECK(kd.decode(ConstByteView(next, 8), &b));
  diff.update(b, emit);
  CHECK(events.size() == 3);                                                          // shift up, A up, 6 down
  events.clear();
  diff.release_all(emit);
  CHECK(events.size() == 2 && !events[0].second && !events[1].second);
  PointerDecoder pd;
  CHECK(pd.init(*ms) && pd.valid() && !pd.absolute());
  MouseEvent e;
  const uint8_t move[4] = {0x05, 0xFE, 0x03, 0xFF};                                   // left+middle, dx -2, dy 3, wheel -1
  CHECK(pd.decode(ConstByteView(move, 4), &e) && e.buttons == 5 && e.dx == -2 && e.dy == 3 && e.wheel == -1 && !e.absolute);
  CHECK(!pd.decode(ConstByteView(move, 3), &e));
  // a tablet, and a keyboard whose keys are a bitmap
  const uint8_t tablet[] = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x01, 0x15, 0x00, 0x25, 0x01, 0x95, 0x01, 0x75, 0x01, 0x81, 0x02,
                            0x95, 0x01, 0x75, 0x07, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0xC0, 0xC0};
  ReportMap tmap;
  CHECK(parse_report_map(ConstByteView(tablet, sizeof tablet), &tmap) && tmap.report_count == 1);
  PointerDecoder tp;
  CHECK(tp.init(tmap.reports[0]) && tp.absolute() && tp.max_x() == 0x7FFF);
  const uint8_t touch[5] = {1, 0x10, 0x20, 0x00, 0x40};
  CHECK(tp.decode(ConstByteView(touch, 5), &e) && e.absolute && e.x == 0x2010 && e.y == 0x4000 && e.buttons == 1 && e.max_x == 0x7FFF);
  const uint8_t nkro[] = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
                          0x19, 0x00, 0x29, 0x77, 0x95, 0x78, 0x81, 0x02, 0xC0};
  ReportMap nmap;
  CHECK(parse_report_map(ConstByteView(nkro, sizeof nkro), &nmap));
  KeyboardDecoder nd;
  CHECK(nd.init(nmap.reports[0]));
  uint8_t bits[16] = {};
  bits[1 + 0] |= 1u << 4;                                                              // usage 4 (A)
  bits[1 + 1] |= 1u << 1;                                                              // usage 9 (F)
  CHECK(nd.decode(ConstByteView(bits, 16), &st) && st.key_count == 2 && st.keys[0] == 4 && st.keys[1] == 9);
  // a descriptor cut at every length and with every bit flipped: nothing read outside, the decoders refuse what does not fit
  for (size_t n = 0; n <= sizeof kKeyboardMouseMap; ++n) {
    ReportMap m;
    parse_report_map(ConstByteView(kKeyboardMouseMap, n), &m);
    for (uint32_t i = 0; i < m.report_count; ++i) { KeyboardDecoder k; PointerDecoder p; k.init(m.reports[i]); p.init(m.reports[i]); k.decode(ConstByteView(report, 8), &st); p.decode(ConstByteView(move, 4), &e); }
  }
  for (size_t bit = 0; bit < sizeof kKeyboardMouseMap * 8; ++bit) {
    uint8_t flipped[sizeof kKeyboardMouseMap];
    memcpy(flipped, kKeyboardMouseMap, sizeof flipped);
    flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    ReportMap m;
    parse_report_map(ConstByteView(flipped, sizeof flipped), &m);
    for (uint32_t i = 0; i < m.report_count; ++i) { KeyboardDecoder k; PointerDecoder p; k.init(m.reports[i]); p.init(m.reports[i]); k.decode(ConstByteView(report, 8), &st); p.decode(ConstByteView(move, 4), &e); p.decode(ConstByteView(report, 1), &e); }
  }
  // the keys of the consumer page (volume, mute, transport): one bit each, and the usages map to the keyboard's key codes
  {
    const uint8_t consumer[] = {0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x07, 0x09, 0xE9, 0x09, 0xEA, 0x09, 0xE2, 0x09, 0xCD,
                                0x09, 0xB5, 0x09, 0xB6, 0x09, 0xB7, 0x81, 0x02, 0x95, 0x01, 0x81, 0x03, 0xC0};
    ReportMap cmap;
    CHECK(parse_report_map(ConstByteView(consumer, sizeof consumer), &cmap) && cmap.report_count == 1 && cmap.reports[0].id == 3 && cmap.reports[0].app_page == 0x0C && cmap.reports[0].app_usage == 1);
    ConsumerDecoder cd;
    CHECK(cd.init(cmap.reports[0]) && cd.valid());
    KeyboardState ck;
    const uint8_t down[1] = {0x05};                                                    // volume up and mute
    CHECK(cd.decode(ConstByteView(down, 1), &ck) && ck.key_count == 2 && ck.keys[0] == hardware::key::kVolumeUp && ck.keys[1] == hardware::key::kMute);
    const uint8_t none[1] = {0};
    CHECK(cd.decode(ConstByteView(none, 1), &ck) && ck.key_count == 0 && !cd.decode(ConstByteView(none, 0), &ck));
    const uint8_t all[1] = {0x7F};
    CHECK(cd.decode(ConstByteView(all, 1), &ck) && ck.key_count == 7);
    CHECK(consumer_usage_to_key(0xE9) == hardware::key::kVolumeUp && consumer_usage_to_key(0xEA) == hardware::key::kVolumeDown && consumer_usage_to_key(0xCD) == hardware::key::kMediaPlayPause &&
          consumer_usage_to_key(0x6F) == hardware::key::kBrightnessUp && consumer_usage_to_key(0x70) == hardware::key::kBrightnessDown && consumer_usage_to_key(0x1234) == 0);
    ConsumerDecoder kbd_only;
    CHECK(!kbd_only.init(*map.find(1)) && !kbd_only.valid());
    for (size_t n = 0; n <= sizeof consumer; ++n) {
      ReportMap m;
      parse_report_map(ConstByteView(consumer, n), &m);
      for (uint32_t i = 0; i < m.report_count; ++i) { ConsumerDecoder d; d.init(m.reports[i]); d.decode(ConstByteView(down, 1), &ck); }
    }
    for (size_t bit = 0; bit < sizeof consumer * 8; ++bit) {
      uint8_t flipped[sizeof consumer];
      memcpy(flipped, consumer, sizeof flipped);
      flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      ReportMap m;
      parse_report_map(ConstByteView(flipped, sizeof flipped), &m);
      for (uint32_t i = 0; i < m.report_count; ++i) { ConsumerDecoder d; d.init(m.reports[i]); d.decode(ConstByteView(down, 1), &ck); d.decode(ConstByteView(all, 1), &ck); }
    }
  }
  // a huge report size and count do not wrap
  const uint8_t huge[] = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x75, 0xFF, 0x96, 0xFF, 0xFF, 0x81, 0x02, 0x75, 0x20, 0x95, 0xFF, 0x81, 0x02, 0xC0};
  ReportMap hm;
  parse_report_map(ConstByteView(huge, sizeof huge), &hm);
  CHECK(extract_bits(ConstByteView(report, 8), 60, 32, false) == 0 && extract_bits(ConstByteView(report, 8), 0xFFFFFFF0u, 8, false) == 0);
}

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

struct PairingUi final : IBluetoothPairingUi {
  Peripheral* peripheral = nullptr;
  uint32_t displayed = 0, displays = 0;
  void display_passkey(const hci::BdAddr&, uint32_t passkey) override { displayed = passkey; ++displays; if (peripheral) peripheral->passkey = passkey; }
  void confirm_numeric(const hci::BdAddr&, uint32_t) override {}
  void request_passkey(const hci::BdAddr&) override {}
};

// A GATT database of a HID peripheral: report map and reports behind encryption, as real keyboards have them.
static void build_hid_database(Peripheral& per, bool report_protocol, bool boot_keyboard) {
  per.add_service(1, 4, Uuid::from16(0x1800));
  per.add_characteristic(2, gatt::kPropRead, Uuid::from16(0x2A00), Bytes{'K', 'e', 'y', 'b', 'o', 'a', 'r', 'd'});
  per.add_service(30, 60, Uuid::from16(0x1812));
  per.add_characteristic(31, gatt::kPropRead | gatt::kPropWriteWithoutResponse, Uuid::from16(0x2A4E), Bytes{1}, false, true);          // protocol mode
  if (report_protocol) {
    per.add_characteristic(33, gatt::kPropRead, Uuid::from16(0x2A4B), Bytes(kKeyboardMouseMap, kKeyboardMouseMap + sizeof kKeyboardMouseMap), true);
    per.add_characteristic(35, gatt::kPropRead | gatt::kPropNotify, Uuid::from16(0x2A4D), Bytes(8, 0), true);                         // keyboard input, id 1
    per.add_cccd(37);
    per.add_descriptor(38, 0x2908, Bytes{1, 1});
    per.add_characteristic(39, gatt::kPropRead | gatt::kPropNotify, Uuid::from16(0x2A4D), Bytes(4, 0), true);                         // mouse input, id 2
    per.add_cccd(41);
    per.add_descriptor(42, 0x2908, Bytes{2, 1});
    per.add_characteristic(43, gatt::kPropRead | gatt::kPropWrite, Uuid::from16(0x2A4D), Bytes(1, 0), true, true);                    // keyboard LEDs, id 1, output
    per.add_descriptor(45, 0x2908, Bytes{1, 2});
  }
  if (boot_keyboard) {
    per.add_characteristic(46, gatt::kPropRead | gatt::kPropNotify, Uuid::from16(0x2A22), Bytes(8, 0), true);
    per.add_cccd(48);
  }
}

struct HidWorld : World {
  TestSink sink;
  BluetoothProfileRegistry profiles;
  HidOverGatt hogp{a};
  UniquePointer<HciBluetoothAdapter> adapter;
  PairingUi pairing_ui;
  HidWorld() {
    profiles.add(&hogp);
    adapter = UniquePointer<HciBluetoothAdapter>::make(a, *c, *l2, *att, *gatt, sm.get(), bonds, profiles, a);
    adapter->set_pairing_ui(&pairing_ui);
  }
  void make_peripheral(bool report_protocol = true, bool boot = false) {
    handle = 0x40;
    p = UniquePointer<Peripheral>::make(a, host.controller, handle, true);
    p->random = &random;
    build_hid_database(*p, report_protocol, boot);
    pairing_ui.peripheral = p.get();
    ui.peripheral = p.get();
  }
  void advertise() { host.controller.advertisements(); }
  // Polls the adapter (and the input source) until the device is in the wanted state; false if it never gets there.
  bool run_until(const hci::BdAddr& address, BluetoothLinkState wanted, int limit = 3000) {
    for (int i = 0; i < limit; ++i) {
      adapter->poll();
      hogp.poll(sink);
      BluetoothDeviceInfo info;
      if (adapter->find_device(address, &info) && info.state == wanted) return true;
    }
    return false;
  }
  hci::BdAddr peer() const { hci::BdAddr b; memcpy(b.b, p->address, 6); return b; }
};

static void test_hid_end_to_end() {
  {   // discovery, connection with pairing (just works), the keyboard and mouse appear in the input system, reports flow, the device leaves
    HidWorld w;
    w.make_peripheral();
    CHECK(w.adapter->ready() && w.adapter->start_discovery() == BluetoothResult::Ok);
    w.advertise();
    w.adapter->poll();
    BluetoothDeviceInfo info;
    CHECK(w.adapter->find_device(w.peer(), &info) && info.le && info.connectable && strcmp(info.name, "Keyboard") == 0 && info.state == BluetoothLinkState::Disconnected && !info.paired && info.appearance == 0x03C1);
    CHECK(w.adapter->connect(w.peer()) == BluetoothResult::Ok && w.adapter->connect(w.peer()) == BluetoothResult::Busy);
    CHECK(w.run_until(w.peer(), BluetoothLinkState::Connected));
    CHECK(w.adapter->find_device(w.peer(), &info) && info.paired && strcmp(info.profiles, "HID over GATT") == 0 && w.bonds.count() == 1);
    CHECK(w.hogp.device_count() == 1 && w.sink.keyboards.count() == 1 && w.sink.mice.count() == 1 && w.sink.noted("Keyboard: connected"));
    // keys: shift+A, then A released, with a mouse report and a report of the keyboard's output kind that is ignored
    w.p->notify(36, Bytes{0x02, 0, 0x04, 0, 0, 0, 0, 0});
    w.adapter->poll();
    std::vector<KeyEvent> keys = w.sink.drain_keys();
    CHECK(keys.size() == 2 && keys[0].usage == 0xE1 && keys[0].pressed && keys[1].usage == 4 && keys[1].pressed && w.sink.keyboards.modifiers() == 0x02);
    w.p->notify(36, Bytes{0x02, 0, 0, 0, 0, 0, 0, 0});
    w.p->notify(40, Bytes{0x01, 5, 0xFD, 1});
    w.adapter->poll();
    keys = w.sink.drain_keys();
    CHECK(keys.size() == 1 && keys[0].usage == 4 && !keys[0].pressed);
    std::vector<MouseEvent> mouse = w.sink.drain_mouse();
    CHECK(mouse.size() == 1 && mouse[0].buttons == 1 && mouse[0].dx == 5 && mouse[0].dy == -3 && mouse[0].wheel == 1);
    w.p->notify(44, Bytes{1});
    w.adapter->poll();
    CHECK(w.sink.drain_keys().empty());
    // a key held when the link drops is released
    w.p->notify(36, Bytes{0, 0, 0x07, 0, 0, 0, 0, 0});
    w.adapter->poll();
    w.sink.drain_keys();
    CHECK(w.adapter->disconnect(w.peer()) == BluetoothResult::Ok);
    w.adapter->poll();
    w.hogp.poll(w.sink);
    keys = w.sink.drain_keys();
    CHECK(keys.size() == 1 && keys[0].usage == 7 && !keys[0].pressed && w.sink.keyboards.count() == 0 && w.sink.mice.count() == 0 && w.sink.noted("disconnected") && w.hogp.device_count() == 0);
    CHECK(w.adapter->find_device(w.peer(), &info) && info.state == BluetoothLinkState::Disconnected && info.paired);
    // coming back by itself when the keyboard advertises: the bond encrypts the link, no pairing
    const uint32_t pairings = w.p->pairings_completed;
    w.p->encrypted = false;
    w.adapter->set_auto_reconnect(true);
    w.adapter->poll();
    w.advertise();
    CHECK(w.run_until(w.peer(), BluetoothLinkState::Connected) && w.p->pairings_completed == pairings && w.sink.keyboards.count() == 1);
    // forgetting removes the bond and the device
    CHECK(w.adapter->forget(w.peer()) == BluetoothResult::Ok && w.bonds.count() == 0);
    w.adapter->poll();
    w.hogp.poll(w.sink);
    CHECK(w.sink.keyboards.count() == 0 && w.adapter->forget(w.peer()) == BluetoothResult::NoSuchDevice);
  }
  {   // a device that makes the user type a passkey: the adapter shows it through the pairing interface
    HidWorld w;
    w.make_peripheral();
    w.p->io_capability = 2;               // keyboard only
    w.p->want_mitm = true;
    w.p->method = 3;                      // the host displays, the keyboard types
    w.advertise();
    w.adapter->poll();
    CHECK(w.adapter->connect(w.peer()) == BluetoothResult::Ok && w.run_until(w.peer(), BluetoothLinkState::Connected));
    CHECK(w.pairing_ui.displays == 1 && w.pairing_ui.displayed < 1000000);
    BluetoothDeviceInfo info;
    CHECK(w.adapter->find_device(w.peer(), &info) && info.authenticated);
  }
  {   // the boot protocol: no report map, a boot keyboard input report
    HidWorld w;
    w.make_peripheral(false, true);
    w.advertise();
    w.adapter->poll();
    CHECK(w.adapter->connect(w.peer()) == BluetoothResult::Ok && w.run_until(w.peer(), BluetoothLinkState::Connected));
    CHECK(w.sink.keyboards.count() == 1 && w.sink.mice.count() == 0 && w.p->attr(32)->value == Bytes{0});            // (protocol mode set to boot)
    w.p->notify(47, Bytes{0x01, 0, 0x1E, 0, 0, 0, 0, 0});
    w.adapter->poll();
    std::vector<KeyEvent> keys = w.sink.drain_keys();
    CHECK(keys.size() == 2 && keys[0].usage == 0xE0 && keys[1].usage == 0x1E);
  }
  {   // refusals and failures
    HidWorld w;
    w.make_peripheral();
    hci::BdAddr nobody;
    CHECK(w.adapter->connect(nobody) == BluetoothResult::NoSuchDevice && w.adapter->disconnect(nobody) == BluetoothResult::NoSuchDevice);
    w.advertise();
    w.adapter->poll();
    w.host.controller.connect_status = hci::kStatusMemoryFull;
    CHECK(w.adapter->connect(w.peer()) == BluetoothResult::Ok);
    BluetoothDeviceInfo info;
    CHECK(w.adapter->find_device(w.peer(), &info) && info.state == BluetoothLinkState::Failed && strlen(info.failure) > 0);
    w.host.controller.connect_status = 0;
    w.p->reject_pairing = true;                                        // the device refuses to pair
    CHECK(w.adapter->connect(w.peer()) == BluetoothResult::Ok && w.run_until(w.peer(), BluetoothLinkState::Failed));
    CHECK(w.adapter->find_device(w.peer(), &info) && strstr(info.failure, "does not support pairing") && w.sink.keyboards.count() == 0 && w.adapter->connected_count() == 0);
    w.p->reject_pairing = false;
    // a classic device cannot be connected to
    w.host.controller.inquiry();
    w.c->poll();
    BluetoothDeviceInfo classic;
    bool found_classic = false;
    for (uint32_t i = 0; i < w.adapter->device_count(); ++i)
      if (w.adapter->device(i, &classic) && !classic.le) { found_classic = true; break; }
    CHECK(found_classic && w.adapter->connect(classic.address) == BluetoothResult::Unsupported);
  }
  {   // a device that never answers the service discovery: the attempt fails with a reason, and nothing is left connected
    HidWorld w;
    w.make_peripheral();
    w.advertise();
    w.adapter->poll();
    w.p->silent_att = true;
    CHECK(w.adapter->connect(w.peer()) == BluetoothResult::Ok && w.run_until(w.peer(), BluetoothLinkState::Failed));
    BluetoothDeviceInfo info;
    CHECK(w.adapter->find_device(w.peer(), &info) && strstr(info.failure, "services") && w.adapter->connected_count() == 0 && w.hogp.device_count() == 0);
    // and a connection in progress can be cancelled
    w.p->silent_att = false;
    CHECK(w.adapter->connect(w.peer()) == BluetoothResult::Ok);
    w.adapter->poll();
    CHECK(w.adapter->disconnect(w.peer()) == BluetoothResult::Ok);
    CHECK(w.adapter->connect(w.peer()) == BluetoothResult::Ok && w.run_until(w.peer(), BluetoothLinkState::Connected));
  }
}

// A profile registered by someone who knows nothing of the adapter or of HID: it is offered the device, and what it makes reaches the system through its own interface.
static void test_profile_plugin() {
  struct Battery final : IBluetoothProfile, IBluetoothNotificationSink {
    int attached = 0, detached = 0, notifications = 0;
    uint8_t level = 0;
    const char* name() const override { return "Battery"; }
    bool matches(const BluetoothDeviceInfo&, const GattDatabase& db) const override { return db.find_service(Uuid::from16(0x180F)) >= 0; }
    bool attach(IBluetoothRemote& r) override {
      ++attached;
      const int c = r.database().find_characteristic(Uuid::from16(0x2A19));
      uint8_t v[4];
      uint32_t n = 0;
      if (c < 0 || r.read(c, ByteView(v), &n) != AttStatus::Ok || n != 1) return false;
      level = v[0];
      return r.subscribe(c, false, this) == AttStatus::Ok;
    }
    void detach(IBluetoothRemote&) override { ++detached; }
    void on_notification(IBluetoothRemote&, uint16_t, ConstByteView v, bool) override { ++notifications; level = v[0]; }
  } battery;
  World w;
  CHECK(w.connect_le());
  BluetoothProfileRegistry profiles;
  CHECK(profiles.add(&battery) && profiles.add(&battery) && profiles.count() == 1);
  UniquePointer<HciBluetoothAdapter> adapter = UniquePointer<HciBluetoothAdapter>::make(w.a, *w.c, *w.l2, *w.att, *w.gatt, w.sm.get(), w.bonds, profiles, w.a);
  // the peripheral is connected already by World; hand the link to a new operation by disconnecting and connecting through the adapter
  CHECK(w.c->disconnect(w.handle) == HciResult::Ok);
  w.c->poll();
  w.host.controller.advertisements();
  adapter->poll();
  hci::BdAddr peer;
  memcpy(peer.b, w.p->address, 6);
  CHECK(adapter->connect(peer) == BluetoothResult::Ok);
  for (int i = 0; i < 500 && battery.attached == 0; ++i) adapter->poll();
  BluetoothDeviceInfo info;
  CHECK(battery.attached == 1 && battery.level == 87 && adapter->find_device(peer, &info) && strcmp(info.profiles, "Battery") == 0);
  w.p->notify(8, Bytes{55});
  adapter->poll();
  CHECK(battery.notifications == 1 && battery.level == 55);
  CHECK(adapter->disconnect(peer) == BluetoothResult::Ok && battery.detached == 1);
  CHECK(profiles.remove(&battery) && !profiles.remove(&battery) && profiles.count() == 0);
}

// The desktop's view of the adapters (SystemBluetoothDetails) over a real adapter: the device rows, and the pairing questions reaching the window and the answers going back.
static void test_system_details() {
  struct Manager final : IBluetoothManager {
    IBluetoothAdapter* a = nullptr;
    BluetoothProfileRegistry* profiles = nullptr;
    bool reconnect = false;
    bool auto_reconnect() const override { return reconnect; }
    void set_auto_reconnect(bool on) override { reconnect = on; if (a) a->set_auto_reconnect(on); }
    uint32_t adapter_count() const override { return a ? 1 : 0; }
    IBluetoothAdapter* adapter(uint32_t i) override { return i == 0 ? a : nullptr; }
    bool add_profile(IBluetoothProfile* p) override { return profiles->add(p); }
    bool remove_profile(IBluetoothProfile* p) override { return profiles->remove(p); }
  };
  auto settle = [](HidWorld& w, SystemBluetoothDetails& d, desktop::BluetoothPromptKind* seen, uint32_t* number, int rounds) {
    for (int i = 0; i < rounds; ++i) {
      w.adapter->poll();
      w.hogp.poll(w.sink);
      desktop::BluetoothPrompt prompt;
      if (d.prompt(0, &prompt)) { *seen = prompt.kind; *number = prompt.number; return true; }
    }
    return false;
  };
  {   // rows
    HidWorld w;
    w.make_peripheral();
    Manager manager;
    manager.a = w.adapter.get();
    manager.profiles = &w.profiles;
    SystemBluetoothDetails details(manager);
    CHECK(details.is_bluetooth(0) && !details.is_bluetooth(1));
    desktop::BluetoothAdapterInfo info;
    CHECK(details.adapter(0, &info) && strcmp(info.name, "Bluetooth adapter") == 0 && strcmp(info.address, "66:55:44:33:22:11") == 0 && info.ready && !info.discovering && !details.adapter(1, &info));
    CHECK(!info.auto_reconnect);
    details.set_auto_reconnect(0, true);
    CHECK(manager.reconnect && w.adapter->auto_reconnect() && details.adapter(0, &info) && info.auto_reconnect);
    details.set_auto_reconnect(1, false);                              // (no such adapter: nothing changes)
    CHECK(manager.reconnect);
    details.set_auto_reconnect(0, false);
    CHECK(!manager.reconnect && !w.adapter->auto_reconnect());
    CHECK(details.set_discovery(0, true) && details.adapter(0, &info) && info.discovering);
    w.advertise();
    w.host.controller.inquiry();
    w.adapter->poll();
    desktop::BluetoothDeviceRow rows[desktop::IBluetoothDetails::kMaxDevices];
    const uint32_t n = details.devices(0, rows, desktop::IBluetoothDetails::kMaxDevices);
    CHECK(n == 6);
    // the supported ones first, by signal: the keyboard (-53 dBm), then the beacon (-80 dBm), then the classic devices
    CHECK(strcmp(rows[0].name, "Keyboard") == 0 && strcmp(rows[0].kind, "Keyboard") == 0 && rows[0].signal_percent == 94 && rows[0].supported && !rows[0].paired && strcmp(rows[0].address, "C6:C5:C4:C3:C2:C1") == 0 && rows[0].key[0] == 0xC1);
    CHECK(strcmp(rows[1].kind, "Device") == 0 && rows[1].signal_percent == 40 && rows[1].name[0] == 0);
    uint32_t classic = 0;
    for (uint32_t i = 2; i < n; ++i) classic += !rows[i].supported;
    CHECK(classic == 4);
    bool saw_phone = false, saw_headset_ext = false;
    for (uint32_t i = 2; i < n; ++i) { saw_phone = saw_phone || strcmp(rows[i].kind, "Phone") == 0; saw_headset_ext = saw_headset_ext || strcmp(rows[i].name, "Headset X") == 0; }
    CHECK(saw_phone && saw_headset_ext);
    CHECK(details.devices(0, rows, 2) == 2 && details.devices(1, rows, 2) == 0);
    CHECK(details.connect(0, rows[2].key) == desktop::BluetoothResult::Unsupported && details.connect(1, rows[0].key) == desktop::BluetoothResult::NotReady);
    uint8_t nobody[6] = {9, 9, 9, 9, 9, 9};
    CHECK(details.connect(0, nobody) == desktop::BluetoothResult::NoSuchDevice);
    details.set_discovery(0, false);
    CHECK(details.adapter(0, &info) && !info.discovering);
  }
  struct Case { const char* name; uint8_t central_io_unused; uint8_t peripheral_io; bool mitm; int method; desktop::BluetoothPromptKind prompt; };
  const Case cases[] = {
      {"just works", 0, 3, false, 0, desktop::BluetoothPromptKind::None},
      {"the host shows a passkey", 0, 2, true, 3, desktop::BluetoothPromptKind::ShowPasskey},
      {"the device shows a passkey", 0, 0, true, 2, desktop::BluetoothPromptKind::EnterPasskey},
      {"numbers to compare", 0, 1, true, 1, desktop::BluetoothPromptKind::ConfirmNumber},
  };
  for (const Case& c : cases) {
    HidWorld w;
    w.make_peripheral();
    w.p->io_capability = c.peripheral_io;
    w.p->want_mitm = c.mitm;
    w.p->method = c.method;
    w.p->passkey = 123456;
    Manager manager;
    manager.a = w.adapter.get();
    manager.profiles = &w.profiles;
    SystemBluetoothDetails details(manager);
    struct Reader { SystemBluetoothDetails* details; desktop::BluetoothPromptKind kind = desktop::BluetoothPromptKind::None; uint32_t number = 0; } reader{&details};
    if (c.prompt == desktop::BluetoothPromptKind::ShowPasskey) {      // (the pairing does not wait for the user to type: the peripheral reads the number off the screen when it needs it)
      w.p->passkey_provider = [](void* context) -> uint32_t {
        Reader& r = *static_cast<Reader*>(context);
        desktop::BluetoothPrompt prompt;
        if (r.details->prompt(0, &prompt)) { r.kind = prompt.kind; r.number = prompt.number; }
        return r.number;
      };
      w.p->provider_context = &reader;
    }
    w.advertise();
    w.adapter->poll();
    uint8_t key[6];
    memcpy(key, w.p->address, 6);
    CHECK(details.connect(0, key) == desktop::BluetoothResult::Ok);
    desktop::BluetoothPromptKind seen = desktop::BluetoothPromptKind::None;
    uint32_t number = 0;
    const bool asked = c.prompt == desktop::BluetoothPromptKind::ShowPasskey ? false : settle(w, details, &seen, &number, 600);
    if (c.prompt == desktop::BluetoothPromptKind::ShowPasskey) {
      for (int i = 0; i < 2000 && reader.kind == desktop::BluetoothPromptKind::None; ++i) { w.adapter->poll(); w.hogp.poll(w.sink); }
      CHECK(reader.kind == desktop::BluetoothPromptKind::ShowPasskey && reader.number < 1000000);
    } else if (c.prompt == desktop::BluetoothPromptKind::None) {
      CHECK(!asked);
    } else {
      CHECK(asked && seen == c.prompt);
      desktop::BluetoothPrompt prompt;
      CHECK(details.prompt(0, &prompt) && strcmp(prompt.device, "Keyboard") == 0);
      if (seen == desktop::BluetoothPromptKind::ConfirmNumber) {
        details.answer_confirm(0, true);
      } else {
        details.answer_passkey(0, 123456);
      }
      CHECK(!details.prompt(0, &prompt));                              // (answered: the question is gone)
    }
    BluetoothDeviceInfo bt;
    bool connected = false;
    for (int i = 0; i < 2000 && !connected; ++i) {
      w.adapter->poll();
      w.hogp.poll(w.sink);
      connected = w.adapter->find_device(w.peer(), &bt) && bt.state == BluetoothLinkState::Connected;
    }
    CHECK(connected && w.sink.keyboards.count() == 1);
    if (c.method == 1) CHECK(number == w.p->numeric_value);
    desktop::BluetoothDeviceRow rows[8];
    CHECK(details.devices(0, rows, 8) >= 1 && rows[0].state == desktop::BluetoothDeviceState::Connected && rows[0].paired && strcmp(rows[0].profiles, "HID over GATT") == 0);
    details.disconnect(0, key);
    w.adapter->poll();
    CHECK(details.devices(0, rows, 8) >= 1 && rows[0].state == desktop::BluetoothDeviceState::Disconnected && rows[0].paired);
    details.forget(0, key);
    CHECK(details.devices(0, rows, 8) >= 1 && !rows[0].paired);
  }
  {   // a numbers-to-compare question the user rejects fails the pairing, and the window sees the failure
    HidWorld w;
    w.make_peripheral();
    w.p->io_capability = 1;
    w.p->want_mitm = true;
    w.p->method = 1;
    Manager manager;
    manager.a = w.adapter.get();
    manager.profiles = &w.profiles;
    SystemBluetoothDetails details(manager);
    w.advertise();
    w.adapter->poll();
    uint8_t key[6];
    memcpy(key, w.p->address, 6);
    CHECK(details.connect(0, key) == desktop::BluetoothResult::Ok);
    desktop::BluetoothPromptKind seen = desktop::BluetoothPromptKind::None;
    uint32_t number = 0;
    CHECK(settle(w, details, &seen, &number, 600) && seen == desktop::BluetoothPromptKind::ConfirmNumber);
    details.answer_confirm(0, false);
    CHECK(w.run_until(w.peer(), BluetoothLinkState::Failed));
    desktop::BluetoothDeviceRow rows[8];
    CHECK(details.devices(0, rows, 8) >= 1 && rows[0].state == desktop::BluetoothDeviceState::Failed && strstr(rows[0].failure, "numbers"));
    desktop::BluetoothPrompt prompt;
    CHECK(!details.prompt(0, &prompt));
  }
}

// The auto-reconnect choice belongs to the service: kept in the bond file after the bonds, read back at start-up, followed by adapters that appear later.
static void test_auto_reconnect_setting() {
  TestAllocator a;
  filesystem::RamFileSystem files(a);
  FileBluetoothBonds storage(files, a);
  TestRandom random;
  struct Fw final : IBluetoothFirmwareSource { bool read(const char*, ByteBuffer&) override { return false; } } fw;
  {   // off by default; switching it on is applied to the adapter and saved with the (empty) bonds
    Host host;
    host.vendor = 0x0A5C;
    Bus bus;
    bus.hosts = {&host};
    BluetoothService service(&bus, &fw, a, &random, &storage);
    service.start();
    CHECK(!service.auto_reconnect() && !service.entry(0)->adapter->auto_reconnect());
    service.set_auto_reconnect(true);
    CHECK(service.auto_reconnect() && service.entry(0)->adapter->auto_reconnect());
    service.set_auto_reconnect(true);                                   // (no change: nothing to do)
    // a bond is added: the next save keeps both
    SmBond bond;
    bond.used = true;
    bond.address.b[0] = 9;
    bond.address_type = 1;
    bond.has_ltk = true;
    CHECK(service.bonds().store(bond) && service.save_bonds());
  }
  {   // the next start finds the choice and the bond; an adapter that comes up is set from it
    Host host;
    host.vendor = 0x0A5C;
    Bus bus;
    bus.hosts = {&host};
    BluetoothService service(&bus, &fw, a, &random, &storage);
    service.start();
    CHECK(service.auto_reconnect() && service.bonds().count() == 1 && service.entry(0)->adapter->auto_reconnect());
    service.set_auto_reconnect(false);
    CHECK(!service.auto_reconnect() && !service.entry(0)->adapter->auto_reconnect());
  }
  {   // switched off again, and kept so
    Host host;
    host.vendor = 0x0A5C;
    Bus bus;
    bus.hosts = {&host};
    BluetoothService service(&bus, &fw, a, &random, &storage);
    service.start();
    CHECK(!service.auto_reconnect() && service.bonds().count() == 1);
    // a controller that is plugged in later follows the choice
    service.set_auto_reconnect(true);
    Host second;
    second.vendor = 0x0A5C;
    bus.hosts.push_back(&second);
    service.step(true);
    CHECK(service.count() == 2 && service.entry(1)->adapter && service.entry(1)->adapter->auto_reconnect());
  }
  {   // a file from before there were settings, and damaged or cut ones: the default, and the bonds as far as they go
    ByteBuffer file(a);
    SmBondStore store;
    SmBond bond;
    bond.used = true;
    bond.address.b[0] = 5;
    bond.has_ltk = true;
    store.store(bond);
    CHECK(store.serialize(file) && file.size() == SmBondStore::kHeaderBytes + SmBondStore::kRecordBytes);
    CHECK(storage.save(file.view()));
    Host host;
    host.vendor = 0x0A5C;
    Bus bus;
    bus.hosts = {&host};
    {
      BluetoothService service(&bus, &fw, a, &random, &storage);
      service.start();
      CHECK(!service.auto_reconnect() && service.bonds().count() == 1);
    }
    for (uint32_t cut = 1; cut <= 5; ++cut) {                          // the trailer cut short
      ByteBuffer partial(a);
      CHECK(partial.append(file.view()) && partial.put_tag("BTST") && partial.put8(1));
      partial.truncate(partial.size() - cut);
      CHECK(storage.save(partial.view()));
      BluetoothService service(&bus, &fw, a, &random, &storage);
      service.start();
      CHECK(!service.auto_reconnect() && service.bonds().count() == 1);
    }
    ByteBuffer wrong(a);
    CHECK(wrong.append(file.view()) && wrong.put_tag("XXXX") && wrong.put8(1) && storage.save(wrong.view()));
    BluetoothService service(&bus, &fw, a, &random, &storage);
    service.start();
    CHECK(!service.auto_reconnect());
    ByteBuffer on(a);
    CHECK(on.append(file.view()) && on.put_tag("BTST") && on.put8(3) && storage.save(on.view()));
    BluetoothService again(&bus, &fw, a, &random, &storage);
    again.start();
    CHECK(again.auto_reconnect());
  }
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_crypto();
  test_bond_store();
  test_l2cap();
  test_att_gatt();
  test_pairing();
  test_pairing_failures();
  test_bonds_and_encryption();
  test_garbage();
  test_service_stack();
  test_key_maps();
  test_ps2_media_keys();
  test_hid_descriptor();
  test_hid_end_to_end();
  test_profile_plugin();
  test_system_details();
  test_auto_reconnect_setting();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("bluetooth stack tests passed\n");
  return 0;
}
