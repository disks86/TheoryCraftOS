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

// AmdPanelPower against an AtomBIOS image whose UNIPHYTransmitterControl table (76) hands its parameters to a register of the simulated card: the action in byte 3 of the first
// parameter word, nothing else set; the wait for the hot plug pin (one sense per millisecond, 300 at most), a pin that shows the panel at once or never, a connector without a pin,
// and an image without the table.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdPanelPower.hpp"
#include "AtomBiosImage.hpp"

using namespace hardware;
using namespace atomimage;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

struct Host final : IAtomHost, IHpdSense {
  static constexpr uint32_t kParams0 = 0x7200, kParams1 = 0x7201;
  uint32_t params0 = 0xDEAD, params1 = 0xBEEF, runs = 0;
  uint64_t delayed_us = 0, clock = 0;
  uint32_t senses = 0, sensed_pin = 0xFF, shows_after = 0;    // the panel shows on the pin at the shows_after-th sense (0: never)

  uint32_t read_register(uint32_t) override { return 0; }
  void write_register(uint32_t reg, uint32_t value) override {
    if (reg == kParams0) { params0 = value; ++runs; }
    if (reg == kParams1) params1 = value;
  }
  uint32_t read_mc(uint32_t) override { return 0; }
  void write_mc(uint32_t, uint32_t) override {}
  uint32_t read_pll(uint32_t) override { return 0; }
  void write_pll(uint32_t, uint32_t) override {}
  void delay_us(uint32_t n) override { delayed_us += n; }
  void delay_ms(uint32_t n) override { delayed_us += uint64_t{n} * 1000; }
  uint64_t now_us() override { return ++clock; }
  bool hpd_sense(uint32_t pin) override {
    sensed_pin = pin;
    ++senses;
    return shows_after != 0 && senses >= shows_after;
  }
};

struct Rig {
  TestAllocator alloc;
  Host host;
  std::vector<uint8_t> image;
  std::unique_ptr<AtomBios> bios;
  explicit Rig(bool with_table = true) {
    Image img;
    Code c;
    if (with_table) {
      c.move(REG, Host::kParams0, PS, 0);
      c.move(REG, Host::kParams1, PS, 1);
    }
    c.eot();
    img.tables.push_back({with_table ? AmdPanelPower::kCommandTable : 0, 8, 8, c});
    image = img.build();
    bios.reset(new AtomBios(host, ConstByteView(image.data(), image.size()), alloc));
  }
};

static void test_run() {
  Rig r;
  CHECK(r.bios->valid());
  CHECK(AmdPanelPower::run(*r.bios, AmdPanelPower::kActionPowerOn));
  CHECK(r.host.runs == 1 && r.host.params0 == 0x0C000000u && r.host.params1 == 0);       // ucAction = 12 in byte 3, usPixelClock and ucConfig 0
  CHECK(AmdPanelPower::power_off(*r.bios));
  CHECK(r.host.runs == 2 && r.host.params0 == 0x0D000000u && r.host.params1 == 0);
  CHECK(r.host.senses == 0 && r.host.delayed_us == 0);
}

static void test_wait() {
  // the panel shows at once: no wait
  Rig a;
  a.host.shows_after = 1;
  CHECK(AmdPanelPower::power_on(a.host, *a.bios, a.host, 2));
  CHECK(a.host.runs == 1 && a.host.params0 == 0x0C000000u && a.host.senses == 1 && a.host.sensed_pin == 2 && a.host.delayed_us == 0);
  // after 5 senses: 4 milliseconds waited
  Rig b;
  b.host.shows_after = 5;
  CHECK(AmdPanelPower::power_on(b.host, *b.bios, b.host, 0));
  CHECK(b.host.senses == 5 && b.host.sensed_pin == 0 && b.host.delayed_us == 4000);
  // after the last sense there is: the 300th
  Rig c;
  c.host.shows_after = AmdPanelPower::kSenseTries;
  CHECK(AmdPanelPower::power_on(c.host, *c.bios, c.host, 1));
  CHECK(c.host.senses == 300 && c.host.delayed_us == 299000);
  // one too late: the driver gives up after 300 ms (and so do we, having tried the pin 300 times)
  Rig d;
  d.host.shows_after = AmdPanelPower::kSenseTries + 1;
  CHECK(!AmdPanelPower::power_on(d.host, *d.bios, d.host, 1));
  CHECK(d.host.senses == 300 && d.host.delayed_us == 300000 && d.host.runs == 1);
  // never
  Rig e;
  CHECK(!AmdPanelPower::power_on(e.host, *e.bios, e.host, 5));
  CHECK(e.host.senses == 300 && e.host.sensed_pin == 5 && e.host.delayed_us == 300000);
}

static void test_no_pin_and_no_table() {
  // no hot plug pin: powered, nothing to wait on
  Rig a;
  CHECK(AmdPanelPower::power_on(a.host, *a.bios, a.host, AmdPanelPower::kNoHpd));
  CHECK(a.host.runs == 1 && a.host.senses == 0 && a.host.delayed_us == 0);
  // an image without the table: fails, and does not wait
  Rig b(false);
  CHECK(!AmdPanelPower::run(*b.bios, AmdPanelPower::kActionPowerOn));
  CHECK(!AmdPanelPower::power_on(b.host, *b.bios, b.host, 1));
  CHECK(b.host.runs == 0 && b.host.senses == 0 && b.host.delayed_us == 0);
}

int main() {
  test_run();
  test_wait();
  test_no_pin_and_no_table();
  printf(g_failed ? "AmdPanelPowerTest: %d FAILED\n" : "AmdPanelPowerTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
