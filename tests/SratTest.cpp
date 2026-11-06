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

// SRAT and SLIT parsing (Hardware/Acpi/Srat): processor and memory affinity entries of every kind, disabled and short entries, the table's own
// length, distances; then every truncation and every single-bit flip of the tables, which must parse without reading out of bounds.
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "Hardware/Acpi/Srat.hpp"

using namespace hardware;

void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  fprintf(stderr, "BOUNDS VIOLATION: %s (index %llu, size %llu)\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

namespace {

struct Table {
  std::vector<uint8_t> bytes;
  explicit Table(const char* signature, uint64_t after_header) {
    bytes.resize(36 + after_header);
    for (int i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(signature[i]);
  }
  void put(uint64_t at, uint64_t value, int size) {
    for (int i = 0; i < size; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
  }
  void entry(std::initializer_list<uint8_t> e) { bytes.insert(bytes.end(), e); }
  void append(uint8_t type, uint8_t length) {
    bytes.push_back(type);
    bytes.push_back(length);
    bytes.resize(bytes.size() + length - 2);
  }
  void finish() { put(4, bytes.size(), 4); }
  ConstByteView view() const { return ConstByteView(bytes.data(), bytes.size()); }
};

Table make_srat() {
  Table t("SRAT", 12);
  t.put(36, 1, 4);
  // local APIC 3 on node 0x020101 (low byte 1, high bytes 0x01 0x02), enabled
  uint64_t at = t.bytes.size();
  t.append(0, 16);
  t.bytes[at + 2] = 0x01; t.bytes[at + 3] = 3; t.put(at + 4, 1, 4); t.bytes[at + 9] = 0x01; t.bytes[at + 10] = 0x02;
  // local APIC 4, disabled
  at = t.bytes.size();
  t.append(0, 16);
  t.bytes[at + 3] = 4;
  // memory: node 1, 4 GiB at 4 GiB, enabled + hot pluggable
  at = t.bytes.size();
  t.append(1, 40);
  t.put(at + 2, 1, 4); t.put(at + 8, 0x1'0000'0000ull, 8); t.put(at + 16, 0x1'0000'0000ull, 8); t.put(at + 28, 3, 4);
  // memory: disabled
  at = t.bytes.size();
  t.append(1, 40);
  t.put(at + 16, 0x1000, 8);
  // an unknown type
  t.append(9, 6);
  // a processor entry shorter than its type: skipped
  at = t.bytes.size();
  t.append(0, 8);
  t.put(at + 4, 1, 4);
  // x2APIC 0x123 on node 7, enabled
  at = t.bytes.size();
  t.append(2, 24);
  t.put(at + 4, 7, 4); t.put(at + 8, 0x123, 4); t.put(at + 12, 1, 4);
  // GICC with UID 5 on node 2, enabled
  at = t.bytes.size();
  t.append(3, 18);
  t.put(at + 2, 2, 4); t.put(at + 6, 5, 4); t.put(at + 10, 1, 4);
  t.finish();
  return t;
}

Table make_slit() {
  Table t("SLIT", 8);
  t.put(36, 3, 8);
  const uint8_t matrix[9] = {10, 21, 31, 21, 10, 21, 31, 21, 10};
  t.bytes.insert(t.bytes.end(), matrix, matrix + 9);
  t.finish();
  return t;
}

void test_srat() {
  const Table t = make_srat();
  SratCpu cpus[8];
  CHECK(srat_cpus(t.view(), ArrayView<SratCpu>(cpus, 8)) == 3);
  CHECK(cpus[0].kind == SratCpuId::LocalApic && cpus[0].id == 3 && cpus[0].node == 0x020101);
  CHECK(cpus[1].kind == SratCpuId::X2Apic && cpus[1].id == 0x123 && cpus[1].node == 7);
  CHECK(cpus[2].kind == SratCpuId::Gicc && cpus[2].id == 5 && cpus[2].node == 2);
  // A small array: the count still says how many.
  SratCpu one[1];
  CHECK(srat_cpus(t.view(), ArrayView<SratCpu>(one, 1)) == 3 && one[0].id == 3);
  CHECK(srat_cpus(t.view(), ArrayView<SratCpu>()) == 3);

  SratMemory memory[4];
  CHECK(srat_memory(t.view(), ArrayView<SratMemory>(memory, 4)) == 1);
  CHECK(memory[0].node == 1 && memory[0].base == 0x1'0000'0000ull && memory[0].length == 0x1'0000'0000ull && memory[0].hot_pluggable && !memory[0].non_volatile);

  // Not an SRAT, too short, a length past the view or under the header.
  Table wrong = make_srat();
  wrong.bytes[0] = 'X';
  CHECK(srat_cpus(wrong.view(), ArrayView<SratCpu>(cpus, 8)) == 0);
  CHECK(srat_cpus(t.view().first(20), ArrayView<SratCpu>(cpus, 8)) == 0);
  Table long_length = make_srat();
  long_length.put(4, long_length.bytes.size() + 1, 4);
  CHECK(srat_cpus(long_length.view(), ArrayView<SratCpu>(cpus, 8)) == 0);
  Table short_length = make_srat();
  short_length.put(4, 10, 4);
  CHECK(srat_cpus(short_length.view(), ArrayView<SratCpu>(cpus, 8)) == 0);
  // The table's own length ends it before the view does: only the first processor entry is inside.
  Table cut = make_srat();
  cut.put(4, 48 + 16, 4);
  CHECK(srat_cpus(cut.view(), ArrayView<SratCpu>(cpus, 8)) == 1);
  // An entry of length 0 or 1 ends the list.
  Table stuck = make_srat();
  stuck.bytes[48 + 1] = 0;
  CHECK(srat_cpus(stuck.view(), ArrayView<SratCpu>(cpus, 8)) == 0);
}

void test_slit() {
  const Table t = make_slit();
  CHECK(slit_distance(t.view(), 0, 0) == 10);
  CHECK(slit_distance(t.view(), 0, 2) == 31);
  CHECK(slit_distance(t.view(), 2, 1) == 21);
  CHECK(slit_distance(t.view(), 3, 0) == 0);
  CHECK(slit_distance(t.view(), 0, 3) == 0);
  Table huge = make_slit();
  huge.put(36, 0xFFFFFFFFFFFFFFFFull, 8);
  CHECK(slit_distance(huge.view(), 0, 0) == 0);
  Table big = make_slit();
  big.put(36, 0x1'0000'0000ull, 8);   // the square wraps 64 bits to 0: still refused
  CHECK(slit_distance(big.view(), 0, 1) == 0);
  Table four = make_slit();
  four.put(36, 4, 8);                 // 16 bytes needed, 9 there
  CHECK(slit_distance(four.view(), 0, 1) == 0);
  CHECK(slit_distance(make_srat().view(), 0, 0) == 0);
}

// Every prefix of the bytes (with the table's length left as it was, and set to the prefix) and every single-bit flip: no read out of bounds
// (ASan, the checked views), no more entries than the intact table has room for.
void test_damage() {
  const Table tables[2] = {make_srat(), make_slit()};
  SratCpu cpus[16];
  SratMemory memory[16];
  for (const Table& original : tables) {
    for (uint64_t size = 0; size <= original.bytes.size(); ++size) {
      for (int fix_length = 0; fix_length < 2; ++fix_length) {
        Table t = original;
        t.bytes.resize(size);
        if (fix_length && size >= 8) t.put(4, size, 4);
        CHECK(srat_cpus(t.view(), ArrayView<SratCpu>(cpus, 16)) <= 16);
        CHECK(srat_memory(t.view(), ArrayView<SratMemory>(memory, 16)) <= 16);
        for (uint32_t a = 0; a < 4; ++a)
          for (uint32_t b = 0; b < 4; ++b) (void)slit_distance(t.view(), a, b);
      }
    }
    for (uint64_t byte = 0; byte < original.bytes.size(); ++byte) {
      for (int bit = 0; bit < 8; ++bit) {
        Table t = original;
        t.bytes[byte] ^= static_cast<uint8_t>(1u << bit);
        CHECK(srat_cpus(t.view(), ArrayView<SratCpu>(cpus, 16)) <= 16);
        CHECK(srat_memory(t.view(), ArrayView<SratMemory>(memory, 16)) <= 16);
        for (uint32_t a = 0; a < 4; ++a)
          for (uint32_t b = 0; b < 4; ++b) (void)slit_distance(t.view(), a, b);
      }
    }
  }
}

}  // namespace

int main() {
  test_srat();
  test_slit();
  test_damage();
  if (g_failed) {
    printf("SratTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("SratTest: all passed\n");
  return 0;
}
