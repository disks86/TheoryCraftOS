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
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce11.hpp"
#include "Hardware/Clock/IClock.hpp"

using namespace hardware;
using namespace hardware::amd_dce11;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

constexpr uint32_t kCrtcControl = 0x1b9c, kGrphEnable = 0x1a00, kGrphControl = 0x1a01, kGrphSwap = 0x1a03, kGrphPrimary = 0x1a04, kGrphPitch = 0x1a06, kGrphPrimaryHigh = 0x1a07;
constexpr uint32_t kGrphUpdate = 0x1a11, kGrphFlipControl = 0x1a12, kApertureLow = 0x80d, kApertureHigh = 0x80e;
constexpr uint32_t kOffsets[6] = {0x0, 0x200, 0x400, 0x2600, 0x2800, 0x2a00};
constexpr uint64_t kSurface = 0x1'2000'0000ull;

struct FakeClock final : IClock {
  mutable uint64_t now = 0;
  uint64_t now_us() const override { now += 50; return now; }
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

class FakeDisplay final : public IVcnRegisters {
 public:
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  uint32_t latency_reads = 2;       // reads of GRPH_UPDATE that still show the update pending
  bool never_take = false;          // the engine never latches the new address
  bool all_ones = false;            // the registers are not there
  bool drop_address = false;        // the pending flag clears but the engine keeps showing the old address

  void set(uint32_t r, uint32_t v) { regs_[r] = v; }
  uint32_t get(uint32_t r) const { auto at = regs_.find(r); return at == regs_.end() ? 0 : at->second; }
  // A CRTC in the state the firmware leaves it in: on, 32-bit ARGB8888 linear, showing `surface`.
  void crtc(uint32_t index, uint64_t surface, uint32_t pitch) {
    const uint32_t off = kOffsets[index];
    set(kCrtcControl + off, 0x1);
    set(kGrphEnable + off, 0x1);
    set(kGrphControl + off, 0x2);          // GRPH_DEPTH 2, GRPH_FORMAT 0, GRPH_ARRAY_MODE 0
    set(kGrphSwap + off, 0);
    set(kGrphPitch + off, pitch);
    set(kGrphPrimaryHigh + off, static_cast<uint32_t>(surface >> 32));
    set(kGrphPrimary + off, static_cast<uint32_t>(surface));
    set(kGrphFlipControl + off, 0x0);
  }
  uint32_t read(uint32_t r) override {
    if (all_ones) return 0xFFFFFFFFu;
    for (uint32_t off : kOffsets)
      if (r == kGrphUpdate + off) {
        if (pending_reads_ > 0) {
          --pending_reads_;
          return 0x4;   // GRPH_SURFACE_UPDATE_PENDING
        }
        if (!never_take && have_pending_) {
          if (!drop_address) {
            set(kGrphPrimary + off_, pending_low_);
            set(kGrphPrimaryHigh + off_, pending_high_);
          }
          have_pending_ = false;
        }
        return never_take && have_pending_ ? 0x4 : 0;
      }
    return get(r);
  }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    for (uint32_t off : kOffsets) {
      if (r == kGrphPrimaryHigh + off) { pending_high_ = v; return; }
      if (r == kGrphPrimary + off) {
        // the write of the low half triggers the update (taken at once with the immediate flip, never with a vsync flip)
        pending_low_ = v;
        off_ = off;
        const bool immediate = get(kGrphFlipControl + off) & 0x10;
        if (immediate) { have_pending_ = true; pending_reads_ = latency_reads; }
        else { have_pending_ = true; never_take = true; }
        return;
      }
    }
    set(r, v);
  }

 private:
  std::map<uint32_t, uint32_t> regs_;
  bool have_pending_ = false;
  uint32_t pending_reads_ = 0, pending_low_ = 0, pending_high_ = 0, off_ = 0;
};

static void setup_aperture(FakeDisplay& d) {
  d.set(kApertureLow, 0x100000);        // 0x1_0000_0000
  d.set(kApertureHigh, 0x1FFFFF);       // up to 0x1_FFFF_FFFF
}

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
    CHECK(!plane.probe(0, 1920) && !plane.probe(kSurface + 128, 1920) && !plane.probe(kSurface, 0) && !plane.probe(kSurface, 1920 + 1));
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
    CHECK(!plane.probe(0, 1920) && !plane.probe(0, 0));
    d.crtc(0, kSurface, 0);
    CHECK(!plane.probe(kSurface, 0));
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

int main() {
  test_probe();
  test_flip();
  test_not_taken();
  test_refusals();
  printf(g_failed ? "AmdDce11Test: %d FAILED\n" : "AmdDce11Test: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
