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

// Host-side tests for the probe of a discrete Radeon (AmdGpuProbe) against a register model: which layout each generation gets, the memory size per family,
// the temperature conversions, the video BIOS strings (also truncated and bit-flipped images), a card that does not answer, and a resume after the model lost
// its state in a power cut. The register numbers are typed in here from AMD's headers (bif_5_0_d.h, nbio_*_offset.h, thm_9_0/thm_11_0_2_offset.h, smu_7_1_3_d.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuProbe.hpp"

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

constexpr uint32_t kConfigMemsize = 0x150a, kRccConfigMemsize = 0xD20 + 0xc3;
constexpr uint32_t kSmcIndex = 0x1ac, kSmcData = 0x1ad, kIxCgMultThermalStatus = 0xc0300014;
constexpr uint32_t kIxRomIndex = 0xc0600010, kIxRomData = 0xc0600014;
constexpr uint32_t kThm9Status = 0x16600 + 0x5a, kThm11Status = 0x16600 + 0x5f;
constexpr uint64_t MiB = 1ull << 20;

// A card's registers: plain dwords, and behind SMC_IND_INDEX_11 / SMC_IND_DATA_11 the SMC's space (with the ROM read a dword at a time through ROM_INDEX / ROM_DATA).
struct FakeRegs final : IVcnRegisters {
  std::map<uint32_t, uint32_t> regs, smc;
  std::vector<uint8_t> rom;
  uint32_t index = 0, rom_position = 0, writes = 0;
  bool dead = false;
  uint32_t read(uint32_t r) override {
    if (dead) return 0xFFFFFFFFu;
    if (r == kSmcData) {
      if (index == kIxRomData) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < 4; ++i) v |= uint32_t{rom_position + i < rom.size() ? rom[rom_position + i] : uint8_t{0xFF}} << (8 * i);
        rom_position += 4;
        return v;
      }
      auto it = smc.find(index);
      return it == smc.end() ? 0 : it->second;
    }
    auto it = regs.find(r);
    return it == regs.end() ? 0 : it->second;
  }
  void write(uint32_t r, uint32_t v) override {
    ++writes;
    if (dead) return;
    if (r == kSmcIndex) { index = v; return; }
    if (r == kSmcData && index == kIxRomIndex) { rom_position = v; return; }
    if (r == kSmcData) { smc[index] = v; return; }
    regs[r] = v;
  }
  // A power cut: everything the VBIOS set up (the memory size) is gone, the sensors read again once the card runs.
  void power_cut() { regs.clear(); smc.clear(); index = 0; rom_position = 0; }
};

static void put16(std::vector<uint8_t>& v, size_t at, uint32_t x) { v[at] = static_cast<uint8_t>(x); v[at + 1] = static_cast<uint8_t>(x >> 8); }
static void put_string(std::vector<uint8_t>& v, size_t at, const char* s) { memcpy(v.data() + at, s, strlen(s) + 1); }

// An AtomBIOS image of 4 KiB as amdgpu checks it (0xAA55, the " 761295520" signature at 0x30, a header at 0x100 with "ATOM" at +4), with the four strings
// at 0x200 that atom_get_vbios_pn / atom_get_vbios_name read, then "\r\n" and the name.
static std::vector<uint8_t> vbios_image() {
  std::vector<uint8_t> v(4096, 0);
  v[0] = 0x55; v[1] = 0xAA; v[2] = 8;
  memcpy(v.data() + 0x30, " 761295520", 10);
  put16(v, 0x48, 0x100);
  memcpy(v.data() + 0x104, "ATOM", 4);
  v[0x2F] = 4;
  put16(v, 0x6E, 0x200);
  size_t at = 0x200;
  for (const char* s : {"113-C9410100-102", "POLARIS11", "PCI_EXPRESS", "GDDR5"}) { put_string(v, at, s); at += strlen(s) + 1; }
  v[at] = 0x0D; v[at + 1] = 0x0A;
  put_string(v, at + 2, "C94101 Polaris11 XT A1 GDDR5 4GB   ");
  return v;
}

static std::string pn(const std::vector<uint8_t>& image) {
  char out[AmdGpuProbe::kPartNumberChars];
  AmdGpuProbe::vbios_part_number(ConstByteView(image.data(), image.size()), ArrayView<char>(out));
  return out;
}
static std::string vname(const std::vector<uint8_t>& image) {
  char out[AmdGpuProbe::kNameChars];
  AmdGpuProbe::vbios_name(ConstByteView(image.data(), image.size()), ArrayView<char>(out));
  return out;
}

static void test_layouts() {
  using L = AmdGpuProbe::Layout;
  CHECK(AmdGpuProbe::layout_of(0x67DF) == L::Vi);         // RX 480/580 (Polaris 10)
  CHECK(AmdGpuProbe::layout_of(0x67EF) == L::Vi);         // RX 460/560 mobile (Polaris 11)
  CHECK(AmdGpuProbe::layout_of(0x699F) == L::Vi);         // RX 550 (Polaris 12)
  CHECK(AmdGpuProbe::layout_of(0x694C) == L::Vi);         // Vega M GH
  CHECK(AmdGpuProbe::layout_of(0x694E) == L::Vi);         // Vega M GL
  CHECK(AmdGpuProbe::layout_of(0x687F) == L::Vega10);     // RX Vega 64
  CHECK(AmdGpuProbe::layout_of(0x69AF) == L::Vega10);     // Vega 12 (thm 9.0 as Vega 10)
  CHECK(AmdGpuProbe::layout_of(0x66AF) == L::Vega20);     // Radeon VII
  CHECK(AmdGpuProbe::layout_of(0x731F) == L::Navi);       // RX 5700 XT
  CHECK(AmdGpuProbe::layout_of(0x7340) == L::Navi);       // RX 5500M
  CHECK(AmdGpuProbe::layout_of(0x73DF) == L::Navi);       // RX 6700 XT / 6800M
  CHECK(AmdGpuProbe::layout_of(0x73EF) == L::Navi);       // RX 6600M class
  CHECK(AmdGpuProbe::layout_of(0x743F) == L::Navi);       // RX 6500M (Navi 24)
  CHECK(AmdGpuProbe::layout_of(0x744C) == L::Discovery);  // RX 7900 XTX
  CHECK(AmdGpuProbe::layout_of(0x7480) == L::Discovery);  // RX 7600M (Navi 33)
  CHECK(AmdGpuProbe::layout_of(0x7550) == L::Discovery);  // RX 9070
  // Not for the probe: APUs, Instinct, other IDs.
  CHECK(AmdGpuProbe::layout_of(0x15DD) == L::None);
  CHECK(AmdGpuProbe::layout_of(0x1681) == L::None);
  CHECK(AmdGpuProbe::layout_of(0x15BF) == L::None);
  CHECK(AmdGpuProbe::layout_of(0x738C) == L::None);
  CHECK(AmdGpuProbe::layout_of(0x0000) == L::None);
  CHECK(AmdGpuProbe::layout_of(0xFFFF) == L::None);
  CHECK(!AmdGpuProbe::supported(0x1636) && AmdGpuProbe::supported(0x73FF));
}

static void test_conversions() {
  // CTF_TEMP is bits 17:9 in whole degrees.
  CHECK(AmdGpuProbe::smu7_temperature(45u << 9) == 45000);
  CHECK(AmdGpuProbe::smu7_temperature((45u << 9) | 0x1FF) == 45000);   // ASIC_MAX_TEMP (bits 8:0) is not the reading
  CHECK(AmdGpuProbe::smu7_temperature(0x1FFu << 9) == 511000);
  CHECK(AmdGpuProbe::smu7_temperature(0xFFFFFFFFu) == -1);
  CHECK(AmdGpuProbe::smu7_temperature(0) == -1);
  CHECK(AmdGpuProbe::smu7_temperature(0xFFFC0000u | (60u << 9)) == 60000);   // other fields above are ignored
  CHECK(AmdGpuProbe::thm_temperature(72u << 9) == 72000);
  CHECK(AmdGpuProbe::thm_temperature(0xFFFFFFFFu) == -1);
  CHECK(AmdGpuProbe::thm_temperature(0x1FF) == -1);
  // The memory size in MiB, the VI garbage rule.
  CHECK(AmdGpuProbe::memsize_bytes(4096, true) == 4096 * MiB);
  CHECK(AmdGpuProbe::memsize_bytes(0x12341000, true) == 4096 * MiB);
  CHECK(AmdGpuProbe::memsize_bytes(0x12341000, false) == 0);   // over the plausible limit, and the rule is for VI only
  CHECK(AmdGpuProbe::memsize_bytes(0x00040000, true) == 0);    // garbage with nothing in the low half: 256 GiB, refused
  CHECK(AmdGpuProbe::memsize_bytes(0xFFFFFFFFu, true) == 0 && AmdGpuProbe::memsize_bytes(0xFFFFFFFFu, false) == 0);
  CHECK(AmdGpuProbe::memsize_bytes(0, false) == 0);
  CHECK(AmdGpuProbe::memsize_bytes(24576, false) == 24576 * MiB);
}

static void test_vbios_strings() {
  std::vector<uint8_t> v = vbios_image();
  CHECK(pn(v) == "113-C9410100-102");
  CHECK(vname(v) == "C94101 Polaris11 XT A1 GDDR5 4GB");   // the trailing spaces taken off
  // No string table: the part number at 0x80, no name.
  std::vector<uint8_t> old = vbios_image();
  old[0x2F] = 0;
  put_string(old, 0x80, "113-OLDPART");
  CHECK(pn(old) == "113-OLDPART");
  CHECK(vname(old).empty());
  // Nothing at 0x80 either: the "ATOMBIOS" prefix is searched for in the first KiB.
  std::vector<uint8_t> prefixed = vbios_image();
  prefixed[0x2F] = 0;
  memcpy(prefixed.data() + 0x60, "ATOMBIOSBK-AMD VER015", 21);
  CHECK(pn(prefixed) == "ATOMBIOSBK-AMD VER015");
  std::vector<uint8_t> none = vbios_image();
  none[0x2F] = 0;
  CHECK(pn(none).empty());
  // A part number longer than BIOS_STRING_LENGTH stops at 43 characters; characters past 'z' end it.
  std::vector<uint8_t> longer = vbios_image();
  put_string(longer, 0x200, "0123456789012345678901234567890123456789ABCDEFGH");
  CHECK(pn(longer).size() == 43);
  std::vector<uint8_t> odd = vbios_image();
  odd[0x205] = '~';
  CHECK(pn(odd) == "113-C");
  // A string table pointer past the end: the prefix search, then nothing; the name is gone too.
  std::vector<uint8_t> away = vbios_image();
  put16(away, 0x6E, 0xFFF0 );
  CHECK(pn(away).empty() && vname(away).empty());
  // An empty image, a tiny one, an output of one character.
  CHECK(pn({}).empty() && vname({}).empty());
  CHECK(pn(std::vector<uint8_t>(0x70, 0)).empty() && vname(std::vector<uint8_t>(0x70, 0)).empty());
  char one[1] = {'x'};
  AmdGpuProbe::vbios_part_number(ConstByteView(v.data(), v.size()), ArrayView<char>(one));
  CHECK(one[0] == 0);
  // Every truncation and every single bit flip: no crash (the sanitizers check the reads), strings always terminated and within bounds.
  for (size_t n = 0; n <= 0x280; ++n) {
    std::vector<uint8_t> cut(v.begin(), v.begin() + n);
    CHECK(pn(cut).size() < AmdGpuProbe::kPartNumberChars && vname(cut).size() < AmdGpuProbe::kNameChars);
  }
  for (size_t at = 0; at < 0x280; ++at)
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<uint8_t> flipped = v;
      flipped[at] ^= static_cast<uint8_t>(1u << bit);
      (void)pn(flipped);
      (void)vname(flipped);
    }
}

static void test_polaris() {
  TestAllocator alloc;
  FakeRegs r;
  r.regs[kConfigMemsize] = 4096;
  r.smc[kIxCgMultThermalStatus] = (51u << 9) | 0x60;
  r.rom = vbios_image();
  AmdGpuProbe probe(r, 0x67EF, alloc);
  CHECK(strcmp(probe.status(), "not started") == 0);
  CHECK(probe.temperature_millicelsius() == -1);   // not started
  CHECK(probe.start());
  CHECK(strcmp(probe.status(), "identified") == 0);
  CHECK(strstr(probe.name(), "Polaris") != nullptr);
  CHECK(probe.info().architecture == AmdGpuArchitecture::Polaris && probe.layout() == AmdGpuProbe::Layout::Vi);
  CHECK(probe.vram_bytes() == 4096 * MiB);
  CHECK(probe.has_vbios() && strcmp(probe.vbios_part_number(), "113-C9410100-102") == 0);
  CHECK(strcmp(probe.vbios_name(), "C94101 Polaris11 XT A1 GDDR5 4GB") == 0);
  CHECK(probe.temperature_millicelsius() == 51000);
  r.smc[kIxCgMultThermalStatus] = 63u << 9;
  CHECK(probe.temperature_millicelsius() == 63000);
  // Only index registers were written: nothing in the plain register space, nothing into the SMC's.
  CHECK(r.regs.size() == 1 && r.smc.size() == 1);
  // Power cut: the memory size reads 0 (not POSTed again), the probe keeps what it had; the sensor answers once the card runs.
  r.power_cut();
  CHECK(probe.resume());
  CHECK(probe.vram_bytes() == 4096 * MiB);
  CHECK(strstr(probe.status(), "power cut") != nullptr);
  CHECK(probe.temperature_millicelsius() == -1);
  r.smc[kIxCgMultThermalStatus] = 40u << 9;
  CHECK(probe.temperature_millicelsius() == 40000);
  r.regs[kConfigMemsize] = 4096;
  CHECK(probe.resume() && strcmp(probe.status(), "identified") == 0);
  // The card stops answering.
  r.dead = true;
  CHECK(!probe.resume());
  CHECK(probe.temperature_millicelsius() == -1);
}

static void test_vega_m_without_rom() {
  TestAllocator alloc;
  FakeRegs r;
  r.regs[kConfigMemsize] = 4096;
  r.smc[kIxCgMultThermalStatus] = 58u << 9;
  AmdGpuProbe probe(r, 0x694C, alloc);   // no ROM behind the SMC: all ones, which is no image
  CHECK(probe.start());
  CHECK(strstr(probe.name(), "Vega M") != nullptr);
  CHECK(!probe.has_vbios() && probe.vbios_part_number()[0] == 0 && probe.vbios_name()[0] == 0);
  CHECK(probe.vram_bytes() == 4096 * MiB && probe.temperature_millicelsius() == 58000);
}

static void test_vega_and_navi() {
  TestAllocator alloc;
  {
    FakeRegs r;
    r.regs[kRccConfigMemsize] = 8192;
    r.regs[kThm9Status] = 66u << 9;
    r.regs[kThm11Status] = 99u << 9;   // (the other layout's register: not read)
    AmdGpuProbe probe(r, 0x687F, alloc);
    CHECK(probe.start() && probe.vram_bytes() == 8192 * MiB && probe.temperature_millicelsius() == 66000);
    CHECK(!probe.has_vbios() && r.writes == 0);   // nothing written at all
  }
  {
    FakeRegs r;
    r.regs[kRccConfigMemsize] = 16384;
    r.regs[kThm9Status] = 99u << 9;
    r.regs[kThm11Status] = 70u << 9;
    AmdGpuProbe probe(r, 0x66AF, alloc);
    CHECK(probe.start() && probe.vram_bytes() == 16384 * MiB && probe.temperature_millicelsius() == 70000);
  }
  {
    // RDNA 2 laptop part, with an image handed in.
    FakeRegs r;
    r.regs[kRccConfigMemsize] = 12288;
    r.regs[kThm11Status] = 48u << 9;
    const std::vector<uint8_t> image = vbios_image();
    AmdGpuProbe probe(r, 0x73DF, alloc, ConstByteView(image.data(), image.size()));
    CHECK(probe.start() && probe.vram_bytes() == 12288 * MiB && probe.temperature_millicelsius() == 48000);
    CHECK(probe.has_vbios() && strcmp(probe.vbios_part_number(), "113-C9410100-102") == 0 && r.writes == 0);
    r.regs[kThm11Status] = 200u << 9;   // not believed on Navi
    CHECK(probe.temperature_millicelsius() == -1);
    // A power cut keeps the handed-in image and the size.
    r.power_cut();
    CHECK(probe.resume() && probe.vram_bytes() == 12288 * MiB && probe.has_vbios());
  }
  {
    FakeRegs r;
    r.regs[kRccConfigMemsize] = 8192;
    r.regs[kThm11Status] = 50u << 9;
    AmdGpuProbe probe(r, 0x7480, alloc);   // RDNA 3: no temperature
    CHECK(probe.start() && probe.vram_bytes() == 8192 * MiB && probe.temperature_millicelsius() == -1);
  }
  {
    FakeRegs r;   // a card nobody POSTed: memory size 0
    AmdGpuProbe probe(r, 0x7340, alloc);
    CHECK(probe.start() && probe.vram_bytes() == 0 && strstr(probe.status(), "reads 0") != nullptr);
  }
}

static void test_refused() {
  TestAllocator alloc;
  FakeRegs r;
  r.dead = true;
  AmdGpuProbe dead(r, 0x73DF, alloc);
  CHECK(!dead.start() && strstr(dead.status(), "does not answer") != nullptr);
  FakeRegs r2;
  r2.dead = true;
  AmdGpuProbe polaris(r2, 0x67DF, alloc);
  CHECK(!polaris.start());
  FakeRegs ok;
  ok.regs[kRccConfigMemsize] = 512;
  AmdGpuProbe apu(ok, 0x15DD, alloc);
  CHECK(!apu.start() && ok.writes == 0);
  AmdGpuProbe unstarted(ok, 0x73DF, alloc);
  CHECK(!unstarted.resume() && unstarted.temperature_millicelsius() == -1);   // not started
  // On the machine without a register mapper.
  GpuContext context{};
  context.device.vendor_id = 0x1002;
  context.device.device_id = 0x73DF;
  AmdGpuProbe machine(context, alloc);
  CHECK(!machine.start() && strstr(machine.status(), "mapper") != nullptr);
  CHECK(!machine.resume() && machine.temperature_millicelsius() == -1);
}

int main() {
  test_layouts();
  test_conversions();
  test_vbios_strings();
  test_polaris();
  test_vega_m_without_rom();
  test_vega_and_navi();
  test_refused();
  if (g_failed) {
    printf("AmdGpuProbeTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("AmdGpuProbeTest: all passed\n");
  return 0;
}
