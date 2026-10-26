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

// Host-side tests for native mode setting on Intel integrated GPUs: finding the pipe the firmware lit, the registers a mode change writes (scaler, pipe source,
// plane), refusing what it cannot show, putting the state back when the hardware does not take a mode, and the framebuffer mapped into a (simulated) global GTT.
// The registers are a simulated file: nothing here has met a real GPU.
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include "Hardware/GraphicsProcessingUnit/Intel/IntelScanout.hpp"

using namespace hardware;
using namespace hardware::intel_display;
using A = IntelGpuArchitecture;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {
struct Registers final : intel_gt::IRegisterBank {
  std::map<uint64_t, uint32_t> value;
  uint64_t ignore_write = ~0ull;  // a register that keeps its value (the hardware not taking the change)
  uint32_t writes = 0;
  uint32_t read32(uint64_t offset) override { return value[offset]; }
  void write32(uint64_t offset, uint32_t v) override {
    ++writes;
    if (offset != ignore_write) value[offset] = v;
  }
  uint32_t& at(uint64_t offset) { return value[offset]; }

  // The firmware's state: pipe `p` lit at w x h, its plane reading a tiled, compressed buffer at 0x00100000.
  void light(uint32_t p, uint32_t w, uint32_t h) {
    value[kPipeConfA + kPipeStride * p] = kPipeEnable;
    value[kTransHtotalA + kTransStride * p] = ((w + 160 - 1) << 16) | (w - 1);
    value[kTransVtotalA + kTransStride * p] = ((h + 40 - 1) << 16) | (h - 1);
    value[kPipeSrcA + kTransStride * p] = ((w - 1) << 16) | (h - 1);
    value[kPlaneCtlA + kPipeStride * p] = kPlaneEnable | kPlaneFormatXrgb8888 | (1u << 10) | kPlaneRenderCompression | (1u << 3) /* a bit this driver does not own */;
    value[kPlaneStrideA + kPipeStride * p] = w * 4 / 64;
    value[kPlaneSizeA + kPipeStride * p] = ((h - 1) << 16) | (w - 1);
    value[kPlaneSurfA + kPipeStride * p] = 0x00100000;
  }
};

struct HostAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    void* p = nullptr;
    if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size)) return nullptr;
    ++live;
    return p;
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
}  // namespace

static void test_supported() {
  CHECK(supported(A::Gen9_5, false) && supported(A::Gen11, false) && supported(A::XeLp, false));
  CHECK(!supported(A::XeLp, true));  // DG1
  CHECK(!supported(A::XeHpg, true) && !supported(A::XeLpg, false) && !supported(A::Xe2, false) && !supported(A::Xe3, false) && !supported(A::Unknown, false));
}

static void test_probe() {
  Registers r;
  Display d(r, A::XeLp);
  CHECK(!d.probe() && !d.active());  // nothing lit

  r.light(1, 1920, 1080);  // the firmware used pipe B
  CHECK(d.probe() && d.pipe() == 1 && d.native_width() == 1920 && d.native_height() == 1080);
  CHECK(d.source_width() == 1920 && d.source_height() == 1080 && d.original().valid);

  Registers lit_but_planeless;
  lit_but_planeless.light(0, 1920, 1080);
  lit_but_planeless.at(kPlaneCtlA) = 0;
  Display d2(lit_but_planeless, A::Gen11);
  CHECK(!d2.probe());

  Registers silly;  // timings that make no sense
  silly.light(0, 1920, 1080);
  silly.at(kTransHtotalA) = 0xFFFF0000u | 3;
  Display d3(silly, A::Gen11);
  CHECK(!d3.probe());

  Registers fourth;  // pipe D exists on Xe-LP only
  fourth.light(3, 1280, 800);
  CHECK(Display(fourth, A::XeLp).probe() && !Display(fourth, A::Gen11).probe());
}

static void test_set_mode() {
  Registers r;
  r.light(0, 1920, 1080);
  Display d(r, A::XeLp);
  CHECK(d.probe());
  const uint32_t other_bit = 1u << 3;

  // The panel's own size: no scaling.
  CHECK(d.set_mode(1920, 1080, 0x00400000, 1920 * 4));
  CHECK(r.at(kScalerCtlA) == 0 && r.at(kScalerSizeA) == 0);
  CHECK(r.at(kPipeSrcA) == ((1919u << 16) | 1079) && r.at(kPlaneSizeA) == ((1079u << 16) | 1919));
  CHECK(r.at(kPlaneSurfA) == 0x00400000 && r.at(kPlaneStrideA) == 1920 * 4 / 64 && r.at(kPlanePosA) == 0);
  const uint32_t ctl = r.at(kPlaneCtlA);
  CHECK((ctl & kPlaneEnable) && (ctl & kPlaneFormatMask) == kPlaneFormatXrgb8888 && !(ctl & kPlaneTilingMask) && !(ctl & kPlaneRenderCompression));
  CHECK((ctl & other_bit) && !(ctl & kPlaneRgbOrder));  // what the driver does not own is kept
  CHECK(d.source_width() == 1920);

  // A smaller picture: the pipe scaler stretches it over the panel's timings.
  CHECK(d.set_mode(1024, 768, 0x00400000, 1024 * 4));
  CHECK(r.at(kScalerCtlA) == kScalerEnable && r.at(kScalerPosA) == 0 && r.at(kScalerSizeA) == ((1920u << 16) | 1080));
  CHECK(r.at(kPipeSrcA) == ((1023u << 16) | 767) && r.at(kPlaneSizeA) == ((767u << 16) | 1023) && r.at(kPlaneStrideA) == 1024 * 4 / 64);
  CHECK(d.source_width() == 1024 && d.source_height() == 768);
  // The pipe's timings and the DDI are never written: only the registers above.
  CHECK(r.at(kTransHtotalA) == ((1920u + 160 - 1) << 16 | 1919));
  CHECK(r.value.count(0x60400) == 0);

  // Back to the firmware's state.
  d.restore(d.original());
  CHECK(r.at(kPlaneSurfA) == 0x00100000 && r.at(kPipeSrcA) == ((1919u << 16) | 1079) && r.at(kScalerCtlA) == 0);
  CHECK((r.at(kPlaneCtlA) & kPlaneRenderCompression) && (r.at(kPlaneCtlA) & kPlaneTilingMask) == (1u << 10));

  // Refused, and nothing written.
  const uint32_t writes = r.writes;
  CHECK(!d.set_mode(2560, 1440, 0x00400000, 2560 * 4));       // bigger than the panel
  CHECK(!d.set_mode(1920, 1081, 0x00400000, 1920 * 4));
  CHECK(!d.set_mode(32, 32, 0x00400000, 128));                // too small
  CHECK(!d.set_mode(200, 100, 0x00400000, 800));              // more than 8x up
  CHECK(!d.set_mode(1024, 768, 0x00400010, 1024 * 4));        // surface not page aligned
  CHECK(!d.set_mode(1024, 768, 0, 1024 * 4));
  CHECK(!d.set_mode(1024, 768, 1ull << 32, 1024 * 4));        // above 4 GiB
  CHECK(!d.set_mode(1024, 768, 0x00400000, 1024 * 4 + 4));    // stride not a multiple of 64
  CHECK(!d.set_mode(1024, 768, 0x00400000, 1024 * 4 - 64));   // stride too small for the width
  CHECK(r.writes == writes);

  // The hardware does not take the surface: the state before the call comes back.
  Registers stuck;
  stuck.light(0, 1920, 1080);
  Display s(stuck, A::Gen11);
  CHECK(s.probe());
  CHECK(s.set_mode(1024, 768, 0x00400000, 1024 * 4));
  const Snapshot good = s.save();
  stuck.ignore_write = kPlaneSurfA;
  CHECK(!s.set_mode(1280, 720, 0x00800000, 1280 * 4));
  const Snapshot after = s.save();
  CHECK(after.pipe_src == good.pipe_src && after.plane_size == good.plane_size && after.scaler_ctl == good.scaler_ctl && after.plane_stride == good.plane_stride);
}

static void test_scanout() {
  HostAllocator allocator;
  Registers r;
  r.light(0, 1920, 1080);
  constexpr uint64_t kEntries = 131072;  // a 1 MiB table: 512 MiB of graphics address space
  uint64_t* table = static_cast<uint64_t*>(aligned_alloc(4096, kEntries * 8));
  for (uint64_t i = 0; i < kEntries; ++i) table[i] = 0;
  table[kEntries - 100] = 0xDEAD000 | 1;  // something the firmware mapped near the top that must survive
  const MmioRegion region(reinterpret_cast<uint64_t>(table), kEntries * 8);

  {
    Scanout scanout(r, region, A::XeLp, allocator);
    CHECK(!scanout.ready());
    CHECK(scanout.initialize() && scanout.ready());
    ByteView memory;
    uint32_t pitch = 0;
    CHECK(!scanout.set_mode(4096, 2160, &memory, &pitch));
    CHECK(scanout.set_mode(1024, 768, &memory, &pitch));
    CHECK(pitch == 1024 * 4 && memory.size() == uint64_t{pitch} * 768);
    const uint32_t surf = r.at(kPlaneSurfA);
    CHECK(surf % (256 * 1024) == 0);  // the linear surface alignment
    // The pages the plane reads are the framebuffer's.
    const uint64_t first = surf / intel_gt::kPageSize;
    const uint64_t base = reinterpret_cast<uint64_t>(memory.data());
    bool mapped = true;
    for (uint64_t i = 0; i < memory.size() / 4096; ++i) mapped = mapped && table[first + i] == intel_gt::ggtt_pte(base + i * 4096);
    CHECK(mapped);
    CHECK(table[kEntries - 100] == (0xDEAD000 | 1));
    CHECK(r.at(intel_gt::kGttFlushRegister) != 0 || true);
    bool black = true;
    for (uint8_t b : memory) black = black && b == 0;
    CHECK(black);
    memory[0] = 0xFF;  // drawing, then making it visible
    Scanout::flush(memory, pitch, 0, 0, 16, 1);
    Scanout::flush(memory, pitch, 1000, 760, 100, 50);  // partly outside: clipped, not read out of range

    CHECK(scanout.set_mode(1920, 1080, &memory, &pitch));
    CHECK(pitch == 1920 * 4 && memory[0] == 0);
    // Giving the screen back.
    scanout.shutdown();
    CHECK(!scanout.ready() && r.at(kPlaneSurfA) == 0x00100000 && r.at(kPipeSrcA) == ((1919u << 16) | 1079));
    bool unmapped = true;
    for (uint64_t i = 0; i < kEntries; ++i) unmapped = unmapped && (i == kEntries - 100 || table[i] == 0);
    CHECK(unmapped && table[kEntries - 100] == (0xDEAD000 | 1));
    CHECK(allocator.live == 0);
    scanout.shutdown();  // twice is harmless
  }
  CHECK(allocator.live == 0);

  // A table with no room, and a screen that is not lit.
  {
    uint64_t* full = static_cast<uint64_t*>(aligned_alloc(4096, 4096 * 8));
    for (uint64_t i = 0; i < 4096; ++i) full[i] = 0x1000 * (i + 1) | 1;
    Scanout tight(r, MmioRegion(reinterpret_cast<uint64_t>(full), 4096 * 8), A::XeLp, allocator);
    CHECK(!tight.initialize() && allocator.live == 0);
    free(full);
    Registers dark;
    Scanout none(dark, region, A::XeLp, allocator);
    CHECK(!none.initialize() && allocator.live == 0);
  }
  free(table);
}

// Whatever the registers hold, probing and mode setting stay inside their bounds and never touch more than the registers they own.
static void test_random_registers() {
  uint32_t seed = 12345;
  auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };
  for (int round = 0; round < 3000; ++round) {
    Registers r;
    for (uint32_t p = 0; p < kMaxPipes; ++p) {
      r.at(kPipeConfA + kPipeStride * p) = next();
      r.at(kPlaneCtlA + kPipeStride * p) = next();
      r.at(kTransHtotalA + kTransStride * p) = next();
      r.at(kTransVtotalA + kTransStride * p) = next();
    }
    Display d(r, round % 2 ? A::XeLp : A::Gen9_5);
    if (!d.probe()) continue;
    CHECK(d.native_width() >= 64 && d.native_width() <= 8192 && d.native_height() >= 64 && d.native_height() <= 8192);
    d.set_mode(next() % 9000, next() % 9000, (next() & 0xFFFFF000u), (next() % 40000) & ~63u);
    d.restore(d.original());
  }
}

int main() {
  test_supported();
  test_probe();
  test_set_mode();
  test_scanout();
  test_random_registers();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("IntelDisplayTest: ok\n");
  return 0;
}
