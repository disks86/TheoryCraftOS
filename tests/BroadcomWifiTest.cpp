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

// The Broadcom Wi-Fi chip layer (src/Hardware/WirelessInterface/Broadcom): the adapter table, the brcmfmac firmware names (by chip and revision, board-specific
// names), the NVRAM text stripping as Linux's brcmf_fw_nvram_strip does it (comments, blanks, multi-device files, the length token), the enumeration ROM parser on
// synthetic ROMs with truncation, bit flips and garbage, and the chip start (ChipID, EROM, ARM halt and release, download, shared structure) on PCIe and SDIO against
// a model of the backplane. With BRCMF_FIRMWARE_DIR naming a folder of decompressed linux-firmware brcm/ files, real NVRAM files are stripped and real images loaded
// into the SDIO model (the default run needs none).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Hardware/WirelessInterface/Broadcom/BrcmfBackplane.hpp"
#include "Hardware/WirelessInterface/Broadcom/BrcmfFirmware.hpp"
#include "Hardware/WirelessInterface/Broadcom/BrcmfPcie.hpp"
#include "Hardware/WirelessInterface/Broadcom/BrcmfSdio.hpp"
#include "Hardware/WirelessInterface/Broadcom/BroadcomWifi.hpp"
#include "Hardware/WirelessInterface/WifiAdapterCatalog.hpp"
#include "Hardware/WirelessInterface/WifiChipFactory.hpp"

namespace hardware {
void delay_us(uint64_t) {}
}  // namespace hardware

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { free(p); }
};
static TestAllocator g_alloc;

using Bytes = std::vector<uint8_t>;
static Bytes bytes_of_text(const std::string& s) { return Bytes(s.begin(), s.end()); }
static uint32_t le32_at(ConstByteView v, uint64_t at) { return uint32_t{v[at]} | uint32_t{v[at + 1]} << 8 | uint32_t{v[at + 2]} << 16 | uint32_t{v[at + 3]} << 24; }
static bool read_file(const std::string& path, Bytes& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) out.insert(out.end(), chunk, chunk + n);
  fclose(f);
  return true;
}

// ---- the adapter table ----------------------------------------------------------------------------------------------------------------------------------------------------

static void test_table() {
  printf("table\n");
  const auto models = broadcom_wifi_models();
  CHECK(models.size() > 60);
  std::set<std::string> keys;
  uint32_t fmac = 0, fmac_named = 0;
  for (const WifiAdapterModel& m : models) {
    CHECK(m.vendor == WifiVendor::Broadcom);
    CHECK(m.chip && m.chip[0] && m.product && m.product[0] && m.firmware);
    CHECK(m.spatial_streams >= 1 && m.spatial_streams <= 4);
    CHECK(wifi_driver_family_vendor(m.family) == WifiVendor::Broadcom);
    char key[32];
    snprintf(key, sizeof key, "%d:%04x:%04x", static_cast<int>(m.bus), m.vendor_id, m.device_id);
    CHECK(keys.insert(key).second);
    if (m.firmware[0]) CHECK(wifi_firmware_name_valid(StringView(m.firmware)));
    if (m.family == WifiDriverFamily::BroadcomWl) CHECK(m.firmware[0] == 0);
    if (m.family == WifiDriverFamily::Brcmfmac) {
      ++fmac;
      CHECK(wifi_mac_split(m.family) == WifiMacSplit::FullMac);
      if (m.firmware[0]) {
        // the name is one a brcmfmac table can produce for this bus
        ++fmac_named;
        const BrcmfBusType bus = m.bus == WifiBus::Pci ? BrcmfBusType::Pcie : m.bus == WifiBus::Sdio ? BrcmfBusType::Sdio : BrcmfBusType::Usb;
        bool found = false;
        for (const BrcmfFirmwareMapping& f : brcmf_firmware_table(bus)) found |= std::string(f.base) + ".bin" == m.firmware;
        if (!found) printf("  %s: %s is not in the brcmfmac table\n", m.chip, m.firmware);
        CHECK(found);
      }
    }
  }
  CHECK(fmac > 50 && fmac_named > 35);

  const WifiAdapterModel* m = wifi_find_pci_adapter(0x14e4, 0x43ec);
  CHECK(m && strcmp(m->chip, "BCM4356") == 0 && strcmp(m->firmware, "brcm/brcmfmac4356-pcie.bin") == 0 && m->family == WifiDriverFamily::Brcmfmac);
  m = wifi_find_adapter(WifiBus::Sdio, 0x02d0, 0xa9bf);
  CHECK(m && strcmp(m->firmware, "brcm/brcmfmac43455-sdio.bin") == 0 && strstr(m->product, "Raspberry Pi 3 Model B+, 4"));
  m = wifi_find_adapter(WifiBus::Sdio, 0x02d0, 0xa9af);
  CHECK(m && strcmp(m->chip, "CYW43439") == 0);
  m = wifi_find_usb_adapter(0x0a5c, 0xbd1e);
  CHECK(m && strcmp(m->firmware, "brcm/brcmfmac43143.bin") == 0);
  m = wifi_find_pci_adapter(0x14e4, 0x4727);
  CHECK(m && m->family == WifiDriverFamily::Brcmsmac && wifi_mac_split(m->family) == WifiMacSplit::SoftMac);
  m = wifi_find_pci_adapter(0x14e4, 0x4331);
  CHECK(m && m->family == WifiDriverFamily::B43);
  m = wifi_find_pci_adapter(0x14e4, 0x43b1);
  CHECK(m && m->family == WifiDriverFamily::BroadcomWl);
  CHECK(wifi_find_pci_adapter(0x14e4, 0x1234) == nullptr);

  // the factory: brcmfmac on PCIe and SDIO only
  WifiChipBus bus{};
  CHECK(make_wifi_chip(*wifi_find_pci_adapter(0x14e4, 0x43ec), bus, g_alloc));
  CHECK(make_wifi_chip(*wifi_find_adapter(WifiBus::Sdio, 0x02d0, 0xa9bf), bus, g_alloc));
  CHECK(!make_wifi_chip(*wifi_find_usb_adapter(0x0a5c, 0xbd1e), bus, g_alloc));
  CHECK(!make_wifi_chip(*wifi_find_pci_adapter(0x14e4, 0x4727), bus, g_alloc));
  CHECK(!make_wifi_chip(*wifi_find_pci_adapter(0x14e4, 0x4331), bus, g_alloc));
  CHECK(!make_wifi_chip(*wifi_find_pci_adapter(0x14e4, 0x43b1), bus, g_alloc));
  auto chip = make_wifi_chip(*wifi_find_pci_adapter(0x14e4, 0x43ec), bus, g_alloc);
  CHECK(chip && chip->state() == WifiChipState::Off && strcmp(chip->firmware_name(), "brcm/brcmfmac4356-pcie.bin") == 0);
}

// ---- firmware names ---------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_firmware_names() {
  printf("firmware names\n");
  auto base = [](BrcmfBusType bus, uint32_t chip, uint32_t rev) -> std::string {
    const BrcmfFirmwareMapping* m = brcmf_find_firmware(bus, chip, rev);
    return m ? m->base : "";
  };
  CHECK(base(BrcmfBusType::Pcie, 0x4356, 2) == "brcm/brcmfmac4356-pcie");
  CHECK(base(BrcmfBusType::Pcie, 0x4350, 7) == "brcm/brcmfmac4350c2-pcie");
  CHECK(base(BrcmfBusType::Pcie, 0x4350, 8) == "brcm/brcmfmac4350-pcie");
  CHECK(base(BrcmfBusType::Pcie, 0xaa52, 1) == "brcm/brcmfmac43602-pcie");
  CHECK(base(BrcmfBusType::Pcie, 0x4355, 12) == "brcm/brcmfmac4355c1-pcie");
  CHECK(base(BrcmfBusType::Pcie, 0x4355, 13) == "brcm/brcmfmac54591-pcie");     // the 0x2000 entry comes before c1's mask, which also has bit 13
  CHECK(base(BrcmfBusType::Pcie, 0x4355, 10) == "brcm/brcmfmac4355-pcie");
  CHECK(base(BrcmfBusType::Pcie, 0x4364, 3) == "brcm/brcmfmac4364b2-pcie");
  CHECK(base(BrcmfBusType::Pcie, 0x4364, 4) == "brcm/brcmfmac4364b3-pcie");
  CHECK(base(BrcmfBusType::Pcie, 0x4378, 3) == "brcm/brcmfmac4378b1-pcie");
  CHECK(base(BrcmfBusType::Pcie, 0x4378, 4) == "");                           // neither 0xf nor 0xffffffe0 has bit 4
  CHECK(base(BrcmfBusType::Pcie, 0x4378, 5) == "brcm/brcmfmac4378b3-pcie");
  CHECK(base(BrcmfBusType::Sdio, 0x4345, 6) == "brcm/brcmfmac43455-sdio");
  CHECK(base(BrcmfBusType::Sdio, 0x4345, 9) == "brcm/brcmfmac43456-sdio");
  CHECK(base(BrcmfBusType::Sdio, 0x4345, 5) == "");
  CHECK(base(BrcmfBusType::Sdio, 0xa9a6, 0) == "brcm/brcmfmac43430a0-sdio");
  CHECK(base(BrcmfBusType::Sdio, 0xa9a6, 1) == "brcm/brcmfmac43430-sdio");
  CHECK(base(BrcmfBusType::Sdio, 0xa9a6, 2) == "brcm/brcmfmac43430b0-sdio");
  CHECK(base(BrcmfBusType::Sdio, 0xa962, 0) == "");
  CHECK(base(BrcmfBusType::Sdio, 0xa9af, 0) == "brcm/brcmfmac43439-sdio");
  CHECK(base(BrcmfBusType::Usb, 0xa8e4, 3) == "brcm/brcmfmac43236b");
  CHECK(base(BrcmfBusType::Usb, 0xa8e4, 2) == "");
  CHECK(base(BrcmfBusType::Pcie, 0x4356, 32) == "");
  CHECK(base(BrcmfBusType::Pcie, 0x1234, 0) == "");

  BrcmfFileName name, board;
  CHECK(brcmf_file_name("brcm/brcmfmac4356-pcie", kBrcmfExtensionNvram, name) && name == "brcm/brcmfmac4356-pcie.txt");
  CHECK(brcmf_file_name("brcm/brcmfmac4356-pcie", kBrcmfExtensionClm, name) && name == "brcm/brcmfmac4356-pcie.clm_blob");
  CHECK(brcmf_file_name("brcm/brcmfmac4356-pcie", kBrcmfExtensionTxcap, name) && name == "brcm/brcmfmac4356-pcie.txcap_blob");
  CHECK(brcmf_board_file_name("brcm/brcmfmac43455-sdio.txt", "raspberrypi,4-model-b", board) && board == "brcm/brcmfmac43455-sdio.raspberrypi,4-model-b.txt");
  CHECK(brcmf_board_file_name("brcm/brcmfmac4356-pcie.bin", "gpd-win-pocket", board) && board == "brcm/brcmfmac4356-pcie.gpd-win-pocket.bin");
  CHECK(!brcmf_board_file_name("brcm/noextension", "x", board));
  CHECK(!brcmf_board_file_name(".txt", "x", board));
  CHECK(!brcmf_board_file_name("a.txt", "", board));
  CHECK(brcmf_board_type_from_dmi("Raspberry Pi Foundation", "Raspberry Pi 4 Model B", board) && board == "Raspberry Pi Foundation-Raspberry Pi 4 Model B");
  CHECK(brcmf_board_type_from_dmi("Vendor/X", "A/B", board) && board == "Vendor-X-A-B");
  CHECK(!brcmf_board_type_from_dmi("", "x", board));
  CHECK(brcmf_board_type_from_compatible("raspberrypi,5-model-b", board) && board == "raspberrypi,5-model-b");
  std::string long_name(300, 'a');
  CHECK(!brcmf_board_type_from_compatible(StringView(long_name.c_str()), board));
  BrcmfChipName chip;
  brcmf_chip_name(0x4356, 2, chip);
  CHECK(chip == "BCM4356/2");
  brcmf_chip_name(0xaa52, 1, chip);
  CHECK(chip == "BCM43602/1");
  brcmf_chip_name(0xa9a6, 1, chip);
  CHECK(chip == "BCM43430/1");
  brcmf_chip_name(0x4345, 6, chip);
  CHECK(chip == "BCM4345/6");

  // versions
  char version[48];
  const std::string image = std::string("\x00\x10\x18\x00junk wl%d: %s version %s FWID Version: %s\0", 45) +
                            "43455c0-roml/43455_sdio-pno Version: 7.45.286 (be70ab3 CY) CRC: 8b7601a5 Date: Mon";
  CHECK(brcmf_image_version(ConstByteView(reinterpret_cast<const uint8_t*>(image.data()), image.size()), ArrayView<char>(version)) &&
        strcmp(version, "7.45.286") == 0);
  const std::string none = "no version here";
  CHECK(!brcmf_image_version(ConstByteView(reinterpret_cast<const uint8_t*>(none.data()), none.size()), ArrayView<char>(version)) && version[0] == 0);
  const std::string console = "\n000000.003 \n000000.004 RTE (PCIE-MSG) 7.35.180.197 (r722171) on BCM4356 r2 @ 37.4/161.0/800.0MHz\n";
  CHECK(brcmf_console_version(ConstByteView(reinterpret_cast<const uint8_t*>(console.data()), console.size()), ArrayView<char>(version)) &&
        strcmp(version, "7.35.180.197") == 0);
  char tiny[4];
  CHECK(brcmf_console_version(ConstByteView(reinterpret_cast<const uint8_t*>(console.data()), console.size()), ArrayView<char>(tiny)) && strcmp(tiny, "7.3") == 0);
}

// ---- NVRAM ------------------------------------------------------------------------------------------------------------------------------------------------------------------

// The expected stripped image of `entries` (each without its zero).
static Bytes nvram_image(const std::vector<std::string>& entries) {
  Bytes out;
  for (const std::string& e : entries) {
    out.insert(out.end(), e.begin(), e.end());
    out.push_back(0);
  }
  const size_t padded = (out.size() + 1 + 3) & ~size_t{3};
  out.resize(padded, 0);
  const uint32_t words = static_cast<uint32_t>(padded / 4);
  const uint32_t token = (~words << 16) | (words & 0xffff);
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(token >> (8 * i)));
  return out;
}

static BrcmfNvramResult strip(const std::string& text, Bytes& out, BrcmfNvramOptions options = {}) {
  ByteBuffer buffer(g_alloc);
  const BrcmfNvramResult r = brcmf_nvram_strip(ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()), options, buffer);
  out.assign(buffer.view().begin(), buffer.view().end());
  return r;
}

static bool image_well_formed(const Bytes& image) {
  if (image.size() < 8 || image.size() % 4 != 0) return false;
  const uint32_t token = le32_at(ConstByteView(image.data(), image.size()), image.size() - 4);
  const uint32_t words = static_cast<uint32_t>((image.size() - 4) / 4);
  return (token & 0xffff) == (words & 0xffff) && (token >> 16) == ((~words) & 0xffff) && image[image.size() - 5] == 0;
}

static void test_nvram() {
  printf("nvram\n");
  Bytes out;
  CHECK(strip("# comment line\nfoo=bar\n  baz=1 2 \n\nRAW1=xx\nbad key=1\n\tboardrev=0x1304\nx=1#trailing comment\r\ny=2\r\n", out) == BrcmfNvramResult::Ok);
  CHECK(out == nvram_image({"foo=bar", "baz=1 2 ", "boardrev=0x1304", "x=1", "y=2"}));
  // no boardrev: the default is added; a last line without a newline is dropped (as in Linux)
  CHECK(strip("a=1\nb=2", out) == BrcmfNvramResult::Ok && out == nvram_image({"a=1", "boardrev=0xff"}));
  CHECK(strip("a=1\nb=2\n", out) == BrcmfNvramResult::Ok && out == nvram_image({"a=1", "b=2", "boardrev=0xff"}));
  // padding: 3 + 1 + "boardrev=0xff" (13 + 1) = 18 -> 20; and an exact multiple still gets one more zero word
  CHECK(strip("ab=\n", out) == BrcmfNvramResult::Ok && out.size() == 20 + 4 && image_well_formed(out));
  CHECK(strip("abc=12\n", out) == BrcmfNvramResult::Ok && image_well_formed(out));
  // a NUL ends a comment like a newline; bytes outside printable ASCII are skipped in Idle
  CHECK(strip(std::string("#x\0a=1\n\x01\xff" "b=2\n", 13), out) == BrcmfNvramResult::Ok && out == nvram_image({"a=1", "b=2", "boardrev=0xff"}));
  // a key with an invalid character drops the rest of its line
  CHECK(strip("k\x01y=1\nz=2\n", out) == BrcmfNvramResult::Ok && out == nvram_image({"z=2", "boardrev=0xff"}));
  // empty and comment-only files have nothing
  CHECK(strip("", out) == BrcmfNvramResult::Empty);
  CHECK(strip("# only\n# comments\n\n", out) == BrcmfNvramResult::Empty);
  CHECK(strip("novalue\n", out) == BrcmfNvramResult::Empty);
  // a platform address replaces the file's
  BrcmfNvramOptions mac;
  mac.has_platform_mac = true;
  const uint8_t address[6] = {0x02, 0x11, 0x22, 0xab, 0xcd, 0xef};
  memcpy(mac.platform_mac, address, 6);
  CHECK(strip("macaddr=00:90:4c:c5:12:38\nboardrev=1\n", out, mac) == BrcmfNvramResult::Ok && out == nvram_image({"boardrev=1", "macaddr=02:11:22:ab:cd:ef"}));

  // multi-device v2: pcie/<domain>/<bus>/ prefixes
  const std::string v2 = "pcie/1/4/boardrev=0x10\npcie/1/4/x=1\npcie/1/5/x=2\ncommon=3\npcie/1/40/x=4\n";
  BrcmfNvramOptions dev;
  dev.domain_nr = 1;
  dev.bus_nr = 4;
  CHECK(strip(v2, out, dev) == BrcmfNvramResult::Ok && out == nvram_image({"boardrev=0x10", "x=1"}));
  dev.bus_nr = 5;
  CHECK(strip(v2, out, dev) == BrcmfNvramResult::Ok && out == nvram_image({"x=2", "boardrev=0xff"}));
  dev.bus_nr = 9;
  CHECK(strip(v2, out, dev) == BrcmfNvramResult::Empty);
  // multi-device v1: devpath<n>=pci[e]/<domain>/<bus>/ and n: prefixes
  const std::string v1 = "devpath0=pcie/1/4/\n0:x=1\n1:x=2\ndevpath1=pci/1/5/\n1:boardrev=0x22\n0:y=3\nfree=1\n";
  dev.bus_nr = 4;
  CHECK(strip(v1, out, dev) == BrcmfNvramResult::Ok && out == nvram_image({"x=1", "y=3", "boardrev=0xff"}));
  dev.bus_nr = 5;
  CHECK(strip(v1, out, dev) == BrcmfNvramResult::Ok && out == nvram_image({"x=2", "boardrev=0x22"}));
  dev.bus_nr = 7;
  CHECK(strip(v1, out, dev) == BrcmfNvramResult::Empty);
  CHECK(strip("devpath0=x\n", out, dev) == BrcmfNvramResult::Empty);    // too short for a device path
  CHECK(strip("pcie/1/4/\n", out, dev) == BrcmfNvramResult::Empty);

  // the output stops before BRCMF_FW_MAX_NVRAM_SIZE
  std::string big;
  for (int i = 0; i < 9000; ++i) big += "key" + std::to_string(i) + "=value\n";
  CHECK(strip(big, out) == BrcmfNvramResult::Ok && out.size() <= brcmf_nvram::kMaxBytes + 32 && image_well_formed(out));

  // brcmf_nvram_find
  CHECK(strip("aa=1\naab=2\nb=\n", out) == BrcmfNvramResult::Ok);
  StringView value;
  CHECK(brcmf_nvram_find(ConstByteView(out.data(), out.size()), "aab", &value) && value == "2");
  CHECK(brcmf_nvram_find(ConstByteView(out.data(), out.size()), "aa", &value) && value == "1");
  CHECK(brcmf_nvram_find(ConstByteView(out.data(), out.size()), "b", &value) && value.empty());    // "b=" is kept with an empty value
  CHECK(!brcmf_nvram_find(ConstByteView(out.data(), out.size()), "zz", &value));

  // damage: every truncation and single bit flip of a multi-device file, and random bytes, give a well-formed image or nothing
  const std::string sample = v1 + "# c\nboardflags=0x00480201\nmacaddr=00:90:4c:c5:12:38\n" + v2;
  for (size_t cut = 0; cut <= sample.size(); ++cut) {
    const BrcmfNvramResult r = strip(sample.substr(0, cut), out, dev);
    CHECK(r == BrcmfNvramResult::Empty || (r == BrcmfNvramResult::Ok && image_well_formed(out)));
  }
  for (size_t i = 0; i < sample.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      std::string flipped = sample;
      flipped[i] = static_cast<char>(flipped[i] ^ (1 << bit));
      for (uint16_t bus : {4, 5}) {
        dev.bus_nr = bus;
        const BrcmfNvramResult r = strip(flipped, out, dev);
        CHECK(r == BrcmfNvramResult::Empty || (r == BrcmfNvramResult::Ok && image_well_formed(out)));
      }
    }
  uint32_t seed = 12345;
  for (int round = 0; round < 300; ++round) {
    std::string junk(1 + round * 7 % 900, '\0');
    for (char& c : junk) {
      seed = seed * 1103515245 + 12345;
      c = static_cast<char>(seed >> 16);
    }
    const BrcmfNvramResult r = strip(junk, out, round % 2 ? dev : BrcmfNvramOptions{});
    CHECK(r == BrcmfNvramResult::Empty || (r == BrcmfNvramResult::Ok && image_well_formed(out)));
  }
}

// Real NVRAM files from linux-firmware (decompressed into BRCMF_FIRMWARE_DIR).
static void test_real_nvram() {
  const char* dir = getenv("BRCMF_FIRMWARE_DIR");
  if (!dir) {
    printf("  (BRCMF_FIRMWARE_DIR not set: no real NVRAM files tested)\n");
    return;
  }
  DIR* d = opendir(dir);
  if (!d) {
    printf("  (cannot open %s)\n", dir);
    return;
  }
  int files = 0;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.size() < 5 || name.substr(name.size() - 4) != ".txt") continue;
    Bytes text, out;
    if (!read_file(std::string(dir) + "/" + name, text)) continue;
    ++files;
    const BrcmfNvramResult r = strip(std::string(text.begin(), text.end()), out);
    CHECK(r == BrcmfNvramResult::Ok && image_well_formed(out));
    // every kept entry is key=value with a printable key, no '#', and every non-comment line of the file is one of them (or the last one without a newline)
    size_t entries = 0;
    bool boardrev = false;
    for (size_t i = 0; i + 4 < out.size();) {
      size_t end = i;
      while (end < out.size() && out[end]) ++end;
      if (end == i) break;
      const std::string entry(out.begin() + i, out.begin() + end);
      CHECK(entry.find('=') != std::string::npos && entry.find('#') == std::string::npos && entry.find('\r') == std::string::npos);
      boardrev |= entry.compare(0, 9, "boardrev=") == 0;
      ++entries;
      i = end + 1;
    }
    CHECK(boardrev);
    // count the file's entry lines
    size_t lines = 0;
    std::string line;
    const std::string body(text.begin(), text.end());
    size_t start = 0;
    bool file_boardrev = false;
    while (start < body.size()) {
      size_t nl = body.find('\n', start);
      const bool complete = nl != std::string::npos;
      if (!complete) nl = body.size();
      std::string l = body.substr(start, nl - start);
      size_t a = l.find_first_not_of(" \t\r");
      // an entry line: a key of printable characters without blanks up to '=' (Linux drops "key =value" lines, as some vendor files have)
      const size_t eq = l.find('=', a == std::string::npos ? 0 : a);
      bool key_ok = a != std::string::npos && eq != std::string::npos && eq > a && l[a] != '#';
      for (size_t k = a; key_ok && k < eq; ++k) key_ok = l[k] > 0x20 && l[k] < 0x7f && l[k] != '#';
      if (complete && key_ok) {
        ++lines;
        file_boardrev |= l.compare(a, 9, "boardrev=") == 0;
      }
      start = nl + 1;
    }
    if (entries != lines + (file_boardrev ? 0 : 1)) printf("  %s: %zu entries, %zu lines\n", name.c_str(), entries, lines);
    CHECK(entries == lines + (file_boardrev ? 0 : 1));
    if (files <= 3) printf("  %s: %zu bytes of text -> %zu bytes, %zu entries\n", name.c_str(), text.size(), out.size(), entries);
  }
  closedir(d);
  printf("  %d real NVRAM files stripped\n", files);
  CHECK(files > 0);
  // every brcmfmac image names its version
  d = opendir(dir);
  int images = 0;
  while (dirent* e = d ? readdir(d) : nullptr) {
    const std::string name = e->d_name;
    if (name.compare(0, 8, "brcmfmac") != 0 || name.size() < 5 || name.substr(name.size() - 4) != ".bin") continue;
    Bytes image;
    if (!read_file(std::string(dir) + "/" + name, image)) continue;
    char version[48];
    const bool ok = brcmf_image_version(ConstByteView(image.data(), image.size()), ArrayView<char>(version));
    if (!ok || images < 3) printf("  %s: %s\n", name.c_str(), ok ? version : "(no version)");
    // (old USB images such as brcmfmac43236b.bin have the bare number without "Version: ")
    CHECK(ok || (name.find("-pcie") == std::string::npos && name.find("-sdio") == std::string::npos));
    ++images;
  }
  if (d) closedir(d);
  printf("  %d real images checked for their version\n", images);
}

// ---- the enumeration ROM ----------------------------------------------------------------------------------------------------------------------------------------------------

namespace erom {
constexpr uint32_t cia(uint32_t designer, uint32_t part, uint32_t cls) { return designer << 20 | part << 8 | cls << 4 | 1; }
constexpr uint32_t cib(uint32_t rev, uint32_t nsw, uint32_t nmw, uint32_t nsp, uint32_t nmp) { return rev << 24 | nsw << 19 | nmw << 14 | nsp << 9 | nmp << 4 | 1; }
constexpr uint32_t mpd(uint32_t uid, uint32_t port) { return uid << 8 | port << 4 | 3; }
constexpr uint32_t asd(uint32_t base, uint32_t port, uint32_t type, uint32_t size, bool gt32 = false) {
  return (base & 0xfffff000) | port << 8 | type << 6 | size << 4 | (gt32 ? 8u : 0u) | 5;
}
constexpr uint32_t kEot = 0xf;
constexpr uint32_t kBcm = 0x4bf, kArm = 0x43b;
}  // namespace erom

struct CoreSpec {
  uint16_t id;
  uint8_t rev;
  uint32_t base, wrap;
  bool master;      // the wrapper is a master wrapper (cores with master ports, the ARMs)
};

static std::vector<uint32_t> build_rom(const std::vector<CoreSpec>& cores) {
  using namespace erom;
  std::vector<uint32_t> rom;
  for (const CoreSpec& c : cores) {
    rom.push_back(cia(c.id == 0x83e || c.id == 0x82a ? kArm : kBcm, c.id, 0));
    rom.push_back(cib(c.rev, c.master ? 0 : 1, c.master ? 1 : 0, 1, c.master ? 1 : 0));
    if (c.master) rom.push_back(mpd(0, 0));
    rom.push_back(asd(c.base, 0, 0, 0));
    rom.push_back(asd(c.wrap, 0, c.master ? 3 : 2, 0));
  }
  rom.push_back(kEot);
  return rom;
}

static void test_erom() {
  printf("erom\n");
  using namespace erom;
  std::vector<uint32_t> rom = {
      0,    // an empty descriptor is skipped
      // chipcommon rev 43: slave 4K, slave wrapper
      cia(kBcm, 0x800, 0), cib(43, 1, 0, 1, 0), asd(0x18000000, 0, 0, 0), asd(0x18100000, 0, 2, 0),
      // CR4 rev 7 with a master port, a 64-bit address descriptor for an extra region and a size descriptor (with an upper word), then its registers and master wrapper
      cia(kArm, 0x83e, 0), cib(7, 0, 1, 2, 1), mpd(1, 0), asd(0x00000000, 1, 0, 3, true), 0x00000001, 0x00180008 | 8, 0x0, asd(0x18002000, 0, 0, 0),
      asd(0x18102000, 0, 3, 0),
      // the 802.11 core: a 16K region first (ignored), then 8K registers and its wrapper
      cia(kBcm, 0x812, 0), cib(42, 1, 0, 2, 0), asd(0x10000000, 1, 0, 2), asd(0x18001000, 0, 0, 1), asd(0x18101000, 0, 2, 0),
      // a core without wrappers (not PMU/GCI): skipped
      cia(kBcm, 0x367, 0), cib(1, 0, 0, 1, 0), asd(0x18200000, 0, 0, 0),
      // PMU without wrappers: kept with its register address
      cia(kBcm, 0x827, 0), cib(30, 0, 0, 1, 0), asd(0x18012000, 0, 0, 0), asd(0x18112000, 0, 2, 0),
      // a core whose first descriptor is not an address: skipped, the descriptor is looked at again
      cia(kBcm, 0x83c, 0), cib(1, 1, 0, 1, 0), cia(kBcm, 0x83c, 0), cib(11, 1, 0, 1, 0), asd(0x18003000, 0, 0, 0), asd(0x18103000, 0, 2, 0),
      // a bridge region does not count as the wrapper
      cia(kBcm, 0x829, 0), cib(21, 1, 0, 1, 0), asd(0x18004000, 0, 1, 0), asd(0x18005000, 0, 0, 0), asd(0x18105000, 0, 2, 0),
      kEot,
      0xdeadbeef,    // after the end: never read
  };
  BrcmfCoreList cores;
  CHECK(brcmf_erom_scan(ArrayView<const uint32_t>(rom.data(), rom.size()), cores) == BrcmfEromResult::Ok);
  CHECK(cores.count == 6);
  const BrcmfCore* c = cores.find(0x800);
  CHECK(c && c->revision == 43 && c->base == 0x18000000 && c->wrapper == 0x18100000);
  c = cores.find(0x83e);
  CHECK(c && c->revision == 7 && c->base == 0x18002000 && c->wrapper == 0x18102000);
  c = cores.find(0x812);
  CHECK(c && c->revision == 42 && c->base == 0x18001000 && c->wrapper == 0x18101000);
  CHECK(!cores.find(0x367));
  c = cores.find(0x827);
  CHECK(c && c->revision == 30 && c->base == 0x18012000 && c->wrapper == 0x18112000);
  c = cores.find(0x83c);
  CHECK(c && c->revision == 11 && c->base == 0x18003000 && cores.find(0x83c, 1) == nullptr);
  c = cores.find(0x829);
  CHECK(c && c->base == 0x18005000 && c->wrapper == 0x18105000);
  CHECK(brcmf_cpu_of(cores) == BrcmfCpu::Cr4 && brcmf_cores_usable(cores, 0x83c) && brcmf_cores_usable(cores, 0x829) && !brcmf_cores_usable(cores, 0x849));

  // every truncation before the end marker says so
  for (size_t cut = 0; cut + 2 < rom.size(); ++cut) {
    BrcmfCoreList partial;
    const BrcmfEromResult r = brcmf_erom_scan(ArrayView<const uint32_t>(rom.data(), cut), partial);
    CHECK(r == BrcmfEromResult::Truncated && partial.count <= 6);
  }
  // single bit flips: never out of bounds, never more cores than there are component pairs
  for (size_t i = 0; i < rom.size(); ++i)
    for (int bit = 0; bit < 32; ++bit) {
      std::vector<uint32_t> flipped = rom;
      flipped[i] ^= 1u << bit;
      BrcmfCoreList list;
      (void)brcmf_erom_scan(ArrayView<const uint32_t>(flipped.data(), flipped.size()), list);
      CHECK(list.count <= 10);
    }
  // garbage
  uint32_t seed = 777;
  for (int round = 0; round < 2000; ++round) {
    std::vector<uint32_t> junk(1 + round % 1024);
    for (uint32_t& w : junk) {
      seed = seed * 1664525 + 1013904223;
      w = seed;
      if ((seed >> 28) == 3) w = (w & ~0xfu) | 1;    // plenty of component descriptors
    }
    BrcmfCoreList list;
    (void)brcmf_erom_scan(ArrayView<const uint32_t>(junk.data(), junk.size()), list);
    CHECK(list.count <= BrcmfCoreList::kMaxCores);
  }
  std::vector<uint32_t> ones(1024, 0xffffffff), zeros(1024, 0);
  BrcmfCoreList list;
  CHECK(brcmf_erom_scan(ArrayView<const uint32_t>(ones.data(), ones.size()), list) == BrcmfEromResult::Ok && list.count == 0);
  CHECK(brcmf_erom_scan(ArrayView<const uint32_t>(zeros.data(), zeros.size()), list) == BrcmfEromResult::Truncated && list.count == 0);
  CHECK(brcmf_erom_scan(ArrayView<const uint32_t>(), list) == BrcmfEromResult::Truncated);
  // a CIA without its CIB
  const uint32_t bad[] = {cia(kBcm, 0x800, 0), asd(0x18000000, 0, 0, 0), kEot};
  CHECK(brcmf_erom_scan(ArrayView<const uint32_t>(bad), list) == BrcmfEromResult::BadComponent);
  // an address list that ends at the end marker: the core is skipped, the scan ends well
  const uint32_t open_end[] = {cia(kBcm, 0x800, 0), cib(1, 1, 0, 1, 0), asd(0x18000000, 0, 0, 0), kEot};
  CHECK(brcmf_erom_scan(ArrayView<const uint32_t>(open_end), list) == BrcmfEromResult::Ok && list.count == 0);
  // too many cores
  std::vector<CoreSpec> many;
  for (uint32_t i = 0; i < 40; ++i) many.push_back({static_cast<uint16_t>(0x800 + (i % 3)), 1, 0x18000000 + i * 0x1000, 0x18100000 + i * 0x1000, false});
  const std::vector<uint32_t> big = build_rom(many);
  CHECK(brcmf_erom_scan(ArrayView<const uint32_t>(big.data(), big.size()), list) == BrcmfEromResult::TooManyCores && list.count == BrcmfCoreList::kMaxCores);

  const BrcmfChipId id = brcmf_decode_chip_id(0x16354356);
  CHECK(id.chip == 0x4356 && id.revision == 5 && id.package == 3 && id.core_count == 6 && id.socitype == 1);
}

// ---- a model of the chip's backplane ----------------------------------------------------------------------------------------------------------------------------------------

struct ChipModel {
  uint32_t chip_id_value = 0;
  uint32_t erom_address = 0x1810d000;
  std::vector<uint32_t> erom;
  std::vector<CoreSpec> cores;
  uint32_t rambase = 0, ramsize = 0;
  std::vector<uint8_t> ram;
  std::map<uint32_t, uint32_t> regs;
  // CR4 TCM: capability and bank info
  uint32_t cr4_cap = 0, cr4_bankinfo = 0, cr4_bankidx = 0;
  // SOCRAM
  uint32_t socram_coreinfo = 0, socram_bankinfo = 0, socram_bankidx = 0;
  std::vector<std::pair<uint32_t, uint32_t>> socram_bank_writes;
  uint32_t watchdog_writes = 0;
  // the firmware
  bool firmware_starts = true;
  bool pcie = true;
  uint32_t shared_version = 6;
  std::string banner = "\n000000.004 RTE (PCIE-MSG) 7.35.180.197 (r722171) on BCM4356 r2 @ 37.4/161.0/800.0MHz\n";
  bool started = false;
  uint32_t start_count = 0;
  uint32_t reset_vector_seen = 0;
  Bytes image_at_start;    // the first bytes of RAM when the ARM was released

  const CoreSpec* core(uint16_t id) const {
    for (const CoreSpec& c : cores)
      if (c.id == id) return &c;
    return nullptr;
  }
  bool in_ram(uint32_t address, uint32_t n) const { return address >= rambase && address - rambase <= ramsize && n <= ramsize - (address - rambase); }

  void setup(uint32_t id_value, std::vector<CoreSpec> list, uint32_t base, uint32_t size) {
    chip_id_value = id_value;
    cores = std::move(list);
    erom = build_rom(cores);
    rambase = base;
    ramsize = size;
    ram.assign(size, 0xa5);
    for (const CoreSpec& c : cores) {
      regs[c.wrap + brcmf_bp::kWrapResetCtl] = 1;    // cores come up in reset
      regs[c.wrap + brcmf_bp::kWrapIoctl] = 0;
    }
  }

  uint32_t read32(uint32_t address) {
    if (in_ram(address, 4)) return le32_at(ConstByteView(ram.data(), ram.size()), address - rambase);
    if (address == brcmf_bp::kEnumBase) return chip_id_value;
    if (address == brcmf_bp::kEnumBase + brcmf_bp::kCcEromPtr) return erom_address;
    if (address >= erom_address && address < erom_address + 0x1000) {
      const uint32_t index = (address - erom_address) / 4;
      return index < erom.size() ? erom[index] : 0;
    }
    if (const CoreSpec* cr4 = core(0x83e)) {
      if (address == cr4->base + brcmf_bp::kCr4Cap) return cr4_cap;
      if (address == cr4->base + brcmf_bp::kCr4BankInfo) return cr4_bankinfo;
    }
    if (const CoreSpec* sr = core(0x80e)) {
      if (address == sr->base + brcmf_bp::kSocramCoreInfo) return socram_coreinfo;
      if (address == sr->base + brcmf_bp::kSocramBankInfo) return socram_bankinfo;
    }
    auto it = regs.find(address);
    return it == regs.end() ? 0 : it->second;
  }

  void write32(uint32_t address, uint32_t value) {
    if (in_ram(address, 4)) {
      for (int i = 0; i < 4; ++i) ram[address - rambase + i] = static_cast<uint8_t>(value >> (8 * i));
      return;
    }
    if (address == brcmf_bp::kEnumBase + brcmf_bp::kCcWatchdog) ++watchdog_writes;
    if (const CoreSpec* cr4 = core(0x83e))
      if (address == cr4->base + brcmf_bp::kCr4BankIdx) cr4_bankidx = value;
    if (const CoreSpec* sr = core(0x80e)) {
      if (address == sr->base + brcmf_bp::kSocramBankIdx) socram_bankidx = value;
      if (address == sr->base + brcmf_bp::kSocramBankIdx || address == sr->base + brcmf_bp::kSocramBankPda) socram_bank_writes.push_back({address - sr->base, value});
    }
    regs[address] = value;
    // the ARM leaves reset with its clock on and not halted: the "firmware" runs
    for (const char* name : {"cr4", "cm3"}) {
      const CoreSpec* arm = core(name[1] == 'r' ? 0x83e : 0x82a);
      if (!arm || address != arm->wrap + brcmf_bp::kWrapIoctl) continue;
      const bool running = value == brcmf_bp::kIoctlClk && regs[arm->wrap + brcmf_bp::kWrapResetCtl] == 0;
      if (running && !started) firmware_start();
      if (!running) started = false;
    }
  }
  void write8(uint32_t address, uint8_t value) {
    if (in_ram(address, 1)) ram[address - rambase] = value;
  }
  uint8_t read8(uint32_t address) { return in_ram(address, 1) ? ram[address - rambase] : static_cast<uint8_t>(read32(address & ~3u) >> (8 * (address & 3))); }

  void put32(uint32_t address, uint32_t value) { write32(address, value); }

  void firmware_start() {
    started = true;
    ++start_count;
    reset_vector_seen = read32(0);
    image_at_start.assign(ram.begin(), ram.begin() + 64);
    if (!firmware_starts) return;
    const uint32_t shared = rambase + ramsize - 0x3000;
    const uint32_t console = shared + 0x100;
    const uint32_t log = shared + 0x200;
    for (uint32_t i = 0; i < 0x400; i += 4) put32(shared + i, 0);
    if (pcie) {
      put32(shared, shared_version | brcmf_pcie::kSharedDmaIndex);
      put32(shared + brcmf_pcie::kSharedConsoleAddrOffset, console);
      put32(shared + brcmf_pcie::kSharedRxDataOffsetOffset, 8);
      put32(shared + brcmf_pcie::kSharedRingInfoAddrOffset, shared + 0x80);
    } else {
      put32(shared, 3);
      put32(shared + brcmf_sdio::kSharedConsoleAddrOffset, console);
    }
    put32(console + brcmf_pcie::kConsoleBufAddrOffset, log);
    put32(console + brcmf_pcie::kConsoleBufSizeOffset, 0x400);
    put32(console + brcmf_pcie::kConsoleWriteIdxOffset, static_cast<uint32_t>(banner.size()));
    for (size_t i = 0; i < banner.size() && i < 0x400; ++i) ram[log - rambase + i] = static_cast<uint8_t>(banner[i]);
    put32(rambase + ramsize - 4, shared);
  }
};

struct FakePci final : IPciBus {
  mutable uint32_t window = 0;
  mutable uint32_t window_writes = 0;
  mutable std::map<uint16_t, uint32_t> config;
  const char* name() const override { return "fake pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress, uint16_t offset) const override {
    if (offset == brcmf_pcie::kBar0Window) return window;
    auto it = config.find(offset);
    return it == config.end() ? 0 : it->second;
  }
  void write32(PciAddress, uint16_t offset, uint32_t value) const override {
    if (offset == brcmf_pcie::kBar0Window) {
      window = value;
      ++window_writes;
    } else {
      config[offset] = value;
    }
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

struct FakeBar0 final : INicRegisters {
  ChipModel& chip;
  FakePci& pci;
  uint32_t out_of_window = 0;
  FakeBar0(ChipModel& c, FakePci& p) : chip(c), pci(p) {}
  uint32_t at(uint32_t offset) {
    if (offset >= brcmf_pcie::kBar0RegSize) ++out_of_window;
    return pci.window + (offset & (brcmf_pcie::kBar0RegSize - 1));
  }
  uint8_t read8(uint32_t offset) override { return chip.read8(at(offset)); }
  uint16_t read16(uint32_t offset) override { return static_cast<uint16_t>(chip.read32(at(offset) & ~3u) >> (8 * (offset & 2))); }
  uint32_t read32(uint32_t offset) override { return chip.read32(at(offset)); }
  void write8(uint32_t offset, uint8_t value) override { chip.write8(at(offset), value); }
  void write16(uint32_t, uint16_t) override {}
  void write32(uint32_t offset, uint32_t value) override { chip.write32(at(offset), value); }
};

struct FakeSdio final : IWifiSdio {
  ChipModel& chip;
  uint8_t io_enable = 0;
  uint8_t window[3] = {};
  uint8_t clock_csr = 0;
  bool alp_works = true;
  uint32_t ht_requests = 0;
  explicit FakeSdio(ChipModel& c) : chip(c) {}
  uint32_t backplane(uint32_t address) const {
    const uint32_t base = uint32_t{window[0]} << 8 | uint32_t{window[1]} << 16 | uint32_t{window[2]} << 24;
    return base | (address & brcmf_sdio::kOffsetMask);
  }
  bool read8(uint8_t function, uint32_t address, uint8_t* value) override {
    if (function == 0) {
      *value = address == brcmf_sdio::kCccrIoEnable || address == brcmf_sdio::kCccrIoReady ? io_enable : 0;
      return true;
    }
    if (function != 1) return false;
    if (address == brcmf_sdio::kChipClkCsr) {
      uint8_t avail = 0;
      if (alp_works && (clock_csr & (brcmf_sdio::kAlpAvailReq | brcmf_sdio::kForceAlp | brcmf_sdio::kHtAvailReq))) avail |= brcmf_sdio::kAlpAvail;
      if (clock_csr & brcmf_sdio::kHtAvailReq) avail |= brcmf_sdio::kHtAvail;
      *value = clock_csr | avail;
      return true;
    }
    if (address >= brcmf_sdio::kSbAddrLow && address <= brcmf_sdio::kSbAddrHigh) {
      *value = window[address - brcmf_sdio::kSbAddrLow];
      return true;
    }
    *value = 0;
    return true;
  }
  bool write8(uint8_t function, uint32_t address, uint8_t value) override {
    if (function == 0) {
      if (address == brcmf_sdio::kCccrIoEnable) io_enable = value;
      return true;
    }
    if (function != 1) return false;
    if (address == brcmf_sdio::kChipClkCsr) {
      clock_csr = value;
      if (value & brcmf_sdio::kHtAvailReq) ++ht_requests;
    }
    if (address >= brcmf_sdio::kSbAddrLow && address <= brcmf_sdio::kSbAddrHigh) window[address - brcmf_sdio::kSbAddrLow] = value;
    return true;
  }
  bool read_block(uint8_t function, uint32_t address, ByteView data) override {
    if (function != 1 || !(address & brcmf_sdio::kAccess24BFlag) || !(io_enable & brcmf_sdio::kFunction1Bit)) return false;
    if ((address & brcmf_sdio::kOffsetMask) + data.size() > brcmf_sdio::kOffsetLimit) return false;    // a transfer must stay in the window
    const uint32_t at = backplane(address);
    for (uint64_t i = 0; i < data.size(); ++i) data[i] = chip.read8(at + static_cast<uint32_t>(i));
    return true;
  }
  bool write_block(uint8_t function, uint32_t address, ConstByteView data) override {
    if (function != 1 || !(address & brcmf_sdio::kAccess24BFlag) || !(io_enable & brcmf_sdio::kFunction1Bit)) return false;
    if ((address & brcmf_sdio::kOffsetMask) + data.size() > brcmf_sdio::kOffsetLimit) return false;
    const uint32_t at = backplane(address);
    if (data.size() == 4 && !chip.in_ram(at, 4)) {
      chip.write32(at, le32_at(data, 0));
      return true;
    }
    for (uint64_t i = 0; i < data.size(); ++i) chip.write8(at + static_cast<uint32_t>(i), data[i]);
    return true;
  }
};

struct MapFirmware final : IWifiFirmwareSource {
  std::map<std::string, Bytes> files;
  std::vector<std::string> asked;
  bool read(const char* name, ByteBuffer& out) override {
    asked.push_back(name);
    auto it = files.find(name);
    if (it == files.end() || it->second.empty()) return false;
    out.clear();
    return out.append(ConstByteView(it->second.data(), it->second.size()));
  }
};

static Bytes make_image(size_t size, const char* version) {
  Bytes image(size);
  for (size_t i = 0; i < size; ++i) image[i] = static_cast<uint8_t>(i * 7 + 3);
  image[0] = 0x78;    // the reset vector
  image[1] = 0x56;
  image[2] = 0x34;
  image[3] = 0x12;
  const std::string text = std::string("4356a2-roml/pcie-ag-msgbuf Version: ") + version + " (r722171 CY) CRC: bd7297a1 Date: Thu";
  if (size > text.size() + 100) memcpy(image.data() + size - text.size() - 50, text.data(), text.size());
  return image;
}

// 4356 rev 2: CR4, 6 banks of 128 KiB = 768 KiB at 0x180000
static std::vector<CoreSpec> pcie_cores(uint8_t pcie_rev = 1) {
  return {{0x800, 52, 0x18000000, 0x18100000, false},
          {0x812, 42, 0x18001000, 0x18101000, false},
          {0x83e, 7, 0x18002000, 0x18102000, true},
          {0x83c, pcie_rev, 0x18003000, 0x18103000, false},
          {0x827, 26, 0x18012000, 0x18112000, false}};
}

static void setup_4356(ChipModel& chip) {
  chip.setup(0x15024356, pcie_cores(), 0x180000, 0xc0000);
  chip.cr4_cap = 4 | 2 << 4;
  chip.cr4_bankinfo = 15;
}

static void test_pcie_start() {
  printf("pcie start\n");
  ChipModel chip;
  setup_4356(chip);
  FakePci pci;
  FakeBar0 bar0(chip, pci);
  WifiChipBus bus{};
  bus.registers = &bar0;
  bus.pci = &pci;
  bus.pci_device.address = {4, 0, 0};
  bus.pci_device.vendor_id = 0x14e4;
  bus.pci_device.device_id = 0x43ec;
  pci.config[0xbc] = 0x13;    // link control with ASPM bits

  MapFirmware fw;
  const Bytes image = make_image(96 * 1024, "7.35.180.197");
  fw.files["brcm/brcmfmac4356-pcie.bin"] = image;
  fw.files["brcm/brcmfmac4356-pcie.txt"] = bytes_of_text("# board\nboardrev=0x1101\npcie/1/4/macaddr=00:90:4c:00:00:01\npcie/1/4/sromrev=11\npcie/1/5/sromrev=99\n");
  fw.files["brcm/brcmfmac4356-pcie.gpd-win-pocket.txt"] = bytes_of_text("boardtype=0x1234\nsromrev=11\n");
  fw.files["brcm/brcmfmac4356-pcie.clm_blob"] = Bytes(5750, 0x42);

  const WifiAdapterModel* model = wifi_find_pci_adapter(0x14e4, 0x43ec);
  BrcmfChip brcmf(*model, bus, g_alloc);
  CHECK(brcmf.add_board_type("gpd-win-pocket"));
  const WifiChipResult r = brcmf.start(fw);
  if (r != WifiChipResult::Unsupported) printf("  start: %s (%s)\n", wifi_chip_result_name(r), brcmf.message());
  CHECK(r == WifiChipResult::Unsupported && brcmf.state() == WifiChipState::FirmwareLoaded);
  CHECK(brcmf.identity().chip_id == 0x4356 && brcmf.identity().revision == 2 && strcmp(brcmf.chip_name(), "BCM4356/2") == 0);
  CHECK(brcmf.chip_id().core_count == 5 && brcmf.chip_id().package == 0);
  CHECK(brcmf.cores().count == 5);
  CHECK(brcmf.ram().base == 0x180000 && brcmf.ram().size == 0xc0000);
  CHECK(strcmp(brcmf.firmware_name(), "brcm/brcmfmac4356-pcie.bin") == 0);
  CHECK(strcmp(brcmf.nvram_name(), "brcm/brcmfmac4356-pcie.gpd-win-pocket.txt") == 0);
  CHECK(strcmp(brcmf.clm_name(), "brcm/brcmfmac4356-pcie.clm_blob") == 0 && brcmf.clm_bytes() == 5750);
  CHECK(brcmf.txcap_name()[0] == 0);
  CHECK(strcmp(brcmf.identity().firmware_version, "7.35.180.197") == 0);
  // the board file was asked for first, then the plain one for the image
  CHECK(fw.asked.size() >= 2 && fw.asked[0] == "brcm/brcmfmac4356-pcie.gpd-win-pocket.bin" && fw.asked[1] == "brcm/brcmfmac4356-pcie.bin");
  // the download
  CHECK(chip.start_count == 1 && chip.reset_vector_seen == 0x12345678);
  CHECK(chip.image_at_start.size() == 64 && memcmp(chip.image_at_start.data(), image.data(), 64) == 0);
  CHECK(memcmp(chip.ram.data() + 64, image.data() + 64, 0x8000) == 0);
  Bytes expected_nvram;
  CHECK(strip("boardtype=0x1234\nsromrev=11\n", expected_nvram) == BrcmfNvramResult::Ok);
  CHECK(brcmf.nvram_bytes() == expected_nvram.size());
  CHECK(memcmp(chip.ram.data() + chip.ramsize - expected_nvram.size(), expected_nvram.data(), expected_nvram.size() - 4) == 0);
  CHECK(chip.watchdog_writes == 1);
  CHECK(pci.config[0xbc] == 0x13);     // ASPM restored
  CHECK(bar0.out_of_window == 0 && pci.window_writes > 2);
  // the ARM is running, the D11 core disabled for the firmware to enable
  CHECK(chip.regs[0x18102000 + brcmf_bp::kWrapIoctl] == brcmf_bp::kIoctlClk && chip.regs[0x18102000 + brcmf_bp::kWrapResetCtl] == 0);
  CHECK(chip.regs[0x18101000 + brcmf_bp::kWrapResetCtl] == 1);
  // the shared structure
  CHECK(brcmf.shared_address() == 0x180000 + 0xc0000 - 0x3000);
  CHECK(brcmf.pcie_shared().version == 6 && brcmf.pcie_shared().dma_index_size == 4 && brcmf.pcie_shared().max_rxbufpost == 255 &&
        brcmf.pcie_shared().rx_dataoffset == 8 && brcmf.pcie_shared().ring_info_addr == brcmf.shared_address() + 0x80);
  CHECK(brcmf.mac_address() == MacAddress{});

  // stop halts the ARM again; a second start works the same
  brcmf.stop();
  CHECK(brcmf.state() == WifiChipState::Off && chip.regs[0x18102000 + brcmf_bp::kWrapIoctl] & brcmf_bp::kArmCr4IoctlCpuHalt);
  fw.files.erase("brcm/brcmfmac4356-pcie.gpd-win-pocket.txt");
  CHECK(brcmf.start(fw) == WifiChipResult::Unsupported && chip.start_count == 2);
  // the multi-device file without a board file: this is bus 4, domain 0 + 1
  CHECK(strcmp(brcmf.nvram_name(), "brcm/brcmfmac4356-pcie.txt") == 0);
  CHECK(strip("macaddr=00:90:4c:00:00:01\nsromrev=11\n", expected_nvram) == BrcmfNvramResult::Ok && brcmf.nvram_bytes() == expected_nvram.size());
  CHECK(memcmp(chip.ram.data() + chip.ramsize - expected_nvram.size(), expected_nvram.data(), expected_nvram.size() - 4) == 0);

  // without any NVRAM (optional on PCIe) the firmware still starts
  fw.files.erase("brcm/brcmfmac4356-pcie.txt");
  CHECK(brcmf.start(fw) == WifiChipResult::Unsupported && brcmf.nvram_bytes() == 0 && brcmf.nvram_name()[0] == 0);

  // failures
  chip.firmware_starts = false;
  CHECK(brcmf.start(fw) == WifiChipResult::FirmwareRejected && brcmf.state() == WifiChipState::Failed);
  chip.firmware_starts = true;
  chip.shared_version = 4;
  CHECK(brcmf.start(fw) == WifiChipResult::FirmwareRejected);
  chip.shared_version = 7;
  CHECK(brcmf.start(fw) == WifiChipResult::Unsupported && brcmf.pcie_shared().version == 7);
  fw.files["brcm/brcmfmac4356-pcie.bin"] = make_image(0xc0000, "1.2.3");    // as large as RAM: no room for the shared word
  CHECK(brcmf.start(fw) == WifiChipResult::FirmwareInvalid);
  fw.files.erase("brcm/brcmfmac4356-pcie.bin");
  CHECK(brcmf.start(fw) == WifiChipResult::FirmwareMissing && brcmf.state() == WifiChipState::Failed &&
        strcmp(brcmf.firmware_name(), "brcm/brcmfmac4356-pcie.bin") == 0);
  fw.files["brcm/brcmfmac4356-pcie.bin"] = image;
  chip.chip_id_value = 0xffffffff;
  CHECK(brcmf.start(fw) == WifiChipResult::NoResponse);
  chip.chip_id_value = 0x15024399;    // an unknown AXI chip: identified, then no RAM base
  CHECK(brcmf.start(fw) == WifiChipResult::UnknownRevision);
  chip.chip_id_value = 0x05024356 & 0x0fffffff;   // Sonics backplane
  CHECK(brcmf.start(fw) == WifiChipResult::UnknownRevision);
  chip.chip_id_value = 0x15024356;
  const std::vector<uint32_t> good_rom = chip.erom;
  chip.erom.assign(1024, 0x11111111);    // garbage ROM
  CHECK(brcmf.start(fw) == WifiChipResult::NoResponse);
  chip.erom = good_rom;
  chip.erom.pop_back();    // no end marker in the window: the rest of the window reads 0
  CHECK(brcmf.start(fw) == WifiChipResult::NoResponse);
  chip.erom = good_rom;
  CHECK(brcmf.start(fw) == WifiChipResult::Unsupported);

  // no registers
  WifiChipBus empty{};
  BrcmfChip lonely(*model, empty, g_alloc);
  CHECK(lonely.start(fw) == WifiChipResult::NoRegisters && lonely.state() == WifiChipState::Failed);
  lonely.stop();
  CHECK(lonely.state() == WifiChipState::Off);
}

static void test_pcie_seed() {
  printf("pcie random seed (4378)\n");
  ChipModel chip;
  chip.setup(0x15054378, pcie_cores(14), 0x352000, 15 * 256 * 1024);
  chip.cr4_cap = 8 | 7 << 4;
  chip.cr4_bankinfo = 31;    // 15 banks of 256 KiB
  FakePci pci;
  FakeBar0 bar0(chip, pci);
  WifiChipBus bus{};
  bus.registers = &bar0;
  bus.pci = &pci;
  MapFirmware fw;
  fw.files["brcm/brcmfmac4378b3-pcie.bin"] = make_image(200 * 1024, "18.20.383.14");
  fw.files["brcm/brcmfmac4378b3-pcie.txt"] = bytes_of_text("boardrev=1\n");
  fw.files["brcm/brcmfmac4378b3-pcie.txcap_blob"] = Bytes(100, 1);
  chip.banner = "";
  BrcmfChip brcmf(*wifi_find_pci_adapter(0x14e4, 0x4425), bus, g_alloc);
  const WifiChipResult r = brcmf.start(fw);
  if (r != WifiChipResult::Unsupported) printf("  start: %s (%s)\n", wifi_chip_result_name(r), brcmf.message());
  CHECK(r == WifiChipResult::Unsupported);
  CHECK(strcmp(brcmf.firmware_name(), "brcm/brcmfmac4378b3-pcie.bin") == 0 && strcmp(brcmf.txcap_name(), "brcm/brcmfmac4378b3-pcie.txcap_blob") == 0);
  CHECK(strcmp(brcmf.identity().firmware_version, "18.20.383.14") == 0);    // no console banner: the image's version
  CHECK(chip.watchdog_writes == 1);
  const uint32_t nvram = chip.ramsize - static_cast<uint32_t>(brcmf.nvram_bytes());
  ConstByteView ram(chip.ram.data(), chip.ram.size());
  CHECK(le32_at(ram, nvram - 8) == 0x100 && le32_at(ram, nvram - 4) == 0xfeedc0de);
  // the seed is not all zeros
  bool nonzero = false;
  for (uint32_t i = nvram - 8 - 256; i < nvram - 8; ++i) nonzero |= chip.ram[i] != 0;
  CHECK(nonzero);
  // seed switched off: the footer is not written
  chip.ram.assign(chip.ramsize, 0);
  ram = ConstByteView(chip.ram.data(), chip.ram.size());
  brcmf.set_random_seed(false);
  CHECK(brcmf.start(fw) == WifiChipResult::Unsupported && le32_at(ram, nvram - 4) == 0);
  // the layout checks
  BrcmfRamInfo info{0x180000, 0x1000, 0};
  BrcmfPcieLayout layout;
  CHECK(brcmf_pcie_layout(info, 0x800, 0x100, true, &layout) && layout.nvram == 0x180f00 && layout.seed_footer == 0x180ef8 && layout.seed == 0x180df8);
  CHECK(!brcmf_pcie_layout(info, 0xe00, 0x100, true, &layout));
  CHECK(brcmf_pcie_layout(info, 0xffc, 0, false, &layout) && layout.shared_word == 0x180ffc);
  CHECK(!brcmf_pcie_layout(info, 0xffd, 0, false, &layout));
  CHECK(!brcmf_pcie_layout(info, 0x10, 0x102, false, &layout));
}

static void test_shared_parsers() {
  printf("shared parsers\n");
  BrcmfRamInfo info{0x180000, 0x10000, 0};
  uint8_t shared[brcmf_pcie::kSharedBytes] = {};
  BrcmfPcieShared out;
  shared[0] = 5;
  shared[2] = 0x11;    // DMA index, 2-byte indices
  shared[34] = 0x40;
  CHECK(brcmf_pcie_parse_shared(ConstByteView(shared), 0x180100, info, &out) == BrcmfSharedResult::Ok && out.version == 5 && out.dma_index_size == 2 &&
        out.max_rxbufpost == 0x40);
  shared[0] = 8;
  CHECK(brcmf_pcie_parse_shared(ConstByteView(shared), 0x180100, info, &out) == BrcmfSharedResult::UnsupportedVersion);
  shared[0] = 7;
  CHECK(brcmf_pcie_parse_shared(ConstByteView(shared), 0x190000, info, &out) == BrcmfSharedResult::BadAddress);
  CHECK(brcmf_pcie_parse_shared(ConstByteView(shared).first(40), 0x180100, info, &out) == BrcmfSharedResult::Truncated);
  for (size_t cut = 0; cut <= sizeof shared; ++cut) (void)brcmf_pcie_parse_shared(ConstByteView(shared).first(cut), 0x180100, info, &out);
  uint8_t console[20] = {};
  BrcmfConsole c;
  console[8] = 0x00; console[9] = 0x10; console[10] = 0x18;    // buffer 0x181000
  console[12] = 0x00; console[13] = 0x04;                      // 1 KiB
  CHECK(brcmf_parse_console(ConstByteView(console), info, &c) && c.buffer == 0x181000 && c.size == 0x400);
  console[13] = 0xff;    // past RAM
  CHECK(!brcmf_parse_console(ConstByteView(console), info, &c));
  CHECK(!brcmf_parse_console(ConstByteView(console).first(12), info, &c));
  // sdpcm_shared
  CHECK(!brcmf_sdio_shared_pointer_valid(0));
  CHECK(!brcmf_sdio_shared_pointer_valid(0xffe5001a));     // an NVRAM token
  CHECK(brcmf_sdio_shared_pointer_valid(0x0019f000));
  uint8_t sdpcm[brcmf_sdio::kSharedBytes] = {};
  BrcmfSdioShared s;
  sdpcm[0] = 3;
  sdpcm[20] = 0x34; sdpcm[21] = 0x12;
  CHECK(brcmf_sdio_parse_shared(ConstByteView(sdpcm), 0x1000, &s) && s.console_addr == 0x1234);
  sdpcm[0] = 4;
  CHECK(!brcmf_sdio_parse_shared(ConstByteView(sdpcm), 0x1000, &s));
  CHECK(!brcmf_sdio_parse_shared(ConstByteView(sdpcm).first(20), 0x1000, &s));
}

// ---- SDIO ---------------------------------------------------------------------------------------------------------------------------------------------------------------------

static std::vector<CoreSpec> sdio_cr4_cores() {
  return {{0x800, 49, 0x18000000, 0x18100000, false},
          {0x829, 21, 0x18002000, 0x18102000, false},
          {0x812, 30, 0x18001000, 0x18101000, false},
          {0x83e, 2, 0x18003000, 0x18103000, true}};
}

static void test_sdio_43455() {
  printf("sdio start (43455, Raspberry Pi 4)\n");
  ChipModel chip;
  chip.pcie = false;
  chip.setup(0x15064345, sdio_cr4_cores(), 0x198000, 0xa0000);
  chip.cr4_cap = 3 | 2 << 4;     // 5 banks of 128 KiB = 640 KiB
  chip.cr4_bankinfo = 15;
  chip.banner = "\n000000.012 RTE (SDIO-CDC) 7.45.286 (be70ab3 CY) on BCM4345 r6 @ 37.4/81.6/81.6MHz\n";
  FakeSdio sdio(chip);
  WifiChipBus bus{};
  bus.sdio = &sdio;
  MapFirmware fw;
  Bytes image = make_image(400 * 1024, "7.45.286");
  Bytes nvram_text = bytes_of_text("# Raspberry Pi 4\naa2g=1\nboardrev=0x1304\nboardtype=0x6e4\nmacaddr=00:90:4c:c5:12:38\n");
  const char* dir = getenv("BRCMF_FIRMWARE_DIR");
  bool real = false;
  if (dir) {
    Bytes real_image, real_text;
    if (read_file(std::string(dir) + "/brcmfmac43455-sdio.bin", real_image) && read_file(std::string(dir) + "/brcmfmac43455-sdio.raspberrypi,4-model-b.txt", real_text)) {
      image = real_image;
      nvram_text = real_text;
      real = true;
      chip.banner = "";
      printf("  using the real brcmfmac43455-sdio.bin (%zu bytes) and the Raspberry Pi 4 NVRAM\n", image.size());
    }
  }
  fw.files["brcm/brcmfmac43455-sdio.bin"] = image;
  fw.files["brcm/brcmfmac43455-sdio.raspberrypi,4-model-b.txt"] = nvram_text;
  fw.files["brcm/brcmfmac43455-sdio.clm_blob"] = Bytes(2000, 7);
  const WifiAdapterModel* model = wifi_find_adapter(WifiBus::Sdio, 0x02d0, 0xa9bf);
  auto made = make_wifi_chip(*model, bus, g_alloc);
  CHECK(made);
  BrcmfChip brcmf(*model, bus, g_alloc);
  BrcmfFileName board;
  CHECK(brcmf_board_type_from_compatible("raspberrypi,4-model-b", board) && brcmf.add_board_type(board));
  const WifiChipResult r = brcmf.start(fw);
  if (r != WifiChipResult::Unsupported) printf("  start: %s (%s)\n", wifi_chip_result_name(r), brcmf.message());
  CHECK(r == WifiChipResult::Unsupported && brcmf.state() == WifiChipState::FirmwareLoaded);
  CHECK(brcmf.identity().chip_id == 0x4345 && brcmf.identity().revision == 6);
  CHECK(strcmp(brcmf.firmware_name(), "brcm/brcmfmac43455-sdio.bin") == 0);
  CHECK(strcmp(brcmf.nvram_name(), "brcm/brcmfmac43455-sdio.raspberrypi,4-model-b.txt") == 0);
  CHECK(brcmf.ram().base == 0x198000 && brcmf.ram().size == 0xa0000);
  CHECK(strcmp(brcmf.identity().firmware_version, "7.45.286") == 0);
  if (real) printf("  firmware version from the real image: %s, NVRAM %llu bytes stripped\n", brcmf.identity().firmware_version,
                   static_cast<unsigned long long>(brcmf.nvram_bytes()));
  CHECK(chip.reset_vector_seen == le32_at(ConstByteView(image.data(), image.size()), 0));
  CHECK(memcmp(chip.ram.data(), image.data(), image.size()) == 0);
  Bytes expected;
  CHECK(strip(std::string(nvram_text.begin(), nvram_text.end()), expected) == BrcmfNvramResult::Ok && brcmf.nvram_bytes() == expected.size());
  CHECK(memcmp(chip.ram.data() + chip.ramsize - expected.size(), expected.data(), expected.size() - 4) == 0);
  CHECK(chip.watchdog_writes == 0 && sdio.ht_requests >= 1);
  CHECK(brcmf.sdio_shared().console_addr == brcmf.shared_address() + 0x100);
  CHECK(chip.regs[0x18002000 + brcmf_sdio::kCoreIntStatus] == 0xffffffff);    // interrupts cleared before the ARM ran

  // NVRAM is required on SDIO
  fw.files.erase("brcm/brcmfmac43455-sdio.raspberrypi,4-model-b.txt");
  CHECK(brcmf.start(fw) == WifiChipResult::FirmwareMissing);
  // no ALP clock
  sdio.alp_works = false;
  CHECK(brcmf.start(fw) == WifiChipResult::NoResponse);
  sdio.alp_works = true;
  // no SDIO bus
  WifiChipBus empty{};
  BrcmfChip lonely(*model, empty, g_alloc);
  CHECK(lonely.start(fw) == WifiChipResult::NoRegisters);
}

static void test_sdio_43430() {
  printf("sdio start (43430, CM3 + SOCRAM)\n");
  ChipModel chip;
  chip.pcie = false;
  // SOCRAM rev 18 (bank registers), 8 banks of 64 KiB... 512 KiB
  chip.setup(0x1401a9a6, {{0x800, 49, 0x18000000, 0x18100000, false},
                          {0x829, 21, 0x18002000, 0x18102000, false},
                          {0x812, 30, 0x18001000, 0x18101000, false},
                          {0x82a, 9, 0x18003000, 0x18103000, true},
                          {0x80e, 18, 0x18004000, 0x18104000, false}},
             0, 0x80000);
  chip.socram_coreinfo = 8 << 4;
  chip.socram_bankinfo = 7;    // (7 + 1) * 8 KiB = 64 KiB
  chip.banner = "\nRTE (SDIO-CDC) 7.45.98.118 (7d96287 CY) on BCM43430 r1 @ 37.4/81.6/81.6MHz\n";
  FakeSdio sdio(chip);
  WifiChipBus bus{};
  bus.sdio = &sdio;
  MapFirmware fw;
  const Bytes image = make_image(300 * 1024, "7.45.98.118");
  fw.files["brcm/brcmfmac43430-sdio.bin"] = image;
  fw.files["brcm/brcmfmac43430-sdio.txt"] = bytes_of_text("boardrev=0x1202\nmanfid=0x2d0\n");
  BrcmfChip brcmf(*wifi_find_adapter(WifiBus::Sdio, 0x02d0, 0xa9a6), bus, g_alloc);
  const WifiChipResult r = brcmf.start(fw);
  if (r != WifiChipResult::Unsupported) printf("  start: %s (%s)\n", wifi_chip_result_name(r), brcmf.message());
  CHECK(r == WifiChipResult::Unsupported);
  CHECK(brcmf.ram().base == 0 && brcmf.ram().size == 0x80000 && brcmf.ram().save_restore_size == 64 * 1024);
  CHECK(strcmp(brcmf.identity().firmware_version, "7.45.98.118") == 0);
  CHECK(memcmp(chip.ram.data() + 4, image.data() + 4, image.size() - 4) == 0);
  // the CM3 is started from address 0 (no vector write), bank 3 remap off
  bool remap = false;
  for (size_t i = 0; i + 1 < chip.socram_bank_writes.size(); ++i)
    remap |= chip.socram_bank_writes[i] == std::make_pair(brcmf_bp::kSocramBankIdx, 3u) && chip.socram_bank_writes[i + 1] == std::make_pair(brcmf_bp::kSocramBankPda, 0u);
  CHECK(remap);
  CHECK(chip.regs[0x18103000 + brcmf_bp::kWrapIoctl] == brcmf_bp::kIoctlClk);
  // rev 0 wants another file
  chip.chip_id_value = 0x1400a9a6;
  CHECK(brcmf.start(fw) == WifiChipResult::FirmwareMissing && strcmp(brcmf.firmware_name(), "brcm/brcmfmac43430a0-sdio.bin") == 0);
}

// Optional: are the names the table and the fwnames tables produce present in a linux-firmware copy?
static void test_real_names() {
  const char* dir = getenv("BRCMF_LINUX_FIRMWARE");
  if (!dir) return;
  printf("linux-firmware names (%s)\n", dir);
  int present = 0, missing = 0;
  for (const WifiAdapterModel& m : broadcom_wifi_models()) {
    if (!m.firmware[0]) continue;
    const std::string path = std::string(dir) + "/" + (m.firmware + 5) + ".zst";    // without "brcm/"
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
      fclose(f);
      ++present;
    } else {
      ++missing;
      printf("  not in linux-firmware: %s\n", m.firmware);
    }
  }
  printf("  %d present, %d missing\n", present, missing);
}

int main() {
  test_table();
  test_firmware_names();
  test_nvram();
  test_real_nvram();
  test_erom();
  test_pcie_start();
  test_pcie_seed();
  test_shared_parsers();
  test_sdio_43455();
  test_sdio_43430();
  test_real_names();
  if (g_failed) {
    printf("%d check(s) FAILED\n", g_failed);
    return 1;
  }
  printf("all Broadcom Wi-Fi tests passed\n");
  return 0;
}
