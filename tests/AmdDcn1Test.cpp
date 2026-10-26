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

// Host-side tests for the AMD (Raven, DCN 1.0) display path: finding the plane that shows the firmware's framebuffer, the order and checks of the surface address
// write, putting the old address back when the engine does not take the new one, and the framebuffer set up in the (simulated) video memory carve-out.
// The register numbers below are typed in from AMD's headers, separately from the code under test. The registers are a simulated file: nothing here has
// met a real APU.
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdScanout.hpp"

using namespace hardware;
using namespace hardware::amd_dcn1;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

constexpr uint64_t kMiB = 1ull << 20;

// dcn_1_0_offset.h: DCN segment 2 (0x34C0 in the Vega10 table) and the registers of pipe 0; each next pipe is 0xC4 further.
constexpr uint32_t kBase = 0x34C0, kPipeStride = 0xC4;
constexpr uint32_t kSurfaceConfig = 0x0559, kTilingConfig = 0x055B, kHubpCntl = 0x0566;
constexpr uint32_t kPitch = 0x057B, kPrimaryLow = 0x057D, kPrimaryHigh = 0x057E, kSurfaceControl = 0x058D, kFlipControl = 0x058E, kInUseLow = 0x0597, kInUseHigh = 0x0598;
constexpr uint32_t kAptLowLsb = 0x05A4, kAptLowMsb = 0x05A5, kAptHighLsb = 0x05A6, kAptHighMsb = 0x05A7;
// sh_mask: the fields the code looks at.
constexpr uint32_t kHubpDisable = 0x4, kUnderflow = 0x70000000, kFlipPending = 0x80000000, kDccEnable = 0x2;

constexpr uint32_t reg(uint32_t pipe, uint32_t number) { return kBase + number + kPipeStride * pipe; }

namespace {
// A display engine whose planes latch a new surface address at once (or after `latency` reads of the in-use address, or never).
struct Dcn final : IVcnRegisters {
  std::map<uint32_t, uint32_t> value;
  std::vector<uint32_t> writes;   // the dword numbers, in order
  int latency = 0;                // reads of the in-use address before it follows the programmed one
  bool stuck = false;             // never follows
  bool underflow_after_flip = false;
  uint32_t pending_pipe = 0xFF;
  int countdown = 0;
  uint32_t read(uint32_t dword) override {
    auto it = value.find(dword);
    const uint32_t v = it == value.end() ? 0xFFFFFFFFu : it->second;
    for (uint32_t p = 0; p < kPipes; ++p) {
      if (dword == reg(p, kInUseLow) && p == pending_pipe && --countdown <= 0) {
        value[reg(p, kInUseLow)] = value[reg(p, kPrimaryLow)];
        value[reg(p, kInUseHigh)] = value[reg(p, kPrimaryHigh)];
        value[reg(p, kFlipControl)] &= ~kFlipPending;
        if (underflow_after_flip) value[reg(p, kHubpCntl)] |= 0x10000000;
        pending_pipe = 0xFF;
        return value[dword];
      }
    }
    return v;
  }
  void write(uint32_t dword, uint32_t v) override {
    writes.push_back(dword);
    value[dword] = v;
    for (uint32_t p = 0; p < kPipes; ++p) {
      if (dword == reg(p, kPrimaryLow) && !stuck) {   // the low half latches the address
        value[reg(p, kFlipControl)] |= kFlipPending;
        pending_pipe = p;
        countdown = latency;
      }
    }
  }
};

constexpr uint64_t kFirmwareSurface = 0xF400000000ull;   // the firmware's framebuffer as the GPU addresses it
constexpr uint32_t kPitchPixels = 1920;

// The firmware's state: pipe `p` shows the framebuffer, linear ARGB8888, system aperture over 512 MiB of video memory at 0xF400000000.
void light(Dcn& d, uint32_t p, uint64_t surface = kFirmwareSurface, uint32_t pitch = kPitchPixels) {
  d.value[reg(p, kHubpCntl)] = 0;
  d.value[reg(p, kSurfaceConfig)] = 8;
  d.value[reg(p, kTilingConfig)] = 0;
  d.value[reg(p, kSurfaceControl)] = 0;
  d.value[reg(p, kPitch)] = pitch - 1;
  d.value[reg(p, kPrimaryLow)] = static_cast<uint32_t>(surface);
  d.value[reg(p, kPrimaryHigh)] = static_cast<uint32_t>(surface >> 32);
  d.value[reg(p, kInUseLow)] = static_cast<uint32_t>(surface);
  d.value[reg(p, kInUseHigh)] = static_cast<uint32_t>(surface >> 32);
  d.value[reg(p, kFlipControl)] = 0;
  const uint64_t low = 0xF400000000ull >> 12, high = (0xF400000000ull + 512 * kMiB - 1) >> 12;
  d.value[reg(p, kAptLowLsb)] = static_cast<uint32_t>(low);
  d.value[reg(p, kAptLowMsb)] = static_cast<uint32_t>(low >> 32);
  d.value[reg(p, kAptHighLsb)] = static_cast<uint32_t>(high);
  d.value[reg(p, kAptHighMsb)] = static_cast<uint32_t>(high >> 32);
}
}  // namespace

static void test_probe() {
  Dcn d;
  light(d, 2);
  light(d, 1, kFirmwareSurface + 0x100000);   // another pipe showing something else
  d.value[reg(0, kHubpCntl)] = kHubpDisable;
  Plane plane(d);
  CHECK(plane.probe(kFirmwareSurface, kPitchPixels) && plane.active() && plane.pipe() == 2 && plane.original_surface() == kFirmwareSurface);
  CHECK(plane.aperture_low() == 0xF400000000ull && plane.aperture_high() == 0xF400000000ull + 512 * kMiB - 1);
  CHECK(plane.surface() == kFirmwareSurface);
  CHECK(d.writes.empty());   // looking changes nothing

  // The things that make it refuse the plane.
  struct Case { const char* what; void (*spoil)(Dcn&); };
  const Case cases[] = {
      {"another pitch", [](Dcn& x) { x.value[reg(2, kPitch)] = 2559; }},
      {"another address", [](Dcn& x) { x.value[reg(2, kPrimaryLow)] += 0x1000; }},
      {"tiled", [](Dcn& x) { x.value[reg(2, kTilingConfig)] = 9; }},
      {"compressed", [](Dcn& x) { x.value[reg(2, kSurfaceControl)] = kDccEnable; }},
      {"not ARGB8888", [](Dcn& x) { x.value[reg(2, kSurfaceConfig)] = 10; }},
      {"rotated", [](Dcn& x) { x.value[reg(2, kSurfaceConfig)] = 8 | (1u << 8); }},
      {"mirrored", [](Dcn& x) { x.value[reg(2, kSurfaceConfig)] = 8 | (1u << 10); }},
      {"disabled", [](Dcn& x) { x.value[reg(2, kHubpCntl)] = kHubpDisable; }},
      {"flip pending", [](Dcn& x) { x.value[reg(2, kFlipControl)] = kFlipPending; }},
      {"empty aperture", [](Dcn& x) { x.value[reg(2, kAptHighLsb)] = 0; x.value[reg(2, kAptHighMsb)] = 0; }},
  };
  for (const Case& c : cases) {
    Dcn x;
    light(x, 2);
    c.spoil(x);
    Plane p(x);
    if (p.probe(kFirmwareSurface, kPitchPixels)) { printf("accepted a plane that is %s\n", c.what); ++g_failed; }
    CHECK(!p.active() && x.writes.empty());
  }
  Dcn off;   // an engine that is powered down reads all ones
  Plane none(off);
  CHECK(!none.probe(kFirmwareSurface, kPitchPixels));
  CHECK(!plane.probe(0, kPitchPixels) && !plane.probe(kFirmwareSurface, 0) && !plane.probe(kFirmwareSurface, 0x10000));
}

static void test_set_surface() {
  const uint64_t fresh = kFirmwareSurface + 100 * kMiB;
  const uint64_t bytes = 8 * kMiB;
  Dcn d;
  light(d, 3);
  Plane plane(d);
  CHECK(plane.probe(kFirmwareSurface, kPitchPixels));
  CHECK(plane.in_aperture(fresh, bytes) && !plane.in_aperture(fresh, 512 * kMiB) && !plane.in_aperture(0xF400000000ull - 4096, 4096));
  CHECK(!plane.in_aperture(0xF400000000ull + 512 * kMiB - 4096, 8192) && plane.in_aperture(0xF400000000ull + 512 * kMiB - 4096, 4096));

  // Refused before anything is written.
  CHECK(!plane.set_surface(fresh + 16, bytes) && !plane.set_surface(0, bytes) && !plane.set_surface(fresh, 0) && !plane.set_surface(0xF400000000ull + 600 * kMiB, bytes));
  CHECK(d.writes.empty());
  d.value[reg(3, kFlipControl)] = kFlipPending;
  CHECK(!plane.set_surface(fresh, bytes) && d.writes.empty());
  d.value[reg(3, kFlipControl)] = 0;

  d.latency = 3;   // the engine uses the new address a few reads later
  CHECK(plane.set_surface(fresh, bytes));
  CHECK(plane.surface() == fresh);
  CHECK(d.writes.size() == 2 && d.writes[0] == reg(3, kPrimaryHigh) && d.writes[1] == reg(3, kPrimaryLow));   // high half first: the low one latches
  CHECK(d.value[reg(3, kPrimaryHigh)] == 0xF4 && d.value[reg(3, kPrimaryLow)] == static_cast<uint32_t>(fresh));

  // Back to the firmware's.
  d.writes.clear();
  CHECK(plane.restore() && plane.surface() == kFirmwareSurface && d.value[reg(3, kInUseLow)] == static_cast<uint32_t>(kFirmwareSurface));
  d.writes.clear();
  CHECK(plane.restore() && d.writes.empty());   // already there

  // The engine does not take the address: the old one is written back.
  Dcn stuck;
  light(stuck, 0);
  Plane sp(stuck);
  CHECK(sp.probe(kFirmwareSurface, kPitchPixels));
  stuck.stuck = true;
  CHECK(!sp.set_surface(fresh, bytes));
  CHECK(stuck.value[reg(0, kPrimaryLow)] == static_cast<uint32_t>(kFirmwareSurface) && stuck.value[reg(0, kPrimaryHigh)] == 0xF4);

  // It takes the address but reports an underflow it did not have before: put back.
  Dcn under;
  light(under, 1);
  Plane up(under);
  CHECK(up.probe(kFirmwareSurface, kPitchPixels));
  under.underflow_after_flip = true;
  CHECK(!up.set_surface(fresh, bytes));
  CHECK(under.value[reg(1, kPrimaryLow)] == static_cast<uint32_t>(kFirmwareSurface));
  // ... but an underflow that was already there is not held against the change.
  Dcn already;
  light(already, 1);
  already.value[reg(1, kHubpCntl)] = 0x10000000;
  Plane ap(already);
  CHECK(ap.probe(kFirmwareSurface, kPitchPixels) && ap.set_surface(fresh, bytes));
}

// The carve-out, in host memory: the CPU address of video memory is the address of the allocation.
namespace {
struct Mapper final : IRegisterMapper {
  uint64_t lo = 0, hi = 0;
  bool refuse = false;
  uint32_t maps = 0;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    ++maps;
    if (refuse || physical < lo || physical > hi || length > hi - physical) return false;
    *region = MmioRegion(physical, length);
    return true;
  }
};
}  // namespace

static void test_scanout() {
  constexpr uint64_t kVideo = 128 * kMiB;
  uint8_t* memory = static_cast<uint8_t*>(aligned_alloc(4096, kVideo));
  for (uint64_t i = 0; i < kVideo; ++i) memory[i] = static_cast<uint8_t>(i * 7);
  AmdRavenMemoryMap map;
  map.read = true;
  map.video_memory_bytes = kVideo;
  map.gpu_base = 0xF400000000ull;
  map.cpu_base = reinterpret_cast<uint64_t>(memory);
  map.hardware_gb_addr_config = 0x24000042;
  Mapper mapper;
  mapper.lo = map.cpu_base;
  mapper.hi = map.cpu_base + kVideo - 1;

  FirmwareFramebuffer fb;
  fb.physical = map.cpu_base;                 // the firmware's framebuffer is at the start of video memory
  fb.width = 1920;
  fb.height = 1080;
  fb.pitch_pixels = 1920;
  fb.bytes = 1920ull * 1080 * 4;
  const uint64_t picture = fb.bytes;
  for (uint64_t i = 0; i < picture; ++i) memory[i] = static_cast<uint8_t>((i * 31) ^ (i >> 8));   // what the firmware has drawn
  std::vector<uint8_t> original(memory, memory + picture);

  auto apt = [](Dcn& d, uint32_t pipe) {   // an aperture over this carve-out
    const uint64_t low = 0xF400000000ull >> 12, high = (0xF400000000ull + kVideo - 1) >> 12;
    d.value[reg(pipe, kAptLowLsb)] = static_cast<uint32_t>(low);
    d.value[reg(pipe, kAptLowMsb)] = static_cast<uint32_t>(low >> 32);
    d.value[reg(pipe, kAptHighLsb)] = static_cast<uint32_t>(high);
    d.value[reg(pipe, kAptHighMsb)] = static_cast<uint32_t>(high >> 32);
  };
  const uint64_t reserved = Scanout::region_bytes(picture) + kMiB;
  CHECK(reserved % kMiB == 0 && reserved > picture);

  {
    Dcn d;
    light(d, 1);
    apt(d, 1);
    Scanout scanout(d, mapper, map, reserved);
    ByteView ours;
    CHECK(scanout.engage(fb, &ours) && scanout.engaged() && ours.size() == picture);
    const AmdDisplayRegion region = amd_plan_display_region(map, reserved, fb.physical, fb.bytes);
    CHECK(region.ok && d.value[reg(1, kPrimaryLow)] == static_cast<uint32_t>(region.gpu_address) && d.value[reg(1, kPrimaryHigh)] == (region.gpu_address >> 32));
    CHECK(ours.data() == memory + region.offset);
    // The picture came across, and the region is clear of the firmware's part and of the end of video memory.
    CHECK(std::vector<uint8_t>(ours.data(), ours.data() + picture) == original);
    CHECK(region.offset >= 25 * kMiB && region.offset + region.bytes <= kVideo - 16 * kMiB);
    // Drawing on ours does not change the firmware's; going back copies it over.
    ours[0] ^= 0xFF;
    ours[picture - 1] ^= 0xFF;
    CHECK(memory[0] == original[0]);
    scanout.disengage();
    CHECK(!scanout.engaged() && d.value[reg(1, kPrimaryLow)] == static_cast<uint32_t>(kFirmwareSurface) && memory[0] == static_cast<uint8_t>(original[0] ^ 0xFF) &&
          memory[picture - 1] == static_cast<uint8_t>(original[picture - 1] ^ 0xFF));
    // Again (a mode change does this): the engine is pointed at ours a second time.
    CHECK(scanout.engage(fb, &ours) && scanout.engaged());
    CHECK(scanout.engage(fb, &ours));   // engaging while engaged first gives the screen back
  }

  // Everything that must keep it off the hardware.
  {
    Dcn d;
    light(d, 1);
    apt(d, 1);
    ByteView ours;
    AmdRavenMemoryMap unread = map;
    unread.read = false;
    CHECK(!Scanout(d, mapper, unread, reserved).engage(fb, &ours));
    CHECK(!Scanout(d, mapper, map, picture - 1).engage(fb, &ours));          // not enough set aside
    FirmwareFramebuffer outside = fb;
    outside.physical = map.cpu_base + kVideo;                                   // the firmware's framebuffer is not in the carve-out: the map is for something else
    CHECK(!Scanout(d, mapper, map, reserved).engage(outside, &ours));
    FirmwareFramebuffer pitched = fb;
    pitched.pitch_pixels = 2048;                                                // the plane is not at that pitch
    CHECK(!Scanout(d, mapper, map, reserved).engage(pitched, &ours));
    FirmwareFramebuffer elsewhere = fb;
    elsewhere.physical += 64 * kMiB;                                            // the plane does not show this memory
    elsewhere.bytes = 8 * kMiB;
    CHECK(!Scanout(d, mapper, map, reserved).engage(elsewhere, &ours));
    CHECK(d.writes.empty());
    mapper.refuse = true;
    CHECK(!Scanout(d, mapper, map, reserved).engage(fb, &ours) && d.writes.empty());
    mapper.refuse = false;
    Dcn narrow;                                                                 // an aperture that does not cover our region
    light(narrow, 1);
    narrow.value[reg(1, kAptHighLsb)] = static_cast<uint32_t>((0xF400000000ull + 30 * kMiB) >> 12);
    narrow.value[reg(1, kAptHighMsb)] = static_cast<uint32_t>((0xF400000000ull + 30 * kMiB) >> 44);
    CHECK(!Scanout(narrow, mapper, map, reserved).engage(fb, &ours) && narrow.writes.empty());
    Dcn stuck;                                                                  // the engine does not take it: the firmware keeps the screen
    light(stuck, 1);
    apt(stuck, 1);
    stuck.stuck = true;
    Scanout s(stuck, mapper, map, reserved);
    CHECK(!s.engage(fb, &ours) && !s.engaged() && stuck.value[reg(1, kPrimaryLow)] == static_cast<uint32_t>(kFirmwareSurface));
  }
  free(memory);
}

// The video engine's plan leaves the display's region alone, whatever the sizes.
static void test_plan_clear_of_display() {
  for (uint32_t mib : {256u, 512u, 1024u, 2048u}) {
    AmdRavenMemoryMap map;
    map.read = true;
    map.video_memory_bytes = uint64_t{mib} * kMiB;
    map.gpu_base = 0xF400000000ull;
    map.cpu_base = 0xE0000000ull;
    map.hardware_gb_addr_config = 0x24000042;
    for (uint64_t picture : {1920ull * 1080 * 4, 2560ull * 1440 * 4, 3840ull * 2160 * 4}) {
      const uint64_t reserved = Scanout::region_bytes(picture) + kMiB;
      const AmdDisplayRegion region = amd_plan_display_region(map, reserved, map.cpu_base, picture);
      const AmdVcn1Plan plan = amd_plan_vcn1_memory(map, 0x15DD, 0, 8 * kMiB, map.cpu_base, picture, region.ok ? region.bytes : 0);
      if (!region.ok) continue;
      CHECK(region.offset % kMiB == 0 && region.bytes >= picture && region.offset + region.bytes + 16 * kMiB <= map.video_memory_bytes);
      CHECK(region.gpu_address == map.gpu_base + region.offset && region.physical == map.cpu_base + region.offset);
      if (plan.result != AmdVcn1Plan::Result::Ok) continue;
      CHECK(plan.rings_offset + AmdVcn1::kRingsTotalBytes <= region.offset && plan.block_offset >= picture);
      CHECK(plan.work_bytes == 0 || plan.work_offset + plan.work_bytes <= region.offset);
    }
  }
  // Not a carve-out for this framebuffer, or no room.
  AmdRavenMemoryMap small;
  small.read = true;
  small.video_memory_bytes = 64 * kMiB;
  small.gpu_base = 0xF400000000ull;
  small.cpu_base = 0xE0000000ull;
  CHECK(!amd_plan_display_region(small, 40 * kMiB, small.cpu_base, 8 * kMiB).ok);
  CHECK(!amd_plan_display_region(small, 0, small.cpu_base, 8 * kMiB).ok && !amd_plan_display_region(AmdRavenMemoryMap(), kMiB, small.cpu_base, 8 * kMiB).ok);
  CHECK(!amd_plan_display_region(small, kMiB, small.cpu_base + 64 * kMiB, 8 * kMiB).ok && !amd_plan_display_region(small, kMiB, small.cpu_base, 0).ok);
}

// Whatever the registers hold, probing and writing stay inside the registers they own.
static void test_random_registers() {
  uint32_t seed = 777;
  auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };
  for (int round = 0; round < 3000; ++round) {
    Dcn d;
    for (uint32_t p = 0; p < kPipes; ++p) {
      light(d, p, (uint64_t{next() & 0xFF} << 32) | (next() & ~0xFFu), 1 + next() % 5000);
      for (uint32_t r : {kHubpCntl, kSurfaceConfig, kTilingConfig, kSurfaceControl, kFlipControl, kAptLowMsb, kAptHighMsb}) if (next() % 3 == 0) d.value[reg(p, r)] = next();
    }
    Plane plane(d);
    if (!plane.probe((uint64_t{next() & 0xFF} << 32) | (next() & ~0xFFu), 1 + next() % 5000)) continue;
    d.stuck = (round % 2) != 0;
    plane.set_surface((uint64_t{next() & 0xFF} << 32) | (next() & ~0xFFu), next() % (64 * kMiB));
    plane.restore();
    for (uint32_t w : d.writes) CHECK(w == reg(plane.pipe(), kPrimaryLow) || w == reg(plane.pipe(), kPrimaryHigh));
  }
}

int main() {
  test_probe();
  test_set_surface();
  test_scanout();
  test_plan_clear_of_display();
  test_random_registers();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AmdDcn1Test: ok\n");
  return 0;
}
