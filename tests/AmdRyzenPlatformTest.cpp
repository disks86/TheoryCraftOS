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

// Host-side tests for the Ryzen SoC catalogue (which die a CPU model is, and the integrated graphics, display, video engine and NPU it carries) and the
// VCN generation table. Also checks that the catalogue, the CPU generation rules and the GPU ID table agree with each other.
#include <stdio.h>
#include <string.h>
#include "Hardware/CentralProcessingUnit/AMD/AmdGenerations.hpp"
#include "Hardware/CentralProcessingUnit/X86_64Cpu.hpp"
#include "Hardware/Platform/AMD/AmdRyzenPlatform.hpp"

using namespace hardware;

extern "C" {
void tcos_halt() {}
void tcos_pause() {}
void tcos_breakpoint() {}
void tcos_disable_interrupts() {}
void tcos_enable_interrupts() {}
uint64_t tcos_rdtsc() { return 0; }
}

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static bool ip_is(const AmdIpVersion& v, uint8_t major, uint8_t minor, uint8_t revision) {
  return v.major == major && v.minor == minor && v.revision == revision;
}

static void test_zen_cores() {
  using Z = AmdZenCore;
  // The boundaries of arch/x86/kernel/cpu/amd.c.
  CHECK(amd_zen_core(0x17, 0x01) == Z::Zen && amd_zen_core(0x17, 0x2F) == Z::Zen && amd_zen_core(0x17, 0x50) == Z::Zen && amd_zen_core(0x17, 0x5F) == Z::Zen);
  CHECK(amd_zen_core(0x17, 0x08) == Z::ZenPlus && amd_zen_core(0x17, 0x18) == Z::ZenPlus);
  CHECK(amd_zen_core(0x17, 0x30) == Z::Zen2 && amd_zen_core(0x17, 0x4F) == Z::Zen2 && amd_zen_core(0x17, 0x60) == Z::Zen2 && amd_zen_core(0x17, 0x7F) == Z::Zen2);
  CHECK(amd_zen_core(0x17, 0x90) == Z::Zen2 && amd_zen_core(0x17, 0x91) == Z::Zen2 && amd_zen_core(0x17, 0x92) == Z::Unknown && amd_zen_core(0x17, 0xA0) == Z::Zen2);
  CHECK(amd_zen_core(0x17, 0x80) == Z::Unknown && amd_zen_core(0x17, 0xB0) == Z::Unknown);
  CHECK(amd_zen_core(0x19, 0x00) == Z::Zen3 && amd_zen_core(0x19, 0x21) == Z::Zen3 && amd_zen_core(0x19, 0x5F) == Z::Zen3);
  CHECK(amd_zen_core(0x19, 0x10) == Z::Zen4 && amd_zen_core(0x19, 0x61) == Z::Zen4 && amd_zen_core(0x19, 0x74) == Z::Zen4 && amd_zen_core(0x19, 0xAF) == Z::Zen4);
  CHECK(amd_zen_core(0x19, 0xB0) == Z::Unknown);
  CHECK(amd_zen_core(0x1A, 0x00) == Z::Zen5 && amd_zen_core(0x1A, 0x24) == Z::Zen5 && amd_zen_core(0x1A, 0x44) == Z::Zen5 && amd_zen_core(0x1A, 0x70) == Z::Zen5);
  CHECK(amd_zen_core(0x1A, 0xD0) == Z::Zen5 && amd_zen_core(0x1A, 0xD7) == Z::Zen5 && amd_zen_core(0x1A, 0x30) == Z::Unknown);
  CHECK(amd_zen_core(0x1A, 0x50) == Z::Zen6 && amd_zen_core(0x1A, 0x80) == Z::Zen6 && amd_zen_core(0x1A, 0xAF) == Z::Zen6 && amd_zen_core(0x1A, 0xC0) == Z::Zen6);
  CHECK(amd_zen_core(0x1A, 0xD8) == Z::Zen6 && amd_zen_core(0x1A, 0xEF) == Z::Zen6 && amd_zen_core(0x1A, 0xF0) == Z::Unknown && amd_zen_core(0x1A, 0xB0) == Z::Unknown);
  CHECK(amd_zen_core(0x15, 0x02) == Z::Unknown && amd_zen_core(0x10, 0x00) == Z::Unknown);
  CHECK(strcmp(amd_zen_core_name(Z::Zen6), "Zen 6") == 0 && strcmp(amd_zen_core_name(Z::ZenPlus), "Zen+") == 0);

  // The CPU generation classes say the same thing for every model the Linux table classifies.
  constexpr uint32_t kZenFamilies[] = {0x17, 0x19, 0x1A};
  for (uint32_t family : kZenFamilies) {
    for (uint32_t model = 0; model < 256; ++model) {
      const Z core = amd_zen_core(family, model);
      if (core == Z::Unknown) continue;
      alignas(X86_64Cpu) unsigned char storage[sizeof(X86_64Cpu)];
      ICentralProcessingUnit* cpu = create_amd_generation(storage, nullptr, {family, model, 0});
      char expected[32];
      snprintf(expected, sizeof expected, "AMD %s", amd_zen_core_name(core));
      if (!cpu || strcmp(cpu->name(), expected) != 0) {
        printf("FAILED family %X model %X: class %s, Linux says %s\n", family, model, cpu ? cpu->name() : "(none)", expected);
        ++g_failed;
      }
    }
  }
}

static void test_identify() {
  using S = AmdRyzenSoc;
  // The desktop parts without graphics.
  AmdRyzenSocInfo m = amd_ryzen_identify(0x17, 0x71);
  CHECK(m.soc == S::Matisse && m.core == AmdZenCore::Zen2 && !m.has_graphics() && !m.has_video_engine() && !m.has_npu() && m.gpu_device_ids.empty());
  CHECK(amd_ryzen_identify(0x19, 0x21).soc == S::Vermeer && amd_ryzen_identify(0x17, 0x01).soc == S::SummitRidge && amd_ryzen_identify(0x17, 0x08).soc == S::PinnacleRidge);
  CHECK(amd_ryzen_identify(0x17, 0x08).core == AmdZenCore::ZenPlus && amd_ryzen_identify(0x19, 0x18).soc == S::StormPeak && amd_ryzen_identify(0x19, 0x08).soc == S::Chagall);
  CHECK(amd_ryzen_identify(0x17, 0x47).soc == S::CyanSkillfish && !amd_ryzen_identify(0x17, 0x47).has_graphics());

  // VCN 1 APUs.
  AmdRyzenSocInfo raven = amd_ryzen_identify(0x17, 0x11);
  CHECK(raven.soc == S::RavenRidge && raven.graphics == AmdGpuArchitecture::Vega && ip_is(raven.gc, 9, 1, 0) && ip_is(raven.vcn, 1, 0, 0) && ip_is(raven.dcn, 1, 0, 0));
  CHECK(raven.video_engine().generation == AmdVcnGeneration::Vcn1 && raven.video_engine().firmware == nullptr);   // (by PCI device: amd_vcn1_firmware_name)
  CHECK(amd_ryzen_identify(0x17, 0x18).soc == S::Picasso && amd_ryzen_identify(0x17, 0x20).soc == S::Dali && ip_is(amd_ryzen_identify(0x17, 0x20).gc, 9, 2, 2));

  // VCN 2.2: the file differs between Renoir/Lucienne and Cezanne/Barcelo.
  CHECK(strcmp(amd_ryzen_identify(0x17, 0x60).video_engine().firmware, "renoir_vcn.bin") == 0);
  CHECK(strcmp(amd_ryzen_identify(0x17, 0x68).video_engine().firmware, "renoir_vcn.bin") == 0 && amd_ryzen_identify(0x17, 0x68).soc == S::Lucienne);
  CHECK(strcmp(amd_ryzen_identify(0x19, 0x50).video_engine().firmware, "green_sardine_vcn.bin") == 0 && amd_ryzen_identify(0x19, 0x50).core == AmdZenCore::Zen3);

  // RDNA 2 APUs.
  AmdRyzenSocInfo deck = amd_ryzen_identify(0x17, 0x90);
  CHECK(deck.soc == S::VanGogh && deck.graphics == AmdGpuArchitecture::Rdna2 && strcmp(deck.video_engine().firmware, "vangogh_vcn.bin") == 0);
  CHECK(deck.video_engine().decode_mask() & codec_mask(VideoCodec::Av1));
  AmdRyzenSocInfo rmb = amd_ryzen_identify(0x19, 0x44);
  CHECK(rmb.soc == S::Rembrandt && ip_is(rmb.vcn, 3, 1, 1) && strcmp(rmb.video_engine().firmware, "yellow_carp_vcn.bin") == 0);
  CHECK(!(rmb.video_engine().decode_mask() & codec_mask(VideoCodec::Mpeg2)) && !(rmb.video_engine().decode_mask() & codec_mask(VideoCodec::Vc1)));
  CHECK(amd_ryzen_identify(0x19, 0x61).soc == S::Raphael && strcmp(amd_ryzen_identify(0x19, 0x61).video_engine().firmware, "vcn_3_1_2.bin") == 0);
  CHECK(amd_ryzen_identify(0x1A, 0x44).soc == S::GraniteRidge && amd_ryzen_identify(0x1A, 0x44).core == AmdZenCore::Zen5 && amd_ryzen_identify(0x1A, 0x44).graphics == AmdGpuArchitecture::Rdna2);
  CHECK(amd_ryzen_identify(0x17, 0xA0).soc == S::Mendocino && ip_is(amd_ryzen_identify(0x17, 0xA0).dcn, 3, 1, 6));

  // Phoenix and Hawk Point share their models: the GPU decides, and the first is taken without one.
  CHECK(amd_ryzen_identify(0x19, 0x74).soc == S::Phoenix && amd_ryzen_identify(0x19, 0x74, 0x15C8).soc == S::Phoenix);
  CHECK(amd_ryzen_identify(0x19, 0x75, 0x1900).soc == S::HawkPoint && amd_ryzen_identify(0x19, 0x7C, 0x1901).soc == S::HawkPoint);
  CHECK(amd_ryzen_identify(0x19, 0x74, 0x744C).soc == S::Phoenix);   // a discrete card's ID does not move it
  AmdRyzenSocInfo phx = amd_ryzen_identify(0x19, 0x74);
  CHECK(phx.npu == AmdNpuGeneration::Xdna && phx.npu_device_id == 0x1502 && phx.video_engine().unified_queue && phx.video_engine().encode_mask() & codec_mask(VideoCodec::Av1));

  // XDNA 2 parts.
  AmdRyzenSocInfo strix = amd_ryzen_identify(0x1A, 0x24);
  CHECK(strix.soc == S::StrixPoint && strix.npu == AmdNpuGeneration::Xdna2 && strix.npu_device_id == 0x17F0 && strix.npu_revision == 0x10);
  CHECK(strix.graphics == AmdGpuArchitecture::Rdna3_5 && ip_is(strix.gc, 11, 5, 0) && strcmp(strix.video_engine().firmware, "vcn_4_0_5.bin") == 0);
  AmdRyzenSocInfo halo = amd_ryzen_identify(0x1A, 0x70);
  CHECK(halo.soc == S::StrixHalo && halo.vcn_instances == 2 && strcmp(halo.video_engine().firmware, "vcn_4_0_6.bin") == 0 && halo.npu_revision == 0x20);
  CHECK(amd_ryzen_identify(0x1A, 0x60).soc == S::KrackanPoint && amd_ryzen_identify(0x1A, 0x60).npu_revision == 0x11);
  CHECK(amd_ryzen_identify(0x1A, 0x78).soc == S::Unknown && amd_ryzen_identify(0x1A, 0x78).core == AmdZenCore::Zen5);   // (0x78..0x7F: not Strix Halo)

  // Zen 6 is recognised by its core; an unknown family gives nothing.
  CHECK(amd_ryzen_identify(0x1A, 0x50).soc == S::Zen6Client && amd_ryzen_identify(0x1A, 0x50).core == AmdZenCore::Zen6 && amd_ryzen_identify(0x1A, 0x90).soc == S::Zen6Client);
  CHECK(amd_ryzen_identify(0x1A, 0xC0).soc == S::Unknown && amd_ryzen_identify(0x1A, 0xC0).core == AmdZenCore::Zen6);
  CHECK(amd_ryzen_identify(0x15, 0x30).soc == S::Unknown && amd_ryzen_identify(0x15, 0x30).core == AmdZenCore::Unknown && !amd_ryzen_identify(0x15, 0x30).has_graphics());

  // By GPU and by NPU.
  CHECK(amd_ryzen_soc_of_gpu(0x1586).soc == S::StrixHalo && amd_ryzen_soc_of_gpu(0x1902).soc == S::KrackanPoint && amd_ryzen_soc_of_gpu(0x1435).soc == S::VanGogh);
  CHECK(amd_ryzen_soc_of_gpu(0x744C).soc == S::Unknown && amd_ryzen_soc_of_gpu(0).soc == S::Unknown);
  CHECK(amd_ryzen_soc_of_npu(0x17F0, 0x11).soc == S::KrackanPoint && amd_ryzen_soc_of_npu(0x17F0, 0x20).soc == S::StrixHalo && amd_ryzen_soc_of_npu(0x1502, 0).soc == S::Phoenix);
  CHECK(amd_ryzen_soc_of_npu(0x17F0, 0x30).soc == S::Unknown && amd_ryzen_soc_of_npu(0, 0).soc == S::Unknown);
  CHECK(strcmp(amd_ryzen_soc_name(S::StrixPoint), "Strix Point") == 0 && strcmp(amd_npu_generation_name(AmdNpuGeneration::Xdna2), "XDNA 2") == 0);
  CHECK(amd_ryzen_soc_at(amd_ryzen_soc_count()).soc == S::Unknown);
}

// Every entry holds together with the other tables.
static void test_catalogue_consistency() {
  for (uint32_t i = 0; i < amd_ryzen_soc_count(); ++i) {
    const AmdRyzenSocInfo soc = amd_ryzen_soc_at(i);
    CHECK(soc.soc != AmdRyzenSoc::Unknown && soc.codename && soc.codename[0] && soc.products && soc.core != AmdZenCore::Unknown);
    CHECK(soc.has_graphics() == !soc.gpu_device_ids.empty());
    CHECK(soc.has_graphics() == soc.has_video_engine());   // every Ryzen with graphics has a VCN, and none without
    CHECK(!soc.has_npu() || (soc.has_graphics() && soc.npu_device_id != 0));
    for (uint16_t id : soc.gpu_device_ids) {
      const AmdGpuInfo gpu = amd_gpu_identify(id);
      if (gpu.architecture != soc.graphics || gpu.discrete) {
        printf("FAILED %s: GPU %04X is %s in the GPU table\n", soc.codename, id, gpu.name);
        ++g_failed;
      }
      if (id != 0x15D8) CHECK(amd_ryzen_soc_of_gpu(id).soc == soc.soc);   // (15D8 is Picasso and Raven2)
    }
    if (soc.has_video_engine()) {
      const AmdVcnInfo vcn = soc.video_engine();
      CHECK(vcn.known() && vcn.decode_mask() & codec_mask(VideoCodec::H264) && vcn.decode_mask() & codec_mask(VideoCodec::Hevc) && vcn.encode_mask() & codec_mask(VideoCodec::H264));
      CHECK(vcn.firmware || vcn.generation == AmdVcnGeneration::Vcn1);
      CHECK(soc.vcn_instances >= 1);
      // The GPU table's media flags match the VCN version's codec lists.
      const AmdGpuInfo gpu = amd_gpu_identify(soc.gpu_device_ids[0]);
      CHECK(gpu.media_av1_decode == ((vcn.decode_mask() & codec_mask(VideoCodec::Av1)) != 0));
      CHECK(gpu.media_av1_encode == ((vcn.encode_mask() & codec_mask(VideoCodec::Av1)) != 0));
      CHECK(gpu.media_vp9 == ((vcn.decode_mask() & codec_mask(VideoCodec::Vp9)) != 0));
    }
  }
  // Every family/model pair of the Zen families gives an entry or Unknown, never a crash, and its core matches the Linux table.
  for (uint32_t family = 0x17; family <= 0x1A; ++family)
    for (uint32_t model = 0; model < 256; ++model) {
      const AmdRyzenSocInfo soc = amd_ryzen_identify(family, model);
      if (soc.soc != AmdRyzenSoc::Unknown && soc.core != amd_zen_core(family, model)) {
        printf("FAILED family %X model %X: %s is %s but the model is %s\n", family, model, soc.codename, amd_zen_core_name(soc.core), amd_zen_core_name(amd_zen_core(family, model)));
        ++g_failed;
      }
    }
}

static void test_vcn_table() {
  const AmdVcnInfo yc = amd_vcn_info({3, 1, 1});
  CHECK(yc.generation == AmdVcnGeneration::Vcn3 && yc.jpeg && !yc.unified_queue && yc.decode_count == 5 && yc.encode_count == 2);
  uint16_t w = 0, h = 0;
  CHECK(yc.decode_limit(VideoCodec::Av1, &w, &h) && w == 8192 && h == 4352 && !yc.decode_limit(VideoCodec::Mpeg2, &w, &h));
  CHECK(yc.decode_limit(VideoCodec::H264, &w, nullptr) && w == 4096);
  const AmdVcnInfo raven = amd_vcn_info({1, 0, 0});
  CHECK(raven.decode_mask() & codec_mask(VideoCodec::Mpeg4) && raven.decode_mask() & codec_mask(VideoCodec::Vc1) && !(raven.decode_mask() & codec_mask(VideoCodec::Av1)));
  CHECK(raven.decode_limit(VideoCodec::Hevc, &w, &h) && w == 4096 && h == 4096);
  CHECK(amd_vcn_info({2, 2, 0}).firmware == nullptr && strcmp(amd_vcn_info({2, 2, 0}, AmdVcnFirmwareHint::GreenSardine).firmware, "green_sardine_vcn.bin") == 0);
  CHECK(amd_vcn_info({2, 2, 0}, AmdVcnFirmwareHint::GreenSardine).generation == AmdVcnGeneration::Vcn2);
  CHECK(amd_vcn_info({4, 0, 2}, AmdVcnFirmwareHint::Renoir).firmware && strcmp(amd_vcn_info({4, 0, 2}, AmdVcnFirmwareHint::Renoir).firmware, "vcn_4_0_2.bin") == 0);   // a hint only matters for 2.2
  const AmdVcnInfo navi24 = amd_vcn_info({3, 0, 33});
  CHECK(navi24.encode_mask() == 0 && !(navi24.decode_mask() & codec_mask(VideoCodec::Av1)) && !navi24.jpeg);
  const AmdVcnInfo mi300 = amd_vcn_info({4, 0, 3});
  CHECK(mi300.encode_count == 0 && mi300.decode_mask() & codec_mask(VideoCodec::Av1));
  CHECK(amd_vcn_info({5, 0, 0}).generation == AmdVcnGeneration::Vcn5 && amd_vcn_info({5, 0, 0}).unified_queue && amd_vcn_info({5, 0, 2}).decode_count == 0);
  const AmdVcnInfo none = amd_vcn_info({9, 9, 9});
  CHECK(!none.known() && none.decode_mask() == 0 && none.encode_mask() == 0 && none.firmware == nullptr && !none.decode_limit(VideoCodec::H264, &w, &h));
  CHECK(!amd_vcn_info({0, 0, 0}).known() && strcmp(amd_vcn_generation_name(AmdVcnGeneration::Vcn4), "VCN 4") == 0);
  // Every byte of version: no entry reads out of its arrays.
  for (uint32_t major = 0; major < 8; ++major)
    for (uint32_t minor = 0; minor < 8; ++minor)
      for (uint32_t revision = 0; revision < 256; ++revision) {
        const AmdVcnInfo info = amd_vcn_info({static_cast<uint8_t>(major), static_cast<uint8_t>(minor), static_cast<uint8_t>(revision)});
        CHECK(info.decode_count <= kAmdVcnMaxCodecs && info.encode_count <= kAmdVcnMaxCodecs);
      }
}

// A PCI bus that lists a few devices and answers the class/revision word.
class FakePci final : public IPciBus {
 public:
  struct Device {
    PciDeviceInfo info;
    uint8_t revision;
  };
  Device devices[6] = {};
  uint32_t count = 0;
  mutable uint32_t reads = 0;
  void add(uint16_t vendor, uint16_t device, uint8_t cls, uint8_t revision = 0) {
    devices[count] = {{{static_cast<uint8_t>(count + 1), 0, 0}, vendor, device, cls, 0, 0}, revision};
    ++count;
  }
  const char* name() const override { return "fake"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return count; }
  bool pci_device(uint32_t i, PciDeviceInfo* out) const override {
    if (i >= count) return false;
    *out = devices[i].info;
    return true;
  }
  uint32_t read32(PciAddress a, uint16_t offset) const override {
    ++reads;
    for (uint32_t i = 0; i < count; ++i)
      if (devices[i].info.address.bus == a.bus && offset == 0x08) return (static_cast<uint32_t>(devices[i].info.class_code) << 24) | devices[i].revision;
    return 0xFFFFFFFF;
  }
  void write32(PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

static void test_machine() {
  constexpr uint8_t kAccelerator = 0x12;
  {
    // A Hawk Point laptop: the CPU model alone would say Phoenix; the GPU on the bus says Hawk Point. A discrete card beside it is not the platform's.
    FakePci pci;
    pci.add(0x1022, 0x14E8, 0x06);
    pci.add(0x10DE, 0x28E0, kPciClassDisplay);
    pci.add(0x1002, 0x744C, kPciClassDisplay);
    pci.add(0x1002, 0x1900, kPciClassDisplay, 0xC1);
    pci.add(0x1022, 0x1502, kAccelerator, 0x00);
    const AmdRyzenMachine m = amd_ryzen_identify_machine(0x19, 0x75, pci);
    CHECK(m.soc.soc == AmdRyzenSoc::HawkPoint && m.gpu_device_id == 0x1900 && m.gpu_address.bus == 4 && m.npu_device_id == 0x1502 && m.npu_address.bus == 5);
    CHECK(pci.reads == 1);   // only the NPU's revision is read
    FixedString<256> text;
    amd_ryzen_describe(m, text);
    CHECK(strstr(text.c_str(), "Hawk Point (Ryzen 8040") && strstr(text.c_str(), "Zen 4") && strstr(text.c_str(), "VCN 4.0.2 (vcn_4_0_2.bin)") && strstr(text.c_str(), "XDNA NPU rev 0"));
    CHECK(!strstr(text.c_str(), "not on the bus"));
  }
  {
    // Strix Halo: two video engines, the XDNA 2 NPU by its revision.
    FakePci pci;
    pci.add(0x1002, 0x1586, kPciClassDisplay);
    pci.add(0x1022, 0x17F0, kAccelerator, 0x20);
    const AmdRyzenMachine m = amd_ryzen_identify_machine(0x1A, 0x70, pci);
    FixedString<256> text;
    amd_ryzen_describe(m, text);
    CHECK(m.soc.soc == AmdRyzenSoc::StrixHalo && m.npu_revision == 0x20 && strstr(text.c_str(), "VCN 4.0.6 x2 (vcn_4_0_6.bin)") && strstr(text.c_str(), "XDNA 2 NPU rev 32"));
    CHECK(strstr(text.c_str(), "GC 11.5.1") && strstr(text.c_str(), "DCN 3.5.1"));
  }
  {
    // A Phoenix2 part: graphics, no NPU on the bus; and an empty bus says what it expected.
    FakePci pci;
    pci.add(0x1002, 0x15C8, kPciClassDisplay);
    const AmdRyzenMachine m = amd_ryzen_identify_machine(0x19, 0x78, pci);
    FixedString<256> text;
    amd_ryzen_describe(m, text);
    CHECK(m.soc.soc == AmdRyzenSoc::Phoenix && !m.npu_found() && strstr(text.c_str(), "NPU not on the bus"));
    FakePci empty;
    const AmdRyzenMachine e = amd_ryzen_identify_machine(0x1A, 0x24, empty);
    amd_ryzen_describe(e, text);
    CHECK(e.soc.soc == AmdRyzenSoc::StrixPoint && !e.gpu_found() && strstr(text.c_str(), "(RDNA 3.5) not on the bus"));
  }
  {
    // Desktop parts, Zen 6, and a non-Zen CPU.
    FakePci pci;
    pci.add(0x1002, 0x7550, kPciClassDisplay);
    FixedString<256> text;
    amd_ryzen_describe(amd_ryzen_identify_machine(0x19, 0x21, pci), text);
    CHECK(strstr(text.c_str(), "Vermeer") && strstr(text.c_str(), "no integrated graphics") && !strstr(text.c_str(), "NPU"));
    amd_ryzen_describe(amd_ryzen_identify_machine(0x1A, 0xC0, pci), text);
    CHECK(strcmp(text.c_str(), "unknown AMD processor (Zen 6)") == 0);
    amd_ryzen_describe(amd_ryzen_identify_machine(0x15, 0x02, pci), text);
    CHECK(strstr(text.c_str(), "not a Zen core"));
  }
  // Every entry's description fits.
  for (uint32_t i = 0; i < amd_ryzen_soc_count(); ++i) {
    AmdRyzenMachine m;
    m.soc = amd_ryzen_soc_at(i);
    m.gpu_device_id = 1;
    m.npu_device_id = 1;
    FixedString<256> text;
    amd_ryzen_describe(m, text);
    CHECK(!text.empty() && strstr(text.c_str(), m.soc.codename));
    if (m.soc.has_npu()) CHECK(strstr(text.c_str(), "NPU rev"));   // (the last part: nothing was cut off)
  }
}

int main() {
  test_zen_cores();
  test_identify();
  test_catalogue_consistency();
  test_vcn_table();
  test_machine();
  if (g_failed) {
    printf("AmdRyzenPlatformTest: %d failure(s)\n", g_failed);
    return 1;
  }
  printf("AmdRyzenPlatformTest: all tests passed\n");
  return 0;
}
