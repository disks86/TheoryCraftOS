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

// Tests of the Thunderbolt 3 host interface driver (src/Hardware/Bus/Thunderbolt): the frame format (an independent CRC-32C, every single bit flipped, every truncation), the DMA rings
// against a model of the chip, the mailbox, the control channel's configuration space reads and writes with their retries and errors, the ICM messages and their parsers (cut and
// flipped), the start-up in each way it can fail, the device list kept from events, the approval of a device with the security levels, and the driver's clean shutdown.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <sys/mman.h>
#include <vector>
#include "FakeThunderbolt.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltKeys.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltControl.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltDevices.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltDriver.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltIcm.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltRing.hpp"

namespace hardware {
void delay_us(uint64_t) {}
}  // namespace hardware

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// DMA memory in the low 2 GiB, as the kernel's pool, so that a pointer is the bus address.
struct TestAllocator final : allocators::IAllocator {
  std::map<void*, size_t> dma;
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void* allocate_dma(uint64_t size, uint64_t) override {
    const size_t rounded = (size + 4095) / 4096 * 4096;
    void* p = mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    dma[p] = rounded;
    return p;
  }
  void deallocate(void* p) override {
    if (!p) return;
    auto it = dma.find(p);
    if (it != dma.end()) { munmap(p, it->second); dma.erase(it); } else free(p);
  }
};

using fake_tb::FakeNhi;

static std::vector<uint8_t> as_bytes(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> b;
  for (uint32_t w : words) for (int s = 0; s < 32; s += 8) b.push_back(static_cast<uint8_t>(w >> s));
  return b;
}

static void test_devices() {
  CHECK(tb_find_nhi(0x8086, 0x15BF) && tb_find_nhi(0x8086, 0x15D2) && tb_find_nhi(0x8086, 0x15D9) && tb_find_nhi(0x8086, 0x15DC));
  CHECK(tb_find_nhi(0x8086, 0x15BF)->generation == TbGeneration::AlpineRidge);
  CHECK(!tb_find_nhi(0x8086, 0x15EB) && !tb_find_nhi(0x8086, 0x9A1B) && !tb_find_nhi(0x10DE, 0x15BF) && !tb_find_nhi(0x8086, 0));
}

static void test_framing() {
  // CRC-32C of "123456789" is the standard check value
  CHECK(fake_tb::crc32c_bitwise(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xE3069283u);
  // one word: 0x01020304 is sent as 01 02 03 04 with its CRC after
  const std::vector<uint8_t> body = as_bytes({0x01020304u, 0xCAFEF00Du});
  uint8_t frame[64];
  const uint32_t n = tb_frame::encode(ConstByteView(body.data(), body.size()), ByteView(frame, sizeof frame));
  CHECK(n == 12);
  const std::vector<uint8_t> expect = fake_tb::wire_frame({0x01020304u, 0xCAFEF00Du});
  CHECK(expect.size() == n && memcmp(frame, expect.data(), n) == 0);
  CHECK(frame[0] == 1 && frame[1] == 2 && frame[2] == 3 && frame[3] == 4);
  // decode returns the words in memory order
  CHECK(tb_frame::decode(ByteView(frame, n), true) == 8 && memcmp(frame, body.data(), 8) == 0);
  // refused: odd sizes, empty, too long, a buffer that is too small
  CHECK(tb_frame::encode(ConstByteView(body.data(), 6), ByteView(frame, sizeof frame)) == 0 && tb_frame::encode(ConstByteView(), ByteView(frame, sizeof frame)) == 0);
  std::vector<uint8_t> big(256, 0);
  uint8_t bigout[300];
  CHECK(tb_frame::encode(ConstByteView(big.data(), 256), ByteView(bigout, sizeof bigout)) == 0 && tb_frame::encode(ConstByteView(big.data(), 252), ByteView(bigout, sizeof bigout)) == 256);
  CHECK(tb_frame::encode(ConstByteView(body.data(), 8), ByteView(frame, 11)) == 0);
  // every single bit flipped in a frame is caught by the CRC, every truncation is refused (and does not touch memory it should not: ASan)
  const std::vector<uint8_t> good = fake_tb::wire_frame({0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u, 0x0F1E2D3Cu});
  for (size_t bit = 0; bit < good.size() * 8; ++bit) {
    std::vector<uint8_t> f = good;
    f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    CHECK(tb_frame::decode(ByteView(f.data(), f.size()), true) == 0);
  }
  for (size_t len = 0; len < good.size(); ++len) {
    std::vector<uint8_t> f(good.begin(), good.begin() + len);
    CHECK(tb_frame::decode(ByteView(f.data(), f.size()), true) == 0);
  }
  std::vector<uint8_t> ok = good;
  CHECK(tb_frame::decode(ByteView(ok.data(), ok.size()), true) == good.size() - 4);
  // without the CRC check (ICM frames) a flipped body is accepted as it is
  std::vector<uint8_t> flipped = good;
  flipped[3] ^= 0x40;
  CHECK(tb_frame::decode(ByteView(flipped.data(), flipped.size()), false) == good.size() - 4);
  std::vector<uint8_t> huge(260, 0);
  CHECK(tb_frame::decode(ByteView(huge.data(), huge.size()), false) == 0);
}

static void test_nhi() {
  FakeNhi chip;
  TbNhi nhi(chip);
  CHECK(nhi.present() && nhi.hop_count() == 12 && nhi.capability_version() == 1);
  chip.present = false;
  CHECK(!nhi.present());
  chip.present = true;
  nhi.disable_interrupts();
  CHECK(chip.interrupt_mask_writes == (31 + 2 * 12) / 32 && chip.interrupt_clear_writes == (31 + 3 * 12) / 32);
  CHECK(nhi.icm_running() && nhi.firmware_mode() == TbFirmwareMode::ConnectionManager);
  chip.icm_enabled = false;
  CHECK(!nhi.icm_running());
  chip.mode = 0;
  CHECK(nhi.firmware_mode() == TbFirmwareMode::Safe);
  chip.mode = 9;
  CHECK(nhi.firmware_mode() == TbFirmwareMode::Other);
  chip.auth_done = false;
  CHECK(!nhi.wait_firmware_authenticated(1000));
  chip.auth_done = true;
  CHECK(nhi.wait_firmware_authenticated(1000));
  CHECK(nhi.mailbox(tb_mailbox::kAllowAllDevices, 0, 1000) == TbMailboxResult::Ok && chip.mailbox_commands.size() == 1 && chip.mailbox_commands[0] == 0x23);
  chip.mailbox_error = true;
  CHECK(nhi.mailbox(tb_mailbox::kSaveDevices, 1, 1000) == TbMailboxResult::Error);
  chip.mailbox_error = false;
  chip.mailbox_hang = true;
  CHECK(nhi.mailbox(tb_mailbox::kDriverUnloads, 0, 1000) == TbMailboxResult::Timeout);
}

static void test_ring() {
  TestAllocator alloc;
  FakeNhi chip;
  {
    TbRing tx(chip, alloc, 3, true, 8), rx(chip, alloc, 3, false, 8);
    CHECK(!tx.running() && !tx.post_transmit(0x1000, 16, 1, 1));              // not started
    CHECK(tx.start(0, 0, 0) && tx.running() && tx.free_slots() == 7);
    CHECK(chip.tx_enabled(3) && chip.tx(3).size == 8 && chip.tx(3).base != 0 && (chip.tx(3).options & (1u << 30)));
    CHECK(rx.start(256, 0xFFFF, 0xFFFF) && chip.rx(3).frame_size == 256 && chip.rx(3).size == 8);
    // transmit: seven fit, the eighth does not; the frames reach the chip with their SOF / EOF
    std::vector<uint8_t> buffers(8 * 64, 0xAB);
    for (uint32_t i = 0; i < 7; ++i) CHECK(tx.post_transmit(reinterpret_cast<uint64_t>(&buffers[i * 64]), 20 + i, 5, 6));
    CHECK(tx.free_slots() == 0 && !tx.post_transmit(0x1000, 16, 1, 1));
    CHECK(!tx.post_transmit(0x1000, 0, 1, 1) && !tx.post_transmit(0x1000, 5000, 1, 1) && !tx.post_receive(0x1000));
    CHECK(chip.sent.size() == 7 && chip.sent[0].hop == 3 && chip.sent[0].sof == 5 && chip.sent[0].eof == 6 && chip.sent[0].bytes.size() == 20 && chip.sent[6].bytes.size() == 26 && chip.sent[3].bytes[0] == 0xAB);
    uint32_t done = 0;
    TbCompletion c;
    while (tx.poll(&c)) { CHECK(c.slot == done && (c.flags & TbRing::kDescCompleted)); ++done; }
    CHECK(done == 7 && tx.free_slots() == 7);
    // the indices wrap around the ring
    for (int round = 0; round < 3; ++round) {
      for (uint32_t i = 0; i < 5; ++i) CHECK(tx.post_transmit(reinterpret_cast<uint64_t>(&buffers[i * 64]), 24, 1, 1));
      for (uint32_t i = 0; i < 5; ++i) CHECK(tx.poll(&c));
      CHECK(!tx.poll(&c));
    }
    // receive: buffers posted, frames delivered with their length and EOF
    std::vector<uint8_t> in(4 * 256, 0);
    for (uint32_t i = 0; i < 4; ++i) CHECK(rx.post_receive(reinterpret_cast<uint64_t>(&in[i * 256])));
    CHECK(!rx.poll(&c));
    chip.push_to_host(3, 2, 9, std::vector<uint8_t>(40, 0x5A));
    chip.push_to_host(3, 0, 5, std::vector<uint8_t>(12, 0x77));
    CHECK(rx.poll(&c) && c.length == 40 && c.sof == 2 && c.eof == 9 && c.slot == 0 && in[0] == 0x5A && in[39] == 0x5A && in[40] == 0);
    CHECK(rx.poll(&c) && c.length == 12 && c.eof == 5 && in[256] == 0x77 && !rx.poll(&c));
    // a frame bigger than the buffer is flagged
    chip.push_to_host(3, 0, 1, std::vector<uint8_t>(300, 1));
    CHECK(rx.poll(&c) && (c.flags & TbRing::kDescOverrun));
    // bad sizes
    TbRing small(chip, alloc, 4, false, 1);
    CHECK(!small.start(256, 0, 0));
    TbRing bad_frame(chip, alloc, 4, false, 4);
    CHECK(!bad_frame.start(100, 0, 0) && !bad_frame.start(5000, 0, 0) && bad_frame.start(0, 0, 0));
    tx.stop();
    CHECK(!tx.running() && !chip.tx_enabled(3) && chip.tx(3).base == 0);
  }
  CHECK(alloc.dma.empty());
}

// ---- the control channel

struct Events final : IThunderboltEvents {
  std::vector<TbPacketType> types;
  std::vector<std::vector<uint8_t>> bodies;
  void on_event(TbPacketType type, ConstByteView body) override {
    types.push_back(type);
    bodies.push_back(std::vector<uint8_t>(body.data(), body.data() + body.size()));
  }
};

static void test_control() {
  TestAllocator alloc;
  FakeNhi chip;
  chip.config_words[5] = 0x11111111u;
  chip.config_words[6] = 0x22222222u;
  {
    TbNhi nhi(chip);
    TbControl control(nhi, alloc);
    uint32_t words[3] = {};
    CHECK(control.read_config(0, 0, TbConfigSpace::Switch, 5, ArrayView<uint32_t>(words, 1), 1000, nullptr, nullptr) == TbControl::Result::NotRunning);
    CHECK(control.start() && control.running());
    CHECK(chip.tx_enabled(0) && chip.rx(0).size == 10 && chip.rx(0).frame_size == 256);
    // a read: the request on the wire is a valid frame of the right type
    CHECK(control.read_config(0, 0, TbConfigSpace::Switch, 5, ArrayView<uint32_t>(words, 2), 1000, nullptr, nullptr) == TbControl::Result::Ok);
    CHECK(words[0] == 0x11111111u && words[1] == 0x22222222u);
    CHECK(chip.sent.size() == 1 && chip.sent[0].eof == fake_tb::kRead && chip.sent[0].sof == fake_tb::kRead);
    {
      const std::vector<uint32_t> w = fake_tb::words_of(chip.sent[0].bytes);
      CHECK(w.size() == 3 && w[0] == 0 && w[1] == 0 && w[2] == (5u | (2u << 13) | (0u << 19) | (2u << 25) | (0u << 27)));
    }
    // a router far away and a port: the route string and the port are in the request
    CHECK(control.read_config(0x0000300000000201ull, 7, TbConfigSpace::Port, 1, ArrayView<uint32_t>(words, 1), 1000, nullptr, nullptr) == TbControl::Result::Ok);
    {
      const std::vector<uint32_t> w = fake_tb::words_of(chip.sent.back().bytes);
      CHECK(w.size() == 3 && w[0] == 0x3000u && w[1] == 0x201u && ((w[2] >> 19) & 0x3F) == 7 && ((w[2] >> 25) & 3) == 1);
    }
    // a lost reply is retried with the next sequence number
    chip.drop_config_replies = 2;
    const size_t before = chip.sent.size();
    CHECK(control.read_config(0, 0, TbConfigSpace::Switch, 5, ArrayView<uint32_t>(words, 1), 1000, nullptr, nullptr) == TbControl::Result::Ok);
    CHECK(chip.sent.size() == before + 3 && ((fake_tb::words_of(chip.sent.back().bytes)[2] >> 27) & 3) == 2);
    // all lost: a timeout after the retries
    chip.drop_config_replies = 100;
    CHECK(control.read_config(0, 0, TbConfigSpace::Switch, 5, ArrayView<uint32_t>(words, 1), 1000, nullptr, nullptr) == TbControl::Result::Timeout);
    chip.drop_config_replies = 0;
    // a write
    const uint32_t out[2] = {0xA5A5A5A5u, 0x5A5A5A5Au};
    CHECK(control.write_config(0, 0, TbConfigSpace::Switch, 9, ArrayView<const uint32_t>(out, 2), 1000, nullptr, nullptr) == TbControl::Result::Ok && chip.writes[9] == 0xA5A5A5A5u && chip.writes[10] == 0x5A5A5A5Au);
    // an error packet from the router
    chip.error_code = 4;                                          // no such port
    TbControl::ConfigError error;
    CHECK(control.read_config(0, 9, TbConfigSpace::Port, 0, ArrayView<uint32_t>(words, 1), 1000, &error, nullptr) == TbControl::Result::Failed && error.reported && error.code == 4 && error.port == 3);
    chip.error_code = 0;
    // arguments that cannot be sent
    CHECK(control.read_config(0, 0, TbConfigSpace::Switch, 0, ArrayView<uint32_t>(), 1000, nullptr, nullptr) == TbControl::Result::Failed);
    uint32_t many[61];
    CHECK(control.read_config(0, 0, TbConfigSpace::Switch, 0, ArrayView<uint32_t>(many, 61), 1000, nullptr, nullptr) == TbControl::Result::Failed);
    CHECK(control.read_config(0, 64, TbConfigSpace::Port, 0, ArrayView<uint32_t>(words, 1), 1000, nullptr, nullptr) == TbControl::Result::Failed);
    CHECK(control.read_config(0, 0, TbConfigSpace::Switch, 0x2000, ArrayView<uint32_t>(words, 1), 1000, nullptr, nullptr) == TbControl::Result::Failed);
    CHECK(control.read_config(1ull << 54, 0, TbConfigSpace::Switch, 0, ArrayView<uint32_t>(words, 1), 1000, nullptr, nullptr) == TbControl::Result::Failed);
    CHECK(control.read_config(0, 0, TbConfigSpace::Switch, 0, ArrayView<uint32_t>(many, 60), 1000, nullptr, nullptr) == TbControl::Result::Ok);
    // events that come while a request waits go to the sink, in order, and the request still completes
    Events events;
    chip.send_icm_event({3u | (1u << 24), 0, 0, 0, 0, 0});
    chip.push_to_host(0, 5, fake_tb::kEvent, fake_tb::wire_frame({0, 0, 1}));
    CHECK(control.read_config(0, 0, TbConfigSpace::Switch, 5, ArrayView<uint32_t>(words, 1), 1000, nullptr, &events) == TbControl::Result::Ok && words[0] == 0x11111111u);
    CHECK(events.types.size() == 2 && events.types[0] == TbPacketType::IcmEvent && events.types[1] == TbPacketType::Event);
    // late and unrelated packets are dropped (a reply nobody waits for, a bad CRC, a runt)
    chip.push_to_host(0, 0, fake_tb::kRead, fake_tb::wire_frame({0x80000000u, 0, 5u | (1u << 13) | (2u << 25), 1}));
    std::vector<uint8_t> bad = fake_tb::wire_frame({0x80000000u, 0, 1, 1});
    bad[2] ^= 1;
    chip.push_to_host(0, 0, fake_tb::kRead, bad);
    chip.push_to_host(0, 0, fake_tb::kRead, std::vector<uint8_t>(3, 0));
    chip.push_to_host(0, 0, 15, fake_tb::wire_frame({1, 2}));
    chip.push_to_host(0, 0, 0, fake_tb::wire_frame({1, 2}));
    Events quiet;
    control.poll(&quiet);
    CHECK(quiet.types.empty());
    // the receive buffers were all given back: many packets later the channel still works
    for (int i = 0; i < 40; ++i) CHECK(control.read_config(0, 0, TbConfigSpace::Switch, 5, ArrayView<uint32_t>(words, 1), 1000, nullptr, nullptr) == TbControl::Result::Ok);
    // asynchronous errors are events
    chip.push_to_host(0, 0, fake_tb::kError, fake_tb::wire_frame({0x200u << 22, 0, 1u | (2u << 8)}));
    Events async;
    control.poll(&async);
    CHECK(async.types.size() == 1 && async.types[0] == TbPacketType::Error);
    control.stop();
    CHECK(!control.running() && !control.send(TbPacketType::Read, ConstByteView()));
  }
  CHECK(alloc.dma.empty());
}

// ---- the ICM messages

static void test_icm_messages() {
  uint8_t out[64];
  CHECK(tb_icm::build_driver_ready(ByteView(out, sizeof out)) == 4 && out[0] == 3 && out[1] == 0 && out[2] == 0 && out[3] == 0);
  CHECK(tb_icm::build_driver_ready(ByteView(out, 3)) == 0);
  // the response of an Alpine Ridge: ROM / RAM versions, security level 2 and a preboot ACL of 16 entries
  const uint16_t info = 2 | (16u << 7) | (1u << 13);
  const uint8_t response[8] = {3, 0x40, 0, 1, 5, 7, static_cast<uint8_t>(info), static_cast<uint8_t>(info >> 8)};
  TbDriverReady ready;
  CHECK(tb_icm::parse_driver_ready(ConstByteView(response, 8), &ready) && ready.rom_version == 5 && ready.ram_version == 7 && ready.security == TbSecurityLevel::Secure && ready.boot_acl_entries == 16 && ready.rtd3);
  for (size_t len = 0; len < 8; ++len) CHECK(!tb_icm::parse_driver_ready(ConstByteView(response, len), &ready));
  uint8_t wrong[8];
  memcpy(wrong, response, 8);
  wrong[0] = 4;
  CHECK(!tb_icm::parse_driver_ready(ConstByteView(wrong, 8), &ready));
  memcpy(wrong, response, 8);
  wrong[6] = 0x0F;                       // security level 15
  wrong[7] = 0;
  CHECK(tb_icm::parse_driver_ready(ConstByteView(wrong, 8), &ready) && ready.security == TbSecurityLevel::Unknown && ready.boot_acl_entries == 0);

  TbDevice d;
  for (int i = 0; i < 16; ++i) d.uuid[i] = static_cast<uint8_t>(0xA0 + i);
  d.connection_key = 0x11;
  d.connection_id = 0x22;
  CHECK(tb_icm::build_approve_device(d, ByteView(out, sizeof out)) == 24 && out[0] == 4 && out[4] == 0xA0 && out[19] == 0xAF && out[20] == 0x11 && out[21] == 0x22 && out[22] == 0 && out[23] == 0);
  CHECK(tb_icm::build_approve_device(d, ByteView(out, 23)) == 0);
  CHECK(tb_icm::build_get_route(2, 3, ByteView(out, sizeof out)) == 8 && out[0] == 0xA && out[6] == 0x32 && out[7] == 0);
  const uint8_t route_reply[16] = {0xA, 0, 0, 1, 0, 0, 0, 0, 0x01, 0x00, 0x00, 0x00, 0x05, 0x04, 0x03, 0x02};
  uint64_t route = 0;
  CHECK(tb_icm::parse_get_route(ConstByteView(route_reply, 16), &route) && route == ((1ull << 32) | 0x02030405u));
  for (size_t len = 0; len < 16; ++len) CHECK(!tb_icm::parse_get_route(ConstByteView(route_reply, len), &route));
  CHECK(tb_icm::response_flags(ConstByteView(route_reply, 16)) == 0 && tb_icm::response_flags(ConstByteView()) == 0);

  // a device connected event: link 2, depth 1, approved, dual lane, security level user (1)
  std::vector<uint8_t> ev(244, 0);
  ev[0] = 3;
  ev[1] = tb_icm::kFlagDualLane | (1u << 3);
  for (int i = 0; i < 16; ++i) ev[4 + i] = static_cast<uint8_t>(0x30 + i);
  ev[20] = 9;
  ev[21] = 8;
  const uint16_t link_info = 2 | (1 << 4) | (1 << 8);
  ev[22] = static_cast<uint8_t>(link_info);
  ev[23] = static_cast<uint8_t>(link_info >> 8);
  bool rejected = true;
  TbDevice parsed;
  CHECK(tb_icm::parse_device_connected(ConstByteView(ev.data(), ev.size()), &parsed, &rejected) && !rejected && parsed.link == 2 && parsed.depth == 1 && parsed.authorized && parsed.dual_lane && !parsed.gen3);
  CHECK(parsed.connection_key == 9 && parsed.connection_id == 8 && parsed.uuid[0] == 0x30 && parsed.uuid[15] == 0x3F && parsed.security == TbSecurityLevel::User);
  // cut at every length (the name field at the end is optional) and every bit flipped: never anything but a clean true / false
  for (size_t len = 0; len < ev.size(); ++len) {
    const bool ok = tb_icm::parse_device_connected(ConstByteView(ev.data(), len), &parsed, &rejected);
    CHECK(ok == (len >= 24));
  }
  for (size_t bit = 0; bit < 24 * 8; ++bit) {
    std::vector<uint8_t> f = ev;
    f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    tb_icm::parse_device_connected(ConstByteView(f.data(), f.size()), &parsed, &rejected);
  }
  const uint8_t gone[8] = {4, 0, 0, 1, 0, 0, static_cast<uint8_t>(3 | (2 << 4)), 0};
  uint8_t link = 0, depth = 0;
  CHECK(tb_icm::parse_device_disconnected(ConstByteView(gone, 8), &link, &depth) && link == 3 && depth == 2);
  for (size_t len = 0; len < 8; ++len) CHECK(!tb_icm::parse_device_disconnected(ConstByteView(gone, len), &link, &depth));
}

// ---- start-up

static fake_tb::Device make_device(uint8_t seed, uint8_t link, uint8_t depth) {
  fake_tb::Device d;
  for (int i = 0; i < 16; ++i) d.uuid[i] = static_cast<uint8_t>(seed + i);
  d.key = seed;
  d.id = static_cast<uint8_t>(seed + 1);
  d.link = link;
  d.depth = depth;
  return d;
}

static void test_icm_start() {
  TestAllocator alloc;
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started && icm.running());
    CHECK(icm.security_level() == TbSecurityLevel::User && icm.driver_ready().rom_version == 5 && icm.driver_ready().ram_version == 7);
    CHECK(chip.mailbox_commands.size() == 1 && chip.mailbox_commands[0] == 0x23);              // ALLOW_ALL_DEVICES
    CHECK(chip.interrupt_mask_writes != 0);
    icm.stop();
    CHECK(!icm.running() && chip.mailbox_commands.size() == 2 && chip.mailbox_commands[1] == 0x07 && !chip.tx_enabled(0));
  }
  CHECK(alloc.dma.empty());
  struct Case { const char* name; ThunderboltIcm::Outcome expect; void (*setup)(FakeNhi&); };
  const Case cases[] = {
      {"absent", ThunderboltIcm::Outcome::NotPresent, [](FakeNhi& c) { c.present = false; }},
      {"no firmware", ThunderboltIcm::Outcome::FirmwareNotRunning, [](FakeNhi& c) { c.icm_enabled = false; }},
      {"not authenticated", ThunderboltIcm::Outcome::AuthTimeout, [](FakeNhi& c) { c.auth_done = false; }},
      {"safe mode", ThunderboltIcm::Outcome::SafeMode, [](FakeNhi& c) { c.mode = 0; }},
      {"wrong mode", ThunderboltIcm::Outcome::WrongMode, [](FakeNhi& c) { c.mode = 1; }},
      {"no driver ready answer", ThunderboltIcm::Outcome::DriverReadyFailed, [](FakeNhi& c) { c.answer_driver_ready = false; }},
      {"root unreadable", ThunderboltIcm::Outcome::RootSwitchFailed, [](FakeNhi& c) { c.unreadable_root_reads = 1000; }},
  };
  for (const Case& c : cases) {
    FakeNhi chip;
    c.setup(chip);
    ThunderboltIcm icm(chip, alloc);
    const ThunderboltIcm::Outcome got = icm.start(200000);
    if (got != c.expect) printf("  case %s: got %s\n", c.name, ThunderboltIcm::outcome_name(got));
    CHECK(got == c.expect && !icm.running());
    icm.stop();
    CHECK(alloc.dma.empty());
  }
  // a root router that is slow to come up is waited for
  {
    FakeNhi chip;
    chip.unreadable_root_reads = 3;
    ThunderboltIcm icm(chip, alloc);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
  }
  // a mailbox that fails does not stop the start (the firmware just keeps its own policy)
  {
    FakeNhi chip;
    chip.mailbox_error = true;
    ThunderboltIcm icm(chip, alloc);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
  }
  CHECK(alloc.dma.empty());
}

static void test_device_events() {
  TestAllocator alloc;
  FakeNhi chip;
  ThunderboltIcm icm(chip, alloc);
  CHECK(icm.start() == ThunderboltIcm::Outcome::Started && icm.device_count() == 0);
  const fake_tb::Device a = make_device(0x10, 1, 1), b = make_device(0x40, 1, 2), c = make_device(0x70, 3, 1);
  chip.send_device_connected(a);
  chip.send_device_connected(b, true, false, tb_icm::kFlagSpeedGen3);
  chip.send_device_connected(c);
  icm.poll();
  CHECK(icm.device_count() == 3 && icm.device(0).link == 1 && icm.device(1).depth == 2 && icm.device(1).authorized && icm.device(1).gen3 && !icm.device(0).authorized && icm.device(2).link == 3);
  CHECK(icm.device(0).uuid[0] == 0x10 && icm.device(0).connection_id == 0x11);
  // the same device again replaces its entry
  chip.send_device_connected(a, true);
  icm.poll();
  CHECK(icm.device_count() == 3);
  bool found = false;
  for (uint64_t i = 0; i < icm.device_count(); ++i) if (icm.device(i).uuid[0] == 0x10) found = icm.device(i).authorized;
  CHECK(found);
  // another device on the same place replaces the old one
  const fake_tb::Device a2 = make_device(0x90, 1, 1);
  chip.send_device_connected(a2);
  icm.poll();
  CHECK(icm.device_count() == 3);
  // a device the firmware rejected is not listed
  chip.send_device_connected(make_device(0xB0, 2, 1), false, true);
  icm.poll();
  CHECK(icm.device_count() == 3);
  // the other link of a dual link port is the same physical port: disconnecting link 2 depth 1 removes ports 1 and 2's devices at depth 1 and beyond
  chip.send_device_disconnected(2, 1);
  icm.poll();
  CHECK(icm.device_count() == 1 && icm.device(0).link == 3);
  // a disconnect of something unknown, and one that is cut short, change nothing
  chip.send_device_disconnected(1, 5);
  chip.send_icm_event({4u | (1u << 24)});
  icm.poll();
  CHECK(icm.device_count() == 1);
  // link 4 is the second lane of the physical port that link 3 is on
  chip.send_device_disconnected(4, 1);
  icm.poll();
  CHECK(icm.device_count() == 0);
  chip.send_device_connected(c);
  icm.poll();
  CHECK(icm.device_count() == 1);
  // garbage events: no change, no crash
  for (uint32_t code = 0; code < 32; ++code) chip.send_icm_event({code | (1u << 24), 0x01010101u, 0x02020202u, 0x03030303u, 0x04040404u, 0x05050505u, 0xFFFFFFFFu});
  icm.poll();
  // (events 3 with zeros are devices at link 0: harmless, but at most kMaxDevices are kept)
  for (uint32_t i = 0; i < 60; ++i) chip.send_device_connected(make_device(static_cast<uint8_t>(i), static_cast<uint8_t>(1 + i % 4), static_cast<uint8_t>(1 + i % 7)));
  icm.poll();
  CHECK(icm.device_count() <= ThunderboltIcm::kMaxDevices);
  icm.stop();
  CHECK(icm.device_count() == 0);
}

static void test_approve() {
  TestAllocator alloc;
  const char* error = nullptr;
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    CHECK(!icm.approve(0, &error) && error);                       // not started
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
    const fake_tb::Device d = make_device(0x20, 1, 1);
    chip.devices.push_back(d);
    chip.send_device_connected(d);
    icm.poll();
    CHECK(icm.device_count() == 1 && !icm.device(0).authorized);
    CHECK(!icm.approve(5, &error) && error);
    CHECK(icm.approve(0, &error) && icm.device(0).authorized && chip.devices[0].approved && chip.uuid_approved.size() == 1);
    uint32_t expect_word;
    memcpy(&expect_word, d.uuid, 4);
    CHECK(chip.uuid_approved[0] == expect_word);
    uint64_t route = 0;
    CHECK(icm.route(0, &route) && route == 1 && !icm.route(7, &route));
    // the firmware refusing: the tunnel could not be created
    chip.approve_error = true;
    const fake_tb::Device e = make_device(0x50, 3, 1);
    chip.devices.push_back(e);
    chip.send_device_connected(e);
    icm.poll();
    CHECK(!icm.approve(1, &error) && error && !icm.device(1).authorized);
  }
  // levels where one approval does not open a PCIe tunnel are refused here, before anything is sent
  const uint32_t levels[] = {2, 3, 4, 5};
  for (uint32_t level : levels) {
    FakeNhi chip;
    chip.security = level;
    ThunderboltIcm icm(chip, alloc);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
    chip.send_device_connected(make_device(0x20, 1, 1));
    icm.poll();
    error = nullptr;
    CHECK(!icm.approve(0, &error) && error && chip.uuid_approved.empty());
  }
  {
    FakeNhi chip;
    chip.security = 0;                                                // none: the firmware approves by itself, the call still works
    ThunderboltIcm icm(chip, alloc);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started && icm.security_level() == TbSecurityLevel::None);
    chip.devices.push_back(make_device(0x20, 1, 1));
    chip.send_device_connected(chip.devices[0]);
    icm.poll();
    CHECK(icm.approve(0, nullptr));
    // no answer: a failure after the retries
    chip.send_device_connected(make_device(0x60, 3, 1));
    icm.poll();
    chip.answer_driver_ready = false;                                 // (the model answers APPROVE regardless; take the channel down instead)
    icm.control().stop();
    CHECK(!icm.approve(1, &error));
  }
  CHECK(alloc.dma.empty());
}

// ---- fuzz: whatever arrives on the control channel, nothing breaks

static void test_garbage() {
  TestAllocator alloc;
  FakeNhi chip;
  ThunderboltIcm icm(chip, alloc);
  CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
  uint32_t seed = 12345;
  auto next = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  for (int i = 0; i < 400; ++i) {
    std::vector<uint8_t> frame(4 + (next() % 63) * 4);
    for (uint8_t& b : frame) b = static_cast<uint8_t>(next());
    if (next() % 3 == 0) frame = fake_tb::wire_frame(std::vector<uint32_t>(1 + next() % 40, next()));
    chip.push_to_host(0, static_cast<uint8_t>(next() & 0xF), static_cast<uint8_t>(next() % 16), frame);
    if (i % 8 == 0) icm.poll();
  }
  icm.poll();
  CHECK(icm.device_count() <= ThunderboltIcm::kMaxDevices);
  // the channel still works afterwards
  uint32_t w[1];
  CHECK(icm.control().read_config(0, 0, TbConfigSpace::Switch, 0, ArrayView<uint32_t>(w, 1), 1000, nullptr, &icm) == TbControl::Result::Ok);
}


// ---- the PCIe-to-CIO registers, starting the firmware, bouncing the links

constexpr uint32_t kLinkUp = 2u << 26;

static void test_cio() {
  FakeNhi chip;
  fake_tb::FakeCio config(chip);
  TbPcie2Cio cio(config, 0x100);
  uint32_t v = 0;
  config.cs1[3] = kLinkUp | 0x1234;
  CHECK(cio.read(TbConfigSpace::Port, 3, 0x37, &v) && v == (kLinkUp | 0x1234));
  CHECK(config.commands.size() == 1 && !config.commands[0].write && config.commands[0].space == 1 && config.commands[0].port == 3 && config.commands[0].index == 0x37);
  CHECK(cio.write(TbConfigSpace::Port, 3, 0x37, 0xAABBCCDDu) && config.cs1[3] == 0xAABBCCDDu && config.commands[1].write && config.commands[1].value == 0xAABBCCDDu);
  // arguments that do not fit the command word, no capability, a command that times out, one that never finishes
  CHECK(!cio.read(TbConfigSpace::Port, 64, 0x37, &v) && !cio.read(TbConfigSpace::Port, 1, 0x2000, &v) && !cio.write(TbConfigSpace::Port, 99, 0, 0));
  TbPcie2Cio none(config, 0);
  CHECK(!none.read(TbConfigSpace::Port, 1, 0x37, &v));
  config.fail_commands = true;
  CHECK(!cio.read(TbConfigSpace::Port, 1, 0x37, &v) && !cio.write(TbConfigSpace::Port, 1, 0x37, 0));
  config.fail_commands = false;
  config.hang = true;
  CHECK(!cio.read(TbConfigSpace::Port, 1, 0x37, &v, 5000));

  // the capability list: the vendor capability is found along a chain, a missing one, a loop and a dead bus end the walk
  struct Chain final : ITbConfigSpace {
    uint32_t words[0x400 / 4] = {};
    uint32_t read32(uint16_t offset) override { return offset < 0x1000 && offset / 4 < 0x400 / 4 + 0x100 / 4 ? words[(offset - 0x100) / 4] : 0; }
    void write32(uint16_t, uint32_t) override {}
  } chain;
  chain.words[0] = 0x0001 | (0x140u << 20);                     // some other capability, next at 0x140
  chain.words[0x40 / 4] = 0x000B | (1u << 16);
  CHECK(tb_find_vendor_capability(chain) == 0x140);
  chain.words[0x40 / 4] = 0x0002 | (0x100u << 20);              // loops back to the first
  CHECK(tb_find_vendor_capability(chain) == 0);
  chain.words[0] = 0;
  CHECK(tb_find_vendor_capability(chain) == 0);
  chain.words[0] = 0xFFFFFFFFu;
  CHECK(tb_find_vendor_capability(chain) == 0);
  CHECK(tb_find_vendor_capability(config) == 0x100);
  config.capability = 0;
  CHECK(tb_find_vendor_capability(config) == 0);
}

static void test_firmware_start() {
  TestAllocator alloc;
  // the BIOS did not start the firmware: without permission, or without the registers, it stays off
  {
    FakeNhi chip;
    chip.icm_enabled = false;
    chip.auth_done = false;
    fake_tb::FakeCio config(chip);
    TbPcie2Cio cio(config, 0x100);
    ThunderboltIcm icm(chip, alloc);
    ThunderboltIcm::Options options;
    options.cio = &cio;
    icm.set_options(options);                                           // start_firmware is off
    CHECK(icm.start(100000) == ThunderboltIcm::Outcome::FirmwareNotRunning && config.commands.empty() && chip.fw_sts_writes.empty());
    options.cio = nullptr;
    options.start_firmware = true;
    icm.set_options(options);
    CHECK(icm.start(100000) == ThunderboltIcm::Outcome::FirmwareNotRunning && chip.fw_sts_writes.empty());
  }
  // with both: CIO_RESET_REQ first, then the ARC restarted, then the reset through the switch space, and the firmware authenticates
  {
    FakeNhi chip;
    chip.icm_enabled = false;
    chip.auth_done = false;
    fake_tb::FakeCio config(chip);
    TbPcie2Cio cio(config, 0x100);
    ThunderboltIcm icm(chip, alloc);
    ThunderboltIcm::Options options;
    options.cio = &cio;
    options.start_firmware = true;
    icm.set_options(options);
    CHECK(icm.start(100000) == ThunderboltIcm::Outcome::Started && chip.cio_resets == 1);
    CHECK(chip.fw_sts_writes.size() == 2 && chip.fw_sts_writes[0] == (1u << 30) && chip.fw_sts_writes[1] == ((1u << 30) | (1u << 2) | (1u << 1)));
    CHECK(config.commands.size() >= 1 && config.commands[0].write && config.commands[0].space == 2 && config.commands[0].port == 0 && config.commands[0].index == 0x50 && config.commands[0].value == (1u << 9));
    CHECK(icm.security_level() == TbSecurityLevel::User);
  }
  // the command fails, the root router does not reset, the firmware never authenticates
  struct Fail { const char* name; void (*setup)(FakeNhi&, fake_tb::FakeCio&); };
  const Fail fails[] = {
      {"cio fails", [](FakeNhi&, fake_tb::FakeCio& c) { c.fail_commands = true; }},
      {"cio hangs", [](FakeNhi&, fake_tb::FakeCio& c) { c.hang = true; }},
      {"no reset", [](FakeNhi&, fake_tb::FakeCio& c) { c.root_resets = false; }},
      {"no firmware", [](FakeNhi& n, fake_tb::FakeCio&) { n.firmware_comes_up_on_reset = false; }},
  };
  for (const Fail& f : fails) {
    FakeNhi chip;
    chip.icm_enabled = false;
    chip.auth_done = false;
    fake_tb::FakeCio config(chip);
    f.setup(chip, config);
    TbPcie2Cio cio(config, 0x100);
    ThunderboltIcm icm(chip, alloc);
    ThunderboltIcm::Options options;
    options.cio = &cio;
    options.start_firmware = true;
    icm.set_options(options);
    const ThunderboltIcm::Outcome got = icm.start(20000);
    if (got != ThunderboltIcm::Outcome::FirmwareStartFailed) printf("  case %s: got %s\n", f.name, ThunderboltIcm::outcome_name(got));
    CHECK(got == ThunderboltIcm::Outcome::FirmwareStartFailed && !icm.running());
    CHECK(alloc.dma.empty());
  }
  // a firmware that already runs is left alone
  {
    FakeNhi chip;
    fake_tb::FakeCio config(chip);
    TbPcie2Cio cio(config, 0x100);
    ThunderboltIcm icm(chip, alloc);
    ThunderboltIcm::Options options;
    options.cio = &cio;
    options.start_firmware = true;
    options.reset_ports = false;
    icm.set_options(options);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started && chip.fw_sts_writes.empty() && config.commands.empty() && chip.cio_resets == 0);
  }
}

static void test_port_reset() {
  TestAllocator alloc;
  auto run = [&](void (*setup)(fake_tb::FakeCio&), bool reset, ThunderboltIcm::Outcome expect, uint32_t failures, std::vector<fake_tb::FakeCio::Command>* log) {
    FakeNhi chip;
    fake_tb::FakeCio config(chip);
    setup(config);
    TbPcie2Cio cio(config, 0x100);
    ThunderboltIcm icm(chip, alloc);
    ThunderboltIcm::Options options;
    options.cio = &cio;
    options.reset_ports = reset;
    icm.set_options(options);
    const ThunderboltIcm::Outcome got = icm.start(50000);
    CHECK(got == expect && icm.port_reset_failures() == failures);
    if (log) *log = config.commands;
    CHECK(alloc.dma.empty() || got == ThunderboltIcm::Outcome::Started);
  };
  std::vector<fake_tb::FakeCio::Command> log;
  // physical port 0 (ports 1 and 2) has a device on both lanes, port 1 (3 and 4) has nothing
  run([](fake_tb::FakeCio& c) { c.cs1[1] = kLinkUp | 5; c.cs1[2] = kLinkUp | 6; c.cs1[3] = 0; c.cs1[4] = 0; }, true, ThunderboltIcm::Outcome::Started, 0, &log);
  // port 0: read lanes 1 and 2, disable both, read them again, enable both; port 1: read lanes 3 and 4 (not up: left alone)
  CHECK(log.size() == 10 && !log[0].write && log[0].port == 1 && !log[1].write && log[1].port == 2);
  CHECK(log.size() == 10 && log[2].write && log[2].port == 1 && log[2].value == (kLinkUp | 5 | (1u << 14)) && log[3].write && log[3].port == 2 && log[3].value == (kLinkUp | 6 | (1u << 14)));
  CHECK(log.size() == 10 && !log[4].write && !log[5].write && log[6].write && log[6].port == 1 && log[6].value == (kLinkUp | 5) && log[7].write && log[7].port == 2 && log[7].value == (kLinkUp | 6));
  CHECK(log.size() == 10 && !log[8].write && log[8].port == 3 && !log[9].write && log[9].port == 4);
  // only one lane up: no reset; and both ports with devices
  run([](fake_tb::FakeCio& c) { c.cs1[1] = kLinkUp; c.cs1[2] = 0; c.cs1[3] = 0; c.cs1[4] = 0; }, true, ThunderboltIcm::Outcome::Started, 0, &log);
  CHECK(log.size() == 4);          // both ports read, nothing written
  run([](fake_tb::FakeCio& c) { c.cs1[1] = kLinkUp; c.cs1[2] = kLinkUp; c.cs1[3] = kLinkUp; c.cs1[4] = kLinkUp; }, true, ThunderboltIcm::Outcome::Started, 0, &log);
  CHECK(log.size() == 16 && log[8].port == 3 && log[10].write && log[10].port == 3 && log[14].write && log[14].port == 3);
  // switched off
  run([](fake_tb::FakeCio& c) { c.cs1[1] = kLinkUp; c.cs1[2] = kLinkUp; }, false, ThunderboltIcm::Outcome::Started, 0, &log);
  CHECK(log.empty());
  // the registers fail: both resets are reported, the start goes on
  run([](fake_tb::FakeCio& c) { c.fail_commands = true; }, true, ThunderboltIcm::Outcome::Started, 2, nullptr);
}

static void test_upstream_port() {
  using fake_tb::FakePci;
  auto make = [](uint8_t bus, uint8_t dev, uint16_t id, uint8_t sec, uint8_t sub, int pcie_type) {
    FakePci::Function f;
    f.info.address = {bus, dev, 0};
    f.info.vendor_id = 0x8086;
    f.info.device_id = id;
    f.info.class_code = 6;
    f.info.subclass = 4;
    f.secondary = sec;
    f.subordinate = sub;
    f.pcie_type = pcie_type;
    return f;
  };
  PciDeviceInfo nhi{};
  nhi.address = {5, 0, 0};
  nhi.vendor_id = 0x8086;
  nhi.device_id = 0x15BF;
  PciAddress port{};
  // the usual: root port (not Intel's), the switch's upstream port 02:00.0 over buses 3 to 6, its downstream ports (same device id, type 6) and the NHI's bus
  {
    FakePci pci;
    FakePci::Function root = make(0, 28, 0x9D10, 1, 6, 4);
    root.info.vendor_id = 0x8086;
    pci.functions.push_back(root);
    pci.functions.push_back(make(1, 0, 0x15C0, 2, 6, 5));      // another upstream port further out: a wider range
    pci.functions.push_back(make(2, 0, 0x15C0, 3, 6, 5));
    pci.functions.push_back(make(3, 0, 0x15C0, 4, 4, 6));
    pci.functions.push_back(make(3, 2, 0x15C0, 5, 5, 6));
    CHECK(tb_find_upstream_port(pci, nhi, &port) && port.bus == 2 && port.device == 0);
  }
  // the nearest wins whatever the order of discovery
  {
    FakePci pci;
    pci.functions.push_back(make(2, 0, 0x15C0, 3, 6, 5));
    pci.functions.push_back(make(1, 0, 0x15C0, 2, 6, 5));
    CHECK(tb_find_upstream_port(pci, nhi, &port) && port.bus == 2);
  }
  // not found: only downstream ports, no PCIe capability, a bridge of another kind, another vendor, a bus range that does not hold the NHI, a header that is not a bridge
  {
    FakePci pci;
    pci.functions.push_back(make(3, 0, 0x15C0, 4, 6, 6));
    pci.functions.push_back(make(2, 0, 0x15C0, 3, 6, -1));
    pci.functions.push_back(make(2, 1, 0x1234, 3, 6, 5));
    FakePci::Function other = make(2, 2, 0x15C0, 3, 6, 5);
    other.info.vendor_id = 0x1022;
    pci.functions.push_back(other);
    pci.functions.push_back(make(2, 3, 0x15C0, 6, 9, 5));
    FakePci::Function endpoint = make(2, 4, 0x15C0, 3, 6, 5);
    endpoint.header_type = 0;
    pci.functions.push_back(endpoint);
    pci.functions.push_back(make(2, 5, 0x15C0, 0, 0, 5));
    CHECK(!tb_find_upstream_port(pci, nhi, &port));
  }
  CHECK([&] { FakePci empty; return !tb_find_upstream_port(empty, nhi, &port); }());
}


// ---- GET_TOPOLOGY: an answer in several packets

static uint32_t lane_to(uint32_t router) { return 1u | (router << 24); }

static void test_topology() {
  TestAllocator alloc;
  // host (0), a router behind port 1 (1), a router behind that one's port 2 (2), another behind port 3 of the host (3)
  auto build = [](FakeNhi& chip) {
    chip.topology.resize(4);
    chip.topology[0].route = 0;
    chip.topology[0].ports[1] = lane_to(1);
    chip.topology[0].ports[3] = lane_to(3);
    chip.topology[0].ports[2] = lane_to(0xFF);
    chip.topology[1].route = 0x01;
    chip.topology[1].ports[2] = lane_to(2);
    chip.topology[2].route = 0x0201;
    chip.topology[3].route = 0x03;
    chip.topology[3].upstream = 5;
  };
  {
    FakeNhi chip;
    build(chip);
    ThunderboltIcm icm(chip, alloc);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
    List<TbTopologySwitch> sw(alloc);
    CHECK(icm.topology(sw) && sw.count() == 4);
    CHECK(sw[1].route == 0x01 && sw[2].route == 0x0201 && sw[3].route == 0x03 && sw[3].upstream_port == 5 && sw[0].used && sw[2].switch_index == 2);
    CHECK(sw[0].behind(1) == 1 && sw[0].behind(3) == 3 && sw[0].behind(2) == 0 && sw[0].behind(0) == 0 && sw[0].behind(16) == 0);
    uint64_t route = 0;
    CHECK(ThunderboltIcm::route_in_topology(sw, 1, 1, &route) && route == 0x01);
    CHECK(ThunderboltIcm::route_in_topology(sw, 1, 2, &route) && route == 0x0201);
    CHECK(ThunderboltIcm::route_in_topology(sw, 3, 1, &route) && route == 0x03);
    CHECK(!ThunderboltIcm::route_in_topology(sw, 3, 2, &route) && !ThunderboltIcm::route_in_topology(sw, 2, 1, &route) && !ThunderboltIcm::route_in_topology(sw, 1, 0, &route) && !ThunderboltIcm::route_in_topology(sw, 1, 9, &route));
    // packets in the other order give the same result
    chip.topology_reversed = true;
    CHECK(icm.topology(sw) && sw.count() == 4 && sw[2].route == 0x0201);
    chip.topology_reversed = false;
    // a packet that never comes: a timeout, nothing half filled
    chip.topology_drop = 2;
    CHECK(!icm.topology(sw) && sw.count() == 0);
    chip.topology_drop = -1;
    // a total larger than the buffer is not taken
    chip.topology_total_override = 15;
    CHECK(!icm.topology(sw));
    chip.topology_total_override = 0;
    // a firmware that answers GET_ROUTE gives the route; one that does not leaves the topology to answer
    chip.send_device_connected(make_device(0x20, 1, 2));
    icm.poll();
    route = 0;
    CHECK(icm.route(0, &route) && route == 1);
    chip.answer_get_route = false;
    CHECK(icm.route(0, &route) && route == 0x0201);
    chip.send_device_connected(make_device(0x50, 3, 2));
    icm.poll();
    CHECK(!icm.route(1, &route));
    chip.answer_topology = false;
    CHECK(!icm.route(0, &route));
  }
  // a topology of one router, and an empty one
  {
    FakeNhi chip;
    chip.topology.resize(1);
    ThunderboltIcm icm(chip, alloc);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
    List<TbTopologySwitch> sw(alloc);
    CHECK(icm.topology(sw) && sw.count() == 1);
    chip.topology.clear();
    CHECK(!icm.topology(sw));
  }
  // the parser: cut and flipped packets
  uint8_t packet[152] = {};
  packet[0] = 1;
  packet[12] = 1 | (3 << 1);
  packet[15] = 2;
  TbTopologySwitch t;
  CHECK(tb_icm::parse_topology_packet(ConstByteView(packet, 152), &t) && t.used && t.upstream_port == 3 && t.switch_index == 2);
  for (size_t len = 0; len < 152; ++len) CHECK(!tb_icm::parse_topology_packet(ConstByteView(packet, len), &t));
  packet[0] = 2;
  CHECK(!tb_icm::parse_topology_packet(ConstByteView(packet, 152), &t));
  uint8_t out[8];
  CHECK(tb_icm::build_get_topology(ByteView(out, 8)) == 4 && out[0] == 1 && tb_icm::build_get_topology(ByteView(out, 3)) == 0);
}


// ---- secure connect

struct MemoryKeys final : ITbKeyStore {
  std::map<uint32_t, std::vector<uint8_t>> keys;
  bool fail_store = false;
  static uint32_t id(const uint8_t uuid[16]) { uint32_t v; memcpy(&v, uuid, 4); return v; }
  bool load(const uint8_t uuid[16], uint8_t key[kTbKeyBytes]) override {
    auto it = keys.find(id(uuid));
    if (it == keys.end()) return false;
    memcpy(key, it->second.data(), kTbKeyBytes);
    return true;
  }
  bool store(const uint8_t uuid[16], const uint8_t key[kTbKeyBytes]) override {
    if (fail_store) return false;
    keys[id(uuid)] = std::vector<uint8_t>(key, key + kTbKeyBytes);
    return true;
  }
  void remove(const uint8_t uuid[16]) override { keys.erase(id(uuid)); }
};

struct CountingRandom final : crypto::IRandom {
  uint8_t next = 1;
  bool fail = false;
  bool fill(ByteView out) override {
    if (fail) return false;
    for (uint8_t& b : out) b = next++;
    return true;
  }
};

static void test_secure() {
  TestAllocator alloc;
  // the HMAC the challenge relies on, against RFC 4231's first test case
  {
    uint8_t key[20], mac[32];
    memset(key, 0x0B, sizeof key);
    crypto::hmac(crypto::HashKind::Sha256, ConstByteView(key, 20), ConstByteView(reinterpret_cast<const uint8_t*>("Hi There"), 8), mac);
    const uint8_t expect[32] = {0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7};
    CHECK(memcmp(mac, expect, 32) == 0);
  }
  // the messages
  TbDevice d;
  for (int i = 0; i < 16; ++i) d.uuid[i] = static_cast<uint8_t>(0xC0 + i);
  d.connection_key = 7;
  d.connection_id = 9;
  uint8_t key[32], out[80];
  for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
  CHECK(tb_icm::build_add_device_key(d, ConstByteView(key, 32), ByteView(out, sizeof out)) == 56 && out[0] == 6 && out[4] == 0xC0 && out[20] == 7 && out[21] == 9 && out[24] == 0 && out[55] == 31);
  CHECK(tb_icm::build_challenge_device(d, ConstByteView(key, 32), ByteView(out, sizeof out)) == 56 && out[0] == 5 && out[24] == 0 && out[55] == 31);
  CHECK(tb_icm::build_add_device_key(d, ConstByteView(key, 31), ByteView(out, sizeof out)) == 0 && tb_icm::build_challenge_device(d, ConstByteView(key, 32), ByteView(out, 55)) == 0);
  uint8_t reply[88] = {};
  reply[56] = 0xEE;
  reply[87] = 0xFF;
  uint8_t got[32];
  CHECK(tb_icm::parse_challenge_response(ConstByteView(reply, 88), ByteView(got, 32)) && got[0] == 0xEE && got[31] == 0xFF);
  for (size_t len = 0; len < 88; ++len) CHECK(!tb_icm::parse_challenge_response(ConstByteView(reply, len), ByteView(got, 32)));
  CHECK(!tb_icm::parse_challenge_response(ConstByteView(reply, 88), ByteView(got, 31)));

  auto start = [&](FakeNhi& chip, ThunderboltIcm& icm, MemoryKeys& keys, CountingRandom& random) {
    chip.security = 2;
    icm.set_secure(&random, &keys);
    return icm.start() == ThunderboltIcm::Outcome::Started;
  };
  const char* error = nullptr;
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    MemoryKeys keys;
    CountingRandom random;
    CHECK(start(chip, icm, keys, random) && icm.security_level() == TbSecurityLevel::Secure);
    chip.send_device_connected(make_device(0x20, 1, 1));
    icm.poll();
    // the plain approval does not work here
    CHECK(!icm.approve(0, &error) && error && strstr(error, "key"));
    // the first time: a key goes to the device and to the store, then the approval
    chip.requests_seen.clear();
    CHECK(icm.approve_secure(0, &error) && icm.device(0).authorized);
    CHECK(chip.requests_seen.size() == 2 && chip.requests_seen[0] == 6 && chip.requests_seen[1] == 4);
    CHECK(keys.keys.size() == 1 && chip.device_keys.size() == 1 && keys.keys.begin()->second == chip.device_keys.begin()->second);
    // the same device later (a new connection): a challenge it can answer
    chip.send_device_disconnected(1, 1);
    icm.poll();
    CHECK(icm.device_count() == 0);
    chip.send_device_connected(make_device(0x20, 1, 1));
    icm.poll();
    chip.requests_seen.clear();
    CHECK(icm.approve_secure(0, &error) && icm.device(0).authorized && chip.requests_seen.size() == 2 && chip.requests_seen[0] == 5 && chip.requests_seen[1] == 4);
    CHECK(chip.challenges.size() == 1 && chip.challenges[0].size() == 32);
    // each challenge is fresh
    chip.send_device_disconnected(1, 1);
    chip.send_device_connected(make_device(0x20, 1, 1));
    icm.poll();
    CHECK(icm.approve_secure(0, &error) && chip.challenges.size() == 2 && chip.challenges[0] != chip.challenges[1]);
    // a device that answers wrongly is not approved and no approval is sent
    chip.send_device_disconnected(1, 1);
    chip.send_device_connected(make_device(0x20, 1, 1));
    icm.poll();
    chip.challenge_wrong = true;
    chip.requests_seen.clear();
    CHECK(!icm.approve_secure(0, &error) && error && strstr(error, "key") && !icm.device(0).authorized && chip.requests_seen.size() == 1);
    chip.challenge_wrong = false;
    chip.challenge_error = true;
    CHECK(!icm.approve_secure(0, &error) && !icm.device(0).authorized);
    chip.challenge_error = false;
    // the device lost its key (a reset): it says so, and nothing is approved
    chip.device_keys.clear();
    chip.requests_seen.clear();
    CHECK(!icm.approve_secure(0, &error) && error && strstr(error, "no key") && chip.requests_seen.size() == 1);
    // once its stored key is removed it can be given a new one
    keys.remove(icm.device(0).uuid);
    CHECK(icm.approve_secure(0, &error) && icm.device(0).authorized && chip.device_keys.size() == 1);
    // a device on the same port further out only after the one it hangs from
    chip.send_device_connected(make_device(0x50, 1, 2));
    icm.poll();
    CHECK(icm.device_count() == 2);
    uint64_t second = 0;
    for (uint64_t i = 0; i < icm.device_count(); ++i) if (icm.device(i).depth == 2) second = i;
    chip.send_device_disconnected(1, 1);                 // the first goes away (and takes the second with it); plug both back, the far one is offered first
    icm.poll();
    CHECK(icm.device_count() == 0);
    chip.send_device_connected(make_device(0x20, 1, 1));
    chip.send_device_connected(make_device(0x50, 1, 2));
    icm.poll();
    CHECK(icm.device_count() == 2);
    for (uint64_t i = 0; i < icm.device_count(); ++i) if (icm.device(i).depth == 2) second = i;
    CHECK(!icm.approve_secure(second, &error) && error && strstr(error, "hangs"));
  }
  // failures on the way: no store or random source, a random source that fails, a device that cannot keep the key, a store that cannot, other levels
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    MemoryKeys keys;
    CountingRandom random;
    chip.security = 2;
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
    chip.send_device_connected(make_device(0x20, 1, 1));
    icm.poll();
    CHECK(!icm.approve_secure(0, &error) && strstr(error, "random"));
    icm.set_secure(&random, &keys);
    random.fail = true;
    CHECK(!icm.approve_secure(0, &error) && strstr(error, "random") && keys.keys.empty() && chip.device_keys.empty());
    random.fail = false;
    chip.add_key_error = true;
    CHECK(!icm.approve_secure(0, &error) && keys.keys.empty() && !icm.device(0).authorized);
    chip.add_key_error = false;
    keys.fail_store = true;
    chip.requests_seen.clear();
    CHECK(!icm.approve_secure(0, &error) && strstr(error, "saved") && chip.requests_seen.size() == 1);        // never approved without the key kept
    keys.fail_store = false;
    CHECK(!icm.approve_secure(9, &error) && strstr(error, "no such"));
  }
  for (uint32_t level : {0u, 1u, 3u}) {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    MemoryKeys keys;
    CountingRandom random;
    chip.security = level;
    icm.set_secure(&random, &keys);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
    chip.send_device_connected(make_device(0x20, 1, 1));
    icm.poll();
    CHECK(!icm.approve_secure(0, &error) && error && chip.uuid_approved.empty() && keys.keys.empty());
  }
  // the parent rule applies to plain approvals as well
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
    chip.send_device_connected(make_device(0x50, 1, 2));
    chip.send_device_connected(make_device(0x20, 1, 1));
    icm.poll();
    uint64_t far = icm.device(0).depth == 2 ? 0 : 1, near = 1 - far;
    CHECK(!icm.approve(far, &error) && strstr(error, "hangs"));
    CHECK(icm.approve(near, nullptr) && icm.approve(far, nullptr));
  }
}

static void test_key_files() {
  TestAllocator alloc;
  filesystem::RamFileSystem files(alloc);
  FileTbKeyStore store(files, alloc);
  uint8_t uuid[16], other[16], key[32], got[32];
  for (int i = 0; i < 16; ++i) { uuid[i] = static_cast<uint8_t>(i); other[i] = static_cast<uint8_t>(0x80 + i); }
  for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(0xA0 + i);
  CHECK(!store.load(uuid, got));
  CHECK(store.store(uuid, key) && store.load(uuid, got) && memcmp(key, got, 32) == 0);
  CHECK(!store.load(other, got));
  // replaced, and each device has its own
  key[0] ^= 0xFF;
  CHECK(store.store(uuid, key) && store.load(uuid, got) && got[0] == key[0]);
  uint8_t key2[32];
  memset(key2, 0x42, 32);
  CHECK(store.store(other, key2) && store.load(other, got) && got[5] == 0x42 && store.load(uuid, got) && got[0] == key[0]);
  // the file is where the layout says, named by the UUID
  filesystem::DirectoryEntry entry;
  CHECK(files.stat(StringView("/AppStore/Thunderbolt/Keys/000102030405060708090a0b0c0d0e0f"), &entry) == filesystem::Status::Ok && entry.size == 32);
  store.remove(uuid);
  CHECK(!store.load(uuid, got) && store.load(other, got));
  store.remove(uuid);                                           // (again: nothing to remove)
  // a file of the wrong size is not a key
  {
    UniquePointer<filesystem::IFile> f;
    CHECK(files.open_file(StringView("/AppStore/Thunderbolt/Keys/000102030405060708090a0b0c0d0e0f"), filesystem::OpenMode::CreateTruncate, &f) == filesystem::Status::Ok);
    uint64_t n = 0;
    uint8_t few[7] = {};
    f->write(0, ConstByteView(few, 7), &n);
    CHECK(!store.load(uuid, got));
  }
}

static void test_driver() {
  TestAllocator alloc;
  {
    FakeNhi chip;
    ThunderboltDriver driver(alloc);
    CHECK(driver.outcome() == ThunderboltDriver::Outcome::NotStarted);
    CHECK(driver.start_device(chip) == ThunderboltDriver::Outcome::Started && driver.icm() && driver.icm()->running());
    chip.send_device_connected(make_device(0x20, 1, 1));
    driver.poll();
    CHECK(driver.icm()->device_count() == 1);
    driver.stop();
    CHECK(driver.outcome() == ThunderboltDriver::Outcome::NotStarted && chip.mailbox_commands.back() == 0x07);
  }
  {
    FakeNhi chip;
    chip.icm_enabled = false;
    ThunderboltDriver driver(alloc);
    CHECK(driver.start_device(chip) == ThunderboltDriver::Outcome::ControllerFailed && driver.icm() && strlen(driver.message()) > 0);
  }
  {
    // the driver hands the PCIe-to-CIO registers and its configuration to the connection manager client
    FakeNhi chip;
    chip.icm_enabled = false;
    chip.auth_done = false;
    fake_tb::FakeCio config(chip);
    TbPcie2Cio cio(config, 0x100);
    ThunderboltDriver::Config cfg;
    cfg.start_firmware = true;
    ThunderboltDriver driver(alloc, cfg);
    CHECK(driver.start_device(chip, &cio) == ThunderboltDriver::Outcome::Started && chip.cio_resets == 1);
    ThunderboltDriver plain(alloc);
    FakeNhi off;
    off.icm_enabled = false;
    CHECK(plain.start_device(off, nullptr) == ThunderboltDriver::Outcome::ControllerFailed);
  }
  {
    PciDeviceInfo info{};
    info.vendor_id = 0x8086;
    info.device_id = 0x1234;
    CHECK(!ThunderboltDriver::supports(info));
    info.device_id = 0x15BF;
    CHECK(ThunderboltDriver::supports(info));
  }
  CHECK(alloc.dma.empty());
}

int main() {
  test_devices();
  test_framing();
  test_nhi();
  test_ring();
  test_control();
  test_icm_messages();
  test_icm_start();
  test_device_events();
  test_approve();
  test_garbage();
  test_cio();
  test_firmware_start();
  test_port_reset();
  test_upstream_port();
  test_topology();
  test_secure();
  test_key_files();
  test_driver();
  printf(g_failed ? "ThunderboltTest: %d FAILED\n" : "ThunderboltTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
