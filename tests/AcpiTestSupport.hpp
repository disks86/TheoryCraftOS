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

// What the ACPI tests share: a pretend chipset (FADT, PM1 control, GPE registers, embedded controller), an allocator, and a few functions that assemble AML byte by byte.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>
#include <map>
#include <vector>
#include "Hardware/Acpi/AcpiEvents.hpp"

using namespace acpi;
using namespace hardware;

#ifndef CHECK
static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)
#endif

struct TestAllocator final : allocators::IAllocator {
  std::vector<void*> blocks;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); blocks.push_back(p); return p; }
  void deallocate(void*) override {}
  ~TestAllocator() { for (void* p : blocks) free(p); }
};

struct FakeHost final : IAcpiHost {
  uint16_t ec_data = 0, ec_command = 0;
  std::vector<uint8_t> ec_events;          // the events the controller holds, oldest first
  bool ec_stuck = false;                   // it always claims an event, and says it is number 0x77
  uint32_t ec_queries = 0;
  void configure_ec(uint16_t data, uint16_t command) override { ec_data = data; ec_command = command; }
  bool ec_status(uint8_t* value) override { *value = (!ec_events.empty() || ec_stuck) ? 0x20 : 0; return true; }
  bool ec_query(uint8_t* event) override {
    ++ec_queries;
    if (ec_stuck) { *event = 0x77; return true; }
    *event = ec_events.empty() ? 0 : ec_events.front();
    if (!ec_events.empty()) ec_events.erase(ec_events.begin());
    return true;
  }
  bool region_read(const RegionAccess&, uint64_t*) override { return false; }
  bool region_write(const RegionAccess&, uint64_t) override { return false; }
};

// The chipset: I/O ports as a map; the GPE status registers clear by writing ones; SMI_CMD with ACPI_ENABLE sets SCI_EN.
struct FakePorts final : IAcpiPorts {
  static constexpr uint16_t kPm1Event = 0x1800, kPm1Control = 0x1804, kSmiCommand = 0xB2, kGpe0 = 0x1860;
  static constexpr uint32_t kGpe0Registers = 16;
  std::map<uint16_t, uint8_t> reg;
  bool scien_follows_smi = true;
  uint32_t smi_writes = 0, delays = 0;
  uint8_t in8(uint16_t port) override { return reg[port]; }
  void out8(uint16_t port, uint8_t value) override {
    if (port == kSmiCommand) {
      ++smi_writes;
      if (value == 0xA0 && scien_follows_smi) reg[kPm1Control] |= 1;
      return;
    }
    if (port >= kGpe0 && port < kGpe0 + kGpe0Registers) { reg[port] &= static_cast<uint8_t>(~value); return; }   // status: write one to clear
    reg[port] = value;
  }
  uint16_t in16(uint16_t port) override { return static_cast<uint16_t>(reg[port] | (reg[port + 1] << 8)); }
  void out16(uint16_t port, uint16_t value) override {
    if (port == kPm1Event) {                           // PM1 status: write one to clear
      reg[port] &= static_cast<uint8_t>(~value);
      reg[port + 1] &= static_cast<uint8_t>(~(value >> 8));
      return;
    }
    reg[port] = static_cast<uint8_t>(value);
    reg[port + 1] = static_cast<uint8_t>(value >> 8);
  }
  void delay_us(uint32_t) override { ++delays; }
  // memory registers: a block of GPE status/enable bytes (status ones clear when written) at kMemGpe, and PM1 at kMemPm1 as 16 bit registers
  static constexpr uint64_t kMemGpe = 0x80000000ull, kMemPm1 = 0x80001000ull;
  std::map<uint64_t, uint8_t> mem;
  uint8_t mem_in8(uint64_t a) override { return mem[a]; }
  void mem_out8(uint64_t a, uint8_t v) override {
    if (a >= kMemGpe && a < kMemGpe + kGpe0Registers) mem[a] &= static_cast<uint8_t>(~v);
    else mem[a] = v;
  }
  uint16_t mem_in16(uint64_t a) override { return static_cast<uint16_t>(mem[a] | (mem[a + 1] << 8)); }
  void mem_out16(uint64_t a, uint16_t v) override {
    if (a == kMemPm1) { mem[a] &= static_cast<uint8_t>(~v); mem[a + 1] &= static_cast<uint8_t>(~(v >> 8)); return; }
    mem[a] = static_cast<uint8_t>(v);
    mem[a + 1] = static_cast<uint8_t>(v >> 8);
  }
  void raise(uint32_t gpe) { reg[kGpe0 + gpe / 8] |= static_cast<uint8_t>(1u << (gpe % 8)); }
  bool status(uint32_t gpe) { return reg[kGpe0 + gpe / 8] & (1u << (gpe % 8)); }
  bool enabled(uint32_t gpe) { return reg[kGpe0 + kGpe0Registers + gpe / 8] & (1u << (gpe % 8)); }
};

// The PCI configuration space: a root port at 00:1c.4 (a bridge with secondary bus 5) and the controller at 05:00.0.
struct FakePci final : IPciBus {
  const char* name() const override { return "fake pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 0; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress a, uint16_t offset) const override {
    if (a.bus == 0 && a.device == 0x1C && a.function == 4) {
      if (offset == 0x0C) return 1u << 16;
      if (offset == 0x18) return (5u << 8) | 0;
    }
    return 0;
  }
  void write32(PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

typedef std::vector<uint8_t> Bytes;
static Bytes cat(std::initializer_list<Bytes> parts) { Bytes r; for (const Bytes& p : parts) r.insert(r.end(), p.begin(), p.end()); return r; }
static Bytes b(std::initializer_list<uint8_t> v) { return Bytes(v); }
static Bytes seg(const char* s) { Bytes r(4, '_'); for (int i = 0; i < 4 && s[i]; ++i) r[i] = static_cast<uint8_t>(s[i]); return r; }
static Bytes path(const char* s) {
  Bytes r;
  if (*s == '\\') { r.push_back(0x5C); ++s; }
  std::vector<Bytes> segs;
  while (*s) { char t[5] = {}; int n = 0; while (*s && *s != '.') t[n++] = *s++; segs.push_back(seg(t)); if (*s == '.') ++s; }
  if (segs.size() == 2) r.push_back(0x2E);
  else if (segs.size() > 2) { r.push_back(0x2F); r.push_back(static_cast<uint8_t>(segs.size())); }
  for (const Bytes& sg : segs) r.insert(r.end(), sg.begin(), sg.end());
  return r;
}
static Bytes pkg(const Bytes& body) {
  Bytes r;
  const uint32_t n = static_cast<uint32_t>(body.size());
  if (n + 1 < 64) r.push_back(static_cast<uint8_t>(n + 1));
  else { const uint32_t t = n + 2; r.push_back(static_cast<uint8_t>(0x40 | (t & 0xF))); r.push_back(static_cast<uint8_t>(t >> 4)); }
  r.insert(r.end(), body.begin(), body.end());
  return r;
}
static Bytes num(uint64_t v) {
  if (v == 0) return b({0x00});
  if (v == 1) return b({0x01});
  if (v <= 0xFF) return b({0x0A, static_cast<uint8_t>(v)});
  if (v <= 0xFFFF) return b({0x0B, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)});
  return b({0x0C, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)});
}
static Bytes name_def(const char* n, const Bytes& value) { return cat({b({0x08}), path(n), value}); }
static Bytes method(const char* n, int argc, const Bytes& body) { return cat({b({0x14}), pkg(cat({path(n), b({static_cast<uint8_t>(argc)}), body}))}); }
static Bytes device(const char* n, const Bytes& body) { return cat({b({0x5B, 0x82}), pkg(cat({path(n), body}))}); }
static Bytes scope(const char* n, const Bytes& body) { return cat({b({0x10}), pkg(cat({path(n), body}))}); }
static Bytes package(uint8_t count, const Bytes& elements) { return cat({b({0x12}), pkg(cat({b({count}), elements}))}); }
static Bytes store(const Bytes& src, const Bytes& dst) { return cat({b({0x70}), src, dst}); }
static Bytes notify(const char* target, uint8_t value) { return cat({b({0x86}), path(target), num(value)}); }
static Bytes make_table(const char* signature, const Bytes& aml) {
  Bytes t(36, 0);
  memcpy(&t[0], signature, 4);
  const uint32_t length = static_cast<uint32_t>(36 + aml.size());
  for (int i = 0; i < 4; ++i) t[4 + i] = static_cast<uint8_t>(length >> (8 * i));
  t[8] = 2;
  t.insert(t.end(), aml.begin(), aml.end());
  return t;
}
static void put32(Bytes& t, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) t[at + i] = static_cast<uint8_t>(v >> (8 * i)); }

static Bytes make_fadt() {
  Bytes t(244, 0);
  memcpy(&t[0], "FACP", 4);
  put32(t, 4, 244);
  t[8] = 5;
  t[46] = 9;                                     // SCI_INT
  put32(t, 48, FakePorts::kSmiCommand);
  t[52] = 0xA0;                                  // ACPI_ENABLE
  t[53] = 0xA1;
  put32(t, 56, FakePorts::kPm1Event);
  t[88] = 4;                                     // PM1_EVT_LEN
  put32(t, 64, FakePorts::kPm1Control);
  put32(t, 80, FakePorts::kGpe0);
  t[92] = FakePorts::kGpe0Registers * 2;         // GPE0_BLK_LEN
  return t;
}

