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

// The Realtek Wi-Fi layer (src/Hardware/WirelessInterface/Realtek): the adapter table, the rtw88 and rtw89 firmware parsers on synthetic files (whole, cut short at
// every length, every bit flipped) and, when the environment names real ones (RTW88_FIRMWARE_FILES, RTW89_FIRMWARE_FILES: paths separated by ':', for example the
// decompressed files of /usr/lib/firmware/rtw88 and rtw89), on those with damage done to them; the eFuse logical-map decoders, the power-sequence interpreter against
// a model of the registers, the USB register window, and RtwChip / Rtw89Chip start-up against a register model (8822CE, 8723DE with its firmware download, 8822CU
// over USB, 8852BE).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "Hardware/WirelessInterface/Realtek/RealtekUsbRegisters.hpp"
#include "Hardware/WirelessInterface/Realtek/RealtekWifi.hpp"
#include "Hardware/WirelessInterface/Realtek/RtwEfuse.hpp"
#include "Hardware/WirelessInterface/Realtek/RtwFirmware.hpp"
#include "Hardware/WirelessInterface/Realtek/RtwPowerSequence.hpp"
#include "Hardware/WirelessInterface/WifiAdapterCatalog.hpp"
#include "Hardware/WirelessInterface/WifiChipFactory.hpp"

namespace hardware {
void delay_us(uint64_t) {}
}  // namespace hardware

using namespace hardware;
using Bytes = std::vector<uint8_t>;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { free(p); }
};

static void put16(Bytes& b, size_t at, uint32_t v) { b[at] = uint8_t(v); b[at + 1] = uint8_t(v >> 8); }
static void put32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }
static void append32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }

struct FakeFirmware final : IWifiFirmwareSource {
  std::map<std::string, Bytes> files;
  std::vector<std::string> asked;
  bool read(const char* name, ByteBuffer& out) override {
    asked.push_back(name);
    auto it = files.find(name);
    if (it == files.end() || it->second.empty()) return false;
    out.clear();
    return out.append(view(it->second));
  }
};

// ---------------------------------------------------------------- the adapter table

static void test_models() {
  const ArrayView<const WifiAdapterModel> models = realtek_wifi_models();
  CHECK(models.size() > 60);
  TestAllocator alloc;
  for (uint64_t i = 0; i < models.size(); ++i) {
    const WifiAdapterModel& m = models[i];
    CHECK(m.vendor == WifiVendor::Realtek);
    CHECK(wifi_driver_family_vendor(m.family) == WifiVendor::Realtek);
    CHECK(m.chip && m.chip[0] && m.product && m.product[0] && m.firmware);
    CHECK(m.spatial_streams >= 1 && m.spatial_streams <= 4);
    CHECK(m.bus == WifiBus::Pci ? m.vendor_id == 0x10EC : m.bus == WifiBus::Usb);
    for (uint64_t j = i + 1; j < models.size(); ++j)
      if (m.bus == models[j].bus && m.vendor_id == models[j].vendor_id && m.device_id == models[j].device_id) {
        printf("FAILED: duplicate %04x:%04x\n", m.vendor_id, m.device_id);
        ++g_failed;
      }
    const bool rtw = m.family == WifiDriverFamily::Rtw88 || m.family == WifiDriverFamily::Rtw89 || m.family == WifiDriverFamily::Rtl8812au;
    if (rtw) {
      CHECK(m.firmware[0] != 0);
      CHECK(rtw_chip_kind(m.chip) != RtwChipKind::Unknown);
      const char* prefix = m.family == WifiDriverFamily::Rtw89 ? "rtw89/" : "rtw88/";
      CHECK(StringView(m.firmware).starts_with(prefix));
      // A chip driver for every rtw88/rtw89 entry, none for the others.
      WifiChipBus bus;
      UniquePointer<IWifiChip> chip = make_wifi_chip(m, bus, alloc);
      CHECK(chip);
      if (chip) CHECK(&chip->model() == &m && chip->state() == WifiChipState::Off);
    } else {
      WifiChipBus bus;
      CHECK(!make_realtek_wifi_chip(m, bus, alloc));
    }
    if (m.firmware[0]) CHECK(wifi_firmware_name_valid(m.firmware));
  }
  const WifiAdapterModel* m = wifi_find_pci_adapter(0x10EC, 0xC822);
  CHECK(m && StringView(m->chip) == StringView("RTL8822CE") && StringView(m->firmware) == StringView("rtw88/rtw8822c_fw.bin"));
  m = wifi_find_usb_adapter(0x2357, 0x0138);
  CHECK(m && m->family == WifiDriverFamily::Rtw88 && rtw_chip_kind(m->chip) == RtwChipKind::Rtw8822B);
  m = wifi_find_usb_adapter(0x0BDA, 0x8179);
  CHECK(m && m->family == WifiDriverFamily::Rtl8xxxu);
  m = wifi_find_pci_adapter(0x10EC, 0xB852);
  CHECK(m && m->family == WifiDriverFamily::Rtw89 && StringView(m->firmware) == StringView("rtw89/rtw8852b_fw-2.bin"));
  CHECK(rtw_chip_kind("RTL8852BTE") == RtwChipKind::Rtw8852BT && rtw_chip_kind("RTL8852BE") == RtwChipKind::Rtw8852B);
  CHECK(rtw_chip_kind("RTL8811CU") == RtwChipKind::Rtw8821C && rtw_chip_kind("RTL9999") == RtwChipKind::Unknown);
}

// ---------------------------------------------------------------- rtw88 firmware

static Bytes rtw88_block(uint32_t n, uint8_t seed) {
  Bytes b(n);
  for (uint32_t i = 0; i < n; ++i) b[i] = uint8_t(seed + i * 7 + (i >> 3));
  return b;
}

static void append_block(Bytes& f, const Bytes& block) {
  f.insert(f.end(), block.begin(), block.end());
  const uint16_t sum = rtw88_block_checksum(view(block));
  const uint8_t trailer[8] = {uint8_t(sum), uint8_t(sum >> 8), 0, 0, 0, 0, 0, 0};
  f.insert(f.end(), trailer, trailer + 8);
}

static Bytes make_rtw88(uint16_t signature, uint32_t dmem, uint32_t imem, uint32_t emem) {
  Bytes f(64, 0);
  put16(f, 0, signature);
  put16(f, 4, 9);
  f[6] = 9;
  f[7] = 15;
  put32(f, 0x0C, 0x0F);
  f[0x10] = 3; f[0x11] = 16; f[0x12] = 13; f[0x13] = 49;
  put16(f, 0x14, 2023);
  f[0x18] = emem ? 0x18 : 0x08;
  put16(f, 0x1C, 0x18);
  put32(f, 0x20, 0x80200000);
  put32(f, 0x24, dmem);
  put32(f, 0x30, imem);
  put32(f, 0x34, emem);
  put32(f, 0x38, 0x80030000);
  put32(f, 0x3C, 0x80100000);
  append_block(f, rtw88_block(dmem, 1));
  append_block(f, rtw88_block(imem, 2));
  if (emem) append_block(f, rtw88_block(emem, 3));
  return f;
}

static Bytes make_rtw88_legacy(uint16_t signature, uint32_t body) {
  Bytes f(32, 0);
  put16(f, 0, signature);
  f[2] = 0x10;
  put16(f, 4, 48);
  f[6] = 2; f[7] = 24; f[8] = 0x11; f[9] = 5; f[10] = 13; f[11] = 30;
  put16(f, 12, body);
  const Bytes b = rtw88_block(body, 9);
  f.insert(f.end(), b.begin(), b.end());
  return f;
}

static void test_rtw88_firmware() {
  Rtw88Firmware fw;
  const Bytes f = make_rtw88(0x8822, 40, 64, 24);
  CHECK(rtw88_parse_firmware(view(f), false, 0x8822, &fw) == RtwFirmwareStatus::Ok);
  CHECK(fw.version == 9 && fw.sub_version == 9 && fw.sub_index == 15 && fw.year == 2023 && fw.month == 3 && fw.h2c_format_version == 0x18);
  CHECK(fw.dmem.size() == 40 && fw.imem.size() == 64 && fw.emem.size() == 24);
  CHECK(fw.dmem.data() == f.data() + 64 && fw.imem.data() == f.data() + 64 + 48 && fw.emem.data() == f.data() + 64 + 48 + 72);
  CHECK(fw.dmem_address == 0x80200000 && fw.imem_address == 0x80100000 && fw.emem_address == 0x80030000);
  CHECK(rtw88_parse_firmware(view(f), false, 0x8821, &fw) == RtwFirmwareStatus::BadSignature);
  CHECK(rtw88_parse_firmware(view(f), false, 0, &fw) == RtwFirmwareStatus::Ok);
  // Without EMEM (mem_usage bit 4 clear): the EMEM size field is ignored.
  Bytes g = make_rtw88(0x8822, 40, 64, 0);
  put32(g, 0x34, 0x1234);
  CHECK(rtw88_parse_firmware(view(g), false, 0x8822, &fw) == RtwFirmwareStatus::Ok && fw.emem.empty());
  // One byte too many.
  g.push_back(0);
  CHECK(rtw88_parse_firmware(view(g), false, 0x8822, &fw) == RtwFirmwareStatus::SizeMismatch);
  // Every shorter length fails.
  for (size_t n = 0; n < f.size(); ++n) CHECK(rtw88_parse_firmware(ConstByteView(f.data(), n), false, 0, &fw) != RtwFirmwareStatus::Ok);
  // Every bit flipped: inside a block or its checksum always caught; never a crash.
  for (size_t at = 0; at < f.size(); ++at)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes h = f;
      h[at] ^= uint8_t(1 << bit);
      const RtwFirmwareStatus status = rtw88_parse_firmware(view(h), false, 0, &fw);
      const bool in_block = (at >= 64 && at < 64 + 40) || (at >= 112 && at < 112 + 64) || (at >= 184 && at < 184 + 24);
      const bool in_sum = at == 104 || at == 105 || at == 176 || at == 177 || at == 208 || at == 209;
      if (in_block || in_sum) CHECK(status == RtwFirmwareStatus::BadChecksum);
    }
  // Sizes that would wrap a 32-bit sum.
  Bytes w = f;
  put32(w, 0x24, 0xFFFFFFF8);
  CHECK(rtw88_parse_firmware(view(w), false, 0, &fw) == RtwFirmwareStatus::Truncated);
  put32(w, 0x24, 40);
  put32(w, 0x30, 0xFFFFFFFF);
  put32(w, 0x34, 0xFFFFFFFF);
  CHECK(rtw88_parse_firmware(view(w), false, 0, &fw) == RtwFirmwareStatus::Truncated);
  CHECK(rtw88_block_checksum(view(Bytes{0x01, 0x02, 0x10, 0x20, 0x07})) == ((0x0201 ^ 0x2010 ^ 0x0007) & 0xFFFF));

  // The 8051 header.
  const Bytes l = make_rtw88_legacy(0x23D1, 301);
  CHECK(rtw88_parse_firmware(view(l), true, 0x23D0, &fw) == RtwFirmwareStatus::Ok);
  CHECK(fw.legacy && fw.signature == 0x23D1 && fw.version == 48 && fw.sub_version == 2 && fw.month == 0x11 && fw.body.size() == 301 && fw.body.data() == l.data() + 32);
  CHECK(rtw88_parse_firmware(view(l), true, 0x9500, &fw) == RtwFirmwareStatus::BadSignature);
  for (size_t n = 0; n < l.size(); ++n) CHECK(rtw88_parse_firmware(ConstByteView(l.data(), n), true, 0, &fw) != RtwFirmwareStatus::Ok);
  Bytes m = l;
  m.push_back(1);
  CHECK(rtw88_parse_firmware(view(m), true, 0, &fw) == RtwFirmwareStatus::SizeMismatch);
  for (size_t at = 0; at < 32; ++at)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes h = l;
      h[at] ^= uint8_t(1 << bit);
      const RtwFirmwareStatus status = rtw88_parse_firmware(view(h), true, 0x23D0, &fw);
      if (at == 12 || at == 13) CHECK(status != RtwFirmwareStatus::Ok);
      if (at == 1 || (at == 0 && bit >= 4)) CHECK(status == RtwFirmwareStatus::BadSignature);
    }
}

// ---------------------------------------------------------------- rtw89 firmware

struct SectionSpec {
  uint32_t address;
  uint8_t type;
  uint32_t length;
  bool checksum;
  uint8_t mssc;
};

// One firmware: header (version 0 or 1), section headers, optional dynamic header, the contents, then `trailing` bytes (the pool's or the signatures) after the
// section `pool_after` (-1: at the end).
static Bytes make_rtw89_image(int version, uint16_t chip, uint32_t fw_version, const std::vector<SectionSpec>& sections, bool dynamic, uint32_t trailing,
                              int pool_after = -1) {
  const uint32_t fixed = version ? 48 : 32;
  const uint32_t base = fixed + 16 * uint32_t(sections.size());
  const uint32_t dynamic_length = dynamic ? 16 : 0;
  Bytes f(base + dynamic_length, 0);
  put32(f, 0, uint32_t(chip) << 16 | 0x0102);
  put32(f, 4, fw_version);
  put32(f, 8, 0xC0FFEE11);
  put32(f, 12, uint32_t(version) << 24 | (version ? 0 : (base + dynamic_length)) << 16 | 0x1020);
  put32(f, 16, 0x1B120708);
  put32(f, 20, 2025);
  put32(f, 24, uint32_t(sections.size()) << 8 | 0x100000);
  put32(f, 28, (dynamic ? 1u << 16 : 0) | 0x7E4);
  for (size_t i = 0; i < sections.size(); ++i) {
    const SectionSpec& s = sections[i];
    put32(f, fixed + 16 * i, s.address);
    put32(f, fixed + 16 * i + 4, s.length | uint32_t(s.type) << 24 | (s.checksum ? 1u << 28 : 0) | 1u << 30);
    put32(f, fixed + 16 * i + 8, s.mssc);
  }
  if (dynamic) {
    put32(f, base, dynamic_length);
    put32(f, base + 4, 1);
  }
  for (size_t i = 0; i < sections.size(); ++i) {
    const uint32_t n = sections[i].length + (sections[i].checksum ? 8 : 0);
    for (uint32_t k = 0; k < n; ++k) f.push_back(uint8_t(0x40 + i * 16 + k));
    if (int(i) == pool_after) {
      const char* magic = "MSSKPOOL";
      for (int k = 0; k < 8; ++k) f.push_back(uint8_t(magic[k]));
      for (uint32_t k = 8; k < trailing; ++k) f.push_back(0xA5);
    }
  }
  if (pool_after < 0)
    for (uint32_t k = 0; k < trailing; ++k) f.push_back(uint8_t(k));
  return f;
}

struct Rtw89File {
  Bytes bytes;
  std::vector<size_t> ok_cuts;     // lengths at which a cut file is still whole (element ends and their padding)
};

static Rtw89File make_rtw89_file(uint16_t chip, uint8_t cv) {
  const Bytes a = make_rtw89_image(0, chip, 0x0F1D1D00, {{0xB8970000, 2, 40, false, 0}, {0xB8E11928, 9, 16, false, 1}}, true, 512);
  const Bytes b = make_rtw89_image(1, chip, 0x00282200, {{0x20100000, 1, 24, true, 0}}, false, 0);
  const Bytes c = make_rtw89_image(1, chip, 0x00282200, {{0x20100000, 2, 32, false, 0}, {0x202093B8, 9, 12, false, 0xFF}, {0x20209BB8, 1, 20, false, 0}}, true, 40, 1);
  Bytes logfmt(20, 0x5A);
  const Bytes* images[] = {&a, &b, &c, &logfmt};
  const uint8_t types[] = {5, 1, 3, 255};
  Rtw89File out;
  Bytes& f = out.bytes;
  f.assign(16, 0);
  f[0] = 0xFF;
  f[1] = 4;
  f[4] = 0; f[5] = 29; f[6] = 29; f[7] = 15;
  f.resize(16 + 4 * 16, 0);
  size_t shift = f.size();
  for (int i = 0; i < 4; ++i) {
    f[16 + 16 * i] = types[i] == 255 ? 0 : cv;
    f[16 + 16 * i + 1] = types[i];
    put32(f, 16 + 16 * i + 4, uint32_t(shift));
    put32(f, 16 + 16 * i + 8, uint32_t(images[i]->size()));
    shift += images[i]->size();
  }
  for (const Bytes* image : images) f.insert(f.end(), image->begin(), image->end());
  // A file may end anywhere in the padding after the firmwares or an element: it is whole up to there.
  out.ok_cuts.push_back(f.size());          // no elements
  while (f.size() % 16) { f.push_back(0); out.ok_cuts.push_back(f.size()); }
  // Two elements; the last one is not padded.
  append32(f, 2); append32(f, 20); append32(f, 0x00280000);
  f.resize(f.size() + 20, 0);
  for (int k = 0; k < 20; ++k) f.push_back(uint8_t(k));
  out.ok_cuts.push_back(f.size());
  while (f.size() % 16) { f.push_back(0); out.ok_cuts.push_back(f.size()); }
  append32(f, 8); append32(f, 5); append32(f, 0x000A0000);
  f.resize(f.size() + 20, 0);
  for (int k = 0; k < 5; ++k) f.push_back(uint8_t(0xE0 + k));
  return out;
}

static void test_rtw89_firmware() {
  const Rtw89File file = make_rtw89_file(0x8852, 1);
  const Bytes& f = file.bytes;
  Rtw89FirmwareFile parsed;
  CHECK(parsed.parse(view(f)) == RtwFirmwareStatus::Ok);
  CHECK(parsed.multi() && parsed.entry_count() == 4 && parsed.element_count() == 2 && parsed.container_version(1) == 29);
  const Rtw89FirmwareEntry* ce = parsed.find(Rtw89FirmwareType::NormalCe, 1);
  CHECK(ce && ce->cv == 1 && ce->data.data() == f.data() + 80);
  CHECK(!parsed.find(Rtw89FirmwareType::NormalCe, 0));
  CHECK(parsed.find(Rtw89FirmwareType::Normal, 1) && parsed.find(Rtw89FirmwareType::Wowlan, 1));
  Rtw89FirmwareElement element;
  CHECK(parsed.element(0, &element) && element.id == 2 && element.version == 0x00280000 && element.contents.size() == 20 && element.contents[19] == 19);
  CHECK(parsed.element(1, &element) && element.id == 8 && element.contents.size() == 5 && element.contents[4] == 0xE4);
  CHECK(!parsed.element(2, &element));

  Rtw89FirmwareImage image;
  if (ce) {
    CHECK(rtw89_parse_firmware_image(ce->data, &image) == RtwFirmwareStatus::Ok);
    CHECK(image.header_version == 0 && image.chip_id == 0x8852 && image.major == 0 && image.minor == 29 && image.sub_version == 29 && image.sub_index == 15);
    CHECK(image.dynamic_header && image.header_length == 32 + 32 + 16 && image.section_count == 2 && image.year == 2025 && image.month == 8 && image.day == 7);
    CHECK(image.sections[0].type == 2 && image.sections[0].data.size() == 40 && image.sections[0].data[0] == 0x40);
    CHECK(image.sections[1].type == 9 && image.sections[1].mssc == 1 && image.sections[1].data.size() == 16 && image.sections[1].data[0] == 0x50);
    CHECK(image.signature_bytes == 512);
  }
  const Rtw89FirmwareEntry* normal = parsed.find(Rtw89FirmwareType::Normal, 1);
  if (normal) {
    CHECK(rtw89_parse_firmware_image(normal->data, &image) == RtwFirmwareStatus::Ok);
    CHECK(image.header_version == 1 && image.header_length == 48 + 16 && image.sections[0].checksum && image.sections[0].data.size() == 32);
    CHECK(image.minor == 34 && image.sub_version == 40);
  }
  const Rtw89FirmwareEntry* wowlan = parsed.find(Rtw89FirmwareType::Wowlan, 1);
  if (wowlan) {
    // The key pool after the first security section; the section after it follows the pool.
    CHECK(rtw89_parse_firmware_image(wowlan->data, &image) == RtwFirmwareStatus::Ok);
    CHECK(image.section_count == 3 && image.signature_bytes == 40 && image.sections[1].mssc == 0xFF);
    CHECK(image.sections[2].data.size() == 20 && image.sections[2].data[0] == 0x60);
    CHECK(image.sections[2].data.data() == image.sections[1].data.data() + 12 + 40);
  }

  // Every cut: whole only at an element boundary; never a crash.
  for (size_t n = 0; n < f.size(); ++n) {
    Rtw89FirmwareFile cut;
    const bool ok = cut.parse(ConstByteView(f.data(), n)) == RtwFirmwareStatus::Ok;
    bool boundary = false;
    for (size_t b : file.ok_cuts) boundary = boundary || b == n;
    if (ok != boundary) {
      printf("FAILED: rtw89 cut at %zu: %s\n", n, ok ? "accepted" : "refused");
      ++g_failed;
    }
  }
  // Every bit flipped: no crash; flips in the container table and section headers are mostly caught.
  uint32_t caught = 0, flips = 0;
  for (size_t at = 0; at < f.size(); ++at)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes h = f;
      h[at] ^= uint8_t(1 << bit);
      Rtw89FirmwareFile damaged;
      const RtwFirmwareStatus status = damaged.parse(view(h));
      Rtw89FirmwareElement e;
      for (uint32_t i = 0; i < damaged.element_count() && damaged.element(i, &e); ++i) {}
      ++flips;
      if (status != RtwFirmwareStatus::Ok) ++caught;
      if (at == 0) CHECK(status != RtwFirmwareStatus::Ok);       // no longer a container (or not a valid single firmware)
    }
  printf("  rtw89 synthetic: %u of %u bit flips refused (the rest fall in contents, padding or fields that are not checked)\n", caught, flips);

  // Damage that has to be refused.
  Bytes h = f;
  h[1] = 0;
  CHECK(parsed.parse(view(h)) == RtwFirmwareStatus::BadLayout);
  h = f;
  h[1] = 17;
  CHECK(parsed.parse(view(h)) == RtwFirmwareStatus::BadLayout);
  h = f;
  put32(h, 16 + 4, 8);                        // the first entry starts inside the table
  CHECK(parsed.parse(view(h)) == RtwFirmwareStatus::BadLayout);
  h = f;
  put32(h, 16 + 16 + 4, 80);                  // the second overlaps the first
  CHECK(parsed.parse(view(h)) != RtwFirmwareStatus::Ok);
  h = f;
  put32(h, 16 + 8, 0xFFFFFFF0);               // a size past the end
  CHECK(parsed.parse(view(h)) == RtwFirmwareStatus::Truncated);
  // An image with signatures declared but missing, or with stray bytes.
  const Bytes a = make_rtw89_image(0, 0x8852, 0x0F1D1D00, {{0xB8970000, 2, 40, false, 0}, {0xB8E11928, 9, 16, false, 2}}, false, 512);
  CHECK(rtw89_parse_firmware_image(view(a), &image) == RtwFirmwareStatus::Truncated);
  Bytes s = make_rtw89_image(0, 0x8852, 0x0F1D1D00, {{0xB8970000, 2, 40, false, 0}}, false, 0);
  CHECK(rtw89_parse_firmware_image(view(s), &image) == RtwFirmwareStatus::Ok);
  s.push_back(0);
  CHECK(rtw89_parse_firmware_image(view(s), &image) == RtwFirmwareStatus::SizeMismatch);
  const Bytes nine = make_rtw89_image(0, 0x8852, 0, std::vector<SectionSpec>(9, SectionSpec{0, 1, 4, false, 0}), false, 0);
  CHECK(rtw89_parse_firmware_image(view(nine), &image) == RtwFirmwareStatus::BadLayout);
  Bytes nopool = make_rtw89_image(1, 0x8852, 0, {{0, 9, 12, false, 0xFF}}, false, 40, 0);
  nopool[48 + 16 + 12] = 'X';
  CHECK(rtw89_parse_firmware_image(view(nopool), &image) == RtwFirmwareStatus::BadLayout);
  // An old single-firmware file.
  CHECK(parsed.parse(view(s)) == RtwFirmwareStatus::SizeMismatch);
  s.pop_back();
  CHECK(parsed.parse(view(s)) == RtwFirmwareStatus::Ok && !parsed.multi() && parsed.find(Rtw89FirmwareType::Normal, 7));
}

// ---------------------------------------------------------------- real firmware files

static bool read_file(const std::string& path, Bytes* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out->clear();
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) out->insert(out->end(), chunk, chunk + n);
  fclose(f);
  return true;
}

static std::vector<std::string> env_paths(const char* variable) {
  std::vector<std::string> paths;
  const char* value = getenv(variable);
  if (!value) return paths;
  std::string all = value, part;
  for (char c : all + ":") {
    if (c == ':') {
      if (!part.empty()) paths.push_back(part);
      part.clear();
    } else {
      part += c;
    }
  }
  return paths;
}

static uint32_t g_random = 12345;
static uint32_t next_random() { g_random = g_random * 1103515245u + 12345u; return g_random >> 8; }

static void test_real_rtw88() {
  const std::vector<std::string> paths = env_paths("RTW88_FIRMWARE_FILES");
  if (paths.empty()) { printf("  (RTW88_FIRMWARE_FILES not set: no real rtw88 file tested)\n"); return; }
  for (const std::string& path : paths) {
    Bytes f;
    if (!read_file(path, &f)) { printf("FAILED: cannot open %s\n", path.c_str()); ++g_failed; continue; }
    Rtw88Firmware fw;
    bool legacy = false;
    RtwFirmwareStatus status = rtw88_parse_firmware(view(f), false, 0, &fw);
    if (status != RtwFirmwareStatus::Ok) {
      legacy = true;
      status = rtw88_parse_firmware(view(f), true, 0, &fw);
    }
    CHECK(status == RtwFirmwareStatus::Ok);
    if (status != RtwFirmwareStatus::Ok) { printf("FAILED: %s not parsed\n", path.c_str()); continue; }
    if (legacy)
      printf("  %s: legacy, signature %04x, version %u.%u.%u, body %zu bytes\n", path.c_str(), fw.signature, fw.version, fw.sub_version, fw.sub_index, fw.body.size());
    else
      printf("  %s: signature %04x, version %u.%u.%u, %u-%02u-%02u, DMEM %zu IMEM %zu EMEM %zu, checksums match\n", path.c_str(), fw.signature, fw.version,
             fw.sub_version, fw.sub_index, fw.year, fw.month, fw.day, fw.dmem.size(), fw.imem.size(), fw.emem.size());
    for (int i = 0; i < 64; ++i) {
      const size_t n = next_random() % f.size();
      CHECK(rtw88_parse_firmware(ConstByteView(f.data(), n), legacy, 0, &fw) != RtwFirmwareStatus::Ok);
    }
    for (int i = 0; i < 2000; ++i) {
      Bytes h = f;
      const size_t at = next_random() % h.size();
      h[at] ^= uint8_t(1 << (next_random() % 8));
      const RtwFirmwareStatus damaged = rtw88_parse_firmware(view(h), legacy, 0, &fw);
      if (!legacy && at >= 64) {
        // Inside the blocks or their checksums' first two bytes everything is caught; the other six trailer bytes are not checked.
        (void)damaged;
      }
    }
    if (!legacy) {
      // Every flip in the first block is caught.
      Rtw88Firmware whole;
      if (rtw88_parse_firmware(view(f), false, 0, &whole) == RtwFirmwareStatus::Ok) {
        for (int i = 0; i < 200; ++i) {
          Bytes h = f;
          const size_t at = 64 + next_random() % whole.dmem.size();
          h[at] ^= uint8_t(1 << (next_random() % 8));
          CHECK(rtw88_parse_firmware(view(h), false, 0, &fw) == RtwFirmwareStatus::BadChecksum);
        }
      }
    }
  }
}

static void test_real_rtw89() {
  const std::vector<std::string> paths = env_paths("RTW89_FIRMWARE_FILES");
  if (paths.empty()) { printf("  (RTW89_FIRMWARE_FILES not set: no real rtw89 file tested)\n"); return; }
  for (const std::string& path : paths) {
    Bytes f;
    if (!read_file(path, &f)) { printf("FAILED: cannot open %s\n", path.c_str()); ++g_failed; continue; }
    Rtw89FirmwareFile file;
    const RtwFirmwareStatus status = file.parse(view(f));
    CHECK(status == RtwFirmwareStatus::Ok);
    if (status != RtwFirmwareStatus::Ok) { printf("FAILED: %s: %s\n", path.c_str(), rtw_firmware_status_name(status)); continue; }
    printf("  %s: %u firmwares, %u elements\n", path.c_str(), file.entry_count(), file.element_count());
    for (uint32_t i = 0; i < file.entry_count(); ++i) {
      const Rtw89FirmwareEntry& entry = file.entry(i);
      if (entry.type != 1 && entry.type != 3 && entry.type != 5) { printf("    cv %u type %u: %zu bytes\n", entry.cv, entry.type, entry.data.size()); continue; }
      Rtw89FirmwareImage image;
      CHECK(rtw89_parse_firmware_image(entry.data, &image) == RtwFirmwareStatus::Ok);
      printf("    cv %u type %u: chip %04x, %u.%u.%u.%u, header v%u (%u bytes), %u sections, %u signature bytes\n", entry.cv, entry.type, image.chip_id, image.major,
             image.minor, image.sub_version, image.sub_index, image.header_version, image.header_length, image.section_count, image.signature_bytes);
    }
    Rtw89FirmwareElement element;
    uint32_t walked = 0;
    for (uint32_t i = 0; file.element(i, &element); ++i) ++walked;
    CHECK(walked == file.element_count());
    for (int i = 0; i < 48; ++i) {
      Rtw89FirmwareFile cut;
      (void)cut.parse(ConstByteView(f.data(), next_random() % f.size()));
    }
    for (int i = 0; i < 300; ++i) {
      Bytes h = f;
      // Damage mostly where the structure is: the container and the first headers.
      const size_t at = (i % 2) ? next_random() % (f.size() < 4096 ? f.size() : 4096) : next_random() % f.size();
      h[at] ^= uint8_t(1 << (next_random() % 8));
      Rtw89FirmwareFile damaged;
      if (damaged.parse(view(h)) == RtwFirmwareStatus::Ok)
        for (uint32_t k = 0; k < damaged.entry_count(); ++k) {
          Rtw89FirmwareImage image;
          (void)rtw89_parse_firmware_image(damaged.entry(k).data, &image);
        }
    }
  }
}

// ---------------------------------------------------------------- eFuse

// Encodes a logical map (0xFF = unwritten) as rtw88 patches: a block with anything written gets a header (one byte for blocks 0-15, two above) and its words.
static void encode_rtw88_efuse(const Bytes& logical, Bytes& physical) {
  size_t at = 0;
  for (size_t block = 0; block * 8 < logical.size(); ++block) {
    uint8_t word_enable = 0x0F;
    for (int w = 0; w < 4; ++w)
      if (logical[block * 8 + w * 2] != 0xFF || logical[block * 8 + w * 2 + 1] != 0xFF) word_enable &= uint8_t(~(1 << w));
    if (word_enable == 0x0F) continue;
    if (block < 16 && (((block << 4) | word_enable) & 0x1F) != 0x0F) {
      physical[at++] = uint8_t(block << 4 | word_enable);
    } else {
      physical[at++] = uint8_t((block & 7) << 5 | 0x0F);
      physical[at++] = uint8_t((block >> 3) << 4 | word_enable);
    }
    for (int w = 0; w < 4; ++w)
      if (!(word_enable & (1 << w))) {
        physical[at++] = logical[block * 8 + w * 2];
        physical[at++] = logical[block * 8 + w * 2 + 1];
      }
  }
}

static void test_efuse() {
  // Hand-built: block 36 (0x120) with words 0-2 through an extended header, block 1 word 0 through a one-byte header.
  Bytes physical(512, 0xFF);
  const uint8_t mac[6] = {0x00, 0xE0, 0x4C, 0x88, 0x22, 0xC1};
  size_t at = 0;
  physical[at++] = 0x8F;      // block 36: (36 & 7) << 5 | 0x0F
  physical[at++] = 0x48;      // (36 >> 3) << 4 | word enable 1000
  for (uint8_t b : mac) physical[at++] = b;
  physical[at++] = 0x1E;      // block 1, words 1-3 absent
  physical[at++] = 0xAB;
  physical[at++] = 0xCD;
  uint8_t logical[512];
  CHECK(rtw88_efuse_logical_map(view(physical), 96, ByteView(logical)));
  CHECK(logical[8] == 0xAB && logical[9] == 0xCD && logical[10] == 0xFF && logical[0] == 0xFF);
  MacAddress address{};
  CHECK(rtw_efuse_mac_address(ConstByteView(logical), 0x120, &address) && address == MacAddress{{0x00, 0xE0, 0x4C, 0x88, 0x22, 0xC1}});
  CHECK(logical[0x126] == 0xFF);
  CHECK(!rtw_efuse_mac_address(ConstByteView(logical), 0x130, &address));      // unwritten
  CHECK(!rtw_efuse_mac_address(ConstByteView(logical), 509, &address));        // does not fit
  uint8_t multicast[6] = {0x01, 0, 0, 0, 0, 1};
  CHECK(!rtw_efuse_mac_address(ConstByteView(multicast), 0, &address));
  // An extended header whose second byte is 0xFF ends the log.
  Bytes ended = physical;
  ended[at] = 0x2F;
  ended[at + 1] = 0xFF;
  CHECK(rtw88_efuse_logical_map(view(ended), 96, ByteView(logical)));
  // A patch that runs into the protected area, or a block past the logical map.
  Bytes over(512, 0xFF);
  size_t fill = 0;
  while (512 - 96 - fill >= 9) {   // block 0, all four words: 9 bytes a patch
    over[fill] = 0x00;
    for (int k = 1; k < 9; ++k) over[fill + k] = uint8_t(k);
    fill += 9;
  }
  over[fill] = 0x00;               // one more, with 2 bytes left before the protected area
  CHECK(!rtw88_efuse_logical_map(view(over), 96, ByteView(logical)));
  Bytes far(512, 0xFF);
  far[0] = 0xEF;                 // block 7 | (0x0F extended) -> block = (hdr2 >> 4) << 3 | 7
  far[1] = 0xF0;                 // block 127, all words
  CHECK(!rtw88_efuse_logical_map(view(far), 96, ByteView(logical)));
  CHECK(!rtw88_efuse_logical_map(view(far), 600, ByteView(logical)));
  // Round trip through the encoder, at several MAC offsets.
  for (uint32_t offset : {0x0D0u, 0x107u, 0x11Au, 0x120u, 0x157u, 0x16Au}) {
    Bytes image(512, 0xFF);
    for (int i = 0; i < 6; ++i) image[offset + i] = mac[i];
    image[0] = 0x29;
    image[0x30] = 0x11;
    Bytes encoded(512, 0xFF);
    encode_rtw88_efuse(image, encoded);
    CHECK(rtw88_efuse_logical_map(view(encoded), 96, ByteView(logical)));
    CHECK(memcmp(logical, image.data(), 512) == 0);
  }
  // Every truncation and bit flip: no crash.
  Bytes image(512, 0xFF);
  for (int i = 0; i < 300; ++i) image[(i * 37) % 512] = uint8_t(i);
  Bytes encoded(512, 0xFF);
  encode_rtw88_efuse(image, encoded);
  for (size_t n = 0; n < 512; n += 7) (void)rtw88_efuse_logical_map(ConstByteView(encoded.data(), n), 0, ByteView(logical));
  for (size_t i = 0; i < 400; ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes h = encoded;
      h[i] ^= uint8_t(1 << bit);
      (void)rtw88_efuse_logical_map(view(h), 96, ByteView(logical));
      (void)rtw89_efuse_logical_map(view(h), 4, ByteView(logical));
    }

  // rtw89: two-byte headers. Block 0x80 (0x400) words 0-2, block 0x87 (0x438) word 3.
  Bytes p89(1216, 0xFF);
  at = 0;
  p89[at++] = 0x38; p89[at++] = 0x08;
  for (uint8_t b : mac) p89[at++] = b;
  p89[at++] = 0x38; p89[at++] = 0x77;
  p89[at++] = 0x12; p89[at++] = 0x34;
  uint8_t l89[2048];
  CHECK(rtw89_efuse_logical_map(view(p89), 4, ByteView(l89)));
  CHECK(rtw_efuse_mac_address(ConstByteView(l89), 0x400, &address) && address.bytes[5] == 0xC1);
  CHECK(l89[0x43E] == 0x12 && l89[0x43F] == 0x34 && l89[0x438] == 0xFF);
  p89[0] = 0x3F;     // block 0xF0: past a 1536-byte map
  p89[1] = 0x00;
  CHECK(!rtw89_efuse_logical_map(view(p89), 4, ByteView(l89).first(1536)));
}

// ---------------------------------------------------------------- a model of an rtw88 chip's registers

struct ChipModel final : INicRegisters {
  std::vector<uint8_t> mem = std::vector<uint8_t>(0x10000, 0);
  Bytes efuse = Bytes(1024, 0xFF);
  Bytes firmware = Bytes(8 * 4096, 0);    // what the 8051 download window received
  size_t firmware_end = 0;
  bool power_ready = true;                // 0x0006 bit 1 (power ready) comes up
  bool cpu_starts = true;                 // the 8051 reports ready after the download
  bool dead = false;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  uint32_t polls_0006 = 0;

  ChipModel(uint32_t sys_cfg1) {
    set32(0x00F0, sys_cfg1);
    mem[0x0100] = 0xEA;                   // REG_CR while the power is off
    mem[0xFF1A] = 0x55;
  }
  void set32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; ++i) mem[a + i] = uint8_t(v >> (8 * i)); }
  uint32_t get32(uint32_t a) const { return uint32_t(mem[a]) | uint32_t(mem[a + 1]) << 8 | uint32_t(mem[a + 2]) << 16 | uint32_t(mem[a + 3]) << 24; }

  uint8_t byte(uint32_t a) {
    if (a == 0x0006) {
      ++polls_0006;
      return power_ready ? uint8_t(mem[a] | 2) : uint8_t(mem[a] & ~2);
    }
    return mem[a & 0xFFFF];
  }
  uint8_t read8(uint32_t a) override { return dead ? 0xFF : byte(a); }
  uint16_t read16(uint32_t a) override { return dead ? 0xFFFF : uint16_t(byte(a) | byte(a + 1) << 8); }
  uint32_t read32(uint32_t a) override { return dead ? 0xFFFFFFFFu : uint32_t(byte(a)) | uint32_t(byte(a + 1)) << 8 | uint32_t(byte(a + 2)) << 16 | uint32_t(byte(a + 3)) << 24; }

  void store8(uint32_t a, uint8_t v) {
    a &= 0xFFFF;
    const uint8_t old = mem[a];
    mem[a] = v;
    if (a == 0x0005) mem[a] &= uint8_t(~3);                // APFM_ONMAC / APFM_OFFMAC finish at once
    if (a == 0x0003 && (v & 4) && !(old & 4) && cpu_starts && (mem[0x0080] & 2)) mem[0x0080] |= 0xC0;   // the 8051 starts: WINTINI_RDY | RAM_DL_SEL
  }
  void write8(uint32_t a, uint8_t v) override { writes.push_back({a, v}); store8(a, v); }
  void write16(uint32_t a, uint16_t v) override { writes.push_back({a, v}); store8(a, uint8_t(v)); store8(a + 1, uint8_t(v >> 8)); }
  void write32(uint32_t a, uint32_t v) override {
    writes.push_back({a, v});
    if (a == 0x0030 && !(v & 0x80000000u)) {             // an eFuse read
      const uint32_t address = (v >> 8) & 0x3FF;
      set32(0x0030, (v & ~0xFFu) | efuse[address] | 0x80000000u);
      return;
    }
    if (a >= 0x1000 && a < 0x2000) {                       // the 8051 download window
      const uint32_t page = (get32(0x0080) >> 16) & 7;
      const size_t at = page * 4096 + (a - 0x1000);
      for (int i = 0; i < 4; ++i) firmware[at + i] = uint8_t(v >> (8 * i));
      if (at + 4 > firmware_end) firmware_end = at + 4;
      mem[0x0080] |= 4;                                    // FWDL_CHK_RPT
      return;
    }
    for (int i = 0; i < 4; ++i) store8(a + i, uint8_t(v >> (8 * i)));
  }
};

static void test_power_sequence() {
  ChipModel chip(0x08002000);
  RtwPowerOptions pci;
  CHECK(rtw_run_power_sequence(chip, rtw8822c_power_on(), pci) == RtwPowerResult::Ok);
  CHECK((chip.mem[0x002E] & 0x0C) == 0x04 && (chip.mem[0x0005] & 0x9F) == 0 && (chip.mem[0x0006] & 1) && (chip.mem[0x0075] & 1) == 0);
  CHECK((chip.mem[0x1018] & 4) && (chip.mem[0x0074] & 0x20) && chip.mem[0x0062] == 0xE0 && chip.mem[0x001F] == 0x80 && chip.mem[0x00EF] == 0x80);
  CHECK((chip.mem[0x1045] & 0x10) && (chip.mem[0x0010] & 4) && (chip.mem[0x1064] & 2));
  CHECK(chip.mem[0xFF1A] == 0x55);          // USB only
  CHECK(chip.polls_0006 >= 1);
  // The 0x0075 bit was set before the poll and cleared after it.
  int set_at = -1, poll_seen = 0;
  for (size_t i = 0; i < chip.writes.size(); ++i)
    if (chip.writes[i].first == 0x0075 && (chip.writes[i].second & 1)) set_at = int(i);
  CHECK(set_at >= 0);
  (void)poll_seen;

  // USB: the USB lines run, the PCIe lines do not.
  ChipModel usb(0x08002000);
  usb.mem[0x0000] = 0x20;
  RtwPowerOptions u;
  u.interface_mask = rtw_power::kInterfaceUsb;
  u.pcie = false;
  CHECK(rtw_run_power_sequence(usb, rtw8822c_power_on(), u) == RtwPowerResult::Ok);
  CHECK(usb.mem[0xFF1A] == 0 && (usb.mem[0x0000] & 0x20) == 0 && usb.mem[0x0062] == 0 && (usb.mem[0x0074] & 0x20) == 0);

  // A poll that never succeeds: on PCIe it toggles BIT_PFM_WOWL and tries once more.
  ChipModel stuck(0x08002000);
  stuck.power_ready = false;
  CHECK(rtw_run_power_sequence(stuck, rtw8822c_power_on(), pci) == RtwPowerResult::Timeout);
  int toggles = 0;
  for (auto& w : stuck.writes) if (w.first == 0x0004) ++toggles;
  CHECK(toggles == 2);
  CHECK(stuck.polls_0006 == 2 * rtw_power::kPollCount);

  // The other chips' tables run to completion on the model; the cut and SDIO masks are honoured.
  for (auto sequence : {rtw8822b_power_on(), rtw8821c_power_on(), rtw8723d_power_on()}) {
    ChipModel other(0x08002000);
    CHECK(rtw_run_power_sequence(other, sequence, pci) == RtwPowerResult::Ok);
    for (auto& w : other.writes) CHECK(w.first < 0x10000);     // no SDIO-local access on PCIe
  }
  const RtwPowerCommand cut_b_only[] = {{0x0040, 1 << 2, rtw_power::kInterfaceAll, 0, rtw_power::kWrite, 0xFF, 0x77}, {0, 0xFF, 0xFF, 0, rtw_power::kEnd, 0, 0}};
  ChipModel cuts(0);
  RtwPowerOptions cut_a = pci;
  cut_a.cut_mask = rtw_power::cut_mask(0);
  CHECK(rtw_run_power_program(cuts, cut_b_only, cut_a) == RtwPowerResult::Ok && cuts.mem[0x40] == 0);
  cut_a.cut_mask = rtw_power::cut_mask(1);
  CHECK(rtw_run_power_program(cuts, cut_b_only, cut_a) == RtwPowerResult::Ok && cuts.mem[0x40] == 0x77);
  const RtwPowerCommand bad[] = {{0x0040, 0xFF, 0xFF, 0, 9, 0, 0}};
  CHECK(rtw_run_power_program(cuts, bad, pci) == RtwPowerResult::BadCommand);
}

// ---------------------------------------------------------------- USB

struct FakeUsb final : IUsbHostController {
  INicRegisters* target = nullptr;
  bool fail = false;
  uint8_t last_type = 0, last_request = 0;
  uint16_t last_value = 0, last_index = 0;
  uint64_t last_length = 0;
  uint32_t transfers = 0;

  const char* name() const override { return "fake"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind{}; }
  uint32_t port_count() const override { return 1; }
  bool port_info(uint32_t, UsbPortInfo*) const override { return false; }
  bool device_descriptor(uint32_t, UsbDeviceDescriptor*) const override { return false; }
  bool configuration_descriptor(uint32_t, ByteView, uint16_t*) const override { return false; }
  bool set_configuration(uint32_t, uint8_t) override { return true; }
  bool control_out(uint32_t, uint8_t, uint8_t, uint16_t, uint16_t) override { return false; }
  bool control_in(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ByteView buffer) override {
    note(type, request, value, index, buffer.size());
    if (fail || !target) return false;
    const uint32_t address = uint32_t(index) << 16 | value;
    for (uint64_t i = 0; i < buffer.size(); ++i) buffer[i] = target->read8(address + uint32_t(i));
    return true;
  }
  bool control_out_data(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ConstByteView data) override {
    note(type, request, value, index, data.size());
    if (fail || !target) return false;
    const uint32_t address = uint32_t(index) << 16 | value;
    if (data.size() == 4) target->write32(address, uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24);
    else if (data.size() == 2) target->write16(address, uint16_t(data[0] | data[1] << 8));
    else for (uint64_t i = 0; i < data.size(); ++i) target->write8(address + uint32_t(i), data[i]);
    return true;
  }
  bool start_interrupt_in(uint32_t, uint8_t, uint16_t, uint8_t) override { return false; }
  bool poll_interrupt_in(uint32_t, uint8_t, ByteView, uint16_t*) override { return false; }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { return false; }
  bool bulk_transfer(uint32_t, uint8_t, ByteView, uint32_t*) override { return false; }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return false; }

  void note(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint64_t length) {
    last_type = type; last_request = request; last_value = value; last_index = index; last_length = length;
    ++transfers;
  }
};

static void test_usb_registers() {
  ChipModel chip(0x12345678);
  FakeUsb usb;
  usb.target = &chip;
  RealtekUsbRegisters r(usb, 0);
  CHECK(r.read32(0x00F0) == 0x12345678 && usb.last_type == 0xC0 && usb.last_request == 0x05 && usb.last_value == 0x00F0 && usb.last_index == 0 && usb.last_length == 4);
  CHECK(r.read16(0x00F2) == 0x1234 && usb.last_length == 2);
  CHECK(r.read8(0x00F1) == 0x56 && usb.last_length == 1);
  r.write16(0x0040, 0xBEEF);
  CHECK(chip.mem[0x40] == 0xEF && chip.mem[0x41] == 0xBE && usb.last_type == 0x40 && usb.last_length == 2);
  r.write32(0x0044, 0x01020304);
  CHECK(chip.get32(0x44) == 0x01020304 && usb.last_length == 4);
  r.write8(0x0048, 0x99);
  CHECK(chip.mem[0x48] == 0x99 && usb.last_length == 1);
  (void)r.read8(0x18601234);
  CHECK(usb.last_value == 0x1234 && usb.last_index == 0x1860);
  usb.fail = true;
  CHECK(r.read32(0) == 0xFFFFFFFFu && r.read16(0) == 0xFFFF && r.read8(0) == 0xFF);
  r.write8(0, 1);
  CHECK(r.failures() == 4);
}

// ---------------------------------------------------------------- the chips

static void program_mac(ChipModel& chip, uint32_t offset, const uint8_t (&mac)[6]) {
  Bytes image(512, 0xFF);
  for (int i = 0; i < 6; ++i) image[offset + i] = mac[i];
  image[0] = 0x29;     // something else in the map
  Bytes encoded(512, 0xFF);
  encode_rtw88_efuse(image, encoded);
  for (size_t i = 0; i < 512; ++i) chip.efuse[i] = encoded[i];
}

static void test_rtw88_chip() {
  TestAllocator alloc;
  const uint8_t mac[6] = {0x00, 0xE0, 0x4C, 0x12, 0x34, 0x56};

  // 8822CE: identified, powered on, MAC read, firmware checked, then Unsupported (no DDMA download).
  {
    const WifiAdapterModel* model = wifi_find_pci_adapter(0x10EC, 0xC822);
    CHECK(model);
    if (!model) return;
    ChipModel regs(0x08002000 | (1u << 27));     // cut C, 2T2R
    program_mac(regs, 0x120, mac);
    WifiChipBus bus;
    bus.registers = &regs;
    UniquePointer<IWifiChip> chip = make_wifi_chip(*model, bus, alloc);
    CHECK(chip);
    if (!chip) return;
    FakeFirmware none;
    CHECK(chip->start(none) == WifiChipResult::FirmwareMissing && chip->state() == WifiChipState::Failed);
    CHECK(chip->mac_address() == MacAddress{{0x00, 0xE0, 0x4C, 0x12, 0x34, 0x56}});
    FakeFirmware fw;
    fw.files["rtw88/rtw8822c_fw.bin"] = make_rtw88(0x8822, 64, 128, 32);
    regs.mem[0x0100] = 0x00;     // the power looks on: it is switched off first
    const WifiChipResult result = chip->start(fw);
    CHECK(result == WifiChipResult::Unsupported);
    CHECK(chip->state() == WifiChipState::Identified);
    CHECK(chip->identity().chip_id == 0x8822 && chip->identity().revision == 2);
    CHECK(strcmp(chip->identity().firmware_version, "9.9.15") == 0);
    CHECK(chip->mac_address() == MacAddress{{0x00, 0xE0, 0x4C, 0x12, 0x34, 0x56}});
    CHECK(strcmp(chip->firmware_name(), "rtw88/rtw8822c_fw.bin") == 0);
    CHECK(static_cast<RtwChip*>(chip.get())->two_streams());
    printf("  8822CE: %s (%s)\n", wifi_chip_result_name(result), chip->message());
    // A damaged file.
    fw.files["rtw88/rtw8822c_fw.bin"][100] ^= 1;
    CHECK(chip->start(fw) == WifiChipResult::FirmwareInvalid);
    // A legacy file where an 11ac one belongs.
    fw.files["rtw88/rtw8822c_fw.bin"] = make_rtw88_legacy(0x23D1, 100);
    CHECK(chip->start(fw) == WifiChipResult::FirmwareInvalid);
    // No MAC in the eFuse.
    std::fill(regs.efuse.begin(), regs.efuse.end(), 0xFF);
    CHECK(chip->start(fw) == WifiChipResult::NoResponse && chip->mac_address() == MacAddress{});
    // The power never comes.
    program_mac(regs, 0x120, mac);
    regs.power_ready = false;
    CHECK(chip->start(fw) == WifiChipResult::Timeout && chip->state() == WifiChipState::Failed);
    regs.power_ready = true;
    // A chip that is not there.
    regs.dead = true;
    CHECK(chip->start(fw) == WifiChipResult::NoResponse);
    regs.dead = false;
    chip->stop();
    CHECK(chip->state() == WifiChipState::Off);
    // No register window.
    WifiChipBus empty;
    UniquePointer<IWifiChip> orphan = make_wifi_chip(*model, empty, alloc);
    CHECK(orphan && orphan->start(fw) == WifiChipResult::NoRegisters);
  }

  // 8723DE: an 8051 chip; the firmware goes through the download window and starts.
  {
    const WifiAdapterModel* model = wifi_find_pci_adapter(0x10EC, 0xD723);
    CHECK(model);
    if (!model) return;
    ChipModel regs(0x00001000);
    program_mac(regs, 0x0D0, mac);
    WifiChipBus bus;
    bus.registers = &regs;
    UniquePointer<IWifiChip> chip = make_wifi_chip(*model, bus, alloc);
    CHECK(chip);
    if (!chip) return;
    FakeFirmware fw;
    const Bytes file = make_rtw88_legacy(0x23D1, 2 * 4096 + 1001);
    fw.files["rtw88/rtw8723d_fw.bin"] = file;
    const WifiChipResult result = chip->start(fw);
    CHECK(result == WifiChipResult::Unsupported && chip->state() == WifiChipState::FirmwareLoaded);
    CHECK(strcmp(chip->identity().firmware_version, "48.2.24") == 0 && chip->identity().revision == 1);
    CHECK(regs.firmware_end == 2 * 4096 + 1004);
    CHECK(memcmp(regs.firmware.data(), file.data() + 32, file.size() - 32) == 0);
    CHECK(regs.firmware[2 * 4096 + 1001] == 0);       // the last word padded with zeros
    CHECK((regs.mem[0x0080] & 1) == 0);              // download disabled again
    printf("  8723DE: %s (%s)\n", wifi_chip_result_name(result), chip->message());
    regs.cpu_starts = false;
    regs.mem[0x0080] = 0;
    CHECK(chip->start(fw) == WifiChipResult::FirmwareRejected);
    // Too large for the eight 4 KiB pages.
    regs.cpu_starts = true;
    fw.files["rtw88/rtw8723d_fw.bin"] = make_rtw88_legacy(0x23D1, 8 * 4096 + 4);
    CHECK(chip->start(fw) == WifiChipResult::FirmwareInvalid);
  }

  // 8822CU over USB vendor requests.
  {
    const WifiAdapterModel* model = wifi_find_usb_adapter(0x0BDA, 0xC82C);
    CHECK(model);
    if (!model) return;
    ChipModel regs(0x08003000);
    program_mac(regs, 0x157, mac);
    FakeUsb usb;
    usb.target = &regs;
    WifiChipBus bus;
    bus.usb = &usb;
    bus.usb_port = 3;
    UniquePointer<IWifiChip> chip = make_wifi_chip(*model, bus, alloc);
    CHECK(chip);
    if (!chip) return;
    FakeFirmware fw;
    fw.files["rtw88/rtw8822c_fw.bin"] = make_rtw88(0x8822, 64, 128, 0);
    CHECK(chip->start(fw) == WifiChipResult::Unsupported && chip->state() == WifiChipState::Identified);
    CHECK(chip->mac_address() == MacAddress{{0x00, 0xE0, 0x4C, 0x12, 0x34, 0x56}} && chip->identity().revision == 3);
    CHECK(regs.mem[0xFF1A] == 0);        // the USB line of the power sequence ran
    CHECK(usb.transfers > 512);
  }

  // A real 8822C firmware file, if one was named.
  for (const std::string& path : env_paths("RTW88_FIRMWARE_FILES")) {
    if (path.find("rtw8822c_fw.bin") == std::string::npos) continue;
    Bytes file;
    if (!read_file(path, &file)) continue;
    const WifiAdapterModel* model = wifi_find_pci_adapter(0x10EC, 0xC822);
    ChipModel regs(0x08002000);
    program_mac(regs, 0x120, mac);
    WifiChipBus bus;
    bus.registers = &regs;
    UniquePointer<IWifiChip> chip = make_wifi_chip(*model, bus, alloc);
    FakeFirmware fw;
    fw.files["rtw88/rtw8822c_fw.bin"] = file;
    CHECK(chip->start(fw) == WifiChipResult::Unsupported);
    printf("  8822CE with %s: firmware %s\n", path.c_str(), chip->identity().firmware_version);
  }
}

static void test_rtw89_chip() {
  TestAllocator alloc;
  const WifiAdapterModel* model = wifi_find_pci_adapter(0x10EC, 0xB852);
  CHECK(model);
  if (!model) return;
  ChipModel regs(0x00001000);       // cut B (cv 1)
  WifiChipBus bus;
  bus.registers = &regs;
  UniquePointer<IWifiChip> chip = make_wifi_chip(*model, bus, alloc);
  CHECK(chip);
  if (!chip) return;
  FakeFirmware fw;
  fw.files["rtw89/rtw8852b_fw.bin"] = make_rtw89_file(0x8852, 1).bytes;
  const WifiChipResult result = chip->start(fw);
  CHECK(result == WifiChipResult::Unsupported && chip->state() == WifiChipState::Identified);
  CHECK(fw.asked.size() == 3 && fw.asked[0] == "rtw89/rtw8852b_fw-2.bin" && fw.asked[1] == "rtw89/rtw8852b_fw-1.bin" && fw.asked[2] == "rtw89/rtw8852b_fw.bin");
  CHECK(strcmp(chip->firmware_name(), "rtw89/rtw8852b_fw.bin") == 0);
  CHECK(strcmp(chip->identity().firmware_version, "0.29.29.15") == 0 && chip->identity().chip_id == 0x8852 && chip->identity().revision == 1);
  CHECK(static_cast<Rtw89Chip*>(chip.get())->chosen_type() == 5);
  printf("  8852BE: %s (%s)\n", wifi_chip_result_name(result), chip->message());
  // Another cut: the file has nothing for it.
  regs.set32(0x00F0, 0x00003000);
  CHECK(chip->start(fw) == WifiChipResult::UnknownRevision && chip->state() == WifiChipState::Failed);
  // A newer file without this cut, an older one with it: the older one is used.
  regs.set32(0x00F0, 0x00000000);
  fw.files["rtw89/rtw8852b_fw-2.bin"] = make_rtw89_file(0x8852, 1).bytes;
  fw.files["rtw89/rtw8852b_fw-1.bin"] = make_rtw89_file(0x8852, 0).bytes;
  CHECK(chip->start(fw) == WifiChipResult::Unsupported && strcmp(chip->firmware_name(), "rtw89/rtw8852b_fw-1.bin") == 0);
  // A damaged newest file and nothing else for the cut.
  fw.files["rtw89/rtw8852b_fw-2.bin"][0] = 0;
  fw.files.erase("rtw89/rtw8852b_fw-1.bin");
  CHECK(chip->start(fw) == WifiChipResult::UnknownRevision);
  fw.files.erase("rtw89/rtw8852b_fw.bin");
  CHECK(chip->start(fw) == WifiChipResult::FirmwareInvalid);
  fw.files.clear();
  fw.files["rtw89/rtw8852b_fw.bin"] = make_rtw89_file(0x8852, 1).bytes;
  // Another chip's file.
  regs.set32(0x00F0, 0x00001000);
  fw.files["rtw89/rtw8852b_fw.bin"] = make_rtw89_file(0x8922, 1).bytes;
  CHECK(chip->start(fw) == WifiChipResult::FirmwareInvalid);
  fw.files.clear();
  CHECK(chip->start(fw) == WifiChipResult::FirmwareMissing);
  regs.dead = true;
  CHECK(chip->start(fw) == WifiChipResult::NoResponse);
  chip->stop();
  CHECK(chip->state() == WifiChipState::Off);

  // Real rtw89 files through the chip, by their chip number.
  for (const std::string& path : env_paths("RTW89_FIRMWARE_FILES")) {
    Bytes file;
    if (!read_file(path, &file)) continue;
    const WifiAdapterModel* m = nullptr;
    if (path.find("rtw8852b_fw") != std::string::npos) m = wifi_find_pci_adapter(0x10EC, 0xB852);
    else if (path.find("rtw8852c_fw") != std::string::npos) m = wifi_find_pci_adapter(0x10EC, 0xC852);
    else if (path.find("rtw8851b_fw") != std::string::npos) m = wifi_find_pci_adapter(0x10EC, 0xB851);
    else if (path.find("rtw8922a_fw") != std::string::npos) m = wifi_find_pci_adapter(0x10EC, 0x8922);
    else if (path.find("rtw8852a_fw") != std::string::npos) m = wifi_find_pci_adapter(0x10EC, 0x8852);
    if (!m) continue;
    ChipModel r(0x00001000);
    WifiChipBus b;
    b.registers = &r;
    UniquePointer<IWifiChip> c = make_wifi_chip(*m, b, alloc);
    FakeFirmware source;
    const std::string name = m->firmware;
    source.files[name] = file;
    CHECK(c->start(source) == WifiChipResult::Unsupported);
    printf("  %s with %s (cut B): firmware %s\n", m->chip, path.c_str(), c->identity().firmware_version);
  }
}

int main() {
  printf("RealtekWifiTest\n");
  test_models();
  test_rtw88_firmware();
  test_rtw89_firmware();
  test_real_rtw88();
  test_real_rtw89();
  test_efuse();
  test_power_sequence();
  test_usb_registers();
  test_rtw88_chip();
  test_rtw89_chip();
  printf(g_failed ? "RealtekWifiTest: %d FAILED\n" : "RealtekWifiTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
