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
#include "Process/Process.hpp"
#include "Utility/ArrayView.hpp"

namespace process::win32k { class Call; }

// What the other win32k modules (and the kernel) use of the GDI module (Win32kGdi*.cpp). The GDI module implements everything declared here; it may add more below the
// marked line. Everything is guarded by the system lock (the callers are handlers that hold it), except where noted.
namespace process::win32k::gdi {

struct Rect { int32_t left = 0, top = 0, right = 0, bottom = 0; };      // RECT: right/bottom exclusive

// --- The screen: one surface of 32-bit pixels (B, G, R, X bytes; rows top to bottom, pitch = width * 4), made on first use at 1024 x 768.
uint32_t screen_width();
uint32_t screen_height();
bool set_screen_size(uint32_t width, uint32_t height);      // contents are cleared to the desktop colour; false for a size over 16384 or under 16 (before the state exists: its size)
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
// `surface`: a window surface (below) the DC draws on instead of the screen; origin and clip are then in the surface's coordinates.
uint32_t create_screen_dc(uint64_t pid, int32_t origin_x, int32_t origin_y, ArrayView<const Rect> clip, uint32_t surface = 0);
// A screen DC for a DPI unaware program (scale > 100 percent): it draws on a shadow of the screen in the program's units (sized device_to_units of the screen,
// filled with what the screen shows when it is made), and what it draws goes on to the screen scaled up as it is drawn. The shadow goes with the DC. 0: no memory.
uint32_t create_screen_shadow_dc(uint64_t pid, uint32_t scale);
bool update_screen_dc(uint32_t dc, int32_t origin_x, int32_t origin_y, ArrayView<const Rect> clip, uint32_t surface = 0);

// --- Window surfaces: the backing store of a top-level window that the kernel's desktop composites (the window and its children draw there through their DCs; the desktop
// copies it into its picture as it is, so a window that moves needs no drawing). Same pixel format as the screen. Called with the system lock held.
uint32_t create_window_surface(uint32_t window, uint32_t width, uint32_t height);      // 0: no memory or a bad size
bool resize_window_surface(uint32_t surface, uint32_t width, uint32_t height);           // the contents are cleared (the window repaints)
void destroy_window_surface(uint32_t surface);
bool window_surface_size(uint32_t surface, uint32_t* width, uint32_t* height);
ArrayView<uint32_t> window_surface_pixels(uint32_t surface, uint32_t* width, uint32_t* height);      // rows top to bottom, pitch = width; valid until resized or destroyed
// Told what part of which window's surface changed. Called with the system lock held: it must only record it.
// Something other than GDI wrote the surface's pixels (a GPU present): the sink is told, as after a drawing.
void window_surface_damaged(uint32_t surface, const Rect& dirty);
// The surface's bytes (B, G, R, X; rows width * 4 bytes apart), for writing them.
ByteView window_surface_bytes(uint32_t surface, uint32_t* width, uint32_t* height);
class IWindowSurfaceSink {
 public:
  virtual void surface_changed(uint32_t window, const Rect& dirty) = 0;

 protected:
  IWindowSurfaceSink() = default;
  ~IWindowSurfaceSink() = default;
};
void set_window_surface_sink(IWindowSurfaceSink* sink);
// DPI virtualization (the window manager's, Win32kUserDpi.cpp): the factor the calling thread's coordinates are divided by and the dpi it works at.
// Without a hook GDI answers 100 (percent: no scaling) and 96.
using CallerDpiHook = void (*)(Call& call, uint32_t* scale, uint32_t* dpi);
void set_caller_dpi_hook(CallerDpiHook hook);
// Where a DC's surface lies on the screen, in the calling thread's coordinates (GetRandomRgn's SYSRGN is in screen coordinates): the window manager
// adds (dx, dy) to the DC's surface coordinates. False (the default): the DC's coordinates are the screen's.
using DcScreenOffsetHook = bool (*)(Call& call, uint32_t dc, int32_t* dx, int32_t* dy);
void set_dc_screen_offset_hook(DcScreenOffsetHook hook);
bool is_dc(uint32_t handle);
bool is_screen_dc(uint32_t handle);      // a DC of the screen or of a window (not a memory DC, not a printer's)
uint64_t dc_owner(uint32_t handle);      // the process a DC belongs to (0: not a DC)
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


// --- Fonts. Text is drawn with the built-in 8x16 bitmap font scaled to the requested size; a provider makes real fonts (TrueType from src/Font) available: the kernel registers one
// and GDI asks it first for every font the program selects.
struct GdiFontRequest {
  ArrayView<const char16_t> face;      // lfFaceName
  int32_t height = 0;                  // the cell height in device pixels
  int32_t width = 0;                   // 0: the face's natural width
  int32_t weight = 400;
  bool italic = false;
  uint8_t charset = 1, pitch_family = 0;
};
struct GdiFontMetrics {
  int32_t ascent = 0, descent = 0, internal_leading = 0, external_leading = 0, ave_width = 0, max_width = 0;      // device pixels
  bool fixed_pitch = false;
};
struct GdiGlyph {
  int32_t advance = 0;                 // pixels to the next glyph
  int32_t left = 0, top = 0;           // the bitmap's top-left corner relative to the pen position on the baseline (top: pixels above the baseline)
  int32_t width = 0, height = 0;
  ConstByteView coverage;              // width * height bytes of 8-bit coverage, valid until the next call
};
class IGdiFontProvider {
 public:
  virtual bool match(const GdiFontRequest& request, uint32_t* face) = 0;
  virtual bool metrics(uint32_t face, int32_t pixel_height, GdiFontMetrics* out) = 0;
  virtual bool glyph(uint32_t face, int32_t pixel_height, char32_t character, GdiGlyph* out) = 0;
  // GetFontData: copies the bytes of an sfnt table (table 0: the whole file) into `out`; returns how many, or -1 if there is no such table.
  virtual int64_t font_data(uint32_t /*face*/, uint32_t /*table*/, uint32_t /*offset*/, ByteView /*out*/) { return -1; }

 protected:
  IGdiFontProvider() = default;
  ~IGdiFontProvider() = default;
};
void set_font_provider(IGdiFontProvider* provider);

// --- DIB sections live in the memory of the process that made them. The default places them in the process's own address space (Process::address_space()); a host with no address
// space (the unit tests) installs these hooks, which hand out an address the process can read and write through Process::read_user / write_user.
struct DibMemory {
  bool (*allocate)(Process& process, uint64_t bytes, uint64_t* address) = nullptr;
  void (*release)(Process& process, uint64_t address, uint64_t bytes) = nullptr;
};
void set_dib_memory(const DibMemory* hooks);      // null: the default; the table must stay alive

// GetStockObject(index): the HGDIOBJ (0 for an index outside 0..19).
uint32_t stock_object(int index);
// Drops every GDI object and the screen (the unit tests call this between systems; the kernel never needs to).
void shutdown();

}  // namespace process::win32k::gdi
