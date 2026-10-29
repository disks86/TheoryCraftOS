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


// Host-side tests for the DCE 11.2 graphics surface of the Vega M (amd_dce11::Plane) against a simulated display block: six CRTCs with their register blocks (offsets typed in
// from vid.h, register numbers from AMD's dce_11_2_d.h), a graphics surface address that is latched after a number of reads of GRPH_UPDATE (or, with a vsync flip, never),
// and the system aperture. It covers finding the right CRTC, refusing everything that is not a plain linear ARGB8888 surface, the order of the writes of a flip, the wait,
// the put-back when the engine does not take the address, and the aperture checks.
#include <stdio.h>
#include <map>
#include <string.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce11.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "FakeAmdDce11.hpp"

using namespace hardware;
using namespace hardware::amd_dce11;
using namespace fakedce;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static void test_probe() {
  FakeClock clock;
  {
    FakeDisplay d;
    setup_aperture(d);
    d.crtc(0, 0xDEAD0000, 640);              // another CRTC with another picture
    d.crtc(3, kSurface, 1920);
    Plane plane(d, clock);
    CHECK(plane.probe(kSurface, 1920) && plane.active() && plane.crtc() == 3 && plane.original_surface() == kSurface && plane.surface() == kSurface);
    CHECK(plane.aperture_low() == 0x1'0000'0000ull && plane.aperture_high() == 0x1'FFFF'FFFFull);
    CHECK(plane.in_aperture(0x1'0000'0000ull, 4096) && plane.in_aperture(0x1'FFFF'F000ull, 4096) && !plane.in_aperture(0x1'FFFF'F001ull, 4096) && !plane.in_aperture(0xFFFF'FF00ull, 512) &&
          !plane.in_aperture(0x1'0000'0000ull, 0));
    CHECK(d.writes.empty());   // probing writes nothing
  }
  const struct { const char* what; void (*mutate)(FakeDisplay&); } refused[] = {
      {"crtc off", [](FakeDisplay& d) { d.set(kCrtcControl, 0); }},
      {"graphics off", [](FakeDisplay& d) { d.set(kGrphEnable, 0); }},
      {"16-bit", [](FakeDisplay& d) { d.set(kGrphControl, 0x1); }},
      {"format 2101010", [](FakeDisplay& d) { d.set(kGrphControl, 0x2 | (1 << 8)); }},
      {"tiled", [](FakeDisplay& d) { d.set(kGrphControl, 0x2 | (4 << 20)); }},
      {"swapped", [](FakeDisplay& d) { d.set(kGrphSwap, 0x2 << 4); }},
      {"other pitch", [](FakeDisplay& d) { d.set(kGrphPitch, 1920 + 64); }},
      {"other address", [](FakeDisplay& d) { d.set(kGrphPrimary, 0x20000100); }},
      {"other high half", [](FakeDisplay& d) { d.set(kGrphPrimaryHigh, 2); }},
      {"aperture empty", [](FakeDisplay& d) { d.set(kApertureHigh, 0x0FFFFF); }},
  };
  for (const auto& r : refused) {
    FakeDisplay d;
    setup_aperture(d);
    d.crtc(0, kSurface, 1920);
    r.mutate(d);
    Plane plane(d, clock);
    if (plane.probe(kSurface, 1920)) printf("  probe accepted: %s\n", r.what);
    CHECK(!plane.probe(kSurface, 1920) && !plane.active());
  }
  {
    FakeDisplay d;   // registers that are not there
    d.all_ones = true;
    Plane plane(d, clock);
    CHECK(!plane.probe(kSurface, 1920));
  }
  {
    FakeDisplay d;
    setup_aperture(d);
    d.crtc(0, kSurface, 1920);
    Plane plane(d, clock);
    CHECK(!plane.probe(kSurface + 128, 1920) && !plane.probe(kSurface, 0) && !plane.probe(kSurface, 1920 + 1));
    CHECK(!plane.set_surface(kSurface, 4096) && !plane.restore());   // not active
  }
  for (uint32_t i = 0; i < 6; ++i) {   // each of the six controllers is found
    FakeDisplay d;
    setup_aperture(d);
    d.crtc(i, kSurface, 1366);
    Plane plane(d, clock);
    CHECK(plane.probe(kSurface, 1366) && plane.crtc() == i);
  }
  {
    FakeDisplay d;   // a plane at address 0 with no pitch is no plane to take over
    setup_aperture(d);
    d.crtc(0, 0, 0);
    Plane plane(d, clock);
    CHECK(!plane.probe(0, 1920) && !plane.probe(0, 0));   // (a framebuffer at address 0 is possible, but not one with a pitch the plane does not have)
    d.crtc(0, kSurface, 0);
    CHECK(!plane.probe(kSurface, 0));
    d.crtc(0, 0, 1920);
    CHECK(plane.probe(0, 1920) && plane.surface() == 0);
  }
}

static void test_flip() {
  FakeClock clock;
  FakeDisplay d;
  setup_aperture(d);
  d.crtc(3, kSurface, 1920);
  d.set(kGrphFlipControl + kOffsets[3], 0x1);   // the firmware flips at the horizontal retrace: the bit is kept
  d.crtc(0, 0xDEAD0000, 640);
  Plane plane(d, clock);
  CHECK(plane.probe(kSurface, 1920));
  const uint64_t fresh = 0x1'4000'0100ull;
  CHECK(plane.set_surface(fresh, 1920 * 1080 * 4) && plane.surface() == fresh);
  // The writes, in dce_v11_0_page_flip's order and only in CRTC 3's block: flip control with the immediate bit, pitch, high half, low half; then the firmware's flip policy back.
  const uint32_t off = kOffsets[3];
  const std::pair<uint32_t, uint32_t> expected[] = {
      {kGrphFlipControl + off, 0x11}, {kGrphPitch + off, 1920}, {kGrphPrimaryHigh + off, 0x1}, {kGrphPrimary + off, 0x40000100}, {kGrphFlipControl + off, 0x1},
  };
  CHECK(d.writes.size() == 5);
  for (size_t i = 0; i < d.writes.size() && i < 5; ++i) CHECK(d.writes[i] == expected[i]);
  // The other controller is as it was.
  CHECK(d.get(kGrphPrimary) == 0xDEAD0000 && d.get(kGrphPitch) == 640);
  // Back to the firmware's picture.
  d.writes.clear();
  CHECK(plane.restore() && plane.surface() == kSurface && d.writes.size() == 5 && d.writes[3] == std::make_pair(kGrphPrimary + off, static_cast<uint32_t>(kSurface)));
  d.writes.clear();
  CHECK(plane.restore() && d.writes.empty());   // already there
  // A slow engine is waited for.
  d.latency_reads = 40;
  CHECK(plane.set_surface(fresh, 4096) && plane.surface() == fresh);
  // An engine that says the update is done but still shows the old picture has not taken it.
  d.drop_address = true;
  d.latency_reads = 2;
  const uint64_t other = 0x1'6000'0000ull;
  CHECK(!plane.set_surface(other, 4096) && plane.surface() == fresh);
}

static void test_not_taken() {
  FakeClock clock;
  FakeDisplay d;
  setup_aperture(d);
  d.crtc(0, kSurface, 1920);
  Plane plane(d, clock);
  CHECK(plane.probe(kSurface, 1920));
  d.never_take = true;
  d.writes.clear();
  CHECK(!plane.set_surface(0x1'4000'0100ull, 4096, 2000));
  // The old address was written back (a second flip), and the flip control restored each time.
  CHECK(d.writes.size() == 10 && d.writes[3].first == kGrphPrimary && d.writes[3].second == 0x40000100 && d.writes[8].first == kGrphPrimary && d.writes[8].second == static_cast<uint32_t>(kSurface));
  CHECK(d.writes[2].second == 0x1 && d.writes[7].second == 0x1);   // the high halves: the new one, then the old one
}

static void test_refusals() {
  FakeClock clock;
  FakeDisplay d;
  setup_aperture(d);
  d.crtc(0, kSurface, 1920);
  Plane plane(d, clock);
  CHECK(plane.probe(kSurface, 1920));
  d.writes.clear();
  CHECK(!plane.set_surface(0x1'4000'0080ull, 4096));        // not 256-byte aligned
  CHECK(!plane.set_surface(0xFFFF'FF00ull, 4096));          // below the aperture
  CHECK(!plane.set_surface(0x2'0000'0000ull, 4096));        // above it
  CHECK(!plane.set_surface(0x1'FFFF'F000ull, 8192));        // runs past its end
  CHECK(!plane.set_surface(0x1'4000'0000ull, 0));           // nothing to show
  CHECK(!plane.set_surface(0x1'4000'0000ull, 0xFFFFFFFFFFFFFFFFull));
  CHECK(d.writes.empty());
}

static void test_display() {
  FakeClock clock;
  // Two 64x4 pictures in "video memory" seen through the memory BAR: the CPU's address of its start, the GPU's address of the same byte, the firmware's framebuffer at the
  // start and ours after it.
  const uint32_t width = 64, height = 4, pitch = 64;
  std::vector<uint8_t> memory(0x4000 + 8192);
  uint8_t* base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(memory.data()) + 255) & ~uintptr_t{255});
  const uint64_t cpu_base = reinterpret_cast<uint64_t>(base), gpu_base = 0x1'0000'0000ull;
  FirmwareFramebuffer fw;
  fw.physical = cpu_base;
  fw.bytes = 0x1000;
  fw.width = width;
  fw.height = height;
  fw.pitch_pixels = pitch;
  for (uint32_t i = 0; i < width * height; ++i) reinterpret_cast<uint32_t*>(base)[i] = 0xFF000000u | i;
  const MmioRegion region(cpu_base + 0x2000, 0x1000);
  {
    FakeDisplay d;
    setup_aperture(d);
    d.crtc(2, gpu_base, pitch);
    Display display(d, clock, cpu_base, gpu_base, region, gpu_base + 0x2000);
    ByteView ours;
    CHECK(display.engage(fw, &ours) && display.engaged() && ours.size() == width * height * 4 && ours.data() == base + 0x2000);
    CHECK(memcmp(ours.data(), base, width * height * 4) == 0 && display.plane().crtc() == 2 && display.plane().surface() == gpu_base + 0x2000);
    ours[0] = 0x77;   // drawn on after the switch
    display.disengage();
    CHECK(!display.engaged() && display.plane().surface() == gpu_base && base[0] == 0x77 && base[4] == 1);
    display.disengage();   // (again: nothing)
    ByteView again;
    CHECK(display.engage(fw, &again) && display.plane().surface() == gpu_base + 0x2000);
    // (leaves the display engaged for the destructor to put back)
  }
  const struct { const char* what; void (*mutate)(FakeDisplay&, FirmwareFramebuffer&, MmioRegion&); } refused[] = {
      {"another picture on the screen", [](FakeDisplay& d, FirmwareFramebuffer&, MmioRegion&) { d.crtc(2, 0x1'0000'4000ull, 64); }},
      {"other pitch", [](FakeDisplay& d, FirmwareFramebuffer&, MmioRegion&) { d.crtc(2, 0x1'0000'0000ull, 128); }},
      {"firmware below the memory BAR", [](FakeDisplay&, FirmwareFramebuffer& f, MmioRegion&) { f.physical -= 256; }},
      {"zero size", [](FakeDisplay&, FirmwareFramebuffer& f, MmioRegion&) { f.height = 0; }},
      {"pitch under the width", [](FakeDisplay&, FirmwareFramebuffer& f, MmioRegion&) { f.pitch_pixels = 32; }},
      {"picture bigger than the allocation", [](FakeDisplay&, FirmwareFramebuffer& f, MmioRegion&) { f.bytes = 100; }},
      {"window too small", [](FakeDisplay&, FirmwareFramebuffer&, MmioRegion& r) { r = MmioRegion(r.address(), 0x3FC); }},
      {"no window", [](FakeDisplay&, FirmwareFramebuffer&, MmioRegion& r) { r = MmioRegion(); }},
  };
  for (const auto& r : refused) {
    FakeDisplay d;
    setup_aperture(d);
    d.crtc(2, gpu_base, pitch);
    FirmwareFramebuffer f = fw;
    MmioRegion window = region;
    r.mutate(d, f, window);
    Display display(d, clock, cpu_base, gpu_base, window, gpu_base + 0x2000);
    ByteView ours;
    if (display.engage(f, &ours)) printf("  engaged: %s\n", r.what);
    CHECK(!display.engage(f, &ours) && !display.engaged() && d.writes.empty());
  }
  {
    // The firmware's picture not at the start of the memory BAR: its GPU address is the base plus its distance.
    FakeDisplay d;
    setup_aperture(d);
    FirmwareFramebuffer inner = fw;
    inner.physical = cpu_base + 0x1000;
    inner.bytes = 0x1000;
    d.crtc(2, gpu_base + 0x1000, pitch);
    for (uint32_t i = 0; i < width * height; ++i) reinterpret_cast<uint32_t*>(base + 0x1000)[i] = 0xFF000000u | (i + 7);
    Display display(d, clock, cpu_base, gpu_base, region, gpu_base + 0x2000);
    ByteView ours;
    CHECK(display.engage(inner, &ours) && display.plane().surface() == gpu_base + 0x2000 && memcmp(ours.data(), base + 0x1000, width * height * 4) == 0);
    // Engaging again first gives the screen back (the plane shows the firmware's picture again, so it is found again).
    CHECK(display.engage(inner, &ours) && display.engaged() && display.plane().surface() == gpu_base + 0x2000);
    display.disengage();
    CHECK(display.plane().surface() == gpu_base + 0x1000);
    // A GPU base so high that the address would wrap.
    Display wrapped(d, clock, cpu_base, ~0ull - 100, region, gpu_base + 0x2000);
    CHECK(!wrapped.engage(inner, &ours));
  }
  {
    FakeDisplay d;   // the region is outside the aperture, or the engine never takes the address
    setup_aperture(d);
    d.crtc(2, gpu_base, pitch);
    Display outside(d, clock, cpu_base, gpu_base, region, 0x3'0000'0000ull);
    ByteView ours;
    CHECK(!outside.engage(fw, &ours));
    Display skew(d, clock, cpu_base, gpu_base, region, gpu_base + 0x2080);
    CHECK(!skew.engage(fw, &ours));
    d.never_take = true;
    Display stuck(d, clock, cpu_base, gpu_base, region, gpu_base + 0x2000);
    CHECK(!stuck.engage(fw, &ours) && !stuck.engaged());
  }
}

// The graphics plane set up for a picture of its own (dce_v11_0_crtc_do_set_base): the registers the driver writes, with their values, for each CRTC; the VGA mode off, the other bits of the
// registers it reads and modifies left alone, and the refusals.
static void test_scanout() {
  const uint32_t offsets[6] = {0x1b9c - 0x1b9c, 0x1d9c - 0x1b9c, 0x1f9c - 0x1b9c, 0x419c - 0x1b9c, 0x439c - 0x1b9c, 0x459c - 0x1b9c};
  const uint32_t vga[6] = {0xcc, 0xce, 0xf8, 0xf9, 0xfa, 0xfb};
  for (uint32_t crtc = 0; crtc < 6; ++crtc) {
    FakeDisplay d;
    const uint32_t off = offsets[crtc];
    d.set(vga[crtc], 0x00010001);              // VGA mode on, another bit set
    d.set(0x1a12 + off, 0x00000011);           // flip control: H_RETRACE_EN and another bit
    d.set(0x1a02 + off, 0x00000123);           // LUT bypass: enable and other bits
    ScanoutSurface s;
    s.address = 0x0000000123456700ull;
    s.width = 1920;
    s.height = 1080;
    s.pitch_pixels = 2048;
    s.viewport_width = 1920;
    s.viewport_height = 1080;
    Scanout scan(d);
    CHECK(scan.set(crtc, s));
    CHECK(d.get(vga[crtc]) == 0x00010000);
    CHECK(d.get(0x1a12 + off) == 0x00000010);
    // (the fake holds the primary address back as a pending flip: the log has what was written)
    const auto written = [&](uint32_t reg) { uint32_t v = 0xDEADBEEFu; for (const auto& w : d.writes) if (w.first == reg) v = w.second; return v; };
    CHECK(written(0x1a07 + off) == 1 && written(0x1a08 + off) == 1);
    CHECK(written(0x1a04 + off) == 0x23456700 && written(0x1a05 + off) == 0x23456700);
    CHECK(d.get(0x1a01 + off) == 2 && d.get(0x1a03 + off) == 0);
    CHECK(d.get(0x1a02 + off) == 0x00000023);
    CHECK(d.get(0x1a09 + off) == 0 && d.get(0x1a0a + off) == 0 && d.get(0x1a0b + off) == 0 && d.get(0x1a0c + off) == 0);
    CHECK(d.get(0x1a0d + off) == 1920 && d.get(0x1a0e + off) == 1080 && d.get(0x1a06 + off) == 2048);
    CHECK(d.get(0x1a00 + off) == 1 && d.get(0x1ac3 + off) == 1080 && d.get(0x1b5c + off) == 0);
    CHECK(d.get(0x1b5d + off) == ((1920u << 16) | 1080u) && d.get(0x1bbe + off) == 0);
    CHECK(scan.disable(crtc) && d.get(0x1a00 + off) == 0);
  }
  // an odd height rounds the viewport up to even
  FakeDisplay d;
  Scanout scan(d);
  ScanoutSurface s;
  s.address = 0x1000;
  s.width = 800;
  s.height = 601;
  s.pitch_pixels = 800;
  s.viewport_width = 800;
  s.viewport_height = 601;
  CHECK(scan.set(0, s) && d.get(0x1b5d) == ((800u << 16) | 602u));
  // refused, nothing written: a CRTC that does not exist, a misaligned address, an empty picture, a pitch below the width, an empty viewport
  const size_t writes = d.writes.size();
  ScanoutSurface bad = s;
  CHECK(!scan.set(6, s) && !scan.disable(6));
  bad.address = 0x1001;
  CHECK(!scan.set(0, bad));
  bad = s; bad.width = 0;
  CHECK(!scan.set(0, bad));
  bad = s; bad.height = 0;
  CHECK(!scan.set(0, bad));
  bad = s; bad.pitch_pixels = 799;
  CHECK(!scan.set(0, bad));
  bad = s; bad.viewport_width = 0;
  CHECK(!scan.set(0, bad));
  bad = s; bad.viewport_height = 0;
  CHECK(!scan.set(0, bad));
  CHECK(d.writes.size() == writes);
}

int main() {
  test_probe();
  test_flip();
  test_not_taken();
  test_refusals();
  test_display();
  test_scanout();
  printf(g_failed ? "AmdDce11Test: %d FAILED\n" : "AmdDce11Test: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
