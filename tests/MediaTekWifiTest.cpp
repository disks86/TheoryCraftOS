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

// The MediaTek Wi-Fi layer (src/Hardware/WirelessInterface/MediaTek): the adapter table, the firmware parsers (connac2 ROM patch, connac2 RAM code, MT76x2U/MT76x0U/MT7601U
// USB firmware) on synthetic files with truncation and bit flips and, when MEDIATEK_FIRMWARE_DIR names a folder laid out like linux-firmware (decompressed files), on the
// real ones; the MCU message layout; the MT7921 address remap; Mt7921Chip against a model of the chip's registers and WFDMA rings that plays the MCU's side of the
// firmware download; MtUsbChip against a model of the vendor requests.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "Hardware/WirelessInterface/MediaTek/Mt7921Chip.hpp"
#include "Hardware/WirelessInterface/MediaTek/Mt7921Pcie.hpp"
#include "Hardware/WirelessInterface/MediaTek/MtFirmware.hpp"
#include "Hardware/WirelessInterface/MediaTek/MtMcu.hpp"
#include "Hardware/WirelessInterface/MediaTek/MtUsb.hpp"
#include "Hardware/WirelessInterface/WifiAdapterCatalog.hpp"
#include "Hardware/WirelessInterface/WifiChipFactory.hpp"

namespace hardware {
void delay_us(uint64_t) {}
}  // namespace hardware

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::map<void*, size_t> dma;
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  // memory the "chip" reads: in the low 2 GiB like the kernel's DMA pool, so that the pointer is the bus address
  void* allocate_dma(uint64_t size, uint64_t) override {
    const size_t rounded = (size + 4095) / 4096 * 4096;
    void* p = mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    dma[p] = rounded;
    return p;
  }
  void deallocate(void* p) override {
    if (!p) return;
    auto it = dma.find(p);
    if (it != dma.end()) { munmap(p, it->second); dma.erase(it); } else free(p);
  }
};

typedef std::vector<uint8_t> Bytes;
static void put32le(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
static void put16le(Bytes& b, uint16_t v) { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8)); }
static void put32be(Bytes& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
static void put_text(Bytes& b, const char* text, size_t width) { for (size_t i = 0; i < width; ++i) b.push_back(i < strlen(text) ? static_cast<uint8_t>(text[i]) : 0); }
static uint32_t get32le(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
static uint16_t get16le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }
static Bytes pattern(size_t n, uint8_t seed) { Bytes b(n); for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8)); return b; }

static Bytes read_file(const std::string& path) {
  Bytes file;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return file;
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) file.insert(file.end(), chunk, chunk + n);
  fclose(f);
  return file;
}

// ---- table ----------------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_table() {
  const ArrayView<const WifiAdapterModel> models = mediatek_wifi_models();
  CHECK(models.size() >= 40);
  std::set<std::tuple<int, int, int>> seen;
  for (const WifiAdapterModel& m : models) {
    CHECK(m.vendor == WifiVendor::MediaTek);
    CHECK(wifi_driver_family_vendor(m.family) == WifiVendor::MediaTek);
    CHECK(seen.insert(std::make_tuple(static_cast<int>(m.bus), m.vendor_id, m.device_id)).second);
    CHECK(m.chip && *m.chip && m.product && *m.product && m.firmware);
    CHECK(wifi_firmware_name_valid(StringView(m.firmware)));
    CHECK(m.spatial_streams >= 1 && m.spatial_streams <= 4);
    if (m.bus == WifiBus::Pci) CHECK(m.vendor_id == 0x14C3);
    // the RAM code file a connac2/3 chip's table entry names is the one of a known chip id
    if (m.family == WifiDriverFamily::Mt7921 || m.family == WifiDriverFamily::Mt7925) {
      bool found = false;
      for (uint32_t id : {0x7961u, 0x7922u, 0x7920u, 0x7925u}) {
        const char* patch;
        const char* ram;
        if (mt792x_firmware_names(id, &patch, &ram) && strcmp(ram, m.firmware) == 0) found = true;
      }
      CHECK(found);
    }
  }
  // the IDs asked for
  for (uint16_t id : {0x7961, 0x0608, 0x0616, 0x7922, 0x7920, 0x7925, 0x0717, 0x7615, 0x7663}) CHECK(wifi_find_pci_adapter(0x14C3, id) != nullptr);
  const uint16_t usb[][2] = {{0x0E8D, 0x7961}, {0x3574, 0x6211}, {0x0846, 0x9060}, {0x35BC, 0x0107}, {0x0E8D, 0x7612}, {0x0B05, 0x1833}, {0x0B05, 0x17EB},
                             {0x0B05, 0x180B}, {0x057C, 0x8503}, {0x7392, 0xB711}, {0x2357, 0x0137}, {0x0E8D, 0x7610}, {0x148F, 0x7610}, {0x2357, 0x0105},
                             {0x148F, 0x7601}, {0x0E8D, 0x760B}, {0x2717, 0x4106}};
  for (const auto& id : usb) CHECK(wifi_find_usb_adapter(id[0], id[1]) != nullptr);
  CHECK(wifi_find_pci_adapter(0x14C3, 0x0616)->family == WifiDriverFamily::Mt7921 && strcmp(wifi_find_pci_adapter(0x14C3, 0x0616)->firmware, "mediatek/WIFI_RAM_CODE_MT7922_1.bin") == 0);
  CHECK(wifi_find_pci_adapter(0x14C3, 0x0717)->family == WifiDriverFamily::Mt7925 && wifi_find_pci_adapter(0x14C3, 0x0717)->standard == WifiStandard::Wifi7);
  CHECK(wifi_find_usb_adapter(0x7392, 0xB711)->family == WifiDriverFamily::Mt76x2u);
  CHECK(wifi_find_usb_adapter(0x148F, 0x7601)->family == WifiDriverFamily::Mt7601u && strcmp(wifi_find_usb_adapter(0x148F, 0x7601)->firmware, "mt7601u.bin") == 0);
  CHECK(wifi_find_pci_adapter(0x14C3, 0x1234) == nullptr && wifi_find_usb_adapter(0x14C3, 0x7961) == nullptr);
  CHECK(wifi_adapter_count() >= models.size());     // (every vendor's table counts)
  const char* p;
  const char* r;
  CHECK(mt792x_firmware_names(0x7961, &p, &r) && strcmp(p, "mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin") == 0);
  CHECK(mt792x_firmware_names(0x7925, &p, &r) && strcmp(p, "mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin") == 0);
  CHECK(!mt792x_firmware_names(0x7663, &p, &r));
}

// ---- firmware files -------------------------------------------------------------------------------------------------------------------------------------------------------

struct PatchSpec { uint32_t address; uint32_t key; size_t length; };

static Bytes make_patch(const std::vector<PatchSpec>& sections) {
  Bytes f;
  put_text(f, "20260101120000a\n", 16);
  put_text(f, "ALPS", 4);
  put32be(f, 0x8A108A10);
  put32be(f, 0xFFFFFFFF);
  f.push_back(0); f.push_back(0); f.push_back(0); f.push_back(0);    // checksum, rsv0
  put32be(f, 0x44332211);
  put32be(f, 4);
  put32be(f, 0);
  put32be(f, static_cast<uint32_t>(sections.size()));
  put32be(f, 0xFFFF);
  f.resize(96, 0);
  size_t offset = 96 + 64 * sections.size();
  for (const PatchSpec& s : sections) {
    put32be(f, 0x00040002);
    put32be(f, static_cast<uint32_t>(offset));
    put32be(f, static_cast<uint32_t>(s.length));
    put32be(f, s.address);
    put32be(f, static_cast<uint32_t>(s.length));
    put32be(f, s.key);
    put32be(f, 0);
    f.resize(f.size() + 36, 0);
    offset += s.length;
  }
  uint8_t seed = 1;
  for (const PatchSpec& s : sections) { Bytes d = pattern(s.length, seed++); f.insert(f.end(), d.begin(), d.end()); }
  return f;
}

struct RamSpec { uint32_t address; uint8_t features; size_t length; };

static Bytes make_ram(const std::vector<RamSpec>& regions) {
  Bytes f;
  uint8_t seed = 0x40;
  for (const RamSpec& r : regions) { Bytes d = pattern(r.length, seed++); f.insert(f.end(), d.begin(), d.end()); }
  f.resize(f.size() + 24, 0xEE);                      // padding between the data and the table, as real files have
  for (const RamSpec& r : regions) {
    put32le(f, 0); put32le(f, 0); put32le(f, 0); put32le(f, 0);
    put32le(f, r.address);
    put32le(f, static_cast<uint32_t>(r.length));
    f.push_back(r.features);
    f.push_back(0);
    f.resize(f.size() + 14, 0);
  }
  f.push_back(0x0D); f.push_back(0x01); f.push_back(static_cast<uint8_t>(regions.size())); f.push_back(2); f.push_back(1); f.push_back(0); f.push_back(0);
  put_text(f, "____010000", 10);
  put_text(f, "20260224110949", 15);
  put32le(f, 0x5775A48A);
  return f;
}

static Bytes make_usb_firmware(size_t ilm, size_t dlm, uint16_t fw_ver, uint16_t build) {
  Bytes f;
  put32le(f, static_cast<uint32_t>(ilm));
  put32le(f, static_cast<uint32_t>(dlm));
  put16le(f, build);
  put16le(f, fw_ver);
  put32le(f, 0);
  put_text(f, "201406241830____", 16);
  Bytes body = pattern(ilm + dlm, 9);
  f.insert(f.end(), body.begin(), body.end());
  return f;
}

static bool inside(ConstByteView part, const Bytes& file) { return part.empty() || (part.data() >= file.data() && part.data() + part.size() <= file.data() + file.size()); }

static void check_patch_consistent(const MtConnacPatch& patch, const Bytes& file) {
  for (uint32_t i = 0; i < patch.section_count(); ++i) {
    CHECK(inside(patch.section(i).data, file));
    CHECK(patch.section(i).data.size() == patch.section(i).length);
  }
}

static void check_ram_consistent(const MtConnacRam& ram, const Bytes& file) {
  uint64_t offset = 0;
  for (uint32_t i = 0; i < ram.region_count(); ++i) {
    const MtRamRegion& r = ram.region(i);
    CHECK(inside(r.data, file) && r.data.size() == r.length && r.file_offset == offset);
    offset += r.length;
  }
  CHECK(offset + MtConnacRam::kTrailerBytes + MtConnacRam::kRegionBytes * ram.region_count() <= file.size());
}

static void test_patch_parser() {
  const Bytes file = make_patch({{0x900000, 0x00000000, 5000}, {0xE0002800, 0x01000002, 300}, {0x200000, 0x02000000, 17}});
  MtConnacPatch patch;
  const char* error = nullptr;
  CHECK(patch.parse(view(file), &error) && error == nullptr);
  CHECK(strcmp(patch.build_date(), "20260101120000a") == 0 && strcmp(patch.platform(), "ALPS") == 0);
  CHECK(patch.hw_sw_version() == 0x8A108A10 && patch.patch_version() == 0xFFFFFFFF && patch.section_count() == 3);
  CHECK(patch.section(0).address == 0x900000 && patch.section(0).length == 5000 && patch.section(0).file_offset == 96 + 3 * 64);
  CHECK(patch.section(0).data.equals(view(pattern(5000, 1))) && patch.section(2).data.equals(view(pattern(17, 3))));
  // plain; AES with key 2 (key index field, bits 1-2); scramble
  CHECK(patch.download_mode(0) == mt_dl_mode::kNeedResponse);
  CHECK(patch.download_mode(1) == (mt_dl_mode::kNeedResponse | mt_dl_mode::kEncrypt | (2u << 1) | mt_dl_mode::kResetSecIv));
  CHECK(patch.download_mode(2) == (mt_dl_mode::kNeedResponse | mt_dl_mode::kEncrypt | mt_dl_mode::kEncryptModeSel | mt_dl_mode::kResetSecIv));
  {
    const Bytes ns = make_patch({{0x900000, 0xFFFFFFFF, 8}});
    MtConnacPatch p;
    CHECK(p.parse(view(ns), nullptr) && p.download_mode(0) == mt_dl_mode::kNeedResponse);
  }
  // truncation: every shorter file is refused (the data is last)
  for (size_t n = 0; n < file.size(); n += (n < 400 ? 1 : 97)) {
    Bytes cut(file.begin(), file.begin() + n);
    MtConnacPatch p;
    CHECK(!p.parse(view(cut), &error) && error != nullptr);
  }
  // bit flips in the header and the table: never out of bounds, and what parses stays inside the file
  uint32_t accepted = 0;
  for (size_t bit = 0; bit < (96 + 3 * 64) * 8; ++bit) {
    Bytes damaged = file;
    damaged[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    MtConnacPatch p;
    if (p.parse(view(damaged), nullptr)) { ++accepted; check_patch_consistent(p, damaged); }
  }
  CHECK(accepted > 0);      // flips in the date or the versions do not matter
  // specific damage
  auto with = [&](size_t at, uint32_t value) { Bytes d = file; for (int i = 0; i < 4; ++i) d[at + i] = static_cast<uint8_t>(value >> (8 * (3 - i))); return d; };
  MtConnacPatch p;
  CHECK(!p.parse(view(with(44, 0)), &error));                       // no regions
  CHECK(!p.parse(view(with(44, 17)), &error));                      // too many
  CHECK(!p.parse(view(with(44, 0xFFFFFFFF)), &error));
  CHECK(!p.parse(view(with(96, 0x00040003)), &error));              // not INFO
  CHECK(!p.parse(view(with(100, 0xFFFFFF00)), &error));             // data offset past the end
  CHECK(!p.parse(view(with(100, 0x10)), &error));                   // data in the header
  CHECK(!p.parse(view(with(112, 0xFFFFFFFF)), &error));             // length past the end
  CHECK(!p.parse(view(with(112, 0)), &error));
  CHECK(!p.parse(ConstByteView(), &error));
}

static void test_ram_parser() {
  const Bytes file = make_ram({{0x915000, 0x20, 3000}, {0x02015C00, 0x01, 4097}, {0x404400, 0x13, 64}, {0, 0x40, 200}});
  MtConnacRam ram;
  const char* error = nullptr;
  CHECK(ram.parse(view(file), &error));
  CHECK(ram.chip_id() == 0x0D && ram.eco_code() == 1 && ram.format_version() == 2 && ram.format_flag() == 1 && ram.crc() == 0x5775A48A);
  CHECK(strcmp(ram.version(), "____010000") == 0 && strcmp(ram.build_date(), "20260224110949") == 0);
  CHECK(ram.region_count() == 4 && ram.region(1).address == 0x02015C00 && ram.region(1).file_offset == 3000 && ram.region(3).file_offset == 3000 + 4097 + 64);
  CHECK(ram.region(1).data.equals(view(pattern(4097, 0x41))));
  CHECK(!ram.region(3).downloaded() && ram.region(0).downloaded());
  CHECK(ram.download_mode(0) == mt_dl_mode::kNeedResponse);
  CHECK(ram.download_mode(1) == (mt_dl_mode::kNeedResponse | mt_dl_mode::kEncrypt | mt_dl_mode::kResetSecIv));
  // 0x13: encrypt, key index 1, encrypt mode select
  CHECK(ram.download_mode(2) == (mt_dl_mode::kNeedResponse | mt_dl_mode::kEncrypt | mt_dl_mode::kResetSecIv | (1u << 1) | mt_dl_mode::kEncryptModeSel));
  CHECK(ram.start_address() == 0x915000 && ram.start_option() == MtConnacRam::kStartOverride);
  check_ram_consistent(ram, file);
  // truncation from the end moves the trailer: never out of bounds, and anything accepted is consistent
  for (size_t n = 0; n < file.size(); n += (n < 300 || file.size() - n < 300 ? 1 : 101)) {
    Bytes cut(file.begin(), file.begin() + n);
    MtConnacRam r;
    if (r.parse(view(cut), nullptr)) check_ram_consistent(r, cut);
  }
  // truncation from the start (the data shrinks under the table)
  for (size_t n = 1; n < 200; ++n) {
    Bytes cut(file.begin() + n, file.end());
    MtConnacRam r;
    // the 24 bytes of padding before the table can go; then the data no longer fits
    if (n <= 24) CHECK(r.parse(view(cut), nullptr) && r.region(0).data.data() == cut.data());
    else CHECK(!r.parse(view(cut), &error));
  }
  // bit flips in the table and the trailer
  const size_t tail = 4 * 40 + 36;
  for (size_t bit = 0; bit < tail * 8; ++bit) {
    Bytes damaged = file;
    damaged[file.size() - tail + bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    MtConnacRam r;
    if (r.parse(view(damaged), nullptr)) check_ram_consistent(r, damaged);
  }
  Bytes d = file;
  d[file.size() - 36 + 2] = 0;
  CHECK(!ram.parse(view(d), &error));
  d[file.size() - 36 + 2] = 200;
  CHECK(!ram.parse(view(d), &error));
  d = file;
  // region 0's length: huge
  for (int i = 0; i < 4; ++i) d[file.size() - 36 - 4 * 40 + 20 + i] = 0xFF;
  CHECK(!ram.parse(view(d), &error));
}

static void test_usb_firmware_parser() {
  const Bytes file = make_usb_firmware(5000, 1200, 0x0100, 0x7640);
  Mt76UsbFirmware fw;
  const char* error = nullptr;
  CHECK(fw.parse(view(file), &error));
  CHECK(fw.ilm_length() == 5000 && fw.dlm_length() == 1200 && fw.firmware_version() == 0x0100 && fw.build_version() == 0x7640);
  CHECK(strcmp(fw.build_time(), "201406241830____") == 0);
  CHECK(fw.ilm().data() == file.data() + 32 && fw.dlm().data() == file.data() + 32 + 5000);
  char text[32];
  CHECK(fw.version_text(ArrayView<char>(text)) && strcmp(text, "0.1.0 (build 7640)") == 0);
  char small[6];
  CHECK(!fw.version_text(ArrayView<char>(small)) && strlen(small) < sizeof small);
  for (size_t n = 0; n < file.size(); n += (n < 100 ? 1 : 53)) {
    Bytes cut(file.begin(), file.begin() + n);
    CHECK(!fw.parse(view(cut), &error) && error);
  }
  Bytes longer = file;
  longer.push_back(0);
  CHECK(!fw.parse(view(longer), &error));
  for (size_t bit = 0; bit < 32 * 8; ++bit) {
    Bytes damaged = file;
    damaged[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    Mt76UsbFirmware f;
    if (f.parse(view(damaged), nullptr)) CHECK(f.ilm_length() + f.dlm_length() + 32 == damaged.size() && inside(f.ilm(), damaged) && inside(f.dlm(), damaged));
  }
  // ILM and DLM lengths that add up past 32 bits
  Bytes wrap = file;
  for (int i = 0; i < 4; ++i) { wrap[i] = 0xFF; wrap[4 + i] = 0xFF; }
  CHECK(!fw.parse(view(wrap), &error));
}

// ---- MCU messages ---------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_mcu_messages() {
  uint8_t payload[12];
  CHECK(mt_mcu_init_download_payload(0x900000, 0x16780, 0x80000000u, ByteView(payload)) == 12);
  uint8_t out[128];
  const uint32_t n = mt_mcu_build_command(mt_mcu::kPatchStartReq, 0, false, 5, ConstByteView(payload), ByteView(out));
  CHECK(n == 76);
  CHECK(get32le(out) == (76u | 2u << 23 | 0x20u << 25));
  CHECK(get32le(out + 4) == 0x80010000u);
  for (int i = 8; i < 32; ++i) CHECK(out[i] == 0);
  CHECK(get16le(out + 32) == 76 - 32 && get16le(out + 34) == 0x8000);
  CHECK(out[36] == 0x05 && out[37] == 0xA0 && out[38] == mt_mcu::kQueryNa && out[39] == 5 && out[40] == 0 && out[41] == 0 && out[42] == 0 && out[43] == 0);
  for (int i = 44; i < 64; ++i) CHECK(out[i] == 0);
  CHECK(get32le(out + 64) == 0x900000 && get32le(out + 68) == 0x16780 && get32le(out + 72) == 0x80000000u);
  // an extended command says set/query and wants its ack
  CHECK(mt_mcu_build_command(mt_mcu::kFwStartReq, 0x3C, true, 9, ConstByteView(), ByteView(out)) == 64 && out[38] == mt_mcu::kQueryQuery && out[41] == 0x3C && out[43] == 1);
  CHECK(mt_mcu_build_command(mt_mcu::kFwStartReq, 0x3C, false, 9, ConstByteView(), ByteView(out)) == 64 && out[38] == mt_mcu::kQuerySet);
  // unified
  CHECK(mt_mcu_build_uni_command(0x0102, 3, ConstByteView(payload).first(4), ByteView(out)) == 52);
  CHECK(get32le(out) == (52u | 2u << 23 | 0x20u << 25) && get16le(out + 32) == 20 && get16le(out + 34) == 0x0102 && out[37] == 0xA0 && out[39] == 3 && out[42] == 0 && out[43] == 7);
  // too small
  CHECK(mt_mcu_build_command(1, 0, false, 1, ConstByteView(payload), ByteView(out).first(75)) == 0);
  CHECK(mt_mcu_build_uni_command(1, 1, ConstByteView(), ByteView(out).first(47)) == 0);
  // payload helpers
  CHECK(mt_mcu_patch_sem_payload(true, ByteView(payload)) == 4 && get32le(payload) == 1 && mt_mcu_patch_sem_payload(false, ByteView(payload)) == 4 && get32le(payload) == 0);
  CHECK(mt_mcu_fw_start_payload(1, 0x915000, ByteView(payload)) == 8 && get32le(payload) == 1 && get32le(payload + 4) == 0x915000);
  CHECK(mt_mcu_patch_finish_payload(ByteView(payload).first(3)) == 0);
  CHECK(mt_mcu_init_download_command(0x900000, false) == mt_mcu::kPatchStartReq && mt_mcu_init_download_command(0x200000, false) == mt_mcu::kPatchStartReq);
  CHECK(mt_mcu_init_download_command(0xE0002800, false) == mt_mcu::kTargetAddressLenReq && mt_mcu_init_download_command(0xE0002800, true) == mt_mcu::kPatchStartReq);
  CHECK(mt_mcu_init_download_command(0x915000, false) == mt_mcu::kTargetAddressLenReq);
  // sequence numbers 1..15
  MtMcuSequence seq;
  for (int round = 0; round < 3; ++round)
    for (uint8_t expect = 1; expect <= 15; ++expect) CHECK(seq.next() == expect);
  // events
  Bytes ev(40, 0);
  ev[0] = 40; ev[3] = 7u << 3;                 // length 40, packet type 7 (bits 27-31)
  ev[29] = 6; ev[32] = 2;
  MtMcuEvent e;
  uint8_t status = 0;
  CHECK(mt_mcu_parse_event(view(ev), &e) && e.seq == 6 && e.rx_bytes == 40 && e.data.size() == 4 && mt_mcu_event_status(e, &status) && status == 2);
  Bytes bad = ev; bad[3] = 6u << 3;
  CHECK(!mt_mcu_parse_event(view(bad), &e));
  bad = ev; bad[0] = 41;
  CHECK(!mt_mcu_parse_event(view(bad), &e));
  bad = ev; bad[0] = 20;
  CHECK(!mt_mcu_parse_event(view(bad), &e));
  for (size_t n2 = 0; n2 < 36; ++n2) CHECK(!mt_mcu_parse_event(ConstByteView(ev.data(), n2), &e));
  for (size_t bit = 0; bit < ev.size() * 8; ++bit) {
    Bytes d = ev;
    d[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    if (mt_mcu_parse_event(view(d), &e)) CHECK(e.message.size() <= d.size() && e.message.size() >= 36);
  }
}

// ---- register model -------------------------------------------------------------------------------------------------------------------------------------------------------

// BAR0 of an MT7921 as far as the download needs it: the remap windows, the own handshake, the chip id, the WFSYS reset, WFDMA0 and its rings, and the MCU's side of
// the firmware download protocol.
struct Mt7921Model final : INicRegisters {
  std::map<uint32_t, uint32_t> bar;                 // plain registers by BAR offset
  std::map<uint32_t, uint32_t> chip;                // registers reached through a window, by chip address
  uint32_t chip_id = 0x7961, revision = 0x8A;
  bool dead = false;                                // reads all ones
  bool fw_owned = false;
  uint32_t reset = 0;
  bool reset_ever_cleared = false;
  bool n9_ready_after_start = true;
  bool mcu_silent = false;                          // never answers
  uint8_t sem_get_status = 2, sem_release_status = 3, finish_status = 0;
  struct Ring { uint32_t base = 0, size = 0, cpu = 0, dma = 0; };
  Ring tx[18], rx[2];
  struct Command { uint8_t cid, seq, set_query; Bytes payload; };
  std::vector<Command> commands;
  struct Download { uint32_t address, length, mode; Bytes data; };
  std::vector<Download> downloads;
  uint32_t start_option = 0xFFFFFFFF, start_address = 0xFFFFFFFF;
  uint32_t scatter_messages = 0;
  bool dma_enabled = false;
  uint32_t glo = 0;

  static bool ring_register(uint32_t offset, uint32_t base, uint32_t count, uint32_t* ring, uint32_t* field) {
    if (offset < base || offset >= base + 0x10 * count) return false;
    *ring = (offset - base) / 0x10;
    *field = (offset - base) % 0x10;
    return true;
  }

  uint32_t chip_read(uint32_t address) {
    if (address == mt7921::kHwChipId) return chip_id;
    if (address == mt7921::kHwRev) return revision;
    if (address == mt7921::kWfsysSwReset) return reset | ((reset & mt7921::kWfsysSwRstB) && reset_ever_cleared ? mt7921::kWfsysSwInitDone : 0);
    auto it = chip.find(address);
    return it == chip.end() ? 0 : it->second;
  }
  void chip_write(uint32_t address, uint32_t value) {
    if (address == mt7921::kWfsysSwReset) {
      if (!(value & mt7921::kWfsysSwRstB)) reset_ever_cleared = true;
      reset = value & ~mt7921::kWfsysSwInitDone;
      return;
    }
    chip[address] = value;
  }

  uint8_t read8(uint32_t) override { return 0xFF; }
  uint16_t read16(uint32_t) override { return 0xFFFF; }
  void write8(uint32_t, uint8_t) override {}
  void write16(uint32_t, uint16_t) override {}

  uint32_t read32(uint32_t offset) override {
    if (dead) return 0xFFFFFFFFu;
    uint32_t ring, field;
    if (offset == 0xE0010) return fw_owned ? mt7921::kLpcrHostOwnSync : 0;                             // MT_CONN_ON_LPCTL
    if (offset == 0xE00F0) return bar[offset];                                                         // MT_CONN_ON_MISC
    if (offset == mt7921::kWfdma0GloCfg) return glo;
    if (offset >= mt7921::kHifRemapBaseL1 && offset < mt7921::kHifRemapBaseL1 + 0x10000) return chip_read((bar[mt7921::kHifRemapL1] & 0xFFFF0000u) | (offset - mt7921::kHifRemapBaseL1));
    if (offset >= mt7921::kHifRemapBaseL2 && offset < mt7921::kHifRemapBaseL2 + 0x1000) return chip_read(((bar[mt7921::kHifRemapL2] & 0xFFFFF) << 12) | (offset - mt7921::kHifRemapBaseL2));
    if (ring_register(offset, mt7921::kTxRingBase, 18, &ring, &field)) {
      const Ring& r = tx[ring];
      return field == 0 ? r.base : field == 4 ? r.size : field == 8 ? r.cpu : r.dma;
    }
    if (ring_register(offset, mt7921::kRxEventRingBase, 2, &ring, &field)) {
      const Ring& r = rx[ring];
      return field == 0 ? r.base : field == 4 ? r.size : field == 8 ? r.cpu : r.dma;
    }
    return bar[offset];
  }

  void write32(uint32_t offset, uint32_t value) override {
    if (dead) return;
    uint32_t ring, field;
    if (offset == 0xE0010) {
      if (value & mt7921::kLpcrHostSetOwn) fw_owned = true;
      if (value & mt7921::kLpcrHostClrOwn) fw_owned = false;
      return;
    }
    if (offset == mt7921::kWfdma0GloCfg) {
      glo = value;
      dma_enabled = (value & (mt7921::kGloTxDmaEn | mt7921::kGloRxDmaEn)) == (mt7921::kGloTxDmaEn | mt7921::kGloRxDmaEn);
      return;
    }
    if (offset == mt7921::kWfdma0RstDtxPtr) {
      for (Ring& r : tx) if (value) r.dma = r.cpu = 0;
      return;
    }
    if (offset >= mt7921::kHifRemapBaseL1 && offset < mt7921::kHifRemapBaseL1 + 0x10000) { chip_write((bar[mt7921::kHifRemapL1] & 0xFFFF0000u) | (offset - mt7921::kHifRemapBaseL1), value); return; }
    if (offset >= mt7921::kHifRemapBaseL2 && offset < mt7921::kHifRemapBaseL2 + 0x1000) { chip_write(((bar[mt7921::kHifRemapL2] & 0xFFFFF) << 12) | (offset - mt7921::kHifRemapBaseL2), value); return; }
    if (ring_register(offset, mt7921::kTxRingBase, 18, &ring, &field)) {
      Ring& r = tx[ring];
      if (field == 0) r.base = value; else if (field == 4) r.size = value; else if (field == 0xC) r.dma = value;
      else { r.cpu = value; process_tx(ring); }
      return;
    }
    if (ring_register(offset, mt7921::kRxEventRingBase, 2, &ring, &field)) {
      Ring& r = rx[ring];
      if (field == 0) r.base = value; else if (field == 4) r.size = value; else if (field == 8) r.cpu = value; else r.dma = value;
      return;
    }
    bar[offset] = value;
  }

  // descriptors in host memory: the bus address is the pointer (the test allocator maps DMA memory below 2 GiB)
  static uint32_t* descriptor(const Ring& r, uint32_t index) { return reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(r.base)) + 4 * index; }

  void process_tx(uint32_t ring) {
    Ring& r = tx[ring];
    if (!dma_enabled || r.size == 0) return;
    while (r.dma != r.cpu) {
      uint32_t* d = descriptor(r, r.dma);
      const uint32_t length = (d[1] >> 16) & 0x3FFF;
      const uint8_t* data = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(d[0]));
      Bytes message(data, data + length);
      d[1] |= mt7921::kDmaCtlDmaDone;
      r.dma = (r.dma + 1) % r.size;
      if (ring == mt7921::kTxRingFwdl) {
        ++scatter_messages;
        if (!downloads.empty()) downloads.back().data.insert(downloads.back().data.end(), message.begin(), message.end());
      } else if (ring == mt7921::kTxRingMcuWm) {
        mcu_command(message);
      }
    }
  }

  void respond(uint8_t seq, uint8_t status) {
    if (mcu_silent) return;
    Ring& r = rx[0];
    if (r.size == 0 || r.dma == r.cpu) return;
    uint32_t* d = descriptor(r, r.dma);
    uint8_t* buffer = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(d[0]));
    Bytes ev(40, 0);
    ev[0] = 40; ev[3] = 7u << 3;
    ev[24] = 40 - 24;
    ev[28] = 1; ev[29] = seq; ev[32] = status;
    memcpy(buffer, ev.data(), ev.size());
    d[1] = mt7921::kDmaCtlDmaDone | mt7921::kDmaCtlLastSec0 | (static_cast<uint32_t>(ev.size()) << 16);
    r.dma = (r.dma + 1) % r.size;
  }

  void mcu_command(const Bytes& m) {
    if (m.size() < 64) return;
    // the TXD must say what the driver says it says
    if (get32le(m.data()) != (static_cast<uint32_t>(m.size()) | 2u << 23 | 0x20u << 25) || get16le(m.data() + 32) != m.size() - 32 || m[37] != 0xA0) {
      ++malformed;
      return;
    }
    Command c{m[36], m[39], m[38], Bytes(m.begin() + 64, m.end())};
    commands.push_back(c);
    const uint8_t* p = c.payload.data();
    switch (c.cid) {
      case mt_mcu::kPatchSemControl:
        respond(c.seq, c.payload.size() >= 4 && get32le(p) == 1 ? sem_get_status : sem_release_status);
        break;
      case mt_mcu::kPatchStartReq:
      case mt_mcu::kTargetAddressLenReq:
        if (c.payload.size() >= 12) downloads.push_back(Download{get32le(p), get32le(p + 4), get32le(p + 8), Bytes()});
        respond(c.seq, 0);
        break;
      case mt_mcu::kPatchFinishReq:
        respond(c.seq, finish_status);
        break;
      case mt_mcu::kFwStartReq:
        if (c.payload.size() >= 8) { start_option = get32le(p); start_address = get32le(p + 4); }
        if (n9_ready_after_start) bar[0xE00F0] = mt7921::kTopMisc2FwN9Ready;
        respond(c.seq, 0);
        break;
      default:
        respond(c.seq, 0);
        break;
    }
  }
  uint32_t malformed = 0;
};

static void test_remap() {
  uint32_t offset = 0;
  CHECK(mt7921_fixed_map(0x7C060010, &offset) && offset == 0xE0010);           // MT_CONN_ON_LPCTL
  CHECK(mt7921_fixed_map(0x7C0600F0, &offset) && offset == 0xE00F0);           // MT_CONN_ON_MISC
  CHECK(mt7921_fixed_map(0x54000120, &offset) && offset == 0x02120);           // MT_WFDMA_DUMMY_CR
  CHECK(mt7921_fixed_map(0x7C026004, &offset) && offset == 0xD6004);           // MT_DMASHDL_SW_CONTROL
  CHECK(mt7921_fixed_map(0x820D0000, &offset) && offset == 0x30000);
  CHECK(mt7921_fixed_map(0x820FD7FF, &offset) && offset == 0xA4FFF);
  CHECK(mt7921_fixed_map(0x000D4208, &offset) && offset == 0xD4208);           // below 1 MiB: as it is
  CHECK(mt7921_fixed_map(0x00400010, &offset) && offset == 0x80010);
  CHECK(!mt7921_fixed_map(0x70010200, &offset) && mt7921_window(0x70010200) == Mt7921Window::L1);
  CHECK(!mt7921_fixed_map(0x18000140, &offset) && mt7921_window(0x18000140) == Mt7921Window::L1);
  CHECK(mt7921_window(0x7C3FFFFC) == Mt7921Window::L1 && mt7921_window(0x7C400000) == Mt7921Window::L2 && mt7921_window(0x18C00000) == Mt7921Window::L2);
  CHECK(mt7921_window(0x12345678) == Mt7921Window::L2 && mt7921_window(0x7C060010) == Mt7921Window::Fixed);
  CHECK(mt7921_window(0x820ED800) != Mt7921Window::Fixed || (mt7921_fixed_map(0x820ED800, &offset) && offset != 0x25000));   // one past WF_MIB is not WF_MIB
  Mt7921Model model;
  model.bar[mt7921::kHifRemapL1] = 0x0000ABCD;
  Mt7921Registers regs(model);
  CHECK(regs.read(mt7921::kHwChipId) == 0x7961 && regs.read(mt7921::kHwRev) == 0x8A);
  CHECK(model.bar[mt7921::kHifRemapL1] == 0x7001ABCD);                         // the page in the top half, the rest kept
  CHECK(regs.map(0x18000140) == 0x130140 && model.bar[mt7921::kHifRemapL1] == 0x1800ABCD);
  model.bar[mt7921::kHifRemapL2] = 0xFFF00000u;
  CHECK(regs.map(0x12345678) == mt7921::kHifRemapBaseL2 + 0x678 && model.bar[mt7921::kHifRemapL2] == 0xFFF12345u);
  regs.write(0x12345678, 0xCAFE);
  CHECK(model.chip[0x12345678] == 0xCAFE && regs.read(0x12345678) == 0xCAFE);
  CHECK(regs.map(0x7C060010) == 0xE0010);
}

// ---- Mt7921Chip ----------------------------------------------------------------------------------------------------------------------------------------------------------

struct MapFirmware final : IWifiFirmwareSource {
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

static const WifiAdapterModel& model_of(WifiBus bus, uint16_t vendor, uint16_t device) {
  const WifiAdapterModel* m = wifi_find_adapter(bus, vendor, device);
  if (!m) { printf("no model %04x:%04x\n", vendor, device); abort(); }
  return *m;
}

static void check_download(const Mt7921Model& model, const MtConnacPatch& patch, const MtConnacRam& ram, bool with_patch) {
  size_t d = 0;
  if (with_patch) {
    for (uint32_t i = 0; i < patch.section_count(); ++i, ++d) {
      CHECK(d < model.downloads.size());
      if (d >= model.downloads.size()) return;
      CHECK(model.downloads[d].address == patch.section(i).address && model.downloads[d].length == patch.section(i).length && model.downloads[d].mode == patch.download_mode(i));
      CHECK(model.downloads[d].data.size() == patch.section(i).data.size() && memcmp(model.downloads[d].data.data(), patch.section(i).data.data(), model.downloads[d].data.size()) == 0);
    }
  }
  for (uint32_t i = 0; i < ram.region_count(); ++i) {
    if (!ram.region(i).downloaded()) continue;
    CHECK(d < model.downloads.size());
    if (d >= model.downloads.size()) return;
    const auto& got = model.downloads[d++];
    CHECK(got.address == ram.region(i).address && got.length == ram.region(i).length && got.mode == ram.download_mode(i));
    CHECK(got.data.size() == ram.region(i).data.size() && memcmp(got.data.data(), ram.region(i).data.data(), got.data.size()) == 0);
  }
  CHECK(d == model.downloads.size());
  CHECK(model.start_option == ram.start_option() && model.start_address == ram.start_address());
}

static void test_mt7921_chip() {
  TestAllocator alloc;
  const WifiAdapterModel& rz608 = model_of(WifiBus::Pci, 0x14C3, 0x0608);
  MapFirmware firmware;
  firmware.files["mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin"] = make_patch({{0x900000, 0, 10000}});
  firmware.files["mediatek/WIFI_RAM_CODE_MT7961_1.bin"] = make_ram({{0x915000, 0x20, 9000}, {0x02015C00, 0x00, 100}, {0, 0x40, 300}});

  {
    // the whole download
    Mt7921Model model;
    WifiChipBus bus;
    bus.registers = &model;
    UniquePointer<IWifiChip> chip = make_wifi_chip(rz608, bus, alloc);
    CHECK(chip != nullptr);
    if (!chip) return;
    CHECK(chip->state() == WifiChipState::Off && strcmp(chip->firmware_name(), "mediatek/WIFI_RAM_CODE_MT7961_1.bin") == 0);
    const WifiChipResult result = chip->start(firmware);
    if (result != WifiChipResult::Unsupported) printf("  start: %s (%s)\n", wifi_chip_result_name(result), chip->message());
    CHECK(result == WifiChipResult::Unsupported && chip->state() == WifiChipState::FirmwareLoaded);
    CHECK(chip->identity().chip_id == 0x7961 && chip->identity().revision == 0x8A);
    CHECK(strcmp(chip->identity().firmware_version, "____010000-20260224110949") == 0);
    CHECK(model.reset_ever_cleared && (model.reset & mt7921::kWfsysSwRstB));
    CHECK(!model.fw_owned && model.malformed == 0 && model.dma_enabled);
    CHECK(model.tx[16].size == mt7921::kTxFwdlRingSize && model.tx[17].size == mt7921::kTxMcuRingSize && model.rx[0].size == mt7921::kRxMcuRingSize);
    CHECK(model.bar[mt7921::kWfdma0GloCfg] == 0 && (model.glo & mt7921::kGloFifoLittleEndian) && (model.glo & mt7921::kGloTxWbDdone));
    CHECK(model.bar[mt7921::tx_ring_ext_ctrl(16)] == 0x03400004 && model.bar[mt7921::rx_ring_ext_ctrl(0)] == 0x4);
    // the commands, in order
    std::vector<uint8_t> cids;
    for (const auto& c : model.commands) cids.push_back(c.cid);
    const std::vector<uint8_t> expected = {mt_mcu::kPatchSemControl, mt_mcu::kPatchStartReq, mt_mcu::kPatchFinishReq, mt_mcu::kPatchSemControl,
                                           mt_mcu::kTargetAddressLenReq, mt_mcu::kTargetAddressLenReq, mt_mcu::kFwStartReq};
    CHECK(cids == expected);
    for (const auto& c : model.commands) CHECK(c.set_query == mt_mcu::kQueryNa && c.seq >= 1 && c.seq <= 15);
    // 10000 bytes in 4096-byte chunks (3), then 9000 (3) and 100 (1)
    CHECK(model.scatter_messages == 7);
    const Mt7921Chip& mt = static_cast<const Mt7921Chip&>(*chip);
    check_download(model, mt.patch(), mt.ram(), true);
    CHECK(model.start_option == 1 && model.start_address == 0x915000);
    chip->stop();
    CHECK(chip->state() == WifiChipState::Off && model.fw_owned && !model.dma_enabled);
    // and again after stop()
    model.commands.clear();
    model.downloads.clear();
    CHECK(chip->start(firmware) == WifiChipResult::Unsupported && chip->state() == WifiChipState::FirmwareLoaded && model.commands.size() == 7);
  }
  {
    // the patch is there already: no patch sections, the semaphore is still released
    Mt7921Model model;
    model.sem_get_status = mt_mcu::kPatchIsDl;
    WifiChipBus bus;
    bus.registers = &model;
    Mt7921Chip chip(rz608, bus, alloc);
    CHECK(chip.start(firmware) == WifiChipResult::Unsupported && chip.patch_was_present());
    CHECK(model.commands.size() == 5 && model.commands[1].cid == mt_mcu::kPatchSemControl && model.downloads.size() == 2);
    check_download(model, chip.patch(), chip.ram(), false);
  }
  {
    // MT7922 (RZ616) names its own files
    Mt7921Model model;
    model.chip_id = 0x7922;
    WifiChipBus bus;
    bus.registers = &model;
    Mt7921Chip chip(model_of(WifiBus::Pci, 0x14C3, 0x0616), bus, alloc);
    CHECK(chip.start(firmware) == WifiChipResult::FirmwareMissing && chip.state() == WifiChipState::Failed);
    CHECK(strcmp(chip.patch_name(), "mediatek/WIFI_MT7922_patch_mcu_1_1_hdr.bin") == 0 && strcmp(chip.firmware_name(), "mediatek/WIFI_RAM_CODE_MT7922_1.bin") == 0);
    CHECK(chip.identity().chip_id == 0x7922);
  }
  auto run = [&](Mt7921Model& model, MapFirmware& fw) {
    WifiChipBus bus;
    bus.registers = &model;
    Mt7921Chip chip(rz608, bus, alloc);
    const WifiChipResult r = chip.start(fw);
    CHECK(chip.state() == WifiChipState::Failed);
    CHECK(chip.message() && *chip.message());
    return r;
  };
  {
    Mt7921Model model;
    model.dead = true;
    CHECK(run(model, firmware) == WifiChipResult::NoResponse);
  }
  {
    Mt7921Model model;
    model.chip_id = 0x7663;
    CHECK(run(model, firmware) == WifiChipResult::UnknownRevision);
  }
  {
    Mt7921Model model;
    model.chip_id = 0x7925;              // MT7925 has its own register map: not this driver
    CHECK(run(model, firmware) == WifiChipResult::UnknownRevision);
  }
  {
    Mt7921Model model;
    model.sem_get_status = mt_mcu::kPatchNotDlSemFail;
    CHECK(run(model, firmware) == WifiChipResult::FirmwareRejected);
  }
  {
    Mt7921Model model;
    model.sem_release_status = 0;
    CHECK(run(model, firmware) == WifiChipResult::FirmwareRejected);
  }
  {
    Mt7921Model model;
    model.finish_status = 1;
    CHECK(run(model, firmware) == WifiChipResult::FirmwareRejected);
  }
  {
    Mt7921Model model;
    model.mcu_silent = true;
    CHECK(run(model, firmware) == WifiChipResult::Timeout);
  }
  {
    Mt7921Model model;
    model.n9_ready_after_start = false;
    CHECK(run(model, firmware) == WifiChipResult::FirmwareRejected);
  }
  {
    MapFirmware broken = firmware;
    Bytes& ram = broken.files["mediatek/WIFI_RAM_CODE_MT7961_1.bin"];
    ram[ram.size() - 36 + 2] = 99;
    Mt7921Model model;
    CHECK(run(model, broken) == WifiChipResult::FirmwareInvalid);
    MapFirmware missing;
    Mt7921Model model2;
    CHECK(run(model2, missing) == WifiChipResult::FirmwareMissing);
  }
  {
    WifiChipBus bus;
    Mt7921Chip chip(rz608, bus, alloc);
    CHECK(chip.start(firmware) == WifiChipResult::NoRegisters);
    chip.stop();
  }
  // the factory
  {
    WifiChipBus bus;
    CHECK(make_wifi_chip(model_of(WifiBus::Pci, 0x14C3, 0x7925), bus, alloc) == nullptr);
    CHECK(make_wifi_chip(model_of(WifiBus::Pci, 0x14C3, 0x7615), bus, alloc) == nullptr);
    CHECK(make_wifi_chip(model_of(WifiBus::Usb, 0x0E8D, 0x7961), bus, alloc) != nullptr);
    CHECK(make_wifi_chip(model_of(WifiBus::Usb, 0x148F, 0x7601), bus, alloc) != nullptr);
  }
  CHECK(alloc.dma.empty());
}

// ---- USB ------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct FakeMtUsb final : IUsbHostController {
  std::map<uint32_t, uint32_t> registers;      // by the address the request carries (wValue << 16 | wIndex)
  struct Request { uint8_t type, request; uint16_t value, index; };
  std::vector<Request> requests;
  bool fail = false;
  uint8_t expect_read_type = 0xDF, expect_read = 0x63, expect_write_type = 0x5F, expect_write = 0x66;

  const char* name() const override { return "fake mt76 usb"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return 1; }
  bool port_info(uint32_t, UsbPortInfo*) const override { return false; }
  bool device_descriptor(uint32_t, UsbDeviceDescriptor*) const override { return false; }
  bool configuration_descriptor(uint32_t, ByteView, uint16_t*) const override { return false; }
  bool set_configuration(uint32_t, uint8_t) override { return true; }
  bool control_out(uint32_t, uint8_t, uint8_t, uint16_t, uint16_t) override { return false; }
  bool control_in(uint32_t port, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ByteView buffer) override {
    requests.push_back(Request{type, request, value, index});
    if (fail || port != 2 || buffer.size() != 4) return false;
    uint32_t v = 0xFFFFFFFF;
    if (type == expect_read_type && request == expect_read) v = registers[static_cast<uint32_t>(value) << 16 | index];
    if (type == 0xC0 && request == mt_usb::kVendReadEeprom) v = 0xEEEE0000u | index;
    for (int i = 0; i < 4; ++i) buffer[i] = static_cast<uint8_t>(v >> (8 * i));
    return true;
  }
  bool control_out_data(uint32_t port, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ConstByteView data) override {
    requests.push_back(Request{type, request, value, index});
    if (fail || port != 2 || data.size() != 4) return false;
    if (type == expect_write_type && request == expect_write) registers[static_cast<uint32_t>(value) << 16 | index] = get32le(data.data());
    return true;
  }
  bool start_interrupt_in(uint32_t, uint8_t, uint16_t, uint8_t) override { return false; }
  bool poll_interrupt_in(uint32_t, uint8_t, ByteView, uint16_t*) override { return false; }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { return false; }
  bool bulk_transfer(uint32_t, uint8_t, ByteView, uint32_t*) override { return false; }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return false; }
};

static void test_usb() {
  TestAllocator alloc;
  {
    // connac register access: READ_EXT / WRITE_EXT, the address split over wValue/wIndex
    FakeMtUsb usb;
    MtUsbRegisters regs(usb, 2, MtUsbProtocol::Connac);
    usb.registers[0x70010200] = 0x00007961;
    CHECK(regs.read32(0x70010200) == 0x7961);
    CHECK(usb.requests.back().type == 0xDF && usb.requests.back().request == 0x63 && usb.requests.back().value == 0x7001 && usb.requests.back().index == 0x0200);
    regs.write32(0x7C060010, 0x11223344);
    CHECK(usb.registers[0x7C060010] == 0x11223344 && usb.requests.back().type == 0x5F && usb.requests.back().request == 0x66);
    CHECK(regs.read8(0x7C060011) == 0x33 && regs.read16(0x7C060012) == 0x1122);
    regs.write8(0x7C060010, 0xAA);
    CHECK(usb.registers[0x7C060010] == 0x112233AA);
    regs.write16(0x7C060012, 0xBEEF);
    CHECK(usb.registers[0x7C060010] == 0xBEEF33AA && !regs.failed());
    usb.fail = true;
    CHECK(regs.read32(0x70010200) == 0xFFFFFFFFu && regs.failed());
  }
  {
    // mt76u: MULTI_READ / MULTI_WRITE, EEPROM and CFG types
    FakeMtUsb usb;
    usb.expect_read_type = 0xC0; usb.expect_read = 0x07; usb.expect_write_type = 0x40; usb.expect_write = 0x06;
    MtUsbRegisters regs(usb, 2, MtUsbProtocol::Mt76u);
    usb.registers[0x1000] = 0x76120044;
    CHECK(regs.read32(0x1000) == 0x76120044 && usb.requests.back().value == 0 && usb.requests.back().index == 0x1000);
    CHECK(regs.read32(mt_usb::kVendTypeEeprom | 0x20) == 0xEEEE0020u && usb.requests.back().request == mt_usb::kVendReadEeprom && usb.requests.back().index == 0x20);
    (void)regs.read32(mt_usb::kVendTypeCfg | 0x80);
    CHECK(usb.requests.back().request == mt_usb::kVendReadCfg && usb.requests.back().index == 0x80);
    regs.write32(mt_usb::kVendTypeCfg | 0x80, 1);
    CHECK(usb.requests.back().request == mt_usb::kVendWriteCfg && usb.requests.back().type == 0x40);
    regs.write32(0x2000, 5);
    CHECK(usb.requests.back().request == mt_usb::kVendMultiWrite && usb.registers[0x2000] == 5);
  }
  {
    // MT7921AU: identified, firmware parsed, then Unsupported
    FakeMtUsb usb;
    usb.registers[0x70010200] = 0x7961;
    usb.registers[0x70010204] = 0x8A10;
    MapFirmware firmware;
    firmware.files["mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin"] = make_patch({{0x900000, 0, 100}});
    firmware.files["mediatek/WIFI_RAM_CODE_MT7961_1.bin"] = make_ram({{0x915000, 0x20, 100}});
    WifiChipBus bus;
    bus.usb = &usb;
    bus.usb_port = 2;
    UniquePointer<IWifiChip> chip = make_wifi_chip(model_of(WifiBus::Usb, 0x0846, 0x9060), bus, alloc);
    CHECK(chip != nullptr);
    if (chip) {
      CHECK(chip->start(firmware) == WifiChipResult::Unsupported && chip->state() == WifiChipState::Identified);
      CHECK(chip->identity().chip_id == 0x7961 && chip->identity().revision == 0x10);
      usb.registers[0x70010200] = 0x7925;
      CHECK(chip->start(firmware) == WifiChipResult::UnknownRevision && chip->state() == WifiChipState::Failed);
      usb.fail = true;
      CHECK(chip->start(firmware) == WifiChipResult::NoResponse);
    }
  }
  {
    // MT7612U, MT7610U, MT7601U
    struct Case { uint16_t vendor, product; uint32_t asic; const char* file; };
    const Case cases[] = {{0x0B05, 0x1833, 0x76620002, "mediatek/mt7662u.bin"}, {0x2357, 0x0105, 0x76100002, "mediatek/mt7610u.bin"}, {0x148F, 0x7601, 0x76010001, "mt7601u.bin"}};
    for (const Case& c : cases) {
      FakeMtUsb usb;
      usb.expect_read_type = 0xC0; usb.expect_read = 0x07; usb.expect_write_type = 0x40; usb.expect_write = 0x06;
      usb.registers[0] = c.asic;
      MapFirmware firmware;
      WifiChipBus bus;
      bus.usb = &usb;
      bus.usb_port = 2;
      MtUsbChip chip(model_of(WifiBus::Usb, c.vendor, c.product), bus, alloc);
      CHECK(chip.start(firmware) == WifiChipResult::FirmwareMissing && chip.identity().chip_id == (c.asic >> 16) && firmware.asked.back() == c.file);
      firmware.files[c.file] = make_usb_firmware(3000, 200, 0x0100, 0x7640);
      CHECK(chip.start(firmware) == WifiChipResult::Unsupported && chip.state() == WifiChipState::Identified);
      CHECK(strcmp(chip.identity().firmware_version, "0.1.0 (build 7640)") == 0);
      firmware.files[c.file].pop_back();
      CHECK(chip.start(firmware) == WifiChipResult::FirmwareInvalid);
      usb.registers[0] = 0x76300001;      // a chip of another family
      if (c.asic >> 16 != 0x7610) CHECK(chip.start(firmware) == WifiChipResult::UnknownRevision);
      usb.registers[0] = 0;
      CHECK(chip.start(firmware) == WifiChipResult::NoResponse);
    }
    WifiChipBus none;
    MtUsbChip chip(model_of(WifiBus::Usb, 0x148F, 0x7601), none, alloc);
    MapFirmware firmware;
    CHECK(chip.start(firmware) == WifiChipResult::NoRegisters);
  }
}

// ---- real files ---------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_real_files() {
  const char* dir = getenv("MEDIATEK_FIRMWARE_DIR");
  if (!dir) { printf("  (MEDIATEK_FIRMWARE_DIR not set: no real firmware files tested)\n"); return; }
  const std::string base = std::string(dir) + "/";
  uint32_t tested = 0;
  for (uint32_t id : {0x7961u, 0x7922u, 0x7920u, 0x7925u}) {
    const char* patch_name;
    const char* ram_name;
    if (!mt792x_firmware_names(id, &patch_name, &ram_name)) continue;
    const Bytes patch_file = read_file(base + patch_name);
    const Bytes ram_file = read_file(base + ram_name);
    if (patch_file.empty() || ram_file.empty()) { printf("  (%04x: %s or %s not there)\n", id, patch_name, ram_name); continue; }
    ++tested;
    MtConnacPatch patch;
    MtConnacRam ram;
    const char* error = nullptr;
    CHECK(patch.parse(view(patch_file), &error));
    if (error) printf("  %s: %s\n", patch_name, error);
    CHECK(ram.parse(view(ram_file), &error));
    if (error) printf("  %s: %s\n", ram_name, error);
    printf("  %s: %zu bytes, %s, platform %s, hw/sw 0x%08x, %u section(s):", patch_name, patch_file.size(), patch.build_date(), patch.platform(), patch.hw_sw_version(), patch.section_count());
    for (uint32_t i = 0; i < patch.section_count(); ++i) printf(" [0x%08x +0x%x mode 0x%08x]", patch.section(i).address, patch.section(i).length, patch.download_mode(i));
    printf("\n  %s: %zu bytes, chip 0x%02x eco %u, version %s, built %s, %u region(s):", ram_name, ram_file.size(), ram.chip_id(), ram.eco_code(), ram.version(), ram.build_date(), ram.region_count());
    for (uint32_t i = 0; i < ram.region_count(); ++i) printf(" [0x%08x +0x%x feat 0x%02x%s]", ram.region(i).address, ram.region(i).length, ram.region(i).feature_set, ram.region(i).downloaded() ? "" : " not sent");
    printf(", start 0x%08x\n", ram.start_address());
    CHECK(strcmp(patch.platform(), "ALPS") == 0 && patch.section_count() >= 1);
    check_patch_consistent(patch, patch_file);
    check_ram_consistent(ram, ram_file);
    for (uint32_t i = 0; i < patch.section_count(); ++i) CHECK(mt_mcu_init_download_command(patch.section(i).address, id == 0x7925) == mt_mcu::kPatchStartReq);
    // damage: the parsers never step outside
    for (size_t n = 0; n < 2000 && n < patch_file.size(); n += 13) {
      MtConnacPatch p;
      Bytes cut(patch_file.begin(), patch_file.begin() + n);
      CHECK(!p.parse(view(cut), nullptr));
    }
    for (size_t bit = 0; bit < 256 * 8; bit += 3) {
      Bytes d = patch_file;
      d[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      MtConnacPatch p;
      if (p.parse(view(d), nullptr)) check_patch_consistent(p, d);
      Bytes r = ram_file;
      r[r.size() - 1 - bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      MtConnacRam m;
      if (m.parse(view(r), nullptr)) check_ram_consistent(m, r);
    }
    // and the whole download of the real files against the model (MT7921 and MT7922: the chips this driver runs)
    if (id == 0x7961 || id == 0x7922) {
      TestAllocator alloc;
      Mt7921Model model;
      model.chip_id = id;
      MapFirmware firmware;
      firmware.files[patch_name] = patch_file;
      firmware.files[ram_name] = ram_file;
      WifiChipBus bus;
      bus.registers = &model;
      Mt7921Chip chip(model_of(WifiBus::Pci, 0x14C3, id == 0x7961 ? 0x7961 : 0x0616), bus, alloc);
      CHECK(chip.start(firmware) == WifiChipResult::Unsupported && chip.state() == WifiChipState::FirmwareLoaded);
      check_download(model, chip.patch(), chip.ram(), true);
      printf("  %04x through the model: %zu commands, %u scatter messages, firmware \"%s\"\n", id, model.commands.size(), model.scatter_messages, chip.identity().firmware_version);
    }
  }
  for (const char* name : {"mediatek/mt7662u.bin", "mediatek/mt7610u.bin", "mt7601u.bin"}) {
    const Bytes file = read_file(base + name);
    if (file.empty()) { printf("  (%s not there)\n", name); continue; }
    ++tested;
    Mt76UsbFirmware fw;
    const char* error = nullptr;
    CHECK(fw.parse(view(file), &error));
    char text[32] = {};
    CHECK(fw.version_text(ArrayView<char>(text)));
    printf("  %s: %zu bytes, ILM %u, DLM %u, version %s, built %s\n", name, file.size(), fw.ilm_length(), fw.dlm_length(), text, fw.build_time());
  }
  CHECK(tested > 0);
}

int main() {
  printf("MediaTekWifiTest\n");
  test_table();
  test_patch_parser();
  test_ram_parser();
  test_usb_firmware_parser();
  test_mcu_messages();
  test_remap();
  test_mt7921_chip();
  test_usb();
  test_real_files();
  if (g_failed) { printf("%d check(s) FAILED\n", g_failed); return 1; }
  printf("all passed\n");
  return 0;
}
