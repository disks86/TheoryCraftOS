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

// Host-side tests for the I/O APIC route of the SCI: the MADT parsers (with truncation and bit flips) and the redirection entries against a model of the register pair.
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Hardware/Acpi/IoApic.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct FakeWindow final : IIoApicWindow {
  uint32_t regs[0x40] = {};
  uint32_t writes = 0;
  uint32_t read(uint8_t index) override { return index < 0x40 ? regs[index] : 0; }
  void write(uint8_t index, uint32_t value) override { ++writes; if (index < 0x40) regs[index] = value; }
};

typedef std::vector<uint8_t> Bytes;
static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }

static Bytes make_madt() {
  Bytes t(36, 0);
  memcpy(&t[0], "APIC", 4);
  put32(t, 0); put32(t, 1);                                    // local APIC address, flags
  t.push_back(0); t.push_back(8); t.push_back(0); t.push_back(0); t.push_back(1); t.push_back(0); t.push_back(0); t.push_back(0);   // a processor (type 0)
  t.push_back(1); t.push_back(12); t.push_back(2); t.push_back(0); put32(t, 0xFEC00000u); put32(t, 0);                  // I/O APIC 2 at 0xFEC00000, GSI 0
  t.push_back(1); t.push_back(12); t.push_back(3); t.push_back(0); put32(t, 0xFEC01000u); put32(t, 24);                 // another from GSI 24
  t.push_back(2); t.push_back(10); t.push_back(0); t.push_back(0); put32(t, 2); t.push_back(0); t.push_back(0);        // IRQ 0 -> GSI 2
  t.push_back(2); t.push_back(10); t.push_back(0); t.push_back(9); put32(t, 9); t.push_back(0x0F); t.push_back(0);     // IRQ 9 -> GSI 9, active low, level
  const uint32_t length = static_cast<uint32_t>(t.size());
  for (int i = 0; i < 4; ++i) t[4 + i] = static_cast<uint8_t>(length >> (8 * i));
  return t;
}

int main() {
  const Bytes madt = make_madt();
  IoApicInfo infos[4];
  CHECK(madt_ioapics(ConstByteView(madt.data(), madt.size()), ArrayView<IoApicInfo>(infos, 4)) == 2);
  CHECK(infos[0].id == 2 && infos[0].address == 0xFEC00000u && infos[0].gsi_base == 0 && infos[1].id == 3 && infos[1].address == 0xFEC01000u && infos[1].gsi_base == 24);
  IoApicInfo one[1];
  CHECK(madt_ioapics(ConstByteView(madt.data(), madt.size()), ArrayView<IoApicInfo>(one, 1)) == 2 && one[0].id == 2);       // counted, not stored
  SciRoute r = madt_sci_route(ConstByteView(madt.data(), madt.size()), 9);
  CHECK(r.gsi == 9 && r.level && r.active_low);
  r = madt_sci_route(ConstByteView(madt.data(), madt.size()), 0);
  CHECK(r.gsi == 2 && !r.level && !r.active_low);                                                                          // (conforms: ISA, edge, high)
  r = madt_sci_route(ConstByteView(madt.data(), madt.size()), 11);
  CHECK(r.gsi == 11 && !r.level && !r.active_low);                                                                         // no override: the same number
  Bytes wrong = madt;
  wrong[0] = 'X';
  CHECK(madt_ioapics(ConstByteView(wrong.data(), wrong.size()), ArrayView<IoApicInfo>(infos, 4)) == 0 && madt_sci_route(ConstByteView(wrong.data(), wrong.size()), 9).gsi == 9);
  for (size_t cut = 0; cut <= madt.size(); ++cut) {                                                                          // truncated everywhere
    madt_ioapics(ConstByteView(madt.data(), cut), ArrayView<IoApicInfo>(infos, 4));
    madt_sci_route(ConstByteView(madt.data(), cut), 9);
  }
  for (size_t bit = 0; bit < madt.size() * 8; ++bit) {                                                                       // every bit flipped
    Bytes f = madt;
    f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    madt_ioapics(ConstByteView(f.data(), f.size()), ArrayView<IoApicInfo>(infos, 4));
    madt_sci_route(ConstByteView(f.data(), f.size()), 9);
  }

  FakeWindow window;
  window.regs[1] = (23u << 16) | 0x20;                           // 24 inputs
  IoApic apic(window, 0);
  CHECK(apic.inputs() == 24 && apic.contains(9) && apic.contains(23) && !apic.contains(24));
  CHECK(apic.route(9, 0x53, 5, true, true, true));
  CHECK(window.regs[0x10 + 18] == (0x53u | (1u << 15) | (1u << 13) | (1u << 16)) && window.regs[0x10 + 19] == (5u << 24) && apic.masked(9));
  CHECK(apic.mask(9, false) && !apic.masked(9) && (window.regs[0x10 + 18] & 0xFF) == 0x53 && (window.regs[0x10 + 18] & (1u << 15)));
  CHECK(apic.mask(9, true) && apic.masked(9));
  CHECK(!apic.route(24, 0x53, 0, true, true, true) && !apic.route(9, 0x53, 256, true, true, true) && !apic.mask(30, false) && apic.masked(30));
  IoApic second(window, 24);
  CHECK(second.contains(24) && !second.contains(23 + 24 + 1) && second.contains(24 + 23));
  if (g_failed) { printf("IoApicTest: %d FAILED\n", g_failed); return 1; }
  printf("IoApicTest: all passed\n");
  return 0;
}
