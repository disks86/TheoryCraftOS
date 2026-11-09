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
#include "Process/Win32k/Win32kGdiInternal.hpp"

// The rest: gamma ramps, colour adjustment, pixel formats, escapes, metafile DCs and the engine services a user-mode driver can call.
namespace process::win32k::gdi {

uint32_t stock_object(int index) { return stock_handle(index); }

namespace {

uint16_t g_gamma[768];
bool g_gamma_ready = false;

void ensure_gamma() {
  if (g_gamma_ready) return;
  for (uint32_t ch = 0; ch < 3; ++ch) for (uint32_t i = 0; i < 256; ++i) g_gamma[ch * 256 + i] = uint16_t(i * 257);
  g_gamma_ready = true;
}

uint64_t NtGdiFlush(Call&) { return 1; }
uint64_t NtGdiInit(Call&) { return 1; }

uint64_t NtGdiGetDeviceGammaRamp(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  ensure_gamma();
  return ret_bool(c.write_array(c.arg(1), g_gamma, 768));
}

uint64_t NtGdiSetDeviceGammaRamp(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  uint16_t ramp[768];
  if (!c.read_array(c.arg(1), ramp, 768)) return c.fail(kErrorInvalidParameter);
  // Windows refuses ramps that dim the screen to nearly nothing: entry 127 must be at least 0x8000 above the entry-0 value... keep its rule (each step may not fall)
  for (uint32_t ch = 0; ch < 3; ++ch) for (uint32_t i = 1; i < 256; ++i) if (ramp[ch * 256 + i] < ramp[ch * 256 + i - 1]) return c.fail(kErrorInvalidParameter);
  for (uint32_t i = 0; i < 768; ++i) g_gamma[i] = ramp[i];
  g_gamma_ready = true;
  return 1;
}

// COLORADJUSTMENT: caSize 24, flags 0, illuminant 0 (default), gamma 10000 each, reference black 0, white 10000, the rest 0.
void default_color_adjust(uint8_t out[24]) {
  const uint16_t v[12] = {24, 0, 0, 10000, 10000, 10000, 0, 10000, 0, 0, 0, 0};
  for (int i = 0; i < 12; ++i) { out[2 * i] = uint8_t(v[i]); out[2 * i + 1] = uint8_t(v[i] >> 8); }
}

uint64_t NtGdiGetColorAdjustment(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  uint8_t ca[24];
  if (dc->has_color_adjust) { for (int i = 0; i < 24; ++i) ca[i] = dc->color_adjust[i]; }
  else default_color_adjust(ca);
  return ret_bool(c.write(c.arg(1), ConstByteView(ca, 24)));
}

uint64_t NtGdiSetColorAdjustment(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  uint8_t ca[24];
  if (!dc || !c.read(c.arg(1), ByteView(ca, 24))) return 0;
  if (ca[0] != 24 || ca[1] != 0) return c.fail(kErrorInvalidParameter);
  for (int i = 0; i < 24; ++i) dc->color_adjust[i] = ca[i];
  dc->has_color_adjust = true;
  return 1;
}

uint64_t NtGdiUpdateColors(Call& c) { return dc_arg(c, c.arg32(0)) ? 1 : 0; }

// ---- Pixel formats: 32-bit BGRA formats drawn by the kernel's OpenGL (src/OpenGl). 1..4 windows: single or double buffered (odd single), without or with a depth and stencil
// buffer (3 and up); 5 and 6 bitmaps (memory device contexts), single buffered, without and with depth. All have an accumulation buffer.
constexpr uint32_t kPixelFormats = 6;

void describe_format(uint32_t index, uint8_t out[40]) {
  for (int i = 0; i < 40; ++i) out[i] = 0;
  const bool bitmap = index >= 5;
  const bool dbl = !bitmap && (index & 1) == 0, depth = bitmap ? index == 6 : index > 2;
  auto p16 = [&](int o, uint32_t v) { out[o] = uint8_t(v); out[o + 1] = uint8_t(v >> 8); };
  auto p32 = [&](int o, uint32_t v) { for (int k = 0; k < 4; ++k) out[o + k] = uint8_t(v >> (8 * k)); };
  p16(0, 40); p16(2, 1);
  // PFD_DRAW_TO_WINDOW 0x4 / PFD_DRAW_TO_BITMAP 0x8, PFD_SUPPORT_GDI 0x10 (not with double buffering), PFD_SUPPORT_OPENGL 0x20, PFD_DOUBLEBUFFER 0x1
  p32(4, (bitmap ? 0x8u : 0x4u) | 0x20 | (dbl ? 0x1u : 0x10u));
  out[8] = 0;      // PFD_TYPE_RGBA
  out[9] = 32;      // cColorBits
  out[10] = 8; out[11] = 16; out[12] = 8; out[13] = 8; out[14] = 8; out[15] = 0;      // red, green, blue bits and shifts
  out[16] = 8; out[17] = 24;      // alpha
  out[18] = 64; out[19] = 16; out[20] = 16; out[21] = 16; out[22] = 16;      // accumulation
  out[23] = depth ? 24 : 0;      // cDepthBits
  out[24] = depth ? 8 : 0;       // cStencilBits
}

uint64_t NtGdiDescribePixelFormat(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const int32_t format = c.argi(1);
  const uint32_t size = c.arg32(2);
  if (c.arg(3) == 0) return kPixelFormats;
  if (format < 1 || uint32_t(format) > kPixelFormats || size < 40) return 0;
  uint8_t d[40];
  describe_format(uint32_t(format), d);
  return c.write(c.arg(3), ConstByteView(d, 40)) ? kPixelFormats : 0;
}

uint64_t NtGdiSetPixelFormat(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  const int32_t format = c.argi(1);
  if (!dc) return 0;
  if (format < 1 || uint32_t(format) > kPixelFormats) return c.fail(kErrorInvalidParameter);
  if (dc->pixel_format != 0) return dc->pixel_format == uint32_t(format) ? 1 : c.fail(kErrorInvalidParameter);      // (set once; the same one again is fine)
  dc->pixel_format = uint32_t(format);
  return 1;
}

uint64_t NtGdiExtEscape(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return ret_i32(-1);
  return 0;      // a display driver that implements no escapes: every escape (including QUERYESCSUPPORT) is "not supported"
}

uint64_t NtGdiCreateMetafileDC(Call& c) {
  if (c.arg32(0) && !dc_arg(c, c.arg32(0))) return 0;
  Dc* dc = new_dc(c.pid());
  if (!dc) return c.fail(kErrorNotEnoughMemory);
  dc->enhmeta = true;
  dc->info = true;
  return dc->handle;
}

uint64_t NtGdiAnyLinkedFonts(Call&) { return 0; }

// ---- Engine services a user-mode driver (UMPD) can call to make its own surfaces.
uint64_t NtGdiEngCreateBitmap(Call& c) {
  const uint64_t sizl = c.arg(0);
  const int32_t w = int32_t(uint32_t(sizl)), h = int32_t(uint32_t(sizl >> 32));
  static const uint32_t kBpp[7] = {0, 1, 4, 8, 16, 24, 32};
  const uint32_t fmt = c.arg32(2);
  if (fmt < 1 || fmt > 6) return c.fail(kErrorInvalidParameter);
  Bitmap* b = new_bitmap(c.pid(), w, h, kBpp[fmt]);
  if (!b) return c.fail(kErrorInvalidParameter);
  if (c.arg(4) != 0) {
    const uint32_t stride_in = c.arg32(1) ? c.arg32(1) : b->stride;
    for (int32_t y = 0; y < h; ++y) {
      const uint64_t n = stride_in < b->stride ? stride_in : b->stride;
      ByteView row = b->data.writable_view().subview(uint64_t(y) * b->stride, n);
      if (!c.read(c.arg(4) + uint64_t(y) * stride_in, row)) { destroy_object(b->handle); return c.fail(kErrorInvalidParameter); }
    }
  }
  return b->handle;
}

uint64_t NtGdiEngDeleteSurface(Call& c) {
  Bitmap* b = find_bitmap(c.arg32(0));
  if (!b || b->pid != c.pid() || b->selected > 0) return 0;
  return ret_bool(destroy_object(b->handle));
}

uint64_t NtGdiEngCreatePalette(Call& c) {
  const uint32_t mode = c.arg32(0), count = c.arg32(1);
  if (mode != 1 || count == 0 || count > 65536) return c.fail(kErrorInvalidParameter);      // PAL_INDEXED only
  Palette* p = make_object<Palette>(kObjPalette, c.pid());
  if (!p) return c.fail(kErrorNotEnoughMemory);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t e = 0;
    if (!c.read_u32(c.arg(2) + uint64_t{i} * 4, &e)) { destroy_object(p->handle); return c.fail(kErrorInvalidParameter); }
    p->entries.push_back(e & 0xFFFFFF);
  }
  return p->handle;
}

uint64_t NtGdiEngDeletePalette(Call& c) {
  Palette* p = find_palette(c.arg32(0));
  if (!p || p->pid != c.pid() || p->stock) return 0;
  return ret_bool(destroy_object(p->handle));
}

}  // namespace

void register_misc_services() {
  GDI_REGISTER(Flush, NtGdiFlush);
  GDI_REGISTER(Init, NtGdiInit);
  GDI_REGISTER(GetDeviceGammaRamp, NtGdiGetDeviceGammaRamp);
  GDI_REGISTER(GetAppliedDeviceGammaRamp, NtGdiGetDeviceGammaRamp);
  GDI_REGISTER(SetDeviceGammaRamp, NtGdiSetDeviceGammaRamp);
  GDI_REGISTER(GetColorAdjustment, NtGdiGetColorAdjustment);
  GDI_REGISTER(SetColorAdjustment, NtGdiSetColorAdjustment);
  GDI_REGISTER(UpdateColors, NtGdiUpdateColors);
  GDI_REGISTER(DescribePixelFormat, NtGdiDescribePixelFormat);
  GDI_REGISTER(SetPixelFormat, NtGdiSetPixelFormat);
  GDI_REGISTER(ExtEscape, NtGdiExtEscape);
  GDI_REGISTER(CreateMetafileDC, NtGdiCreateMetafileDC);
  GDI_REGISTER(AnyLinkedFonts, NtGdiAnyLinkedFonts);
  GDI_REGISTER(EngCreateBitmap, NtGdiEngCreateBitmap);
  GDI_REGISTER(EngDeleteSurface, NtGdiEngDeleteSurface);
  GDI_REGISTER(EngCreatePalette, NtGdiEngCreatePalette);
  GDI_REGISTER(EngDeletePalette, NtGdiEngDeletePalette);
}

}  // namespace process::win32k::gdi
