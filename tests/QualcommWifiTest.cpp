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

// The Qualcomm Atheros chips (src/Hardware/WirelessInterface/Qualcomm): the adapter table, the ath10k firmware-N.bin, ath11k/ath12k firmware-2.bin and board-2.bin
// parsers on synthetic files (with every truncation and every single bit flipped), the board names and their lookup, the BMI message encoding, the ath10k bring-up
// (wake, cold + warm reset, SOC_CHIP_ID, BMI over copy engines 0/1, board and firmware download) against a model of a QCA6174 that runs a BMI target, and the ath11k/
// ath12k MHI boot (BHI download of the SBL) against a model of the MHI registers.
//
// Real linux-firmware files (decompressed) are parsed when the environment names them; the default run needs none:
//   ATH10K_FIRMWARE_FILE   an ath10k firmware-N.bin              ATH10K_BOARD_FILE   an ath10k board-2.bin (QCA6174 hw3.0's also starts the chip model with it)
//   ATH11K_BOARD_FILE      an ath11k board-2.bin                 ATH12K_BOARD_FILE   an ath12k board-2.bin
//   ATH12K_FIRMWARE_FILE   an ath12k firmware-2.bin (QCN9274)    ATH_AMSS_FILE       an ath11k/ath12k amss.bin
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Hardware/WirelessInterface/WifiAdapterCatalog.hpp"
#include "Hardware/WirelessInterface/WifiChipFactory.hpp"
#include "Hardware/WirelessInterface/Qualcomm/QualcommWifi.hpp"

namespace hardware {
void delay_us(uint64_t) {}
}  // namespace hardware

using namespace hardware;
using namespace hardware::ath;

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
static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
static void pad4(Bytes& b) { while (b.size() % 4) b.push_back(0); }
static void magic(Bytes& b, const char* m) { b.insert(b.end(), m, m + strlen(m) + 1); pad4(b); }
static void ie(Bytes& b, uint32_t id, const Bytes& data) { put32(b, id); put32(b, static_cast<uint32_t>(data.size())); b.insert(b.end(), data.begin(), data.end()); pad4(b); }
static Bytes text(const char* s) { return Bytes(s, s + strlen(s)); }
static Bytes fill(size_t n, uint8_t seed) { Bytes b(n); for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(seed + i * 7); return b; }
static Bytes u32le(uint32_t v) { Bytes b; put32(b, v); return b; }
static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }
static std::string str(StringView s) { return std::string(s.data(), s.size()); }

static Bytes read_file(const char* path) {
  Bytes out;
  FILE* f = fopen(path, "rb");
  if (!f) return out;
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) out.insert(out.end(), chunk, chunk + n);
  fclose(f);
  return out;
}

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

// ---- the table -------------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_models() {
  const ArrayView<const WifiAdapterModel> models = qualcomm_wifi_models();
  CHECK(models.size() >= 30);
  std::set<std::string> keys;
  for (const WifiAdapterModel& m : models) {
    CHECK(m.vendor == WifiVendor::Qualcomm);
    CHECK(wifi_driver_family_vendor(m.family) == WifiVendor::Qualcomm);
    char key[32];
    snprintf(key, sizeof key, "%d:%04x:%04x", static_cast<int>(m.bus), m.vendor_id, m.device_id);
    CHECK(keys.insert(key).second);
    CHECK(m.chip && m.chip[0] && m.product && m.product[0] && m.firmware);
    CHECK(m.spatial_streams >= 1 && m.spatial_streams <= 4);
    if (m.family == WifiDriverFamily::Ath9k) CHECK(m.bus == WifiBus::Pci && m.firmware[0] == 0 && m.standard == WifiStandard::Wifi4);
    if (m.family == WifiDriverFamily::Ath9kHtc) CHECK(m.bus == WifiBus::Usb && strncmp(m.firmware, "ath9k_htc/htc_", 14) == 0);
    if (m.family == WifiDriverFamily::Ath10k) CHECK(strncmp(m.firmware, "ath10k/", 7) == 0 && m.standard == WifiStandard::Wifi5);
    if (m.family == WifiDriverFamily::Ath11k) CHECK(strncmp(m.firmware, "ath11k/", 7) == 0 && m.vendor_id == 0x17CB);
    if (m.family == WifiDriverFamily::Ath12k) CHECK(strncmp(m.firmware, "ath12k/", 7) == 0 && m.standard == WifiStandard::Wifi7);
    if (m.firmware[0]) CHECK(wifi_firmware_name_valid(StringView(m.firmware)));
  }
  const WifiAdapterModel* qca6174 = wifi_find_pci_adapter(0x168C, 0x003E);
  CHECK(qca6174 && strcmp(qca6174->chip, "QCA6174") == 0 && qca6174->family == WifiDriverFamily::Ath10k && qca6174->bluetooth && qca6174->spatial_streams == 2);
  CHECK(qca6174 && strcmp(qca6174->firmware, "ath10k/QCA6174/hw3.0/firmware-6.bin") == 0 && strstr(qca6174->product, "Killer Wireless-n/a/ac 1535"));
  const WifiAdapterModel* wcn6855 = wifi_find_pci_adapter(0x17CB, 0x1103);
  CHECK(wcn6855 && wcn6855->standard == WifiStandard::Wifi6E && strstr(wcn6855->product, "Killer Wi-Fi 6E AX1675") && strcmp(wcn6855->firmware, "ath11k/WCN6855/hw2.0/amss.bin") == 0);
  const WifiAdapterModel* wcn7850 = wifi_find_pci_adapter(0x17CB, 0x1107);
  CHECK(wcn7850 && wcn7850->family == WifiDriverFamily::Ath12k && strcmp(wcn7850->firmware, "ath12k/WCN7850/hw2.0/amss.bin") == 0);
  const WifiAdapterModel* ar9271 = wifi_find_usb_adapter(0x0CF3, 0x9271);
  CHECK(ar9271 && ar9271->family == WifiDriverFamily::Ath9kHtc && strcmp(ar9271->firmware, "ath9k_htc/htc_9271-1.4.0.fw") == 0);
  CHECK(wifi_find_usb_adapter(0x0CF3, 0x7015) && strcmp(wifi_find_usb_adapter(0x0CF3, 0x7015)->firmware, "ath9k_htc/htc_7010-1.4.0.fw") == 0);
  CHECK(wifi_find_pci_adapter(0x168C, 0x0036) && wifi_find_pci_adapter(0x168C, 0x0036)->family == WifiDriverFamily::Ath9k);
  CHECK(!wifi_find_pci_adapter(0x168C, 0x9999) && !wifi_find_usb_adapter(0x168C, 0x003E));
  for (uint16_t id : {0x003C, 0x003E, 0x0040, 0x0041, 0x0042, 0x0046, 0x0050, 0x0056}) CHECK(wifi_find_pci_adapter(0x168C, id));
  for (uint16_t id : {0x1101, 0x1103, 0x1104, 0x1107, 0x1109}) CHECK(wifi_find_pci_adapter(0x17CB, id));

  // the factory: chip drivers for ath10k/11k/12k, none for ath9k
  TestAllocator alloc;
  WifiChipBus bus;
  CHECK(!make_wifi_chip(*wifi_find_pci_adapter(0x168C, 0x002A), bus, alloc));
  CHECK(!make_wifi_chip(*ar9271, bus, alloc));
  UniquePointer<IWifiChip> chip = make_wifi_chip(*qca6174, bus, alloc);
  CHECK(chip && &chip->model() == qca6174 && chip->state() == WifiChipState::Off && strcmp(chip->firmware_name(), qca6174->firmware) == 0);
  MapFirmware none;
  if (chip) {
    CHECK(chip->start(none) == WifiChipResult::NoRegisters && chip->state() == WifiChipState::Failed);
    chip->stop();
    CHECK(chip->state() == WifiChipState::Off);
  }
  CHECK(make_wifi_chip(*wcn6855, bus, alloc) && make_wifi_chip(*wcn7850, bus, alloc));
  // ath10k chips without a register map here stop before touching the chip
  struct Dead final : INicRegisters {
    uint8_t read8(uint32_t) override { return 0xFF; }
    uint16_t read16(uint32_t) override { return 0xFFFF; }
    uint32_t read32(uint32_t) override { return 0xFFFFFFFFu; }
    void write8(uint32_t, uint8_t) override {}
    void write16(uint32_t, uint16_t) override {}
    void write32(uint32_t, uint32_t) override {}
  } dead;
  bus.registers = &dead;
  UniquePointer<IWifiChip> qca9984 = make_wifi_chip(*wifi_find_pci_adapter(0x168C, 0x0046), bus, alloc);
  CHECK(qca9984 && qca9984->start(none) == WifiChipResult::Unsupported && qca9984->state() == WifiChipState::Off);
  // a chip that fell off the bus
  CHECK(chip && make_wifi_chip(*qca6174, bus, alloc)->start(none) == WifiChipResult::NoResponse);
  CHECK(make_wifi_chip(*wcn6855, bus, alloc)->start(none) == WifiChipResult::NoResponse);

  // the hardware parameters
  CHECK(ath10k_chip_supported(kQca6174, 0x00000A00) && ath10k_chip_supported(kQca6174, 0x00000800) && !ath10k_chip_supported(kQca6174, 0x00000300));
  CHECK(ath10k_chip_supported(kQca9377, 0x00000000) && ath10k_chip_supported(kQca988x, 0x00000200) && !ath10k_chip_supported(kQca988x, 0x00000100));
  const Ath10kHwParams* hw32 = ath10k_find_hw_params(kQca6174Hw32Version, kQca6174);
  CHECK(hw32 && strcmp(hw32->dir, "ath10k/QCA6174/hw3.0") == 0 && hw32->board_size == 8192 && hw32->patch_load_addr == 0x1234 && hw32->otp_exe_param == 0);
  CHECK(ath10k_hw_params_from_chip_id(kQca6174, 0x00000A00) == hw32);
  CHECK(ath10k_hw_params_from_chip_id(kQca6174, 0x00000400) && strcmp(ath10k_hw_params_from_chip_id(kQca6174, 0x00000400)->dir, "ath10k/QCA6174/hw2.1") == 0);
  CHECK(ath10k_find_hw_params(kQca9377Hw11Version, kQca9377) && strcmp(ath10k_find_hw_params(kQca9377Hw11Version, kQca9377)->dir, "ath10k/QCA9377/hw1.0") == 0);
  CHECK(!ath10k_find_hw_params(kQca6174Hw32Version, kQca9377) && !ath10k_find_hw_params(0x12345678, kQca6174));
  CHECK(ath10k_regs_for_device(kQca9377) == ath10k_regs_for_device(kQca6174) && ath10k_regs_for_device(kQca6174)->ce_base(1) == 0x34800);
  CHECK(ath10k_regs_for_device(kQca988x)->ce_base(7) == 0x59000 && !ath10k_regs_for_device(kQca99x0));
  CHECK(strcmp(ath11k_firmware_dir(kWcn6855, 0x200), "ath11k/WCN6855/hw2.0") == 0 && strcmp(ath11k_firmware_dir(kWcn6855, 0x211), "ath11k/WCN6855/hw2.1") == 0);
  CHECK(!ath11k_firmware_dir(kWcn6855, 0x305) && !ath11k_firmware_dir(kWcn6855, 0x220) && strcmp(ath11k_firmware_dir(kQcn9074, 0), "ath11k/QCN9074/hw1.0") == 0);
  CHECK(strcmp(ath11k_firmware_dir(kWcn7850, 0x200), "ath12k/WCN7850/hw2.0") == 0 && strcmp(ath11k_firmware_dir(kQcn9274, 0x201), "ath12k/QCN9274/hw2.0") == 0);
  CHECK(ath11k_bdf_search(kWcn6855) == AthBdfSearch::BusAndBoard && ath11k_bdf_search(kQca6390) == AthBdfSearch::Default);
}

// ---- firmware containers ---------------------------------------------------------------------------------------------------------------------------------------------------

static Bytes make_ath10k_firmware(bool with_otp = true, uint32_t features = 0x10C0) {
  Bytes f;
  magic(f, "QCA-ATH10K");
  ie(f, 0, text("WLAN.RM.4.4.1-test"));
  ie(f, 1, u32le(0x6419B55D));
  if (with_otp) ie(f, 4, fill(37, 0x11));
  ie(f, 3, fill(1001, 0x22));
  ie(f, 2, Bytes{static_cast<uint8_t>(features), static_cast<uint8_t>(features >> 8)});
  ie(f, 5, u32le(4));
  ie(f, 6, u32le(3));
  ie(f, 99, fill(5, 0));     // unknown
  return f;
}

static void test_ath10k_firmware() {
  const Bytes f = make_ath10k_firmware();
  Ath10kFirmwareFile fw;
  CHECK(ath10k_parse_firmware(view(f), &fw) == AthParse::Ok);
  CHECK(fw.version == StringView("WLAN.RM.4.4.1-test") && fw.timestamp == 0x6419B55D);
  CHECK(fw.firmware.size() == 1001 && fw.firmware[0] == 0x22 && fw.otp.size() == 37 && fw.otp[36] == static_cast<uint8_t>(0x11 + 36 * 7));
  CHECK(fw.firmware.data() >= f.data() && fw.firmware.data() + fw.firmware.size() <= f.data() + f.size());
  CHECK(fw.has_feature(6) && fw.has_feature(7) && fw.has_feature(12) && !fw.has_feature(0) && !fw.has_feature(64));
  CHECK(fw.wmi_op == Ath10kWmiOp::Tlv && fw.htt_op == Ath10kHttOp::Tlv && fw.unknown_ies == 1 && fw.code_swap.empty());

  // without the WMI op IE: derived from the features
  Bytes old;
  magic(old, "QCA-ATH10K");
  ie(old, 3, fill(8, 1));
  ie(old, 2, Bytes{0x10});      // WMI_10_2
  CHECK(ath10k_parse_firmware(view(old), &fw) == AthParse::Ok && fw.wmi_op == Ath10kWmiOp::V10_2);
  Bytes no_image;
  magic(no_image, "QCA-ATH10K");
  ie(no_image, 0, text("1.0"));
  CHECK(ath10k_parse_firmware(view(no_image), &fw) == AthParse::NoImage);
  // a too long version is skipped (the driver's limit is 31 characters)
  Bytes long_version;
  magic(long_version, "QCA-ATH10K");
  ie(long_version, 0, text("0123456789012345678901234567890123456789"));
  ie(long_version, 3, fill(8, 1));
  CHECK(ath10k_parse_firmware(view(long_version), &fw) == AthParse::Ok && fw.version.empty());
  // magic
  Bytes bad = f;
  bad[3] = 'B';
  CHECK(ath10k_parse_firmware(view(bad), &fw) == AthParse::BadMagic);
  bad = f;
  bad[10] = '\n';   // no NUL after the magic
  CHECK(ath10k_parse_firmware(view(bad), &fw) == AthParse::BadMagic);
  CHECK(ath10k_parse_firmware(ConstByteView(), &fw) == AthParse::Truncated);
  // an IE longer than the rest
  bad = f;
  bad[12 + 4] = 0xFF;
  CHECK(ath10k_parse_firmware(view(bad), &fw) == AthParse::BadLength);

  // every truncation: never Ok with an image that runs past the end
  uint32_t ok = 0;
  for (size_t n = 0; n < f.size(); ++n) {
    Bytes t(f.begin(), f.begin() + n);
    const AthParse r = ath10k_parse_firmware(view(t), &fw);
    if (r == AthParse::Ok) {
      ++ok;
      CHECK(fw.firmware.data() + fw.firmware.size() <= t.data() + t.size());
    }
  }
  CHECK(ok > 0 && ok < f.size());
  // every bit flipped: no out-of-bounds access (sanitizers), views inside the file
  for (size_t i = 0; i < f.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes t = f;
      t[i] ^= static_cast<uint8_t>(1u << bit);
      if (ath10k_parse_firmware(view(t), &fw) == AthParse::Ok) {
        CHECK(fw.firmware.data() >= t.data() && fw.firmware.data() + fw.firmware.size() <= t.data() + t.size());
        CHECK(fw.otp.empty() || (fw.otp.data() >= t.data() && fw.otp.data() + fw.otp.size() <= t.data() + t.size()));
      }
    }
}

static void test_qcom_firmware() {
  Bytes f;
  magic(f, "QCOM-ATH12K-FW");
  CHECK(f.size() == 16);
  ie(f, 0, u32le(0x6938867D));
  Bytes elf = fill(300, 3);
  const uint8_t ident[6] = {0x7F, 'E', 'L', 'F', 1, 1};
  memcpy(elf.data(), ident, 6);
  ie(f, 3, elf);
  ie(f, 2, elf);
  ie(f, 4, fill(77, 9));
  ie(f, 1, Bytes{0x03});
  AthQcomFirmwareFile fw;
  CHECK(ath_parse_qcom_firmware(view(f), kAth12kFirmwareMagic, &fw) == AthParse::Ok);
  CHECK(fw.timestamp == 0x6938867D && fw.amss.size() == 300 && fw.m3.size() == 300 && fw.amss_dual_mac.size() == 77 && fw.features == 3);
  CHECK(ath_is_elf32(fw.amss) && !ath_is_elf32(fw.amss_dual_mac) && !ath_is_elf32(ConstByteView(ident)));
  CHECK(ath_parse_qcom_firmware(view(f), kAth11kFirmwareMagic, &fw) == AthParse::BadMagic);
  Bytes m3_only;
  magic(m3_only, "QCOM-ATH11K-FW");
  ie(m3_only, 3, elf);
  CHECK(ath_parse_qcom_firmware(view(m3_only), kAth11kFirmwareMagic, &fw) == AthParse::NoImage);
  for (size_t n = 0; n < f.size(); ++n) {
    Bytes t(f.begin(), f.begin() + n);
    if (ath_parse_qcom_firmware(view(t), kAth12kFirmwareMagic, &fw) == AthParse::Ok) CHECK(fw.amss.data() + fw.amss.size() <= t.data() + t.size());
  }
  for (size_t i = 0; i < f.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes t = f;
      t[i] ^= static_cast<uint8_t>(1u << bit);
      if (ath_parse_qcom_firmware(view(t), kAth12kFirmwareMagic, &fw) == AthParse::Ok)
        CHECK(fw.amss.data() >= t.data() && fw.amss.data() + fw.amss.size() <= t.data() + t.size());
    }
}

// ---- board files -----------------------------------------------------------------------------------------------------------------------------------------------------------

static Bytes board_entry(std::initializer_list<const char*> names, const Bytes& data) {
  Bytes e;
  for (const char* n : names) ie(e, 0, text(n));
  ie(e, 1, data);
  return e;
}

static const char* kSub1535 = "bus=pci,vendor=168c,device=003e,subsystem-vendor=1a56,subsystem-device=1535";

static Bytes make_ath10k_board() {
  Bytes f;
  magic(f, "QCA-ATH10K-BOARD");
  ie(f, 0, board_entry({"bus=pci,vendor=168c,device=003e,subsystem-vendor=168c,subsystem-device=3363"}, fill(64, 1)));
  ie(f, 0, board_entry({kSub1535, "bus=pci,vendor=168c,device=003e,subsystem-vendor=1a56,subsystem-device=1536"}, fill(8124, 2)));
  ie(f, 1, board_entry({kSub1535}, fill(16, 3)));                                                     // BOARD_EXT with the same name
  ie(f, 0, board_entry({"bus=pci,bmi-chip-id=0,bmi-board-id=1"}, fill(40, 4)));
  ie(f, 0, board_entry({"bus=pci,vendor=168c,device=003e,subsystem-vendor=1a56,subsystem-device=1535,variant=Acme"}, fill(24, 5)));
  return f;
}

struct Listed {
  std::vector<std::string> names;
  std::vector<uint32_t> types;
  size_t bytes = 0;
};
static bool list_entry(void* context, uint32_t type, StringView name, ConstByteView data) {
  Listed& l = *static_cast<Listed*>(context);
  l.names.push_back(str(name));
  l.types.push_back(type);
  l.bytes += data.size();
  return true;
}

static void test_board_names() {
  AthBoardIds ids;
  ids.vendor = 0x168C;
  ids.device = 0x003E;
  ids.subsystem_vendor = 0x1A56;
  ids.subsystem_device = 0x1535;
  AthBoardName name;
  CHECK(ath10k_board_name(ids, true, true, &name) && name == StringView(kSub1535));
  ids.variant = "Acme";
  CHECK(ath10k_board_name(ids, true, true, &name) && name == StringView("bus=pci,vendor=168c,device=003e,subsystem-vendor=1a56,subsystem-device=1535,variant=Acme"));
  CHECK(ath10k_board_name(ids, false, true, &name) && name == StringView(kSub1535));
  ids.bmi_ids_valid = true;
  ids.bmi_chip_id = 0;
  ids.bmi_board_id = 11;
  CHECK(ath10k_board_name(ids, false, true, &name) && name == StringView("bus=pci,bmi-chip-id=0,bmi-board-id=11"));
  CHECK(ath10k_board_name(ids, true, true, &name) && name == StringView("bus=pci,bmi-chip-id=0,bmi-board-id=11,variant=Acme"));
  AthBoardIds snoc;
  snoc.bus = "snoc";
  snoc.qmi_ids_valid = true;
  snoc.qmi_board_id = 0xFF;
  snoc.qmi_chip_id = 0x320;
  CHECK(ath10k_board_name(snoc, false, true, &name) && name == StringView("bus=snoc,qmi-board-id=ff,qmi-chip-id=320"));
  CHECK(ath10k_board_name(snoc, true, false, &name) && name == StringView("bus=snoc,qmi-board-id=ff"));

  AthBoardIds q;
  q.vendor = 0x17CB;
  q.device = 0x1103;
  q.subsystem_vendor = 0x17CB;
  q.subsystem_device = 0x3374;
  q.qmi_chip_id = 2;
  q.qmi_board_id = 262;
  CHECK(ath11k_board_name(q, AthBdfSearch::BusAndBoard, true, false, false, &name) &&
        name == StringView("bus=pci,vendor=17cb,device=1103,subsystem-vendor=17cb,subsystem-device=3374,qmi-chip-id=2,qmi-board-id=262"));
  CHECK(ath11k_board_name(q, AthBdfSearch::BusAndBoard, false, true, true, &name) && name == StringView("bus=pci"));
  CHECK(ath11k_board_name(q, AthBdfSearch::Default, true, false, false, &name) && name == StringView("bus=pci,qmi-chip-id=2,qmi-board-id=262"));
  CHECK(ath11k_board_name(q, AthBdfSearch::Default, false, false, true, &name) && name == StringView("bus=pci,qmi-chip-id=2,qmi-board-id=255"));
  q.variant = "Xiaomi_Pad_5Pro";
  CHECK(ath11k_board_name(q, AthBdfSearch::Default, true, false, false, &name) && name == StringView("bus=pci,qmi-chip-id=2,qmi-board-id=262,variant=Xiaomi_Pad_5Pro"));
  // a variant too long for the name is refused, not cut
  char huge[300];
  memset(huge, 'x', sizeof huge - 1);
  huge[sizeof huge - 1] = 0;
  q.variant = huge;
  CHECK(!ath11k_board_name(q, AthBdfSearch::BusAndBoard, true, false, false, &name));
}

static void test_board_file() {
  const Bytes f = make_ath10k_board();
  ConstByteView data;
  CHECK(ath_board_find(view(f), kAth10kBoardMagic, kAthBoardIeBoard, kSub1535, &data) == AthBoardFind::Found && data.size() == 8124 && data[0] == 2);
  CHECK(ath_board_find(view(f), kAth10kBoardMagic, kAthBoardIeBoard, "bus=pci,vendor=168c,device=003e,subsystem-vendor=1a56,subsystem-device=1536", &data) ==
            AthBoardFind::Found && data.size() == 8124);
  CHECK(ath_board_find(view(f), kAth10kBoardMagic, kAth10kBoardIeBoardExt, kSub1535, &data) == AthBoardFind::Found && data.size() == 16);
  CHECK(ath_board_find(view(f), kAth10kBoardMagic, kAthBoardIeBoard, "bus=pci,vendor=168c,device=003e,subsystem-vendor=1a56,subsystem-device=153", &data) ==
        AthBoardFind::NotFound);     // a prefix is not a match
  CHECK(ath_board_find(view(f), kAth11kBoardMagic, kAthBoardIeBoard, kSub1535, &data) == AthBoardFind::Invalid);
  Listed listed;
  CHECK(ath_board_for_each(view(f), kAth10kBoardMagic, &list_entry, &listed));
  CHECK(listed.names.size() == 6 && listed.names[1] == kSub1535 && listed.types[3] == 1 && listed.bytes == 64 + 8124 * 2 + 16 + 40 + 24);

  // selection with the fallback name: a variant without its own entry falls back to the plain name
  AthBoardIds ids;
  ids.vendor = 0x168C;
  ids.device = 0x003E;
  ids.subsystem_vendor = 0x1A56;
  ids.subsystem_device = 0x1535;
  ids.variant = "Other";
  AthBoardName tried;
  CHECK(ath_select_board(view(f), AthFamily::Ath10k, AthBdfSearch::Default, kAthBoardIeBoard, ids, &data, &tried) == AthBoardFind::Found && data.size() == 8124 &&
        tried == StringView(kSub1535));
  ids.variant = "Acme";
  CHECK(ath_select_board(view(f), AthFamily::Ath10k, AthBdfSearch::Default, kAthBoardIeBoard, ids, &data, &tried) == AthBoardFind::Found && data.size() == 24);
  ids.subsystem_device = 0x9999;
  CHECK(ath_select_board(view(f), AthFamily::Ath10k, AthBdfSearch::Default, kAthBoardIeBoard, ids, &data, &tried) == AthBoardFind::NotFound);

  // ath11k: board by QMI ids with the default board id as fallback, REGDB by the bus only
  Bytes f11;
  magic(f11, "QCA-ATH11K-BOARD");
  ie(f11, 0, board_entry({"bus=pci,qmi-chip-id=0,qmi-board-id=4"}, fill(30, 6)));
  ie(f11, 0, board_entry({"bus=pci,qmi-chip-id=0,qmi-board-id=255"}, fill(31, 7)));
  ie(f11, 1, board_entry({"bus=pci,qmi-chip-id=0,qmi-board-id=255"}, fill(12, 8)));      // REGDB: in the default search the "bus type" name is the default board's
  AthBoardIds q;
  q.qmi_chip_id = 0;
  q.qmi_board_id = 4;
  CHECK(ath_select_board(view(f11), AthFamily::Ath11k, AthBdfSearch::Default, kAthBoardIeBoard, q, &data, &tried) == AthBoardFind::Found && data.size() == 30);
  q.qmi_board_id = 9;
  CHECK(ath_select_board(view(f11), AthFamily::Ath11k, AthBdfSearch::Default, kAthBoardIeBoard, q, &data, &tried) == AthBoardFind::Found && data.size() == 31 &&
        tried == StringView("bus=pci,qmi-chip-id=0,qmi-board-id=255"));
  CHECK(ath_select_board(view(f11), AthFamily::Ath11k, AthBdfSearch::Default, kAthBoardIeRegdb, q, &data, &tried) == AthBoardFind::Found && data.size() == 12);
  CHECK(ath_select_board(view(f11), AthFamily::Ath12k, AthBdfSearch::Default, kAthBoardIeBoard, q, &data, &tried) == AthBoardFind::Invalid);

  // truncations and bit flips: no out-of-bounds access, a found entry lies inside the file
  for (size_t n = 0; n < f.size(); n += (n < 200 || n > f.size() - 200) ? 1 : 97) {
    Bytes t(f.begin(), f.begin() + n);
    const AthBoardFind r = ath_board_find(view(t), kAth10kBoardMagic, kAthBoardIeBoard, kSub1535, &data);
    if (r == AthBoardFind::Found) CHECK(data.data() + data.size() <= t.data() + t.size());
    Listed l;
    (void)ath_board_for_each(view(t), kAth10kBoardMagic, &list_entry, &l);
  }
  for (size_t i = 0; i < f.size(); i += (i < 400 || i > f.size() - 400) ? 1 : 61)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes t = f;
      t[i] ^= static_cast<uint8_t>(1u << bit);
      if (ath_board_find(view(t), kAth10kBoardMagic, kAthBoardIeBoard, kSub1535, &data) == AthBoardFind::Found)
        CHECK(data.data() >= t.data() && data.data() + data.size() <= t.data() + t.size());
      Listed l;
      (void)ath_board_for_each(view(t), kAth10kBoardMagic, &list_entry, &l);
      (void)ath_select_board(view(t), AthFamily::Ath10k, AthBdfSearch::Default, kAthBoardIeBoard, ids, &data, nullptr);
    }
}

// ---- BMI ------------------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_bmi_encoding() {
  uint8_t buf[kBmiMaxCmdSize];
  ByteView out(buf);
  auto bytes = [&](uint32_t n) { return Bytes(buf, buf + n); };
  CHECK(bmi_encode_get_target_info(out) == 4 && bytes(4) == u32le(8));
  CHECK(bmi_encode_done(out) == 4 && bytes(4) == u32le(1));
  Bytes expect = u32le(4);
  put32(expect, 0x1234);
  put32(expect, 0x10);
  CHECK(bmi_encode_execute(out, 0x1234, 0x10) == 12 && bytes(12) == expect);
  expect = u32le(2);
  put32(expect, 0x400854);
  put32(expect, 4);
  CHECK(bmi_encode_read_memory(out, 0x400854, 4) == 12 && bytes(12) == expect);
  CHECK(bmi_encode_read_memory(out, 0, 0) == 0 && bmi_encode_read_memory(out, 0, 257) == 0);
  const uint8_t three[3] = {0xAA, 0xBB, 0xCC};
  expect = u32le(3);
  put32(expect, 0x00428000);
  put32(expect, 4);       // rounded up as the driver does
  expect.insert(expect.end(), {0xAA, 0xBB, 0xCC, 0x00});
  CHECK(bmi_encode_write_memory(out, 0x00428000, ConstByteView(three)) == 16 && bytes(16) == expect);
  const Bytes chunk = fill(kBmiWriteChunk, 1);
  CHECK(bmi_encode_write_memory(out, 0, view(chunk)) == kBmiMaxDataSize);
  const Bytes too_big = fill(kBmiWriteChunk + 1, 1);
  CHECK(bmi_encode_write_memory(out, 0, view(too_big)) == 0);
  expect = u32le(13);
  put32(expect, 0x1234);
  CHECK(bmi_encode_lz_stream_start(out, 0x1234) == 8 && bytes(8) == expect);
  const Bytes lz = fill(kBmiLzChunk, 5);
  CHECK(bmi_encode_lz_data(out, view(lz)) == kBmiMaxDataSize && buf[0] == 14 && buf[4] == kBmiLzChunk && buf[8] == lz[0]);
  CHECK(bmi_encode_lz_data(out, ConstByteView(three)) == 0);
  expect = u32le(7);
  put32(expect, 0x80000);
  put32(expect, 0xDEADBEEF);
  CHECK(bmi_encode_write_soc_register(out, 0x80000, 0xDEADBEEF) == 12 && bytes(12) == expect);
  CHECK(bmi_encode_read_soc_register(out, 0x80000) == 8 && buf[0] == 6);
  CHECK(bmi_encode_execute(ByteView(buf, 11), 0, 0) == 0);     // too small

  BmiTargetInfo info;
  Bytes answer = u32le(12);
  put32(answer, kQca6174Hw32Version);
  put32(answer, 7);
  CHECK(bmi_decode_target_info(view(answer), &info) && info.version == kQca6174Hw32Version && info.type == 7);
  CHECK(!bmi_decode_target_info(ConstByteView(answer.data(), 11), &info));
  uint32_t word = 0;
  CHECK(bmi_decode_word(view(answer), &word) && word == 12 && !bmi_decode_word(ConstByteView(answer.data(), 3), &word));

  // the client over a recording transport: chunking and the fast download's padding and closing stream
  struct Recorder final : IAth10kBmiTransport {
    std::vector<Bytes> sent;
    uint32_t answer = 0;
    bool exchange(ConstByteView command, ByteView reply, uint32_t* received) override {
      sent.emplace_back(command.data(), command.data() + command.size());
      for (uint64_t i = 0; i < reply.size(); ++i) reply[i] = static_cast<uint8_t>(answer >> (8 * (i % 4)));
      *received = static_cast<uint32_t>(reply.size());
      return true;
    }
  } rec;
  Ath10kBmi bmi(rec);
  const Bytes image = fill(1001, 9);
  CHECK(bmi.fast_download(0x1234, view(image)) == BmiResult::Ok);
  // start, 1000 bytes in chunks of 248 (5 messages), the last byte padded to a word, the closing stream
  CHECK(rec.sent.size() == 1 + 5 + 1 + 1);
  CHECK(rec.sent[0] == Bytes({13, 0, 0, 0, 0x34, 0x12, 0, 0}) && rec.sent[7] == Bytes({13, 0, 0, 0, 0, 0, 0, 0}));
  CHECK(rec.sent[1].size() == 256 && rec.sent[5].size() == 8 + (1000 - 4 * 248) && rec.sent[6] == Bytes({14, 0, 0, 0, 4, 0, 0, 0, image[1000], 0, 0, 0}));
  rec.sent.clear();
  CHECK(bmi.write_memory(0x428000, view(fill(500, 1))) == BmiResult::Ok && rec.sent.size() == 3 && rec.sent[2].size() == 12 + 12);
  rec.answer = 0x00002400;     // board id 9 from the OTP
  uint32_t result = 0;
  CHECK(bmi.execute(0x1234, kBmiParamGetEepromBoardId, &result) == BmiResult::Ok && result == 0x2400);
  CHECK(bmi.done() == BmiResult::Ok && bmi.done_sent());
  CHECK(bmi.write32(0, 0) == BmiResult::NotAllowed && bmi.get_target_info(&info) == BmiResult::NotAllowed);
}

// ---- ath10k chip model ------------------------------------------------------------------------------------------------------------------------------------------------------

// A QCA6174 hw3.2 behind BAR 0: the PCIe local block (wake, global reset), the RTC SoC block (reset control, chip id), FW_INDICATOR, CORE_CTRL and copy engines 0
// and 1, which run a BMI target with its own memory.
struct Qca6174Model final : INicRegisters {
  const Ath10kRegs& r = *ath10k_regs_for_device(kQca6174);
  std::map<uint32_t, uint32_t> regs;
  std::map<uint32_t, uint8_t> memory;
  uint32_t chip_id = 0x00000A00;
  uint32_t target_version = kQca6174Hw32Version;
  uint32_t board_query_result = 0x0000FF00 | 0x01;    // status byte not 0: no board id in the OTP
  uint32_t otp_result = 0;
  bool dead = false, never_initialized = false, in_reset = false, cpu_woken = false, done = false;
  uint32_t cold_resets = 0, warm_cpu_resets = 0, executes = 0, messages = 0;
  struct Stream { uint32_t address; Bytes data; };
  std::vector<Stream> streams;
  uint32_t ce_srri[2] = {0, 0}, ce_drri[2] = {0, 0};

  Qca6174Model() { put_word(kHostInterestAddress + kHiBoardData, 0x00428000); }
  void put_word(uint32_t a, uint32_t v) { for (int i = 0; i < 4; ++i) memory[a + i] = static_cast<uint8_t>(v >> (8 * i)); }
  uint32_t word(uint32_t a) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t{memory[a + i]} << (8 * i); return v; }
  uint32_t ce(uint32_t n, uint32_t reg) const { return r.ce_base(n) + reg; }
  uint32_t get(uint32_t offset) { auto it = regs.find(offset); return it == regs.end() ? 0 : it->second; }

  uint8_t read8(uint32_t) override { return 0; }
  uint16_t read16(uint32_t) override { return 0; }
  void write8(uint32_t, uint8_t) override {}
  void write16(uint32_t, uint16_t) override {}

  void reset_engines() {
    for (uint32_t n = 0; n < 2; ++n) {
      for (uint32_t reg = 0; reg <= 0x50; reg += 4) regs.erase(ce(n, reg));
      ce_srri[n] = ce_drri[n] = 0;
    }
  }
  uint32_t read32(uint32_t offset) override {
    if (dead) return 0xFFFFFFFFu;
    const uint32_t local = r.pcie_local_base_address;
    if (offset == local + kRtcStateAddress) return get(local + kPcieSocWakeAddress) == kPcieSocWakeV ? r.rtc_state_val_on : 0;
    if (offset == r.rtc_soc_base + r.soc_chip_id_address) return chip_id;
    for (uint32_t n = 0; n < 2; ++n) {
      if (offset == ce(n, ce_reg::kCurrentSrri)) return ce_srri[n];
      if (offset == ce(n, ce_reg::kCurrentDrri)) return ce_drri[n];
    }
    return get(offset);
  }
  void write32(uint32_t offset, uint32_t value) override {
    if (dead) return;
    const uint32_t local = r.pcie_local_base_address;
    regs[offset] = value;
    if (offset == local + kSocGlobalResetAddress) {
      if (value & 1) {
        in_reset = true;
      } else if (in_reset) {
        in_reset = false;
        ++cold_resets;
        done = false;
        regs.erase(local + kPcieSocWakeAddress);
        reset_engines();
        if (!never_initialized) regs[r.fw_indicator_address] = kFwIndInitialized;
      }
    } else if (offset == r.rtc_soc_base + kSocResetControlAddress) {
      if (value & kSocResetControlCpuWarmRst) {
        ++warm_cpu_resets;
        done = false;
        regs[offset] = value & ~kSocResetControlCpuWarmRst;        // the bit clears itself as the CPU restarts
        if (!never_initialized) regs[r.fw_indicator_address] = kFwIndInitialized;
      }
      if (value & r.soc_reset_control_ce_rst_mask) reset_engines();
    } else if (offset == r.soc_core_base + kCoreCtrlAddress && (value & kCoreCtrlCpuIntr)) {
      cpu_woken = true;
    } else if (offset == ce(0, ce_reg::kSrWrIndex)) {
      run_engine(value);
    }
  }

  // The bus address in a descriptor is the host pointer (the test's DMA memory lies below 2 GiB).
  static uint8_t* host(uint32_t address) { return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address)); }

  void answer(const Bytes& reply) {
    const uint32_t entries = get(ce(1, ce_reg::kDrSize));
    const uint32_t base = get(ce(1, ce_reg::kDrBaseLow));
    if (!entries || !base || ce_drri[1] == get(ce(1, ce_reg::kDstWrIndex))) { printf("model: no receive buffer posted\n"); return; }
    uint8_t* desc = host(base) + ce_drri[1] * 8;
    uint32_t buffer = 0;
    memcpy(&buffer, desc, 4);
    memcpy(host(buffer), reply.data(), reply.size());
    const uint16_t n = static_cast<uint16_t>(reply.size());
    memcpy(desc + 4, &n, 2);
    ce_drri[1] = (ce_drri[1] + 1) & (entries - 1);
  }

  void run_engine(uint32_t write_index) {
    const uint32_t entries = get(ce(0, ce_reg::kSrSize));
    const uint32_t base = get(ce(0, ce_reg::kSrBaseLow));
    if (!entries || !base) return;
    while (ce_srri[0] != write_index) {
      const uint8_t* desc = host(base) + ce_srri[0] * 8;
      uint32_t address = 0;
      uint16_t length = 0, flags = 0;
      memcpy(&address, desc, 4);
      memcpy(&length, desc + 4, 2);
      memcpy(&flags, desc + 6, 2);
      CHECK(flags == 0xFFFC && length <= 256 && length >= 4);
      bmi(Bytes(host(address), host(address) + length));
      ce_srri[0] = (ce_srri[0] + 1) & (entries - 1);
    }
  }

  void bmi(const Bytes& m) {
    ++messages;
    auto w = [&](size_t i) { uint32_t v = 0; if (m.size() >= 4 * i + 4) memcpy(&v, m.data() + 4 * i, 4); return v; };
    CHECK(!done);
    switch (w(0)) {
      case 8: { Bytes a = u32le(12); put32(a, target_version); put32(a, 7); answer(a); break; }
      case 2: { Bytes a; for (uint32_t i = 0; i < w(2); ++i) a.push_back(memory[w(1) + i]); answer(a); break; }
      case 3: { CHECK(m.size() == 12 + w(2)); for (uint32_t i = 0; i < w(2); ++i) memory[w(1) + i] = m[12 + i]; break; }
      case 4: { ++executes; answer(u32le(w(2) == kBmiParamGetEepromBoardId ? board_query_result : otp_result)); break; }
      case 13: streams.push_back({w(1), {}}); break;
      case 14: CHECK(!streams.empty() && m.size() == 8 + w(1) && w(1) % 4 == 0); if (!streams.empty()) streams.back().data.insert(streams.back().data.end(), m.begin() + 8, m.end()); break;
      case 1: done = true; break;
      default: printf("model: BMI command %u\n", w(0)); CHECK(false);
    }
  }
};

struct ConfigSpace final : IPciBus {
  uint32_t subsystem = 0x15351A56;
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress, uint16_t offset) const override { return offset == 0x2C ? subsystem : 0xFFFFFFFFu; }
  void write32(PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
  uint32_t device_count() const override { return 0; }
  const char* name() const override { return "test"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

static void test_ath10k_chip() {
  TestAllocator alloc;
  const WifiAdapterModel& model = *wifi_find_pci_adapter(0x168C, 0x003E);
  const Bytes fw_file = make_ath10k_firmware();
  const Bytes board2 = make_ath10k_board();
  {
    Qca6174Model chip;
    ConfigSpace config;
    WifiChipBus bus;
    bus.registers = &chip;
    bus.pci = &config;
    Ath10kChip ath(model, bus, alloc);
    MapFirmware files;
    files.files["ath10k/QCA6174/hw3.0/firmware-6.bin"] = fw_file;
    files.files["ath10k/QCA6174/hw3.0/board-2.bin"] = board2;
    const WifiChipResult r = ath.start(files);
    CHECK(r == WifiChipResult::Unsupported && ath.state() == WifiChipState::FirmwareLoaded);
    if (ath.state() != WifiChipState::FirmwareLoaded) printf("  ath10k: %s (%s)\n", ath.message(), wifi_chip_result_name(r));
    CHECK(chip.cold_resets == 1 && chip.warm_cpu_resets == 2 && chip.cpu_woken && chip.done);
    CHECK(ath.identity().chip_id == 0x00000A00 && ath.identity().revision == 10 && strcmp(ath.identity().firmware_version, "WLAN.RM.4.4.1-test") == 0);
    CHECK(ath.hw_params() && strcmp(ath.hw_params()->name, "qca6174 hw3.2") == 0 && ath.target_version() == kQca6174Hw32Version);
    CHECK(strcmp(ath.firmware_name(), "ath10k/QCA6174/hw3.0/firmware-6.bin") == 0 && strcmp(ath.board_file(), "ath10k/QCA6174/hw3.0/board-2.bin") == 0);
    CHECK(ath.subsystem_vendor() == 0x1A56 && ath.subsystem_device() == 0x1535 && ath.board_name() == StringView(kSub1535) && !ath.otp_board_ids_valid());
    // the target's memory: configuration, board data at hi_board_data, the streams (OTP for the board id, OTP to run, the firmware, each closed by a stream to 0)
    CHECK(chip.word(kHostInterestAddress + kHiAppHostInterest) == 2 && chip.word(kHostInterestAddress + kHiOptionFlag) == 0x2208);
    CHECK(chip.word(kHostInterestAddress + kHiBoardDataInitialized) == 1);
    bool board_ok = true;
    for (uint32_t i = 0; i < 8124; ++i) board_ok = board_ok && chip.memory[0x00428000 + i] == static_cast<uint8_t>(2 + i * 7);
    CHECK(board_ok && chip.memory.count(0x00428000 + 8124) == 0);
    CHECK(chip.streams.size() == 6 && chip.executes == 2);
    if (chip.streams.size() == 6) {
      Bytes otp = fill(37, 0x11), image = fill(1001, 0x22);
      otp.resize(40, 0);
      image.resize(1004, 0);
      CHECK(chip.streams[0].address == 0x1234 && chip.streams[0].data == otp && chip.streams[1].address == 0 && chip.streams[1].data.empty());
      CHECK(chip.streams[2].data == otp && chip.streams[4].address == 0x1234 && chip.streams[4].data == image && chip.streams[5].address == 0);
    }
    CHECK(ath.bmi_messages() == chip.messages);
    ath.stop();
    CHECK(ath.state() == WifiChipState::Off && chip.warm_cpu_resets == 4);
  }
  // the board id from the OTP picks the bmi-chip-id/bmi-board-id entry
  {
    Qca6174Model chip;
    chip.board_query_result = (1u << kBmiBoardIdFromOtpShift);      // board 1, chip 0, status 0
    ConfigSpace config;
    WifiChipBus bus;
    bus.registers = &chip;
    bus.pci = &config;
    Ath10kChip ath(model, bus, alloc);
    MapFirmware files;
    files.files["ath10k/QCA6174/hw3.0/firmware-5.bin"] = fw_file;    // an older API is found too
    files.files["ath10k/QCA6174/hw3.0/board-2.bin"] = board2;
    CHECK(ath.start(files) == WifiChipResult::Unsupported && ath.state() == WifiChipState::FirmwareLoaded);
    CHECK(ath.otp_board_ids_valid() && ath.board_name() == StringView("bus=pci,bmi-chip-id=0,bmi-board-id=1") && chip.memory[0x00428000] == 4);
    CHECK(strcmp(ath.firmware_name(), "ath10k/QCA6174/hw3.0/firmware-5.bin") == 0);
  }
  // an unknown subsystem falls back to board.bin; no config space at all too
  {
    Qca6174Model chip;
    ConfigSpace config;
    config.subsystem = 0x00011234;
    WifiChipBus bus;
    bus.registers = &chip;
    bus.pci = &config;
    Ath10kChip ath(model, bus, alloc);
    MapFirmware files;
    files.files["ath10k/QCA6174/hw3.0/firmware-6.bin"] = fw_file;
    files.files["ath10k/QCA6174/hw3.0/board-2.bin"] = board2;
    files.files["ath10k/QCA6174/hw3.0/board.bin"] = fill(9000, 0x40);
    CHECK(ath.start(files) == WifiChipResult::Unsupported && strcmp(ath.board_file(), "ath10k/QCA6174/hw3.0/board.bin") == 0 && ath.board_name().empty());
    CHECK(chip.memory[0x00428000] == 0x40 && chip.memory.count(0x00428000 + 8192) == 0);     // cut to the board size
    files.files.erase("ath10k/QCA6174/hw3.0/board.bin");
    CHECK(ath.start(files) == WifiChipResult::FirmwareMissing && ath.state() == WifiChipState::Failed);
  }
  // failures
  {
    Qca6174Model chip;
    WifiChipBus bus;
    bus.registers = &chip;
    Ath10kChip ath(model, bus, alloc);
    MapFirmware files;
    CHECK(ath.start(files) == WifiChipResult::FirmwareMissing && ath.state() == WifiChipState::Failed && ath.hw_params());
    files.files["ath10k/QCA6174/hw3.0/firmware-6.bin"] = fill(100, 1);
    CHECK(ath.start(files) == WifiChipResult::FirmwareInvalid);
    files.files["ath10k/QCA6174/hw3.0/firmware-6.bin"] = make_ath10k_firmware(true, 0x1040);      // no IGNORE_OTP_RESULT
    files.files["ath10k/QCA6174/hw3.0/board.bin"] = fill(100, 1);
    chip.otp_result = 5;
    CHECK(ath.start(files) == WifiChipResult::FirmwareRejected && strstr(ath.message(), "OTP"));
    chip.otp_result = 0;
    CHECK(ath.start(files) == WifiChipResult::Unsupported);
    chip.target_version = 0x05099999;
    CHECK(ath.start(files) == WifiChipResult::UnknownRevision && ath.target_version() == 0x05099999);
    chip.target_version = kQca6174Hw32Version;
    chip.chip_id = 0x00000300;
    CHECK(ath.start(files) == WifiChipResult::UnknownRevision && ath.identity().revision == 3);
    chip.chip_id = 0x00000A00;
    chip.never_initialized = true;
    chip.regs.erase(chip.r.fw_indicator_address);
    CHECK(ath.start(files) == WifiChipResult::Timeout && ath.state() == WifiChipState::Failed);
    chip.dead = true;
    CHECK(ath.start(files) == WifiChipResult::NoResponse);
  }
  // with real files when given: QCA6174 hw3.0 firmware-6.bin and board-2.bin (subsystem 1a56:1535 is a Killer 1535)
  const char* real_fw = getenv("ATH10K_FIRMWARE_FILE");
  const char* real_board = getenv("ATH10K_BOARD_FILE");
  if (real_fw && real_board && strstr(real_board, "6174")) {
    Qca6174Model chip;
    ConfigSpace config;
    WifiChipBus bus;
    bus.registers = &chip;
    bus.pci = &config;
    Ath10kChip ath(model, bus, alloc);
    MapFirmware files;
    files.files["ath10k/QCA6174/hw3.0/firmware-6.bin"] = read_file(real_fw);
    files.files["ath10k/QCA6174/hw3.0/board-2.bin"] = read_file(real_board);
    CHECK(ath.start(files) == WifiChipResult::Unsupported && ath.state() == WifiChipState::FirmwareLoaded);
    printf("  real QCA6174 files: firmware %s, board \"%s\", %u BMI messages, %zu bytes streamed\n", ath.identity().firmware_version, ath.board_name().c_str(),
           ath.bmi_messages(), chip.streams.size() > 4 ? chip.streams[4].data.size() : size_t{0});
    CHECK(ath.board_name() == StringView(kSub1535));
  }
}

// ---- ath11k/ath12k MHI model -----------------------------------------------------------------------------------------------------------------------------------------------

struct MhiModel final : INicRegisters {
  std::map<uint32_t, uint32_t> regs;
  uint32_t bhi = 0x200;
  uint32_t hw_version = 0x200;
  bool windowed = false, refuse = false, never_ready = false, sys_err = false, dead = false;
  uint32_t ee = 0;
  Bytes loaded;
  uint32_t loads = 0;

  uint8_t read8(uint32_t) override { return 0; }
  uint16_t read16(uint32_t) override { return 0; }
  void write8(uint32_t, uint8_t) override {}
  void write16(uint32_t, uint16_t) override {}
  uint32_t get(uint32_t o) { auto it = regs.find(o); return it == regs.end() ? 0 : it->second; }
  uint32_t read32(uint32_t o) override {
    if (dead) return 0xFFFFFFFFu;
    if (o == mhi_reg::kBhiOff) return bhi;
    if (o == bhi + mhi_reg::kBhiExecEnv) return ee;
    if (o == kAth11kTcsrSocHwVersion && !windowed) return hw_version;
    if (o == kAthWindowStart && windowed && get(kAthWindowRegAddress) == (kAthWindowEnable | 0x36)) return hw_version;
    if (o == mhi_reg::kMhiStatus) return get(o) | (sys_err ? (0xFFu << 8) | mhi_reg::kMhiStatusSysErr : 0);
    return get(o);
  }
  void write32(uint32_t o, uint32_t v) override {
    if (dead) return;
    regs[o] = v;
    if (o == mhi_reg::kMhiCtrl && (v & mhi_reg::kMhiCtrlReset)) { regs[o] = v & ~mhi_reg::kMhiCtrlReset; sys_err = false; }
    if (o == bhi + mhi_reg::kBhiImgTxDb) {
      ++loads;
      const uint64_t address = (uint64_t{get(bhi + mhi_reg::kBhiImgAddrHigh)} << 32) | get(bhi + mhi_reg::kBhiImgAddrLow);
      const uint8_t* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(address));
      loaded.assign(p, p + get(bhi + mhi_reg::kBhiImgSize));
      CHECK(v != 0 && (v & ~mhi_reg::kBhiTxDbSeqMask) == 0);
      regs[bhi + mhi_reg::kBhiStatus] = (refuse ? mhi_reg::kBhiStatusError : mhi_reg::kBhiStatusSuccess) << mhi_reg::kBhiStatusShift;
      if (!refuse) {
        ee = 1;
        if (!never_ready) regs[mhi_reg::kMhiStatus] = mhi_reg::kMhiStatusReady | (1u << 8);
      }
    }
  }
};

static Bytes make_elf(size_t n) {
  Bytes b = fill(n, 0x31);
  const uint8_t ident[6] = {0x7F, 'E', 'L', 'F', 1, 1};
  memcpy(b.data(), ident, 6);
  return b;
}

static void test_ath11k_chip() {
  TestAllocator alloc;
  const WifiAdapterModel& wcn6855 = *wifi_find_pci_adapter(0x17CB, 0x1103);
  Bytes board;
  magic(board, "QCA-ATH11K-BOARD");
  ie(board, 0, board_entry({"bus=pci,vendor=17cb,device=1103,subsystem-vendor=17cb,subsystem-device=3374,qmi-chip-id=2,qmi-board-id=262",
                            "bus=pci,vendor=17cb,device=1103,subsystem-vendor=17cb,subsystem-device=3374,qmi-chip-id=2,qmi-board-id=266"}, fill(60, 1)));
  ie(board, 0, board_entry({"bus=pci,vendor=17cb,device=1103,subsystem-vendor=1043,subsystem-device=0108,qmi-chip-id=18,qmi-board-id=255"}, fill(60, 2)));
  ie(board, 1, board_entry({"bus=pci,vendor=17cb,device=1103,subsystem-vendor=17cb,subsystem-device=3374,qmi-chip-id=2,qmi-board-id=262"}, fill(20, 3)));
  const Bytes amss = make_elf(600 * 1024);
  {
    MhiModel chip;
    ConfigSpace config;
    config.subsystem = 0x337417CB;
    WifiChipBus bus;
    bus.registers = &chip;
    bus.pci = &config;
    Ath11kChip ath(wcn6855, bus, alloc);
    MapFirmware files;
    files.files["ath11k/WCN6855/hw2.0/amss.bin"] = amss;
    files.files["ath11k/WCN6855/hw2.0/board-2.bin"] = board;
    const WifiChipResult r = ath.start(files);
    CHECK(r == WifiChipResult::Unsupported && ath.state() == WifiChipState::FirmwareLoaded);
    if (ath.state() != WifiChipState::FirmwareLoaded) printf("  ath11k: %s\n", ath.message());
    CHECK(strcmp(ath.firmware_dir(), "ath11k/WCN6855/hw2.0") == 0 && strcmp(ath.firmware_name(), "ath11k/WCN6855/hw2.0/amss.bin") == 0);
    CHECK(ath.board_entries() == 3 && ath.matching_board_entries() == 2 && ath.regdb_present() && ath.exec_env_at_start() == 0);
    CHECK(chip.loads == 1 && chip.loaded.size() == kAthSblSize && Bytes(amss.begin(), amss.begin() + kAthSblSize) == chip.loaded);
    CHECK(chip.get(kAth11kPcieSocGlobalReset) == 0);
    CHECK(alloc.dma.empty());      // the SBL buffer was given back
    // hw2.1 by the minor version, firmware-2.bin preferred
    chip.hw_version = 0x210;
    chip.ee = 0;
    chip.regs.erase(mhi_reg::kMhiStatus);
    Bytes container;
    magic(container, "QCOM-ATH11K-FW");
    ie(container, 2, make_elf(1000));
    ie(container, 3, make_elf(100));
    files.files["ath11k/WCN6855/hw2.1/firmware-2.bin"] = container;
    files.files["ath11k/WCN6855/hw2.1/board.bin"] = fill(10, 1);
    CHECK(ath.start(files) == WifiChipResult::Unsupported && strcmp(ath.firmware_name(), "ath11k/WCN6855/hw2.1/firmware-2.bin") == 0 && chip.loaded.size() == 1000);
    // failures
    chip.hw_version = 0x305;
    CHECK(ath.start(files) == WifiChipResult::UnknownRevision);
    chip.hw_version = 0x200;
    chip.ee = 0;
    chip.refuse = true;
    CHECK(ath.start(files) == WifiChipResult::FirmwareRejected && ath.state() == WifiChipState::Failed);
    chip.refuse = false;
    chip.never_ready = true;
    chip.ee = 0;
    chip.regs.erase(mhi_reg::kMhiStatus);
    CHECK(ath.start(files) == WifiChipResult::Timeout);
    chip.never_ready = false;
    chip.bhi = 0x90000;
    CHECK(ath.start(files) == WifiChipResult::NoResponse && strstr(ath.message(), "BHIOFF"));
    chip.bhi = 0x200;
    chip.ee = 9;
    CHECK(ath.start(files) == WifiChipResult::NoResponse);
    chip.ee = 2;      // already in mission mode: no download, wait for READY
    chip.loads = 0;
    chip.regs[mhi_reg::kMhiStatus] = mhi_reg::kMhiStatusReady;
    CHECK(ath.start(files) == WifiChipResult::Unsupported && chip.loads == 0);
    files.files["ath11k/WCN6855/hw2.0/amss.bin"] = fill(1000, 1);     // not an ELF file
    CHECK(ath.start(files) == WifiChipResult::FirmwareInvalid);
    files.files.erase("ath11k/WCN6855/hw2.0/amss.bin");
    CHECK(ath.start(files) == WifiChipResult::FirmwareMissing);
    files.files["ath11k/WCN6855/hw2.0/amss.bin"] = amss;
    Bytes damaged = board;
    damaged[24] = 0xFF;      // the first entry's length
    files.files["ath11k/WCN6855/hw2.0/board-2.bin"] = damaged;
    CHECK(ath.start(files) == WifiChipResult::FirmwareInvalid);
    chip.dead = true;
    CHECK(ath.start(files) == WifiChipResult::NoResponse);
  }
  // ath12k WCN7850: the hardware version through the register window
  {
    MhiModel chip;
    chip.windowed = true;
    WifiChipBus bus;
    bus.registers = &chip;
    const WifiAdapterModel& wcn7850 = *wifi_find_pci_adapter(0x17CB, 0x1107);
    Ath11kChip ath(wcn7850, bus, alloc);
    MapFirmware files;
    Bytes board12;
    magic(board12, "QCA-ATH12K-BOARD");
    ie(board12, 0, board_entry({"bus=pci,vendor=17cb,device=1107,subsystem-vendor=17cb,subsystem-device=3378,qmi-chip-id=2,qmi-board-id=260"}, fill(60, 1)));
    files.files["ath12k/WCN7850/hw2.0/amss.bin"] = make_elf(4000);
    files.files["ath12k/WCN7850/hw2.0/board-2.bin"] = board12;
    CHECK(ath.start(files) == WifiChipResult::Unsupported && ath.state() == WifiChipState::FirmwareLoaded && ath.is_ath12k());
    CHECK(strcmp(ath.firmware_dir(), "ath12k/WCN7850/hw2.0") == 0 && chip.loaded.size() == 4000 && ath.board_entries() == 1 && ath.matching_board_entries() == 0);
    CHECK(strstr(ath.message(), "ath12k"));
    files.files["ath12k/WCN7850/hw2.0/board-2.bin"] = board;     // an ath11k board file
    CHECK(ath.start(files) == WifiChipResult::FirmwareInvalid);
  }
}

// ---- real files -----------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_real_files() {
  bool any = false;
  if (const char* path = getenv("ATH10K_FIRMWARE_FILE")) {
    any = true;
    const Bytes f = read_file(path);
    Ath10kFirmwareFile fw;
    const AthParse r = ath10k_parse_firmware(view(f), &fw);
    CHECK(r == AthParse::Ok);
    printf("  %s: %zu bytes, \"%s\", features 0x%llx, image %llu, OTP %llu, WMI op %u, HTT op %u, code swap %llu\n", path, f.size(), fw.version.c_str(),
           static_cast<unsigned long long>(fw.features), static_cast<unsigned long long>(fw.firmware.size()), static_cast<unsigned long long>(fw.otp.size()),
           static_cast<unsigned>(fw.wmi_op), static_cast<unsigned>(fw.htt_op), static_cast<unsigned long long>(fw.code_swap.size()));
    CHECK(fw.firmware.size() > 100000 && fw.unknown_ies == 0);
    for (size_t i = 0; i < 64 && i < f.size(); ++i) {     // damage in the header and the first IEs
      Bytes t = f;
      t[i] ^= 0x80;
      (void)ath10k_parse_firmware(view(t), &fw);
    }
    Bytes t(f.begin(), f.begin() + f.size() / 2);
    CHECK(ath10k_parse_firmware(view(t), &fw) != AthParse::Ok || fw.firmware.data() + fw.firmware.size() <= t.data() + t.size());
  }
  struct Board { const char* env; const char* magic; };
  for (const Board& b : {Board{"ATH10K_BOARD_FILE", kAth10kBoardMagic}, Board{"ATH11K_BOARD_FILE", kAth11kBoardMagic}, Board{"ATH12K_BOARD_FILE", kAth12kBoardMagic}}) {
    const char* path = getenv(b.env);
    if (!path) continue;
    any = true;
    const Bytes f = read_file(path);
    Listed l;
    CHECK(ath_board_for_each(view(f), b.magic, &list_entry, &l));
    size_t regdb = 0;
    for (uint32_t t : l.types) regdb += (b.magic != kAth10kBoardMagic && t == kAthBoardIeRegdb);
    printf("  %s: %zu entries (%zu regdb), %zu data bytes, first \"%s\"\n", path, l.names.size(), regdb, l.bytes, l.names.empty() ? "" : l.names[0].c_str());
    CHECK(!l.names.empty());
    // every name is found again, and its data lies in the file
    size_t found = 0;
    for (size_t i = 0; i < l.names.size(); ++i) {
      ConstByteView data;
      if (ath_board_find(view(f), b.magic, l.types[i], StringView(l.names[i].data(), l.names[i].size()), &data) == AthBoardFind::Found &&
          data.data() >= f.data() && data.data() + data.size() <= f.data() + f.size())
        ++found;
    }
    CHECK(found == l.names.size());
    for (size_t i = 0; i < f.size(); i += 4093) {
      Bytes t = f;
      t[i] ^= 0x40;
      Listed d;
      (void)ath_board_for_each(view(t), b.magic, &list_entry, &d);
    }
  }
  if (const char* path = getenv("ATH12K_FIRMWARE_FILE")) {
    any = true;
    const Bytes f = read_file(path);
    AthQcomFirmwareFile fw;
    CHECK(ath_parse_qcom_firmware(view(f), kAth12kFirmwareMagic, &fw) == AthParse::Ok && ath_is_elf32(fw.amss) && ath_is_elf32(fw.m3));
    printf("  %s: AMSS %llu, M3 %llu, dual-MAC AMSS %llu bytes, features 0x%llx, time %u\n", path, static_cast<unsigned long long>(fw.amss.size()),
           static_cast<unsigned long long>(fw.m3.size()), static_cast<unsigned long long>(fw.amss_dual_mac.size()), static_cast<unsigned long long>(fw.features), fw.timestamp);
  }
  if (const char* path = getenv("ATH_AMSS_FILE")) {
    any = true;
    const Bytes f = read_file(path);
    CHECK(ath_is_elf32(view(f)));
    printf("  %s: %zu bytes, ELF\n", path, f.size());
  }
  if (!any) printf("  (no real firmware files named in the environment)\n");
}

int main() {
  test_models();
  test_ath10k_firmware();
  test_qcom_firmware();
  test_board_names();
  test_board_file();
  test_bmi_encoding();
  test_ath10k_chip();
  test_ath11k_chip();
  test_real_files();
  printf(g_failed ? "QualcommWifiTest: %d FAILED\n" : "QualcommWifiTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
