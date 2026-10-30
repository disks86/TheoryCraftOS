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
#include <functional>
#include <map>
#include <sys/mman.h>
#include <vector>
#include "Hardware/WirelessInterface/Intel/IwlDevices.hpp"
#include "Hardware/WirelessInterface/Intel/IwlFirmware.hpp"
#include "Hardware/WirelessInterface/Intel/IwlFwLoad.hpp"
#include "Hardware/WirelessInterface/Intel/IwlRx.hpp"
#include "Hardware/WirelessInterface/Intel/IwlTx.hpp"
#include "Hardware/WirelessInterface/Intel/IwlTransport.hpp"
#include "Hardware/WirelessInterface/Intel/IwlNvm.hpp"
#include "Hardware/WirelessInterface/Intel/IwlPcie.hpp"

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
  uint32_t regs[0x2000 / 4] = {};
  uint32_t prph[0x100000 / 4] = {};
  uint32_t mem[0x1000] = {};
  uint32_t prph_addr_w = 0, prph_addr_r = 0, mem_w = 0, mem_r = 0;
  bool own_after_wake = false;       // PCI_OWN_SET sticks only once WAKE_ME has been set
  bool never_ready = false;
  bool clock_never = false;
  uint32_t last_prph_read_address = 0, last_prph_write_address = 0;
  uint32_t resets = 0;
  uint32_t reads_of_gp = 0;
  struct Transfer { uint32_t dst, bytes; bool extended; };
  std::vector<Transfer> transfers;
  std::map<uint32_t, uint8_t> device_memory;
  std::function<void()> on_alive, on_doorbell;
  bool force_radio = false;
  bool w1c = false, fail_transfers = false, cpu_released = false, alive_never = false, extended_during = false;

  uint32_t read32(uint32_t offset) override {
    switch (offset) {
      case iwl_csr::kHbusPrphRdat: last_prph_read_address = prph_addr_r; return prph[(prph_addr_r & 0xFFFFF) / 4];
      case iwl_csr::kHbusMemRdat: return mem[(mem_r++) % 0x1000];
      case iwl_csr::kGpCntrl: ++reads_of_gp; return regs[offset / 4] | (force_radio ? iwl_csr::kGpHwRfKillSw : 0);
      default: return regs[(offset / 4) % 0x800];
    }
  }
  void write32(uint32_t offset, uint32_t value) override {
    switch (offset) {
      case iwl_csr::kHbusPrphRaddr: prph_addr_r = value; return;
      case iwl_csr::kHbusPrphWaddr: prph_addr_w = value; return;
      case iwl_csr::kHbusPrphWdat:
        last_prph_write_address = prph_addr_w;
        prph[(prph_addr_w & 0xFFFFF) / 4] = value;
        if ((prph_addr_w & 0xFFFFF) == (iwl_prph::kReleaseCpuReset & 0xFFFFF) && (value & iwl_prph::kReleaseCpuResetBit)) cpu_released = true;
        if ((prph_addr_w & 0xFFFFF) == (iwl_prph::kLmpmChick & 0xFFFFF)) extended_during = value & 1;
        return;
      case iwl_csr::kHbusMemRaddr: mem_r = value / 4; return;
      case iwl_csr::kHbusMemWaddr: mem_w = value / 4; return;
      case iwl_csr::kHbusMemWdat: mem[(mem_w++) % 0x1000] = value; return;
      default: break;
    }
    if (offset == iwl_fh::tx_config(iwl_fh::kServiceChannel) && (value & iwl_fh::kTxChannelEnable)) {
      // the flow handler's DMA: bus address (the host pointer), byte count and device address from the registers
      const uint64_t bus = regs[iwl_fh::tfdib_ctrl0(9) / 4] | (static_cast<uint64_t>(regs[iwl_fh::tfdib_ctrl1(9) / 4] >> iwl_fh::kTfdibAddressShift) << 32);
      const uint32_t bytes = regs[iwl_fh::tfdib_ctrl1(9) / 4] & 0x0FFFFFFF, dst = regs[iwl_fh::service_sram_address(9) / 4];
      transfers.push_back({dst, bytes, extended_during});
      if (!fail_transfers) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(bus);
        for (uint32_t i = 0; i < bytes; ++i) device_memory[dst + i] = src[i];
        regs[iwl_csr::kFhIntStatus / 4] |= iwl_csr::kFhIntTxMask;
      }
    }
    if (offset == iwl_fh::kUcodeLoadStatus && value == 0xFFFFFFFFu && cpu_released && !alive_never) { regs[iwl_csr::kInt / 4] |= iwl_csr::kIntBitAlive; if (on_alive) on_alive(); }
    if (offset == iwl_tx::kHbusTargWrptr) { regs[offset / 4] = value; if (on_doorbell) on_doorbell(); return; }
    uint32_t& r = regs[(offset / 4) % 0x800];
    if (w1c && (offset == iwl_csr::kFhIntStatus || offset == iwl_csr::kInt)) { r &= ~value; return; }
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

// ---- loading the firmware ------------------------------------------------------------------------------------------------------------------------------------------------------

static Bytes make_loadable_firmware(uint32_t big_size) {
  Bytes f;
  put32(f, 0);
  put32(f, IwlFirmware::kMagic);
  f.insert(f.end(), 88 - 8, 0);
  tlv(f, 27, u32s({2}));
  tlv(f, 19, section(0x00000000, 16, 0xA1));
  tlv(f, 19, section(0x00040000, 8, 0xA2));                       // in the extended address range
  tlv(f, 19, section(0xFFFFCCCC, 0, 0));
  tlv(f, 24, section(0x00800000, big_size, 0xB1));                // a section of several chunks
  tlv(f, 19, section(0xAAAABBBB, 0, 0));
  tlv(f, 19, section(0x00000000, 16, 0xC1));                      // paging: not loaded
  return f;
}

static void test_load() {
  TestAllocator alloc;
  const Bytes file = make_loadable_firmware(0x20000 * 2 + 4100);
  IwlFirmware fw(alloc);
  const char* error = nullptr;
  CHECK(fw.parse(ConstByteView(file.data(), file.size()), &error));
  {
    FakeIwl chip;
    chip.w1c = true;
    IwlPcieDevice dev(chip);
    CHECK(dev.apm_init());
    IwlFwLoad load(dev, alloc);
    CHECK(load.load(fw, IwlImage::Regular, &error));
    if (error) printf("  load error: %s\n", error);
    CHECK(load.sections_loaded() == 3 && load.chunks_loaded() == 1 + 1 + 3);
    // the bytes landed at their device addresses; the paging section did not
    CHECK(chip.device_memory[0] == 0xA1 && chip.device_memory[15] == 0xA1 && chip.device_memory[16] == 0 && chip.device_memory[0x40000] == 0xA2 && chip.device_memory[0x40007] == 0xA2);
    CHECK(chip.device_memory[0x00800000] == 0xB1 && chip.device_memory[0x00800000 + 0x20000 * 2 + 4099] == 0xB1 && chip.device_memory.count(0x00800000 + 0x20000 * 2 + 4100) == 0);
    CHECK(chip.transfers.size() == 5 && chip.transfers[0].dst == 0 && chip.transfers[1].dst == 0x40000 && chip.transfers[1].extended && !chip.transfers[0].extended);
    CHECK(chip.transfers[2].dst == 0x00800000 && chip.transfers[2].bytes == 0x20000 && chip.transfers[3].dst == 0x00800000 + 0x20000 && chip.transfers[4].bytes == 4100);
    CHECK(chip.cpu_released && !(chip.prph[(iwl_prph::kLmpmChick & 0xFFFFF) / 4] & 1) && chip.prph[(iwl_prph::kWfpmGp2 & 0xFFFFF) / 4] == 0x01010101);
    // the ROM was told: the first processor's two sections (bits 0 and 1), then 0xFFFF, then the second's, then everything
    CHECK(chip.regs[iwl_fh::kUcodeLoadStatus / 4] == 0xFFFFFFFFu);
    CHECK(load.wait_alive(1000, &error) && !(chip.regs[iwl_csr::kInt / 4] & iwl_csr::kIntBitAlive));
  }
  {
    FakeIwl chip;                                                  // a transfer that never completes
    chip.w1c = true;
    chip.fail_transfers = true;
    IwlPcieDevice dev(chip);
    CHECK(dev.apm_init());
    IwlFwLoad load(dev, alloc);
    CHECK(!load.load(fw, IwlImage::Regular, &error) && error);
  }
  {
    FakeIwl chip;                                                  // the firmware never says it is alive
    chip.w1c = true;
    chip.alive_never = true;
    IwlPcieDevice dev(chip);
    CHECK(dev.apm_init());
    IwlFwLoad load(dev, alloc);
    CHECK(load.load(fw, IwlImage::Regular, &error) && !load.wait_alive(200, &error) && error);
    chip.regs[iwl_csr::kInt / 4] = iwl_csr::kIntBitSwError;
    CHECK(!load.wait_alive(200, &error));
  }
  {
    FakeIwl chip;                                                  // no MAC clock: nothing can be loaded
    chip.w1c = true;
    chip.clock_never = true;
    IwlPcieDevice dev(chip);
    IwlFwLoad load(dev, alloc);
    CHECK(!load.load(fw, IwlImage::Regular, &error));
    FakeIwl chip2;
    chip2.w1c = true;
    IwlPcieDevice dev2(chip2);
    CHECK(dev2.apm_init());
    IwlFwLoad load2(dev2, alloc);
    CHECK(!load2.load(fw, IwlImage::Wowlan, &error));              // an image the file does not have
    // a section that is not a whole number of dwords
    Bytes odd = make_loadable_firmware(13);
    IwlFirmware ofw(alloc);
    CHECK(ofw.parse(ConstByteView(odd.data(), odd.size()), &error));
    CHECK(!load2.load(ofw, IwlImage::Regular, &error));
  }
}

// ---- the receive queue ----------------------------------------------------------------------------------------------------------------------------------------------------------

struct CollectSink final : IIwlPacketSink {
  struct Seen { uint8_t command, group; uint16_t sequence; Bytes payload; };
  std::vector<Seen> seen;
  void on_packet(const IwlRxPacket& p) override {
    Seen s{p.command, p.group, p.sequence, Bytes()};
    for (uint64_t i = 0; i < p.payload.size(); ++i) s.payload.push_back(p.payload[i]);
    seen.push_back(s);
  }
};

// The chip's receive side: takes the posted buffers in order, fills them with frames, closes them.
struct FakeRxChip {
  FakeIwl& chip;
  uint32_t taken = 0, closed = 0;
  explicit FakeRxChip(FakeIwl& c) : chip(c) {}
  uint64_t prph64(uint32_t a) { return chip.prph[(a & 0xFFFFF) / 4] | (static_cast<uint64_t>(chip.prph[((a & 0xFFFFF) + 4) / 4]) << 32); }
  uint32_t write_pointer() { return chip.regs[0x1C80 / 4]; }
  // frame: len_n_flags(size = 4 + payload), command header, payload; padded to 64
  static void frame(Bytes& b, uint8_t group, uint8_t cmd, uint16_t seq, const Bytes& payload) {
    put32(b, static_cast<uint32_t>(4 + payload.size()));
    b.push_back(cmd); b.push_back(group); b.push_back(static_cast<uint8_t>(seq)); b.push_back(static_cast<uint8_t>(seq >> 8));
    b.insert(b.end(), payload.begin(), payload.end());
    while (b.size() % 64) b.push_back(0);
  }
  // returns false when the host has not posted a buffer
  bool deliver(const Bytes& content, bool end_marker = true) {
    if (((write_pointer() - taken) & 255) == 0) return false;
    const uint64_t* ring = reinterpret_cast<const uint64_t*>(prph64(0xA08000));
    const uint64_t entry = ring[taken % 256];
    const uint32_t vid = entry & 0xFFF;
    uint8_t* buffer = reinterpret_cast<uint8_t*>(entry & ~0xFFFull);
    memset(buffer, 0, 4096);
    memcpy(buffer, content.data(), content.size() < 4096 ? content.size() : 4096);
    if (end_marker && content.size() + 4 <= 4096) { const uint32_t m = iwl_frame::kInvalid; memcpy(buffer + content.size(), &m, 4); }
    uint32_t* used = reinterpret_cast<uint32_t*>(prph64(0xA08100));
    used[closed % 256] = vid;
    ++taken;
    ++closed;
    *reinterpret_cast<uint16_t*>(prph64(0xA08200)) = static_cast<uint16_t>(closed % 256);
    return true;
  }
};

static void test_rx() {
  TestAllocator alloc;
  FakeIwl chip;
  chip.w1c = true;
  IwlPcieDevice dev(chip);
  CHECK(dev.apm_init());
  IwlRxQueue rxq(dev, alloc);
  const char* error = nullptr;
  CHECK(rxq.init(true, &error));
  if (error) printf("  rx error: %s\n", error);
  FakeRxChip hw(chip);
  CHECK(chip.prph[0x09820 / 4] == (iwl_rfh::kDmaEnable | iwl_rfh::kDmaBufferSize4K | iwl_rfh::kDmaMinBuffer4And8 | iwl_rfh::kDmaDropTooLarge | iwl_rfh::kDmaRingSize256));
  CHECK(chip.prph[0x0980C / 4] == 0x10001u && (chip.prph[0x09800 / 4] & 3) == 3 && !(chip.prph[0x09800 / 4] & iwl_rfh::kGenCfgChunk128));
  CHECK(hw.write_pointer() == 248 && rxq.buffers_posted() == 248 && (chip.regs[iwl_rfh::kIntCoalescing / 4] & 0xFF) == iwl_rfh::kIntCoalescingDefault);
  const uint64_t* ring = reinterpret_cast<const uint64_t*>(hw.prph64(0xA08000));
  CHECK((ring[0] & 0xFFF) == 1 && (ring[247] & 0xFFF) == 248 && (ring[0] & ~0xFFFull) != 0);
  CollectSink sink;
  CHECK(rxq.poll(sink) == 0);
  // one buffer with three frames
  Bytes content;
  FakeRxChip::frame(content, 0, 0x01, 0x8000 | 5, Bytes{1, 2, 3, 4, 5});                 // a notification
  FakeRxChip::frame(content, 3, 0x40, 0x0007, Bytes(100, 0xEE));                         // a response of queue 0, index 7
  FakeRxChip::frame(content, 0, 0x02, 0x0400, Bytes());
  CHECK(hw.deliver(content));
  CHECK(rxq.poll(sink) == 3 && sink.seen.size() == 3);
  CHECK(sink.seen[0].command == 1 && sink.seen[0].group == 0 && sink.seen[0].sequence == 0x8005 && sink.seen[0].payload == Bytes({1, 2, 3, 4, 5}));
  CHECK(sink.seen[1].command == 0x40 && sink.seen[1].group == 3 && sink.seen[1].payload.size() == 100 && sink.seen[1].payload[99] == 0xEE);
  CHECK(sink.seen[2].payload.empty() && sink.seen[2].sequence == 0x0400);
  { IwlRxPacket p; p.sequence = 0x0407; CHECK(p.is_response() && p.queue() == 4 && p.index() == 7); p.sequence = 0x8001; CHECK(!p.is_response()); }
  // the buffer went back: 249 posted, the pointer moves in groups of eight
  CHECK(rxq.buffers_posted() == 249 && hw.write_pointer() == 248);
  for (int i = 0; i < 7; ++i) { CHECK(hw.deliver(content)); }
  CHECK(rxq.poll(sink) == 21 && rxq.buffers_posted() == 256 && hw.write_pointer() == 0);
  // lying frames: a size beyond the buffer, a size smaller than the header, the invalid marker in the middle
  sink.seen.clear();
  Bytes bad;
  put32(bad, 0x3FFF); bad.push_back(1); bad.push_back(0); bad.push_back(0); bad.push_back(0);
  CHECK(hw.deliver(bad));
  Bytes tiny;
  put32(tiny, 2); tiny.insert(tiny.end(), 8, 0);
  CHECK(hw.deliver(tiny));
  Bytes marked;
  FakeRxChip::frame(marked, 0, 9, 0x8000, Bytes{7});
  put32(marked, iwl_frame::kInvalid);
  FakeRxChip::frame(marked, 0, 10, 0x8000, Bytes{8});
  CHECK(hw.deliver(marked, false));
  CHECK(rxq.poll(sink) == 1 && sink.seen.size() == 1 && sink.seen[0].command == 9);
  // a full buffer of frames up to its last byte
  Bytes full;
  while (full.size() + 64 <= 4096) FakeRxChip::frame(full, 0, 11, 0x8000, Bytes(40, 1));
  CHECK(hw.deliver(full, false));
  sink.seen.clear();
  CHECK(rxq.poll(sink) == 64);
  // the chip names buffers that do not exist and a closed index beyond what it wrote
  {
    uint32_t* used = reinterpret_cast<uint32_t*>(hw.prph64(0xA08100));
    used[hw.closed % 256] = 0; used[(hw.closed + 1) % 256] = 0xFFF; used[(hw.closed + 2) % 256] = 300;
    hw.closed += 3;
    *reinterpret_cast<uint16_t*>(hw.prph64(0xA08200)) = static_cast<uint16_t>(hw.closed % 256);
    CHECK(rxq.poll(sink) == 0);
    *reinterpret_cast<uint16_t*>(hw.prph64(0xA08200)) = 0xFFFF;                       // an index with the high bits set: only 12 bits count
    (void)rxq.poll(sink);
  }
  // random buffers
  uint32_t seed = 99;
  for (int round = 0; round < 200; ++round) {
    Bytes junk(4096);
    for (auto& b : junk) { seed = seed * 1664525u + 1013904223u; b = static_cast<uint8_t>(seed >> 24); }
    if (round % 3 == 0) for (size_t i = 0; i < junk.size(); i += 64) { junk[i + 1] &= 0x3F; junk[i + 2] = 0; junk[i + 3] = 0; }
    *reinterpret_cast<uint16_t*>(hw.prph64(0xA08200)) = static_cast<uint16_t>(hw.closed % 256);
    if (!hw.deliver(junk, false)) break;
    (void)rxq.poll(sink);
  }
  rxq.release();
  CHECK(!rxq.ready() && alloc.dma.empty() && chip.prph[0x09820 / 4] == 0);
  // no memory
  struct Empty final : allocators::IAllocator {
    void* allocate(uint64_t) override { return nullptr; }
    void* allocate_aligned(uint64_t, uint64_t) override { return nullptr; }
    void* allocate_dma(uint64_t, uint64_t) override { return nullptr; }
    void deallocate(void*) override {}
  } none;
  IwlRxQueue starved(dev, none);
  CHECK(!starved.init(false, &error) && !starved.ready());
}

// ---- the command queue ------------------------------------------------------------------------------------------------------------------------------------------------------------

// The chip's transmit side for the command queue: reads the TFDs the host announced and puts their buffers together.
struct FakeTxChip {
  FakeIwl& chip;
  uint32_t read = 0;
  explicit FakeTxChip(FakeIwl& c) : chip(c) {}
  uint64_t ring_base() { return static_cast<uint64_t>(chip.regs[iwl_tx::cbbc_queue(0) / 4]) << 8; }
  std::vector<Bytes> process(bool* ok) {
    std::vector<Bytes> out;
    *ok = true;
    const uint32_t wr = chip.regs[iwl_tx::kHbusTargWrptr / 4] & 0xFF;
    while (read != wr) {
      const uint8_t* tfd = reinterpret_cast<const uint8_t*>(ring_base()) + read * 128;
      Bytes cmd;
      const uint32_t tbs = tfd[3] & 0x1F;
      if (tbs == 0 || tbs > 20) { *ok = false; }
      for (uint32_t i = 0; i < tbs && i < 20; ++i) {
        const uint8_t* tb = tfd + 4 + i * 6;
        uint32_t lo; memcpy(&lo, tb, 4);
        uint16_t hi_len; memcpy(&hi_len, tb + 4, 2);
        const uint64_t addr = lo | (static_cast<uint64_t>(hi_len & 0xF) << 32);
        const uint32_t len = hi_len >> 4;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(addr);
        cmd.insert(cmd.end(), p, p + len);
      }
      out.push_back(cmd);
      read = (read + 1) & 255;
    }
    return out;
  }
};

static void test_tx() {
  TestAllocator alloc;
  FakeIwl chip;
  chip.w1c = true;
  IwlPcieDevice dev(chip);
  CHECK(dev.apm_init());
  IwlCommandQueue q(dev, alloc);
  const char* error = nullptr;
  CHECK(q.init(&error));
  if (error) printf("  tx error: %s\n", error);
  FakeTxChip hw(chip);
  CHECK(hw.ring_base() != 0 && (hw.ring_base() & 255) == 0 && (chip.prph[iwl_tx::kScdTxFact & 0xFFFFF / 4 ? (iwl_tx::kScdTxFact & 0xFFFFF) / 4 : 0]) == 0);
  CHECK((chip.prph[(iwl_tx::kScdGpCtrl & 0xFFFFF) / 4] & (iwl_tx::kScdGpCtrlAutoActive | 1)) == (iwl_tx::kScdGpCtrlAutoActive | 1) && chip.regs[iwl_tx::kFhKeepWarm / 4] != 0);
  // every TFD in the ring points at the invalid block until used
  { const uint8_t* t = reinterpret_cast<const uint8_t*>(hw.ring_base()); CHECK(t[3] == 1 && t[255 * 128 + 3] == 1); }
  // before the scheduler is started: commands can be queued
  chip.prph[(iwl_tx::kScdSramBase & 0xFFFFF) / 4] = 0x800000;
  CHECK(q.start(&error));
  CHECK(q.scd_base() == 0x800000);
  CHECK(chip.prph[(iwl_tx::scd_queue_status(0) & 0xFFFFF) / 4] == (iwl_tx::kQueueStatusActive | iwl_tx::kCommandFifo | iwl_tx::kQueueStatusWsl | iwl_tx::kQueueStatusMask));
  CHECK(chip.prph[(iwl_tx::kScdEnCtrl & 0xFFFFF) / 4] == 1 && chip.prph[(iwl_tx::kScdTxFact & 0xFFFFF) / 4] == 0xFF);
  CHECK(chip.prph[(iwl_tx::kScdDramBase & 0xFFFFF) / 4] != 0);
  CHECK(chip.regs[iwl_fh::tx_config(3) / 4] == (iwl_fh::kTxChannelEnable | iwl_tx::kTcsrCreditEnable) && (chip.regs[iwl_tx::kFhTxChickenBits / 4] & 2));
  CHECK(chip.mem[((0x800000 + iwl_tx::scd_context_queue(0) + 4) / 4) % 0x1000] == (64u | (64u << 16)));
  // a short command: header 4 bytes, 6 bytes of data
  Bytes d1{1, 2, 3, 4, 5, 6};
  IwlHostCommand c1;
  c1.opcode = 0x40; c1.data = ConstByteView(d1.data(), d1.size());
  int32_t seq = q.enqueue(c1, &error);
  CHECK(seq == 0 && q.in_flight() == 1 && (chip.regs[iwl_tx::kHbusTargWrptr / 4] & 0xFF) == 1 && ((chip.regs[iwl_tx::kHbusTargWrptr / 4] >> 8) & 0xFF) == 0);
  bool ok = false;
  std::vector<Bytes> got = hw.process(&ok);
  CHECK(ok && got.size() == 1 && got[0] == Bytes({0x40, 0, 0, 0, 1, 2, 3, 4, 5, 6}));
  { const uint8_t* t = reinterpret_cast<const uint8_t*>(hw.ring_base()); CHECK(t[3] == 1); }   // one TB: the command fits the first 20 bytes
  // a long wide command
  Bytes d2(100);
  for (size_t i = 0; i < d2.size(); ++i) d2[i] = static_cast<uint8_t>(i);
  IwlHostCommand c2;
  c2.group = 3; c2.opcode = 0x07; c2.version = 2; c2.data = ConstByteView(d2.data(), d2.size());
  seq = q.enqueue(c2, &error);
  CHECK(seq == 1);
  got = hw.process(&ok);
  CHECK(ok && got.size() == 1 && got[0].size() == 108 && got[0][0] == 7 && got[0][1] == 3 && got[0][2] == 1 && got[0][3] == 0 && got[0][4] == 100 && got[0][5] == 0 && got[0][7] == 2 && got[0][8] == 0 && got[0][107] == 99);
  { const uint8_t* t = reinterpret_cast<const uint8_t*>(hw.ring_base()) + 128; CHECK(t[3] == 2); }
  // the queue fills at 31 commands
  CHECK(q.in_flight() == 2);
  int sent = 0;
  while (q.enqueue(c1, &error) >= 0) ++sent;
  CHECK(sent == 29 && q.in_flight() == 31 && error);
  // too large
  Bytes big(2000);
  IwlHostCommand c3; c3.opcode = 1; c3.data = ConstByteView(big.data(), big.size());
  CHECK(q.enqueue(c3, &error) < 0);
  // answers free the commands, bogus indexes change nothing
  q.reclaim(200);
  CHECK(q.in_flight() == 31);
  q.reclaim(5);
  CHECK(q.in_flight() == 25);
  q.reclaim(5);
  CHECK(q.in_flight() == 25);
  q.reclaim(30);
  CHECK(q.in_flight() == 0);
  // the ring wraps (the chip catches up first)
  (void)hw.process(&ok);
  for (int round = 0; round < 40; ++round) {
    const int32_t s = q.enqueue(c1, &error);
    CHECK(s >= 0);
    if (s < 0) break;
    got = hw.process(&ok);
    CHECK(ok && got.size() == 1 && got[0][2] == static_cast<uint8_t>(s) && got[0][3] == 0);
    q.reclaim(static_cast<uint32_t>(s) & 0xFF);
  }
  CHECK(q.in_flight() == 0);
  for (int round = 0; round < 300; ++round) {
    const int32_t s = q.enqueue(c1, &error);
    CHECK(s >= 0);
    if (s < 0) break;
    (void)hw.process(&ok);
    q.reclaim(static_cast<uint32_t>(s) & 0xFF);
  }
  CHECK(q.write_pointer() == (1 + 1 + 29 + 40 + 300) % 256);
  q.release();
  CHECK(!q.ready() && alloc.dma.empty() && q.enqueue(c1, &error) < 0);
}

// ---- bringing the firmware up -----------------------------------------------------------------------------------------------------------------------------------------------------

static Bytes alive_payload(uint16_t status) {
  Bytes p;
  p.push_back(static_cast<uint8_t>(status)); p.push_back(static_cast<uint8_t>(status >> 8)); p.push_back(0); p.push_back(0);
  put32(p, 46); put32(p, 1);
  p.insert(p.end(), 8, 0);
  put32(p, 0x1234);
  p.insert(p.end(), 16, 0);
  put32(p, 0x800000);
  p.insert(p.end(), 40, 0);
  return p;
}

struct NoteCollector final : IIwlNotificationHandler {
  std::vector<uint8_t> commands;
  void on_notification(const IwlRxPacket& p) override { commands.push_back(p.command); }
};

static void test_transport() {
  TestAllocator alloc;
  const Bytes file = make_loadable_firmware(64);
  IwlFirmware fw(alloc);
  const char* error = nullptr;
  CHECK(fw.parse(ConstByteView(file.data(), file.size()), &error));
  for (int scenario = 0; scenario < 5; ++scenario) {
    FakeIwl chip;
    chip.w1c = true;
    chip.force_radio = scenario != 3;
    chip.regs[iwl_csr::kHwRev / 4] = (0x31u << 4) | 0x3;
    chip.prph[(iwl_tx::kScdSramBase & 0xFFFFF) / 4] = 0x800000;
    IwlTransport tp(chip, alloc, true);
    FakeRxChip rxhw(chip);
    FakeTxChip txhw(chip);
    bool answer = scenario != 4;
    chip.on_alive = [&] {
      Bytes f;
      FakeRxChip::frame(f, 0, iwl_cmd::kAliveNotification, 0x8000, alive_payload(scenario == 2 ? 0xDEAD : 0xCAFE));
      rxhw.deliver(f);
    };
    chip.on_doorbell = [&] {
      bool ok = false;
      const std::vector<Bytes> cmds = txhw.process(&ok);
      if (!answer) return;
      for (const Bytes& c : cmds) {
        Bytes f;
        const uint16_t seq = static_cast<uint16_t>(c[2] | (c[3] << 8));
        FakeRxChip::frame(f, c[1], c[0], seq, Bytes{0xAA, c[0], static_cast<uint8_t>(c.size())});
        FakeRxChip::frame(f, 0, 0x77, 0x8000, Bytes{1});                              // and a notification after it
        rxhw.deliver(f);
      }
    };
    const bool started = tp.start(fw, IwlImage::Regular, 5000, &error);
    if (scenario == 2) { CHECK(!started && error && alloc.dma.empty() && !tp.running()); continue; }
    if (scenario == 3) { CHECK(!started && error && strstr(error, "radio")); continue; }
    CHECK(started && tp.running());
    if (error && !started) printf("  start error: %s\n", error);
    CHECK(tp.alive().valid && tp.alive().status == 0xCAFE && tp.alive().ucode_major == 46 && tp.alive().ucode_minor == 1 && tp.alive().scd_base == 0x800000 && tp.alive().error_event_table == 0x1234);
    // the MAC step and dash and the radio configuration went into the config register
    CHECK((chip.regs[iwl_csr::kHwIfConfig / 4] & 0xF) == 3);
    CHECK(chip.regs[iwl_csr::kMacShadowRegCtrl / 4] == iwl_csr::kMacShadowRegCtrlEnable);
    Bytes d{9, 8, 7};
    IwlHostCommand c;
    c.opcode = 0x21; c.data = ConstByteView(d.data(), d.size());
    uint8_t buf[16] = {};
    NoteCollector notes;
    if (scenario == 4) {
      CHECK(tp.send_sync(c, ByteView(buf, sizeof buf), 2000, &error) < 0 && error);    // nobody answers
      tp.stop();
      CHECK(alloc.dma.empty());
      continue;
    }
    int32_t n = tp.send_sync(c, ByteView(buf, sizeof buf), 2000, &error);
    CHECK(n == 3 && buf[0] == 0xAA && buf[1] == 0x21 && buf[2] == 7);
    // the notification that followed the response was left for the next poll
    CHECK(tp.poll(&notes) <= 1);
    n = tp.send_sync(c, ByteView(buf, 2), 2000, &error);                       // a response longer than the buffer: the length is still reported
    CHECK(n == 3 && buf[1] == 0x21);
    IwlHostCommand wide;
    wide.group = 4; wide.opcode = 0x02; wide.version = 1;
    CHECK(tp.send_sync(wide, ByteView(buf, sizeof buf), 2000, &error) == 3 && buf[2] == 8);
    tp.stop();
    CHECK(!tp.running() && alloc.dma.empty() && tp.send_sync(c, ByteView(buf, sizeof buf), 100, &error) < 0);
  }
}

// ---- the NVM and the MAC address ---------------------------------------------------------------------------------------------------------------------------------------------------

static void test_nvm() {
  uint8_t mac[6];
  CHECK(iwl_valid_ether_address(Bytes({0x00, 0x1B, 0x21, 1, 2, 3}).data()) && !iwl_valid_ether_address(Bytes({0x01, 0, 0, 0, 0, 1}).data()) && !iwl_valid_ether_address(Bytes(6, 0).data()));
  // the fused words: byte-reversed
  iwl_flip_hw_address(0x11223344, 0x0000AABB, mac);
  CHECK(mac[0] == 0x11 && mac[1] == 0x22 && mac[2] == 0x33 && mac[3] == 0x44 && mac[4] == 0xAA && mac[5] == 0xBB);
  // the override section wins when usable
  Bytes ov{0, 0, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
  CHECK(iwl_choose_mac_address(ConstByteView(ov.data(), ov.size()), 0x11223344, 0xAABB, mac) && mac[0] == 0x02 && mac[5] == 0x55);
  Bytes reserved{0, 0, 0x02, 0xcc, 0xaa, 0xff, 0xee, 0x00};                               // the reserved address: use the fused one
  CHECK(iwl_choose_mac_address(ConstByteView(reserved.data(), reserved.size()), 0x10223344, 0xAABB, mac) && mac[0] == 0x10);
  Bytes multicast{0, 0, 0x01, 1, 2, 3, 4, 5};
  CHECK(iwl_choose_mac_address(ConstByteView(multicast.data(), multicast.size()), 0x10223344, 0xAABB, mac) && mac[0] == 0x10);
  CHECK(!iwl_choose_mac_address(ConstByteView(), 0, 0, mac) && !iwl_choose_mac_address(ConstByteView(ov.data(), 5), 0, 0, mac));
  CHECK(!iwl_choose_mac_address(ConstByteView(), 0x01000000, 0, mac));                      // a multicast fused address
  {
    FakeIwl chip;
    IwlPcieDevice dev(chip);
    CHECK(dev.apm_init());
    chip.prph[(iwl_nvm::kWfmpMacAddr0 & 0xFFFFF) / 4] = 0x00A0C9F1;
    chip.prph[(iwl_nvm::kWfmpMacAddr1 & 0xFFFFF) / 4] = 0x00000A0B;
    CHECK(iwl_read_fused_address(dev, mac) && mac[0] == 0x00 && mac[1] == 0xA0 && mac[2] == 0xC9 && mac[3] == 0xF1 && mac[4] == 0x0A && mac[5] == 0x0B);
  }
  // requests and responses
  uint8_t req[8];
  iwl_nvm_read_request(12, 0x0102, 2048, req);
  CHECK(req[0] == 0 && req[1] == 0 && req[2] == 12 && req[3] == 0 && req[4] == 2 && req[5] == 1 && req[6] == 0 && req[7] == 8);
  auto response = [](uint16_t offset, uint16_t length, uint16_t type, uint16_t status, size_t data) {
    Bytes b;
    for (uint16_t v : {offset, length, type, status}) { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8)); }
    b.insert(b.end(), data, 0x5A);
    return b;
  };
  ConstByteView data;
  Bytes ok = response(100, 10, 3, 0, 10);
  CHECK(iwl_nvm_parse_response(ConstByteView(ok.data(), ok.size()), 3, 100, 2048, &data) && data.size() == 10);
  for (const Bytes& bad : {response(100, 10, 3, 1, 10), response(100, 10, 4, 0, 10), response(104, 10, 3, 0, 10), response(100, 3000, 3, 0, 3000), response(100, 10, 3, 0, 4), response(100, 10, 3, 0, 0)})
    CHECK(!iwl_nvm_parse_response(ConstByteView(bad.data(), bad.size()), 3, 100, 2048, &data));
  CHECK(!iwl_nvm_parse_response(ConstByteView(ok.data(), 6), 3, 100, 2048, &data));

  // reading sections through a running firmware
  TestAllocator alloc;
  const Bytes file = make_loadable_firmware(64);
  IwlFirmware fw(alloc);
  const char* error = nullptr;
  CHECK(fw.parse(ConstByteView(file.data(), file.size()), &error));
  for (int scenario = 0; scenario < 3; ++scenario) {
    FakeIwl chip;
    chip.w1c = true;
    chip.force_radio = true;
    chip.prph[(iwl_tx::kScdSramBase & 0xFFFFF) / 4] = 0x800000;
    IwlTransport tp(chip, alloc, true);
    FakeRxChip rxhw(chip);
    FakeTxChip txhw(chip);
    Bytes section(5000);
    for (size_t i = 0; i < section.size(); ++i) section[i] = static_cast<uint8_t>(i * 7);
    chip.on_alive = [&] { Bytes f; FakeRxChip::frame(f, 0, 1, 0x8000, alive_payload(0xCAFE)); rxhw.deliver(f); };
    chip.on_doorbell = [&] {
      bool good = false;
      for (const Bytes& c : txhw.process(&good)) {
        const uint16_t offset = static_cast<uint16_t>(c[8] | (c[9] << 8)), want = static_cast<uint16_t>(c[10] | (c[11] << 8));
        const uint16_t type = static_cast<uint16_t>(c[6] | (c[7] << 8));
        const uint16_t seq = static_cast<uint16_t>(c[2] | (c[3] << 8));
        size_t n = offset >= section.size() ? 0 : section.size() - offset;
        if (n > want) n = want;
        Bytes payload;
        for (uint16_t v : {offset, static_cast<uint16_t>(n), type, static_cast<uint16_t>(scenario == 1 ? 1 : 0)}) { payload.push_back(static_cast<uint8_t>(v)); payload.push_back(static_cast<uint8_t>(v >> 8)); }
        if (scenario == 2 && offset > 0) payload[0] ^= 1;                                  // the wrong offset comes back
        payload.insert(payload.end(), section.begin() + (offset < section.size() ? offset : section.size()), section.begin() + (offset < section.size() ? offset : section.size()) + n);
        Bytes f;
        FakeRxChip::frame(f, 0, 0x88, seq, payload);
        rxhw.deliver(f);
      }
    };
    CHECK(tp.start(fw, IwlImage::Regular, 5000, &error));
    ByteBuffer out(alloc);
    const bool read = iwl_read_nvm_section(tp, 12, out, &error);
    if (scenario == 0) {
      CHECK(read && out.size() == 5000);
      bool same = out.size() == 5000;
      for (size_t i = 0; same && i < 5000; ++i) same = out.view()[i] == section[i];
      CHECK(same);
    } else {
      CHECK(!read && error);
    }
    tp.stop();
  }
}

int main() {
  test_devices();
  test_firmware();
  test_real_firmware();
  test_pcie();
  test_load();
  test_rx();
  test_tx();
  test_transport();
  test_nvm();
  printf(g_failed ? "IwlTest: %d FAILED\n" : "IwlTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
