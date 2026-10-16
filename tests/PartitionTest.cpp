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

// Host-side tests for partition scanning (MBR, extended partitions, GPT with
// backup recovery), CRC-32 and PartitionStorageDevice.
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Hardware/StorageDevice/Partition/PartitionScanner.hpp"
#include "Hardware/StorageDevice/Partition/PartitionStorageDevice.hpp"
#include "Utility/Crc32.hpp"

using namespace hardware;

struct TestAllocator final : allocators::IAllocator {
  int live = 0; bool fail = false;
  void* allocate(uint64_t size) override { if (fail) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

struct MemDisk final : IStorageDevice {
  MemDisk(uint64_t block_count, uint32_t block) : block_size(block), blocks(block_count) { data = static_cast<uint8_t*>(calloc(blocks, block)); }
  ~MemDisk() { free(data); }
  const char* name() const override { return "mem"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  StorageInfo info() const override { StorageInfo i; i.block_count = blocks; i.block_size = block_size; return i; }
  StorageStatistics statistics() const override { return {}; }
  bool read(uint64_t lba, ByteView b) override {
    if (fail_reads || b.size() % block_size || lba + b.size() / block_size > blocks) return false;
    memcpy(b.data(), data + lba * block_size, b.size()); return true;
  }
  bool write(uint64_t lba, ConstByteView b) override {
    if (b.size() % block_size || lba + b.size() / block_size > blocks) return false;
    memcpy(data + lba * block_size, b.data(), b.size()); return true;
  }
  bool flush() override { return true; }
  uint8_t* at(uint64_t lba) { return data + lba * block_size; }
  uint8_t* data; uint32_t block_size; uint64_t blocks; bool fail_reads = false;
};

static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = v >> (8 * i); }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static void mbr_entry(uint8_t* sector, int slot, uint8_t status, uint8_t type, uint32_t start, uint32_t count) {
  uint8_t* e = sector + 446 + slot * 16;
  e[0] = status; e[4] = type; put32(e + 8, start); put32(e + 12, count);
  sector[510] = 0x55; sector[511] = 0xAA;
}

static const uint8_t kEfi[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x1E, 0x6B, 0x7B};
static const uint8_t kLinux[16] = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};

struct GptSpec { const uint8_t* type; uint64_t first, last; const uint16_t* name; };

// Writes a protective MBR, both GPT headers and both entry arrays (128 entries of 128 bytes).
static void make_gpt(MemDisk& d, const GptSpec* specs, int n, int slot_gap = 0) {
  const uint32_t bs = d.block_size;
  const uint64_t array_blocks = 128 * 128 / bs;
  const uint64_t last = d.blocks - 1;
  mbr_entry(d.at(0), 0, 0, 0xEE, 1, (uint32_t)(last > 0xFFFFFFFF ? 0xFFFFFFFF : last));
  uint8_t* array = d.at(2);
  for (int i = 0; i < n; ++i) {
    uint8_t* e = array + (i + slot_gap * (i > 0)) * 128;
    memcpy(e, specs[i].type, 16); e[16] = i + 1; e[17] = 0xAB;
    put64(e + 32, specs[i].first); put64(e + 40, specs[i].last);
    for (int c = 0; specs[i].name && specs[i].name[c]; ++c) { e[56 + c * 2] = specs[i].name[c]; e[57 + c * 2] = specs[i].name[c] >> 8; }
  }
  const uint64_t backup_array = last - array_blocks;
  memcpy(d.at(backup_array), array, 128 * 128);
  const uint32_t array_crc = crc32(ConstByteView(array, 128 * 128));
  for (int which = 0; which < 2; ++which) {
    uint8_t* h = d.at(which == 0 ? 1 : last);
    memcpy(h, "EFI PART", 8); put32(h + 8, 0x00010000); put32(h + 12, 92);
    put64(h + 24, which == 0 ? 1 : last); put64(h + 32, which == 0 ? last : 1);
    put64(h + 40, 2 + array_blocks); put64(h + 48, last - array_blocks - 1);
    h[56] = 0xDD;
    put64(h + 72, which == 0 ? 2 : backup_array); put32(h + 80, 128); put32(h + 84, 128); put32(h + 88, array_crc);
    put32(h + 16, crc32(ConstByteView(h, 92)));
  }
}

static const uint16_t kName1[] = {'E', 'S', 'P', 0};
static const uint16_t kName2[] = {'d', 'a', 't', 'a', ' ', 0xE9, 0xD83D, 0xDE00, 0};

static void test_crc() {
  const char* s = "123456789";
  CHECK(crc32(ConstByteView(reinterpret_cast<const uint8_t*>(s), 9)) == 0xCBF43926u);
  CHECK(crc32(ConstByteView(reinterpret_cast<const uint8_t*>(s + 4), 5), crc32(ConstByteView(reinterpret_cast<const uint8_t*>(s), 4))) == 0xCBF43926u);
  CHECK(crc32(ConstByteView()) == 0);
}

static void test_none(uint32_t bs) {
  TestAllocator a, la; List<PartitionInfo> parts(la); PartitionScheme scheme = PartitionScheme::Gpt;
  MemDisk blank(100, bs);
  CHECK(scan_partitions(blank, a, &parts, &scheme) && scheme == PartitionScheme::None && parts.count() == 0);
  // A filesystem boot sector also ends in 55 AA but is not a partition table.
  MemDisk fat(100, bs);
  uint8_t* b = fat.at(0);
  b[0] = 0xEB; b[11] = 0; b[12] = 2; b[13] = 4; b[16] = 2; b[510] = 0x55; b[511] = 0xAA;
  b[446] = 0x00; b[446 + 4] = 0x0C; put32(b + 446 + 8, 10); put32(b + 446 + 12, 10);  // junk that would parse as an entry
  CHECK(scan_partitions(fat, a, &parts, &scheme) && scheme == PartitionScheme::None && parts.count() == 0);
  // Signature but an impossible status byte.
  MemDisk junk(100, bs);
  junk.at(0)[510] = 0x55; junk.at(0)[511] = 0xAA; junk.at(0)[446] = 0x55;
  CHECK(scan_partitions(junk, a, &parts, &scheme) && scheme == PartitionScheme::None);
  // Signature and empty entries: a table with no partitions.
  MemDisk empty(100, bs);
  empty.at(0)[510] = 0x55; empty.at(0)[511] = 0xAA;
  CHECK(scan_partitions(empty, a, &parts, &scheme) && scheme == PartitionScheme::None && parts.count() == 0);
  CHECK(a.live == 0);
}

static void test_mbr(uint32_t bs) {
  TestAllocator a, la; List<PartitionInfo> parts(la); PartitionScheme scheme;
  MemDisk d(10000, bs);
  mbr_entry(d.at(0), 0, 0x80, 0x0C, 64, 1000);
  mbr_entry(d.at(0), 1, 0x00, 0x83, 2000, 500);
  mbr_entry(d.at(0), 2, 0x00, 0x0F, 3000, 4000);   // extended: 3000..6999
  mbr_entry(d.at(0), 3, 0x00, 0x83, 9000, 5000);   // reaches past the end: ignored
  // EBR 1 at the start of the extended partition: logical at +63, link to EBR 2 at ext+1000.
  mbr_entry(d.at(3000), 0, 0, 0x06, 63, 900);
  mbr_entry(d.at(3000), 1, 0, 0x05, 1000, 1500);
  // EBR 2: logical at +63 (absolute 4063), link to EBR 3 at ext+2500 (relative to the extended start).
  mbr_entry(d.at(4000), 0, 0x80, 0x07, 63, 800);
  mbr_entry(d.at(4000), 1, 0, 0x05, 2500, 1500);
  // EBR 3: a logical partition that overruns the extended partition: dropped, chain ends.
  mbr_entry(d.at(5500), 0, 0, 0x83, 63, 9000);
  CHECK(scan_partitions(d, a, &parts, &scheme) && scheme == PartitionScheme::Mbr);
  CHECK(parts.count() == 4);
  if (parts.count() == 4) {
    CHECK(parts[0].index == 1 && parts[0].first_block == 64 && parts[0].block_count == 1000 && parts[0].bootable && !parts[0].logical && parts[0].mbr_type == 0x0C);
    CHECK(!strcmp(partition_type_name(parts[0]), "FAT32") && !is_efi_system_partition(parts[0]));
    CHECK(parts[1].index == 2 && parts[1].first_block == 2000 && !parts[1].bootable);
    CHECK(parts[2].index == 5 && parts[2].logical && parts[2].first_block == 3063 && parts[2].block_count == 900 && parts[2].mbr_type == 0x06);
    CHECK(parts[3].index == 6 && parts[3].first_block == 4063 && parts[3].block_count == 800 && parts[3].bootable);
  }
  // Loops: an EBR linking to itself or backwards ends the chain.
  MemDisk loop(10000, bs);
  mbr_entry(loop.at(0), 0, 0, 0x05, 100, 5000);
  mbr_entry(loop.at(100), 0, 0, 0x83, 63, 100);
  mbr_entry(loop.at(100), 1, 0, 0x05, 0, 100);
  CHECK(scan_partitions(loop, a, &parts, &scheme) && parts.count() == 1 && parts[0].index == 5);
  // EFI system partition.
  MemDisk esp(1000, bs);
  mbr_entry(esp.at(0), 0, 0, 0xEF, 1, 100);
  CHECK(scan_partitions(esp, a, &parts, &scheme) && parts.count() == 1 && is_efi_system_partition(parts[0]));
  CHECK(a.live == 0);
}

static void test_gpt(uint32_t bs) {
  TestAllocator a, la; List<PartitionInfo> parts(la); PartitionScheme scheme;
  const uint64_t n = 4000;
  MemDisk d(n, bs);
  const uint64_t ab = 128 * 128 / bs, usable = 2 + ab;
  const GptSpec specs[] = {{kEfi, usable, usable + 99, kName1}, {kLinux, usable + 200, usable + 999, kName2}, {kLinux, usable + 1000, usable + 1999, nullptr}};
  make_gpt(d, specs, 3, 2);  // slots 1, 4, 5 are used
  CHECK(scan_partitions(d, a, &parts, &scheme) && scheme == PartitionScheme::Gpt && parts.count() == 3);
  if (parts.count() == 3) {
    CHECK(parts[0].index == 1 && parts[0].first_block == usable && parts[0].block_count == 100 && parts[0].name.equals("ESP"));
    CHECK(is_efi_system_partition(parts[0]) && !strcmp(partition_type_name(parts[0]), "EFI System"));
    CHECK(parts[1].index == 4 && parts[1].block_count == 800 && parts[1].name.equals("data \xC3\xA9\xF0\x9F\x98\x80") && !strcmp(partition_type_name(parts[1]), "Linux filesystem"));
    CHECK(parts[1].unique_guid.bytes[0] == 2 && !parts[1].unique_guid.is_zero());
    CHECK(parts[2].index == 5 && parts[2].name.empty());
  }
  // A damaged primary header: the backup supplies the table.
  d.at(1)[20] ^= 0xFF;
  CHECK(scan_partitions(d, a, &parts, &scheme) && scheme == PartitionScheme::Gpt && parts.count() == 3);
  d.at(1)[20] ^= 0xFF;
  // A damaged primary entry array.
  d.at(2)[0] ^= 0xFF;
  CHECK(scan_partitions(d, a, &parts, &scheme) && scheme == PartitionScheme::Gpt && parts.count() == 3 && is_efi_system_partition(parts[0]));
  // Both damaged: the protective MBR alone says nothing usable.
  d.at(n - 1)[20] ^= 0xFF;
  CHECK(scan_partitions(d, a, &parts, &scheme) && scheme == PartitionScheme::None && parts.count() == 0);
  d.at(n - 1)[20] ^= 0xFF; d.at(2)[0] ^= 0xFF;
  CHECK(scan_partitions(d, a, &parts, &scheme) && scheme == PartitionScheme::Gpt && parts.count() == 3);

  // An entry outside the usable area or backwards is skipped; the CRC still matches.
  MemDisk bad(n, bs);
  const GptSpec bad_specs[] = {{kEfi, 0, 50, nullptr}, {kLinux, usable + 10, usable + 5, nullptr}, {kLinux, usable + 20, n + 5, nullptr}, {kLinux, usable + 30, usable + 40, nullptr}};
  make_gpt(bad, bad_specs, 4);
  CHECK(scan_partitions(bad, a, &parts, &scheme) && scheme == PartitionScheme::Gpt && parts.count() == 1 && parts[0].index == 4);

  // Hybrid-ish: a protective entry next to a real MBR entry with the GPT broken falls back to the MBR.
  MemDisk hybrid(n, bs);
  mbr_entry(hybrid.at(0), 0, 0, 0xEE, 1, 100);
  mbr_entry(hybrid.at(0), 1, 0x80, 0x0B, 200, 300);
  CHECK(scan_partitions(hybrid, a, &parts, &scheme) && scheme == PartitionScheme::Mbr && parts.count() == 1 && parts[0].index == 2);

  // Device failure and memory failure are errors, not "no partitions".
  d.fail_reads = true;
  CHECK(!scan_partitions(d, a, &parts, &scheme) && parts.count() == 0);
  d.fail_reads = false;
  a.fail = true;
  CHECK(!scan_partitions(d, a, &parts, &scheme));
  a.fail = false;
  CHECK(a.live == 0);
}

static void test_partition_device() {
  TestAllocator a;
  MemDisk d(1000, 512);
  for (int i = 0; i < 1000; ++i) memset(d.at(i), i & 255, 512);
  PartitionStorageDevice p(d, 100, 50, 3);
  CHECK(!strcmp(p.name(), "memp3") && p.initialize());
  StorageInfo info = p.info();
  CHECK(info.block_count == 50 && info.block_size == 512);
  uint8_t buf[1024];
  CHECK(p.read(0, ByteView(buf)) && buf[0] == 100 && buf[512] == 101);
  CHECK(p.read(48, ByteView(buf)) && buf[0] == 148 && buf[512] == 149);
  CHECK(!p.read(49, ByteView(buf)));                     // runs one block past the end
  CHECK(!p.read(50, ByteView(buf, 512)) && !p.read(~uint64_t{0}, ByteView(buf, 512)));
  CHECK(p.read(50, ByteView(buf, 0)));                   // empty read at the end is fine
  CHECK(!p.read(0, ByteView(buf, 100)));                 // not whole blocks
  memset(buf, 0x5A, sizeof buf);
  CHECK(p.write(49, ConstByteView(buf, 512)) && d.at(149)[0] == 0x5A && d.at(150)[0] == 150);
  CHECK(!p.write(49, ConstByteView(buf, 1024)) && d.at(150)[0] == 150);
  CHECK(p.flush());
  StorageStatistics st = p.statistics();
  CHECK(st.reads == 3 && st.writes == 1 && st.errors == 5 && st.blocks_read == 4 && st.blocks_written == 1);
}

int main() {
  test_crc();
  for (uint32_t bs : {512u, 2048u}) { test_none(bs); test_mbr(bs); test_gpt(bs); }
  test_partition_device();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("partition tests passed\n");
  return 0;
}
