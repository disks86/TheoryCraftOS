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

// AmdAsicReset against a simulated card behind a simulated PCI function: the order of vi_asic_pci_config_reset's steps (the hung flag in the BIOS scratch register, bus mastering
// off, the magic value in configuration dword 0x7c, 100 us, the wait for CONFIG_MEMSIZE, bus mastering on, the flag cleared), a card that takes a while, a card that never comes
// back, and what is left alone (other bits of the scratch register and the command register, the status register).
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdAsicReset.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct World;

struct Clock final : IClock {
  mutable uint64_t now = 0;
  uint64_t now_us() const override { now += 10; return now; }
  const char* name() const override { return "clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

struct World {
  Clock clock;
  std::vector<std::string> log;          // what happened, in order
  uint32_t scratch = 0x00000123;         // BIOS_SCRATCH_3
  uint32_t command = 0x0407;             // io, memory, bus master, and the INTx disable bit
  uint32_t status = 0x0010;
  uint32_t status_writes_with_ones = 0;
  bool in_reset = false;
  uint32_t reads_in_reset = 3;           // CONFIG_MEMSIZE reads all ones this many times after the reset
  bool never_returns = false;
  uint32_t memsize_reads = 0, reads_left = 0;
  uint64_t reset_time = 0, first_read_time = 0;
  uint32_t pci_writes_to_other_offsets = 0;
};

struct Registers final : IVcnRegisters {
  World& w;
  explicit Registers(World& world) : w(world) {}
  uint32_t read(uint32_t r) override {
    if (r == 0x5cc) return w.scratch;
    if (r == 0x150a) {
      ++w.memsize_reads;
      if (w.first_read_time == 0) w.first_read_time = w.clock.now;
      w.log.push_back("memsize");
      if (w.in_reset && (w.never_returns || w.reads_left > 0)) {
        if (w.reads_left > 0) --w.reads_left;
        return 0xFFFFFFFFu;
      }
      w.in_reset = false;
      return 0x40000000;
    }
    return 0;
  }
  void write(uint32_t r, uint32_t v) override {
    if (r == 0x5cc) {
      w.log.push_back((v & 0x20000000) ? "hung set" : "hung clear");
      w.scratch = v;
    }
  }
};

struct Pci final : IPciBus {
  World& w;
  explicit Pci(World& world) : w(world) {}
  const char* name() const override { return "pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress, uint16_t offset) const override { return offset == 4 ? (w.command | (w.status << 16)) : 0; }
  void write32(PciAddress, uint16_t offset, uint32_t value) const override {
    if (offset == 4) {
      if (value >> 16) ++w.status_writes_with_ones;
      const bool was = (w.command & 4) != 0, is = (value & 4) != 0;
      w.command = value & 0xFFFF;
      if (was && !is) w.log.push_back("bus master off");
      if (!was && is) w.log.push_back("bus master on");
    } else if (offset == 0x7c) {   // (the dword the driver writes: spelled out, not the constant under test)
      w.log.push_back(value == 0x39d5e86b ? "reset" : "reset with the wrong value");
      w.in_reset = true;
      w.reads_left = w.reads_in_reset;
      w.reset_time = w.clock.now;
    } else {
      ++w.pci_writes_to_other_offsets;
    }
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

static const PciAddress kFunction{1, 0, 0};

static void test_reset() {
  World w;
  Registers regs(w);
  Pci pci(w);
  CHECK(AmdAsicReset::pci_config_reset(regs, w.clock, pci, kFunction) == AmdAsicReset::Result::Ok);
  // The steps, in order: flag, bus master off, reset, the card answers all ones three times and then for real, bus master on, flag off.
  const std::vector<std::string> expected = {"hung set", "bus master off", "reset", "memsize", "memsize", "memsize", "memsize", "bus master on", "hung clear"};
  CHECK(w.log == expected);
  // At least 100 us before the first look at the card.
  CHECK(w.first_read_time - w.reset_time >= 100);
  // Everything else is as it was: the other scratch bits, the other command bits, the status register.
  CHECK(w.scratch == 0x00000123 && w.command == 0x0407 && w.status == 0x0010 && w.status_writes_with_ones == 0 && w.pci_writes_to_other_offsets == 0);
  CHECK(AmdAsicReset::result_name(AmdAsicReset::Result::Ok) && AmdAsicReset::result_name(AmdAsicReset::Result::NotComingBack));
}

static void test_immediate_and_slow() {
  World w;
  w.reads_in_reset = 0;
  Registers regs(w);
  Pci pci(w);
  CHECK(AmdAsicReset::pci_config_reset(regs, w.clock, pci, kFunction) == AmdAsicReset::Result::Ok && w.memsize_reads == 1);
  World s;
  s.reads_in_reset = 5000;   // 5000 reads at 10 us a time: 50 ms, within the 100 ms
  Registers sregs(s);
  Pci spci(s);
  CHECK(AmdAsicReset::pci_config_reset(sregs, s.clock, spci, kFunction) == AmdAsicReset::Result::Ok && s.memsize_reads > 5000 && (s.command & 4));
  World late;
  late.reads_in_reset = 20000;   // 200 ms: too long for the default
  Registers lregs(late);
  Pci lpci(late);
  CHECK(AmdAsicReset::pci_config_reset(lregs, late.clock, lpci, kFunction) == AmdAsicReset::Result::NotComingBack);
  // ...but a longer timeout waits for it.
  World longer;
  longer.reads_in_reset = 20000;
  Registers gregs(longer);
  Pci gpci(longer);
  CHECK(AmdAsicReset::pci_config_reset(gregs, longer.clock, gpci, kFunction, 400000) == AmdAsicReset::Result::Ok);
}

static void test_never_back() {
  World w;
  w.never_returns = true;
  Registers regs(w);
  Pci pci(w);
  CHECK(AmdAsicReset::pci_config_reset(regs, w.clock, pci, kFunction, 2000) == AmdAsicReset::Result::NotComingBack);
  // Bus mastering stays off (a card that is not answering must not write to memory), the hung flag is cleared all the same, and the wait ended at the timeout.
  CHECK(!(w.command & 4) && w.command == (0x0407 & ~4u) && !(w.scratch & 0x20000000) && w.scratch == 0x123);
  CHECK(w.log.front() == "hung set" && w.log.back() == "hung clear" && w.log[1] == "bus master off" && w.log[2] == "reset");
  const std::string last_but_one = w.log[w.log.size() - 2];
  CHECK(last_but_one == "memsize" && w.memsize_reads >= 150 && w.memsize_reads <= 250);   // (2000 us in steps of 10 us, minus the 100 us wait)
  CHECK(w.status_writes_with_ones == 0);
}

int main() {
  test_reset();
  test_immediate_and_slow();
  test_never_back();
  printf(g_failed ? "AmdAsicResetTest: %d FAILED\n" : "AmdAsicResetTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
