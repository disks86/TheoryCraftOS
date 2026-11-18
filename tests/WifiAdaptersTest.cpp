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

// The vendor-independent Wi-Fi adapter layer: the catalogue across every vendor's table (unique IDs, sensible entries, the families each vendor's drivers handle), firmware
// file names (the folder rule, Intel's naming per generation), IntelWifiChip against a register model and an in-memory firmware folder, the factory, and the probe over
// fake PCI and USB buses. With IWL_FIRMWARE_DIR naming a folder of real (decompressed) iwlwifi files, every one of them is parsed and the names the chip would build
// are looked for there.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "Hardware/WirelessInterface/Intel/IntelWifiChip.hpp"
#include "Hardware/WirelessInterface/Intel/IwlDevices.hpp"
#include "Hardware/WirelessInterface/WifiAdapterCatalog.hpp"
#include "Hardware/WirelessInterface/WifiAdapterProbe.hpp"
#include "Hardware/WirelessInterface/WifiChipFactory.hpp"

namespace hardware {
void delay_us(uint64_t) {}
}  // namespace hardware

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { free(p); }
} g_allocator;

struct ModelRegisters final : INicRegisters {
  std::map<uint32_t, uint32_t> values;
  uint32_t writes = 0;
  uint8_t read8(uint32_t o) override { return static_cast<uint8_t>(read32(o & ~3u) >> (8 * (o & 3))); }
  uint16_t read16(uint32_t o) override { return static_cast<uint16_t>(read32(o & ~3u) >> (8 * (o & 2))); }
  uint32_t read32(uint32_t o) override { auto it = values.find(o); return it == values.end() ? 0 : it->second; }
  void write8(uint32_t, uint8_t) override { ++writes; }
  void write16(uint32_t, uint16_t) override { ++writes; }
  void write32(uint32_t, uint32_t) override { ++writes; }
};

struct MemoryFirmware final : IWifiFirmwareSource {
  std::map<std::string, std::vector<uint8_t>> files;
  std::vector<std::string> asked;
  bool read(const char* name, ByteBuffer& out) override {
    asked.push_back(name);
    auto it = files.find(name);
    if (it == files.end() || it->second.empty()) return false;
    out.clear();
    return out.append(ConstByteView(it->second.data(), it->second.size()));
  }
};

// A small but whole iwlwifi TLV file: header, a human-readable version, and one runtime section.
std::vector<uint8_t> tiny_iwl_file(const char* version) {
  std::vector<uint8_t> f;
  auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(v >> (8 * i))); };
  put32(0);
  put32(0x0a4c5749);
  char human[64] = {};
  snprintf(human, sizeof human, "%s", version);
  for (char c : human) f.push_back(static_cast<uint8_t>(c));
  put32(0x00004D00);     // version (API 77 in bits 8-15)
  put32(1);              // build
  put32(0); put32(0);    // (reserved)
  // IWL_UCODE_TLV_SEC_RT (19): device offset + 8 bytes of code
  put32(19); put32(12); put32(0x00800000); put32(0x11111111); put32(0x22222222);
  return f;
}

std::string name_of(const IwlFirmwareName& n) { return std::string(n.c_str()); }

void test_catalogue() {
  const uint32_t count = wifi_adapter_count();
  CHECK(count > 0);
  std::set<std::tuple<int, int, int>> ids;
  std::map<WifiVendor, uint32_t> per_vendor;
  for (uint32_t i = 0; i < count; ++i) {
    const WifiAdapterModel* m = wifi_adapter_at(i);
    CHECK(m != nullptr);
    if (!m) continue;
    const bool fresh = ids.insert({static_cast<int>(m->bus), m->vendor_id, m->device_id}).second;
    if (!fresh) printf("duplicate %s %04x:%04x (%s)\n", wifi_bus_name(m->bus), m->vendor_id, m->device_id, m->chip);
    CHECK(fresh);
    CHECK(wifi_driver_family_vendor(m->family) == m->vendor);
    CHECK(m->chip && m->chip[0] && m->product && m->product[0] && m->firmware);
    CHECK(m->spatial_streams >= 1 && m->spatial_streams <= 8);
    if (m->firmware[0]) CHECK(wifi_firmware_name_valid(StringView(m->firmware)));
    CHECK(wifi_find_adapter(m->bus, m->vendor_id, m->device_id) == m);
    ++per_vendor[m->vendor];
  }
  CHECK(wifi_adapter_at(count) == nullptr);
  CHECK(per_vendor[WifiVendor::Intel] > 50);
  printf("catalogue: %u adapters (Intel %u, Realtek %u, MediaTek %u, Qualcomm %u, Broadcom %u)\n", count, per_vendor[WifiVendor::Intel], per_vendor[WifiVendor::Realtek],
         per_vendor[WifiVendor::MediaTek], per_vendor[WifiVendor::Qualcomm], per_vendor[WifiVendor::Broadcom]);
  // Every 9000-series ID the Intel driver binds is in the catalogue, as that family.
  for (uint16_t id : {0x2526, 0x271B, 0x271C, 0x30DC, 0x31DC, 0x9DF0, 0xA370}) {
    CHECK(iwl_find_pci_device(0x8086, id) != nullptr);
    const WifiAdapterModel* m = wifi_find_pci_adapter(0x8086, id);
    CHECK(m && m->family == WifiDriverFamily::IwlMvm9000);
  }
  const WifiAdapterModel* ax210 = wifi_find_pci_adapter(0x8086, 0x2725);
  CHECK(ax210 && ax210->standard == WifiStandard::Wifi6E && wifi_mac_split(ax210->family) == WifiMacSplit::Offload);
  CHECK(wifi_find_pci_adapter(0x8086, 0x1234) == nullptr);
  CHECK(wifi_find_usb_adapter(0x8086, 0x2725) == nullptr);     // a PCI ID is not a USB one
  CHECK(StringView(wifi_standard_name(WifiStandard::Wifi7)) == StringView("Wi-Fi 7 (802.11be)"));
  CHECK(wifi_mac_split(WifiDriverFamily::Brcmfmac) == WifiMacSplit::FullMac);
  CHECK(wifi_mac_split(WifiDriverFamily::Rtw88) == WifiMacSplit::SoftMac);
}

void test_firmware_names() {
  CHECK(wifi_firmware_name_valid("rtw88/rtw8822c_fw.bin"));
  CHECK(wifi_firmware_name_valid("ath10k/QCA6174/hw3.0/firmware-6.bin"));
  CHECK(wifi_firmware_name_valid("iwlwifi-cc-a0-77.ucode"));
  CHECK(!wifi_firmware_name_valid(""));
  CHECK(!wifi_firmware_name_valid("/etc/passwd"));
  CHECK(!wifi_firmware_name_valid("../x"));
  CHECK(!wifi_firmware_name_valid("a/../../x"));
  CHECK(!wifi_firmware_name_valid("a/./x"));
  CHECK(!wifi_firmware_name_valid("a//x"));
  CHECK(!wifi_firmware_name_valid("a/"));
  CHECK(!wifi_firmware_name_valid("a b"));
  CHECK(!wifi_firmware_name_valid("a\\b"));
  CHECK(wifi_firmware_name_valid("..a/b.."));     // dots inside a name are fine

  IwlFirmwareName n;
  // 22000 family
  CHECK(iwl_gen2_firmware_prefix(0x334, 0x0010A100, &n) && name_of(n) == "iwlwifi-Qu-b0-hr-b0");
  CHECK(iwl_gen2_firmware_prefix(0x338, 0x0010A100, &n) && name_of(n) == "iwlwifi-Qu-c0-hr-b0");
  CHECK(iwl_gen2_firmware_prefix(0x334, 0x00105100, &n) && name_of(n) == "iwlwifi-Qu-b0-jf-b0");     // a 9560 module on an Ice Lake CNVi
  CHECK(iwl_gen2_firmware_prefix(0x354, 0x0010A100, &n) && name_of(n) == "iwlwifi-QuZ-a0-hr-b0");
  CHECK(iwl_gen2_firmware_prefix(0x354, 0x00108000, &n) && name_of(n) == "iwlwifi-QuZ-a0-jf-b0");
  // AX210 family
  CHECK(iwl_gen2_firmware_prefix(0x420, 0x0010D000, &n) && name_of(n) == "iwlwifi-ty-a0-gf-a0");
  CHECK(iwl_gen2_firmware_prefix(0x370, 0x0010D000, &n) && name_of(n) == "iwlwifi-so-a0-gf-a0");
  CHECK(iwl_gen2_firmware_prefix(0x370, 0x1010D000, &n) && name_of(n) == "iwlwifi-so-a0-gf4-a0");
  CHECK(iwl_gen2_firmware_prefix(0x430, 0x0010A100, &n) && name_of(n) == "iwlwifi-so-a0-hr-b0");     // SoF
  CHECK(iwl_gen2_firmware_prefix(0x444, 0x0010D000, &n) && name_of(n) == "iwlwifi-ma-b0-gf-a0");
  // Wi-Fi 7 (BZ MAC: step in bits 0-3)
  CHECK(iwl_gen2_firmware_prefix(0x461, 0x00112200, &n) && name_of(n) == "iwlwifi-bz-b0-fm-c0");
  CHECK(iwl_gen2_firmware_prefix(0x461, 0x00113100, &n) && name_of(n) == "iwlwifi-bz-b0-wh-b0");
  CHECK(iwl_gen2_firmware_prefix(0x472, 0x00112200, &n) && name_of(n) == "iwlwifi-gl-c0-fm-c0");
  CHECK(iwl_gen2_firmware_prefix(0x480, 0x00113100, &n) && name_of(n) == "iwlwifi-sc-a0-wh-b0");
  // the 9000 MACs agree with IwlDevices
  CHECK(iwl_gen2_firmware_prefix(0x310, 0x00105000, &n) && name_of(n) == "iwlwifi-9000-pu-b0-jf-b0");
  // unknown MAC or radio
  CHECK(!iwl_gen2_firmware_prefix(0x200, 0x0010A100, &n));
  CHECK(!iwl_gen2_firmware_prefix(0x370, 0, &n));

  CHECK(iwl_versioned_firmware_name("iwlwifi-so-a0-gf-a0", 89, false, &n) && name_of(n) == "iwlwifi-so-a0-gf-a0-89.ucode");
  CHECK(iwl_versioned_firmware_name("iwlwifi-bz-b0-fm-c0", 107, true, &n) && name_of(n) == "iwlwifi-bz-b0-fm-c0-c107.ucode");
  uint32_t api = 0;
  CHECK(iwl_split_firmware_name("iwlwifi-7260-17.ucode", &n, &api) && name_of(n) == "iwlwifi-7260" && api == 17);
  CHECK(iwl_split_firmware_name("iwlwifi-bz-b0-fm-c0-c107.ucode", &n, &api) && name_of(n) == "iwlwifi-bz-b0-fm-c0" && api == 107);
  CHECK(!iwl_split_firmware_name("iwlwifi-7260.bin", &n, &api));
  CHECK(!iwl_split_firmware_name("iwlwifi-7260-.ucode", &n, &api));
  CHECK(!iwl_split_firmware_name("iwlwifi-7260-1x.ucode", &n, &api));
  CHECK(!iwl_split_firmware_name(".ucode", &n, &api));
  CHECK(!iwl_split_firmware_name("-123456.ucode", &n, &api));
  // a name that does not fit the 63 characters
  std::string longer(70, 'x');
  CHECK(!iwl_versioned_firmware_name(StringView(longer.c_str()), 1, false, &n));
}

void test_intel_chip() {
  const WifiAdapterModel* ax210 = wifi_find_pci_adapter(0x8086, 0x2725);
  CHECK(ax210 != nullptr);
  if (!ax210) return;
  ModelRegisters regs;
  regs.values[0x028] = 0x420;          // TY
  regs.values[0x09C] = 0x0010D000;     // GF
  WifiChipBus bus;
  bus.registers = &regs;
  UniquePointer<IWifiChip> chip = make_wifi_chip(*ax210, bus, g_allocator);
  CHECK(chip.get() != nullptr);
  if (!chip) return;
  MemoryFirmware files;
  // nothing there: names the newest, asks for every API in the range
  CHECK(chip->start(files) == WifiChipResult::FirmwareMissing);
  CHECK(StringView(chip->firmware_name()) == StringView("iwlwifi-ty-a0-gf-a0-89.ucode"));
  CHECK(files.asked.size() == 89 - 59 + 1 && files.asked.front() == "iwlwifi-ty-a0-gf-a0-89.ucode" && files.asked.back() == "iwlwifi-ty-a0-gf-a0-59.ucode");
  CHECK(chip->state() == WifiChipState::Failed);
  // an older API is found
  files.files["iwlwifi-ty-a0-gf-a0-77.ucode"] = tiny_iwl_file("77.test.0");
  files.asked.clear();
  CHECK(chip->start(files) == WifiChipResult::Unsupported);
  CHECK(StringView(chip->firmware_name()) == StringView("iwlwifi-ty-a0-gf-a0-77.ucode"));
  CHECK(chip->state() == WifiChipState::Identified);
  CHECK(StringView(chip->identity().firmware_version) == StringView("77.test.0"));
  CHECK(chip->identity().chip_id == 0x42);
  CHECK(regs.writes == 0);     // identification only reads
  // a damaged file
  std::vector<uint8_t> bad = tiny_iwl_file("x");
  bad[4] ^= 0x01;
  files.files["iwlwifi-ty-a0-gf-a0-89.ucode"] = bad;
  CHECK(chip->start(files) == WifiChipResult::FirmwareInvalid);
  // truncated at every length: never accepted as whole, never out of bounds (ASan)
  std::vector<uint8_t> good = tiny_iwl_file("t");
  for (size_t cut = 0; cut < good.size(); ++cut) {
    files.files["iwlwifi-ty-a0-gf-a0-89.ucode"] = std::vector<uint8_t>(good.begin(), good.begin() + static_cast<long>(cut));
    const WifiChipResult r = chip->start(files);
    CHECK(r != WifiChipResult::Unsupported || cut == 0);     // (an empty file counts as missing, and then API 77 is found)
  }
  // a card that does not answer
  regs.values[0x028] = 0xFFFFFFFF;
  CHECK(chip->start(files) == WifiChipResult::NoResponse);
  // an unknown radio
  regs.values[0x028] = 0x370;
  regs.values[0x09C] = 0;
  CHECK(chip->start(files) == WifiChipResult::UnknownRevision);
  // no registers
  WifiChipBus empty;
  UniquePointer<IWifiChip> bare = make_wifi_chip(*ax210, empty, g_allocator);
  CHECK(bare && bare->start(files) == WifiChipResult::NoRegisters);

  // the 7265 IDs: 7265 or 7265D by the revision
  const WifiAdapterModel* s7265 = wifi_find_pci_adapter(0x8086, 0x095A);
  CHECK(s7265 != nullptr);
  if (s7265) {
    UniquePointer<IWifiChip> c = make_wifi_chip(*s7265, bus, g_allocator);
    MemoryFirmware none;
    regs.values[0x028] = 0x210;
    CHECK(c && c->start(none) == WifiChipResult::FirmwareMissing && StringView(c->firmware_name()) == StringView("iwlwifi-7265D-29.ucode"));
    regs.values[0x028] = 0x144;
    CHECK(c && c->start(none) == WifiChipResult::FirmwareMissing && StringView(c->firmware_name()) == StringView("iwlwifi-7265-29.ucode"));
    CHECK(none.asked.size() > 0 && none.asked.back() == "iwlwifi-7265-17.ucode");
  }
  // the 9000 series and DVM cards get no chip from here
  const WifiAdapterModel* s9560 = wifi_find_pci_adapter(0x8086, 0x9DF0);
  CHECK(s9560 && !make_wifi_chip(*s9560, bus, g_allocator));
  const WifiAdapterModel* s6205 = wifi_find_pci_adapter(0x8086, 0x0085);
  CHECK(s6205 && !make_wifi_chip(*s6205, bus, g_allocator));
}

struct FakePci final : IPciBus {
  std::vector<PciDeviceInfo> devices;
  mutable std::map<uint16_t, uint32_t> config;
  uint64_t bar_base = 0;
  mutable uint8_t last_bar = 0xFF;
  const char* name() const override { return "fake"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return static_cast<uint32_t>(devices.size()); }
  bool pci_device(uint32_t i, PciDeviceInfo* out) const override { if (i >= devices.size()) return false; *out = devices[i]; return true; }
  uint32_t read32(PciAddress, uint16_t offset) const override { auto it = config.find(offset); return it == config.end() ? 0xFFFFFFFF : it->second; }
  void write32(PciAddress, uint16_t offset, uint32_t value) const override { config[offset] = value; }
  uint64_t memory_bar(PciAddress, uint8_t bar) const override { last_bar = bar; return bar_base; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

struct FakeUsb final : IUsbBus {
  std::vector<std::vector<int>> ports;    // per controller: product IDs (-1 empty), all with vendor 0x0BDA except 0x7961 (0x0E8D)
  const char* name() const override { return "fake usb"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 0; }
  uint32_t controller_count() const override { return static_cast<uint32_t>(ports.size()); }
  UsbControllerKind controller_kind(uint32_t) const override { return UsbControllerKind{}; }
  uint32_t port_count(uint32_t c) const override { return c < ports.size() ? static_cast<uint32_t>(ports[c].size()) : 0; }
  bool port_info(uint32_t, uint32_t, UsbPortInfo*) const override { return false; }
  bool device_descriptor(uint32_t c, uint32_t p, UsbDeviceDescriptor* out) const override {
    if (c >= ports.size() || p >= ports[c].size() || ports[c][p] < 0) return false;
    *out = UsbDeviceDescriptor{};
    out->product_id = static_cast<uint16_t>(ports[c][p]);
    out->vendor_id = out->product_id == 0x7961 ? 0x0E8D : 0x0BDA;
    return true;
  }
  IUsbHostController* host_controller(uint32_t) override { return nullptr; }
};

void test_probe() {
  FakePci pci;
  pci.devices.push_back(PciDeviceInfo{{0, 2, 0}, 0x8086, 0x3E9B, 3, 0, 0});    // a GPU
  pci.devices.push_back(PciDeviceInfo{{1, 0, 0}, 0x8086, 0x2725, 2, 0x80, 0});  // AX210
  pci.devices.push_back(PciDeviceInfo{{0, 0x14, 3}, 0x8086, 0x9DF0, 2, 0x80, 0});
  FakeUsb usb;
  usb.ports = {{-1, 0x1234}, {}};
  DetectedWifiAdapter found[4];
  uint32_t n = find_wifi_adapters(&pci, &usb, ArrayView<DetectedWifiAdapter>(found));
  CHECK(n == 2);
  CHECK(found[0].model && found[0].model->device_id == 0x2725 && found[0].pci.address.bus == 1);
  CHECK(found[1].model && found[1].model->device_id == 0x9DF0);
  // more than fit: counted, not written past the end
  DetectedWifiAdapter one[1];
  CHECK(find_wifi_adapters(&pci, nullptr, ArrayView<DetectedWifiAdapter>(one)) == 2 && one[0].model->device_id == 0x2725);
  CHECK(find_wifi_adapters(nullptr, nullptr, ArrayView<DetectedWifiAdapter>(found)) == 0);
  // USB adapters, if the vendor tables have them
  const WifiAdapterModel* usb_model = wifi_find_usb_adapter(0x0E8D, 0x7961);
  usb.ports = {{0x7961}};
  n = find_wifi_adapters(nullptr, &usb, ArrayView<DetectedWifiAdapter>(found));
  CHECK(n == (usb_model ? 1u : 0u));
  if (usb_model && n == 1) CHECK(found[0].model == usb_model && found[0].usb_controller == 0 && found[0].usb_port == 0);
}

// A test mapper hands out plain memory as the register window.
struct MemoryMapper final : IRegisterMapper {
  alignas(16) uint32_t window[0x2000 / 4] = {};
  uint64_t asked_length = 0;
  bool map_registers(uint64_t, uint64_t length, MmioRegion* region) override {
    asked_length = length;
    if (length > sizeof window) length = sizeof window;
    *region = MmioRegion(reinterpret_cast<uint64_t>(window), length);
    return true;
  }
};

void test_slot() {
  FakePci pci;
  pci.devices.push_back(PciDeviceInfo{{1, 0, 0}, 0x8086, 0x2723, 2, 0x80, 0});  // AX200
  pci.config[0x04] = 0x00100000;      // a status bit, no decoding yet
  pci.bar_base = 0xF0000000;
  MemoryMapper mapper;
  mapper.window[0x028 / 4] = 0x340;
  mapper.window[0x09C / 4] = 0x0010A100;
  DetectedWifiAdapter found[2];
  CHECK(find_wifi_adapters(&pci, nullptr, ArrayView<DetectedWifiAdapter>(found)) == 1);
  WifiChipSlot slot(g_allocator);
  WifiChipSlot::Inputs inputs;
  inputs.pci = &pci;
  inputs.mapper = &mapper;
  MemoryFirmware files;
  files.files["iwlwifi-cc-a0-74.ucode"] = tiny_iwl_file("cc 74");
  CHECK(slot.start(found[0], inputs, files) == WifiChipResult::Unsupported);
  CHECK(slot.chip() && slot.chip()->state() == WifiChipState::Identified);
  CHECK(slot.chip() && StringView(slot.chip()->firmware_name()) == StringView("iwlwifi-cc-a0-74.ucode"));
  CHECK(pci.config[0x04] == 0x6);     // memory + bus master on, status half not written back
  CHECK(pci.last_bar == 0 && mapper.asked_length == 0x2000);
  slot.stop();
  CHECK(slot.chip() == nullptr);
  // a family without a chip driver: nothing is written to the device
  pci.devices[0].device_id = 0x9DF0;
  pci.config[0x04] = 0;
  CHECK(find_wifi_adapters(&pci, nullptr, ArrayView<DetectedWifiAdapter>(found)) == 1);
  CHECK(slot.start(found[0], inputs, files) == WifiChipResult::Unsupported && slot.chip() == nullptr);
  CHECK(pci.config[0x04] == 0);
  // no BAR
  pci.devices[0].device_id = 0x2723;
  pci.bar_base = 0;
  CHECK(find_wifi_adapters(&pci, nullptr, ArrayView<DetectedWifiAdapter>(found)) == 1);
  CHECK(slot.start(found[0], inputs, files) == WifiChipResult::NoRegisters);
  // no buses at all
  CHECK(slot.start(found[0], WifiChipSlot::Inputs{}, files) == WifiChipResult::NoRegisters);
  CHECK(wifi_pci_register_window(WifiDriverFamily::Rtw88).bar == 2);
}

// Real files: every iwlwifi file in the folder parses, and the prefixes built for the known MAC/radio pairs exist there.
void test_real_files() {
  const char* dir = getenv("IWL_FIRMWARE_DIR");
  if (!dir) {
    printf("(IWL_FIRMWARE_DIR not set: real firmware files skipped)\n");
    return;
  }
  DIR* d = opendir(dir);
  CHECK(d != nullptr);
  if (!d) return;
  uint32_t parsed = 0;
  while (dirent* e = readdir(d)) {
    std::string name = e->d_name;
    if (name.size() < 7 || name.substr(name.size() - 6) != ".ucode") continue;
    std::string path = std::string(dir) + "/" + name;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) continue;
    std::vector<uint8_t> bytes;
    int c;
    while ((c = fgetc(f)) != EOF) bytes.push_back(static_cast<uint8_t>(c));
    fclose(f);
    IwlFirmware fw(g_allocator);
    const char* error = "";
    const bool ok = fw.parse(ConstByteView(bytes.data(), bytes.size()), &error);
    if (!ok) printf("%s: %s\n", name.c_str(), error);
    CHECK(ok && fw.has_image(IwlImage::Regular));
    printf("  %s: %s, API %u\n", name.c_str(), fw.human_readable(), fw.api_version());
    ++parsed;
  }
  closedir(d);
  CHECK(parsed > 0);
}

}  // namespace

int main() {
  test_catalogue();
  test_firmware_names();
  test_intel_chip();
  test_probe();
  test_slot();
  test_real_files();
  if (g_failed) {
    printf("WifiAdaptersTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("WifiAdaptersTest: all passed\n");
  return 0;
}
