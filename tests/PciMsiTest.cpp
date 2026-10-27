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

// PciMsi against a simulated PCI function whose config space has an MSI capability (32- or 64-bit, with or without per-vector masking): the message that ends up in the
// capability, the enable and multiple-message bits, the command register's INTx disable, refusals, and that the status register is never written with ones.
#include <stdio.h>
#include <string.h>
#include "Hardware/Bus/Pci/PciMsi.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// One function with a config space of 64 dwords. The capability list starts at 0x40 with a power management capability (id 1) pointing at the MSI capability (id 5) at 0x50.
class FakeFunction final : public IPciBus {
 public:
  mutable uint32_t config[64] = {};
  mutable uint32_t status_writes_with_ones = 0;
  uint16_t msi_offset = 0x50;
  uint32_t writable_control_mask = 0x0071;   // enable and multiple message enable are the writable bits (the rest is read only)
  bool stuck_enable = false;                 // a function that will not switch MSI on

  explicit FakeFunction(bool is64, bool masking, bool with_msi = true) {
    config[1] = 0x00100000 | 0x0007;                         // status: capability list; command: io, memory, bus master
    config[0x34 / 4] = 0x40;
    config[0x40 / 4] = 0x01 | (with_msi ? (0x50u << 8) : 0u);   // power management -> MSI
    uint32_t control = (3u << 1) | (is64 ? 0x80u : 0u) | (masking ? 0x100u : 0u);   // asks for 8 vectors
    config[0x50 / 4] = 0x05 | (control << 16);
  }
  const char* name() const override { return "fake"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress, uint16_t offset) const override { return offset < 256 ? config[offset / 4] : 0; }
  void write32(PciAddress, uint16_t offset, uint32_t value) const override {
    if (offset >= 256) return;
    if (offset == 4) {
      if (value >> 16) ++status_writes_with_ones;
      config[1] = (config[1] & 0xFFFF0000) | (value & 0xFFFF);
      return;
    }
    if (offset == msi_offset) {
      uint32_t control = config[offset / 4] >> 16;
      control = (control & ~writable_control_mask) | (((value >> 16) & writable_control_mask) & ~(stuck_enable ? 1u : 0u));
      config[offset / 4] = (config[offset / 4] & 0xFFFF) | (control << 16);
      return;
    }
    config[offset / 4] = value;
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t id) const override {
    uint16_t offset = config[0x34 / 4] & 0xFC;
    for (int guard = 0; offset && guard < 8; ++guard) {
      const uint32_t header = config[offset / 4];
      if ((header & 0xFF) == id) return offset;
      offset = (header >> 8) & 0xFC;
    }
    return 0;
  }
  uint32_t control() const { return config[0x50 / 4] >> 16; }
};

static const PciAddress kFunction{1, 0, 0};

static void test_64_bit_with_masking() {
  FakeFunction f(true, true);
  f.config[0x60 / 4] = 0xFFFFFFFF;   // the mask bits (at +16 for a 64-bit function): all masked by the firmware
  PciMsi msi(f);
  CHECK(!msi.enabled());
  // x86: message address 0xFEE00000 | apic id 3 << 12, vector 0x51.
  CHECK(msi.enable(kFunction, 0xFEE03000, 0x0051) && msi.enabled());
  CHECK(f.config[0x54 / 4] == 0xFEE03000 && f.config[0x58 / 4] == 0 && f.config[0x5C / 4] == 0x51 && f.config[0x60 / 4] == 0);
  CHECK((f.control() & 1) == 1 && ((f.control() >> 4) & 7) == 0 && (f.control() & 0x180) == 0x180 && ((f.control() >> 1) & 7) == 3);   // on, one vector, read-only bits kept
  CHECK((f.config[1] & 0x0400) != 0 && (f.config[1] & 7) == 7 && (f.config[1] >> 16) == 0x10);   // INTx disabled, the rest of the command and the status kept
  CHECK(f.status_writes_with_ones == 0);
  CHECK(!msi.enable(kFunction, 0xFEE00000, 0x50));   // already on
  msi.disable();
  CHECK(!msi.enabled() && (f.control() & 1) == 0 && (f.config[1] & 0x0400) == 0 && f.status_writes_with_ones == 0);
  msi.disable();   // (twice is fine)
  // Again, with a high half of the address.
  CHECK(msi.enable(kFunction, 0x1'FEE0'1000ull, 0xAB));
  CHECK(f.config[0x54 / 4] == 0xFEE01000 && f.config[0x58 / 4] == 1 && f.config[0x5C / 4] == 0xAB);
}

static void test_32_bit() {
  FakeFunction f(false, false);
  PciMsi msi(f);
  CHECK(msi.enable(kFunction, 0xFEE00000, 0x50));
  CHECK(f.config[0x54 / 4] == 0xFEE00000 && f.config[0x58 / 4] == 0x50 && (f.control() & 1) == 1);
  msi.disable();
  // 32-bit function: an address above 4 GiB, an unaligned one.
  CHECK(!msi.enable(kFunction, 0x1'0000'0000ull, 0x50) && (f.control() & 1) == 0 && !msi.enabled());
  CHECK(!msi.enable(kFunction, 0xFEE00002, 0x50) && (f.control() & 1) == 0);
}

static void test_32_bit_masking() {
  FakeFunction f(false, true);
  f.config[0x5C / 4] = 0xFFFFFFFF;   // the mask bits (at +12 for a 32-bit function): all masked
  PciMsi msi(f);
  CHECK(msi.enable(kFunction, 0xFEE00000, 0x50));
  CHECK(f.config[0x5C / 4] == 0 && f.config[0x58 / 4] == 0x50);
}

static void test_refusals() {
  FakeFunction none(true, false, false);   // no MSI capability
  PciMsi a(none);
  CHECK(!a.enable(kFunction, 0xFEE00000, 0x50) && !a.enabled());
  FakeFunction stuck(true, false);
  stuck.stuck_enable = true;
  PciMsi b(stuck);
  CHECK(!b.enable(kFunction, 0xFEE00000, 0x50) && !b.enabled() && (stuck.control() & 1) == 0 && (stuck.config[1] & 0x0400) == 0);
  // Multiple message enable that was left set by the firmware is cleared.
  FakeFunction f(true, false);
  f.config[0x50 / 4] |= (3u << 4) << 16;
  PciMsi c(f);
  CHECK(c.enable(kFunction, 0xFEE00000, 0x50) && ((f.control() >> 4) & 7) == 0);
  // The destructor turns it off.
  {
    FakeFunction g(true, false);
    {
      PciMsi d(g);
      CHECK(d.enable(kFunction, 0xFEE00000, 0x50) && (g.control() & 1));
    }
    CHECK((g.control() & 1) == 0 && (g.config[1] & 0x0400) == 0);
  }
}

int main() {
  test_64_bit_with_masking();
  test_32_bit();
  test_32_bit_masking();
  test_refusals();
  printf(g_failed ? "PciMsiTest: %d FAILED\n" : "PciMsiTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
