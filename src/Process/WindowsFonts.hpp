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

#pragma once
#include <stdint.h>
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace process::windowsabi {

// What a Windows program's DirectWrite (dwrite.dll) needs from the system's fonts. The kernel's font system (src/Font) implements it (src/Font/SystemFontService.hpp); the
// service below only reads the program's request, checks it and calls this, so it is tested without the fonts (tests/WindowsFontTest.cpp). Faces are numbered 0 .. face_count()-1
// for the life of the system: the fonts the system has, then the ones programs added with add_font(). Glyph numbers and design measurements are the font's own units (a TrueType
// glyph index, units of the em square). The caller holds the system lock, so an implementation needs no lock of its own.
struct FontFaceInfo {
  uint32_t units_per_em = 0;
  int32_t ascent = 0, descent = 0, line_gap = 0, x_height = 0, cap_height = 0;      // font units; descent is positive
  uint32_t glyph_count = 0;
  uint16_t weight = 400, stretch = 100;      // OS/2 weight class, width as a percentage
  bool italic = false, bold = false, bundled = false, monospaced = false;
};
struct FontGlyphBox {
  int32_t advance = 0;      // font units
  int32_t x_min = 0, y_min = 0, x_max = 0, y_max = 0;      // the bounding box of the outline, y up (all 0 for a glyph without outline)
};
struct FontBitmap {
  int32_t left = 0, top = 0;      // the pen sits at pixel (x, y): column 0 of row 0 is (x + left, y - top)
  uint32_t width = 0, height = 0;
  ByteBuffer pixels;      // width * height bytes of coverage, rows top to bottom
  explicit FontBitmap(allocators::IAllocator& allocator) : pixels(allocator) {}
};

class IFontService {
 public:
  virtual allocators::IAllocator& allocator() = 0;      // for the temporary buffers of a request
  virtual uint32_t face_count() = 0;
  virtual bool face_info(uint32_t face, FontFaceInfo* out) = 0;
  // which: 0 family name, 1 style name, 2 full name, 3 PostScript name (UTF-8; empty if the font has none).
  virtual bool face_string(uint32_t face, uint32_t which, String* out) = 0;
  virtual bool map_codepoint(uint32_t face, uint32_t codepoint, uint32_t* glyph) = 0;      // 0 = .notdef
  virtual bool glyph_box(uint32_t face, uint32_t glyph, FontGlyphBox* out) = 0;
  virtual bool kerning(uint32_t face, uint32_t left, uint32_t right, int32_t* units) = 0;
  // size in 26.6 pixels, subpixel 0..3 quarters of a pixel to the right.
  virtual bool rasterize(uint32_t face, uint32_t glyph, int32_t size, uint32_t subpixel, FontBitmap* out) = 0;
  // The outline: contour_ends (index of the last point of each contour) and points (x, y, on-curve), quadratic TrueType contours.
  virtual bool outline(uint32_t face, uint32_t glyph, ByteBuffer* contour_ends, ByteBuffer* points) = 0;
  // The sfnt file of the face (bundled fonts are decoded): its size, and `out.size()` bytes from `offset` (as many as there are).
  virtual bool font_data(uint32_t face, uint64_t offset, ByteView out, uint64_t* total, uint64_t* copied) = 0;
  // Adds a font the program has (TrueType, a collection's first face, WOFF); the new face's number.
  virtual bool add_font(ConstByteView data, uint32_t* face) = 0;

 protected:
  IFontService() = default;
  ~IFontService() = default;
};

// The system's fonts (null: none, the service answers STATUS_NOT_SUPPORTED).
void set_font_service(IFontService* service);
IFontService* font_service();

// NtTcosFont(PTCOS_FONT_REQUEST request): this OS's own service (507), behind dwrite.dll. Windows keeps its fonts in the font cache service and files; here the system's fonts are
// the kernel's, and dwrite.dll (src/Windows/dwrite) does the rest (families, text layout, shaping by cmap, the COM interfaces) over this service. One structure in the program's
// memory (64 bytes, little endian):
//
//    0 ULONG size (64)   4 ULONG operation   8 ULONG face   12 ULONG x   16 ULONG y   20 ULONG count   24 PVOID in   32 PVOID out   40 ULONG capacity (bytes of out)
//   44 ULONG result0   48 ULONG result1   52 ULONG result2   56 ULONG result3   60 reserved
//
//   1 FACE_COUNT       result0 = the number of faces.
//   2 FACE_INFO        face; out: 64 bytes {u32 units_per_em, i32 ascent, descent, line_gap, x_height, cap_height, u32 glyph_count, u16 weight, u16 stretch, u32 flags
//                      (1 italic, 2 bold, 4 bundled, 8 monospaced), 24 reserved bytes}.
//   3 FACE_STRING      face, x = 0 family 1 style 2 full name 3 PostScript name; out: UTF-16, NUL ended, capacity bytes. result0 = its length in WCHARs without the NUL;
//                      STATUS_BUFFER_TOO_SMALL (length still set) when it does not fit.
//   4 MAP_CODEPOINTS   face, count (<= 65536), in = ULONG[count] code points; out = USHORT[count] glyphs (0 = .notdef; a glyph number past 65535 is 0).
//   5 GLYPH_BOXES      face, count (<= 65536), in = USHORT[count] glyphs; out = {i32 advance, x_min, y_min, x_max, y_max}[count]; a glyph the font has not is all zero.
//   6 KERNING          face, x = left glyph, y = right glyph; result0 = the kerning in font units (an LONG).
//   7 RASTERIZE        face, x = glyph, y = subpixel (0..3), count = the size in 26.6 pixels; out = the coverage bytes (rows top to bottom, width bytes each), capacity bytes.
//                      result0 = left (LONG), result1 = top (LONG), result2 = width, result3 = height. With capacity 0 only the four results (the size query).
//                      STATUS_BUFFER_TOO_SMALL when width * height > capacity.
//   8 OUTLINE          face, x = glyph; out: {u32 contours, u32 points, u32 contour_end[contours], {i32 x, i32 y, u32 on_curve}[points]}. result0 = the bytes it takes.
//   9 FONT_DATA        face, x = offset into the sfnt file; out: its bytes (at most 1 MiB a call). result0 = the size of the whole file, result1 = the bytes copied.
//  10 ADD_FONT         in = the font file, count = its size (<= 8 MiB); result0 = the new face's number.
// STATUS_INVALID_PARAMETER for a request that is wrong (an operation or face that does not exist, a count that is too big).
uint32_t nt_tcos_font(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
