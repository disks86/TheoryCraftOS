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

// USB Attached SCSI: the information units and the interface search (exact layouts, then every truncation and random bit flips), and
// the UAS disk driver against a fake device with streams (FakeUas.hpp): found ahead of Bulk-Only, its queue depth, reads and writes
// checked against a model from several threads with commands overlapping and finishing out of order, a CHECK CONDITION before the data
// stage (the queued data transfer taken back, the tag usable again), UNIT ATTENTION retried, unplugging, a device that will not come
// up (put back to alternate setting 0), and the USB 2 way without streams. Built with ASan/UBSan and, separately, ThreadSanitizer.
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <thread>
#include <vector>
#include "FakeUas.hpp"
#include "Hardware/StorageDevice/Usb/UasProtocol.hpp"
#include "Hardware/StorageDevice/Usb/UasStorageDevice.hpp"
#include "Hardware/StorageDevice/Usb/UsbMassStorageDevice.hpp"

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int64_t> live{0};
  void* allocate(uint64_t size) override { return allocate_aligned(size, 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    ++live;
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override {
    if (p) {
      --live;
      free(p);
    }
  }
};

void test_protocol() {
  uint8_t iu[32];
  const uint8_t cdb[10] = {0x28, 0, 0, 0, 0x10, 0, 0, 0, 8, 0};
  CHECK(uas::build_command_iu(ByteView(iu), 0x1234, ConstByteView(cdb)));
  CHECK(iu[0] == 0x01 && iu[2] == 0x12 && iu[3] == 0x34 && iu[4] == 0 && iu[6] == 0 && iu[16] == 0x28 && iu[24] == 8 && iu[26] == 0 && iu[31] == 0);
  CHECK(!uas::build_command_iu(ByteView(iu), 0, ConstByteView(cdb)));                      // tag 0 is not a tag
  CHECK(!uas::build_command_iu(ByteView(iu).first(31), 1, ConstByteView(cdb)));
  uint8_t long_cdb[17] = {};
  CHECK(!uas::build_command_iu(ByteView(iu), 1, ConstByteView(long_cdb)));

  uint8_t sense[34] = {0x03, 0, 0x00, 0x07, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 18, 0x70, 0, 0x05};
  uas::StatusIu s;
  CHECK(uas::parse_status_iu(ConstByteView(sense), &s) && s.id == uas::kIuSense && s.tag == 7 && s.status == 2 && s.sense.size() == 18 && s.sense[2] == 5);
  CHECK(!uas::parse_status_iu(ConstByteView(sense).first(33), &s));   // sense length past the end
  const uint8_t ready[4] = {0x06, 0, 0, 3}, response[8] = {0x04, 0, 0, 9, 0, 0, 0, 0x05}, unknown[8] = {0x09, 0, 0, 1};
  CHECK(uas::parse_status_iu(ConstByteView(ready), &s) && s.id == uas::kIuReadReady && s.tag == 3);
  CHECK(uas::parse_status_iu(ConstByteView(response), &s) && s.id == uas::kIuResponse && s.response_code == 5);
  CHECK(!uas::parse_status_iu(ConstByteView(unknown), &s));
  const uint8_t zero_tag[4] = {0x06, 0, 0, 0};
  CHECK(!uas::parse_status_iu(ConstByteView(zero_tag), &s));

  // The fake device's configuration: the UAS alternate setting with its pipes and 32 streams.
  FakeUasHost host;
  uint8_t config[256];
  uint16_t length = 0;
  CHECK(host.configuration_descriptor(0, ByteView(config), &length));
  uas::UasInterface u;
  CHECK(uas::find_uas_interface(ConstByteView(config).first(length), &u));
  CHECK(u.number == 0 && u.alternate == 1 && u.endpoint[0] == 0x01 && u.endpoint[1] == 0x82 && u.endpoint[2] == 0x83 && u.endpoint[3] == 0x04 && u.streams == 32);

  // Every truncation, and random bit flips: no crash, and nothing parsed that is not whole.
  for (uint32_t n = 0; n < sizeof sense; ++n) (void)uas::parse_status_iu(ConstByteView(sense).first(n), &s);
  for (uint32_t n = 0; n < length; ++n) {
    uas::UasInterface t;
    if (uas::find_uas_interface(ConstByteView(config).first(n), &t)) CHECK(t.endpoint[3] != 0);
  }
  std::mt19937 rng(7);
  for (int round = 0; round < 20000; ++round) {
    uint8_t c[256], i2[34];
    memcpy(c, config, length);
    memcpy(i2, sense, sizeof sense);
    for (int k = 0; k < 3; ++k) {
      c[rng() % length] ^= uint8_t(1u << (rng() % 8));
      i2[rng() % sizeof i2] ^= uint8_t(1u << (rng() % 8));
    }
    uas::UasInterface t;
    if (uas::find_uas_interface(ConstByteView(c).first(length), &t))
      for (uint8_t e : t.endpoint) CHECK(e != 0);
    if (uas::parse_status_iu(ConstByteView(i2), &s) && s.id == uas::kIuSense) CHECK(s.sense.size() <= sizeof i2 - 16);
  }
}

void pattern(std::vector<uint8_t>& v, uint32_t seed) {
  for (size_t i = 0; i < v.size(); ++i) v[i] = uint8_t((i * 131 + seed * 7) ^ (i >> 9));
}

UniquePointer<IStorageDevice> discover(FakeUasHost& host, TestAllocator& a, List<UniquePointer<IStorageDevice>>& found) {
  UsbMassStorageDevice::discover_port(host, 0, a, nullptr, found);
  return found.count() ? util::move(found[0]) : UniquePointer<IStorageDevice>();
}

void test_streams() {
  TestAllocator a;
  {
    FakeUasHost host;
    List<UniquePointer<IStorageDevice>> found(a);
    UniquePointer<IStorageDevice> d = discover(host, a, found);
    CHECK(d && host.alternate == 1);
    if (!d) return;
    const StorageInfo info = d->info();
    CHECK(strcmp(d->name(), "USB disk (UAS)") == 0 && info.queue_depth == 16 && info.block_count == host.blocks && info.block_size == 512);

    // One big request in parallel chunks.
    std::vector<uint8_t> out(600 * 1024), in(600 * 1024);
    pattern(out, 1);
    CHECK(d->write(100, ConstByteView(out.data(), out.size())) && d->read(100, ByteView(in.data(), in.size())) && in == out);

    // Threads on their own regions, at random, checked against a model; the device sees overlapping commands.
    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    std::atomic<int> bad{0};
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        std::mt19937 r(t + 10);
        const uint64_t base = 2000 + uint64_t(t) * 600;
        std::vector<uint8_t> model(600 * 512, 0), buf, back;
        for (int i = 0; i < 120; ++i) {
          const uint32_t first = r() % 500, count = 1 + r() % 100;
          buf.resize(count * 512);
          back.resize(count * 512);
          if (r() % 2) {
            pattern(buf, t * 1000 + i);
            if (!d->write(base + first, ConstByteView(buf.data(), buf.size()))) ++bad;
            memcpy(model.data() + first * 512, buf.data(), buf.size());
          } else if (!d->read(base + first, ByteView(back.data(), back.size())) || memcmp(back.data(), model.data() + first * 512, back.size()) != 0) {
            ++bad;
          }
        }
      });
    }
    for (std::thread& t : threads) t.join();
    CHECK(bad == 0);
    printf("  streams: %llu commands, at most %u in flight at once\n", (unsigned long long)host.commands_seen, host.peak);
    CHECK(host.peak >= 4);

    // CHECK CONDITION before the data stage: the read fails, its queued data transfer is taken back, and the tag works again after.
    host.medium_error_lba = 5000;
    std::vector<uint8_t> small(20 * 512);
    CHECK(!d->read(4990, ByteView(small.data(), small.size())));
    CHECK(host.cancels >= 1);
    host.medium_error_lba = ~0ull;
    for (int i = 0; i < 40; ++i) CHECK(d->read(i * 8, ByteView(small.data(), 8 * 512)));   // (every tag used again)
    CHECK(d->read(4990, ByteView(small.data(), small.size())));

    // UNIT ATTENTION: retried.
    host.unit_attentions = 1;
    CHECK(d->read(0, ByteView(small.data(), 512)));
    CHECK(d->flush());

    // Unplugged: everything fails.
    {
      std::lock_guard<std::mutex> g(host.m);
      host.generation = 2;
    }
    CHECK(!d->present() && !d->read(0, ByteView(small.data(), 512)));
    d->shutdown();
  }
  CHECK(a.live == 0);
}

void test_without_streams() {
  TestAllocator a;
  {
    FakeUasHost host;
    host.streams_offered = 0;   // a USB 2 port
    List<UniquePointer<IStorageDevice>> found(a);
    UniquePointer<IStorageDevice> d = discover(host, a, found);
    CHECK(d);
    if (!d) return;
    CHECK(d->info().queue_depth == 1);
    std::vector<uint8_t> out(200 * 1024), in(200 * 1024);
    pattern(out, 3);
    CHECK(d->write(10, ConstByteView(out.data(), out.size())) && d->read(10, ByteView(in.data(), in.size())) && in == out);
    std::vector<std::thread> threads;
    std::atomic<int> bad{0};
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&, t] {
        std::vector<uint8_t> buf(16 * 512), back(16 * 512);
        for (int i = 0; i < 30; ++i) {
          pattern(buf, t * 100 + i);
          if (!d->write(1000 + t * 100, ConstByteView(buf.data(), buf.size())) || !d->read(1000 + t * 100, ByteView(back.data(), back.size())) || back != buf) ++bad;
        }
      });
    }
    for (std::thread& t : threads) t.join();
    CHECK(bad == 0);
    host.medium_error_lba = 300;
    std::vector<uint8_t> small(8 * 512);
    CHECK(!d->read(296, ByteView(small.data(), small.size())) && d->read(0, ByteView(small.data(), small.size())));
    d->shutdown();
  }
  CHECK(a.live == 0);
}

// A UAS device that does not come up is put back to alternate setting 0 (for Bulk-Only, which this fake does not speak: nothing found).
void test_no_start() {
  TestAllocator a;
  FakeUasHost host;
  host.fail_inquiry = true;
  List<UniquePointer<IStorageDevice>> found(a);
  UsbMassStorageDevice::discover_port(host, 0, a, nullptr, found);
  CHECK(found.count() == 0 && host.alternate == 0 && host.set_interfaces >= 2);
}

}  // namespace

int main() {
  test_protocol();
  test_streams();
  test_without_streams();
  test_no_start();
  if (g_failed) {
    printf("UasTest: %d FAILED\n", g_failed.load());
    return 1;
  }
  printf("UasTest: all passed\n");
  return 0;
}
