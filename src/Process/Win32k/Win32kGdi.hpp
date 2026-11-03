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
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

// What the other win32k modules (and the kernel) use of the GDI module (Win32kGdi*.cpp). The GDI module implements everything declared here; it may add more below the
// marked line. Everything is guarded by the system lock (the callers are handlers that hold it), except where noted.
namespace process::win32k::gdi {

struct Rect { int32_t left = 0, top = 0, right = 0, bottom = 0; };      // RECT: right/bottom exclusive

// --- The screen: one surface of 32-bit pixels (B, G, R, X bytes; rows top to bottom, pitch = width * 4), made on first use at 1024 x 768.
uint32_t screen_width();
uint32_t screen_height();
bool set_screen_size(uint32_t width, uint32_t height);      // contents are cleared to the desktop colour; false for a size over 16384 or under 16
ConstByteView screen_pixels();                              // the whole surface (valid until the screen is resized)
// The kernel's desktop shows the surface: it is told what part changed (screen coordinates). Called with the system lock held, so it must only record the rectangle.
class IScreenSink {
 public:
  virtual void screen_changed(const Rect& dirty) = 0;

 protected:
  IScreenSink() = default;
  ~IScreenSink() = default;
};
void set_screen_sink(IScreenSink* sink);

// --- Device contexts of windows and of the screen (what GetDC/BeginPaint make). The DC draws on the screen surface; its origin is the window's (client or window) top-left in
// screen coordinates, and `clip` (screen coordinates) is where drawing is allowed - the visible part of the window, minus its children and what covers it. An empty `clip`
// array means "nowhere" (everything is clipped). Returns the HDC (0 on failure). The DC belongs to process `pid` (gone when it ends).
uint32_t create_screen_dc(uint64_t pid, int32_t origin_x, int32_t origin_y, ArrayView<const Rect> clip);
bool update_screen_dc(uint32_t dc, int32_t origin_x, int32_t origin_y, ArrayView<const Rect> clip);
bool is_dc(uint32_t handle);
bool delete_dc(uint32_t dc);      // ReleaseDC / EndPaint: the DC goes (any objects selected into it stay)

// --- Regions as lists of rectangles (HRGN <-> window update regions, window shapes).
uint32_t create_region(uint64_t pid, ArrayView<const Rect> rects);
bool region_rects(uint32_t region, List<Rect>* out);      // appends the rectangles of the region (a union of non-overlapping rectangles)
bool replace_region(uint32_t region, ArrayView<const Rect> rects);
bool is_region(uint32_t handle);

// --- Bitmaps (icons and cursors are made from them).
bool bitmap_info(uint32_t bitmap, int32_t* width, int32_t* height, uint32_t* bits_per_pixel);
// The pixels of a bitmap as 32-bit 0xAARRGGBB values, top row first (appended to `out`); 1/4/8/16/24/32-bit bitmaps are converted (alpha 0xFF unless the bitmap has alpha).
bool bitmap_pixels(uint32_t bitmap, List<uint32_t>* out);
uint32_t create_bitmap(uint64_t pid, int32_t width, int32_t height, ArrayView<const uint32_t> argb);      // a 32-bit bitmap from 0xAARRGGBB pixels (top row first)

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// (the GDI module's own declarations follow)

}  // namespace process::win32k::gdi
