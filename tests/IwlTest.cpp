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


// The Intel Wireless-AC 9000 series card (9260 / 9560): the PCI IDs and the firmware file names from the hardware revision, the firmware file (TLV) parser on a synthetic file and, when
// the environment names a real file (IWL_FIRMWARE_FILE), on that one with damage done to it, and the register level bring-up (taking ownership, waking the MAC, the peripheral and
// memory windows, the adapter's start and stop) against a model of the chip's registers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/WirelessInterface/Intel/IwlDevices.hpp"
#include "Hardware/WirelessInterface/Intel/IwlFirmware.hpp"
#include "Hardware/WirelessInterface/Intel/IwlPcie.hpp"

namespace hardware {
void delay_us(uint64_t) {}
}  // namespace hardware

using namespace hardware;

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

static void test_devices() {
  CHECK(iwl_find_pci_device(0x8086, 0x2526) && iwl_find_pci_device(0x8086, 0x9DF0) && iwl_find_pci_device(0x8086, 0xA370) && iwl_find_pci_device(0x8086, 0x31DC));
  CHECK(!iwl_find_pci_device(0x8086, 0x1234) && !iwl_find_pci_device(0x10EC, 0x2526));
  CHECK(iwl_find_pci_device(0x8086, 0x31DC)->shared_clock && iwl_find_pci_device(0x8086, 0x31DC)->xtal_latency == 670 && iwl_find_pci_device(0x8086, 0x30DC)->xtal_latency == 2820);
  CHECK(!iwl_find_pci_device(0x8086, 0x2526)->integrated && iwl_find_pci_device(0x8086, 0xA370)->integrated);
  char prefix[48], name[64];
  // hardware revision type PU (0x31) and TH (0x32) in bits 4 to 15, the radio JF1 / JF2 in bits 12 to 23 of the RF id, any steps
  CHECK(iwl_firmware_prefix((0x31u << 4) | 0x1, 0x108u << 12, prefix, sizeof prefix) && strcmp(prefix, "iwlwifi-9000-pu-b0-jf-b0") == 0);
  CHECK(iwl_firmware_prefix((0x32u << 4) | 0x2, (0x105u << 12) | (3u << 8), prefix, sizeof prefix) && strcmp(prefix, "iwlwifi-9260-th-b0-jf-b0") == 0);
  CHECK(!iwl_firmware_prefix((0x37u << 4), 0x108u << 12, prefix, sizeof prefix));                        // another MAC
  CHECK(!iwl_firmware_prefix((0x31u << 4), 0x10Au << 12, prefix, sizeof prefix));                        // another radio (HR)
  CHECK(!iwl_firmware_prefix((0x31u << 4), (0x108u << 12) | 0x10000000u, prefix, sizeof prefix));        // CDB
  CHECK(!iwl_firmware_prefix((0x31u << 4), 0x108u << 12, prefix, 10) && !iwl_firmware_prefix((0x31u << 4), 0x108u << 12, prefix, 0));
  CHECK(iwl_firmware_prefix((0x31u << 4), 0x108u << 12, prefix, sizeof prefix) && iwl_firmware_file_name(prefix, 46, name, sizeof name) && strcmp(name, "iwlwifi-9000-pu-b0-jf-b0-46.ucode") == 0);
  CHECK(iwl_firmware_file_name(prefix, 7, name, sizeof name) && strcmp(name, "iwlwifi-9000-pu-b0-jf-b0-7.ucode") == 0 && !iwl_firmware_file_name(prefix, 46, name, 12));
}

// ---- firmware files -------------------------------------------------------------------------------------------------------------------------------------------------------------

typedef std::vector<uint8_t> Bytes;
static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
static void tlv(Bytes& b, uint32_t type, const Bytes& data) {
  put32(b, type);
  put32(b, static_cast<uint32_t>(data.size()));
  b.insert(b.end(), data.begin(), data.end());
  while (b.size() % 4) b.push_back(0);
}
static Bytes u32s(std::initializer_list<uint32_t> v) { Bytes b; for (uint32_t x : v) put32(b, x); return b; }
static Bytes section(uint32_t offset, size_t size, uint8_t fill) { Bytes b = u32s({offset}); b.insert(b.end(), size, fill); return b; }

static Bytes make_firmware() {
  Bytes f;
  put32(f, 0);
  put32(f, IwlFirmware::kMagic);
  const char human[64] = "9.9.9.9 test firmware";
  f.insert(f.end(), human, human + 64);
  put32(f, (9u << 24) | (46u << 8) | 1);                         // major 9, API 46
  put32(f, 1234);                                                // build
  put32(f, 0);
  put32(f, 0);
  tlv(f, 18, u32s({0x00000012}));                                // FLAGS
  tlv(f, 29, u32s({0, 0x00000005}));                             // API_CHANGES_SET: index 0, bits 0 and 2
  tlv(f, 30, u32s({1, 0x00000100}));                             // ENABLED_CAPABILITIES: index 1, bit 8 -> capability 40
  tlv(f, 23, u32s({0x0001A2B3}));                                // PHY_SKU
  tlv(f, 27, u32s({2}));                                         // NUM_OF_CPU
  tlv(f, 36, u32s({46, 7, 99}));                                 // FW_VERSION
  tlv(f, 6, u32s({512}));                                        // PROBE_MAX_LEN
  tlv(f, 32, u32s({0x20000}));                                   // PAGING
  tlv(f, 19, section(0x00000000, 13, 0xA1));                     // SEC_RT: odd size, padded
  tlv(f, 19, section(0x00010000, 8, 0xA2));
  tlv(f, 19, section(0xFFFFCCCC, 0, 0));                         // the processors' separator
  tlv(f, 24, section(0x00800000, 12, 0xB1));                     // SECURE_SEC_RT for the second processor
  tlv(f, 19, section(0xAAAABBBB, 0, 0));                         // the paging separator
  tlv(f, 19, section(0x00000000, 16, 0xC1));                     // a paging section
  tlv(f, 20, section(0x00000000, 4, 0xD1));                      // SEC_INIT
  tlv(f, 48, Bytes{1, 2, 3, 4, 0, 5, 6, 7});                     // CMD_VERSIONS: (group 1, cmd 2, ver 3, notif 4) and (0, 5, 6, 7)
  tlv(f, 52, Bytes{9, 9, 9, 9, 9});                              // IML
  tlv(f, 0x1000005, Bytes{1, 2, 3});                             // a type nobody needs (debug)
  return f;
}

static void test_firmware() {
  TestAllocator alloc;
  IwlFirmware fw(alloc);
  const Bytes file = make_firmware();
  const char* error = nullptr;
  CHECK(fw.parse(ConstByteView(file.data(), file.size()), &error));
  if (error) printf("  error: %s\n", error);
  CHECK(fw.api_version() == 46 && fw.version() == ((9u << 24) | (46u << 8) | 1) && fw.build() == 1234 && strcmp(fw.human_readable(), "9.9.9.9 test firmware") == 0);
  CHECK(fw.flags() == 0x12 && fw.phy_config() == 0x0001A2B3 && fw.cpu_count() == 2 && fw.paging_size() == 0x20000 && fw.probe_max_length() == 512 && fw.fw_major() == 46 && fw.fw_minor() == 7);
  CHECK(fw.has_api(0) && !fw.has_api(1) && fw.has_api(2) && !fw.has_api(32) && fw.has_capability(40) && !fw.has_capability(8) && !fw.has_api(10000));
  CHECK(fw.sections(IwlImage::Regular).count() == 6 && fw.has_image(IwlImage::Init) && !fw.has_image(IwlImage::Wowlan));
  IwlSection s[8];
  CHECK(fw.part(IwlImage::Regular, 0, s, 8) == 2 && s[0].device_offset == 0 && s[0].data.size() == 13 && s[0].data[0] == 0xA1 && s[1].device_offset == 0x10000 && s[1].data.size() == 8 && !s[0].secure);
  CHECK(fw.part(IwlImage::Regular, 1, s, 8) == 1 && s[0].device_offset == 0x00800000 && s[0].secure && s[0].data[11] == 0xB1);
  CHECK(fw.part(IwlImage::Regular, 2, s, 8) == 1 && s[0].data.size() == 16 && fw.part(IwlImage::Regular, 3, s, 8) == 0);
  CHECK(fw.part(IwlImage::Regular, 0, s, 1) == 1);
  CHECK(fw.part(IwlImage::Init, 0, s, 8) == 1 && s[0].data.size() == 4);
  CHECK(fw.command_version_count() == 2 && fw.command_version_of(1, 2) == 3 && fw.command_version_of(0, 5) == 6 && fw.command_version_of(9, 9) == 0 && fw.command_version(5).group == 0);
  CHECK(fw.image_loader().size() == 5 && fw.image_loader()[0] == 9);

  // not a firmware file, and files that are not whole
  Bytes bad = file;
  bad[4] ^= 1;
  CHECK(!fw.parse(ConstByteView(bad.data(), bad.size()), &error) && error);
  CHECK(!fw.parse(ConstByteView(file.data(), 20), &error));
  bad = file;
  bad[88 + 4] = 0xFF; bad[88 + 5] = 0xFF; bad[88 + 6] = 0xFF; bad[88 + 7] = 0x7F;           // the first TLV is longer than the file
  CHECK(!fw.parse(ConstByteView(bad.data(), bad.size()), &error));
  bad = file;
  bad.push_back(1);
  bad.push_back(2);                                                                          // two stray bytes after the last TLV
  CHECK(!fw.parse(ConstByteView(bad.data(), bad.size()), &error));
  Bytes no_runtime;
  put32(no_runtime, 0);
  put32(no_runtime, IwlFirmware::kMagic);
  no_runtime.insert(no_runtime.end(), 88 - 8, 0);
  tlv(no_runtime, 20, section(0, 4, 1));
  CHECK(!fw.parse(ConstByteView(no_runtime.data(), no_runtime.size()), &error));
  Bytes cpus = file;
  // NUM_OF_CPU of 3
  for (size_t i = 88; i + 12 <= cpus.size(); i += 4)
    if (cpus[i] == 27 && cpus[i + 4] == 4) { cpus[i + 8] = 3; break; }
  CHECK(!fw.parse(ConstByteView(cpus.data(), cpus.size()), &error));
  // a fixed length TLV with the wrong length
  Bytes wrong = file;
  for (size_t i = 88; i + 12 <= wrong.size(); i += 4)
    if (wrong[i] == 23 && wrong[i + 1] == 0 && wrong[i + 4] == 4) { wrong[i + 4] = 8; break; }
  CHECK(!fw.parse(ConstByteView(wrong.data(), wrong.size()), &error));
  // every prefix and every flipped bit: a verdict, no crash, no read past the file
  uint32_t accepted = 0;
  for (size_t cut = 0; cut <= file.size(); ++cut) accepted += fw.parse(ConstByteView(file.data(), cut), &error) ? 1 : 0;
  for (size_t i = 0; i < file.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes copy = file;
      copy[i] ^= static_cast<uint8_t>(1 << bit);
      accepted += fw.parse(ConstByteView(copy.data(), copy.size()), &error) ? 1 : 0;
    }
  CHECK(accepted > 0);
  // too many sections
  Bytes many;
  put32(many, 0);
  put32(many, IwlFirmware::kMagic);
  many.insert(many.end(), 88 - 8, 0);
  for (int i = 0; i < 70; ++i) tlv(many, 19, section(static_cast<uint32_t>(i) * 16, 4, 1));
  CHECK(!fw.parse(ConstByteView(many.data(), many.size()), &error));
}

// the real firmware file, when the environment has one
static void test_real_firmware() {
  const char* path = getenv("IWL_FIRMWARE_FILE");
  if (!path) { printf("  (IWL_FIRMWARE_FILE not set: no real firmware file tested)\n"); return; }
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  (cannot open %s)\n", path); return; }
  Bytes file;
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) file.insert(file.end(), chunk, chunk + n);
  fclose(f);
  TestAllocator alloc;
  IwlFirmware fw(alloc);
  const char* error = nullptr;
  CHECK(fw.parse(ConstByteView(file.data(), file.size()), &error));
  if (error) printf("  error: %s\n", error);
  printf("  %s: %zu bytes, \"%s\", API %u, build %u, flags 0x%x, %u cpus, %u section(s) in the runtime image, %u in init, paging %u, %u command versions, image loader %zu bytes\n", path,
         file.size(), fw.human_readable(), fw.api_version(), fw.build(), fw.flags(), fw.cpu_count(), static_cast<unsigned>(fw.sections(IwlImage::Regular).count()),
         static_cast<unsigned>(fw.sections(IwlImage::Init).count()), fw.paging_size(), fw.command_version_count(), static_cast<size_t>(fw.image_loader().size()));
  // current files leave the header's version at 0 (the API is in the file name); older ones carry it there
  CHECK(fw.api_version() == 0 || (fw.api_version() >= kIwl9000FirmwareApiMin && fw.api_version() <= kIwl9000FirmwareApiMax));
  printf("  firmware version %u.%u\n", fw.fw_major(), fw.fw_minor());
  CHECK(fw.sections(IwlImage::Regular).count() > 0);
  // sections lie inside the file, the chip's memory offsets are not huge
  for (uint64_t i = 0; i < fw.sections(IwlImage::Regular).count(); ++i) {
    const IwlSection& s = fw.sections(IwlImage::Regular)[i];
    CHECK(s.data.data() >= file.data() && s.data.data() + s.data.size() <= file.data() + file.size());
  }
  // damage
  uint32_t seed = 7;
  for (int round = 0; round < 300; ++round) {
    Bytes copy = file;
    for (int k = 0; k < 4; ++k) { seed = seed * 1664525u + 1013904223u; copy[(seed >> 4) % copy.size()] ^= static_cast<uint8_t>(1u << (seed & 7)); }
    seed = seed * 1664525u + 1013904223u;
    const size_t cut = round % 3 == 0 ? (seed >> 4) % copy.size() : copy.size();
    (void)fw.parse(ConstByteView(copy.data(), cut), &error);
  }
}

// ---- the chip's registers ---------------------------------------------------------------------------------------------------------------------------------------------------------

struct FakeIwl final : IIwlRegisters {
  uint32_t regs[0x1000 / 4] = {};
  uint32_t prph[0x100000 / 4] = {};
  uint32_t mem[0x1000] = {};
  uint32_t prph_addr_w = 0, prph_addr_r = 0, mem_w = 0, mem_r = 0;
  bool own_after_wake = false;       // PCI_OWN_SET sticks only once WAKE_ME has been set
  bool never_ready = false;
  bool clock_never = false;
  uint32_t last_prph_read_address = 0, last_prph_write_address = 0;
  uint32_t resets = 0;
  uint32_t reads_of_gp = 0;

  uint32_t read32(uint32_t offset) override {
    switch (offset) {
      case iwl_csr::kHbusPrphRdat: last_prph_read_address = prph_addr_r; return prph[(prph_addr_r & 0xFFFFF) / 4];
      case iwl_csr::kHbusMemRdat: return mem[(mem_r++) % 0x1000];
      case iwl_csr::kGpCntrl: ++reads_of_gp; return regs[offset / 4];
      default: return regs[(offset / 4) % 0x400];
    }
  }
  void write32(uint32_t offset, uint32_t value) override {
    switch (offset) {
      case iwl_csr::kHbusPrphRaddr: prph_addr_r = value; return;
      case iwl_csr::kHbusPrphWaddr: prph_addr_w = value; return;
      case iwl_csr::kHbusPrphWdat: last_prph_write_address = prph_addr_w; prph[(prph_addr_w & 0xFFFFF) / 4] = value; return;
      case iwl_csr::kHbusMemRaddr: mem_r = value / 4; return;
      case iwl_csr::kHbusMemWaddr: mem_w = value / 4; return;
      case iwl_csr::kHbusMemWdat: mem[(mem_w++) % 0x1000] = value; return;
      default: break;
    }
    uint32_t& r = regs[(offset / 4) % 0x400];
    r = value;
    if (offset == iwl_csr::kHwIfConfig) {
      if (never_ready || (own_after_wake && !(value & iwl_csr::kHwIfWakeMe))) r &= ~iwl_csr::kHwIfPciOwnSet;
    } else if (offset == iwl_csr::kGpCntrl) {
      if (!clock_never && (value & (iwl_csr::kGpInitDone | iwl_csr::kGpMacAccessReq))) r |= iwl_csr::kGpMacClockReady;
      else r &= ~iwl_csr::kGpMacClockReady;
    } else if (offset == iwl_csr::kReset) {
      if (value & iwl_csr::kResetStopMaster) r |= iwl_csr::kResetMasterDisabled;
      if (value & iwl_csr::kResetSwReset) ++resets;
    }
  }
};

static void test_pcie() {
  {
    FakeIwl chip;
    IwlPcieDevice dev(chip);
    chip.regs[iwl_csr::kHwRev / 4] = (0x31u << 4) | 1;
    chip.regs[iwl_csr::kHwRfId / 4] = 0x108u << 12;
    CHECK(dev.present() && dev.hw_rev() == ((0x31u << 4) | 1) && dev.hw_rf_id() == (0x108u << 12));
    char prefix[48];
    CHECK(iwl_firmware_prefix(dev.hw_rev(), dev.hw_rf_id(), prefix, sizeof prefix) && strcmp(prefix, "iwlwifi-9000-pu-b0-jf-b0") == 0);
    // the card is ours at once
    CHECK(dev.prepare_card_hw() && (chip.regs[iwl_csr::kHwIfConfig / 4] & iwl_csr::kHwIfPciOwnSet) && (chip.regs[iwl_csr::kMboxSet / 4] & iwl_csr::kMboxOsAlive));
  }
  {
    // ownership comes only after the retries with WAKE_ME
    FakeIwl chip;
    chip.own_after_wake = true;
    IwlPcieDevice dev(chip);
    CHECK(dev.prepare_card_hw() && (chip.regs[iwl_csr::kHwIfConfig / 4] & iwl_csr::kHwIfWakeMe) && (chip.regs[iwl_csr::kDbgLinkPwrMgmt / 4] & iwl_csr::kResetLinkPwrMgmtDisabled));
    FakeIwl stuck;
    stuck.never_ready = true;
    IwlPcieDevice dev2(stuck);
    CHECK(!dev2.prepare_card_hw());
    FakeIwl gone;
    for (uint32_t& r : gone.regs) r = 0xFFFFFFFFu;
    IwlPcieDevice dev3(gone);
    CHECK(!dev3.present());
  }
  {
    FakeIwl chip;
    IwlPcieDevice dev(chip);
    // the adapter's basic functions keep what the hardware set: set bits, never overwrite
    chip.regs[iwl_csr::kGioChickenBits / 4] = 0x00000011;
    chip.regs[iwl_csr::kHwIfConfig / 4] = 0x00000008;
    CHECK(dev.apm_init());
    CHECK(chip.regs[iwl_csr::kGioChickenBits / 4] == (0x00000011u | iwl_csr::kGioChickenL1aNoL0sRx) && chip.regs[iwl_csr::kDbgHpetMem / 4] == iwl_csr::kDbgHpetMemVal);
    CHECK((chip.regs[iwl_csr::kHwIfConfig / 4] & iwl_csr::kHwIfHapWake) && (chip.regs[iwl_csr::kHwIfConfig / 4] & 8) && (chip.regs[iwl_csr::kGpCntrl / 4] & iwl_csr::kGpInitDone));
    // the peripheral window: the address is masked to 20 bits and asks for a dword; grab the MAC for the access and give it back
    CHECK(dev.grab_nic_access() && (chip.regs[iwl_csr::kGpCntrl / 4] & iwl_csr::kGpMacAccessReq));
    dev.write_prph(0x00ABC340, 0xDEADBEEF);
    CHECK(chip.last_prph_write_address == (0x000BC340u | (3u << 24)) && dev.read_prph(0x00ABC340) == 0xDEADBEEF && chip.last_prph_read_address == (0x000BC340u | (3u << 24)));
    dev.set_prph_bits(0x00ABC340, 0x0F);
    dev.clear_prph_bits(0x00ABC340, 0xF0000000);
    CHECK(dev.read_prph(0x00ABC340) == 0x0EADBEEF);
    uint32_t words[3] = {1, 2, 3}, back[3] = {};
    dev.write_mem(0x400, words, 3);
    dev.read_mem(0x400, back, 3);
    CHECK(back[0] == 1 && back[1] == 2 && back[2] == 3);
    dev.release_nic_access();
    CHECK(!(chip.regs[iwl_csr::kGpCntrl / 4] & iwl_csr::kGpMacAccessReq));
    // stop: the master is stopped, the device reset, INIT_DONE gone
    dev.apm_stop();
    CHECK(chip.resets == 1 && !(chip.regs[iwl_csr::kGpCntrl / 4] & iwl_csr::kGpInitDone) && (chip.regs[iwl_csr::kReset / 4] & iwl_csr::kResetStopMaster));
    // interrupts off, acknowledged
    chip.regs[iwl_csr::kIntMask / 4] = 0x1234;
    dev.disable_interrupts();
    CHECK(chip.regs[iwl_csr::kIntMask / 4] == 0 && chip.regs[iwl_csr::kInt / 4] == 0xFFFFFFFFu && chip.regs[iwl_csr::kFhIntStatus / 4] == 0xFFFFFFFFu);
    // the radio switch
    chip.regs[iwl_csr::kGpCntrl / 4] = iwl_csr::kGpHwRfKillSw;
    CHECK(!dev.hw_rfkill());
    chip.regs[iwl_csr::kGpCntrl / 4] = 0;
    CHECK(dev.hw_rfkill());
  }
  {
    // a clock that never becomes ready: no activation and no access
    FakeIwl chip;
    chip.clock_never = true;
    IwlPcieDevice dev(chip);
    CHECK(!dev.activate_nic() && !dev.apm_init() && !dev.grab_nic_access());
  }
}

int main() {
  test_devices();
  test_firmware();
  test_real_firmware();
  test_pcie();
  printf(g_failed ? "IwlTest: %d FAILED\n" : "IwlTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
