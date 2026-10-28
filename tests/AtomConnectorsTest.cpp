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

// AtomConnectors against hand-assembled video BIOS images: the object header with its display path, connector and GPIO tables at literal offsets (not the parser's constants), a board
// like the XPS 9575's (eDP, HDMI, two DisplayPorts with the hot plug pins on DC_GPIO_HPD_A), the skipped devices and unknown objects, records before the hot plug record, connectors
// with several paths, the capacity, the names, and every truncation and every single bit flip of the image under the sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AtomConnectors.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {
// The layout: ROM header at 0x100, master data table at 0x200 (Object_Header, entry 22, at 0x230 -> 0x400; GPIO_Pin_LUT, entry 12, at 0x21C -> 0x300), the object header at 0x400 with
// the path table at +0x40, the connector object table at +0x100 and the records from +0x180.
constexpr uint32_t kRom = 0x100, kMaster = 0x200, kGpio = 0x300, kHeader = 0x400, kPaths = kHeader + 0x40, kObjects = kHeader + 0x100, kRecords = kHeader + 0x180;

struct Img {
  std::vector<uint8_t> b = std::vector<uint8_t>(0x800, 0);
  void u8(uint32_t at, uint32_t v) { b[at] = static_cast<uint8_t>(v); }
  void u16(uint32_t at, uint32_t v) { u8(at, v); u8(at + 1, v >> 8); }
  uint32_t path_at = kPaths + 4, path_count = 0, object_at = kObjects + 4, object_count = 0, record_at = kRecords, gpio_count = 0;

  Img(uint32_t device_support = 0x0FFF, uint32_t crev = 2) {
    u8(0, 0x55); u8(1, 0xAA);
    u16(0x48, kRom);
    memcpy(&b[kRom + 4], "ATOM", 4);
    u16(kRom + 0x20, kMaster);
    u16(kMaster + 4 + 22 * 2, kHeader);
    u16(kMaster + 4 + 12 * 2, kGpio);
    u16(kHeader, 0x20); u8(kHeader + 2, 1); u8(kHeader + 3, crev);
    u16(kHeader + 4, device_support);
    u16(kHeader + 6, kObjects - kHeader);
    u16(kHeader + 14, kPaths - kHeader);
    u16(kGpio, 4);
  }
  // a GPIO_Pin_LUT entry
  void gpio(uint32_t id, uint32_t reg, uint32_t shift) {
    const uint32_t at = kGpio + 4 + gpio_count * 4;
    u16(at, reg); u8(at + 2, shift); u8(at + 3, id);
    ++gpio_count;
    u16(kGpio, 4 + gpio_count * 4);
  }
  // a path: device tag, connector object id, optionally with `extra` more bytes of object ids
  void path(uint32_t tag, uint32_t conn_id, uint32_t extra = 4) {
    u16(path_at, tag); u16(path_at + 2, 8 + extra); u16(path_at + 4, conn_id);
    path_at += 8 + extra;
    u8(kPaths, ++path_count);
  }
  // a connector object whose records begin where the last one left off; returns where the records start so the caller can write them
  uint32_t object(uint32_t conn_id) {
    u16(object_at, conn_id); u16(object_at + 4, record_at - kHeader);
    object_at += 8;
    u8(kObjects, ++object_count);
    return record_at;
  }
  void record(uint32_t type, uint32_t size, uint32_t a = 0, uint32_t b2 = 0) {
    u8(record_at, type); u8(record_at + 1, size); u8(record_at + 2, a); u8(record_at + 3, b2);
    record_at += size;
  }
  void end_records() { record_at += 2; }   // (type 0, size 0)
  void hpd(uint32_t conn_id, uint32_t gpio_id, uint32_t plugged = 1) { object(conn_id); record(2, 4, gpio_id, plugged); end_records(); }
  ConstByteView view() const { return ConstByteView(b.data(), b.size()); }
};

// connector object ids: type 1 (connector) << 12, the enumeration << 8, the object
constexpr uint32_t kEdp = 0x1000 | 0x100 | 20, kHdmi = 0x1000 | 0x100 | 12, kDp1 = 0x1000 | 0x100 | 19, kDp2 = 0x1000 | 0x200 | 19;
constexpr uint32_t kLcd1 = 0x0002, kDfp1 = 0x0008, kDfp2 = 0x0080, kDfp3 = 0x0200, kDfp4 = 0x0400, kTv1 = 0x0004, kCv = 0x0100, kCrt1 = 0x0001;

Img xps_board() {
  Img i;
  i.gpio(1, 0x488d, 0); i.gpio(2, 0x488d, 8); i.gpio(3, 0x488d, 16); i.gpio(4, 0x488d, 24); i.gpio(5, 0x488d, 26); i.gpio(6, 0x488d, 28);
  i.path(kLcd1, kEdp); i.path(kDfp1, kHdmi); i.path(kDfp2, kDp1); i.path(kDfp3, kDp2);
  i.hpd(kEdp, 1); i.hpd(kHdmi, 2); i.hpd(kDp1, 3); i.hpd(kDp2, 5);
  return i;
}

void test_board() {
  Img i = xps_board();
  AtomConnectors::Connector c[8];
  const uint32_t n = AtomConnectors::parse(i.view(), ArrayView<AtomConnectors::Connector>(c, 8));
  CHECK(n == 4);
  char name[16];
  AtomConnectors::name(c[0], ArrayView<char>(name, sizeof name));
  CHECK(!strcmp(name, "eDP-1") && c[0].hpd_pin == 0 && c[0].devices == kLcd1 && c[0].object_id == 20 && c[0].enum_id == 1 && c[0].plugged_state == 1);
  AtomConnectors::name(c[1], ArrayView<char>(name, sizeof name));
  CHECK(!strcmp(name, "HDMI-A-1") && c[1].hpd_pin == 1 && c[1].devices == kDfp1 && c[1].object_id == 12);
  AtomConnectors::name(c[2], ArrayView<char>(name, sizeof name));
  CHECK(!strcmp(name, "DP-1") && c[2].hpd_pin == 2 && c[2].enum_id == 1);
  AtomConnectors::name(c[3], ArrayView<char>(name, sizeof name));
  CHECK(!strcmp(name, "DP-2") && c[3].hpd_pin == 4 && c[3].enum_id == 2 && c[3].object_id == 19);
  const AtomConnectors::Connector* found = AtomConnectors::find_pin(ArrayView<const AtomConnectors::Connector>(c, n), 2);
  CHECK(found == &c[2]);
  CHECK(AtomConnectors::find_pin(ArrayView<const AtomConnectors::Connector>(c, n), 3) == nullptr);
  CHECK(AtomConnectors::find_pin(ArrayView<const AtomConnectors::Connector>(c, n), AtomConnectors::kNoPin) == nullptr);
}

void test_pins() {
  // every pin the driver knows, a bit of the right register that is not a pin, and the right bit of another register
  Img i;
  i.gpio(1, 0x488d, 0); i.gpio(2, 0x488d, 8); i.gpio(3, 0x488d, 16); i.gpio(4, 0x488d, 24); i.gpio(5, 0x488d, 26); i.gpio(6, 0x488d, 28);
  i.gpio(7, 0x488d, 5); i.gpio(8, 0x488c, 0); i.gpio(9, 0x488e, 8); i.gpio(10, 0x488d, 32);
  const uint32_t ids[] = {0x1000 | 0x100 | 12, 0x1000 | 0x200 | 12, 0x1000 | 0x300 | 12, 0x1000 | 0x400 | 12, 0x1000 | 0x500 | 12, 0x1000 | 0x600 | 12,
                          0x1000 | 0x100 | 19, 0x1000 | 0x200 | 19, 0x1000 | 0x300 | 19, 0x1000 | 0x400 | 19};
  const uint32_t tags[] = {0x8, 0x80, 0x200, 0x400, 0x800, 0x40, 0x1, 0x2, 0x10, 0x20};
  for (int k = 0; k < 10; ++k) i.path(tags[k], ids[k]);
  for (int k = 0; k < 10; ++k) i.hpd(ids[k], k + 1);
  AtomConnectors::Connector c[16];
  const uint32_t n = AtomConnectors::parse(i.view(), ArrayView<AtomConnectors::Connector>(c, 16));
  CHECK(n == 10);
  const uint8_t expected[] = {0, 1, 2, 3, 4, 5, 0xFF, 0xFF, 0xFF, 0xFF};
  for (uint32_t k = 0; k < n && k < 10; ++k) CHECK(c[k].hpd_pin == expected[k]);
  CHECK(c[4].number == 5 && c[5].number == 6 && c[0].number == 1);   // six HDMI-A: 1..6
  char name[16];
  AtomConnectors::name(c[5], ArrayView<char>(name, sizeof name));
  CHECK(!strcmp(name, "HDMI-A-6"));
  AtomConnectors::name(c[9], ArrayView<char>(name, sizeof name));
  CHECK(!strcmp(name, "DP-4"));
}

void test_skips() {
  // TV and component video, a device the header does not list, objects that have no name (0, 8, 9, 11, 16, 17, 18, 21 and beyond the table)
  Img i(0x0FFF & ~0x0020);
  const uint32_t unnamed[] = {0, 8, 9, 11, 16, 17, 18, 21, 22, 0xFF};
  i.path(kTv1, kHdmi); i.path(kCv, kHdmi); i.path(0x20, kHdmi);
  for (uint32_t id : unnamed) i.path(kDfp1, 0x1100 | id);
  i.path(kDfp2, 0x1100 | 5);
  AtomConnectors::Connector c[8];
  const uint32_t n = AtomConnectors::parse(i.view(), ArrayView<AtomConnectors::Connector>(c, 8));
  CHECK(n == 1 && c[0].type == AtomConnectors::Type::Vga && c[0].devices == kDfp2 && c[0].hpd_pin == AtomConnectors::kNoPin);
  // a path that lists TV and DFP together is not the TV path: it is looked at
  Img j;
  j.path(kTv1 | kDfp1, kHdmi);
  CHECK(AtomConnectors::parse(j.view(), ArrayView<AtomConnectors::Connector>(c, 8)) == 1);
}

void test_names_of_every_type() {
  const struct { uint32_t object; const char* name; } cases[] = {
      {1, "DVI-I-1"}, {2, "DVI-I-2"}, {3, "DVI-D-1"}, {4, "DVI-D-2"}, {5, "VGA-1"}, {6, "Composite-1"}, {7, "SVIDEO-1"}, {10, "DIN-1"}, {15, "DIN-2"},
      {12, "HDMI-A-1"}, {13, "HDMI-B-1"}, {14, "LVDS-1"}, {19, "DP-1"}, {20, "eDP-1"},
  };
  Img i;
  for (const auto& k : cases) i.path(kDfp1, 0x1100 | k.object);
  AtomConnectors::Connector c[16];
  const uint32_t n = AtomConnectors::parse(i.view(), ArrayView<AtomConnectors::Connector>(c, 16));
  CHECK(n == 14);
  for (uint32_t k = 0; k < n && k < 14; ++k) {
    char name[16];
    AtomConnectors::name(c[k], ArrayView<char>(name, sizeof name));
    if (strcmp(name, cases[k].name)) { printf("  %u: %s != %s\n", k, name, cases[k].name); CHECK(false); }
  }
}

void test_records() {
  Img i;
  i.gpio(1, 0x488d, 8);
  i.path(kDfp1, kHdmi); i.path(kDfp2, kDp1); i.path(kDfp3, kDp2); i.path(kDfp4, 0x1300 | 12);
  // HDMI: an I2C record (type 1, 6 bytes) and a type 3 record before the hot plug record
  i.object(kHdmi); i.record(1, 6, 0x90, 0x00); i.record(3, 4); i.record(2, 4, 1, 0); i.end_records();
  // DP-1: a record type beyond the last one stops the walk before the hot plug record
  i.object(kDp1); i.record(23, 4); i.record(2, 4, 1, 1); i.end_records();
  // DP-2: a record of size 0 stops the walk
  i.object(kDp2); i.record(3, 0); i.record_at += 2; i.record(2, 4, 1, 1); i.end_records();
  // the fourth has no object at all
  AtomConnectors::Connector c[8];
  const uint32_t n = AtomConnectors::parse(i.view(), ArrayView<AtomConnectors::Connector>(c, 8));
  CHECK(n == 4);
  CHECK(c[0].hpd_pin == 1 && c[0].plugged_state == 0);
  CHECK(c[1].hpd_pin == AtomConnectors::kNoPin);
  CHECK(c[2].hpd_pin == AtomConnectors::kNoPin);
  CHECK(c[3].hpd_pin == AtomConnectors::kNoPin && c[3].enum_id == 3 && c[3].number == 2);
  // a hot plug record of size 0 ends the walk (it is not read), and so does a record of type 0 before it
  Img z;
  z.gpio(1, 0x488d, 0);
  z.path(kDfp1, kHdmi); z.path(kDfp2, kDp1);
  z.object(kHdmi); z.record(2, 0, 1, 1); z.record_at += 4;
  z.object(kDp1); z.record(0, 4, 0, 0); z.record(2, 4, 1, 1); z.end_records();
  CHECK(AtomConnectors::parse(z.view(), ArrayView<AtomConnectors::Connector>(c, 8)) == 2 && c[0].hpd_pin == AtomConnectors::kNoPin && c[1].hpd_pin == AtomConnectors::kNoPin);
  // a GPIO that is in the file just beyond the end of the table the header says is not in the table
  Img y;
  y.gpio(1, 0x488d, 0);
  y.u16(kGpio + 4 + 4, 0x488d); y.u8(kGpio + 4 + 4 + 2, 8); y.u8(kGpio + 4 + 4 + 3, 2);
  y.path(kDfp1, kHdmi);
  y.hpd(kHdmi, 2);
  CHECK(AtomConnectors::parse(y.view(), ArrayView<AtomConnectors::Connector>(c, 8)) == 1 && c[0].hpd_pin == AtomConnectors::kNoPin);
  // an HPD record naming a GPIO that is not in the table
  Img j;
  j.gpio(1, 0x488d, 0);
  j.path(kDfp1, kHdmi);
  j.hpd(kHdmi, 9);
  CHECK(AtomConnectors::parse(j.view(), ArrayView<AtomConnectors::Connector>(c, 8)) == 1 && c[0].hpd_pin == AtomConnectors::kNoPin);
}

void test_ddc() {
  // the I2C record (type 1: id byte at +2, address at +3) gives the DDC line; the last one wins, as in the driver's loop; before or after the hot plug record; none: no DDC
  Img i;
  i.gpio(1, 0x488d, 0);
  i.path(kDfp1, kHdmi); i.path(kDfp2, kDp1); i.path(kDfp3, kDp2); i.path(kDfp4, 0x1300 | 12);
  i.object(kHdmi); i.record(1, 4, 0x91, 0); i.record(2, 4, 1, 1); i.end_records();
  i.object(kDp1); i.record(2, 4, 1, 0); i.record(1, 4, 0x12, 0); i.record(1, 4, 0x13, 0); i.end_records();
  i.object(kDp2); i.record(3, 4); i.end_records();
  AtomConnectors::Connector c[8];
  const uint32_t n = AtomConnectors::parse(i.view(), ArrayView<AtomConnectors::Connector>(c, 8));
  CHECK(n == 4);
  CHECK(c[0].has_ddc && c[0].ddc_id == 0x91 && c[0].hpd_pin == 0);
  CHECK(c[1].has_ddc && c[1].ddc_id == 0x13 && c[1].hpd_pin == 0);
  CHECK(!c[2].has_ddc && c[2].ddc_id == 0);
  CHECK(!c[3].has_ddc);   // (no object at all)
  // a connector reached by two paths takes the DDC line from whichever path has one
  Img m;
  m.path(kTv1 | kDfp1, kHdmi); m.path(kDfp2, kHdmi);
  m.object(kHdmi); m.record(1, 4, 0x55, 0); m.end_records();
  CHECK(AtomConnectors::parse(m.view(), ArrayView<AtomConnectors::Connector>(c, 8)) == 1 && c[0].has_ddc && c[0].ddc_id == 0x55);
  // a record cut off by the end of the image has no id
  Img t;
  t.path(kDfp1, kHdmi);
  t.object(kHdmi); t.record(1, 4, 0x91, 0);
  const std::vector<uint8_t> cut(t.b.begin(), t.b.begin() + (kRecords + 2));
  CHECK(AtomConnectors::parse(ConstByteView(cut.data(), cut.size()), ArrayView<AtomConnectors::Connector>(c, 8)) == 1 && !c[0].has_ddc);
}

void test_merging() {
  // one connector reached by two devices: merged, and the pin of whichever path has one
  Img i;
  i.gpio(1, 0x488d, 16);
  i.path(kDfp1, kHdmi); i.path(kDfp2, kHdmi); i.path(kDfp3, kDp1);
  i.hpd(kHdmi, 1); i.hpd(kDp1, 1);
  AtomConnectors::Connector c[8];
  uint32_t n = AtomConnectors::parse(i.view(), ArrayView<AtomConnectors::Connector>(c, 8));
  CHECK(n == 2 && c[0].devices == (kDfp1 | kDfp2) && c[0].hpd_pin == 2 && c[1].number == 1 && c[1].type == AtomConnectors::Type::DisplayPort);
  // the first path of a connector is a TV/DFP mix, which looks up no pin: the second path's pin is the connector's
  Img m;
  m.gpio(1, 0x488d, 24);
  m.path(kTv1 | kDfp1, kHdmi); m.path(kDfp2, kHdmi);
  m.hpd(kHdmi, 1);
  n = AtomConnectors::parse(m.view(), ArrayView<AtomConnectors::Connector>(c, 8));
  CHECK(n == 1 && c[0].devices == (kTv1 | kDfp1 | kDfp2) && c[0].hpd_pin == 3 && c[0].plugged_state == 1);
  // the same connector with the same id, but a different enumeration, is another connector
  Img j;
  j.path(kDfp1, kDp1); j.path(kDfp2, kDp2);
  n = AtomConnectors::parse(j.view(), ArrayView<AtomConnectors::Connector>(c, 8));
  CHECK(n == 2 && c[0].number == 1 && c[1].number == 2);
  // the first path has no pin (no object), the second path does: the pin is taken from it
  Img k;
  k.gpio(1, 0x488d, 0);
  k.path(kDfp1, kHdmi); k.path(kDfp2, kHdmi);
  k.hpd(kHdmi, 1);
  n = AtomConnectors::parse(k.view(), ArrayView<AtomConnectors::Connector>(c, 8));
  CHECK(n == 1 && c[0].hpd_pin == 0);   // (the object is found for both paths: same result)
}

void test_limits() {
  // capacity: the output's size and kMaxConnectors
  Img i;
  for (int k = 0; k < 20; ++k) i.path(kDfp1, 0x1000 | ((k % 7 + 1) << 8) | 12);
  AtomConnectors::Connector c[32];
  CHECK(AtomConnectors::parse(i.view(), ArrayView<AtomConnectors::Connector>(c, 2)) == 2);
  CHECK(AtomConnectors::parse(i.view(), ArrayView<AtomConnectors::Connector>(c, 0)) == 0);
  CHECK(AtomConnectors::parse(i.view(), ArrayView<AtomConnectors::Connector>(c, 32)) == 7);
  Img many;
  for (uint32_t k = 0; k < 40; ++k) many.path(kDfp1, 0x1000 | ((k % 8) << 8) | (k % 2 ? 12 : 19) | (k / 16) * 0x2000 * 0);
  // distinct connector ids come only from the enumeration and the object (8 * 2 here)
  CHECK(AtomConnectors::parse(many.view(), ArrayView<AtomConnectors::Connector>(c, 32)) <= AtomConnectors::kMaxConnectors);
  // a connector table of revision 1 is not read
  Img old(0x0FFF, 1);
  old.path(kDfp1, kHdmi);
  CHECK(AtomConnectors::parse(old.view(), ArrayView<AtomConnectors::Connector>(c, 8)) == 0);
  // no Object_Header table
  Img none;
  none.path(kDfp1, kHdmi);
  none.u16(kMaster + 4 + 22 * 2, 0);
  CHECK(AtomConnectors::parse(none.view(), ArrayView<AtomConnectors::Connector>(c, 8)) == 0);
  // no GPIO table: connectors, no pins
  Img nogpio;
  nogpio.path(kDfp1, kHdmi); nogpio.hpd(kHdmi, 1);
  nogpio.u16(kMaster + 4 + 12 * 2, 0);
  CHECK(AtomConnectors::parse(nogpio.view(), ArrayView<AtomConnectors::Connector>(c, 8)) == 1 && c[0].hpd_pin == AtomConnectors::kNoPin);
  // a path of a size below its header ends the walk, with what was found kept
  Img small;
  small.path(kDfp1, kHdmi);
  small.path(kDfp2, kDp1);
  small.u16(kPaths + 4 + 12 + 2, 4);
  CHECK(AtomConnectors::parse(small.view(), ArrayView<AtomConnectors::Connector>(c, 8)) == 1);
  // not an AtomBIOS image
  Img bad = xps_board();
  bad.b[0] = 0;
  CHECK(AtomConnectors::parse(bad.view(), ArrayView<AtomConnectors::Connector>(c, 8)) == 0);
  for (uint32_t k = 0; k < 4; ++k) {
    bad = xps_board();
    bad.b[kRom + 4 + k] = 'X';
    CHECK(AtomConnectors::parse(bad.view(), ArrayView<AtomConnectors::Connector>(c, 8)) == 0);
  }
  // the ROM header at the very end of the image: its data table pointer is not there to read, however the tables that pointer would have led to are
  bad = xps_board();
  bad.b.resize(0x700);
  const uint32_t late = static_cast<uint32_t>(bad.b.size()) - 0x10;
  bad.u16(0x48, late);
  memcpy(&bad.b[late + 4], "ATOM", 4);
  CHECK(AtomConnectors::parse(bad.view(), ArrayView<AtomConnectors::Connector>(c, 8)) == 0);
  CHECK(AtomConnectors::parse(ConstByteView(), ArrayView<AtomConnectors::Connector>(c, 8)) == 0);
}

void test_names() {
  AtomConnectors::Connector c;
  c.type = AtomConnectors::Type::HdmiA;
  char name[16];
  c.number = 7;
  CHECK(AtomConnectors::name(c, ArrayView<char>(name, sizeof name)) == 8 && !strcmp(name, "HDMI-A-7"));
  c.number = 9;
  CHECK(AtomConnectors::name(c, ArrayView<char>(name, sizeof name)) == 8 && !strcmp(name, "HDMI-A-9"));
  c.number = 10;
  CHECK(AtomConnectors::name(c, ArrayView<char>(name, sizeof name)) == 9 && !strcmp(name, "HDMI-A-10"));
  c.number = 12;
  CHECK(AtomConnectors::name(c, ArrayView<char>(name, sizeof name)) == 9 && !strcmp(name, "HDMI-A-12"));
  c.number = 99;
  CHECK(AtomConnectors::name(c, ArrayView<char>(name, sizeof name)) == 9 && !strcmp(name, "HDMI-A-99"));
  c.number = 100;
  CHECK(AtomConnectors::name(c, ArrayView<char>(name, sizeof name)) == 10 && !strcmp(name, "HDMI-A-100"));
  c.number = 120;
  CHECK(AtomConnectors::name(c, ArrayView<char>(name, sizeof name)) == 10 && !strcmp(name, "HDMI-A-120"));
  // cut short: always a terminated string, and the length is what was written
  c.number = 1;
  for (uint32_t size = 1; size < 10; ++size) {
    memset(name, 'x', sizeof name);
    const uint32_t n = AtomConnectors::name(c, ArrayView<char>(name, size));
    CHECK(n == size - 1 && name[n] == 0 && !strncmp(name, "HDMI-A-1", n));
    CHECK(name[size] == 'x' || size >= sizeof name);
  }
  CHECK(AtomConnectors::name(c, ArrayView<char>()) == 0);
}

// Every truncation and every flipped bit: no crash, no report from the sanitizers, and what comes back is well-formed
bool well_formed(const AtomConnectors::Connector* c, uint32_t n, uint32_t capacity) {
  if (n > capacity || n > AtomConnectors::kMaxConnectors) return false;
  for (uint32_t k = 0; k < n; ++k) {
    if (c[k].number < 1) return false;
    if (c[k].hpd_pin > 5 && c[k].hpd_pin != AtomConnectors::kNoPin) return false;
    if (static_cast<uint8_t>(c[k].type) > static_cast<uint8_t>(AtomConnectors::Type::EmbeddedDisplayPort)) return false;
  }
  return true;
}

void test_damage() {
  const Img good = xps_board();
  AtomConnectors::Connector c[16];
  for (size_t len = 0; len <= good.b.size(); ++len) {
    const uint32_t n = AtomConnectors::parse(ConstByteView(good.b.data(), len), ArrayView<AtomConnectors::Connector>(c, 16));
    if (!well_formed(c, n, 16)) { printf("  truncated to %zu\n", len); CHECK(false); break; }
  }
  // a truncation inside the tables keeps what came before it
  CHECK(AtomConnectors::parse(ConstByteView(good.b.data(), kPaths + 4 + 12 + 12 + 4), ArrayView<AtomConnectors::Connector>(c, 16)) == 2);
  Img flipped = good;
  for (size_t at = 0; at < flipped.b.size(); ++at) {
    for (int bit = 0; bit < 8; ++bit) {
      flipped.b[at] ^= static_cast<uint8_t>(1 << bit);
      const uint32_t n = AtomConnectors::parse(flipped.view(), ArrayView<AtomConnectors::Connector>(c, 16));
      if (!well_formed(c, n, 16)) { printf("  byte %zu bit %d\n", at, bit); CHECK(false); }
      flipped.b[at] ^= static_cast<uint8_t>(1 << bit);
    }
  }
  // noise in the table area
  uint32_t seed = 12345;
  for (int round = 0; round < 2000; ++round) {
    Img noisy = good;
    for (int k = 0; k < 16; ++k) {
      seed = seed * 1664525u + 1013904223u;
      noisy.b[kHeader + (seed >> 8) % 0x200] = static_cast<uint8_t>(seed >> 24);
    }
    const uint32_t n = AtomConnectors::parse(noisy.view(), ArrayView<AtomConnectors::Connector>(c, 16));
    if (!well_formed(c, n, 16)) { CHECK(false); break; }
  }
}
}  // namespace

int main() {
  test_board();
  test_pins();
  test_skips();
  test_names_of_every_type();
  test_records();
  test_merging();
  test_ddc();
  test_limits();
  test_names();
  test_damage();
  printf(g_failed ? "AtomConnectorsTest: %d FAILED\n" : "AtomConnectorsTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
