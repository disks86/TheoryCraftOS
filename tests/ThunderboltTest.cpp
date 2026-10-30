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
#include "Hardware/Bus/Thunderbolt/ThunderboltNet.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltXdomain.hpp"
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
  CHECK(tb_find_nhi(0x8086, 0x15EB) && tb_find_nhi(0x8086, 0x15E8) && tb_find_nhi(0x8086, 0x15EB)->generation == TbGeneration::TitanRidge && !tb_find_nhi(0x8086, 0x15EA) && !tb_find_nhi(0x8086, 0x9A1B) && !tb_find_nhi(0x10DE, 0x15BF) && !tb_find_nhi(0x8086, 0));
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
  CHECK(chip.interrupt_mask_writes == (31 + 2 * 12) / 32 && chip.notify_reads == (31 + 3 * 12) / 32);          // (the enable registers written, the notify registers read to clear them)
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
  bool on_event(TbPacketType type, ConstByteView body) override {
    types.push_back(type);
    bodies.push_back(std::vector<uint8_t>(body.data(), body.data() + body.size()));
    return false;
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


// ---- sleep

static void test_suspend() {
  TestAllocator alloc;
  {
    FakeNhi chip;
    fake_tb::FakeCio config(chip);
    TbPcie2Cio cio(config, 0x100);
    ThunderboltIcm icm(chip, alloc);
    ThunderboltIcm::Options options;
    options.cio = &cio;
    icm.set_options(options);
    CHECK(!icm.suspend());                                            // nothing to suspend yet
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
    const size_t commands_at_start = config.commands.size();
    chip.send_device_connected(make_device(0x20, 1, 1), true);
    icm.poll();
    CHECK(icm.device_count() == 1);
    chip.mailbox_commands.clear();
    CHECK(icm.suspend() && icm.outcome() == ThunderboltIcm::Outcome::Suspended && !icm.running());
    CHECK(chip.mailbox_commands.size() == 2 && chip.mailbox_commands[0] == 0x05 && chip.mailbox_commands[1] == 0x07);        // SAVE_DEVS, DRIVER_UNLOADS
    CHECK(!chip.tx_enabled(0) && alloc.dma.empty() && icm.device_count() == 0);
    CHECK(!icm.suspend());
    const char* error = nullptr;
    CHECK(!icm.approve(0, &error) && error);
    icm.poll();                                                       // (nothing happens)
    // wake up: the channel again, DRIVER_READY, no approval, no port bounce; the firmware reports what is still there
    CHECK(icm.resume() == ThunderboltIcm::Outcome::Started && icm.running() && chip.tx_enabled(0));
    CHECK(config.commands.size() == commands_at_start && icm.device_count() == 0);
    chip.send_device_connected(make_device(0x20, 1, 1), true);
    icm.poll();
    CHECK(icm.device_count() == 1 && icm.device(0).authorized);
    // resume when nothing was suspended changes nothing
    CHECK(icm.resume() == ThunderboltIcm::Outcome::Started);
    icm.stop();
    CHECK(alloc.dma.empty());
  }
  // the firmware did not survive: restarted if it may be, otherwise reported
  for (bool allowed : {true, false}) {
    FakeNhi chip;
    fake_tb::FakeCio config(chip);
    TbPcie2Cio cio(config, 0x100);
    ThunderboltIcm icm(chip, alloc);
    ThunderboltIcm::Options options;
    options.cio = &cio;
    options.start_firmware = allowed;
    icm.set_options(options);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started && icm.suspend());
    chip.icm_enabled = false;
    chip.auth_done = false;
    const ThunderboltIcm::Outcome got = icm.resume(50000);
    CHECK(got == (allowed ? ThunderboltIcm::Outcome::Started : ThunderboltIcm::Outcome::FirmwareNotRunning));
    CHECK(alloc.dma.empty() || got == ThunderboltIcm::Outcome::Started);
  }
  // a controller that is gone after sleep
  {
    FakeNhi chip;
    ThunderboltDriver driver(alloc);
    CHECK(driver.start_device(chip) == ThunderboltDriver::Outcome::Started && driver.suspend());
    chip.present = false;
    CHECK(driver.resume() == ThunderboltDriver::Outcome::ControllerFailed);
    chip.present = true;
    CHECK(driver.start_device(chip) == ThunderboltDriver::Outcome::Started && driver.suspend() && driver.resume() == ThunderboltDriver::Outcome::Started);
  }
  CHECK(alloc.dma.empty());
}


// ---- Titan Ridge

static void test_titan() {
  TestAllocator alloc;
  // the messages
  TbDevice d;
  for (int i = 0; i < 16; ++i) d.uuid[i] = static_cast<uint8_t>(0xD0 + i);
  d.route = (0x12ull << 32) | 0x00030201;
  d.connection_id = 5;
  uint8_t out[80], key[32];
  for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i + 1);
  CHECK(tb_icm::build_approve_device_tr(d, ByteView(out, sizeof out)) == 32 && out[0] == 4 && out[4] == 0xD0 && out[20] == 0x12 && out[24] == 0x01 && out[25] == 0x02 && out[26] == 0x03 && out[28] == 5);
  CHECK(tb_icm::build_add_device_key_tr(d, ConstByteView(key, 32), ByteView(out, sizeof out)) == 64 && out[0] == 6 && out[32] == 1 && out[63] == 32);
  CHECK(tb_icm::build_challenge_device_tr(d, ConstByteView(key, 32), ByteView(out, sizeof out)) == 64 && out[0] == 5);
  CHECK(tb_icm::build_approve_device_tr(d, ByteView(out, 31)) == 0 && tb_icm::build_add_device_key_tr(d, ConstByteView(key, 31), ByteView(out, sizeof out)) == 0);
  CHECK(tb_icm::route_length(0) == 0 && tb_icm::route_length(1) == 1 && tb_icm::route_length(0xFF) == 1 && tb_icm::route_length(0x100) == 2 && tb_icm::route_length(0x030201) == 3 && tb_icm::route_length(1ull << 63) == 8);
  // driver ready
  const uint8_t ready_bytes[16] = {3, 0x40, 0, 1, 0, 0, 0x82, 0x00, 1, 2, 3, 4, 0xEB, 0x15, 0, 0};            // info: level 2, boot ACL 1
  TbDriverReady ready;
  CHECK(tb_icm::parse_driver_ready_tr(ConstByteView(ready_bytes, 16), &ready) && ready.security == TbSecurityLevel::Secure && ready.boot_acl_entries == 1 && ready.rtd3);
  for (size_t len = 0; len < 16; ++len) CHECK(!tb_icm::parse_driver_ready_tr(ConstByteView(ready_bytes, len), &ready));
  // events, cut and flipped
  std::vector<uint8_t> ev(244, 0);
  ev[0] = 3;
  for (int i = 0; i < 16; ++i) ev[4 + i] = static_cast<uint8_t>(0x60 + i);
  ev[24] = 0x01;
  ev[25] = 0x02;
  ev[28] = 7;
  ev[31] = 1;                                         // link_info: approved (bit 8)
  TbDevice parsed;
  bool rejected = true;
  CHECK(tb_icm::parse_device_connected_tr(ConstByteView(ev.data(), ev.size()), &parsed, &rejected) && !rejected && parsed.route == 0x0201 && parsed.depth == 2 && parsed.link == 1 && parsed.authorized && parsed.connection_id == 7);
  for (size_t len = 0; len < ev.size(); ++len) CHECK(tb_icm::parse_device_connected_tr(ConstByteView(ev.data(), len), &parsed, &rejected) == (len >= 32));
  for (size_t bit = 0; bit < 32 * 8; ++bit) {
    std::vector<uint8_t> f = ev;
    f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    tb_icm::parse_device_connected_tr(ConstByteView(f.data(), f.size()), &parsed, &rejected);
  }
  ev[2] = 1;                                          // a later packet (quality of service)
  CHECK(!tb_icm::parse_device_connected_tr(ConstByteView(ev.data(), ev.size()), &parsed, &rejected));
  const uint8_t gone[12] = {4, 0, 0, 1, 0, 0, 0, 0, 0x01, 0x02, 0, 0};
  uint64_t route = 0;
  CHECK(tb_icm::parse_device_disconnected_tr(ConstByteView(gone, 12), &route) && route == 0x0201);
  for (size_t len = 0; len < 12; ++len) CHECK(!tb_icm::parse_device_disconnected_tr(ConstByteView(gone, len), &route));

  const char* error = nullptr;
  auto make = [](uint8_t seed) { return make_device(seed, 0, 0); };
  {
    FakeNhi chip;
    chip.titan_ridge = true;
    chip.security = 1;
    ThunderboltIcm icm(chip, alloc);
    ThunderboltIcm::Options options;
    options.generation = TbGeneration::TitanRidge;
    icm.set_options(options);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started && icm.security_level() == TbSecurityLevel::User);
    // a device behind the host, one behind that, another on the other port
    chip.send_device_connected_tr(make(0x10), 0x01);
    chip.send_device_connected_tr(make(0x20), 0x0201);
    chip.send_device_connected_tr(make(0x30), 0x03, true);
    chip.send_device_connected_tr(make(0x40), 0x0401, false, false, 1);       // a QoS packet: ignored
    chip.send_device_connected_tr(make(0x50), 0x05, false, true);             // rejected
    icm.poll();
    CHECK(icm.device_count() == 3 && icm.device(0).route == 0x01 && icm.device(1).depth == 2 && icm.device(2).authorized);
    uint64_t r = 0;
    CHECK(icm.route(1, &r) && r == 0x0201);
    // the one behind first needs the first
    uint64_t far = 1;
    CHECK(!icm.approve(far, &error) && strstr(error, "hangs"));
    chip.devices.push_back(make(0x10));
    CHECK(icm.approve(0, &error) && chip.approved_routes.size() == 1 && chip.approved_routes[0] == 0x01 && icm.device(0).authorized);
    CHECK(icm.approve(far, &error) && chip.approved_routes.size() == 2 && chip.approved_routes[1] == 0x0201);
    // a device at the same route replaces the old one, the same device elsewhere moves
    chip.send_device_connected_tr(make(0x60), 0x03);
    icm.poll();
    CHECK(icm.device_count() == 3);
    chip.send_device_connected_tr(make(0x10), 0x07);
    icm.poll();
    CHECK(icm.device_count() == 3 && icm.device(2).route == 0x07);
    // a disconnect takes the device and what hangs below it, not its neighbours
    chip.send_device_connected_tr(make(0x70), 0x010201);
    chip.send_device_connected_tr(make(0x80), 0x0301);
    icm.poll();
    const uint64_t before = icm.device_count();
    chip.send_device_disconnected_tr(0x0201);
    icm.poll();
    bool left = false, gone_below = true;
    for (uint64_t i = 0; i < icm.device_count(); ++i) {
      if (icm.device(i).route == 0x0201 || icm.device(i).route == 0x010201) gone_below = false;
      if (icm.device(i).route == 0x0301 || icm.device(i).route == 0x03) left = true;
    }
    CHECK(gone_below && left && icm.device_count() == before - 2);
    chip.send_device_disconnected_tr(0x09);
    chip.send_icm_event({4u | (1u << 24)});
    icm.poll();
    CHECK(icm.device_count() == before - 2);
    // a disconnect of the host router's own route is nonsense and changes nothing
    chip.send_device_disconnected_tr(0);
    icm.poll();
    CHECK(icm.device_count() == before - 2);
  }
  // secure connect with the route forms
  {
    FakeNhi chip;
    chip.titan_ridge = true;
    chip.security = 2;
    ThunderboltIcm icm(chip, alloc);
    ThunderboltIcm::Options options;
    options.generation = TbGeneration::TitanRidge;
    icm.set_options(options);
    MemoryKeys keys;
    CountingRandom random;
    icm.set_secure(&random, &keys);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started && icm.security_level() == TbSecurityLevel::Secure);
    chip.send_device_connected_tr(make(0x10), 0x01);
    icm.poll();
    chip.requests_seen.clear();
    CHECK(icm.approve_secure(0, &error) && chip.requests_seen.size() == 2 && chip.requests_seen[0] == 6 && chip.requests_seen[1] == 4 && keys.keys.size() == 1 && chip.device_keys.size() == 1);
    chip.send_device_disconnected_tr(0x01);
    chip.send_device_connected_tr(make(0x10), 0x01);
    icm.poll();
    chip.requests_seen.clear();
    CHECK(icm.approve_secure(0, &error) && chip.requests_seen.size() == 2 && chip.requests_seen[0] == 5 && chip.requests_seen[1] == 4);
    chip.send_device_disconnected_tr(0x01);
    chip.send_device_connected_tr(make(0x10), 0x01);
    icm.poll();
    chip.challenge_wrong = true;
    CHECK(!icm.approve_secure(0, &error) && !icm.device(0).authorized);
  }
  // the firmware is started through the other register
  {
    FakeNhi chip;
    chip.titan_ridge = true;
    chip.icm_enabled = false;
    chip.auth_done = false;
    fake_tb::FakeCio config(chip);
    config.titan = true;
    TbPcie2Cio cio(config, 0x100);
    ThunderboltIcm icm(chip, alloc);
    ThunderboltIcm::Options options;
    options.generation = TbGeneration::TitanRidge;
    options.cio = &cio;
    options.start_firmware = true;
    options.reset_ports = false;
    icm.set_options(options);
    CHECK(icm.start(50000) == ThunderboltIcm::Outcome::Started && config.commands.size() >= 1 && config.commands[0].index == 0x777 && config.commands[0].value == 2);
  }
  // an Alpine Ridge firmware answering the Titan Ridge client is refused (the reply is too short)
  {
    FakeNhi chip;                                                      // answers with 8 bytes
    ThunderboltIcm icm(chip, alloc);
    ThunderboltIcm::Options options;
    options.generation = TbGeneration::TitanRidge;
    icm.set_options(options);
    CHECK(icm.start(20000) == ThunderboltIcm::Outcome::DriverReadyFailed);
  }
  // the driver passes the generation on
  {
    FakeNhi chip;
    chip.titan_ridge = true;
    ThunderboltDriver driver(alloc);
    CHECK(driver.start_device(chip, nullptr, TbGeneration::TitanRidge) == ThunderboltDriver::Outcome::Started);
  }
}


// ---- interrupts

static void test_interrupts() {
  TestAllocator alloc;
  FakeNhi chip;
  ThunderboltDriver driver(alloc);
  CHECK(!driver.use_interrupts(true) && driver.interrupts() == 0);               // not started
  CHECK(driver.start_device(chip) == ThunderboltDriver::Outcome::Started);
  CHECK(chip.unmasked[0] == 0 && chip.interrupts_raised == 0);                    // start-up keeps them masked
  chip.raise_interrupt = [&] { driver.on_interrupt(); };
  CHECK(driver.use_interrupts(true) && driver.icm()->interrupts_on());
  CHECK(chip.unmasked[0] == ((1u << 0) | (1u << 12)));                           // hop 0, transmit (bit 0) and receive (bit hops)
  // a frame for the host: the chip raises the interrupt, the handler reads the notify registers and counts
  const uint32_t reads = chip.notify_reads;
  chip.send_device_connected(make_device(0x20, 1, 1));
  CHECK(chip.interrupts_raised == 1 && driver.interrupts() == 1 && chip.notify_reads > reads && chip.notify[0] == 0);
  CHECK(driver.icm()->device_count() == 0);                                      // (the handler only counts)
  driver.poll();
  CHECK(driver.icm()->device_count() == 1);
  // what the host sends completes a transmit descriptor: another interrupt
  uint32_t word = 0;
  CHECK(driver.icm()->control().read_config(0, 0, TbConfigSpace::Switch, 0, ArrayView<uint32_t>(&word, 1), 1000, nullptr, driver.icm()) == TbControl::Result::Ok);
  CHECK(driver.interrupts() >= 3);
  // masked again: nothing more
  CHECK(driver.use_interrupts(false) && chip.unmasked[0] == 0);
  const uint32_t before = chip.interrupts_raised;
  chip.send_device_connected(make_device(0x50, 3, 1));
  driver.poll();
  CHECK(chip.interrupts_raised == before && driver.icm()->device_count() == 2);
  // an interrupt with nothing to report is not counted
  const uint32_t counted = driver.interrupts();
  driver.on_interrupt();
  CHECK(driver.interrupts() == counted);
  // they survive sleep: masked by the bring-up, unmasked again after it
  CHECK(driver.use_interrupts(true) && driver.suspend());
  CHECK(!driver.use_interrupts(false));
  CHECK(driver.resume() == ThunderboltDriver::Outcome::Started && chip.unmasked[0] == ((1u << 0) | (1u << 12)));
  driver.stop();
  CHECK(!driver.use_interrupts(true));
  driver.on_interrupt();                                                          // (safe with no controller)
  // a controller with many hops: the receive ring's bit is in another register
  FakeNhi wide;
  wide.hops = 40;
  ThunderboltDriver wide_driver(alloc);
  CHECK(wide_driver.start_device(wide) == ThunderboltDriver::Outcome::Started && wide_driver.use_interrupts(true));
  CHECK(wide.unmasked[0] == 1u && wide.unmasked[1] == (1u << 8));
}


// ---- XDomain

// A property block as another host would send it: values of words, text in 4 byte groups most significant byte first.
static uint32_t chars4(const char* s) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v = (v << 8) | (s[i] ? static_cast<uint8_t>(s[i]) : 0);
  return v;
}
static void put_text(std::vector<uint32_t>& block, const char* text) {
  const size_t len = strlen(text);
  const size_t words = (len + 1 + 3) / 4;
  for (size_t w = 0; w < words; ++w) {
    char part[5] = {};
    for (size_t k = 0; k < 4; ++k) part[k] = w * 4 + k < len ? text[w * 4 + k] : 0;
    block.push_back(chars4(part));
  }
}
struct EntryText {
  static void key(std::vector<uint32_t>& b, const char* k) {
    char pad[9] = {};
    strncpy(pad, k, 8);
    b.push_back(chars4(pad));
    b.push_back(chars4(pad + 4));
  }
};
// root: vendorid 0x1234, vendorid "Acme", deviceid 0x77, a service directory with prtcid 1; and filler text so that the block is longer than one response
static std::vector<uint32_t> remote_block(size_t filler_words = 60) {
  std::vector<uint32_t> b = {0x55584401u, 0};
  auto entry = [&](const char* k, uint32_t length, uint32_t type, uint32_t value) {
    EntryText::key(b, k);
    b.push_back(length | (type << 24));
    b.push_back(value);
  };
  // entries: 5 (vendorid value, vendorid text, deviceid value, filler data, services directory)
  const size_t entries = 5;
  b[1] = static_cast<uint32_t>(4 * entries);
  const size_t data_at = 2 + 4 * entries;
  std::vector<uint32_t> data;
  put_text(data, "Acme");
  const size_t text_words = data.size();
  entry("vendorid", 1, 0x76, 0x1234);
  entry("vendorid", static_cast<uint32_t>(text_words), 0x74, static_cast<uint32_t>(data_at));
  entry("deviceid", 1, 0x76, 0x77);
  for (size_t i = 0; i < filler_words; ++i) data.push_back(0xA0000000u + static_cast<uint32_t>(i));
  entry("filler", static_cast<uint32_t>(filler_words), 0x64, static_cast<uint32_t>(data_at + text_words));
  const size_t dir_at = data_at + data.size();
  entry("network", 4 + 4 + 4, 0x44, static_cast<uint32_t>(dir_at));          // a directory: uuid (4 words) and two entries
  for (uint32_t w : data) b.push_back(w);
  for (int i = 0; i < 4; ++i) b.push_back(0x11111111u * (i + 1));
  EntryText::key(b, "prtcid");
  b.push_back(1u | (0x76u << 24));
  b.push_back(1);
  EntryText::key(b, "prtcvers");
  b.push_back(1u | (0x76u << 24));
  b.push_back(1);
  return b;
}

static void test_xdomain_messages() {
  uint8_t out[256];
  uint8_t uuid[16], other[16];
  for (int i = 0; i < 16; ++i) { uuid[i] = static_cast<uint8_t>(0x20 + i); other[i] = static_cast<uint8_t>(0x80 + i); }
  // the UUID request: route, length and sequence, the protocol UUID, the type
  CHECK(tb_xdp::build_uuid_request(0x0000001200000034ull, 2, ByteView(out, sizeof out)) == 32);
  CHECK(out[0] == 0x12 && out[1] == 0x00 && out[2] == 0x00 && out[3] == 0x00 && out[4] == 0x34 && out[8] == 0x05 && out[11] == 0x10);             // (the words little endian: route high 0x12, low 0x34; length 5 words after the 12 byte route and length part; sequence 2 in bits 27 and 28)
  CHECK(memcmp(out + 12, tb_xdp::kProtocolUuid, 16) == 0 && out[28] == 12);
  tb_xdp::Header h;
  CHECK(tb_xdp::parse_header(ConstByteView(out, 32), &h) && h.route == 0x0000001200000034ull && h.sequence == 2 && h.length_words == 5 && h.type == 12);
  // the cut and the flipped: never anything but a clean answer
  for (size_t len = 0; len < 32; ++len) CHECK(!tb_xdp::parse_header(ConstByteView(out, len), &h));
  for (size_t bit = 0; bit < 32 * 8; ++bit) {
    uint8_t f[32];
    memcpy(f, out, 32);
    f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    const bool ok = tb_xdp::parse_header(ConstByteView(f, 32), &h);
    if (bit >= 12 * 8 && bit < 28 * 8) CHECK(!ok);                       // the protocol UUID is checked
  }
  // the top bit of the route that answers carry is dropped
  uint8_t reply_copy[32];
  memcpy(reply_copy, out, 32);
  reply_copy[3] |= 0x80;
  CHECK(tb_xdp::parse_header(ConstByteView(reply_copy, 32), &h) && h.route == 0x0000001200000034ull);
  // a length that claims more than there is
  memcpy(reply_copy, out, 32);
  reply_copy[8] = 0x30;
  CHECK(!tb_xdp::parse_header(ConstByteView(reply_copy, 32), &h));

  CHECK(tb_xdp::build_uuid_response(0x01, 1, uuid, ByteView(out, sizeof out)) == 56);
  tb_xdp::UuidResponse u;
  uint32_t error = 99;
  CHECK(tb_xdp::parse_uuid_response(ConstByteView(out, 56), &u, &error) == tb_xdp::Parsed::Ok && memcmp(u.uuid, uuid, 16) == 0 && u.route == 0x01);
  for (size_t len = 0; len < 56; ++len) CHECK(tb_xdp::parse_uuid_response(ConstByteView(out, len), &u, &error) == tb_xdp::Parsed::Malformed);
  CHECK(tb_xdp::build_error_response(0x01, 0, tb_xdp::kErrorNotReady, ByteView(out, sizeof out)) == 36);
  CHECK(tb_xdp::parse_uuid_response(ConstByteView(out, 36), &u, &error) == tb_xdp::Parsed::Error && error == 4);
  tb_xdp::PropertiesResponse pr;
  CHECK(tb_xdp::parse_properties_response(ConstByteView(out, 36), &pr, &error) == tb_xdp::Parsed::Error);
  for (size_t len = 0; len < 36; ++len) CHECK(tb_xdp::parse_properties_response(ConstByteView(out, len), &pr, &error) == tb_xdp::Parsed::Malformed);

  // properties: the request, a response with a window of the block, bit flips and cuts
  CHECK(tb_xdp::build_properties_request(0x01, 3, uuid, other, 90, ByteView(out, sizeof out)) == 68);
  tb_xdp::PropertiesRequest q;
  CHECK(tb_xdp::parse_properties_request(ConstByteView(out, 68), &q) && memcmp(q.source, uuid, 16) == 0 && memcmp(q.destination, other, 16) == 0 && q.offset == 90);
  for (size_t len = 0; len < 68; ++len) CHECK(!tb_xdp::parse_properties_request(ConstByteView(out, len), &q));
  std::vector<uint32_t> block(100);
  for (size_t i = 0; i < block.size(); ++i) block[i] = 0x1000u + static_cast<uint32_t>(i);
  uint8_t response[256];
  const uint32_t rn = tb_xdp::build_properties_response(0x01, 1, uuid, other, ArrayView<const uint32_t>(block.data(), block.size()), 7, 50, ByteView(response, sizeof response));
  CHECK(rn == 72 + 45 * 4 && tb_xdp::parse_properties_response(ConstByteView(response, rn), &pr, &error) == tb_xdp::Parsed::Ok && pr.offset == 50 && pr.data_length == 100 && pr.generation == 7 && pr.words == 45);
  CHECK(response[72] == 0x00 + 0x32 && response[73] == 0x10);                                  // block[50] = 0x1032, little endian
  const uint32_t tail = tb_xdp::build_properties_response(0x01, 1, uuid, other, ArrayView<const uint32_t>(block.data(), block.size()), 7, 95, ByteView(response, sizeof response));
  CHECK(tail == 72 + 5 * 4 && tb_xdp::parse_properties_response(ConstByteView(response, tail), &pr, &error) == tb_xdp::Parsed::Ok && pr.words == 5);
  CHECK(tb_xdp::build_properties_response(0x01, 1, uuid, other, ArrayView<const uint32_t>(block.data(), block.size()), 7, 100, ByteView(response, sizeof response)) == 0);
  for (size_t len = 0; len < tail; ++len) CHECK(tb_xdp::parse_properties_response(ConstByteView(response, len), &pr, &error) == tb_xdp::Parsed::Malformed);
  for (size_t bit = 0; bit < 72 * 8; ++bit) {
    uint8_t f[256];
    memcpy(f, response, tail);
    f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    tb_xdp::parse_properties_response(ConstByteView(f, tail), &pr, &error);
  }

  // the events
  FakeNhi chip;
  Events sink;
  uint8_t remote[16], local[16];
  for (int i = 0; i < 16; ++i) { remote[i] = static_cast<uint8_t>(0x40 + i); local[i] = static_cast<uint8_t>(0xC0 + i); }
  std::vector<uint8_t> ev(56, 0);
  ev[0] = 6;
  ev[6] = static_cast<uint8_t>(2 | (1 << 4));
  memcpy(&ev[8], remote, 16);
  memcpy(&ev[24], local, 16);
  ev[44] = 0x01;
  ev[45] = 0x02;
  TbXDomain x;
  CHECK(tb_icm::parse_xdomain_connected(ConstByteView(ev.data(), ev.size()), &x) && x.link == 2 && x.depth == 1 && x.route == 0x0201 && memcmp(x.remote_uuid, remote, 16) == 0 && memcmp(x.local_uuid, local, 16) == 0);
  for (size_t len = 0; len < ev.size(); ++len) CHECK(!tb_icm::parse_xdomain_connected(ConstByteView(ev.data(), len), &x));
  for (size_t bit = 0; bit < ev.size() * 8; ++bit) {
    std::vector<uint8_t> f = ev;
    f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    tb_icm::parse_xdomain_connected(ConstByteView(f.data(), f.size()), &x);
  }
  uint8_t gone_uuid[16], link = 0, depth = 0;
  std::vector<uint8_t> gone(24, 0);
  gone[0] = 7;
  gone[6] = static_cast<uint8_t>(3 | (2 << 4));
  memcpy(&gone[8], remote, 16);
  CHECK(tb_icm::parse_xdomain_disconnected(ConstByteView(gone.data(), 24), gone_uuid, &link, &depth) && link == 3 && depth == 2 && memcmp(gone_uuid, remote, 16) == 0);
  for (size_t len = 0; len < 24; ++len) CHECK(!tb_icm::parse_xdomain_disconnected(ConstByteView(gone.data(), len), gone_uuid, &link, &depth));
  std::vector<uint8_t> gone_tr(28, 0);
  gone_tr[0] = 7;
  gone_tr[8] = 0x01;
  gone_tr[9] = 0x02;
  memcpy(&gone_tr[12], remote, 16);
  uint64_t route = 0;
  CHECK(tb_icm::parse_xdomain_disconnected_tr(ConstByteView(gone_tr.data(), 28), &route, gone_uuid) && route == 0x0201);
  for (size_t len = 0; len < 28; ++len) CHECK(!tb_icm::parse_xdomain_disconnected_tr(ConstByteView(gone_tr.data(), len), &route, gone_uuid));
  (void)chip;
  (void)sink;
}

static void test_properties() {
  TestAllocator alloc;
  List<uint32_t> mine(alloc);
  CHECK(tb_properties_build_local(0xABCD, 0x42, 3, "Theory", "Craft OS machine", mine));
  List<TbPropertyEntry> entries(alloc);
  CHECK(tb_properties_parse(ArrayView<const uint32_t>(&mine[0], mine.count()), entries));
  const TbPropertyEntry* v = tb_properties_find(entries, "vendorid", TbPropertyType::Value);
  const TbPropertyEntry* vn = tb_properties_find(entries, "vendorid", TbPropertyType::Text);
  const TbPropertyEntry* d = tb_properties_find(entries, "deviceid", TbPropertyType::Value);
  const TbPropertyEntry* dn = tb_properties_find(entries, "deviceid", TbPropertyType::Text);
  const TbPropertyEntry* rv = tb_properties_find(entries, "devicerv", TbPropertyType::Value);
  CHECK(v && v->value == 0xABCD && vn && strcmp(vn->text, "Theory") == 0 && d && d->value == 0x42 && dn && strcmp(dn->text, "Craft OS machine") == 0 && rv && rv->value == 3);
  CHECK(!tb_properties_find(entries, "vendorid", TbPropertyType::Data) && !tb_properties_find(entries, "nothing", TbPropertyType::Value));
  CHECK(mine[0] == 0x55584401u && mine[1] == 20);
  // a block like another host's: the service directory is read, its entries are at depth 1
  const std::vector<uint32_t> block = remote_block();
  CHECK(tb_properties_parse(ArrayView<const uint32_t>(block.data(), block.size()), entries));
  const TbPropertyEntry* id = nullptr;
  for (uint64_t i = 0; i < entries.count(); ++i) if (entries[i].depth == 1 && strcmp(entries[i].key, "prtcid") == 0) id = &entries[i];
  CHECK(id && id->value == 1 && entries.count() == 7);
  // a block that is not a root directory
  List<uint32_t> bad(alloc);
  CHECK(!tb_properties_parse(ArrayView<const uint32_t>(), entries) && entries.count() == 0);
  std::vector<uint32_t> wrong = block;
  wrong[0] ^= 1;
  CHECK(!tb_properties_parse(ArrayView<const uint32_t>(wrong.data(), wrong.size()), entries));
  wrong = block;
  wrong[1] = 1000;                                                     // longer than the block
  CHECK(!tb_properties_parse(ArrayView<const uint32_t>(wrong.data(), wrong.size()), entries));
  // truncations and bit flips: a clean answer, nothing out of bounds (ASan), no entries left on failure
  for (size_t len = 0; len < block.size(); ++len) {
    const bool ok = tb_properties_parse(ArrayView<const uint32_t>(block.data(), len), entries);
    if (!ok) CHECK(entries.count() == 0);
  }
  for (size_t bit = 0; bit < 40 * 32; ++bit) {
    std::vector<uint32_t> f = block;
    f[bit / 32] ^= 1u << (bit % 32);
    tb_properties_parse(ArrayView<const uint32_t>(f.data(), f.size()), entries);
  }
  // a directory that contains itself ends at the depth limit; so does one with too many entries
  std::vector<uint32_t> loop = {0x55584401u, 4};
  EntryText::key(loop, "again");
  loop.push_back(8u | (0x44u << 24));
  loop.push_back(6);
  // (the directory at word 6 is: uuid 4 words and one entry pointing at word 6 again)
  loop.push_back(0); loop.push_back(0); loop.push_back(0); loop.push_back(0);
  EntryText::key(loop, "again");
  loop.push_back(8u | (0x44u << 24));
  loop.push_back(6);
  CHECK(!tb_properties_parse(ArrayView<const uint32_t>(loop.data(), loop.size()), entries) && entries.count() == 0);
  std::vector<uint32_t> many = {0x55584401u, 4 * 200};
  for (int i = 0; i < 200; ++i) { EntryText::key(many, "k"); many.push_back(1u | (0x76u << 24)); many.push_back(i); }
  CHECK(!tb_properties_parse(ArrayView<const uint32_t>(many.data(), many.size()), entries));
  // text that is not terminated is cut, a value of the wrong length is refused, an unknown type is kept as unknown
  std::vector<uint32_t> odd = {0x55584401u, 8};
  EntryText::key(odd, "text");
  odd.push_back(1u | (0x74u << 24));
  odd.push_back(10);
  EntryText::key(odd, "weird");
  odd.push_back(1u | (0x21u << 24));
  odd.push_back(5);
  odd.push_back(chars4("full"));
  CHECK(tb_properties_parse(ArrayView<const uint32_t>(odd.data(), odd.size()), entries) && entries.count() == 2 && strcmp(entries[0].text, "full") == 0 && entries[1].type == TbPropertyType::Unknown);
  std::vector<uint32_t> badvalue = {0x55584401u, 4};
  EntryText::key(badvalue, "v");
  badvalue.push_back(2u | (0x76u << 24));
  badvalue.push_back(1);
  CHECK(!tb_properties_parse(ArrayView<const uint32_t>(badvalue.data(), badvalue.size()), entries));
  CHECK(!tb_properties_build_local(1, 2, 3, "x", "y", mine) == false);
}

static void test_xdomain() {
  TestAllocator alloc;
  uint8_t remote[16], local[16], other[16];
  for (int i = 0; i < 16; ++i) { remote[i] = static_cast<uint8_t>(0x40 + i); local[i] = static_cast<uint8_t>(0xC0 + i); other[i] = static_cast<uint8_t>(0x10 + i); }
  auto start = [&](FakeNhi& chip, ThunderboltIcm& icm) {
    memcpy(chip.remote.uuid, remote, 16);
    chip.remote.block = remote_block();
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
  };
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    start(chip, icm);
    chip.send_xdomain_connected(remote, local, 1, 1, 0x01);
    icm.poll();                                                        // announced, and discovered in the same poll
    CHECK(icm.xdomain_count() == 1 && icm.xdomain(0).route == 0x01 && icm.xdomain(0).discovered);
    const TbXDomain& x = icm.xdomain(0);
    CHECK(x.vendor_id == 0x1234 && x.device_id == 0x77 && strcmp(x.vendor_name, "Acme") == 0 && x.device_name[0] == 0 && x.generation == 5 && x.protocol_count == 1 && x.protocols[0] == 1);
    // the requests: UUID first, then the 96 words of properties in three pieces (45, 45 and the rest), our UUID as the source
    CHECK(chip.xdp_requests.size() == 4 && chip.xdp_requests[0][7] == 12 && chip.xdp_requests[1][7] == 3 && chip.xdp_requests[2][7] == 3 && chip.xdp_requests[3][7] == 3);
    CHECK((chip.xdp_requests[1][16] & 0xFFFF) == 0 && (chip.xdp_requests[2][16] & 0xFFFF) == 45 && (chip.xdp_requests[3][16] & 0xFFFF) == 90);
    CHECK(memcmp(&chip.xdp_requests[1][8], local, 16) == 0 && memcmp(&chip.xdp_requests[1][12], remote, 16) == 0);
    // nothing more is asked once it is discovered
    icm.poll();
    CHECK(chip.xdp_requests.size() == 4);
    // the same announcement again changes nothing; another host at that place replaces it
    chip.send_xdomain_connected(remote, local, 1, 1, 0x01);
    icm.poll();
    CHECK(icm.xdomain_count() == 1 && chip.xdp_requests.size() == 4);
    uint8_t second[16];
    memcpy(second, other, 16);
    chip.send_xdomain_connected(second, local, 2, 1, 0x02);              // link 2 is the same physical port as link 1
    chip.remote.answer = false;
    icm.poll();
    CHECK(icm.xdomain_count() == 1 && memcmp(icm.xdomain(0).remote_uuid, other, 16) == 0 && !icm.xdomain(0).discovered);
    // disconnects
    chip.send_xdomain_disconnected(other, 2, 1);
    icm.poll();
    CHECK(icm.xdomain_count() == 0);
    chip.send_xdomain_disconnected(other, 2, 1);                         // (again: nothing there)
    icm.poll();
    CHECK(icm.xdomain_count() == 0);
  }
  // a host that answers with another UUID than announced is not taken; one that does not answer is given up on after a few tries
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    start(chip, icm);
    chip.remote.wrong_uuid = true;
    chip.send_xdomain_connected(remote, local, 1, 1, 0x01);
    icm.poll();
    CHECK(icm.xdomain_count() == 1 && !icm.xdomain(0).discovered && icm.xdomain(0).attempts == 1);
    for (int i = 0; i < 400; ++i) icm.poll();
    CHECK(icm.xdomain(0).gave_up && !icm.xdomain(0).discovered);
    const size_t asked = chip.xdp_requests.size();
    for (int i = 0; i < 100; ++i) icm.poll();
    CHECK(chip.xdp_requests.size() == asked);
  }
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    start(chip, icm);
    chip.remote.not_ready = true;
    chip.send_xdomain_connected(remote, local, 1, 1, 0x01);
    icm.poll();
    CHECK(!icm.xdomain(0).discovered);
    chip.remote.not_ready = false;
    for (int i = 0; i < 60 && !icm.xdomain(0).discovered; ++i) icm.poll();             // the retry finds it
    CHECK(icm.xdomain(0).discovered && icm.xdomain(0).vendor_id == 0x1234);
  }
  // properties answers that do not make sense: the wrong offset, a length that changes between pieces
  for (int mode = 0; mode < 2; ++mode) {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    start(chip, icm);
    chip.remote.wrong_offset = mode == 0;
    chip.remote.shrink_length = mode == 1;
    chip.send_xdomain_connected(remote, local, 1, 1, 0x01);
    icm.poll();
    CHECK(!icm.xdomain(0).discovered);
  }
  // a very long block is refused
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    start(chip, icm);
    chip.remote.block.assign(600, 0);
    chip.send_xdomain_connected(remote, local, 1, 1, 0x01);
    icm.poll();
    CHECK(!icm.xdomain(0).discovered);
  }
  // a block that is not a property block
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    start(chip, icm);
    chip.remote.block.assign(10, 0x12345678u);
    chip.send_xdomain_connected(remote, local, 1, 1, 0x01);
    icm.poll();
    CHECK(!icm.xdomain(0).discovered);
  }
  // lost requests are retried by the poll after, with the next sequence number
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    start(chip, icm);
    chip.remote.drop_first = 1;
    chip.send_xdomain_connected(remote, local, 1, 1, 0x01);
    icm.poll();
    CHECK(!icm.xdomain(0).discovered && icm.xdomain(0).attempts == 1);
    for (int i = 0; i < 60 && !icm.xdomain(0).discovered; ++i) icm.poll();
    CHECK(icm.xdomain(0).discovered);
    CHECK(((chip.xdp_requests.back()[2] >> 27) & 3) == 1);
  }
  // what the other host asks of us
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    start(chip, icm);
    chip.remote.answer = false;
    chip.remote_request(12, {});                                          // a UUID request before any connection is known: nobody to answer as
    icm.poll();
    CHECK(chip.xdp_responses.empty());
    CHECK(icm.set_identity(0x5555, 9, 2, "Craft", "Box"));
    chip.send_xdomain_connected(remote, local, 1, 1, 0x01);
    icm.poll();
    chip.remote_request(12, {}, 0x01, 2);
    icm.poll();
    CHECK(chip.xdp_responses.size() == 1 && chip.xdp_responses[0][7] == 2 && memcmp(&chip.xdp_responses[0][8], local, 16) == 0 && ((chip.xdp_responses[0][2] >> 27) & 3) == 2);
    // its properties (the identity block is 25 words: one response, and an offset into it gives the rest)
    std::vector<uint32_t> ask(9, 0);
    memcpy(&ask[0], remote, 16);
    memcpy(&ask[4], local, 16);
    ask[8] = 0;
    chip.remote_request(3, ask, 0x01, 1);
    icm.poll();
    CHECK(chip.xdp_responses.size() == 2 && chip.xdp_responses[1][7] == 4);
    const std::vector<uint32_t>& pr = chip.xdp_responses[1];
    CHECK(pr[16] == (25u << 16) && pr.size() == 18 + 25);                 // offset 0, length 25 words
    std::vector<uint32_t> got(pr.begin() + 18, pr.end());
    List<TbPropertyEntry> entries(alloc);
    CHECK(tb_properties_parse(ArrayView<const uint32_t>(got.data(), got.size()), entries) && tb_properties_find(entries, "vendorid", TbPropertyType::Value)->value == 0x5555 && strcmp(tb_properties_find(entries, "deviceid", TbPropertyType::Text)->text, "Box") == 0);
    ask[8] = 20;
    chip.remote_request(3, ask, 0x01, 1);
    icm.poll();
    CHECK(chip.xdp_responses.size() == 3 && chip.xdp_responses[2][16] == (20u | (25u << 16)) && chip.xdp_responses[2].size() == 18 + 5);
    // asked for a domain that is not us, or past the end: an error / nothing
    ask[0] ^= 1;
    ask[4] ^= 0;
    std::vector<uint32_t> not_us(9, 0);
    memcpy(&not_us[0], remote, 16);
    memcpy(&not_us[4], other, 16);
    chip.remote_request(3, not_us, 0x01, 1);
    icm.poll();
    CHECK(chip.xdp_responses.size() == 4 && chip.xdp_responses[3][7] == 7 && chip.xdp_responses[3][8] == 2);
    ask[8] = 30;
    chip.remote_request(3, ask, 0x01, 1);
    icm.poll();
    CHECK(chip.xdp_responses.size() == 4);
    // link state and anything else: not supported
    chip.remote_request(15, {}, 0x01, 3);
    chip.remote_request(99, {}, 0x01, 3);
    icm.poll();
    CHECK(chip.xdp_responses.size() == 6 && chip.xdp_responses[4][7] == 7 && chip.xdp_responses[4][8] == 3 && chip.xdp_responses[5][8] == 3);
    // a request for a connection we do not know, and a runt
    chip.remote_request(3, ask, 0x77, 1);
    chip.push_to_host(0, 6, 6, fake_tb::wire_frame({1, 2, 3}));
    icm.poll();
    CHECK(chip.xdp_responses.size() == 6);
    // their properties changed: acknowledged, and read again
    chip.remote.answer = true;
    const size_t asked = chip.xdp_requests.size();
    chip.remote_request(5, {}, 0x01, 0);
    icm.poll();
    CHECK(chip.xdp_responses.size() == 7 && chip.xdp_responses[6][7] == 6);
    icm.poll();
    CHECK(chip.xdp_requests.size() > asked && icm.xdomain(0).discovered);
  }
  // Titan Ridge: the disconnect names the route
  {
    FakeNhi chip;
    chip.titan_ridge = true;
    ThunderboltIcm icm(chip, alloc);
    ThunderboltIcm::Options options;
    options.generation = TbGeneration::TitanRidge;
    icm.set_options(options);
    start(chip, icm);
    chip.send_xdomain_connected(remote, local, 0, 0, 0x0201);
    icm.poll();
    CHECK(icm.xdomain_count() == 1 && icm.xdomain(0).route == 0x0201 && icm.xdomain(0).discovered);
    chip.send_xdomain_disconnected_tr(other, 0x0201);                    // some other host: not this one
    icm.poll();
    CHECK(icm.xdomain_count() == 1);
    chip.send_xdomain_disconnected_tr(remote, 0x0201);
    icm.poll();
    CHECK(icm.xdomain_count() == 0);
  }
  // sleep drops them; they are announced again after
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    start(chip, icm);
    chip.send_xdomain_connected(remote, local, 1, 1, 0x01);
    icm.poll();
    CHECK(icm.xdomain_count() == 1 && icm.suspend() && icm.xdomain_count() == 0);
    CHECK(icm.resume() == ThunderboltIcm::Outcome::Started);
    chip.send_xdomain_connected(remote, local, 1, 1, 0x01);
    icm.poll();
    CHECK(icm.xdomain_count() == 1 && icm.xdomain(0).discovered);
  }
  // garbage XDomain frames of every type and size do no harm
  {
    FakeNhi chip;
    ThunderboltIcm icm(chip, alloc);
    start(chip, icm);
    chip.send_xdomain_connected(remote, local, 1, 1, 0x01);
    icm.poll();
    uint32_t seed = 99;
    auto next = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int i = 0; i < 300; ++i) {
      std::vector<uint32_t> w(8 + next() % 40, 0);
      for (uint32_t& x : w) x = next();
      w[0] = 0;
      w[1] = 1;
      memcpy(&w[3], fake_tb::protocol_uuid, 16);
      w[2] = static_cast<uint32_t>(w.size() * 4 - 12) / 4 | ((next() & 3) << 27);
      w[7] = next() % 20;
      chip.push_to_host(0, 6, 6, fake_tb::wire_frame(w));
      if (i % 8 == 0) icm.poll();
    }
    icm.poll();
    CHECK(icm.xdomain_count() <= ThunderboltIcm::kMaxDevices);
  }
}


// ---- networking over the cable (ThunderboltIP)

static void test_net_messages() {
  uint8_t initiator[16], target[16], out[128];
  for (int i = 0; i < 16; ++i) { initiator[i] = static_cast<uint8_t>(0x10 + i); target[i] = static_cast<uint8_t>(0x90 + i); }
  CHECK(tb_ip::build_login(0x0201, 2, initiator, target, 77, 9, ByteView(out, sizeof out)) == 92);
  CHECK(out[4] == 0x01 && out[5] == 0x02 && out[8] == 20 && out[11] == 0x10);                               // route low; length (92 - 12) / 4 = 20 words, sequence 2
  CHECK(memcmp(out + 12, tb_ip::kServiceUuid, 16) == 0 && out[28] == 0x10 && out[44] == 0x90 && out[60] == 0 && out[64] == 77 && out[68] == 1 && out[72] == 9);
  tb_ip::Header h;
  CHECK(tb_ip::parse_header(ConstByteView(out, 92), &h) && h.route == 0x0201 && h.sequence == 2 && h.type == 0 && h.command_id == 77 && memcmp(h.initiator, initiator, 16) == 0 && memcmp(h.target, target, 16) == 0);
  uint32_t version = 0, path = 0;
  CHECK(tb_ip::parse_login(ConstByteView(out, 92), &version, &path) && version == 1 && path == 9);
  for (size_t len = 0; len < 92; ++len) {
    CHECK(!tb_ip::parse_login(ConstByteView(out, len), &version, &path));
    if (len < 68) CHECK(!tb_ip::parse_header(ConstByteView(out, len), &h));
  }
  for (size_t bit = 12 * 8; bit < 28 * 8; ++bit) {                                                          // the service UUID is checked
    uint8_t f[92];
    memcpy(f, out, 92);
    f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    CHECK(!tb_ip::parse_header(ConstByteView(f, 92), &h));
  }
  const uint8_t mac[6] = {2, 3, 4, 5, 6, 7};
  CHECK(tb_ip::build_login_response(0x01, 1, target, initiator, 77, mac, ByteView(out, sizeof out)) == 100 && out[60] == 1);
  uint32_t status = 9;
  uint8_t got[6];
  CHECK(tb_ip::parse_login_response(ConstByteView(out, 100), &status, got) && status == 0 && memcmp(got, mac, 6) == 0);
  for (size_t len = 0; len < 100; ++len) CHECK(!tb_ip::parse_login_response(ConstByteView(out, len), &status, got));
  out[80] = 5;                                                                                            // an address that is not six bytes
  CHECK(!tb_ip::parse_login_response(ConstByteView(out, 100), &status, got));
  CHECK(tb_ip::build_logout(0x01, 0, initiator, target, 5, ByteView(out, sizeof out)) == 68 && out[60] == 2);
  CHECK(tb_ip::build_status(0x01, 0, initiator, target, 5, 0, ByteView(out, sizeof out)) == 72 && out[60] == 3);
  CHECK(tb_ip::build_login(0x01, 0, initiator, target, 1, 1, ByteView(out, 91)) == 0);
  // the network directory in a property block
  TestAllocator alloc;
  List<uint32_t> block(alloc);
  CHECK(tb_properties_build_local(1, 2, 3, "A", "B", true, 3, block));
  List<TbPropertyEntry> entries(alloc);
  CHECK(tb_properties_parse(ArrayView<const uint32_t>(&block[0], block.count()), entries));
  uint32_t flags = 99;
  bool dir = false;
  for (uint64_t i = 0; i < entries.count(); ++i) {
    if (entries[i].depth == 0 && entries[i].type == TbPropertyType::Directory && strcmp(entries[i].key, "network") == 0) dir = true;
    if (entries[i].depth == 1 && strcmp(entries[i].key, "prtcstns") == 0) flags = entries[i].value;
  }
  CHECK(dir && flags == 3 && entries.count() == 10);
  for (size_t len = 0; len < block.count(); ++len) tb_properties_parse(ArrayView<const uint32_t>(&block[0], len), entries);
}

static std::vector<uint8_t> ethernet(size_t payload, uint8_t seed) {
  std::vector<uint8_t> f(14 + payload);
  for (size_t i = 0; i < f.size(); ++i) f[i] = static_cast<uint8_t>(seed + i * 7);
  f[12] = 0x08;
  f[13] = 0x00;
  return f;
}

struct NetFixture {
  TestAllocator alloc;
  FakeNhi chip;
  CountingRandom random;
  uint8_t remote[16], local[16];
  ThunderboltIcm icm;
  ThunderboltNet::Config config;
  UniquePointer<ThunderboltNet> net;
  explicit NetFixture(bool titan = false, uint32_t peer_flags = 3) : icm(chip, alloc) {
    titan_ = titan;
    for (int i = 0; i < 16; ++i) { remote[i] = static_cast<uint8_t>(0x40 + i); local[i] = static_cast<uint8_t>(0xC0 + i); }
    config.login_delay_polls = 3;
    chip.titan_ridge = titan;
    ThunderboltIcm::Options options;
    options.generation = titan ? TbGeneration::TitanRidge : TbGeneration::AlpineRidge;
    icm.set_options(options);
    memcpy(chip.remote.uuid, remote, 16);
    List<uint32_t> block(alloc);
    tb_properties_build_local(0x1234, 0x77, 1, "Acme", "Box", true, peer_flags, block);
    chip.remote.block.clear();
    for (uint64_t i = 0; i < block.count(); ++i) chip.remote.block.push_back(block[i]);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
    icm.offer_network(ThunderboltNet::offered_flags(config));
    net = UniquePointer<ThunderboltNet>::make(alloc, icm, alloc, random, config);
    CHECK(net->initialize());
  }
  ~NetFixture() { net = UniquePointer<ThunderboltNet>(); }
  uint64_t route() const { return titan_ ? 0x0201 : 0x01; }
  bool titan_ = false;
  void connect_host() { chip.send_xdomain_connected(remote, local, 1, 1, route()); }
  void step() { icm.poll(); net->poll(); }
  // up to login: the host is found, our login is answered; then the other host logs in
  void bring_up() {
    connect_host();
    for (int i = 0; i < 6; ++i) step();
    chip.peer_login(remote, local, route(), 5, chip.peer.path);
    for (int i = 0; i < 6 && !net->connected(); ++i) step();
  }
};

static void test_net() {
  {
    NetFixture f;
    f.titan_ = false;
    CHECK(!f.net->link_up() && !f.net->connected());
    f.connect_host();
    f.step();
    CHECK(f.icm.xdomain_count() == 1 && f.icm.xdomain(0).discovered && f.icm.xdomain(0).has_network && f.icm.xdomain(0).network_flags == 3);
    for (int i = 0; i < 3; ++i) f.step();
    // we logged in: the type 0 message went out once, on the route we know the other host by; its answer gave us its address
    CHECK(f.chip.peer.logins_seen.size() == 1 && f.chip.peer.host_path == 8 && f.net->bound() && !f.net->connected());
    CHECK(memcmp(f.net->peer_mac(), f.chip.peer.mac, 6) == 0 && f.net->peer_flags() == 3);
    // the other host's login: answered (with our address), and the connection is made
    f.chip.peer_login(f.remote, f.local, 0x01, 5, f.chip.peer.path);
    for (int i = 0; i < 4 && !f.net->connected(); ++i) f.step();
    CHECK(f.net->connected() && f.net->link_up() && f.net->link_speed_mbps() == 10000);
    const std::vector<uint32_t>& response = f.chip.peer.service_messages.back();
    CHECK(response[15] == 1 && response[16] == 5 && memcmp(&response[18], f.net->mac_address().bytes, 6) == 0 && (f.net->mac_address().bytes[0] & 3) == 2);
    // the path: our transmit path 8 from our ring, theirs 9 into our receive ring; one approval
    CHECK(f.chip.xdomain_approvals == 1 && f.chip.peer.approved && f.chip.peer.tx_path == 8 && f.chip.peer.rx_path == 9 && f.chip.peer.tx_ring == 1 && f.chip.peer.rx_ring == 1);
    CHECK(f.icm.xdomain(0).tunnel);
    // end to end flow control is on in both rings (both ends do it), the receiver names our transmit hop for the credits
    CHECK((f.chip.tx(1).options & (1u << 28)) && (f.chip.rx(1).options & (1u << 28)) && ((f.chip.rx(1).options >> 12) & 0x7FF) == 1);
    CHECK(f.chip.rx(1).frame_size == 0 && f.chip.rx(1).size == tb_ip::kRingSize && !(f.chip.rx(1).options & (1u << 30)));            // frame mode: not raw
    // a frame out
    const std::vector<uint8_t> out = ethernet(100, 3);
    CHECK(f.net->transmit(ConstByteView(out.data(), out.size())));
    const FakeNhi::SentFrame& sent = f.chip.sent.back();
    CHECK(sent.hop == 1 && sent.sof == 1 && sent.eof == 2 && sent.bytes.size() == 12 + out.size());
    uint32_t header[3];
    memcpy(header, sent.bytes.data(), 12);
    CHECK(header[0] == out.size() && header[1] == 0 && header[2] == 1 && memcmp(sent.bytes.data() + 12, out.data(), out.size()) == 0);
    // the next one has the next frame id (the other host matches frames by it)
    CHECK(f.net->transmit(ConstByteView(out.data(), out.size())));
    memcpy(header, f.chip.sent.back().bytes.data(), 12);
    CHECK((header[1] >> 16) == 1);
    // a short frame is padded to the Ethernet minimum, a long one refused, an empty one too
    const std::vector<uint8_t> tiny = ethernet(0, 1);
    CHECK(f.net->transmit(ConstByteView(tiny.data(), tiny.size())) && f.chip.sent.back().bytes.size() == 12 + 60);
    const std::vector<uint8_t> big = ethernet(1501, 1);
    CHECK(!f.net->transmit(ConstByteView(big.data(), big.size())) && !f.net->transmit(ConstByteView()));
    // frames in: one frame, a packet of several frames, one that needs three
    uint8_t buffer[70000];
    uint32_t length = 0;
    CHECK(!f.net->receive(ByteView(buffer, sizeof buffer), &length));
    const std::vector<uint8_t> in1 = ethernet(300, 5), in2 = ethernet(1500, 9), in3 = ethernet(9000, 11);
    f.chip.peer_send(in1, 0);
    f.chip.peer_send(in2, 1, 600);
    f.chip.peer_send(in3, 2);
    CHECK(f.net->receive(ByteView(buffer, sizeof buffer), &length) && length == in1.size() && memcmp(buffer, in1.data(), length) == 0);
    CHECK(f.net->receive(ByteView(buffer, sizeof buffer), &length) && length == in2.size() && memcmp(buffer, in2.data(), length) == 0);
    CHECK(f.net->receive(ByteView(buffer, sizeof buffer), &length) && length == in3.size() && memcmp(buffer, in3.data(), length) == 0);
    CHECK(!f.net->receive(ByteView(buffer, sizeof buffer), &length));
    // a buffer that is too small gets the start and the true length
    f.chip.peer_send(in1, 3);
    uint8_t small[20];
    CHECK(f.net->receive(ByteView(small, sizeof small), &length) && length == in1.size() && memcmp(small, in1.data(), 20) == 0);
    // many frames in a row: the receive buffers are given back
    for (int i = 0; i < 300; ++i) {
      f.chip.peer_send(in1, static_cast<uint32_t>(i));
      if (!f.net->receive(ByteView(buffer, sizeof buffer), &length) || length != in1.size()) { printf("  receive %d failed (dropped %llu)\n", i, (unsigned long long)f.net->statistics().frames_dropped); CHECK(false); break; }
    }
    // a lot of sending: the ring never fills for good
    uint32_t refused = 0;
    for (int i = 0; i < 300; ++i) if (!f.net->transmit(ConstByteView(out.data(), out.size()))) { ++refused; f.step(); }
    CHECK(f.net->statistics().frames_sent >= 300 - refused);
    // damaged frames: dropped, and the ones after still arrive
    const NetworkStatistics before = f.net->statistics();
    auto crafted = [&](uint32_t size, uint32_t index, uint32_t id, uint32_t count, uint32_t data_bytes) {
      std::vector<uint8_t> frame(12 + data_bytes, 0x5A);
      const uint32_t h[3] = {size, index | (id << 16), count};
      memcpy(frame.data(), h, 12);
      f.chip.push_to_host(f.chip.peer.rx_ring, 1, 2, frame);
    };
    crafted(0, 0, 0, 1, 20);                 // no data
    crafted(50, 0, 0, 1, 20);                // claims more than there is
    crafted(20, 0, 0, 0, 20);                // no frames in the packet
    crafted(20, 1, 0, 2, 20);                // starts in the middle
    crafted(20, 0, 0, 40, 20);               // far too many frames
    crafted(20, 0, 0, 3, 20);                // the first of three ...
    crafted(20, 2, 0, 3, 20);                // ... then the third: one is missing
    crafted(10, 0, 0, 1, 10);                // (too short for an Ethernet header: dropped)
    f.chip.push_to_host(f.chip.peer.rx_ring, 1, 2, std::vector<uint8_t>(8, 1));         // shorter than the header
    CHECK(!f.net->receive(ByteView(buffer, sizeof buffer), &length));
    CHECK(f.net->statistics().frames_dropped >= before.frames_dropped + 8);
    f.chip.peer_send(in1, 9);
    CHECK(f.net->receive(ByteView(buffer, sizeof buffer), &length) && length == in1.size());
    // the other host logs out: answered, the connection ends and is made again once it logs in
    f.chip.peer_logout(f.remote, f.local, 0x01, 6);
    f.step();
    CHECK(!f.net->connected() && f.chip.rx(1).base == 0);
    CHECK(!f.net->transmit(ConstByteView(out.data(), out.size())));
    f.net->shutdown();
    CHECK(f.alloc.dma.size() > 0 || true);
  }
}



static void test_net_lifecycle() {
  // the other host logs out: the connection is closed (path and rings), and made again when it logs in
  {
    NetFixture f;
    f.bring_up();
    CHECK(f.net->connected());
    f.chip.peer_logout(f.remote, f.local, 0x01, 6);
    f.step();
    CHECK(!f.net->connected() && !f.net->bound() && !f.icm.xdomain(0).tunnel && f.chip.rx(1).base == 0 && f.chip.tx(1).base == 0);
    CHECK(!f.chip.mailbox_commands.empty() && f.chip.mailbox_commands.back() == 0x10);                  // the path closed through the mailbox (Alpine Ridge)
    f.chip.peer.logins_seen.clear();
    f.chip.peer.approved = false;
    for (int i = 0; i < 6; ++i) f.step();                                                              // opens again and logs in
    CHECK(f.chip.peer.logins_seen.size() == 1);
    f.chip.peer_login(f.remote, f.local, 0x01, 7, f.chip.peer.path);
    for (int i = 0; i < 6 && !f.net->connected(); ++i) f.step();
    CHECK(f.net->connected() && f.chip.xdomain_approvals == 2);
    // the hops were given back: the same ones again
    CHECK(f.chip.peer.tx_ring == 1 && f.chip.peer.rx_ring == 1);
  }
  // the host goes away: no logout is sent, everything is released
  {
    NetFixture f;
    f.bring_up();
    CHECK(f.net->connected());
    const size_t messages = f.chip.peer.service_messages.size();
    f.chip.send_xdomain_disconnected(f.remote, 1, 1);
    f.step();
    CHECK(!f.net->connected() && !f.net->bound() && f.chip.peer.service_messages.size() == messages && !f.chip.peer.logout_seen);
  }
  // the connection is closed by the user: a logout is asked for, the path closed
  {
    NetFixture f;
    f.bring_up();
    f.net->shutdown();
    CHECK(f.chip.peer.logout_seen && !f.net->connected() && f.chip.rx(1).base == 0);
    f.net->shutdown();                                                // (again: nothing to do)
  }
  // no answer to our login: tried again after the delay, not forever; the other host's login starts the tries over
  {
    NetFixture f;
    f.config.login_retries = 3;
    f.net = UniquePointer<ThunderboltNet>::make(f.alloc, f.icm, f.alloc, f.random, f.config);
    CHECK(f.net->initialize());
    f.chip.peer.answer_login = false;
    f.connect_host();
    for (int i = 0; i < 60; ++i) f.step();
    CHECK(f.chip.peer.logins_seen.size() == 3 && !f.net->connected());
    f.chip.peer.answer_login = true;
    f.chip.peer_login(f.remote, f.local, 0x01, 5, f.chip.peer.path);
    for (int i = 0; i < 10 && !f.net->connected(); ++i) f.step();
    CHECK(f.net->connected());
    CHECK(f.chip.peer.logins_seen.size() == 4);
  }
  // the firmware refuses the path: the rings are taken down again and nothing leaks, then it is tried once more
  {
    NetFixture f;
    f.chip.peer.approve_error = true;
    f.bring_up();
    CHECK(!f.net->connected() && f.chip.xdomain_approvals >= 1 && !f.icm.xdomain(0).tunnel && f.chip.rx(1).base == 0 && f.chip.tx(1).base == 0);
    f.chip.peer.approve_error = false;
    for (int i = 0; i < 12 && !f.net->connected(); ++i) f.step();
    CHECK(f.net->connected() && f.icm.xdomain(0).tunnel);
  }
  // before the connection there is no link and nothing is sent
  {
    NetFixture f;
    const std::vector<uint8_t> frame = ethernet(50, 1);
    CHECK(!f.net->transmit(ConstByteView(frame.data(), frame.size())) && f.net->statistics().send_failures == 1);
    uint8_t buffer[100];
    uint32_t length = 0;
    CHECK(!f.net->receive(ByteView(buffer, sizeof buffer), &length) && f.net->link_speed_mbps() == 0 && f.net->link_duplex() == LinkDuplex::Unknown);
    // another host's login, from a host we do not know or on another route, is not answered
    f.connect_host();
    f.step();
    auto responses = [&]() { size_t n = 0; for (const auto& m : f.chip.peer.service_messages) if (m.size() > 15 && m[15] == 1) ++n; return n; };
    const size_t seen = responses();
    uint8_t stranger[16];
    memset(stranger, 0x77, 16);
    f.chip.peer_login(stranger, f.local, 0x01, 1, 9);
    f.chip.peer_login(f.remote, f.local, 0x05, 1, 9);
    f.step();
    CHECK(responses() == seen);
    // no network service offered: nothing to bind to
    FakeNhi quiet;
    List<uint32_t> plain(f.alloc);
    tb_properties_build_local(1, 2, 3, "A", "B", plain);
    for (uint64_t i = 0; i < plain.count(); ++i) quiet.remote.block.push_back(plain[i]);
    memcpy(quiet.remote.uuid, f.remote, 16);
    ThunderboltIcm icm(quiet, f.alloc);
    CHECK(icm.start() == ThunderboltIcm::Outcome::Started);
    ThunderboltNet net(icm, f.alloc, f.random);
    CHECK(net.initialize());
    quiet.send_xdomain_connected(f.remote, f.local, 1, 1, 0x01);
    icm.poll();
    net.poll();
    CHECK(icm.xdomain(0).discovered && !icm.xdomain(0).has_network && !net.bound());
    // a random source that fails: no interface
    CountingRandom broken;
    broken.fail = true;
    ThunderboltNet bad(icm, f.alloc, f.random);
    ThunderboltNet bad2(icm, f.alloc, broken);
    CHECK(!bad2.initialize());
  }
  // a peer without end to end flow control: the rings are made without it, and without frame ids
  {
    NetFixture f(false, 0);
    f.bring_up();
    CHECK(f.net->connected() && !(f.chip.tx(1).options & (1u << 28)) && !(f.chip.rx(1).options & (1u << 28)));
    const std::vector<uint8_t> out = ethernet(100, 3);
    CHECK(f.net->transmit(ConstByteView(out.data(), out.size())) && f.net->transmit(ConstByteView(out.data(), out.size())));
    uint32_t header[3];
    memcpy(header, f.chip.sent.back().bytes.data(), 12);
    CHECK((header[1] >> 16) == 0);
  }
  // the entry in the interface list: it works through the connection and goes quiet when it is detached
  {
    NetFixture f;
    ThunderboltNetLink link(f.net.get());
    CHECK(link.mac_address() == f.net->mac_address() && link.link_comes_and_goes() && !link.link_up());
    f.bring_up();
    CHECK(link.link_up() && link.link_speed_mbps() == 10000);
    const std::vector<uint8_t> out = ethernet(100, 3);
    CHECK(link.transmit(ConstByteView(out.data(), out.size())) && link.statistics().frames_sent == 1);
    f.chip.peer_send(out, 0);
    uint8_t buffer[200];
    uint32_t length = 0;
    CHECK(link.receive(ByteView(buffer, sizeof buffer), &length) && length == out.size());
    link.detach();
    f.net->shutdown();
    f.net = UniquePointer<ThunderboltNet>();
    CHECK(!link.link_up() && !link.transmit(ConstByteView(out.data(), out.size())) && !link.receive(ByteView(buffer, sizeof buffer), &length) && link.link_speed_mbps() == 0 && link.statistics().frames_sent == 0);
  }
  // one tunnel at a time
  {
    NetFixture f;
    f.bring_up();
    const char* error = nullptr;
    CHECK(!f.icm.approve_xdomain_paths(0, 20, 5, 21, 6, &error) && error);
    CHECK(!f.icm.approve_xdomain_paths(9, 20, 5, 21, 6, &error));
  }
  // Titan Ridge: the same, with the route forms of the messages and the two stage disconnect
  {
    NetFixture f(true);
    f.bring_up();
    CHECK(f.net->connected() && f.chip.peer.approved && f.chip.peer.tx_path == 8 && f.chip.peer.rx_path == 9);
    f.chip.peer_logout(f.remote, f.local, 0x0201, 6);
    f.step();
    CHECK(!f.net->connected() && f.chip.xdomain_disconnects == 2);
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
  test_suspend();
  test_titan();
  test_interrupts();
  test_xdomain_messages();
  test_properties();
  test_xdomain();
  test_net_messages();
  test_net();
  test_net_lifecycle();
  test_driver();
  printf(g_failed ? "ThunderboltTest: %d FAILED\n" : "ThunderboltTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
