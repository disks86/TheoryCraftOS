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
#include <string>
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
#include "Hardware/WirelessInterface/Intel/IwlInit.hpp"
#include "Hardware/WirelessInterface/Intel/IwlDriver.hpp"
#include "Hardware/WirelessInterface/Intel/IwlScan.hpp"
#include "Hardware/WirelessInterface/Intel/IwlLink.hpp"
#include "Hardware/WirelessInterface/Intel/IwlWireless.hpp"
#include "Network/NetworkStack.hpp"
#include "Hardware/WirelessInterface/Ieee80211Frames.hpp"
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
  tlv(f, 48, Bytes{1, 2, 3, 4, 0, 5, 6, 7, 0x18, 1, 10, 0});     // CMD_VERSIONS: (cmd 1, group 2, ver 3, notif 4), (cmd 0, group 5, ver 6, notif 7) and (cmd 0x18, the long group 1, ver 10)
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
  CHECK(fw.command_version_count() == 3 && fw.command_version_of(0, 0x18) == 10 && fw.command_version_of(1, 0x18) == 10 && fw.command_version_of(2, 0x18) == 0 && fw.command_version_of(0, 0x19) == 0 && fw.command_version_of(2, 1) == 3 && fw.command_version_of(5, 0) == 6 && fw.command_version_of(9, 9) == 0 && fw.command_version(1).group == 5 && fw.command_version(1).command == 0);
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
  std::function<void(uint32_t)> on_queue_doorbell, on_ring_programmed;
  uint32_t queue_wrptr[32] = {};
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
    if (on_ring_programmed)
      for (uint32_t q = 0; q < 32; ++q)
        if (offset == iwl_tx::cbbc_queue(q)) on_ring_programmed(q);
    if (offset == iwl_tx::kHbusTargWrptr) {
      if (((value >> 8) & 0xFF) != 0) {                             // another queue's pointer: not the command queue's doorbell
        const uint32_t q = (value >> 8) & 0x1F;
        queue_wrptr[q] = value & 0xFF;
        if (on_queue_doorbell) on_queue_doorbell(q);
        return;
      }
      regs[offset / 4] = value;
      if (on_doorbell) on_doorbell();
      return;
    }
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
      if (value & iwl_csr::kResetSwReset) {                       // the device's queues start again from nothing
        ++resets;
        for (uint32_t& w : queue_wrptr) w = 0;
        regs[iwl_tx::kHbusTargWrptr / 4] = 0;
        regs[0x1C80 / 4] = 0;
      }
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

static Bytes make_loadable_firmware(uint32_t big_size, bool scan_tlvs = false) {
  Bytes f;
  put32(f, 0);
  put32(f, IwlFirmware::kMagic);
  f.insert(f.end(), 88 - 8, 0);
  tlv(f, 27, u32s({2}));
  if (scan_tlvs) {
    tlv(f, 29, u32s({0, 0x40000000}));                           // API 30 (station type)
    tlv(f, 29, u32s({1, 0x00000405}));                           // APIs 32, 34, 42
    tlv(f, 30, u32s({0, 0x00001000}));                           // capability 12 (DQA)
    tlv(f, 31, u32s({20}));                                      // 20 scan channel slots
    tlv(f, 48, Bytes{0x18, 0, 10, 2, 0x0D, 0, 8, 0, 0x17, 0, 1, 0, 0x1C, 0, 6, 5, 0x08, 0, 2, 0});   // ADD_STA 10, SCAN_REQ_UMAC 8, ADD_STA_KEY 1, TX_CMD 6, PHY_CONTEXT 2
  }
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
  uint32_t taken = 0, closed = 0, seen_resets = 0;
  explicit FakeRxChip(FakeIwl& c) : chip(c), seen_resets(c.resets) {}
  void sync() { if (seen_resets != chip.resets) { taken = closed = 0; seen_resets = chip.resets; } }
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
    sync();
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
  uint32_t queue = 0, seen_resets = 0;
  explicit FakeTxChip(FakeIwl& c, uint32_t q = 0) : chip(c), queue(q), seen_resets(c.resets) {}
  uint64_t ring_base() { return static_cast<uint64_t>(chip.regs[iwl_tx::cbbc_queue(queue) / 4]) << 8; }
  std::vector<Bytes> process(bool* ok) {
    std::vector<Bytes> out;
    *ok = true;
    if (seen_resets != chip.resets) { read = 0; seen_resets = chip.resets; }
    const uint32_t wr = queue == 0 ? (chip.regs[iwl_tx::kHbusTargWrptr / 4] & 0xFF) : chip.queue_wrptr[queue];
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
  Bytes big(2100);
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

// ---- NVM info, the init handshake and the driver ---------------------------------------------------------------------------------------------------------------------------------

static Bytes nvm_info(int version, uint32_t sku, uint32_t lar) {
  Bytes b;
  put32(b, 0);                                   // flags: OTP not empty
  b.push_back(0x34); b.push_back(0x12); b.push_back(7); b.push_back(2);     // nvm version 0x1234, board 7, 2 addresses
  put32(b, sku);
  put32(b, 3);                                   // tx chains
  put32(b, 3);                                   // rx chains
  put32(b, lar);
  if (version == 3) {
    for (int i = 0; i < 51; ++i) { const uint16_t f = (i % 5 == 4) ? 0 : 0x0309; b.push_back(static_cast<uint8_t>(f)); b.push_back(static_cast<uint8_t>(f >> 8)); }
    b.push_back(0); b.push_back(0);
  } else {
    const int room = version == 4 ? 110 : 115;
    put32(b, 51);
    for (int i = 0; i < room; ++i) put32(b, i < 51 ? ((i % 5 == 4) ? 0u : 0x0309u | (i >= 14 ? 0x10u : 0u)) : 0x0FFFFu);
  }
  return b;
}

static void test_nvm_info() {
  for (int version : {3, 4, 5}) {
    const Bytes info = nvm_info(version, 0x1F, 1);
    IwlNvmData d;
    const char* error = nullptr;
    CHECK(iwl_parse_nvm_get_info(ConstByteView(info.data(), info.size()), 3, 1, true, &d, &error));
    CHECK(d.nvm_version == 0x1234 && d.board_type == 7 && d.n_hw_addresses == 2 && !d.empty_otp && d.band_24ghz && d.band_5ghz && d.ieee80211n && d.ieee80211ac && d.ieee80211ax && !d.mimo_disabled);
    CHECK(d.tx_antennas == 3 && d.rx_antennas == 1 && d.lar_enabled && d.channel_count == 51);
    CHECK(d.channels[0].number == 1 && d.channels[0].frequency_mhz == 2412 && d.channels[13].number == 14 && d.channels[13].frequency_mhz == 2484 && !d.channels[0].is_5ghz);
    CHECK(d.channels[14].number == 36 && d.channels[14].frequency_mhz == 5180 && d.channels[14].is_5ghz && d.channels[50].number == 181);
    CHECK(!d.channels[4].valid() && d.channels[0].valid() && d.valid_channels() == 41);
    IwlNvmData nolar;
    CHECK(iwl_parse_nvm_get_info(ConstByteView(info.data(), info.size()), 3, 3, false, &nolar, &error) && !nolar.lar_enabled);
    // wrong sizes, every prefix, and bit flips: a verdict and no read past the end
    for (size_t cut = 0; cut <= info.size(); ++cut) {
      IwlNvmData t;
      const bool ok = iwl_parse_nvm_get_info(ConstByteView(info.data(), cut), 3, 3, true, &t, &error);
      CHECK(ok == (cut == 128 || cut == 468 || cut == 488));      // (the length is what tells the layouts apart)
    }
    for (size_t i = 0; i < info.size(); ++i)
      for (int bit = 0; bit < 8; ++bit) {
        Bytes copy = info;
        copy[i] ^= static_cast<uint8_t>(1 << bit);
        IwlNvmData t;
        (void)iwl_parse_nvm_get_info(ConstByteView(copy.data(), copy.size()), 3, 3, true, &t, &error);
      }
  }
  { // a count beyond the array is clamped
    Bytes info = nvm_info(4, 3, 0);
    info[20 + 4 + 0] = 0xFF; info[20 + 4 + 1] = 0xFF;
    IwlNvmData t;
    const char* error = nullptr;
    CHECK(iwl_parse_nvm_get_info(ConstByteView(info.data(), info.size()), 3, 3, true, &t, &error) && t.channel_count == iwl_nvm::kMaxChannels);
  }
  CHECK(iwl_nvm_channel_number(0) == 1 && iwl_nvm_channel_number(13) == 14 && iwl_nvm_channel_number(14) == 36 && iwl_nvm_channel_number(21) == 64 && iwl_nvm_channel_number(22) == 68 &&
        iwl_nvm_channel_number(29) == 96 && iwl_nvm_channel_number(30) == 100 && iwl_nvm_channel_number(41) == 144 && iwl_nvm_channel_number(42) == 149 && iwl_nvm_channel_number(50) == 181 &&
        iwl_nvm_channel_number(51) == 0);
  // the hardware address from the CSR block: the strap, else the OTP
  FakeIwl chip;
  IwlPcieDevice dev(chip);
  uint8_t mac[6];
  chip.regs[0x380 / 4] = 0x00A0C9F1; chip.regs[0x384 / 4] = 0x00000A0B;
  CHECK(iwl_read_csr_hw_address(dev, mac) && mac[0] == 0x00 && mac[1] == 0xA0 && mac[3] == 0xF1 && mac[5] == 0x0B);
  chip.regs[0x388 / 4] = 0x28D2447A; chip.regs[0x38C / 4] = 0x00000102;
  CHECK(iwl_read_csr_hw_address(dev, mac) && mac[0] == 0x28 && mac[1] == 0xD2 && mac[4] == 0x01 && mac[5] == 0x02);
  chip.regs[0x388 / 4] = 0x01000000; chip.regs[0x38C / 4] = 0;                                  // a multicast strap: the OTP
  CHECK(iwl_read_csr_hw_address(dev, mac) && mac[3] == 0xF1);
  chip.regs[0x380 / 4] = 0; chip.regs[0x384 / 4] = 0; chip.regs[0x388 / 4] = 0;
  CHECK(!iwl_read_csr_hw_address(dev, mac));
}

struct MemorySource final : IIwlFirmwareSource {
  Bytes file;
  const char* only = nullptr;           // the one name that exists
  std::vector<std::string> asked;
  bool read(const char* name, ByteBuffer& out) override {
    asked.push_back(name);
    if (!only || strcmp(name, only) != 0) return false;
    out.clear();
    return out.append(ConstByteView(file.data(), file.size()));
  }
};

static void test_driver() {
  TestAllocator alloc;
  MemorySource source;
  source.file = make_loadable_firmware(64);
  source.only = "iwlwifi-9000-pu-b0-jf-b0-40.ucode";
  for (int scenario = 0; scenario < 6; ++scenario) {
    FakeIwl chip;
    chip.w1c = true;
    chip.force_radio = true;
    chip.regs[iwl_csr::kHwRev / 4] = scenario == 5 ? (0x37u << 4) : ((0x31u << 4) | 0x1);
    chip.regs[iwl_csr::kHwRfId / 4] = 0x108u << 12;
    chip.prph[(iwl_tx::kScdSramBase & 0xFFFFF) / 4] = 0x800000;
    chip.regs[0x388 / 4] = 0x28D2447A; chip.regs[0x38C / 4] = 0x00000102;
    FakeRxChip rxhw(chip);
    FakeTxChip txhw(chip);
    chip.on_alive = [&] { Bytes f; FakeRxChip::frame(f, 0, 1, 0x8000, alive_payload(0xCAFE)); rxhw.deliver(f); };
    chip.on_doorbell = [&] {
      bool good = false;
      for (const Bytes& c : txhw.process(&good)) {
        const uint16_t seq = static_cast<uint16_t>(c[2] | (c[3] << 8));
        Bytes f;
        if (c[1] == 0x0C && c[0] == 0x02) {
          if (scenario == 4) FakeRxChip::frame(f, c[1], c[0], seq, Bytes(77, 0));                  // a response of no known layout
          else FakeRxChip::frame(f, c[1], c[0], seq, nvm_info(5, 0x1F, 1));
        } else {
          FakeRxChip::frame(f, c[1], c[0], seq, Bytes{0, 0, 0, 0});
          if (c[1] == 0x02 && c[0] == 0x03 && scenario != 3) FakeRxChip::frame(f, 2, 4, 0x8000, Bytes{1});  // INIT_COMPLETE (not in scenario 3)
        }
        rxhw.deliver(f);
      }
    };
    source.asked.clear();
    IwlDriver driver(alloc);
    const IwlDriver::Outcome r = driver.start_device(chip, true, source);
    if (scenario == 0 || scenario == 1) {
      CHECK(r == IwlDriver::Outcome::Started && driver.running());
      CHECK(strcmp(driver.firmware_name(), "iwlwifi-9000-pu-b0-jf-b0-40.ucode") == 0);
      CHECK(source.asked.size() == 7 && source.asked[0] == "iwlwifi-9000-pu-b0-jf-b0-46.ucode");             // newest first
      const IwlNvmData& n = driver.nvm();
      CHECK(n.hw_address[0] == 0x28 && n.hw_address[5] == 0x02 && n.channel_count == 51 && n.band_5ghz && n.nvm_version == 0x1234);
      driver.stop();
      CHECK(!driver.running() && alloc.dma.empty());
    } else if (scenario == 2) {
      CHECK(r == IwlDriver::Outcome::Started);                                                                   // (same, a second run on a fresh chip)
    } else if (scenario == 3) {
      CHECK(r == IwlDriver::Outcome::InitFailed && driver.message()[0] && alloc.dma.empty());               // no INIT_COMPLETE
    } else if (scenario == 4) {
      CHECK(r == IwlDriver::Outcome::InitFailed);
    } else {
      CHECK(r == IwlDriver::Outcome::UnsupportedHardware && source.asked.empty());
    }
  }
  { // no firmware file at all, and a broken one
    FakeIwl chip;
    chip.regs[iwl_csr::kHwRev / 4] = (0x32u << 4) | 0x1;
  }
}

// ---- runtime commands and scanning ------------------------------------------------------------------------------------------------------------------------------------------------

static void test_mvm_builders() {
  uint8_t buf[2048];
  // ADD_STA (48 bytes, laid out as the firmware header's offsets: sta id 16, type 35, queue mask 40)
  CHECK(iwl_build_add_aux_station(3, 2, ByteView(buf, sizeof buf)) == 48 && buf[0] == 0 && buf[2] == 0xFF && buf[3] == 0xFF && buf[4] == 4 && buf[16] == 3 && buf[35] == 4 && buf[40] == 2);
  CHECK(iwl_build_add_aux_station(3, 2, ByteView(buf, 47)) == 0);
  CHECK(iwl_build_scan_config(0, 3, 3, ByteView(buf, sizeof buf)) == 12 && buf[2] == 0 && buf[4] == 3 && buf[8] == 3);
  CHECK(iwl_build_tx_ant_config(3, ByteView(buf, sizeof buf)) == 4 && buf[0] == 3 && iwl_build_dqa_enable(0, ByteView(buf, 3)) == 0);
  // BT_CONFIG: the network mode and the high band retention module; TIME_EVENT_CMD: 36 bytes, the interface first, then the action, the type, the times and the policy
  CHECK(iwl_build_bt_config(ByteView(buf, sizeof buf)) == 8 && buf[0] == 1 && buf[4] == 0x10 && iwl_build_bt_config(ByteView(buf, 7)) == 0);
  IwlTimeEvent te;
  te.mac_id = 2;
  te.mac_color = 3;
  te.duration_tu = 500;
  CHECK(iwl_build_time_event(te, ByteView(buf, sizeof buf)) == 36 && buf[0] == 2 && buf[1] == 3 && buf[4] == 1 && buf[8] == 0 && buf[28] == (500 & 0xFF) && buf[29] == (500 >> 8) && buf[24] == 1 && buf[32] == 1 && buf[33] == 0 &&
        (buf[34] | (buf[35] << 8)) == 0x803 && iwl_build_time_event(te, ByteView(buf, 35)) == 0);
  te.action = iwl_mvm::kCtxtActionRemove;
  te.event = 0x1234;
  CHECK(iwl_build_time_event(te, ByteView(buf, sizeof buf)) == 36 && buf[4] == 3 && buf[8] == 0x34 && buf[9] == 0x12);
  uint32_t te_status, te_unique;
  const uint8_t te_response[16] = {1, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0};
  CHECK(iwl_parse_time_event_response(ConstByteView(te_response, 16), &te_status, &te_unique) && te_status == 1 && te_unique == 9 && !iwl_parse_time_event_response(ConstByteView(te_response, 10), &te_status, &te_unique));
  IwlErrorResponse er;
  const uint8_t error_response[12] = {0x20, 0, 0, 0, 0x18, 0, 0x34, 0x12, 5, 0, 0, 0};
  CHECK(iwl_parse_error_response(ConstByteView(error_response, 12), &er) && er.error_type == 0x20 && er.command == 0x18 && er.sequence == 0x1234 && er.service == 5 && !iwl_parse_error_response(ConstByteView(error_response, 11), &er));
  // the MAC context (148 bytes: node address 16, bssid 24, rates 32/36, filter 52, access categories from 60)
  IwlMacContext mc;
  for (int i = 0; i < 6; ++i) mc.address[i] = static_cast<uint8_t>(0xA0 + i);
  CHECK(iwl_build_station_mac_context(mc, ByteView(buf, sizeof buf)) == 148 && buf[4] == 1 && buf[8] == 5 && buf[16] == 0xA0 && buf[21] == 0xA5 && buf[24] == 0xFF && buf[32] == 0x0F && buf[36] == 0x15);
  CHECK((buf[52] & 0x44) == 0x44 && buf[60] == 15 && buf[62] == 0xFF && buf[63] == 3 && buf[64] == 7 && buf[65] == 1 && buf[100 + 8] == 0);
  CHECK(iwl_build_station_mac_context(mc, ByteView(buf, 147)) == 0);
  // the scan request (52 + 8 per slot + 1224 bytes)
  IwlScanChannelEntry ch[3] = {{1, false, true}, {36, true, true}, {52, true, false}};
  IwlScanRequest rq;
  rq.uid = 0x1234; rq.mac_id = 2; rq.firmware_channel_slots = 20; rq.channels = ch; rq.channel_count = 3;
  for (int i = 0; i < 6; ++i) rq.source_address[i] = static_cast<uint8_t>(0x10 + i);
  const uint32_t total = 52 + 8 * 20 + 1224;
  CHECK(iwl_scan_request_bytes(20) == total && iwl_build_scan_request(rq, ByteView(buf, sizeof buf)) == total);
  CHECK(buf[4] == 0x34 && buf[5] == 0x12 && buf[8] == 6 && buf[15] == 2);
  CHECK(((buf[12] | (buf[13] << 8)) & 0x2004) == 0x2004 && !((buf[12] | (buf[13] << 8)) & 0x8));                 // pass all, adaptive dwell, not passive
  CHECK(buf[16] == 10 && buf[19] == 2 && buf[20] == 10 && buf[21] == 2 && buf[22] == (300 & 0xFF) && buf[23] == (300 >> 8) && buf[40] == 6 && buf[44] == 110 && buf[49] == 3);
  CHECK(buf[52] == 1 && buf[56] == 1 && buf[57] == 1 && buf[58] == 1);                                    // channel 1: probes (ssid bitmap 1), band 2.4 GHz (1), one iteration
  CHECK(buf[60] == 1 && buf[64] == 36 && buf[65] == 0 && buf[68] == 0 && buf[72] == 52 && buf[76] == 0);    // channel 36 probes (5 GHz band 0), channel 52 only listens
  CHECK(buf[52 + 8 * 3 + 4] == 0);                                                                         // an unused slot is empty
  const uint32_t tail = 52 + 8 * 20;
  CHECK(buf[tail + 2] == 1 && buf[tail + 6] == 0);                                                          // schedule: one iteration
  const uint32_t preq = tail + 12;
  CHECK(buf[preq + 2] == 26 && buf[preq + 3] == 0);                                                         // mac header segment: 24 + the wildcard SSID element
  const uint32_t pbuf = preq + 20;
  CHECK(buf[pbuf] == 0x40 && buf[pbuf + 4] == 0xFF && buf[pbuf + 10] == 0x10 && buf[pbuf + 15] == 0x15 && buf[pbuf + 24] == 0 && buf[pbuf + 25] == 0);
  // passive: the PASSIVE flag and no probing channels
  rq.active = false;
  CHECK(iwl_build_scan_request(rq, ByteView(buf, sizeof buf)) == total && (buf[12] & 0x08) && buf[52] == 0 && buf[60] == 0);
  // refusals: no channels, too many, no room
  rq.channel_count = 0;
  CHECK(iwl_build_scan_request(rq, ByteView(buf, sizeof buf)) == 0);
  rq.channel_count = 21;
  CHECK(iwl_build_scan_request(rq, ByteView(buf, sizeof buf)) == 0);
  rq.channel_count = 3;
  CHECK(iwl_build_scan_request(rq, ByteView(buf, total - 1)) == 0);
  // notifications
  IwlScanComplete sc;
  const uint8_t done[16] = {0x34, 0x12, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(iwl_parse_scan_complete(ConstByteView(done, sizeof done), &sc) && sc.uid == 0x1234 && sc.completed() && sc.ebs_status == 0);
  CHECK(!iwl_parse_scan_complete(ConstByteView(done, 5), &sc));
}

static Bytes rx_mpdu(const Bytes& frame, uint8_t channel, uint8_t energy, bool crc_ok, bool decrypted = false) {
  Bytes b;
  b.push_back(static_cast<uint8_t>(frame.size())); b.push_back(static_cast<uint8_t>(frame.size() >> 8));   // mpdu_len
  b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0);
  b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0);                                     // csum, l3l4
  put32(b, (crc_ok ? 3u : 0u) | (decrypted ? ((2u << 8) | (1u << 11)) : 0u));                                // status
  put32(b, 0);                                                                                        // reorder
  put32(b, 0); put32(b, 0); put32(b, 0);
  b.push_back(energy); b.push_back(0); b.push_back(channel); b.push_back(0);
  put32(b, 0); put32(b, 0); put32(b, 0);
  b.insert(b.end(), frame.begin(), frame.end());
  return b;
}

static Bytes beacon(const char* name, uint8_t last_octet, bool wpa2, const uint8_t* bssid = nullptr) {
  Bytes f{0x80, 0x00, 0, 0};
  f.insert(f.end(), 6, 0xFF);
  for (int i = 0; i < 2; ++i) {
    if (bssid) f.insert(f.end(), bssid, bssid + 6);
    else f.insert(f.end(), {0x02, 0x11, 0x22, 0x33, 0x44, last_octet});
  }
  f.insert(f.end(), {0, 0});
  f.insert(f.end(), 8, 0);
  f.insert(f.end(), {0x64, 0});
  f.push_back(0x11); f.push_back(0x04);
  f.push_back(0); f.push_back(static_cast<uint8_t>(strlen(name)));
  f.insert(f.end(), name, name + strlen(name));
  if (wpa2) {
    const uint8_t rsn[] = {48, 20, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 2, 0, 0};
    f.insert(f.end(), rsn, rsn + sizeof rsn);
  }
  return f;
}

static void test_scanner() {
  TestAllocator alloc;
  const Bytes file = make_loadable_firmware(64, true);
  IwlFirmware fw(alloc);
  const char* error = nullptr;
  CHECK(fw.parse(ConstByteView(file.data(), file.size()), &error));
  CHECK(fw.scan_channels() == 20 && fw.command_version_of(0, 0x18) == 10 && fw.has_api(30) && fw.has_api(34) && fw.has_api(42) && fw.has_capability(12));
  for (int scenario = 0; scenario < 4; ++scenario) {
    FakeIwl chip;
    chip.w1c = true;
    chip.force_radio = true;
    chip.prph[(iwl_tx::kScdSramBase & 0xFFFFF) / 4] = 0x800000;
    IwlTransport tp(chip, alloc, true);
    FakeRxChip rxhw(chip);
    FakeTxChip txhw(chip);
    std::vector<std::pair<uint8_t, uint8_t>> seen;                  // (group, opcode) in order
    std::vector<Bytes> scan_requests;
    chip.on_alive = [&] { Bytes f; FakeRxChip::frame(f, 0, 1, 0x8000, alive_payload(0xCAFE)); rxhw.deliver(f); };
    chip.on_doorbell = [&] {
      bool good = false;
      for (const Bytes& c : txhw.process(&good)) {
        const uint8_t group = c[1], op = c[0];
        const uint16_t seq = static_cast<uint16_t>(c[2] | (c[3] << 8));
        seen.push_back({group, op});
        Bytes f;
        if (group == 0 && op == 0x18) {
          FakeRxChip::frame(f, group, op, seq, Bytes{scenario == 1 ? uint8_t(2) : uint8_t(1), 0, 0, 0});     // ADD_STA status (2 = failure in scenario 1)
        } else if (group == 1 && op == 0x0D) {
          scan_requests.push_back(c);
          FakeRxChip::frame(f, group, op, seq, Bytes{1, 0, 0, 0, 0, 0, 0, 0});
          rxhw.deliver(f);
          f.clear();
          const uint32_t uid = static_cast<uint32_t>(c[8 + 4] | (c[8 + 5] << 8));
          // two beacons of one network (a stronger one second), another, a damaged one, a hidden one; then the end of the scan
          Bytes g;
          FakeRxChip::frame(g, 0, 0xC1, 0x8000, rx_mpdu(beacon("Home", 1, true), 6, 80, true));
          FakeRxChip::frame(g, 0, 0xC1, 0x8000, rx_mpdu(beacon("Cafe", 2, false), 11, 70, true));
          FakeRxChip::frame(g, 0, 0xC1, 0x8000, rx_mpdu(beacon("Home", 1, true), 6, 55, true));
          FakeRxChip::frame(g, 0, 0xC1, 0x8000, rx_mpdu(beacon("Bad", 3, false), 1, 50, false));
          FakeRxChip::frame(g, 0, 0xC1, 0x8000, rx_mpdu(beacon("", 4, false), 1, 50, true));
          rxhw.deliver(g);
          Bytes h;
          Bytes other{9, 9, 9, 9, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};                                     // another scan's end: ignored
          FakeRxChip::frame(h, 0, 0x0F, 0x8000, other);
          rxhw.deliver(h);
          if (scenario != 3) {                                                                           // (scenario 3: it never ends)
            Bytes e;
            Bytes end{static_cast<uint8_t>(uid), static_cast<uint8_t>(uid >> 8), 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            FakeRxChip::frame(e, 0, 0x0F, 0x8000, end);
            rxhw.deliver(e);
          }
          continue;
        } else {
          FakeRxChip::frame(f, group, op, seq, Bytes{0, 0, 0, 0});
        }
        rxhw.deliver(f);
      }
    };
    CHECK(tp.start(fw, IwlImage::Regular, 5000, &error));
    IwlNvmData nvm;
    nvm.band_24ghz = nvm.band_5ghz = true;
    nvm.tx_antennas = nvm.rx_antennas = 3;
    for (int i = 0; i < 6; ++i) nvm.hw_address[i] = static_cast<uint8_t>(0x28 + i);
    for (uint32_t i = 0; i < 51; ++i) {
      IwlChannel& c = nvm.channels[i];
      c.number = iwl_nvm_channel_number(i); c.is_5ghz = i >= 14; c.frequency_mhz = iwl_channel_frequency(c.number, c.is_5ghz);
      c.flags = (i % 3 == 2) ? 0 : (iwl_nvm::kChannelValid | iwl_nvm::kChannelActive | (c.number >= 52 && c.number <= 64 ? iwl_nvm::kChannelRadar : 0u));
    }
    nvm.channel_count = 51;
    IwlScanner sc(tp, alloc);
    const bool up = sc.bring_up(fw, nvm, &error);
    if (scenario == 1) {
      CHECK(!up && error && !sc.ready());
      continue;
    }
    CHECK(up && sc.ready());
    if (!up) printf("  bring_up: %s\n", error ? error : "?");
    // the commands went in the order of iwl_mvm_up: antennas, Bluetooth coexistence, DQA, the auxiliary station, the scan configuration, the interface
    CHECK(seen.size() >= 6);
    if (seen.size() >= 6) CHECK(seen[seen.size() - 6] == std::make_pair(uint8_t(0), uint8_t(0x98)) && seen[seen.size() - 5] == std::make_pair(uint8_t(0), uint8_t(0x9B)) && seen[seen.size() - 4] == std::make_pair(uint8_t(5), uint8_t(0)) &&
                                seen[seen.size() - 3] == std::make_pair(uint8_t(0), uint8_t(0x18)) && seen[seen.size() - 2] == std::make_pair(uint8_t(1), uint8_t(0x0C)) && seen[seen.size() - 1] == std::make_pair(uint8_t(0), uint8_t(0x28)));
    // the auxiliary queue was enabled on its FIFO and chained
    CHECK(chip.prph[(iwl_tx::scd_queue_status(1) & 0xFFFFF) / 4] == (iwl_tx::kQueueStatusActive | iwl_tx::kMcastFifo | iwl_tx::kQueueStatusWsl | iwl_tx::kQueueStatusMask));
    CHECK(chip.prph[(iwl_tx::kScdQueueChainSel & 0xFFFFF) / 4] & 2);
    CHECK(chip.regs[iwl_tx::cbbc_queue(1) / 4] != 0 && chip.regs[iwl_tx::cbbc_queue(1) / 4] != chip.regs[iwl_tx::cbbc_queue(0) / 4]);
    const bool started = sc.start_scan(true, &error);
    CHECK(started);
    if (!started) printf("  start_scan: %s\n", error ? error : "?");
    if (scenario == 3) CHECK(!sc.start_scan(true, &error));                                                // (the others finish inside the call)
    // the request: 20 slots, the channels the NVM allowed and the band flags
    CHECK(scan_requests.size() == 1 && scan_requests[0].size() == 8 + 52 + 8 * 20 + 1224);
    if (!scan_requests.empty()) {
      const Bytes& r = scan_requests[0];
      CHECK(r[1] == 1 && r[0] == 0x0D && r[8 + 3] == 0 && r[8 + 8] == 6);                                    // wide header, priority
      const uint32_t channels = r[8 + 49];
      CHECK(channels > 0 && channels <= 20);
      CHECK(r[8 + 52 + 4] == 1 || r[8 + 52 + 4] == 36 || r[8 + 52 + 4] == 2);                               // the first valid channel (channel 3 is invalid: i % 3 == 2)
    }
    uint32_t guard = 0;
    while (sc.poll() && guard++ < 100) {}
    if (scenario == 3) {
      CHECK(sc.scanning() && sc.network_count() == 2);                                                         // frames are in, the end never came
      CHECK(!sc.start_scan(true, &error));
    } else {
      CHECK(!sc.scanning() && sc.last_scan_completed() && sc.frames_seen() == 4);
      WifiNetwork nets[8];
      const uint32_t n = sc.results(ArrayView<WifiNetwork>(nets, 8));
      CHECK(n == 2 && strcmp(nets[0].ssid, "Home") == 0 && nets[0].security == WifiSecurity::Wpa2 && nets[0].channel == 6);
      CHECK(n == 2 && strcmp(nets[1].ssid, "Cafe") == 0 && nets[1].security == WifiSecurity::Wep && nets[1].channel == 11);
      CHECK(n == 2 && nets[0].signal_percent >= nets[1].signal_percent);
      // a second scan works again
      CHECK(sc.start_scan(false, &error) && scan_requests.size() == 2 && (scan_requests[1][8 + 12] & 0x08));
    }
    sc.shut_down();
    tp.stop();
    CHECK(alloc.dma.empty());
  }
  { // firmware that speaks other versions is refused
    const Bytes plain = make_loadable_firmware(64, false);
    IwlFirmware fw2(alloc);
    CHECK(fw2.parse(ConstByteView(plain.data(), plain.size()), &error));
    FakeIwl chip;
    chip.w1c = true; chip.force_radio = true;
    chip.prph[(iwl_tx::kScdSramBase & 0xFFFFF) / 4] = 0x800000;
    IwlTransport tp(chip, alloc, true);
    FakeRxChip rxhw(chip);
    FakeTxChip txhw(chip);
    chip.on_alive = [&] { Bytes f; FakeRxChip::frame(f, 0, 1, 0x8000, alive_payload(0xCAFE)); rxhw.deliver(f); };
    CHECK(tp.start(fw2, IwlImage::Regular, 5000, &error));
    IwlScanner sc(tp, alloc);
    IwlNvmData nvm;
    CHECK(!sc.bring_up(fw2, nvm, &error) && error && !sc.start_scan(true, &error));
    tp.stop();
  }
}

// ---- the station link: contexts, transmit commands, keys, received data ------------------------------------------------------------------------------------------------------------

static void test_link_builders() {
  uint8_t buf[300];
  CHECK(iwl_build_phy_context(1, 11, false, 3, 3, ByteView(buf, sizeof buf)) == 36 && buf[4] == 1 && buf[16] == 1 && buf[17] == 11 && buf[18] == 0 && buf[20] == 3);
  CHECK(buf[24] == 6 && (buf[25] & 0x0C) == 0x08 && (buf[25] & 0x30) == 0x20);                            // valid chains, two in use
  CHECK(iwl_build_phy_context(1, 36, true, 3, 3, ByteView(buf, sizeof buf)) == 36 && buf[16] == 0 && buf[17] == 36 && iwl_build_phy_context(1, 1, false, 1, 1, ByteView(buf, 35)) == 0);
  CHECK(iwl_build_binding(1, 0, 2, 3, ByteView(buf, sizeof buf)) == 24 && buf[4] == 1 && buf[8] == 2 && buf[9] == 3 && buf[12] == 0xFF && buf[19] == 0xFF && buf[20] == 0);
  IwlStationSpec st;
  st.station_id = 1; st.queue_mask = 0x10; st.association_id = 5; st.modify = true;
  for (int i = 0; i < 6; ++i) st.address[i] = static_cast<uint8_t>(0xA0 + i);
  CHECK(iwl_build_add_station(st, ByteView(buf, sizeof buf)) == 48 && buf[0] == 1 && buf[2] == 0xFF && buf[8] == 0xA0 && buf[16] == 1 && buf[35] == 0 && buf[36] == 5 && buf[40] == 0x10 && buf[24] == 0x00);
  CHECK((buf[27] & 0x30) == 0x30 && (buf[26] & 0x0C) == 0x00);                                            // flags mask: the 20 MHz / MIMO fields and RTS protection (bit 17)
  uint8_t key[16], rsc[8] = {1, 2, 3, 4, 5, 6, 0, 0};
  for (int i = 0; i < 16; ++i) key[i] = static_cast<uint8_t>(0x60 + i);
  CHECK(iwl_build_station_key(1, 1, 2, true, key, rsc, ByteView(buf, sizeof buf)) == 64 && buf[0] == 1 && buf[1] == 1 && buf[2] == (2 | 8) && buf[3] == (2 | 0x40) && buf[4] == 0x60 && buf[19] == 0x6F && buf[20] == 0 && buf[36] == 1 && buf[41] == 6);
  CHECK(iwl_build_station_key(1, 0, 0, false, key, rsc, ByteView(buf, sizeof buf)) == 64 && (buf[3] & 0x40) == 0);
  // the transmit command
  Bytes hdr(24, 0);
  hdr[0] = 0xB0; hdr[4] = 0xA0; hdr[10] = 0x28;
  IwlTxParams tp;
  tp.header = ConstByteView(hdr.data(), 24); tp.frame_bytes = 30; tp.station_id = 1; tp.tx_antennas = 3;
  CHECK(iwl_build_tx_prefix(tp, ByteView(buf, sizeof buf)) == 84 && buf[0] == 0x1C && buf[4] == 30 && buf[6] == 0 && buf[7] == 12 && buf[8] == 0x08 && buf[9] == 0x20);
  CHECK(buf[16] == 10 && buf[17] == 0x42 && buf[18] == 0x00 && buf[20] == 1 && buf[21] == 0 && buf[44] == 0xFF && buf[47] == 0xFF && buf[60] == 0xB0 && buf[64] == 0xA0);
  CHECK(buf[4 + 52] == 2 && buf[4 + 49] == 60 && buf[4 + 50] == 3 && buf[4 + 51] == 8);                    // pm_frame_timeout (management), retries, tid                    // pm_frame_timeout (management), retries, tid
  tp.five_ghz = true; tp.association_request = true;
  CHECK(iwl_build_tx_prefix(tp, ByteView(buf, sizeof buf)) == 84 && buf[16] == 13 && buf[17] == 0x40 && buf[4 + 52] == 3);
  tp.data = true; tp.protect = true; tp.association_request = false; tp.frame_bytes = 100;
  for (int i = 0; i < 16; ++i) tp.tk[i] = static_cast<uint8_t>(0x10 + i);
  CHECK(iwl_build_tx_prefix(tp, ByteView(buf, sizeof buf)) == 84 && buf[4 + 17] == 2 && buf[4 + 20] == 0x10 && buf[4 + 35] == 0x1F && buf[7] == 16 && buf[16] == 9 && buf[4 + 52] == 0 && buf[4 + 50] == 15);
  CHECK(iwl_build_tx_prefix(tp, ByteView(buf, 83)) == 0);
  IwlTxParams bad; bad.header = ConstByteView(hdr.data(), 10);
  CHECK(iwl_build_tx_prefix(bad, ByteView(buf, sizeof buf)) == 0);
  // the response
  Bytes resp(48, 0);
  resp[0] = 1; resp[40] = 1; resp[44] = 0x07; resp[45] = 0xF0;
  IwlTxResponse tr;
  CHECK(iwl_parse_tx_response(ConstByteView(resp.data(), resp.size()), &tr) && tr.frame_count == 1 && tr.success() && tr.next_index == 7);
  CHECK(!iwl_parse_tx_response(ConstByteView(resp.data(), 44), &tr));
  resp[0] = 0;
  CHECK(!iwl_parse_tx_response(ConstByteView(resp.data(), resp.size()), &tr));
  // Ethernet <-> 802.11
  const uint8_t bssid[6] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, us[6] = {0x28, 1, 2, 3, 4, 5};
  Bytes eth{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x28, 1, 2, 3, 4, 5, 0x08, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
  uint8_t h[24], body[100];
  uint32_t n = iwl_ethernet_to_data_frame(ConstByteView(eth.data(), eth.size()), bssid, us, true, 0x010203040506ull, 0, ByteView(h, 24), ByteView(body, sizeof body));
  CHECK(n == 8 + 6 + 2 + 4 && h[0] == 0x08 && h[1] == 0x41 && h[4] == 0xA0 && h[10] == 0x28 && h[16] == 0x11 && body[0] == 0x06 && body[1] == 0x05 && body[3] == 0x20 && body[4] == 0x04 && body[7] == 0x01 && body[8] == 0xAA && body[14] == 0x08 && body[16] == 0xDE);
  n = iwl_ethernet_to_data_frame(ConstByteView(eth.data(), eth.size()), bssid, us, false, 0, 0, ByteView(h, 24), ByteView(body, sizeof body));
  CHECK(n == 6 + 2 + 4 && h[1] == 0x01 && body[0] == 0xAA);
  CHECK(iwl_ethernet_to_data_frame(ConstByteView(eth.data(), 13), bssid, us, false, 0, 0, ByteView(h, 24), ByteView(body, sizeof body)) == 0);
  // a frame from the access point to us becomes an Ethernet frame (address 3 is the source)
  Bytes from_ap{0x08, 0x02, 0, 0, 0x28, 1, 2, 3, 4, 5, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0, 0, 0xAA, 0xAA, 3, 0, 0, 0, 0x08, 0x06, 0x01, 0x02};
  uint8_t out[100];
  n = iwl_data_frame_to_ethernet(ConstByteView(from_ap.data(), from_ap.size()), false, ByteView(out, sizeof out));
  CHECK(n == 14 + 2 && out[0] == 0x28 && out[6] == 0x77 && out[11] == 0xCC && out[12] == 0x08 && out[13] == 0x06 && out[14] == 1);
  Bytes prot = from_ap; prot[1] |= 0x40;
  CHECK(iwl_data_frame_to_ethernet(ConstByteView(prot.data(), prot.size()), false, ByteView(out, sizeof out)) == 0);        // protected and not decrypted: unreadable
  prot.insert(prot.begin() + 24, {1, 0, 0, 0x20, 0, 0, 0, 0});
  CHECK(iwl_data_frame_to_ethernet(ConstByteView(prot.data(), prot.size()), true, ByteView(out, sizeof out)) == 16);
  Bytes mgmt = from_ap; mgmt[0] = 0x80;
  CHECK(iwl_data_frame_to_ethernet(ConstByteView(mgmt.data(), mgmt.size()), false, ByteView(out, sizeof out)) == 0);
  Bytes to_ds = from_ap; to_ds[1] = 0x01;
  CHECK(iwl_data_frame_to_ethernet(ConstByteView(to_ds.data(), to_ds.size()), false, ByteView(out, sizeof out)) == 0);
  CHECK(iwl_data_frame_to_ethernet(ConstByteView(from_ap.data(), 30), false, ByteView(out, sizeof out)) == 0);
  CHECK(iwl_data_frame_to_ethernet(ConstByteView(from_ap.data(), from_ap.size()), false, ByteView(out, 10)) == 0);
}


namespace linktest {
using namespace hardware::wpa2;
static const uint8_t kBssid[6] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, kUs[6] = {0x28, 0xD2, 0x44, 0x01, 0x02, 0x03};
struct Authenticator {
  uint8_t pmk[kPmkBytes], anonce[kNonceBytes], snonce[kNonceBytes] = {}, ptk[kPtkBytes] = {};
  uint64_t counter = 100;
  Bytes rsn_ie;
  uint8_t gtk[16];
  static void put_counter(EapolKey& k, uint64_t c) { for (int i = 0; i < 8; ++i) k.replay_counter[i] = static_cast<uint8_t>(c >> (8 * (7 - i))); }
  Bytes message1() {
    EapolKey k; k.info = kInfoVersionHmacSha1Aes | kInfoPairwise | kInfoAck; put_counter(k, ++counter); memcpy(k.nonce, anonce, 32);
    Bytes out(400); out.resize(build_eapol_key(k, ByteView(out.data(), out.size()))); return out;
  }
  bool check_message2(const Bytes& m2) {
    EapolKey k;
    if (!parse_eapol_key(ConstByteView(m2.data(), m2.size()), &k)) return false;
    memcpy(snonce, k.nonce, 32);
    derive_ptk(pmk, kBssid, kUs, anonce, snonce, ptk);
    return verify_mic(ConstByteView(ptk, 16), ConstByteView(m2.data(), m2.size()));
  }
  Bytes wrapped_gtk(uint8_t id, bool with_rsn) {
    Bytes plain;
    if (with_rsn) plain.insert(plain.end(), rsn_ie.begin(), rsn_ie.end());
    const uint8_t kde[8] = {0xDD, 22, 0x00, 0x0F, 0xAC, 1, id, 0};
    plain.insert(plain.end(), kde, kde + 8); plain.insert(plain.end(), gtk, gtk + 16);
    if (plain.size() % 8) { plain.push_back(0xDD); while (plain.size() % 8) plain.push_back(0); }
    Bytes wrapped(plain.size() + 8);
    aes_key_wrap(ConstByteView(ptk + 16, 16), ConstByteView(plain.data(), plain.size()), ByteView(wrapped.data(), wrapped.size()));
    return wrapped;
  }
  Bytes message3() {
    EapolKey k;
    k.info = kInfoVersionHmacSha1Aes | kInfoPairwise | kInfoInstall | kInfoAck | kInfoMic | kInfoSecure | kInfoEncryptedData;
    k.key_length = 16; put_counter(k, ++counter); memcpy(k.nonce, anonce, 32);
    Bytes wrapped = wrapped_gtk(1, true);
    k.data = ConstByteView(wrapped.data(), wrapped.size());
    Bytes out(400); out.resize(build_eapol_key(k, ByteView(out.data(), out.size())));
    set_mic(ConstByteView(ptk, 16), ByteView(out.data(), out.size()));
    return out;
  }
  Bytes group_message1(uint8_t id) {
    EapolKey k;
    k.info = kInfoVersionHmacSha1Aes | kInfoAck | kInfoMic | kInfoSecure | kInfoEncryptedData | (static_cast<uint16_t>(id) << 4);
    k.key_length = 16; put_counter(k, ++counter);
    Bytes wrapped = wrapped_gtk(id, false);
    k.data = ConstByteView(wrapped.data(), wrapped.size());
    Bytes out(400); out.resize(build_eapol_key(k, ByteView(out.data(), out.size())));
    set_mic(ConstByteView(ptk, 16), ByteView(out.data(), out.size()));
    return out;
  }
};
}  // namespace linktest

struct LinkSink final : IWifiDataSink {
  std::vector<Bytes> frames;
  void on_ethernet_frame(ConstByteView f) override { frames.push_back(Bytes(f.data(), f.data() + f.size())); }
};
struct TestClock final : IClock {
  mutable uint64_t us = 1000000;
  uint64_t now_us() const override { return us; }
  const char* name() const override { return "test clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

static const uint8_t (&kLinkBssid)[6] = linktest::kBssid;
static const uint8_t (&kLinkUs)[6] = linktest::kUs;

static Bytes ap_mgmt(uint16_t fc) {
  Bytes f{static_cast<uint8_t>(fc), static_cast<uint8_t>(fc >> 8), 0, 0};
  f.insert(f.end(), kLinkUs, kLinkUs + 6);
  f.insert(f.end(), kLinkBssid, kLinkBssid + 6);
  f.insert(f.end(), kLinkBssid, kLinkBssid + 6);
  f.insert(f.end(), {0, 0});
  return f;
}
static Bytes ap_data(const Bytes& eapol_or_payload, uint16_t ethertype, bool protect) {
  Bytes f = ap_mgmt(protect ? 0x4208 : 0x0208);
  if (protect) f.insert(f.end(), {1, 0, 0, 0x20, 0, 0, 0, 0});
  f.insert(f.end(), {0xAA, 0xAA, 0x03, 0, 0, 0, static_cast<uint8_t>(ethertype >> 8), static_cast<uint8_t>(ethertype)});
  f.insert(f.end(), eapol_or_payload.begin(), eapol_or_payload.end());
  return f;
}

static Bytes ap_mgmt_auth() { Bytes f = ap_mgmt(0x00B0); f.insert(f.end(), {0, 0, 2, 0, 0, 0}); return f; }
static Bytes ap_assoc_response() { Bytes f = ap_mgmt(0x0010); f.insert(f.end(), {0x11, 0x04, 0, 0, 3, 0xC0}); return f; }

static void test_station_link() {
  using namespace hardware::wpa2;
  using linktest::Authenticator;
  TestAllocator alloc;
  const Bytes file = make_loadable_firmware(64, true);
  IwlFirmware fw(alloc);
  const char* error = nullptr;
  CHECK(fw.parse(ConstByteView(file.data(), file.size()), &error));
  for (int scenario = 0; scenario < 2; ++scenario) {
    FakeIwl chip;
    chip.w1c = true;
    chip.force_radio = true;
    chip.prph[(iwl_tx::kScdSramBase & 0xFFFFF) / 4] = 0x800000;
    IwlTransport tp(chip, alloc, true);
    FakeRxChip rxhw(chip);
    FakeTxChip txhw(chip), q4(chip, 4);
    TestClock clock;
    LinkSink sink;
    IwlStationLink link(tp, alloc, clock);
    Ieee80211Association assoc(link);
    link.set_association(&assoc);
    link.set_data_sink(&sink);
    std::vector<std::pair<uint8_t, uint8_t>> commands;
    Bytes last_key_cmd, last_ptk_cmd, last_mac_cmd, last_sta_cmd;
    std::vector<Bytes> te_commands;
    chip.on_alive = [&] { Bytes f; FakeRxChip::frame(f, 0, 1, 0x8000, alive_payload(0xCAFE)); rxhw.deliver(f); };
    chip.on_doorbell = [&] {
      bool ok = false;
      for (const Bytes& c : txhw.process(&ok)) {
        const uint8_t group = c[1], op = c[0];
        const uint16_t seq = static_cast<uint16_t>(c[2] | (c[3] << 8));
        commands.push_back({group, op});
        if (op == 0x28) last_mac_cmd = c;
        if (op == 0x18) last_sta_cmd = c;
        if (op == 0x17) { if (c[4 + 3] & 0x40) last_key_cmd = c; else last_ptk_cmd = c; }
        Bytes f;
        const uint8_t status = (op == 0x2B) ? 0 : (op == 0x18 || op == 0x17) ? 1 : 0;
        if (op == 0x29) { te_commands.push_back(c); FakeRxChip::frame(f, group, op, seq, Bytes{1, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0}); rxhw.deliver(f); continue; }   // the time event is scheduled: unique id 9
        FakeRxChip::frame(f, group, op, seq, Bytes{status, 0, 0, 0});
        rxhw.deliver(f);
      }
    };
    // the access point
    Authenticator ap;
    const Bytes ssid_bytes{'H', 'o', 'm', 'e'};
    {
      uint8_t pmk[32];
      wpa2::derive_pmk(StringView("hunter2hunter2"), ConstByteView(ssid_bytes.data(), 4), pmk);
      memcpy(ap.pmk, pmk, 32);
    }
    for (int i = 0; i < 32; ++i) ap.anonce[i] = static_cast<uint8_t>(i * 3 + 1);
    ap.rsn_ie = Bytes{48, 20, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 2, 0, 0};
    for (int i = 0; i < 16; ++i) ap.gtk[i] = static_cast<uint8_t>(0x30 + i);
    std::vector<Bytes> tx_frames;                    // everything the card was asked to send on the data queue, as the firmware sees it
    bool m4 = false, group_reply_protected = false;
    auto deliver = [&](const Bytes& frame, bool decrypted = false) {
      Bytes g;
      FakeRxChip::frame(g, 0, 0xC1, 0x8000, rx_mpdu(frame, 6, 50, true, decrypted));
      rxhw.deliver(g);
    };
    chip.on_queue_doorbell = [&](uint32_t q) {
      if (q != 4) return;
      bool ok = false;
      for (const Bytes& c : q4.process(&ok)) {
        tx_frames.push_back(c);
        const uint16_t seq = static_cast<uint16_t>(c[2] | (c[3] << 8));
        Bytes resp(48, 0);
        resp[0] = 1; resp[40] = 1; resp[44] = static_cast<uint8_t>((seq & 0xFF) + 1);
        Bytes f;
        FakeRxChip::frame(f, 0, 0x1C, static_cast<uint16_t>(0x8000 | seq), resp);
        rxhw.deliver(f);
        const Bytes frame(c.begin() + 60, c.end());
        const uint16_t fc = static_cast<uint16_t>(frame[0] | (frame[1] << 8));
        if ((fc & 0x00FC) == 0x00B0) deliver(ap_mgmt_auth());
        else if ((fc & 0x00FC) == 0x0000) { deliver(ap_assoc_response()); deliver(ap_data(ap.message1(), 0x888E, false)); }
        else if ((fc & 0x000C) == 0x0008 && (fc & 0x4000)) {
          if (frame.size() >= 24 + 8 + 8 + 99) group_reply_protected = true;                     // the group key answer went out encrypted
        } else if ((fc & 0x000C) == 0x0008) {
          EapolKey k;
          ConstByteView p(frame.data() + 32, frame.size() - 32);
          if (!parse_eapol_key(p, &k)) continue;
          if (!(k.info & kInfoSecure)) { if (ap.check_message2(Bytes(p.data(), p.data() + p.size()))) deliver(ap_data(ap.message3(), 0x888E, false)); }
          else m4 = true;
        }
      }
    };
    CHECK(tp.start(fw, IwlImage::Regular, 5000, &error));
    tp.set_handler(&link);
    IwlNvmData nvm;
    nvm.tx_antennas = nvm.rx_antennas = 3;
    nvm.band_24ghz = nvm.band_5ghz = true;
    memcpy(nvm.hw_address, kLinkUs, 6);
    WifiNetwork net;
    strcpy(net.ssid, "Home"); memcpy(net.bssid.bytes, kLinkBssid, 6); net.security = WifiSecurity::Wpa2; net.channel = 6;
    if (scenario == 1) {                                                // firmware of another PHY context version
      const Bytes other = make_loadable_firmware(64, false);
      IwlFirmware fw2(alloc);
      CHECK(fw2.parse(ConstByteView(other.data(), other.size()), &error));
      CHECK(!link.prepare(fw2, nvm, net, &error) && error && !link.prepared());
      tp.stop();
      continue;
    }
    CHECK(link.prepare(fw, nvm, net, &error));
    if (!link.prepared()) printf("  prepare: %s\n", error ? error : "?");
    // the contexts went in the order: PHY, MAC (with the BSSID), binding, station
    CHECK(commands.size() >= 5 && commands[commands.size() - 5].second == 0x08 && commands[commands.size() - 4].second == 0x28 && commands[commands.size() - 3].second == 0x2B && commands[commands.size() - 2].second == 0x18 && commands.back().second == 0x29);   // (then the time event that keeps the channel)
    CHECK(te_commands.size() == 1 && te_commands[0].size() == 4 + 36 && te_commands[0][4 + 4] == 1 && te_commands[0][4 + 8] == 0 && (te_commands[0][4 + 28] | (te_commands[0][4 + 29] << 8)) == 500);     // the join's channel is held
    CHECK(last_mac_cmd.size() > 36 && memcmp(&last_mac_cmd[4 + 24], kLinkBssid, 6) == 0 && last_mac_cmd[4 + 4] == 2);        // action: modify
    CHECK(last_sta_cmd.size() == 4 + 48 && last_sta_cmd[4 + 16] == 1 && last_sta_cmd[4 + 8] == 0xA0 && last_sta_cmd[4 + 40] == 0x10);
    CHECK(chip.prph[(iwl_tx::scd_queue_status(4) & 0xFFFFF) / 4] == (iwl_tx::kQueueStatusActive | 3 | iwl_tx::kQueueStatusWsl | iwl_tx::kQueueStatusMask));
    // the join
    uint8_t snonce[32];
    for (int i = 0; i < 32; ++i) snonce[i] = static_cast<uint8_t>(0x55 + i);
    uint8_t ap_rsn[22] = {48, 20, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 2, 0, 0};
    CHECK(assoc.start(net, ConstByteView(ap_rsn, 22), StringView("hunter2hunter2"), snonce, kLinkUs) == WifiResult::Ok);
    for (int i = 0; i < 40 && assoc.state() != Ieee80211Association::State::Connected && assoc.state() != Ieee80211Association::State::Failed; ++i) { tp.poll(nullptr); assoc.poll(); }
    if (assoc.state() != Ieee80211Association::State::Connected) printf("  association: state %d, %s\n", static_cast<int>(assoc.state()), assoc.failure());
    CHECK(assoc.state() == Ieee80211Association::State::Connected && m4);
    // the first frame sent: the authentication request as a TX command on queue 4
    CHECK(tx_frames.size() >= 4);
    if (tx_frames.size() >= 4) {
      const Bytes& a = tx_frames[0];
      CHECK(a[0] == 0x1C && a[3] == 0x04 && a[4 + 16] == 1 && a[4 + 17] == 0 && a.size() == 4 + 56 + 24 + 6 && a[60] == 0xB0 && a[64] == 0xA0 && a[16] == 0x0A && a[17] == 0x42);       // queue 4, station 1, 1 Mbit/s CCK on antenna A
      const uint16_t len = static_cast<uint16_t>(a[4] | (a[5] << 8));
      CHECK(len == 30);
      CHECK(tx_frames[1][60] == 0x00 && tx_frames[2][60] == 0x08 && tx_frames[2][4 + 17] == 0 && tx_frames[3][4 + 17] == 0);                 // association request, then the handshake's frames unencrypted
    }
    // the byte count table: the first frames' entries (30 bytes + 4 + 4 = 38 -> 10 dwords) with the station in the top bits
    {
      const uint8_t* table = reinterpret_cast<const uint8_t*>(static_cast<uint64_t>(chip.prph[(iwl_tx::kScdDramBase & 0xFFFFF) / 4]) << 10);
      const uint16_t e = static_cast<uint16_t>(table[(4 * 320 + 0) * 2] | (table[(4 * 320 + 0) * 2 + 1] << 8));
      CHECK(e == (10 | (1 << 12)));
      const uint16_t dup = static_cast<uint16_t>(table[(4 * 320 + 256) * 2] | (table[(4 * 320 + 256) * 2 + 1] << 8));
      CHECK(dup == e);
    }
    // keys: the pairwise key (id 0, not multicast) with the TK, the group key (id 1, multicast)
    CHECK(last_ptk_cmd.size() == 4 + 64 && last_ptk_cmd[4 + 0] == 1 && last_ptk_cmd[4 + 1] == 0 && memcmp(&last_ptk_cmd[4 + 4], ap.ptk + 32, 16) == 0);
    CHECK(last_key_cmd.size() == 4 + 64 && last_key_cmd[4 + 1] == 1 && (last_key_cmd[4 + 3] & 0x40) && ((last_key_cmd[4 + 3] & 3) == 1) && last_key_cmd[4 + 4] == 0x30 && last_key_cmd[4 + 19] == 0x3F);
    // the interface was told it is associated: id 5? (the response's aid 3)
    CHECK(last_mac_cmd[4 + 4] == 2 && last_mac_cmd[4 + 100] == 1 && last_mac_cmd[4 + 100 + 36] == 3);
    // protected data goes out with the key in the command and a CCMP header
    tx_frames.clear();
    Bytes eth{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x28, 0xD2, 0x44, 1, 2, 3, 0x08, 0x00, 'p', 'i', 'n', 'g'};
    CHECK(link.send_ethernet(ConstByteView(eth.data(), eth.size())));
    CHECK(tx_frames.size() == 1);
    if (!tx_frames.empty()) {
      const Bytes& d = tx_frames[0];
      CHECK(d[4 + 17] == 2 && memcmp(&d[4 + 20], ap.ptk + 32, 16) == 0 && d[60] == 0x08 && d[61] == 0x41 && d[64] == 0xA0 && d[70] == 0x28 && d[76] == 0x11);
      CHECK(d[84] == 1 && d[85] == 0 && d[87] == 0x20 && d[92] == 0xAA && d[98] == 0x08 && d[100] == 'p');                                   // packet number 1 after the handshake's own: see below
      CHECK(d[16] == 9 && d[4 + 52] == 0);                                                                                                       // 24 Mbit/s OFDM, no power-management hold
      const uint16_t len = static_cast<uint16_t>(d[4] | (d[5] << 8));
      CHECK(len == 24 + 8 + 8 + 4);                                                                                                           // header, CCMP header, SNAP, 4 byte payload... plus ethertype below
    }
    // a sent frame is reclaimed by its response; the queue does not fill up over many frames
    for (int i = 0; i < 100; ++i) {
      tp.poll(nullptr);
      CHECK(link.send_ethernet(ConstByteView(eth.data(), eth.size())));
    }
    CHECK(link.tx_failures() == 0);
    // received data: a decrypted frame reaches the sink as Ethernet; one that was not decrypted does not; EAPOL does not go to the sink
    deliver(ap_data(Bytes{'h', 'e', 'y'}, 0x0800, true), true);
    tp.poll(nullptr);
    deliver(ap_data(Bytes{'n', 'o'}, 0x0800, true), false);
    tp.poll(nullptr);
    deliver(ap_data(Bytes{'o', 'p', 'e', 'n'}, 0x0806, false));
    tp.poll(nullptr);
    CHECK(sink.frames.size() == 2 && sink.frames[0].size() == 14 + 3 && sink.frames[0][12] == 0x08 && sink.frames[0][14] == 'h' && sink.frames[0][0] == 0x28 && sink.frames[1][12] == 0x08 && sink.frames[1][13] == 0x06);
    // the group key handshake arrives encrypted and is answered encrypted
    {
      for (int i = 0; i < 16; ++i) ap.gtk[i] = static_cast<uint8_t>(0x90 + i);
      Bytes g1 = ap.group_message1(2);
      deliver(ap_data(g1, 0x888E, true), true);
      tx_frames.clear();
      tp.poll(nullptr);
      CHECK(group_reply_protected && sink.frames.size() == 2);
      CHECK(assoc.state() == Ieee80211Association::State::Connected);
    }
    // frames of the card's radio are counted
    CHECK(link.frames_received() > 4 && link.frames_sent() > 100);
    assoc.leave();
    link.release();
    tp.stop();
    CHECK(alloc.dma.empty());
  }
}

// ---- the wireless interface: scan, connect, frames, disconnect, through one fixture of firmware, chip and access point ---------------------------------------------------------

struct FixedRandom final : crypto::IRandom {
  bool fill(ByteView out) override { for (uint64_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(0x40 + i); return true; }
};

struct WifiFixture {
  TestAllocator alloc;
  FakeIwl chip;
  FakeRxChip rxhw{chip};
  FakeTxChip txhw{chip}, q4{chip, 4};
  MemorySource source;
  TestClock clock;
  FixedRandom random;
  linktest::Authenticator ap;
  std::vector<uint8_t> commands;                        // opcodes seen on the command queue
  std::vector<Bytes> te_commands;                       // the time event commands
  std::vector<Bytes> queue4;                            // frames sent to the access point
  bool ap_answers_handshake = true;
  uint32_t phy_actions = 0;                             // PHY context action of the latest PHY command
  int last_phy_action = 0;

  explicit WifiFixture(const char* ap_passphrase) {
    source.file = make_loadable_firmware(64, true);
    source.only = "iwlwifi-9000-pu-b0-jf-b0-40.ucode";
    chip.w1c = true;
    chip.force_radio = true;
    chip.regs[iwl_csr::kHwRev / 4] = (0x31u << 4) | 0x1;
    chip.regs[iwl_csr::kHwRfId / 4] = 0x108u << 12;
    chip.prph[(iwl_tx::kScdSramBase & 0xFFFFF) / 4] = 0x800000;
    chip.regs[0x388 / 4] = 0x28D24401; chip.regs[0x38C / 4] = 0x00000203;
    wpa2::derive_pmk(StringView(ap_passphrase), ConstByteView(reinterpret_cast<const uint8_t*>("Home"), 4), ap.pmk);
    for (int i = 0; i < 32; ++i) ap.anonce[i] = static_cast<uint8_t>(i * 3 + 1);
    ap.rsn_ie = Bytes{48, 20, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 2, 0, 0};
    for (int i = 0; i < 16; ++i) ap.gtk[i] = static_cast<uint8_t>(0x30 + i);
    chip.on_alive = [this] { Bytes f; FakeRxChip::frame(f, 0, 1, 0x8000, alive_payload(0xCAFE)); rxhw.deliver(f); };
    chip.on_doorbell = [this] { command_doorbell(); };
    chip.on_queue_doorbell = [this](uint32_t q) { if (q == 4) data_doorbell(); };
    chip.on_ring_programmed = [this](uint32_t q) { if (q == 4) q4.read = 0; };           // a new ring starts at the beginning
  }
  void deliver_frame(const Bytes& frame, bool decrypted = false) {
    Bytes g;
    FakeRxChip::frame(g, 0, 0xC1, 0x8000, rx_mpdu(frame, 6, 50, true, decrypted));
    rxhw.deliver(g);
  }
  void command_doorbell() {
    bool ok = false;
    for (const Bytes& c : txhw.process(&ok)) {
      const uint8_t group = c[1], op = c[0];
      const uint16_t seq = static_cast<uint16_t>(c[2] | (c[3] << 8));
      commands.push_back(op);
      Bytes f;
      if (group == 0x0C && op == 0x02) {
        FakeRxChip::frame(f, group, op, seq, nvm_info(5, 0x1F, 1));
      } else if (group == 1 && op == 0x0D) {
        FakeRxChip::frame(f, group, op, seq, Bytes{1, 0, 0, 0, 0, 0, 0, 0});
        rxhw.deliver(f);
        Bytes g;
        FakeRxChip::frame(g, 0, 0xC1, 0x8000, rx_mpdu(beacon("Home", 0xA5, true, linktest::kBssid), 6, 50, true));
        FakeRxChip::frame(g, 0, 0xC1, 0x8000, rx_mpdu(beacon("Cafe", 2, false), 11, 70, true));
        rxhw.deliver(g);
        const uint32_t uid = static_cast<uint32_t>(c[8 + 4] | (c[8 + 5] << 8));
        Bytes e, h;
        FakeRxChip::frame(e, 0, 0x0F, 0x8000, Bytes{static_cast<uint8_t>(uid), static_cast<uint8_t>(uid >> 8), 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0});
        rxhw.deliver(e);
        continue;
      } else {
        const uint8_t status = (op == 0x2B) ? 0 : (op == 0x18 || op == 0x17) ? 1 : 0;
        if (op == 0x29) { te_commands.push_back(c); FakeRxChip::frame(f, group, op, seq, Bytes{1, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0}); rxhw.deliver(f); continue; }   // the time event is scheduled: unique id 9
        FakeRxChip::frame(f, group, op, seq, Bytes{status, 0, 0, 0});
        if (group == 2 && op == 3) FakeRxChip::frame(f, 2, 4, 0x8000, Bytes{1});
        if (op == 0x08) last_phy_action = c[4 + 4];
      }
      rxhw.deliver(f);
    }
  }
  void data_doorbell() {
    bool ok = false;
    for (const Bytes& c : q4.process(&ok)) {
      queue4.push_back(c);
      const uint16_t seq = static_cast<uint16_t>(c[2] | (c[3] << 8));
      Bytes resp(48, 0);
      resp[0] = 1; resp[40] = 1; resp[44] = static_cast<uint8_t>((seq & 0xFF) + 1);
      Bytes f;
      FakeRxChip::frame(f, 0, 0x1C, static_cast<uint16_t>(0x8000 | seq), resp);
      rxhw.deliver(f);
      const Bytes frame(c.begin() + 60, c.end());
      const uint16_t fc = static_cast<uint16_t>(frame[0] | (frame[1] << 8));
      if ((fc & 0x00FC) == 0x00B0) deliver_frame(ap_mgmt_auth());
      else if ((fc & 0x00FC) == 0x0000) { deliver_frame(ap_assoc_response()); deliver_frame(ap_data(ap.message1(), 0x888E, false)); }
      else if ((fc & 0x000C) == 0x0008 && !(fc & 0x4000) && frame.size() > 32) {
        wpa2::EapolKey k;
        ConstByteView p(frame.data() + 32, frame.size() - 32);
        if (wpa2::parse_eapol_key(p, &k) && !(k.info & wpa2::kInfoSecure) && ap_answers_handshake && ap.check_message2(Bytes(p.data(), p.data() + p.size())))
          deliver_frame(ap_data(ap.message3(), 0x888E, false));
      }
    }
  }
};

static void test_wireless_interface() {
  for (int scenario = 0; scenario < 3; ++scenario) {
    WifiFixture fx(scenario == 2 ? "another passphrase" : "hunter2hunter2");
    IwlWirelessInterface::Inputs in;
    in.firmware = &fx.source; in.allocator = &fx.alloc; in.clock = &fx.clock; in.random = &fx.random;
    IwlWirelessInterface wifi(in);
    CHECK(!wifi.link_up() && wifi.start_scan() == false && wifi.connect(StringView("Home"), StringView("hunter2hunter2")) == WifiResult::Failed);       // not started
    CHECK(wifi.start(fx.chip, true));
    if (!wifi.link_up() && wifi.failure()[0]) printf("  failure: %s\n", wifi.failure());
    const MacAddress mac = wifi.mac_address();
    CHECK(mac.bytes[0] == 0x28 && mac.bytes[1] == 0xD2 && mac.bytes[2] == 0x44 && mac.bytes[3] == 0x01 && mac.bytes[4] == 0x02 && mac.bytes[5] == 0x03);
    CHECK(wifi.wireless() == &wifi && !wifi.link_up() && wifi.status().state == WifiState::Disconnected);
    // scan
    CHECK(wifi.start_scan() && wifi.status().state != WifiState::Connecting);
    for (int i = 0; i < 20 && wifi.status().state == WifiState::Scanning; ++i) {}
    WifiNetwork nets[8];
    uint32_t n = wifi.scan_results(ArrayView<WifiNetwork>(nets, 8));
    CHECK(n == 2 && strcmp(nets[0].ssid, "Home") == 0 && strcmp(nets[1].ssid, "Cafe") == 0 && nets[0].security == WifiSecurity::Wpa2 && nets[0].channel == 6);
    // refusals
    CHECK(wifi.connect(StringView("Nowhere"), StringView("hunter2hunter2")) == WifiResult::NoSuchNetwork);
    CHECK(wifi.connect(StringView("Home"), StringView("short")) == WifiResult::BadPassphrase);
    CHECK(wifi.connect(StringView(), StringView()) == WifiResult::InvalidArgument);
    CHECK(wifi.connect(StringView("Cafe"), StringView()) == WifiResult::Unsupported);                  // WEP
    // join
    CHECK(wifi.connect(StringView("Home"), StringView("hunter2hunter2")) == WifiResult::Ok);
    CHECK(wifi.connect(StringView("Home"), StringView("hunter2hunter2")) == WifiResult::Busy && !wifi.start_scan());
    WifiStatus st = wifi.status();
    if (scenario == 2) {                                                  // the wrong passphrase: the access point never sends message 3
      CHECK(st.state == WifiState::Connecting && !wifi.link_up());
      fx.clock.us += 6000000;
      st = wifi.status();
      CHECK(st.state == WifiState::Disconnected && st.last_result == WifiResult::BadPassphrase && !wifi.link_up());
      CHECK(wifi.start_scan());                                           // free to scan and try again
      wifi.shutdown();
      CHECK(fx.alloc.dma.empty());
      continue;
    }
    CHECK(st.state == WifiState::Connected && st.last_result == WifiResult::Ok && strcmp(st.network.ssid, "Home") == 0 && wifi.link_up() && wifi.link_speed_mbps() == 24);
    if (scenario == 1) {
      // a deauthentication ends the connection
      fx.deliver_frame(ap_mgmt(0x00C0));
      st = wifi.status();
      CHECK(st.state == WifiState::Connected);                            // (the deauthentication frame lacks a reason: dropped as malformed)
      Bytes de = ap_mgmt(0x00C0); de.insert(de.end(), {7, 0});
      fx.deliver_frame(de);
      st = wifi.status();
      CHECK(st.state == WifiState::Disconnected && st.last_result == WifiResult::Failed && !wifi.link_up());
      CHECK(wifi.start_scan());
      wifi.shutdown();
      CHECK(fx.alloc.dma.empty());
      continue;
    }
    // frames through the interface
    Bytes eth{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x28, 0xD2, 0x44, 1, 2, 3, 0x08, 0x00, 'h', 'i'};
    fx.queue4.clear();
    CHECK(wifi.transmit(ConstByteView(eth.data(), eth.size())));
    CHECK(fx.queue4.size() == 1 && fx.queue4[0][4 + 17] == 2 && fx.queue4[0][61] == 0x41);
    CHECK(!wifi.transmit(ConstByteView(eth.data(), 5)));
    fx.deliver_frame(ap_data(Bytes{'y', 'o'}, 0x0800, true), true);
    uint8_t buf[64];
    uint32_t len = 0;
    CHECK(wifi.receive(ByteView(buf, sizeof buf), &len) && len == 16 && buf[12] == 8 && buf[14] == 'y');
    CHECK(!wifi.receive(ByteView(buf, sizeof buf), &len));
    NetworkStatistics ns = wifi.statistics();
    CHECK(ns.frames_sent == 1 && ns.frames_received == 1 && ns.send_failures == 1);
    // the network stack over it: DHCP starts when the link comes up
    {
      TestClock stack_clock;
      network::NetworkStack stack(wifi, stack_clock, fx.alloc);
      CHECK(stack.initialize());
      stack.set_dhcp_follows_link(true);
      fx.queue4.clear();
      stack.poll();                                                        // the link is up: DHCP discover goes out as protected data
      CHECK(!fx.queue4.empty() && fx.queue4[0][61] == 0x41);
    }
    // disconnect: the station and the binding leave the firmware, the link drops, joining again works with the PHY context modified
    fx.commands.clear();
    wifi.disconnect();
    CHECK(wifi.status().state == WifiState::Disconnected && !wifi.link_up());
    bool removed_station = false, removed_binding = false;
    for (uint8_t op : fx.commands) { removed_station |= op == 0x19; removed_binding |= op == 0x2B; }
    CHECK(removed_station && removed_binding);
    CHECK(fx.te_commands.size() >= 2 && fx.te_commands.back()[4 + 4] == 3 && fx.te_commands.back()[4 + 8] == 9);       // the time event was taken away (action remove, the id the firmware gave)
    CHECK(!wifi.transmit(ConstByteView(eth.data(), eth.size())));
    CHECK(wifi.start_scan());
    for (int i = 0; i < 20 && wifi.status().state == WifiState::Scanning; ++i) {}
    const WifiResult again = wifi.connect(StringView("Home"), StringView("hunter2hunter2"));
    if (again != WifiResult::Ok) { printf("  reconnect: %s; commands:", wifi_result_name(again)); for (size_t i = fx.commands.size() > 12 ? fx.commands.size() - 12 : 0; i < fx.commands.size(); ++i) printf(" %02x", fx.commands[i]); printf(" ; taken %u wp %u\n", fx.rxhw.taken, fx.rxhw.write_pointer()); }
    CHECK(again == WifiResult::Ok);
    CHECK(fx.last_phy_action == 2);
    CHECK(wifi.status().state == WifiState::Connected);
    wifi.shutdown();
    CHECK(fx.alloc.dma.empty() && !wifi.link_up());
  }
}

// What the card says when the firmware stops: the interrupt status, the error table in the device's memory, a command that is never answered, the refusal of a command.
static void test_firmware_failure() {
  TestAllocator alloc;
  const Bytes file = make_loadable_firmware(64);
  IwlFirmware fw(alloc);
  const char* error = nullptr;
  CHECK(fw.parse(ConstByteView(file.data(), file.size()), &error));
  for (int scenario = 0; scenario < 3; ++scenario) {
    FakeIwl chip;
    chip.w1c = true;
    chip.force_radio = true;
    chip.regs[iwl_csr::kHwRev / 4] = (0x31u << 4) | 0x3;
    chip.prph[(iwl_tx::kScdSramBase & 0xFFFFF) / 4] = 0x800000;
    IwlTransport tp(chip, alloc, true);
    FakeRxChip rxhw(chip);
    FakeTxChip txhw(chip);
    bool answer = true;
    chip.on_alive = [&] { Bytes f; FakeRxChip::frame(f, 0, iwl_cmd::kAliveNotification, 0x8000, alive_payload(0xCAFE)); rxhw.deliver(f); };
    chip.on_doorbell = [&] {
      bool ok = false;
      for (const Bytes& c : txhw.process(&ok)) {
        if (!answer) continue;
        Bytes f;
        FakeRxChip::frame(f, c[1], c[0], static_cast<uint16_t>(c[2] | (c[3] << 8)), Bytes{0, 0, 0, 0});
        rxhw.deliver(f);
      }
    };
    CHECK(tp.start(fw, IwlImage::Regular, 5000, &error) && !tp.firmware_failed() && tp.failure_text()[0] == 0);
    // the error table the ALIVE notification pointed to (0x1234): one entry, an assert with its data words
    uint32_t* table = &chip.mem[0x1234 / 4];
    table[0] = 1;
    table[1] = 0x70;                                  // error_id
    table[4] = 0xB2B2B2B2;                            // blink2
    table[6] = 0x12121212;                            // ilink2
    table[7] = 0xD1;
    table[8] = 0xD2;
    table[9] = 0xD3;
    table[23] = 0xC0DE;                               // hcmd
    table[29] = 0x28;                                 // last command
    uint8_t buf[16];
    IwlHostCommand c;
    c.opcode = 0x21;
    CHECK(tp.send_sync(c, ByteView(buf, sizeof buf), 2000, &error) == 4 && tp.command_errors() == 0);
    {   // a REPLY_ERROR notification is counted and the firmware goes on
      Bytes f;
      FakeRxChip::frame(f, 0, iwl_cmd::kReplyError, 0x8000, Bytes{0x20, 0, 0, 0, 0x18, 0, 0x34, 0x12, 5, 0, 0, 0});
      rxhw.deliver(f);
      tp.poll(nullptr);
      CHECK(tp.command_errors() == 1 && !tp.firmware_failed());
    }
    if (scenario == 0) {                              // the card raises the software error interrupt
      chip.regs[iwl_csr::kInt / 4] |= iwl_csr::kIntBitSwError;
      CHECK(tp.check_firmware() && tp.firmware_failed());
    } else if (scenario == 1) {                       // the same, found while a command waits
      answer = false;
      chip.regs[iwl_csr::kInt / 4] |= iwl_csr::kIntBitHwError;
      CHECK(tp.send_sync(c, ByteView(buf, sizeof buf), 2000, &error) < 0 && tp.firmware_failed());
    } else {                                          // nobody answers and no interrupt: the log is read after the wait
      answer = false;
      CHECK(tp.send_sync(c, ByteView(buf, sizeof buf), 500, &error) < 0 && tp.firmware_failed());
    }
    CHECK(tp.error_log().valid() && tp.error_log().error_id() == 0x70 && tp.error_log().data1() == 0xD1 && tp.error_log().data2() == 0xD2 && tp.error_log().data3() == 0xD3 && tp.error_log().hcmd() == 0xC0DE);
    CHECK(strstr(tp.failure_text(), "0x00000070") && strstr(tp.failure_text(), "0x000000D1") && strstr(tp.failure_text(), "0x12121212") && strstr(tp.failure_text(), "last command 0x00000028"));
    // every later command fails at once with the text; a new start clears it
    error = nullptr;
    answer = true;
    CHECK(tp.send_sync(c, ByteView(buf, sizeof buf), 2000, &error) < 0 && error && strstr(error, "0x00000070"));
    chip.regs[iwl_csr::kInt / 4] = 0;
    tp.stop();
    CHECK(tp.start(fw, IwlImage::Regular, 5000, &error) && !tp.firmware_failed() && tp.failure_text()[0] == 0 && !tp.error_log().valid());
    tp.stop();
  }
  {   // the log reader on its own: an empty table, a pointer that is no pointer
    FakeIwl chip;
    chip.regs[iwl_csr::kGpCntrl / 4] = iwl_csr::kGpMacClockReady;
    IwlPcieDevice dev(chip);
    IwlErrorLog log;
    char text[256];
    CHECK(iwl_read_error_log(dev, 0x40, &log) && !log.valid());
    iwl_format_error_log(log, text, sizeof text);
    CHECK(strstr(text, "without an error log"));
    CHECK(!iwl_read_error_log(dev, 0, &log) && !iwl_read_error_log(dev, 0x41, &log) && !iwl_read_error_log(dev, 0xFFFFFFFFu, &log));
    log.words[0] = 1;
    log.words[1] = 0xABCD;
    CHECK(iwl_format_error_log(log, text, sizeof text) > 20 && strstr(text, "0x0000ABCD"));
    char small[12];
    CHECK(iwl_format_error_log(log, small, sizeof small) == 11 && small[11] == 0);
    CHECK(iwl_format_error_log(log, small, 0) == 0);
  }
}

// The firmware stops while the interface is joined: it is taken down, the card started again, and the interface works again; after too many in a minute it stays down.
static void test_firmware_recovery() {
  WifiFixture fx("hunter2hunter2");
  IwlWirelessInterface::Inputs in;
  in.firmware = &fx.source; in.allocator = &fx.alloc; in.clock = &fx.clock; in.random = &fx.random;
  IwlWirelessInterface wifi(in);
  CHECK(wifi.start(fx.chip, true));
  CHECK(wifi.start_scan());
  CHECK(wifi.connect(StringView("Home"), StringView("hunter2hunter2")) == WifiResult::Ok && wifi.link_up());
  uint32_t* table = &fx.chip.mem[0x1234 / 4];
  table[0] = 1;
  table[1] = 0x70;
  table[7] = 0xD1;
  for (int failure = 1; failure <= 4; ++failure) {
    fx.chip.regs[iwl_csr::kInt / 4] |= iwl_csr::kIntBitSwError;                        // the firmware asserts
    CHECK(!wifi.link_up());                                                           // noticed on the next look: the connection is gone
    if (failure <= 3) {
      CHECK(wifi.restarts() == static_cast<uint32_t>(failure) && wifi.driver().running() && wifi.status().state == WifiState::Disconnected && strstr(wifi.last_firmware_failure(), "0x00000070"));
      CHECK(wifi.start_scan());                                                       // alive again
      for (int i = 0; i < 20 && wifi.status().state == WifiState::Scanning; ++i) {}
      WifiNetwork nets[8];
      CHECK(wifi.scan_results(ArrayView<WifiNetwork>(nets, 8)) == 2);
      if (failure == 1) {
        CHECK(wifi.connect(StringView("Home"), StringView("hunter2hunter2")) == WifiResult::Ok && wifi.link_up());      // and can join again
      }
    } else {
      CHECK(wifi.restarts() == 3 && !wifi.driver().running() && strstr(wifi.failure(), "keeps failing") && !wifi.start_scan());
    }
  }
  // a minute later the count starts again
  wifi.shutdown();
  CHECK(fx.alloc.dma.empty());
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
  test_nvm_info();
  test_driver();
  test_mvm_builders();
  test_scanner();
  test_link_builders();
  test_station_link();
  test_firmware_failure();
  test_wireless_interface();
  test_firmware_recovery();
  printf(g_failed ? "IwlTest: %d FAILED\n" : "IwlTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
