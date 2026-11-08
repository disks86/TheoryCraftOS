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
#include "Win32kTestSupport.hpp"
#include "Process/Win32k/Win32kGdi.hpp"
#include "Process/Win32k/Win32kDesktopBridge.hpp"
#include "Process/Win32k/Win32kUser.hpp"

using namespace w32test;
using namespace process;
using namespace process::win32k;
using WN = WindowsSyscallNumber;

#define N(x) WN::NtGdi##x

// DIB sections in the fake memory: a bump allocator in the upper part of the megabyte.
static uint64_t g_dib_next = kBase + 0x80000;
static bool dib_alloc(Process&, uint64_t bytes, uint64_t* address) {
  if (g_dib_next + bytes > kBase + kSize) return false;
  *address = g_dib_next;
  g_dib_next = (g_dib_next + bytes + 15) & ~uint64_t{15};
  return true;
}
static void dib_free(Process&, uint64_t, uint64_t) {}
static const gdi::DibMemory kDibMemory{dib_alloc, dib_free};

struct Screen final : gdi::IScreenSink {
  gdi::Rect dirty{};
  int calls = 0;
  void screen_changed(const gdi::Rect& r) override {
    dirty = calls == 0 ? r : gdi::Rect{dirty.left < r.left ? dirty.left : r.left, dirty.top < r.top ? dirty.top : r.top, dirty.right > r.right ? dirty.right : r.right, dirty.bottom > r.bottom ? dirty.bottom : r.bottom};
    ++calls;
  }
};

struct Fixture {
  System sys;
  Program prog;
  uint64_t pid;
  explicit Fixture(uint64_t p) : prog(p), pid(p) {
    g_dib_next = kBase + 0x80000;
    gdi::set_dib_memory(&kDibMemory);
  }
  ~Fixture() {
    process_exited(pid);
    gdi::set_screen_sink(nullptr);
    gdi::set_font_provider(nullptr);
    gdi::shutdown();
  }
  uint64_t operator()(WN n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0, uint64_t a7 = 0, uint64_t a8 = 0, uint64_t a9 = 0,
                      uint64_t a10 = 0, uint64_t a11 = 0, uint64_t a12 = 0, uint64_t a13 = 0, uint64_t a14 = 0, uint64_t a15 = 0) {
    return prog.call(n, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15);
  }
  uint32_t u(WN n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0, uint64_t a7 = 0, uint64_t a8 = 0, uint64_t a9 = 0,
             uint64_t a10 = 0, uint64_t a11 = 0, uint64_t a12 = 0, uint64_t a13 = 0, uint64_t a14 = 0, uint64_t a15 = 0) {
    return uint32_t((*this)(n, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15));
  }
  int32_t rect(uint64_t at, int i) { return int32_t(prog.memory.get32(at + 4 * i)); }
};

constexpr uint32_t kPatCopy = 0xF00021, kSrcCopy = 0xCC0020, kSrcInvert = 0x660046, kDstInvert = 0x550009, kBlackness = 0x42, kWhiteness = 0xFF0062;
constexpr uint64_t kOut = kScratch + 0x1000, kIn = kScratch + 0x2000, kBig = kScratch + 0x4000;
constexpr uint32_t kRed = 0x0000FF, kGreen = 0x00FF00, kBlue = 0xFF0000, kWhite = 0xFFFFFF;

static uint32_t make_dc(Fixture& f, int w, int h, uint32_t* bitmap_out = nullptr) {
  const uint32_t dc = f.u(N(CreateCompatibleDC), 0);
  const uint32_t bm = f.u(N(CreateBitmap), w, h, 1, 32, 0);
  f.u(N(SelectBitmap), dc, bm);
  if (bitmap_out) *bitmap_out = bm;
  return dc;
}

static uint32_t px(Fixture& f, uint32_t dc, int x, int y) { return f.u(N(GetPixel), dc, uint64_t(uint32_t(x)), uint64_t(uint32_t(y))); }

// A pixel read straight from the bitmap (GetPixel answers CLR_INVALID outside the clip).
static uint32_t pb(Fixture& f, uint32_t dc, int x, int y) {
  const uint32_t bm = f.u(N(GetDCObject), dc, 7);
  List<uint32_t> pix(*system_allocator());
  int32_t w = 0, h = 0;
  uint32_t bpp = 0;
  if (!gdi::bitmap_info(bm, &w, &h, &bpp) || !gdi::bitmap_pixels(bm, &pix) || x < 0 || y < 0 || x >= w || y >= h) return 0xFFFFFFFFu;
  const uint32_t p = pix[uint64_t(y) * uint32_t(w) + uint32_t(x)];
  return ((p >> 16) & 0xFF) | (p & 0xFF00) | ((p & 0xFF) << 16);
}

static void test_objects() {
  Fixture f(21);
  // stock objects
  {      // gdi32.dll names the stock objects without a call (GetStockObject in src/Windows/gdi32/object.inc): the handles must follow its table
    static const uint8_t slot[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 10, 11, 12, 13, 14, 19, 15, 16, 17, 18};
    static const uint8_t type[20] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x30, 0x30, 0x30, 0, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x08, 0x0A, 0x0A, 0x10, 0x30};
    for (int i = 0; i < 20; ++i) {
      if (i == 9) { CHECK(gdi::stock_object(i) == 0); continue; }
      CHECK(gdi::stock_object(i) == (0x01000000u | 0x00800000u | (uint32_t(type[i]) << 16) | slot[i]));
    }
  }
  const uint32_t white = gdi::stock_object(0), black_pen = gdi::stock_object(7), null_brush = gdi::stock_object(5);
  CHECK(white && black_pen && null_brush && white != black_pen);
  CHECK(((white >> 16) & 0x7F) == 0x10 && (white & 0x800000) && ((black_pen >> 16) & 0x7F) == 0x30);
  CHECK(f.u(N(DeleteObjectApp), white) == 1);      // deleting a stock object does nothing
  CHECK(f.u(N(ExtGetObjectW), white, 0, 0) == 16);
  // solid brush
  const uint32_t br = f.u(N(CreateSolidBrush), 0x123456, 0);
  CHECK(br && ((br >> 16) & 0x7F) == 0x10);
  CHECK(f.u(N(ExtGetObjectW), br, 16, kOut) == 16 && f.prog.memory.get32(kOut) == 0 && f.prog.memory.get32(kOut + 4) == 0x123456);
  // hatch brush validation
  CHECK(f.u(N(CreateHatchBrushInternal), 7, 0, 0) == 0 && f.u(N(CreateHatchBrushInternal), 5, kRed, 0) != 0);
  // pen
  const uint32_t pen = f.u(N(CreatePen), 0, 3, kBlue, 0);
  CHECK(pen && f.u(N(ExtGetObjectW), pen, 16, kOut) == 16 && f.prog.memory.get32(kOut + 4) == 3 && f.prog.memory.get32(kOut + 12) == kBlue);
  CHECK(f.u(N(CreatePen), 9, 1, 0, 0) == 0);
  // ext pen: a cosmetic pen must be one pixel wide
  CHECK(f.u(N(ExtCreatePen), 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
  const uint32_t ext = f.u(N(ExtCreatePen), 0x10000 | 0x200, 5, 0, kRed, 0, 0, 0, 0, 0, 0, 0);
  CHECK(ext && ((ext >> 16) & 0x7F) == 0x50 && f.u(N(ExtGetObjectW), ext, 64, kOut) == 32 && f.prog.memory.get32(kOut + 4) == 5);
  // user dash pen
  f.prog.memory.put32(kIn, 3); f.prog.memory.put32(kIn + 4, 2);
  CHECK(f.u(N(ExtCreatePen), 7, 1, 0, 0, 0, 0, 2, kIn, 0, 0, 0) != 0);
  CHECK(f.u(N(ExtCreatePen), 7, 1, 0, 0, 0, 0, 0, kIn, 0, 0, 0) == 0);      // PS_USERSTYLE needs entries
  // font
  uint8_t lf[92] = {};
  lf[0] = 0xF0; lf[1] = 0xFF; lf[2] = 0xFF; lf[3] = 0xFF;      // height -16
  lf[16] = 0xBC; lf[17] = 0x02;      // weight 700
  const char* name = "Arial";
  for (int i = 0; name[i]; ++i) lf[28 + 2 * i] = uint8_t(name[i]);
  for (int i = 0; i < 92; ++i) f.prog.memory.put8(kIn + i, lf[i]);
  const uint32_t font = f.u(N(HfontCreate), kIn, 92, 0, 0, 0);
  CHECK(font && ((font >> 16) & 0x7F) == 0x0A);
  CHECK(f.u(N(ExtGetObjectW), font, 92, kOut) == 92 && int32_t(f.prog.memory.get32(kOut)) == -16 && f.prog.memory.get16(kOut + 28) == 'A');
  // palette
  f.prog.memory.put16(kIn, 0x300); f.prog.memory.put16(kIn + 2, 3);
  f.prog.memory.put32(kIn + 4, 0x00112233); f.prog.memory.put32(kIn + 8, 0x00FF0000); f.prog.memory.put32(kIn + 12, 0x0000FF00);
  const uint32_t pal = f.u(N(CreatePaletteInternal), kIn, 3);
  CHECK(pal && f.u(N(DoPalette), pal, 0, 3, kOut, 2, 0) == 3 && f.prog.memory.get32(kOut + 4) == 0x00FF0000);
  CHECK(f.u(N(GetNearestPaletteIndex), pal, 0x00FE0000) == 1);      // nearest of the three entries
  CHECK(f.u(N(ResizePalette), pal, 5) == 1 && f.u(N(DoPalette), pal, 0, 0, 0, 2, 0) == 5);
  // delete: a bad handle fails, a good one works once
  CHECK(f.u(N(DeleteObjectApp), br) == 1 && f.u(N(DeleteObjectApp), br) == 0);
  CHECK(f.u(N(DeleteObjectApp), 0x12345) == 0);
  // an object selected into a DC cannot be deleted
  const uint32_t dc = make_dc(f, 8, 8);
  const uint32_t b2 = f.u(N(CreateSolidBrush), kRed, 0);
  const uint32_t prev = f.u(N(SelectBrush), dc, b2);
  CHECK(prev == white && f.u(N(DeleteObjectApp), b2) == 0);
  f.u(N(SelectBrush), dc, prev);
  CHECK(f.u(N(DeleteObjectApp), b2) == 1);
  CHECK(f.u(N(DeleteObjectApp), dc) == 1);
  // handles of another process are not usable
  Program other(22);
  const uint32_t theirs = uint32_t(other.call(N(CreateSolidBrush), kRed, 0));
  CHECK(theirs != 0 && f.u(N(DeleteObjectApp), theirs) == 0);
  process_exited(22);
}

static void test_dc_state() {
  Fixture f(23);
  const uint32_t dc = make_dc(f, 16, 16);
  // the attribute families
  CHECK(f.u(N(GetAndSetDCDword), dc, 100, kRed, kOut) == 1 && f.prog.memory.get32(kOut) == kWhite);      // SetBkColor returns the old colour
  CHECK(f.u(N(GetDCDword), dc, 1, kOut) == 1 && f.prog.memory.get32(kOut) == kRed);
  CHECK(f.u(N(GetAndSetDCDword), dc, 102, kBlue, kOut) == 1 && f.u(N(GetDCDword), dc, 9, kOut) == 1 && f.prog.memory.get32(kOut) == kBlue);      // text colour
  CHECK(f.u(N(GetAndSetDCDword), dc, 101, 1, kOut) == 1 && f.prog.memory.get32(kOut) == 2);      // bk mode TRANSPARENT; was OPAQUE
  CHECK(f.u(N(GetAndSetDCDword), dc, 101, 7, kOut) == 0);      // invalid mode
  CHECK(f.u(N(GetAndSetDCDword), dc, 106, 7, kOut) == 1 && f.prog.memory.get32(kOut) == 13);      // ROP2
  CHECK(f.u(N(GetAndSetDCDword), dc, 106, 99, kOut) == 0);
  CHECK(f.u(N(GetAndSetDCDword), dc, 999, 1, kOut) == 0);
  CHECK(f.u(N(GetDCDword), dc, 10, kOut) == 1 && f.prog.memory.get32(kOut) == 1);      // IsMemDC
  CHECK(f.u(N(GetDCDword), 0x777, 0, kOut) == 0);
  // SaveDC / RestoreDC
  CHECK(f.u(N(SaveDC), dc) == 1);
  f.u(N(GetAndSetDCDword), dc, 100, 0x445566, kOut);
  const uint32_t br = f.u(N(CreateSolidBrush), kGreen, 0);
  f.u(N(SelectBrush), dc, br);
  CHECK(f.u(N(SaveDC), dc) == 2);
  f.u(N(GetAndSetDCDword), dc, 100, 0x778899, kOut);
  CHECK(f.u(N(RestoreDC), dc, 1) == 1);      // back to the state of the first save: also drops the second
  f.u(N(GetDCDword), dc, 1, kOut);
  CHECK(f.prog.memory.get32(kOut) == kRed);
  CHECK(f.u(N(GetDCObject), dc, 2) == gdi::stock_object(0));
  CHECK(f.u(N(RestoreDC), dc, 1) == 0);      // no saved state left
  CHECK(f.u(N(SaveDC), dc) == 1 && f.u(N(RestoreDC), dc, uint32_t(-1)) == 1);
  // map modes: MM_LOMETRIC flips y; LPtoDP / DPtoLP
  CHECK(f.u(N(GetAndSetDCDword), dc, 8, 2, kOut) == 1 && f.prog.memory.get32(kOut) == 1);
  f.prog.memory.put32(kIn, 254); f.prog.memory.put32(kIn + 4, 254);
  CHECK(f.u(N(TransformPoints), dc, kIn, kOut, 1, 0) == 1);
  CHECK(int32_t(f.prog.memory.get32(kOut)) == 96 && int32_t(f.prog.memory.get32(kOut + 4)) == -96);
  CHECK(f.u(N(TransformPoints), dc, kOut, kIn + 0x100, 1, 1) == 1 && int32_t(f.prog.memory.get32(kIn + 0x100)) == 254 && int32_t(f.prog.memory.get32(kIn + 0x104)) == 254);
  // viewport / window origin through the extended methods
  f.u(N(GetAndSetDCDword), dc, 8, 1, kOut);
  f.u(N(GetAndSetDCDword), dc, 124, 10, kOut);      // viewport org x
  f.u(N(GetAndSetDCDword), dc, 125, 20, kOut);
  f.prog.memory.put32(kIn, 1); f.prog.memory.put32(kIn + 4, 2);
  f.u(N(TransformPoints), dc, kIn, kOut, 1, 0);
  CHECK(int32_t(f.prog.memory.get32(kOut)) == 11 && int32_t(f.prog.memory.get32(kOut + 4)) == 22);
  CHECK(f.u(N(GetDCPoint), dc, 3, kOut) == 1 && int32_t(f.prog.memory.get32(kOut)) == 10);
  // current position and brush origin
  CHECK(f.u(N(MoveTo), dc, 5, 6, kOut) == 1 && f.u(N(MoveTo), dc, 7, 8, kOut) == 1 && int32_t(f.prog.memory.get32(kOut)) == 5 && int32_t(f.prog.memory.get32(kOut + 4)) == 6);
  CHECK(f.u(N(GetDCPoint), dc, 1, kOut) == 1 && int32_t(f.prog.memory.get32(kOut)) == 7);
  CHECK(f.u(N(SetBrushOrg), dc, 3, 4, kOut) == 1 && f.u(N(GetDCPoint), dc, 0, kOut) == 1 && int32_t(f.prog.memory.get32(kOut + 4)) == 4);
  // world transform needs the advanced graphics mode
  float m[6] = {2, 0, 0, 2, 5, 5};
  for (int i = 0; i < 6; ++i) { uint32_t bits; __builtin_memcpy(&bits, &m[i], 4); f.prog.memory.put32(kIn + 4 * i, bits); }
  CHECK(f.u(N(ModifyWorldTransform), dc, kIn, 4) == 0);
  CHECK(f.u(N(GetAndSetDCDword), dc, 105, 2, kOut) == 1);
  CHECK(f.u(N(ModifyWorldTransform), dc, kIn, 4) == 1);
  f.prog.memory.put32(kIn + 0x100, 3); f.prog.memory.put32(kIn + 0x104, 4);
  f.u(N(GetAndSetDCDword), dc, 124, 0, kOut);
  f.u(N(GetAndSetDCDword), dc, 125, 0, kOut);
  CHECK(f.u(N(TransformPoints), dc, kIn + 0x100, kOut, 1, 0) == 1 && int32_t(f.prog.memory.get32(kOut)) == 11 && int32_t(f.prog.memory.get32(kOut + 4)) == 13);
  CHECK(f.u(N(GetTransform), dc, 0x203, kOut) == 1 && f.prog.memory.get32(kOut) == 0x40000000u);      // eM11 == 2.0f
  CHECK(f.u(N(ModifyWorldTransform), dc, 0, 1) == 1 && f.u(N(TransformPoints), dc, kIn + 0x100, kOut, 1, 0) == 1 && int32_t(f.prog.memory.get32(kOut)) == 3);
  // a singular matrix is refused
  float z[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 6; ++i) { uint32_t bits; __builtin_memcpy(&bits, &z[i], 4); f.prog.memory.put32(kIn + 4 * i, bits); }
  CHECK(f.u(N(ModifyWorldTransform), dc, kIn, 4) == 0);
  // device caps
  CHECK(f.u(N(GetDeviceCaps), dc, 8) == 16 && f.u(N(GetDeviceCaps), dc, 12) == 32 && f.u(N(GetDeviceCaps), dc, 88) == 96);
  // miter limit
  CHECK(f.u(N(SetMiterLimit), dc, 0x41200000u, 0) == 1 && f.u(N(SetMiterLimit), dc, 0x3F000000u, 0) == 0);      // 10.0 ok, 0.5 refused
  CHECK(f.u(N(GetMiterLimit), dc, kOut) == 1 && f.prog.memory.get32(kOut) == 0x41200000u);
  // layout
  CHECK(f.u(N(SetLayout), dc, 0, 1) == 0 && f.u(N(GetDCDword), dc, 6, kOut) == 1 && f.prog.memory.get32(kOut) == 1);
  // GetDCObject for the bitmap, and the DC that has a bitmap selected
  CHECK(f.u(N(GetDCObject), dc, 7) != 0 && f.u(N(GetDCforBitmap), f.u(N(GetDCObject), dc, 7)) == dc);
  // CombineTransform: scale by two, then move by ten
  {
    const float xa[6] = {2, 0, 0, 2, 0, 0}, xb[6] = {1, 0, 0, 1, 10, 0};
    for (int i = 0; i < 6; ++i) { uint32_t bits; __builtin_memcpy(&bits, &xa[i], 4); f.prog.memory.put32(kIn + 4 * i, bits); __builtin_memcpy(&bits, &xb[i], 4); f.prog.memory.put32(kIn + 0x40 + 4 * i, bits); }
    CHECK(f.u(N(CombineTransform), kOut, kIn, kIn + 0x40) == 1 && f.prog.memory.get32(kOut) == 0x40000000u && f.prog.memory.get32(kOut + 16) == 0x41200000u);
    CHECK(f.u(N(CombineTransform), 0x10, kIn, kIn + 0x40) == 0);
  }
  CHECK(f.u(N(DeleteObjectApp), dc) == 1);
}

static void test_pixels_and_shapes() {
  Fixture f(24);
  uint32_t bm = 0;
  const uint32_t dc = make_dc(f, 100, 100, &bm);
  // SetPixel / GetPixel and the edges
  CHECK(f.u(N(SetPixel), dc, 5, 5, kRed) == kRed && px(f, dc, 5, 5) == kRed);
  CHECK(px(f, dc, 6, 5) == 0 && px(f, dc, -1, 0) == 0xFFFFFFFFu && px(f, dc, 100, 0) == 0xFFFFFFFFu);
  CHECK(f.u(N(SetPixel), dc, 100, 100, kRed) == 0xFFFFFFFFu);
  // PatBlt with a solid brush, ROPs
  const uint32_t green = f.u(N(CreateSolidBrush), kGreen, 0);
  f.u(N(SelectBrush), dc, green);
  CHECK(f.u(N(PatBlt), dc, 10, 10, 20, 20, kPatCopy) == 1);
  CHECK(px(f, dc, 10, 10) == kGreen && px(f, dc, 29, 29) == kGreen && px(f, dc, 30, 30) == 0 && px(f, dc, 9, 10) == 0);
  CHECK(f.u(N(PatBlt), dc, 10, 10, 5, 5, kDstInvert) == 1 && px(f, dc, 11, 11) == 0xFF00FF);
  CHECK(f.u(N(PatBlt), dc, 0, 0, 3, 3, kWhiteness) == 1 && px(f, dc, 1, 1) == kWhite);
  CHECK(f.u(N(PatBlt), dc, 0, 0, 3, 3, kBlackness) == 1 && px(f, dc, 1, 1) == 0);
  CHECK(f.u(N(PatBlt), dc, 0, 0, 3, 3, kSrcCopy) == 0);      // needs a source
  // Rectangle: 1 pixel black pen frame, green interior
  CHECK(f.u(N(Rectangle), dc, 40, 40, 60, 60) == 1);
  CHECK(px(f, dc, 40, 40) == 0 && px(f, dc, 59, 59) == 0 && px(f, dc, 50, 50) == kGreen && px(f, dc, 60, 60) == 0 && px(f, dc, 50, 40) == 0);
  CHECK(px(f, dc, 41, 41) == kGreen && px(f, dc, 58, 58) == kGreen);
  // null pen: the whole rectangle is brush
  f.u(N(SelectPen), dc, gdi::stock_object(8));
  f.u(N(Rectangle), dc, 70, 70, 80, 80);
  CHECK(px(f, dc, 70, 70) == kGreen && px(f, dc, 79, 79) == kGreen && px(f, dc, 80, 80) == 0);
  // lines with a red pen
  const uint32_t red_pen = f.u(N(CreatePen), 0, 1, kRed, 0);
  f.u(N(SelectPen), dc, red_pen);
  f.u(N(MoveTo), dc, 0, 99, 0);
  CHECK(f.u(N(LineTo), dc, 99, 99) == 1);
  CHECK(px(f, dc, 0, 99) == kRed && px(f, dc, 50, 99) == kRed && px(f, dc, 98, 99) == kRed && px(f, dc, 99, 99) == 0);      // the end point is not drawn
  f.u(N(MoveTo), dc, 0, 90, 0);
  f.u(N(LineTo), dc, 9, 81);      // a diagonal
  CHECK(px(f, dc, 0, 90) == kRed && px(f, dc, 4, 86) == kRed && px(f, dc, 5, 90) == 0);
  CHECK(f.u(N(GetDCPoint), dc, 1, kOut) == 1 && int32_t(f.prog.memory.get32(kOut)) == 9);
  // dashed pen leaves gaps
  const uint32_t dash = f.u(N(CreatePen), 2, 1, kBlue, 0);      // PS_DOT
  f.u(N(SelectPen), dc, dash);
  f.u(N(GetAndSetDCDword), dc, 101, 1, kOut);      // transparent: gaps stay
  f.u(N(MoveTo), dc, 0, 95, 0);
  f.u(N(LineTo), dc, 40, 95);
  int blue = 0, black = 0;
  for (int x = 0; x < 40; ++x) (px(f, dc, x, 95) == kBlue ? blue : black)++;
  CHECK(blue > 10 && black > 10 && blue + black == 40);
  // wide pen
  const uint32_t wide = f.u(N(CreatePen), 0, 5, kRed, 0);
  f.u(N(SelectPen), dc, wide);
  f.u(N(MoveTo), dc, 20, 70, 0);
  f.u(N(LineTo), dc, 60, 70);
  CHECK(px(f, dc, 40, 68) == kRed && px(f, dc, 40, 72) == kRed && px(f, dc, 40, 66) != kRed && px(f, dc, 40, 74) != kRed);
  // Ellipse: a filled disc
  f.u(N(SelectPen), dc, gdi::stock_object(8));
  const uint32_t blue_brush = f.u(N(CreateSolidBrush), kBlue, 0);
  f.u(N(SelectBrush), dc, blue_brush);
  CHECK(f.u(N(PatBlt), dc, 0, 0, 100, 100, kBlackness) == 1);
  CHECK(f.u(N(Ellipse), dc, 10, 10, 90, 90) == 1);
  CHECK(px(f, dc, 50, 50) == kBlue && px(f, dc, 12, 50) == kBlue && px(f, dc, 50, 12) == kBlue && px(f, dc, 11, 11) == 0 && px(f, dc, 88, 88) == 0 && px(f, dc, 5, 50) == 0);
  // RoundRect
  f.u(N(PatBlt), dc, 0, 0, 100, 100, kBlackness);
  CHECK(f.u(N(RoundRect), dc, 10, 10, 90, 90, 40, 40) == 1);
  CHECK(px(f, dc, 50, 50) == kBlue && px(f, dc, 10, 50) == kBlue && px(f, dc, 10, 10) == 0 && px(f, dc, 89, 89) == 0 && px(f, dc, 35, 11) == kBlue && px(f, dc, 13, 13) == 0);
  // PolyPolygon (a triangle) with ALTERNATE fill
  f.u(N(PatBlt), dc, 0, 0, 100, 100, kBlackness);
  const int32_t tri[6] = {50, 10, 90, 90, 10, 90};
  for (int i = 0; i < 6; ++i) f.prog.memory.put32(kIn + 4 * i, uint32_t(tri[i]));
  f.prog.memory.put32(kIn + 0x100, 3);
  CHECK(f.u(N(PolyPolyDraw), dc, kIn, kIn + 0x100, 1, 1) == 1);
  CHECK(px(f, dc, 50, 60) == kBlue && px(f, dc, 50, 85) == kBlue && px(f, dc, 15, 30) == 0 && px(f, dc, 85, 30) == 0 && px(f, dc, 50, 5) == 0);
  CHECK(f.u(N(PolyPolyDraw), dc, kIn, kIn + 0x100, 0, 1) != 0 ? false : true);      // zero polygons is an error (returns 0)
  // polyline with the red pen
  f.u(N(SelectPen), dc, red_pen);
  f.u(N(PatBlt), dc, 0, 0, 100, 100, kBlackness);
  const int32_t line[6] = {5, 5, 50, 5, 50, 50};
  for (int i = 0; i < 6; ++i) f.prog.memory.put32(kIn + 4 * i, uint32_t(line[i]));
  f.prog.memory.put32(kIn + 0x100, 3);
  CHECK(f.u(N(PolyPolyDraw), dc, kIn, kIn + 0x100, 1, 2) == 1);
  CHECK(px(f, dc, 25, 5) == kRed && px(f, dc, 50, 25) == kRed && px(f, dc, 25, 25) == 0);
  // flood fill
  f.u(N(PatBlt), dc, 0, 0, 100, 100, kBlackness);
  f.u(N(SelectPen), dc, red_pen);
  f.u(N(SelectBrush), dc, gdi::stock_object(5));      // hollow
  f.u(N(Rectangle), dc, 20, 20, 60, 60);
  f.u(N(SelectBrush), dc, blue_brush);
  CHECK(f.u(N(ExtFloodFill), dc, 30, 30, kRed, 0) == 1);      // FLOODFILLBORDER
  CHECK(px(f, dc, 30, 30) == kBlue && px(f, dc, 58, 58) == kBlue && px(f, dc, 20, 20) == kRed && px(f, dc, 10, 10) == 0 && px(f, dc, 61, 61) == 0);
  // hatch brush
  const uint32_t hatch = f.u(N(CreateHatchBrushInternal), 4, kGreen, 0);      // HS_CROSS
  f.u(N(SelectBrush), dc, hatch);
  f.u(N(GetAndSetDCDword), dc, 101, 2, kOut);      // opaque: gaps use the bk colour (white by default)
  f.u(N(PatBlt), dc, 0, 0, 16, 16, kPatCopy);
  int greens = 0, whites = 0;
  for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) { const uint32_t c = px(f, dc, x, y); greens += c == kGreen; whites += c == kWhite; }
  CHECK(greens > 20 && greens < 100 && greens + whites == 256);
  // FillRgn / FrameRgn / InvertRgn
  f.u(N(PatBlt), dc, 0, 0, 100, 100, kBlackness);
  const uint32_t rgn = f.u(N(CreateRectRgn), 10, 10, 30, 30);
  CHECK(f.u(N(FillRgn), dc, rgn, green) == 1 && px(f, dc, 10, 10) == kGreen && px(f, dc, 29, 29) == kGreen && px(f, dc, 30, 30) == 0);
  CHECK(f.u(N(FrameRgn), dc, rgn, blue_brush, 2, 2) == 1 && px(f, dc, 10, 10) == kBlue && px(f, dc, 11, 11) == kBlue && px(f, dc, 15, 15) == kGreen && px(f, dc, 28, 15) == kBlue);
  CHECK(f.u(N(InvertRgn), dc, rgn) == 1 && px(f, dc, 15, 15) == 0xFF00FF && px(f, dc, 10, 10) == 0x00FFFF);
  // arcs and pies
  f.u(N(PatBlt), dc, 0, 0, 100, 100, kBlackness);
  f.u(N(SelectPen), dc, red_pen);
  CHECK(f.u(N(ArcInternal), 0, dc, 10, 10, 90, 90, 90, 50, 50, 10) == 1);      // the upper right quarter (counter-clockwise from 3 o'clock to 12 o'clock)
  CHECK(px(f, dc, 78, 22) == kRed || px(f, dc, 79, 22) == kRed || px(f, dc, 77, 22) == kRed);
  CHECK(px(f, dc, 30, 78) == 0 && px(f, dc, 50, 50) == 0);
  f.u(N(PatBlt), dc, 0, 0, 100, 100, kBlackness);
  f.u(N(SelectBrush), dc, blue_brush);
  CHECK(f.u(N(ArcInternal), 3, dc, 10, 10, 90, 90, 90, 50, 50, 10) == 1);      // a pie
  CHECK(px(f, dc, 60, 40) == kBlue && px(f, dc, 40, 60) == 0 && px(f, dc, 40, 40) == 0);
  CHECK(f.u(N(ArcInternal), 9, dc, 10, 10, 90, 90, 90, 50, 50, 10) == 0);
  // the error cases
  CHECK(f.u(N(LineTo), 0x999, 1, 1) == 0 && f.u(N(Rectangle), 0, 1, 1, 2, 2) == 0 && f.u(N(PolyPolyDraw), dc, 0, kIn + 0x100, 1, 1) == 0);
  CHECK(f.u(N(DeleteObjectApp), dc) == 1);
}

static void test_blt() {
  Fixture f(25);
  const uint32_t dst = make_dc(f, 40, 40), src = make_dc(f, 40, 40);
  const uint32_t green = f.u(N(CreateSolidBrush), kGreen, 0), red = f.u(N(CreateSolidBrush), kRed, 0);
  f.u(N(SelectBrush), src, green);
  f.u(N(PatBlt), src, 0, 0, 10, 10, kPatCopy);
  f.u(N(SelectBrush), src, red);
  f.u(N(PatBlt), src, 10, 0, 10, 10, kPatCopy);
  // BitBlt SRCCOPY
  CHECK(f.u(N(BitBlt), dst, 5, 5, 20, 10, src, 0, 0, kSrcCopy, 0, 0) == 1);
  CHECK(px(f, dst, 5, 5) == kGreen && px(f, dst, 14, 14) == kGreen && px(f, dst, 15, 5) == kRed && px(f, dst, 24, 14) == kRed && px(f, dst, 25, 5) == 0 && px(f, dst, 4, 5) == 0);
  // SRCINVERT twice restores
  f.u(N(PatBlt), dst, 0, 0, 40, 40, kWhiteness);
  CHECK(f.u(N(BitBlt), dst, 0, 0, 20, 10, src, 0, 0, kSrcInvert, 0, 0) == 1 && px(f, dst, 2, 2) == (kWhite ^ kGreen));
  f.u(N(BitBlt), dst, 0, 0, 20, 10, src, 0, 0, kSrcInvert, 0, 0);
  CHECK(px(f, dst, 2, 2) == kWhite && px(f, dst, 12, 2) == kWhite);
  // a rop that needs a source with no source DC fails
  CHECK(f.u(N(BitBlt), dst, 0, 0, 5, 5, 0, 0, 0, kSrcCopy, 0, 0) == 0);
  // clipped source rectangle reads nothing outside the bitmap
  f.u(N(PatBlt), dst, 0, 0, 40, 40, kBlackness);
  CHECK(f.u(N(BitBlt), dst, 0, 0, 20, 20, src, 30, 30, kSrcCopy, 0, 0) == 1 && px(f, dst, 0, 0) == 0);
  // StretchBlt: 10x10 green square to 30x30
  f.u(N(PatBlt), dst, 0, 0, 40, 40, kBlackness);
  CHECK(f.u(N(StretchBlt), dst, 0, 0, 30, 30, src, 0, 0, 10, 10, kSrcCopy, 0) == 1);
  CHECK(px(f, dst, 0, 0) == kGreen && px(f, dst, 29, 29) == kGreen && px(f, dst, 30, 30) == 0);
  // a mirrored stretch (negative destination width) flips the picture
  f.u(N(PatBlt), dst, 0, 0, 40, 40, kBlackness);
  CHECK(f.u(N(StretchBlt), dst, 20, 0, uint64_t(uint32_t(-20)), 10, src, 0, 0, 20, 10, kSrcCopy, 0) == 1);
  CHECK(px(f, dst, 19, 5) == kGreen && px(f, dst, 0, 5) == kRed);
  // overlapping blit within one surface reads from a copy
  f.u(N(PatBlt), src, 0, 0, 40, 40, kBlackness);
  f.u(N(SelectBrush), src, green);
  f.u(N(PatBlt), src, 0, 0, 10, 10, kPatCopy);
  CHECK(f.u(N(BitBlt), src, 5, 0, 20, 10, src, 0, 0, kSrcCopy, 0, 0) == 1 && px(f, src, 5, 5) == kGreen && px(f, src, 14, 5) == kGreen && px(f, src, 15, 5) == 0);
  // TransparentBlt skips the key colour
  f.u(N(PatBlt), dst, 0, 0, 40, 40, kBlackness);
  f.u(N(PatBlt), src, 0, 0, 40, 40, kBlackness);
  f.u(N(SelectBrush), src, green);
  f.u(N(PatBlt), src, 0, 0, 10, 10, kPatCopy);
  CHECK(f.u(N(TransparentBlt), dst, 0, 0, 20, 20, src, 0, 0, 20, 20, 0) == 1 && px(f, dst, 5, 5) == kGreen && px(f, dst, 15, 15) == 0);
  f.u(N(SelectBrush), dst, red);
  f.u(N(PatBlt), dst, 0, 0, 40, 40, kPatCopy);
  CHECK(f.u(N(TransparentBlt), dst, 0, 0, 20, 20, src, 0, 0, 20, 20, 0) == 1 && px(f, dst, 5, 5) == kGreen && px(f, dst, 15, 15) == kRed);
  // AlphaBlend with constant alpha 50%
  f.u(N(PatBlt), dst, 0, 0, 40, 40, kBlackness);
  CHECK(f.u(N(AlphaBlend), dst, 0, 0, 10, 10, src, 0, 0, 10, 10, 0x00800000u, 0) == 1);
  const uint32_t mixed = px(f, dst, 2, 2);
  CHECK((mixed & 0xFF00) >= 0x7F00 && (mixed & 0xFF00) <= 0x8000 && (mixed & 0xFF) == 0);
  CHECK(f.u(N(AlphaBlend), dst, 0, 0, 10, 10, src, 0, 0, 10, 10, 0x00800001u, 0) == 0);      // only AC_SRC_OVER exists
  // MaskBlt: only the pixels the mask has set come from the source
  f.u(N(PatBlt), dst, 0, 0, 40, 40, kBlackness);
  const uint32_t mask = f.u(N(CreateBitmap), 10, 10, 1, 1, 0);
  const uint32_t mdc = f.u(N(CreateCompatibleDC), 0);
  f.u(N(SelectBitmap), mdc, mask);
  f.u(N(PatBlt), mdc, 0, 0, 10, 10, kBlackness);
  f.u(N(PatBlt), mdc, 0, 0, 5, 10, kWhiteness);
  CHECK(f.u(N(MaskBlt), dst, 0, 0, 10, 10, src, 0, 0, mask, 0, 0, 0xAACC0000u, 0) == 1);
  CHECK(px(f, dst, 2, 2) == kGreen && px(f, dst, 7, 2) == 0);
  // GradientFill, horizontal
  f.u(N(PatBlt), dst, 0, 0, 40, 40, kBlackness);
  uint8_t v[32] = {};
  auto put_vertex = [&](uint64_t at, int32_t x, int32_t y, uint16_t r, uint16_t g, uint16_t b) {
    f.prog.memory.put32(at, uint32_t(x)); f.prog.memory.put32(at + 4, uint32_t(y)); f.prog.memory.put16(at + 8, r); f.prog.memory.put16(at + 10, g); f.prog.memory.put16(at + 12, b); f.prog.memory.put16(at + 14, 0);
  };
  (void)v;
  put_vertex(kIn, 0, 0, 0, 0, 0);
  put_vertex(kIn + 16, 31, 10, 0xFF00, 0, 0);
  f.prog.memory.put32(kIn + 0x100, 0); f.prog.memory.put32(kIn + 0x104, 1);
  CHECK(f.u(N(GradientFill), dst, kIn, 2, kIn + 0x100, 1, 0) == 1);
  CHECK((px(f, dst, 0, 3) & 0xFF) < 10 && (px(f, dst, 31 - 1, 3) & 0xFF) > 240 && (px(f, dst, 16, 3) & 0xFF) > 100 && (px(f, dst, 16, 3) & 0xFF) < 150);
  CHECK(f.u(N(GradientFill), dst, kIn, 2, kIn + 0x100, 1, 9) == 0);
  // PlgBlt: an unrotated parallelogram is a plain copy
  f.u(N(PatBlt), dst, 0, 0, 40, 40, kBlackness);
  f.prog.memory.put32(kIn, 20); f.prog.memory.put32(kIn + 4, 20); f.prog.memory.put32(kIn + 8, 30); f.prog.memory.put32(kIn + 12, 20); f.prog.memory.put32(kIn + 16, 20); f.prog.memory.put32(kIn + 20, 30);
  CHECK(f.u(N(PlgBlt), dst, kIn, src, 0, 0, 10, 10, 0, 0, 0, 0) == 1 && px(f, dst, 25, 25) == kGreen && px(f, dst, 15, 15) == 0);
}

static void test_regions() {
  Fixture f(26);
  const uint32_t a = f.u(N(CreateRectRgn), 0, 0, 10, 10), b = f.u(N(CreateRectRgn), 5, 5, 15, 15), c = f.u(N(CreateRectRgn), 0, 0, 0, 0);
  CHECK(a && b && c && ((a >> 16) & 0x7F) == 4);
  CHECK(f.u(N(GetRgnBox), c, kOut) == 1);      // NULLREGION
  CHECK(f.u(N(CombineRgn), c, a, b, 2) == 3 && f.u(N(GetRgnBox), c, kOut) == 3 && f.rect(kOut, 0) == 0 && f.rect(kOut, 2) == 15 && f.rect(kOut, 3) == 15);
  CHECK(f.u(N(CombineRgn), c, a, b, 1) == 2 && f.u(N(GetRgnBox), c, kOut) == 2 && f.rect(kOut, 0) == 5 && f.rect(kOut, 1) == 5 && f.rect(kOut, 2) == 10 && f.rect(kOut, 3) == 10);
  CHECK(f.u(N(CombineRgn), c, a, b, 4) == 3 && f.u(N(PtInRegion), c, 2, 2) == 1 && f.u(N(PtInRegion), c, 7, 7) == 0 && f.u(N(PtInRegion), c, 12, 12) == 0);
  CHECK(f.u(N(CombineRgn), c, a, b, 3) == 3 && f.u(N(PtInRegion), c, 7, 7) == 0 && f.u(N(PtInRegion), c, 12, 12) == 1 && f.u(N(PtInRegion), c, 2, 2) == 1);
  CHECK(f.u(N(CombineRgn), c, a, 0, 5) == 2 && f.u(N(EqualRgn), a, c) == 1 && f.u(N(EqualRgn), a, b) == 0);
  CHECK(f.u(N(CombineRgn), c, a, b, 7) == 0 && f.u(N(CombineRgn), 0x1234, a, b, 2) == 0);
  // RectInRegion
  f.prog.memory.put32(kIn, 8); f.prog.memory.put32(kIn + 4, 8); f.prog.memory.put32(kIn + 8, 20); f.prog.memory.put32(kIn + 12, 20);
  CHECK(f.u(N(RectInRegion), a, kIn) == 1);
  f.prog.memory.put32(kIn, 10);
  f.prog.memory.put32(kIn + 4, 0);
  f.prog.memory.put32(kIn + 8, 20);
  f.prog.memory.put32(kIn + 12, 5);
  CHECK(f.u(N(RectInRegion), a, kIn) == 0);
  // OffsetRgn
  CHECK(f.u(N(OffsetRgn), c, 100, 50) == 2 && f.u(N(GetRgnBox), c, kOut) == 2 && f.rect(kOut, 0) == 100 && f.rect(kOut, 1) == 50);
  // GetRegionData
  CHECK(f.u(N(CombineRgn), c, a, b, 2) == 3);
  const uint32_t need = f.u(N(GetRegionData), c, 0, 0);
  CHECK(need == 32 + 16 * 3);      // the union is three bands: 0..5, 5..10, 10..15
  CHECK(f.u(N(GetRegionData), c, need - 1, kBig) == 0);
  CHECK(f.u(N(GetRegionData), c, need, kBig) == need && f.prog.memory.get32(kBig) == 32 && f.prog.memory.get32(kBig + 4) == 1 && f.prog.memory.get32(kBig + 8) == 3);
  // ExtCreateRegion from that data
  const uint32_t again = f.u(N(ExtCreateRegion), 0, need, kBig);
  CHECK(again && f.u(N(EqualRgn), again, c) == 1);
  f.prog.memory.put32(kBig, 31);      // a bad header size
  CHECK(f.u(N(ExtCreateRegion), 0, need, kBig) == 0);
  f.prog.memory.put32(kBig, 32); f.prog.memory.put32(kBig + 8, 1000);      // more rectangles than the data holds
  CHECK(f.u(N(ExtCreateRegion), 0, need, kBig) == 0);
  // elliptic and round-rect regions
  const uint32_t e = f.u(N(CreateEllipticRgn), 0, 0, 40, 40);
  CHECK(e && f.u(N(PtInRegion), e, 20, 20) == 1 && f.u(N(PtInRegion), e, 1, 1) == 0 && f.u(N(PtInRegion), e, 20, 1) == 1 && f.u(N(PtInRegion), e, 39, 20) == 1);
  const uint32_t rr = f.u(N(CreateRoundRectRgn), 0, 0, 40, 40, 20, 20);
  CHECK(rr && f.u(N(PtInRegion), rr, 20, 20) == 1 && f.u(N(PtInRegion), rr, 0, 0) == 0 && f.u(N(PtInRegion), rr, 20, 0) == 1);
  // SetRectRgn
  CHECK(f.u(N(SetRectRgn), c, 1, 2, 11, 12) == 1 && f.u(N(GetRgnBox), c, kOut) == 2 && f.rect(kOut, 3) == 12);
  CHECK(f.u(N(SetRectRgn), 0x77, 1, 2, 11, 12) == 0);
  // contract functions used by the window manager
  gdi::Rect rs[3] = {{0, 0, 10, 10}, {5, 5, 20, 20}, {0, 0, 0, 0}};
  const uint32_t h = gdi::create_region(26, ArrayView<const gdi::Rect>(rs, 3));
  CHECK(h && gdi::is_region(h));
  List<gdi::Rect> out(*system_allocator());
  CHECK(gdi::region_rects(h, &out) && out.count() == 3);
  uint64_t area = 0;
  for (const gdi::Rect& r : out.view()) area += uint64_t(r.right - r.left) * uint64_t(r.bottom - r.top);
  CHECK(area == 100 + 225 - 25);      // the union, no overlap counted twice
  gdi::Rect one[1] = {{1, 1, 3, 3}};
  CHECK(gdi::replace_region(h, ArrayView<const gdi::Rect>(one, 1)) && f.u(N(PtInRegion), h, 2, 2) == 1 && f.u(N(PtInRegion), h, 7, 7) == 0);
  CHECK(!gdi::is_region(f.u(N(CreateSolidBrush), 0, 0)));
  // random rectangle sets: union area by inclusion-exclusion over a grid
  uint32_t seed = 12345;
  for (int round = 0; round < 20; ++round) {
    gdi::Rect rects[6];
    for (auto& r : rects) {
      seed = seed * 1664525u + 1013904223u;
      const int32_t x = int32_t(seed >> 24) % 30, y = int32_t(seed >> 16 & 0xFF) % 30, w = int32_t(seed >> 8 & 0xF) + 1, hh = int32_t(seed & 0xF) + 1;
      r = gdi::Rect{x, y, x + w, y + hh};
    }
    const uint32_t reg = gdi::create_region(26, ArrayView<const gdi::Rect>(rects, 6));
    int expected = 0, got = 0;
    for (int y = 0; y < 50; ++y) for (int x = 0; x < 50; ++x) {
      bool in = false;
      for (auto& r : rects) in = in || (x >= r.left && x < r.right && y >= r.top && y < r.bottom);
      expected += in;
      got += f.u(N(PtInRegion), reg, x, y) == 1;
    }
    CHECK(expected == got);
    // the complement against a big rectangle plus the region has the area of the big rectangle
    const uint32_t big = f.u(N(CreateRectRgn), 0, 0, 50, 50), tmp = f.u(N(CreateRectRgn), 0, 0, 1, 1);
    f.u(N(CombineRgn), tmp, big, reg, 4);
    int rest = 0;
    for (int y = 0; y < 50; ++y) for (int x = 0; x < 50; ++x) rest += f.u(N(PtInRegion), tmp, x, y) == 1;
    CHECK(rest == 2500 - expected);
  }
}

static void test_clipping() {
  Fixture f(27);
  const uint32_t dc = make_dc(f, 50, 50);
  const uint32_t green = f.u(N(CreateSolidBrush), kGreen, 0);
  f.u(N(SelectBrush), dc, green);
  CHECK(f.u(N(IntersectClipRect), dc, 10, 10, 30, 30) == 2);
  f.u(N(PatBlt), dc, 0, 0, 50, 50, kPatCopy);
  CHECK(pb(f, dc, 10, 10) == kGreen && pb(f, dc, 29, 29) == kGreen && pb(f, dc, 9, 9) == 0 && pb(f, dc, 30, 30) == 0);
  CHECK(f.u(N(ExcludeClipRect), dc, 15, 15, 25, 25) == 3);
  f.u(N(PatBlt), dc, 0, 0, 50, 50, kBlackness);
  CHECK(pb(f, dc, 20, 20) == kGreen && pb(f, dc, 12, 12) == 0);
  CHECK(f.u(N(PtVisible), dc, 12, 12) == 1 && f.u(N(PtVisible), dc, 20, 20) == 0 && f.u(N(PtVisible), dc, 5, 5) == 0);
  f.prog.memory.put32(kIn, 0); f.prog.memory.put32(kIn + 4, 0); f.prog.memory.put32(kIn + 8, 8); f.prog.memory.put32(kIn + 12, 8);
  CHECK(f.u(N(RectVisible), dc, kIn) == 0);
  CHECK(f.u(N(GetAppClipBox), dc, kOut) == 3 && f.rect(kOut, 0) == 10 && f.rect(kOut, 2) == 30);
  // clip region from a handle, combine modes
  const uint32_t r = f.u(N(CreateRectRgn), 0, 0, 5, 5);
  CHECK(f.u(N(ExtSelectClipRgn), dc, r, 5) == 2);
  f.u(N(PatBlt), dc, 0, 0, 50, 50, kPatCopy);
  CHECK(pb(f, dc, 2, 2) == kGreen && pb(f, dc, 20, 20) == kGreen && pb(f, dc, 7, 7) == 0);      // (10,10) was green from before; (7,7) is outside the clip
  f.u(N(PatBlt), dc, 0, 0, 50, 50, kBlackness);
  CHECK(pb(f, dc, 2, 2) == 0 && pb(f, dc, 20, 20) == kGreen);
  CHECK(f.u(N(ExtSelectClipRgn), dc, 0, 5) == 2 && f.u(N(ExtSelectClipRgn), dc, 0, 1) == 0);
  f.u(N(PatBlt), dc, 0, 0, 50, 50, kBlackness);
  CHECK(pb(f, dc, 10, 10) == 0);
  // OffsetClipRgn
  f.u(N(IntersectClipRect), dc, 0, 0, 10, 10);
  CHECK(f.u(N(OffsetClipRgn), dc, 20, 20) == 2);
  f.u(N(PatBlt), dc, 0, 0, 50, 50, kPatCopy);
  CHECK(pb(f, dc, 25, 25) == kGreen && pb(f, dc, 5, 5) == 0);
  // meta region survives a clip reset
  f.u(N(ExtSelectClipRgn), dc, 0, 5);
  f.u(N(PatBlt), dc, 0, 0, 50, 50, kBlackness);
  f.u(N(IntersectClipRect), dc, 0, 0, 10, 10);
  CHECK(f.u(N(SetMetaRgn), dc) == 2);
  f.u(N(ExtSelectClipRgn), dc, 0, 5);
  f.u(N(PatBlt), dc, 0, 0, 50, 50, kPatCopy);
  CHECK(pb(f, dc, 5, 5) == kGreen && pb(f, dc, 20, 20) == 0);
  // GetRandomRgn: the clip (code 1) and the meta (code 2)
  CHECK(f.u(N(IntersectClipRect), dc, 2, 2, 4, 4) == 2);
  const uint32_t out = f.u(N(CreateRectRgn), 0, 0, 1, 1);
  CHECK(f.u(N(GetRandomRgn), dc, out, 1) == 1 && f.u(N(GetRgnBox), out, kOut) == 2 && f.rect(kOut, 0) == 2);
  CHECK(f.u(N(GetRandomRgn), dc, out, 2) == 1 && f.u(N(GetRgnBox), out, kOut) == 2 && f.rect(kOut, 2) == 10);
}

static void test_paths() {
  Fixture f(28);
  const uint32_t dc = make_dc(f, 100, 100);
  const uint32_t blue = f.u(N(CreateSolidBrush), kBlue, 0);
  f.u(N(SelectBrush), dc, blue);
  f.u(N(SelectPen), dc, gdi::stock_object(8));
  CHECK(f.u(N(FillPath), dc) == 0);      // no path yet
  CHECK(f.u(N(EndPath), dc) == 0);
  CHECK(f.u(N(BeginPath), dc) == 1);
  CHECK(f.u(N(FillPath), dc) == 0);      // still recording
  f.u(N(Rectangle), dc, 20, 20, 60, 60);
  CHECK(f.u(N(EndPath), dc) == 1);
  CHECK(f.u(N(GetPath), dc, 0, 0, 0) == 4);
  CHECK(f.u(N(GetPath), dc, kIn, kOut, 2) == 0xFFFFFFFFu);      // the buffer is too small
  CHECK(f.u(N(GetPath), dc, kIn, kOut, 4) == 4);
  CHECK(int32_t(f.prog.memory.get32(kIn)) == 20 && int32_t(f.prog.memory.get32(kIn + 4)) == 20 && f.prog.memory.get8(kOut) == 6 && (f.prog.memory.get8(kOut + 3) & 1) == 1);
  CHECK(f.u(N(FillPath), dc) == 1);
  CHECK(px(f, dc, 30, 30) == kBlue && px(f, dc, 59, 59) == kBlue && px(f, dc, 60, 60) == 0 && px(f, dc, 19, 30) == 0);
  CHECK(f.u(N(FillPath), dc) == 0);      // the path went with the fill
  // an ellipse path to a region
  f.u(N(PatBlt), dc, 0, 0, 100, 100, kBlackness);
  f.u(N(BeginPath), dc);
  f.u(N(Ellipse), dc, 10, 10, 90, 90);
  f.u(N(EndPath), dc);
  const uint32_t rgn = f.u(N(PathToRegion), dc);
  CHECK(rgn && f.u(N(PtInRegion), rgn, 50, 50) == 1 && f.u(N(PtInRegion), rgn, 11, 11) == 0 && f.u(N(PtInRegion), rgn, 50, 12) == 1);
  // a path made with MoveTo / LineTo / CloseFigure, stroked with a red pen
  f.u(N(PatBlt), dc, 0, 0, 100, 100, kBlackness);
  const uint32_t pen = f.u(N(CreatePen), 0, 1, kRed, 0);
  f.u(N(SelectPen), dc, pen);
  f.u(N(BeginPath), dc);
  f.u(N(MoveTo), dc, 10, 10, 0);
  f.u(N(LineTo), dc, 50, 10);
  f.u(N(LineTo), dc, 50, 50);
  CHECK(f.u(N(CloseFigure), dc) == 1);
  f.u(N(EndPath), dc);
  CHECK(f.u(N(StrokePath), dc) == 1);
  CHECK(px(f, dc, 30, 10) == kRed && px(f, dc, 50, 30) == kRed && px(f, dc, 30, 30) == kRed && px(f, dc, 30, 20) == 0);      // the closing edge is the diagonal
  // SelectClipPath
  f.u(N(PatBlt), dc, 0, 0, 100, 100, kBlackness);
  f.u(N(BeginPath), dc);
  f.u(N(Rectangle), dc, 0, 0, 10, 10);
  f.u(N(EndPath), dc);
  CHECK(f.u(N(SelectClipPath), dc, 5) == 1);
  f.u(N(PatBlt), dc, 0, 0, 100, 100, kPatCopy);
  CHECK(pb(f, dc, 5, 5) == kBlue && pb(f, dc, 15, 15) == 0);
  f.u(N(ExtSelectClipRgn), dc, 0, 5);
  // WidenPath gives the outline of a wide line, FlattenPath keeps the points
  const uint32_t wide = f.u(N(CreatePen), 0, 6, kRed, 0);
  f.u(N(SelectPen), dc, wide);
  f.u(N(BeginPath), dc);
  f.u(N(MoveTo), dc, 10, 50, 0);
  f.u(N(LineTo), dc, 80, 50);
  f.u(N(EndPath), dc);
  CHECK(f.u(N(WidenPath), dc) == 1);
  CHECK(f.u(N(FillPath), dc) == 1);
  CHECK(px(f, dc, 40, 50) == kBlue && px(f, dc, 40, 52) == kBlue && px(f, dc, 40, 47) == kBlue && px(f, dc, 40, 44) == 0 && px(f, dc, 40, 56) == 0);
  // a bezier path flattens to line segments
  f.u(N(BeginPath), dc);
  f.u(N(MoveTo), dc, 0, 0, 0);
  const int32_t bz[6] = {10, 40, 30, 40, 40, 0};
  for (int i = 0; i < 6; ++i) f.prog.memory.put32(kIn + 4 * i, uint32_t(bz[i]));
  f.prog.memory.put32(kIn + 0x100, 3);
  CHECK(f.u(N(PolyPolyDraw), dc, kIn, kIn + 0x100, 1, 4) == 1);      // PolyBezierTo
  f.u(N(EndPath), dc);
  CHECK(f.u(N(GetPath), dc, 0, 0, 0) == 4);
  CHECK(f.u(N(FlattenPath), dc) == 1);
  const uint32_t flat = f.u(N(GetPath), dc, 0, 0, 0);
  CHECK(flat > 10);
  CHECK(f.u(N(AbortPath), dc) == 1 && f.u(N(GetPath), dc, 0, 0, 0) == 0xFFFFFFFFu);
}

static uint64_t put_bmi(Fixture& f, uint64_t at, int w, int h, int bpp, uint32_t compression = 0, int colors = 0) {
  f.prog.memory.put32(at, 40); f.prog.memory.put32(at + 4, uint32_t(w)); f.prog.memory.put32(at + 8, uint32_t(h));
  f.prog.memory.put16(at + 12, 1); f.prog.memory.put16(at + 14, uint16_t(bpp)); f.prog.memory.put32(at + 16, compression);
  f.prog.memory.put32(at + 20, 0); f.prog.memory.put32(at + 24, 0); f.prog.memory.put32(at + 28, 0); f.prog.memory.put32(at + 32, uint32_t(colors)); f.prog.memory.put32(at + 36, 0);
  return at;
}

static void test_dib() {
  Fixture f(29);
  const uint32_t dc = f.u(N(CreateCompatibleDC), 0);
  // a 32 bpp section: the bits are in the process's memory
  put_bmi(f, kIn, 16, 16, 32);
  const uint32_t sec = f.u(N(CreateDIBSection), dc, 0, 0, kIn, 0, 40, 0, 0, kOut);
  CHECK(sec != 0);
  const uint64_t bits = f.prog.memory.get64(kOut);
  CHECK(bits >= kBase + 0x80000 && f.u(N(ExtGetObjectW), sec, 104, kBig) == 104 && f.prog.memory.get64(kBig + 24) == bits && f.prog.memory.get16(kBig + 18) == 32);
  CHECK(f.u(N(SelectBitmap), dc, sec) != 0);
  // the program writes a pixel (bottom-up: row 0 is the bottom line), GDI sees it
  f.prog.memory.put32(bits + (15 * 16 + 3) * 4, 0x00FF8040);      // y = 0 from the top is the last row in memory
  CHECK(px(f, dc, 3, 0) == 0x4080FF);
  // GDI draws, the program sees it
  const uint32_t green = f.u(N(CreateSolidBrush), kGreen, 0);
  f.u(N(SelectBrush), dc, green);
  f.u(N(PatBlt), dc, 0, 0, 4, 4, kPatCopy);
  CHECK((f.prog.memory.get32(bits + (15 * 16 + 0) * 4) & 0xFFFFFF) == 0x00FF00 && (f.prog.memory.get32(bits + (12 * 16 + 3) * 4) & 0xFFFFFF) == 0x00FF00 && (f.prog.memory.get32(bits + (12 * 16 + 4) * 4) & 0xFFFFFF) == 0);
  // top-down section
  put_bmi(f, kIn, 8, -8, 32);
  const uint32_t td = f.u(N(CreateDIBSection), dc, 0, 0, kIn, 0, 40, 0, 0, kOut);
  const uint64_t tb = f.prog.memory.get64(kOut);
  f.u(N(SelectBitmap), dc, td);
  f.u(N(PatBlt), dc, 0, 0, 2, 1, kPatCopy);
  CHECK((f.prog.memory.get32(tb) & 0xFFFFFF) == 0x00FF00);      // top-left is the first pixel in memory
  // 24 bpp
  put_bmi(f, kIn, 5, 2, 24);
  const uint32_t d24 = f.u(N(CreateDIBSection), dc, 0, 0, kIn, 0, 40, 0, 0, kOut);
  const uint64_t b24 = f.prog.memory.get64(kOut);
  f.u(N(SelectBitmap), dc, d24);
  f.u(N(SetPixel), dc, 1, 1, 0x112233);      // the bottom line is the first in memory (stride 16)
  CHECK(f.prog.memory.get8(b24 + 3) == 0x11 && f.prog.memory.get8(b24 + 4) == 0x22 && f.prog.memory.get8(b24 + 5) == 0x33);
  // 8 bpp with a colour table
  put_bmi(f, kIn, 4, 4, 8, 0, 2);
  f.prog.memory.put32(kIn + 40, 0x00000000); f.prog.memory.put32(kIn + 44, 0x000000FF);      // black, blue (RGBQUAD: B, G, R)
  const uint32_t d8 = f.u(N(CreateDIBSection), dc, 0, 0, kIn, 0, 48, 0, 0, kOut);
  const uint64_t b8 = f.prog.memory.get64(kOut);
  CHECK(d8 && b8);
  f.u(N(SelectBitmap), dc, d8);
  f.prog.memory.put8(b8 + 0, 1);      // bottom-left is index 1 -> blue
  CHECK(px(f, dc, 0, 3) == 0xFF0000 && px(f, dc, 1, 3) == 0);
  CHECK(f.u(N(SetPixel), dc, 2, 3, 0x0101FE) == 0 && f.prog.memory.get8(b8 + 2) == 0);      // the nearest table entry is black
  CHECK(f.u(N(SetPixel), dc, 3, 3, 0xFE0000) == 0xFF0000 && f.prog.memory.get8(b8 + 3) == 1);      // ... and blue
  // colour table access
  CHECK(f.u(N(DoPalette), dc, 0, 2, kOut, 4, 0) == 2 && f.prog.memory.get32(kOut + 4) == 0x000000FF);
  f.prog.memory.put32(kOut, 0x00123456);
  CHECK(f.u(N(DoPalette), dc, 1, 1, kOut, 5, 1) == 1 && px(f, dc, 0, 3) == 0x563412);
  // bad headers
  put_bmi(f, kIn, 4, 4, 7);
  CHECK(f.u(N(CreateDIBSection), dc, 0, 0, kIn, 0, 40, 0, 0, kOut) == 0);
  put_bmi(f, kIn, 0, 4, 32);
  CHECK(f.u(N(CreateDIBSection), dc, 0, 0, kIn, 0, 40, 0, 0, kOut) == 0);
  put_bmi(f, kIn, 100000, 4, 32);
  CHECK(f.u(N(CreateDIBSection), dc, 0, 0, kIn, 0, 40, 0, 0, kOut) == 0);
  CHECK(f.u(N(CreateDIBSection), dc, 0, 0, 0x10, 0, 40, 0, 0, kOut) == 0);      // unmapped header
  put_bmi(f, kIn, 4, 4, 32);
  CHECK(f.u(N(CreateDIBSection), dc, 0, 0, kIn, 0, 20, 0, 0, kOut) == 0);      // the header is cut off
  CHECK(f.u(N(CreateDIBSection), dc, 0x55, 0, kIn, 0, 40, 0, 0, kOut) == 0);      // a handle that names no section of the process

  // SetDIBitsToDevice / GetDIBits round trip on a memory DC
  const uint32_t mem = make_dc(f, 8, 8);
  put_bmi(f, kIn, 4, 4, 32);
  for (int i = 0; i < 16; ++i) f.prog.memory.put32(kBig + 4 * i, 0x00000000u | uint32_t(i * 10) << 16 | 0xFF);      // blue = i*10, red = 255
  CHECK(f.u(N(SetDIBitsToDeviceInternal), mem, 2, 2, 4, 4, 0, 0, 0, 4, kBig, kIn, 0, 64, 40, 0, 0) == 4);
  // bottom-up: the first row in memory is the bottom line of the picture
  CHECK(px(f, mem, 2, 5) == 0xFF0000 && px(f, mem, 5, 5) == (30u | 0xFF0000u) && px(f, mem, 2, 2) == (120u | 0xFF0000u) && px(f, mem, 1, 1) == 0);
  // GetDIBits: ask for 24 bpp, bottom-up
  const uint32_t bm = f.u(N(GetDCObject), mem, 7);
  put_bmi(f, kIn, 8, 8, 24);
  CHECK(f.u(N(GetDIBitsInternal), mem, bm, 0, 8, kBig + 0x400, kIn, 0, 8 * 24 + 100, 40) == 8);
  // row 0 in memory is the bottom line y = 7: all black; line y = 5 (index 2) holds the first source row
  const uint64_t row5 = kBig + 0x400 + 2 * 24;
  CHECK(f.prog.memory.get8(row5 + 2 * 3) == 0xFF && f.prog.memory.get8(row5 + 2 * 3 + 2) == 0 && f.prog.memory.get8(row5 + 5 * 3 + 2) == 30);
  // only the header
  put_bmi(f, kIn, 0, 0, 0);
  f.prog.memory.put32(kIn + 14, 0);
  CHECK(f.u(N(GetDIBitsInternal), mem, bm, 0, 0, 0, kIn, 0, 0, 40) == 1 && f.prog.memory.get32(kIn + 4) == 8 && f.prog.memory.get32(kIn + 8) == 8 && f.prog.memory.get16(kIn + 14) == 32);
  // 1 bpp output builds a table
  put_bmi(f, kIn, 8, 8, 1);
  CHECK(f.u(N(GetDIBitsInternal), mem, bm, 0, 8, kBig + 0x800, kIn, 0, 64, 48) == 8);
  // StretchDIBits 2x
  put_bmi(f, kIn, 2, 2, 32);
  f.prog.memory.put32(kBig, 0x0000FF); f.prog.memory.put32(kBig + 4, 0x00FF00); f.prog.memory.put32(kBig + 8, 0xFF0000); f.prog.memory.put32(kBig + 12, 0xFFFFFF);      // bottom row red, green; top row blue, white
  CHECK(f.u(N(StretchDIBitsInternal), mem, 0, 0, 8, 8, 0, 0, 2, 2, kBig, kIn, 0, kSrcCopy, 40, 16, 0) == 2);
  // the memory colours are BGR in the DIB (0x0000FF = red component in 0x00RRGGBB) -> COLORREF 0xFF0000 blue
  CHECK(px(f, mem, 3, 3) == 0x0000FF && px(f, mem, 4, 3) == 0xFFFFFF && px(f, mem, 3, 4) == 0xFF0000 && px(f, mem, 4, 4) == 0x00FF00);
  // CreateDIBitmap with CBM_INIT and a DIB brush
  put_bmi(f, kIn, 2, 2, 32);
  const uint32_t ddb = f.u(N(CreateDIBitmapInternal), mem, 2, 2, 4, kBig, kIn, 0, 40, 16, 0, 0);
  CHECK(ddb && f.u(N(ExtGetObjectW), ddb, 32, kOut) == 32 && f.prog.memory.get32(kOut + 4) == 2);
  f.prog.memory.put32(kBig + 0x1000 - 16, 0);
  // a packed DIB brush: header + 2x2 pixels
  put_bmi(f, kBig + 0x2000, 2, 2, 32);
  for (int i = 0; i < 4; ++i) f.prog.memory.put32(kBig + 0x2000 + 40 + 4 * i, i == 0 ? 0xFF0000u : 0x00FF00u);      // RGB 0x00RRGGBB
  const uint32_t pat = f.u(N(CreateDIBBrush), kBig + 0x2000, 0, 56, 0, 0, 0);
  CHECK(pat != 0);
  f.u(N(SelectBrush), mem, pat);
  f.u(N(PatBlt), mem, 0, 0, 4, 4, kPatCopy);
  CHECK(px(f, mem, 0, 0) != px(f, mem, 1, 0) || px(f, mem, 0, 0) != px(f, mem, 0, 1));
  CHECK(f.u(N(CreateDIBBrush), kBig + 0x2000, 0, 20, 0, 0, 0) == 0);      // the declared size is too small for the pixels
  CHECK(f.u(N(CreateDIBBrush), kBig + 0x2000, 5, 56, 0, 0, 0) == 0);
}

static void test_bitmap_bits() {
  Fixture f(30);
  // monochrome bitmap: 16-bit aligned rows in the caller's buffer
  const uint8_t rows[4] = {0xF0, 0x00, 0x0F, 0x00};      // two rows of a 12 pixel wide 1 bpp bitmap
  for (int i = 0; i < 4; ++i) f.prog.memory.put8(kIn + i, rows[i]);
  const uint32_t bm = f.u(N(CreateBitmap), 12, 2, 1, 1, kIn);
  CHECK(bm && f.u(N(GetBitmapBits), bm, 0, 0) == 4);
  CHECK(f.u(N(GetBitmapBits), bm, 4, kOut) == 4 && f.prog.memory.get8(kOut) == 0xF0 && f.prog.memory.get8(kOut + 2) == 0x0F);
  const uint32_t dc = f.u(N(CreateCompatibleDC), 0);
  f.u(N(SelectBitmap), dc, bm);
  CHECK(px(f, dc, 0, 0) == kWhite && px(f, dc, 4, 0) == 0 && px(f, dc, 4, 1) == kWhite && px(f, dc, 0, 1) == 0);
  CHECK(f.u(N(SetBitmapBits), bm, 4, kIn) == 4);
  CHECK(f.u(N(GetBitmapDimension), bm, kOut) == 1 && f.u(N(SetBitmapDimension), bm, 7, 9, kOut) == 1 && f.u(N(GetBitmapDimension), bm, kOut) == 1 && f.prog.memory.get32(kOut) == 7);
  // the object info
  CHECK(f.u(N(ExtGetObjectW), bm, 32, kOut) == 32 && f.prog.memory.get32(kOut + 4) == 12 && f.prog.memory.get32(kOut + 8) == 2 && f.prog.memory.get16(kOut + 18) == 1);
  CHECK(f.u(N(ExtGetObjectW), bm, 16, kOut) == 16);      // truncated to the buffer
  CHECK(f.u(N(MonoBitmap), bm) == 1);
  // the same bitmap cannot be selected twice
  const uint32_t dc2 = f.u(N(CreateCompatibleDC), 0);
  CHECK(f.u(N(SelectBitmap), dc2, bm) == 0);
  // bitmap_info / bitmap_pixels / create_bitmap
  int32_t w = 0, h = 0;
  uint32_t bpp = 0;
  CHECK(gdi::bitmap_info(bm, &w, &h, &bpp) && w == 12 && h == 2 && bpp == 1);
  List<uint32_t> pix(*system_allocator());
  CHECK(gdi::bitmap_pixels(bm, &pix) && pix.count() == 24 && pix[0] == 0xFFFFFFFFu && pix[4] == 0xFF000000u);
  const uint32_t argb[4] = {0x80FF0000, 0xFF00FF00, 0x00000000, 0xFF0000FF};
  const uint32_t made = gdi::create_bitmap(30, 2, 2, ArrayView<const uint32_t>(argb, 4));
  List<uint32_t> back(*system_allocator());
  CHECK(made && gdi::bitmap_pixels(made, &back) && back.count() == 4 && back[0] == 0x80FF0000 && back[3] == 0xFF0000FF);
  CHECK(gdi::create_bitmap(30, 2, 3, ArrayView<const uint32_t>(argb, 4)) == 0);      // wrong pixel count
  process_exited(30);
}

static void test_text() {
  Fixture f(31);
  const uint32_t dc = make_dc(f, 200, 60);
  f.u(N(PatBlt), dc, 0, 0, 200, 60, kWhiteness);
  f.u(N(GetAndSetDCDword), dc, 102, 0, kOut);      // text colour black
  f.u(N(GetAndSetDCDword), dc, 101, 1, kOut);      // transparent background
  f.prog.memory.put_wide(kIn, "Hello World");
  CHECK(f.u(N(ExtTextOutW), dc, 10, 10, 0, 0, kIn, 11, 0, 0) == 1);
  int dark = 0;
  for (int y = 10; y < 26; ++y) for (int x = 10; x < 98; ++x) dark += px(f, dc, x, y) == 0;
  CHECK(dark > 100);
  int outside = 0;
  for (int y = 0; y < 9; ++y) for (int x = 0; x < 200; ++x) outside += px(f, dc, x, y) != kWhite;
  for (int y = 27; y < 60; ++y) for (int x = 0; x < 200; ++x) outside += px(f, dc, x, y) != kWhite;
  CHECK(outside == 0);
  // metrics of the default font (the system font: 16 px)
  CHECK(f.u(N(GetTextMetricsW), dc, kOut, 0) == 1 && int32_t(f.prog.memory.get32(kOut)) == 16 && int32_t(f.prog.memory.get32(kOut + 20)) == 8);
  const int32_t ascent = int32_t(f.prog.memory.get32(kOut + 4)), descent = int32_t(f.prog.memory.get32(kOut + 8));
  CHECK(ascent + descent == 16 && ascent > descent);
  // extents
  CHECK(f.u(N(GetTextExtentExW), dc, kIn, 11, -1, 0, 0, kOut, 0) == 1 && int32_t(f.prog.memory.get32(kOut)) == 88 && int32_t(f.prog.memory.get32(kOut + 4)) == 16);
  CHECK(f.u(N(GetTextExtentExW), dc, kIn, 11, 30, kOut, kBig, kOut + 16, 0) == 1 && f.prog.memory.get32(kOut) == 3 && f.prog.memory.get32(kBig) == 8 && f.prog.memory.get32(kBig + 4 * 10) == 88);
  CHECK(f.u(N(GetTextExtent), dc, kIn, 5, kOut, 0) == 1 && int32_t(f.prog.memory.get32(kOut)) == 40);
  // opaque background and ETO_OPAQUE with a clip rectangle
  f.u(N(PatBlt), dc, 0, 0, 200, 60, kWhiteness);
  f.u(N(GetAndSetDCDword), dc, 100, kRed, kOut);
  f.u(N(GetAndSetDCDword), dc, 101, 2, kOut);
  f.u(N(ExtTextOutW), dc, 10, 10, 0, 0, kIn, 2, 0, 0);
  CHECK(px(f, dc, 10, 10) == kRed || px(f, dc, 11, 10) == kRed);
  CHECK(px(f, dc, 30, 10) == kWhite);
  f.prog.memory.put32(kOut, 100); f.prog.memory.put32(kOut + 4, 30); f.prog.memory.put32(kOut + 8, 140); f.prog.memory.put32(kOut + 12, 50);
  CHECK(f.u(N(ExtTextOutW), dc, 100, 30, 2 | 4, kOut, kIn, 11, 0, 0) == 1);
  CHECK(px(f, dc, 101, 31) == kRed && px(f, dc, 139, 49) == kRed && px(f, dc, 141, 40) == kWhite && px(f, dc, 150, 35) == kWhite);
  // ETO_OPAQUE needs a rectangle
  CHECK(f.u(N(ExtTextOutW), dc, 100, 30, 2, 0, kIn, 11, 0, 0) == 0);
  // alignment: right aligned text ends at x
  f.u(N(PatBlt), dc, 0, 0, 200, 60, kWhiteness);
  f.u(N(GetAndSetDCDword), dc, 101, 1, kOut);
  f.u(N(GetAndSetDCDword), dc, 107, 2, kOut);      // TA_RIGHT
  f.u(N(ExtTextOutW), dc, 100, 10, 0, 0, kIn, 5, 0, 0);
  int minx = 1000, maxx = -1;
  for (int y = 10; y < 26; ++y) for (int x = 0; x < 200; ++x) if (px(f, dc, x, y) == 0) { minx = x < minx ? x : minx; maxx = x > maxx ? x : maxx; }
  CHECK(maxx < 100 && minx >= 60 && maxx > 90);
  // explicit advances
  f.u(N(PatBlt), dc, 0, 0, 200, 60, kWhiteness);
  f.u(N(GetAndSetDCDword), dc, 107, 0, kOut);
  for (int i = 0; i < 3; ++i) f.prog.memory.put32(kBig + 4 * i, 20);
  f.u(N(ExtTextOutW), dc, 0, 0, 0, 0, kIn, 3, kBig, 0);
  int right = 0;
  for (int y = 0; y < 16; ++y) for (int x = 0; x < 200; ++x) if (px(f, dc, x, y) == 0) right = x > right ? x : right;
  CHECK(right >= 40 && right < 60);
  // a taller font scales the glyphs
  uint8_t lf[92] = {};
  lf[0] = 0xE0; lf[1] = 0xFF; lf[2] = 0xFF; lf[3] = 0xFF;      // -32
  for (int i = 0; i < 92; ++i) f.prog.memory.put8(kBig + 0x100 + i, lf[i]);
  const uint32_t big = f.u(N(HfontCreate), kBig + 0x100, 92, 0, 0, 0);
  f.u(N(SelectFont), dc, big);
  CHECK(f.u(N(GetTextMetricsW), dc, kOut, 0) == 1 && int32_t(f.prog.memory.get32(kOut)) == 32 && int32_t(f.prog.memory.get32(kOut + 20)) == 16);
  // text face and font enumeration
  CHECK(f.u(N(GetTextFaceW), dc, 0, 0, 0) > 1);
  CHECK(f.u(N(GetTextFaceW), dc, 32, kOut, 0) > 1 && f.prog.memory.get16(kOut) == 'S');
  f.prog.memory.put32(kBig, 100);
  CHECK(f.u(N(EnumFonts), dc, 1, 0, 0, 0, 0, kBig, 0) == 1 && f.prog.memory.get32(kBig) > 5);
  // widths and glyph indices
  CHECK(f.u(N(GetCharWidthW), dc, 'a', 'c', 0, 2, kOut) == 1 && f.prog.memory.get32(kOut) == 16 && f.prog.memory.get32(kOut + 8) == 16);
  CHECK(f.u(N(GetCharABCWidthsW), dc, 65, 66, 0, 1, kBig) == 1 && f.prog.memory.get32(kBig + 4) == 16);
  f.prog.memory.put16(kIn, 'A'); f.prog.memory.put16(kIn + 2, 0x4E2D);
  CHECK(f.u(N(GetGlyphIndicesW), dc, kIn, 2, kOut, 1) == 2 && f.prog.memory.get16(kOut) == 'A' && f.prog.memory.get16(kOut + 2) == 0xFFFF);
  // bad text pointers fail cleanly
  CHECK(f.u(N(ExtTextOutW), dc, 0, 0, 0, 0, 0x10, 5, 0, 0) == 0);
  CHECK(f.u(N(GetTextExtentExW), dc, 0x10, 5, -1, 0, 0, kOut, 0) == 0);
  CHECK(f.u(N(ExtTextOutW), dc, 0, 0, 0, 0, kIn, 5000000, 0, 0) == 0);
}

struct FakeProvider final : gdi::IGdiFontProvider {
  int glyph_calls = 0;
  uint8_t cover[16];
  bool match(const gdi::GdiFontRequest& r, uint32_t* face) override { *face = 7; return r.height > 0; }
  bool metrics(uint32_t, int32_t h, gdi::GdiFontMetrics* m) override { m->ascent = h * 3 / 4; m->descent = h - m->ascent; m->ave_width = 6; m->max_width = 6; m->fixed_pitch = true; return true; }
  bool glyph(uint32_t, int32_t, char32_t, gdi::GdiGlyph* g) override {
    ++glyph_calls;
    for (auto& c : cover) c = 255;
    g->advance = 6; g->left = 0; g->top = 4; g->width = 4; g->height = 4; g->coverage = ConstByteView(cover, 16);
    return true;
  }
};

static void test_font_provider() {
  Fixture f(32);
  FakeProvider provider;
  gdi::set_font_provider(&provider);
  const uint32_t dc = make_dc(f, 100, 40);
  f.u(N(PatBlt), dc, 0, 0, 100, 40, kWhiteness);
  f.u(N(GetAndSetDCDword), dc, 101, 1, kOut);
  f.prog.memory.put_wide(kIn, "AB");
  CHECK(f.u(N(ExtTextOutW), dc, 10, 10, 0, 0, kIn, 2, 0, 0) == 1 && provider.glyph_calls >= 2);
  CHECK(f.u(N(GetTextExtentExW), dc, kIn, 2, -1, 0, 0, kOut, 0) == 1 && int32_t(f.prog.memory.get32(kOut)) == 12);
  int dark = 0;
  for (int y = 0; y < 40; ++y) for (int x = 0; x < 100; ++x) dark += px(f, dc, x, y) == 0;
  CHECK(dark == 32);      // two 4x4 glyphs
  CHECK(f.u(N(GetTextMetricsW), dc, kOut, 0) == 1 && int32_t(f.prog.memory.get32(kOut + 20)) == 6);
}

static void test_screen_dc() {
  Fixture f(33);
  Screen sink;
  gdi::set_screen_sink(&sink);
  CHECK(gdi::screen_width() == 1024 && gdi::screen_height() == 768 && gdi::screen_pixels().size() == 1024u * 768 * 4);
  CHECK(!gdi::set_screen_size(8, 8) && !gdi::set_screen_size(20000, 20) && gdi::set_screen_size(320, 200) && gdi::screen_width() == 320 && gdi::screen_pixels().size() == 320u * 200 * 4);
  // a window DC whose origin is (100, 50) and that may draw in two rectangles
  const gdi::Rect clip[2] = {{100, 50, 140, 90}, {150, 50, 160, 90}};
  const uint32_t dc = gdi::create_screen_dc(33, 100, 50, ArrayView<const gdi::Rect>(clip, 2));
  CHECK(dc && gdi::is_dc(dc) && !gdi::is_dc(0x1234));
  const uint32_t green = f.u(N(CreateSolidBrush), kGreen, 0);
  f.u(N(SelectBrush), dc, green);
  CHECK(f.u(N(PatBlt), dc, 0, 0, 100, 100, kPatCopy) == 1);
  auto pixel = [&](int x, int y) { ConstByteView p = gdi::screen_pixels(); const uint64_t o = (uint64_t(y) * 320 + x) * 4; return uint32_t(p[o + 2]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o]) << 16; };
  CHECK(pixel(100, 50) == kGreen && pixel(139, 89) == kGreen && pixel(140, 60) != kGreen && pixel(150, 60) == kGreen && pixel(159, 60) == kGreen && pixel(160, 60) != kGreen && pixel(99, 60) != kGreen);
  CHECK(sink.calls > 0 && sink.dirty.left >= 100 && sink.dirty.top >= 50 && sink.dirty.right <= 160 && sink.dirty.bottom <= 90);
  // GetPixel outside the clip is CLR_INVALID; DC origin
  CHECK(px(f, dc, 45, 5) == 0xFFFFFFFFu && px(f, dc, 5, 5) == kGreen);
  CHECK(f.u(N(GetDCPoint), dc, 2, kOut) == 1 && f.prog.memory.get32(kOut) == 100 && f.prog.memory.get32(kOut + 4) == 50);
  // moving the DC
  const gdi::Rect clip2[1] = {{0, 0, 20, 20}};
  CHECK(gdi::update_screen_dc(dc, 10, 10, ArrayView<const gdi::Rect>(clip2, 1)));
  const uint32_t red = f.u(N(CreateSolidBrush), kRed, 0);
  f.u(N(SelectBrush), dc, red);
  f.u(N(PatBlt), dc, 0, 0, 5, 5, kPatCopy);
  CHECK(pixel(10, 10) == kRed && pixel(14, 14) == kRed && pixel(15, 15) != kRed);
  // an empty clip draws nothing
  CHECK(gdi::update_screen_dc(dc, 0, 0, ArrayView<const gdi::Rect>()));
  f.u(N(PatBlt), dc, 100, 100, 5, 5, kPatCopy);
  CHECK(pixel(100, 100) != kRed);
  // blit from the screen into a memory DC
  const uint32_t mem = make_dc(f, 20, 20);
  const gdi::Rect all[1] = {{0, 0, 320, 200}};
  gdi::update_screen_dc(dc, 0, 0, ArrayView<const gdi::Rect>(all, 1));
  CHECK(f.u(N(BitBlt), mem, 0, 0, 20, 20, dc, 100, 50, kSrcCopy, 0, 0) == 1 && px(f, mem, 5, 5) == kGreen);
  CHECK(gdi::delete_dc(dc) && !gdi::is_dc(dc) && !gdi::delete_dc(dc));
  // the screen DC of OpenDCW("DISPLAY")
  f.prog.memory.put_unicode_string(kIn, kIn + 0x40, "DISPLAY");
  const uint32_t disp = f.u(N(OpenDCW), kIn, 0, 0, 0, 1, 0, 0, 0);
  CHECK(disp && f.u(N(GetDeviceCaps), disp, 8) == 320 && f.u(N(GetDeviceCaps), disp, 10) == 200);
  CHECK(f.u(N(SetPixel), disp, 3, 3, kBlue) == kBlue && pixel(3, 3) == kBlue);
}

static void test_misc_and_errors() {
  Fixture f(34);
  CHECK(f.u(N(Flush)) == 1);
  const uint32_t dc = make_dc(f, 16, 16);
  // gamma ramp
  CHECK(f.u(N(GetDeviceGammaRamp), dc, kBig) == 1 && f.prog.memory.get16(kBig + 2 * 255) == 0xFFFF && f.prog.memory.get16(kBig + 2) == 257);
  // pixel formats
  CHECK(f.u(N(DescribePixelFormat), dc, 1, 40, 0) == 4 && f.u(N(DescribePixelFormat), dc, 1, 40, kOut) == 4 && f.prog.memory.get8(kOut + 9) == 32);
  CHECK(f.u(N(DescribePixelFormat), dc, 99, 40, kOut) == 0);
  CHECK(f.u(N(SetPixelFormat), dc, 1) == 1 && f.u(N(SetPixelFormat), dc, 2) == 0);
  // colour adjustment round trip
  CHECK(f.u(N(GetColorAdjustment), dc, kOut) == 1 && f.prog.memory.get16(kOut) == 24 && f.prog.memory.get16(kOut + 6) == 10000);
  f.prog.memory.put16(kOut + 6, 12000);
  CHECK(f.u(N(SetColorAdjustment), dc, kOut) == 1);
  f.prog.memory.put16(kOut + 6, 0);
  CHECK(f.u(N(GetColorAdjustment), dc, kOut) == 1 && f.prog.memory.get16(kOut + 6) == 12000);
  f.prog.memory.put16(kOut, 12);
  CHECK(f.u(N(SetColorAdjustment), dc, kOut) == 0);
  // engine bitmaps
  const uint64_t sizl = 4 | (uint64_t{3} << 32);
  const uint32_t eb = f.u(N(EngCreateBitmap), sizl, 0, 6, 0, 0);
  CHECK(eb && f.u(N(ExtGetObjectW), eb, 32, kOut) == 32 && f.prog.memory.get32(kOut + 4) == 4 && f.prog.memory.get32(kOut + 8) == 3 && f.u(N(EngDeleteSurface), eb) == 1);
  // metafile DC
  const uint32_t mf = f.u(N(CreateMetafileDC), 0);
  CHECK(mf && f.u(N(DeleteObjectApp), mf) == 1);
  // calls with bad pointers or bad handles never fault
  CHECK(f.u(N(GetRgnBox), 0x55, kOut) == 0);
  const uint32_t rg = f.u(N(CreateRectRgn), 0, 0, 5, 5);
  CHECK(f.u(N(GetRgnBox), rg, 0x10) == 0 && f.u(N(GetRegionData), rg, 100, 0x10) == 0);
  CHECK(f.u(N(TransformPoints), dc, 0x10, kOut, 1, 0) == 0 && f.u(N(TransformPoints), dc, kIn, 0x10, 1, 0) == 0);
  CHECK(f.u(N(GetTextMetricsW), dc, 0x10, 0) == 0);
  CHECK(f.u(N(GetDCPoint), dc, 1, 0) == 0 && f.u(N(GetDIBitsInternal), dc, 0, 0, 1, 0, 0, 0, 0, 0) == 0);
  CHECK(f.u(N(StretchDIBitsInternal), dc, 0, 0, 1, 1, 0, 0, 1, 1, 0x10, 0x20, 0, kSrcCopy, 40, 4, 0) == 0);
  CHECK(f.u(N(PolyPolyDraw), dc, 0x10, 0x20, 1, 1) == 0);
  CHECK(f.u(N(CreatePaletteInternal), 0x10, 4) == 0);
  CHECK(f.u(N(HfontCreate), 0x10, 92, 0, 0, 0) == 0);
  CHECK(f.u(N(ExtCreatePen), 7, 1, 0, 0, 0, 0, 3, 0x10, 0, 0, 0) == 0);
  // a truncated BITMAPINFO of every length does not crash
  put_bmi(f, kIn, 4, 4, 24);
  for (uint32_t cut = 0; cut <= 44; cut += 4) {
    uint64_t at = kBase + kSize - cut;      // the header ends exactly at the end of the mapped memory
    for (uint32_t i = 0; i < cut; ++i) f.prog.memory.put8(at + i, f.prog.memory.get8(kIn + i));
    f.u(N(CreateDIBSection), dc, 0, 0, at, 0, 40, 0, 0, kOut);
  }
  // bit flips in a header
  for (uint32_t bit = 0; bit < 40 * 8; bit += 3) {
    put_bmi(f, kIn, 4, 4, 24);
    f.prog.memory.put8(kIn + bit / 8, uint8_t(f.prog.memory.get8(kIn + bit / 8) ^ (1u << (bit % 8))));
    const uint32_t b = f.u(N(CreateDIBSection), dc, 0, 0, kIn, 0, 40, 0, 0, kOut);
    if (b) f.u(N(DeleteObjectApp), b);
  }
  // huge coordinates do not hang or overflow
  f.u(N(MoveTo), dc, 0, 0, 0);
  CHECK(f.u(N(LineTo), dc, 0x7FFFFFFF, 0x7FFFFFFF) == 1);
  CHECK(f.u(N(Rectangle), dc, 0x80000000u, 0x80000000u, 0x7FFFFFFF, 0x7FFFFFFF) == 1);
  CHECK(f.u(N(Ellipse), dc, 0, 0, 0x7FFFFFFF, 0x7FFFFFFF) == 1);
  CHECK(f.u(N(PatBlt), dc, 0x80000000u, 0x80000000u, 0x7FFFFFFF, 0x7FFFFFFF, kPatCopy) == 1);
  CHECK(f.u(N(IntersectClipRect), dc, 0x80000000u, 0x80000000u, 0x7FFFFFFF, 0x7FFFFFFF) != 0);
  CHECK(f.u(N(CreateRectRgn), 0x80000000u, 0, 0x7FFFFFFF, 10) != 0);
  CHECK(f.u(N(CreateBitmap), 100000, 100000, 1, 32, 0) == 0);
  CHECK(f.u(N(CreateBitmap), 0x7FFFFFFF, 0x7FFFFFFF, 1, 1, 0) == 0);
  // hostile polylines and polygons finish quickly: thousands of far-apart points with a thin and with a wide pen
  {
    const uint32_t thin = f.u(N(CreatePen), 0, 1, kRed, 0), thick = f.u(N(CreatePen), 0, 9, kRed, 0);
    for (uint32_t i = 0; i < 5000; ++i) {
      f.prog.memory.put32(kBig + 0x1000 + 8 * i, (i & 1) ? 0x7FFFFFF0u : 0u);
      f.prog.memory.put32(kBig + 0x1000 + 8 * i + 4, (i & 2) ? 0x7FFFFFF0u : 5u);
    }
    f.prog.memory.put32(kBig, 5000);
    f.u(N(SelectPen), dc, thin);
    CHECK(f.u(N(PolyPolyDraw), dc, kBig + 0x1000, kBig, 1, 2) == 1);
    CHECK(f.u(N(PolyPolyDraw), dc, kBig + 0x1000, kBig, 1, 1) == 1);
    f.u(N(SelectPen), dc, thick);
    CHECK(f.u(N(PolyPolyDraw), dc, kBig + 0x1000, kBig, 1, 2) == 1);
  }
  // the process-exit hook removes what the process owned
  const uint32_t br = f.u(N(CreateSolidBrush), kRed, 0);
  CHECK(f.u(N(ExtGetObjectW), br, 0, 0) == 16);
  process_exited(34);
  CHECK(f.u(N(ExtGetObjectW), br, 0, 0) == 0 && !gdi::is_dc(dc) && !gdi::is_region(rg));
  CHECK(f.u(N(ExtGetObjectW), gdi::stock_object(0), 0, 0) == 16);      // stock objects stay
}

static void test_registration() {
  System sys;
  uint32_t gdi_registered = 0;
  for (uint32_t n = kServiceBase; n < kServiceEnd; ++n) if (handler_of(n)) ++gdi_registered;
  printf("win32k services registered by all modules: %u\n", gdi_registered);
  CHECK(gdi_registered >= 150);
  CHECK(duplicate_registrations() == 0);
  gdi::shutdown();
}

// The desktop's view of the Windows programs (Win32kDesktopBridge): the keys of the USB keyboard as Windows names them, the screen as the desktop draws it, and the
// pointer handed over.
static void test_desktop_bridge() {
  WindowsKey k;
  CHECK(windows_key_of_usage(0x04, &k) && k.vk == 'A' && k.scan == 0x1E && !k.extended);           // A
  CHECK(windows_key_of_usage(0x27, &k) && k.vk == '0' && k.scan == 0x0B);                           // 0
  CHECK(windows_key_of_usage(0x28, &k) && k.vk == 0x0D && k.scan == 0x1C && !k.extended);           // Enter
  CHECK(windows_key_of_usage(0x58, &k) && k.vk == 0x0D && k.scan == 0x1C && k.extended);            // keypad Enter
  CHECK(windows_key_of_usage(0x52, &k) && k.vk == 0x26 && k.scan == 0x48 && k.extended);            // Up
  CHECK(windows_key_of_usage(0x38, &k) && k.vk == 0xBF && k.scan == 0x35);                          // /
  CHECK(windows_key_of_usage(0x45, &k) && k.vk == 0x7B && k.scan == 0x58);                          // F12
  CHECK(windows_key_of_usage(0x68, &k) && k.vk == 0x7C && k.scan == 0x64);                          // F13
  CHECK(windows_key_of_usage(0x62, &k) && k.vk == 0x60 && k.scan == 0x52);                          // keypad 0
  CHECK(windows_key_of_usage(0x65, &k) && k.vk == 0x5D && k.extended);                              // Application
  CHECK(windows_key_of_usage(0xE1, &k) && k.vk == 0xA0 && k.scan == 0x2A && !k.extended);           // left Shift
  CHECK(windows_key_of_usage(0xE6, &k) && k.vk == 0xA5 && k.scan == 0x38 && k.extended);            // right Alt
  CHECK(!windows_key_of_usage(0x01, &k) && !windows_key_of_usage(0x80, &k) && !windows_key_of_usage(0xE8, &k));

}

// A window surface: a screen DC aimed at it draws there (not on the screen), the sink hears of it in surface coordinates, a resize starts over blank, and a DC of a
// surface that is gone draws nowhere.
struct SurfaceSink : gdi::IWindowSurfaceSink {
  uint32_t window = 0, calls = 0;
  gdi::Rect dirty{};
  void surface_changed(uint32_t w, const gdi::Rect& r) override { window = w; dirty = r; ++calls; }
};

static void test_window_surfaces() {
  Fixture f(35);
  SurfaceSink sink;
  gdi::set_window_surface_sink(&sink);
  const uint32_t id = gdi::create_window_surface(0x1234, 64, 48);
  CHECK(id != 0);
  uint32_t w = 0, h = 0;
  CHECK(gdi::window_surface_size(id, &w, &h) && w == 64 && h == 48);
  CHECK(gdi::create_window_surface(0x1234, 0, 10) == 0 && gdi::create_window_surface(0x1234, 20000, 10) == 0);      // (no empty or huge ones)
  const gdi::Rect clip[1] = {{0, 0, 64, 48}};
  const uint32_t dc = gdi::create_screen_dc(35, 5, 6, ArrayView<const gdi::Rect>(clip, 1), id);      // origin (5, 6) of the surface
  const uint32_t green = f.u(N(CreateSolidBrush), kGreen, 0);
  f.u(N(SelectBrush), dc, green);
  CHECK(f.u(N(PatBlt), dc, 0, 0, 10, 10, kPatCopy) == 1);
  CHECK(sink.calls == 1 && sink.window == 0x1234 && sink.dirty.left == 5 && sink.dirty.top == 6 && sink.dirty.right == 15 && sink.dirty.bottom == 16);
  ArrayView<uint32_t> px = gdi::window_surface_pixels(id, &w, &h);
  const uint32_t want = (kGreen & 0xFF) << 16 | (kGreen & 0xFF00) | (kGreen >> 16 & 0xFF);      // COLORREF 0x00BBGGRR as 0x..RRGGBB
  CHECK(px.size() == 64u * 48 && (px[6 * 64 + 5] & 0xFFFFFF) == want && (px[15 * 64 + 14] & 0xFFFFFF) == want && (px[0] & 0xFFFFFF) != want);
  const ConstByteView screen = gdi::screen_pixels();      // (the screen is not where it went)
  const uint64_t at = (6ull * gdi::screen_width() + 5) * 4;
  CHECK(screen.size() > at + 3 && (uint32_t(screen[at + 2]) << 16 | uint32_t(screen[at + 1]) << 8 | screen[at]) != want);
  // drawing that falls outside the surface is clipped and not reported
  sink.calls = 0;
  CHECK(f.u(N(PatBlt), dc, 200, 200, 10, 10, kPatCopy) == 1 && sink.calls == 0);
  // a GPU present writes the bytes and says so
  uint32_t bw = 0, bh = 0;
  ByteView bytes = gdi::window_surface_bytes(id, &bw, &bh);
  CHECK(bytes.size() == 64u * 48 * 4 && bw == 64 && bh == 48);
  bytes[0] = 0x11;
  gdi::window_surface_damaged(id, gdi::Rect{0, 0, 1, 1});
  CHECK(sink.calls == 1 && sink.dirty.right == 1 && (gdi::window_surface_pixels(id, &w, &h)[0] & 0xFF) == 0x11);
  // resized: blank (the window paints it again), the DC still draws on it
  CHECK(gdi::resize_window_surface(id, 32, 32) && gdi::window_surface_size(id, &w, &h) && w == 32 && h == 32);
  px = gdi::window_surface_pixels(id, &w, &h);
  CHECK(px.size() == 32u * 32 && (px[6 * 32 + 5] & 0xFFFFFF) != want);
  CHECK(f.u(N(PatBlt), dc, 0, 0, 2, 2, kPatCopy) == 1 && (gdi::window_surface_pixels(id, &w, &h)[6 * 32 + 5] & 0xFFFFFF) == want);
  // gone: the DC draws nowhere, the views are empty
  gdi::destroy_window_surface(id);
  sink.calls = 0;
  f.u(N(PatBlt), dc, 0, 0, 2, 2, kPatCopy);
  CHECK(sink.calls == 0 && gdi::window_surface_pixels(id, &w, &h).empty() && !gdi::window_surface_size(id, &w, &h));
  gdi::delete_dc(dc);
  gdi::set_window_surface_sink(nullptr);
}

int main() {
  test_registration();
  test_objects();
  test_dc_state();
  test_pixels_and_shapes();
  test_blt();
  test_regions();
  test_clipping();
  test_paths();
  test_dib();
  test_bitmap_bits();
  test_text();
  test_font_provider();
  test_screen_dc();
  test_desktop_bridge();
  test_window_surfaces();
  test_misc_and_errors();
  return finish("Win32kGdiTest");
}
