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

// The SD card protocol and storage device against a model of a card and its host: identification of version 2 (high capacity) and version 1 (standard capacity) cards, the registers,
// the 4 bit bus, block transfers through the storage device (chunked, addressed by block or by byte), write protection, a card that is not an SD memory card, a card that goes away,
// and the slot source that makes a device when a card is put in.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "FakeSd.hpp"
#include "Hardware/StorageDevice/Sd/SdStorageDevice.hpp"

namespace hardware { void delay_us(uint64_t) {} }

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::vector<void*> blocks;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); blocks.push_back(p); return p; }
  void deallocate(void*) override {}
  ~TestAllocator() { for (void* p : blocks) free(p); }
};

static void pattern(std::vector<uint8_t>& v, uint32_t seed) { for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint8_t>((i * 31 + seed) ^ (i >> 8)); }

static void test_registers() {
  FakeSdCard card(2048);
  uint32_t w[4];
  card.csd(w);
  uint64_t blocks;
  bool ro;
  CHECK(sd_parse_csd(w, &blocks, &ro) && blocks == 2048 && !ro);
  card.high_capacity = false;
  card.csd(w);
  CHECK(sd_parse_csd(w, &blocks, &ro) && blocks == 2048);
  card.csd_write_protected = true;
  card.csd(w);
  CHECK(sd_parse_csd(w, &blocks, &ro) && ro);
  w[0] = 2u << 30;                                                     // a CSD structure that does not exist
  CHECK(!sd_parse_csd(w, &blocks, &ro));
  card.cid(w);
  SdCardInfo info;
  sd_parse_cid(w, &info);
  CHECK(strcmp(info.name, "SD16G") == 0 && info.serial == 0xCAFE0123u && info.manufacturer == 3);
  // a 64 GiB high capacity card
  FakeSdCard big(1);
  big.blocks = 128u * 1024 * 1024;
  big.csd(w);
  CHECK(sd_parse_csd(w, &blocks, &ro) && blocks == 128u * 1024 * 1024);
}

static void test_high_capacity() {
  TestAllocator a;
  FakeSdCard card(4096);
  FakeSdHost host;
  host.insert(&card);
  {
    SdStorageDevice device(host, a);
    CHECK(device.initialize() && device.present());
    const StorageInfo info = device.info();
    CHECK(info.block_count == 4096 && info.block_size == 512 && info.removable && !info.read_only && strcmp(info.model, "SD SD16G") == 0 && strcmp(info.serial, "CAFE0123") == 0);
    CHECK(host.width == 4 && host.clock == 25000000 && host.clocks.size() >= 2 && host.clocks[0] == 400000);
    // the sequence of commands of an identification
    const uint8_t expected[] = {0, 8, 55, 41, 55, 41, 55, 41, 2, 3, 9, 7, 55, 51, 55, 6};
    CHECK(host.commands_seen.size() == sizeof expected && memcmp(host.commands_seen.data(), expected, sizeof expected) == 0);
    // a write and a read back: odd sizes and positions, through chunks of the bounce buffer and of the host's limit
    std::vector<uint8_t> out(300 * 512), in(300 * 512);
    pattern(out, 7);
    CHECK(device.write(10, ConstByteView(out.data(), out.size())));
    CHECK(device.read(10, ByteView(in.data(), in.size())) && in == out);
    CHECK(card.data[10 * 512 + 5] == out[5] && card.data[309 * 512 + 511] == out[300 * 512 - 1] && card.data[310 * 512] == 0 && card.data[9 * 512 + 511] == 0);
    std::vector<uint8_t> one(512), got(512);
    pattern(one, 99);
    CHECK(device.write(4095, ConstByteView(one.data(), 512)) && device.read(4095, ByteView(got.data(), 512)) && got == one);   // (the last block, a single block command)
    CHECK(!device.read(4096, ByteView(got.data(), 512)) && !device.read(4095, ByteView(in.data(), 1024)) && !device.read(0, ByteView(got.data(), 100)));    // outside / not whole blocks
    CHECK(device.statistics().errors == 3 && device.statistics().blocks_written == 301);
    // a transfer error is an error, and the next one works
    card.inject_error = true;
    CHECK(!device.read(0, ByteView(got.data(), 512)) && card.errors_seen == 1);
    CHECK(device.read(0, ByteView(got.data(), 512)));
    // the slot's write protect switch
    host.write_protect_switch = true;
    CHECK(!device.write(0, ConstByteView(one.data(), 512)));
    host.write_protect_switch = false;
    CHECK(device.write(0, ConstByteView(one.data(), 512)) && device.flush());
    // the card is pulled out
    host.remove();
    CHECK(!device.present() && !device.read(0, ByteView(got.data(), 512)));
    host.insert(&card);                                                // and put back: not the same card for the device
    CHECK(!device.present());
    device.shutdown();
  }
  CHECK(a.blocks.size() > 0);
}

static void test_standard_capacity() {
  TestAllocator a;
  FakeSdCard card(2048);
  card.version2 = false;
  card.high_capacity = false;
  card.supports_4bit = false;
  FakeSdHost host;
  host.insert(&card);
  SdStorageDevice device(host, a);
  CHECK(device.initialize() && device.info().block_count == 2048 && host.width == 1 && card.block_length == 512);
  bool cmd8 = false, cmd16 = false;
  for (uint8_t c : host.commands_seen) { cmd8 = cmd8 || c == 8; cmd16 = cmd16 || c == 16; }
  CHECK(cmd8 && cmd16);                                                // (tried CMD8 and got no answer; the block length is set for a standard capacity card)
  std::vector<uint8_t> out(5 * 512), in(5 * 512);
  pattern(out, 3);
  CHECK(device.write(100, ConstByteView(out.data(), out.size())) && device.read(100, ByteView(in.data(), in.size())) && in == out);
  CHECK(card.data[100 * 512] == out[0] && card.data[104 * 512 + 511] == out[5 * 512 - 1]);       // (byte addressed: the model refuses addresses that are not multiples of 512)
  // a card with write protection in its CSD
  FakeSdCard locked(1024);
  locked.csd_write_protected = true;
  FakeSdHost host2;
  host2.insert(&locked);
  SdStorageDevice d2(host2, a);
  CHECK(d2.initialize() && d2.info().read_only && !d2.write(0, ConstByteView(out.data(), 512)) && d2.read(0, ByteView(in.data(), 512)));
}

static void test_not_a_card() {
  TestAllocator a;
  FakeSdHost host;
  SdStorageDevice none(host, a);
  CHECK(!none.initialize());                                           // no card
  FakeSdCard mmc(1024);
  mmc.is_sd = false;
  host.insert(&mmc);
  SdStorageDevice d1(host, a);
  CHECK(!d1.initialize());
  FakeSdCard stuck(1024);
  stuck.power_up_polls = 100000;                                       // never finishes powering up
  host.insert(&stuck);
  SdStorageDevice d2(host, a);
  CHECK(!d2.initialize());
  FakeSdCard fine(1024);
  host.insert(&fine);
  host.fail_power = true;
  SdStorageDevice d3(host, a);
  CHECK(!d3.initialize());
  host.fail_power = false;
  SdStorageDevice d4(host, a);
  CHECK(d4.initialize());
  d4.shutdown();
  CHECK(host.power_offs >= 1 && !d4.present());
}

static void test_slot_source() {
  TestAllocator a;
  FakeSdHost host;
  SdSlotSource source(host, a);
  List<UniquePointer<IStorageDevice>> found(a);
  source.discover(found);
  CHECK(found.count() == 0);                                           // empty slot
  FakeSdCard card(2048);
  host.insert(&card);
  source.discover(found);
  CHECK(found.count() == 1 && found[0]->present() && strcmp(found[0]->name(), "SD card") == 0 && found[0]->info().removable);
  UniquePointer<IStorageDevice> kept = util::move(found[0]);
  found.clear();
  source.discover(found);
  CHECK(found.count() == 0);                                           // the same card: nothing new
  host.remove();
  source.discover(found);
  CHECK(found.count() == 0 && !kept->present());
  FakeSdCard second(4096);
  host.insert(&second);
  source.discover(found);
  CHECK(found.count() == 1 && found[0]->info().block_count == 4096);
  found.clear();
  // a card that cannot be identified is tried a few times, then left alone until it is taken out
  FakeSdCard mmc(1024);
  mmc.is_sd = false;
  host.insert(&mmc);
  for (int i = 0; i < 10; ++i) source.discover(found);
  CHECK(found.count() == 0);
  const uint32_t power_ons = host.power_ons;
  source.discover(found);
  CHECK(host.power_ons == power_ons && power_ons >= SdSlotSource::kMaxAttempts);
  host.remove();
  host.insert(&second);
  source.discover(found);
  CHECK(found.count() == 1);
  kept->shutdown();
}

int main() {
  test_registers();
  test_high_capacity();
  test_standard_capacity();
  test_not_a_card();
  test_slot_source();
  printf(g_failed ? "SdCardTest: %d FAILED\n" : "SdCardTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
