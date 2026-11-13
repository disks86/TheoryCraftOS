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
#include "Process/Win32k/Win32k.hpp"
#include "Process/Win32k/Win32kGdi.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

// The internals the files of the GDI module share (Win32kGdi*.cpp). See docs/win32k/Gdi.md for the design.
namespace process::win32k::gdi {

using ColorRef = uint32_t;      // COLORREF: 0x00BBGGRR (the top byte is the palette flag / alpha)
constexpr ColorRef make_rgb(uint32_t r, uint32_t g, uint32_t b) { return (r & 0xFF) | (g & 0xFF) << 8 | (b & 0xFF) << 16; }
constexpr uint32_t cr_r(ColorRef c) { return c & 0xFF; }
constexpr uint32_t cr_g(ColorRef c) { return (c >> 8) & 0xFF; }
constexpr uint32_t cr_b(ColorRef c) { return (c >> 16) & 0xFF; }

struct Point { int32_t x = 0, y = 0; };
struct Size { int32_t cx = 0, cy = 0; };

inline bool rect_empty(const Rect& r) { return r.right <= r.left || r.bottom <= r.top; }
inline Rect rect_and(const Rect& a, const Rect& b) {
  Rect r{a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top, a.right < b.right ? a.right : b.right, a.bottom < b.bottom ? a.bottom : b.bottom};
  if (rect_empty(r)) return Rect{};
  return r;
}
inline Rect rect_union(const Rect& a, const Rect& b) {
  if (rect_empty(a)) return b;
  if (rect_empty(b)) return a;
  return Rect{a.left < b.left ? a.left : b.left, a.top < b.top ? a.top : b.top, a.right > b.right ? a.right : b.right, a.bottom > b.bottom ? a.bottom : b.bottom};
}
inline bool rect_has(const Rect& r, int32_t x, int32_t y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
inline Rect rect_normalized(const Rect& r) {
  return Rect{r.left < r.right ? r.left : r.right, r.top < r.bottom ? r.top : r.bottom, r.left < r.right ? r.right : r.left, r.top < r.bottom ? r.bottom : r.top};
}
inline int32_t clamp_i32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : int32_t(v); }
inline int32_t iabs(int32_t v) { return v < 0 ? (v == INT32_MIN ? INT32_MAX : -v) : v; }
// Saturating arithmetic on coordinates that come from user mode.
inline int32_t sat_add(int32_t a, int32_t b) { return clamp_i32(int64_t{a} + b); }
inline int32_t sat_sub(int32_t a, int32_t b) { return clamp_i32(int64_t{a} - b); }

// ---- Small floating point helpers (the module links nothing but the compiler: no libm).
double g_floor(double v);
double g_sqrt(double v);
double g_sin(double v);
double g_cos(double v);
double g_atan2(double y, double x);
inline int32_t round_i32(double v) { return clamp_i32(int64_t(g_floor(v + 0.5))); }

// ---- Object kinds (the type in bits 16..22 of a handle) and the rest of the handle layout.
constexpr uint8_t kObjDc = 0x01, kObjRegion = 0x04, kObjBitmap = 0x05, kObjPalette = 0x08, kObjColorSpace = 0x09, kObjFont = 0x0A, kObjBrush = 0x10, kObjClient = 0x20,
                  kObjPen = 0x30, kObjExtPen = 0x50;
constexpr uint32_t kStockFlag = 0x00800000u;
inline uint8_t handle_type(uint32_t h) { return uint8_t((h >> 16) & 0x7F); }

struct Object {
  uint8_t type = 0;
  bool stock = false;
  uint32_t handle = 0;
  uint64_t pid = 0;
  uint32_t selected = 0;      // how many device contexts have it selected
};

// Stock object indexes (GetStockObject).
constexpr int kStockCount = 21;      // WHITE_BRUSH 0 .. DC_PEN 19, DEFAULT_GUI_FONT 17 (20 is unused; 21 = the 1x1 monochrome bitmap, internal)

struct ClientObject : Object { explicit ClientObject(allocators::IAllocator&) {} };

struct Xform { double m11 = 1, m12 = 0, m21 = 0, m22 = 1, dx = 0, dy = 0; };

struct Region : Object {
  explicit Region(allocators::IAllocator& a) : rects(a) {}
  List<Rect> rects;      // banded, non-overlapping, canonical
};

struct Palette : Object {
  explicit Palette(allocators::IAllocator& a) : entries(a) {}
  List<uint32_t> entries;      // PALETTEENTRY: R | G << 8 | B << 16 | flags << 24
};

struct Font : Object {
  explicit Font(allocators::IAllocator&) {}
  uint32_t height = 0, width = 0, escapement = 0, orientation = 0, weight = 400;
  int32_t lf_height = 0, lf_width = 0, lf_escapement = 0, lf_orientation = 0;
  uint8_t italic = 0, underline = 0, strikeout = 0, charset = 1, out_precision = 0, clip_precision = 0, quality = 0, pitch_family = 0;
  char16_t face[32] = {};
  char16_t full_name[64] = {}, style[32] = {}, script[32] = {};
};

struct Brush : Object {
  explicit Brush(allocators::IAllocator& a) : pattern(a) {}
  uint32_t style = 0;      // BS_SOLID 0, BS_NULL 1, BS_HATCHED 2, BS_PATTERN 3, BS_DIBPATTERN 5, BS_DIBPATTERNPT 6, BS_PATTERN8X8 7, BS_DIBPATTERN8X8 8, BS_MONOPATTERN 9
  ColorRef color = 0;
  uint32_t hatch = 0;
  bool dc_brush = false;       // the stock DC_BRUSH: takes the colour of the DC
  bool mono = false;           // a 1-bit pattern: shown in the text and background colour of the DC
  int32_t pw = 0, ph = 0;
  List<uint32_t> pattern;      // pw * ph COLORREFs (0/1 when mono)
  uint32_t usage = 0;          // DIB_RGB_COLORS / DIB_PAL_COLORS of a DIB pattern
  uint32_t attributes = 0;
};

struct Pen : Object {
  explicit Pen(allocators::IAllocator& a) : dashes(a) {}
  uint32_t style = 0;          // PS_* with the type bits
  uint32_t width = 1;
  ColorRef color = 0;
  uint32_t brush_style = 0, hatch = 0;
  bool ext = false, dc_pen = false;
  List<uint32_t> dashes;       // user style dash lengths (PS_USERSTYLE)
  uint32_t brush_handle_bitmap = 0;
};

struct Bitmap : Object {
  explicit Bitmap(allocators::IAllocator& a) : data(a), colors(a) {}
  int32_t width = 0, height = 0;
  uint32_t planes = 1, bpp = 32, stride = 0;
  bool dib = false, bottom_up = false;
  uint32_t compression = 0;
  uint32_t masks[3] = {0, 0, 0};
  ByteBuffer data;             // the pixels (a DIB section's is a mirror of the process's memory: synced in/out around every drawing call)
  List<uint32_t> colors;       // COLORREFs of the colour table (bpp <= 8)
  Process* proc = nullptr;     // DIB section: whose memory holds the bits
  uint64_t user_address = 0;
  bool user_mapped = false;    // the region was mapped by this module (and is unmapped with the bitmap)
  uint64_t section_view = 0;   // a DIB section on a section of the program's (CreateDIBSection's hSection): the view of it the bits are in (unmapped with the bitmap)
  uint32_t section_offset = 0;
  uint64_t section_handle = 0; // the program's handle to that section (DIBSECTION.dshSection)
  int32_t dim_cx = 0, dim_cy = 0;
  uint32_t attributes = 0;
  bool is_screen = false;      // the screen surface (not in the handle table)
  uint32_t window_surface = 0; // the surface of this top-level window (not in the handle table; changes go to the window surface sink)
  uint32_t dib_usage = 0;
  uint32_t clr_important = 0;
  uint32_t x_ppm = 0, y_ppm = 0;
};

// A view of pixels in some format, as the drawing code sees every surface. Coordinates outside it are ignored by put and read as 0 by get.
struct Surface {
  ByteView bytes;
  int32_t width = 0, height = 0;
  uint32_t bpp = 32, stride = 0;
  bool bottom_up = false;
  ArrayView<const uint32_t> colors;
  uint32_t masks[3] = {0, 0, 0};      // 16 / 32 bpp bit fields (0: the default layout)
  bool valid() const;
  ColorRef get(int32_t x, int32_t y) const;
  uint32_t get_raw32(int32_t x, int32_t y) const;      // the 4 bytes of a 32 bpp pixel (alpha included)
  void put(int32_t x, int32_t y, ColorRef c);
  void put_raw32(int32_t x, int32_t y, uint32_t v);
  uint32_t nearest_index(ColorRef c) const;
  Rect bounds() const { return Rect{0, 0, width, height}; }
};
Surface surface_of(Bitmap& b);
Bitmap* new_bitmap(uint64_t pid, int32_t w, int32_t h, uint32_t bpp);      // a zeroed device-dependent bitmap (default colours for bpp <= 8)
void fill_default_colors(uint32_t bpp, List<uint32_t>* out);

// ---- Paths.
constexpr uint8_t kPtCloseFigure = 1, kPtLineTo = 2, kPtBezierTo = 4, kPtMoveTo = 6;
struct Path {
  explicit Path(allocators::IAllocator& a) : points(a), types(a) {}
  List<Point> points;      // device coordinates of the DC
  List<uint8_t> types;
  bool recording = false, valid = false;      // recording: between BeginPath and EndPath; valid: a finished path exists
  bool figure_open = false, have_start = false;
  Point start;
};

// ---- A device context.
struct DcPod {
  uint32_t pen = 0, brush = 0, font = 0, palette = 0;
  ColorRef bk_color = 0xFFFFFF, text_color = 0, dc_brush_color = 0xFFFFFF, dc_pen_color = 0;
  int32_t bk_mode = 2, rop2 = 13, poly_fill = 1, stretch = 1, text_align = 0, arc_dir = 1, graphics_mode = 1, map_mode = 1, layout = 0, rel_abs = 1;
  int32_t char_extra = 0, just_extra = 0, just_breaks = 0;
  Point wnd_org, vport_org;
  Size wnd_ext{1, 1}, vport_ext{1, 1};
  Xform world;
  float miter_limit = 10.0f;
  Point brush_org;
  uint32_t icm_mode = 1;
  uint32_t mapper_flags = 0;
  bool has_clip = false, has_meta = false;
};

struct DcState : DcPod {
  explicit DcState(allocators::IAllocator& a) : clip(a), meta(a) {}
  List<Rect> clip;      // device coordinates of the DC (relative to its origin)
  List<Rect> meta;
  bool copy_from(const DcState& o);
};

struct Dc : Object {
  explicit Dc(allocators::IAllocator& a) : s(a), saved(a), surface_clip(a), path(a) {}
  DcState s;
  List<UniquePointer<DcState>> saved;
  uint32_t bitmap = 0;                 // memory DC: the selected bitmap
  bool screen = false, memory = false, info = false, printer = false, enhmeta = false;
  int32_t origin_x = 0, origin_y = 0;  // screen DC: where device (0,0) lies on the screen
  List<Rect> surface_clip;             // screen DC: where drawing is allowed (screen coordinates)
  uint32_t surface = 0;                // screen DC of a window with a window surface: that surface (origin and clip are then the surface's coordinates)
  Point cur_pos;
  Path path;
  Rect bounds;
  uint32_t bounds_flags = 0;
  uint32_t pixel_format = 0;
  uint32_t doc_active = 0;
  uint32_t system_palette_use = 1;
  uint32_t device_kind = 0;            // 0 display, 1 printer (a printer DC has no surface)
  uint32_t color_space = 0;
  int32_t virt_res_x = 0, virt_res_y = 0, virt_size_x = 0, virt_size_y = 0;
  uint64_t abort_proc = 0;
  uint8_t color_adjust[24] = {};
  bool has_color_adjust = false;
};

// ---- The module state and the handle table (Win32kGdi.cpp).
allocators::IAllocator* alloc();      // null if the system has no object manager
Object* find_object(uint32_t handle);
Object* find_object(uint32_t handle, uint8_t type);
inline Dc* find_dc(uint32_t h) { return static_cast<Dc*>(find_object(h, kObjDc)); }
inline Region* find_region(uint32_t h) { return static_cast<Region*>(find_object(h, kObjRegion)); }
inline Bitmap* find_bitmap(uint32_t h) { return static_cast<Bitmap*>(find_object(h, kObjBitmap)); }
inline Palette* find_palette(uint32_t h) { return static_cast<Palette*>(find_object(h, kObjPalette)); }
inline Font* find_font(uint32_t h) { return static_cast<Font*>(find_object(h, kObjFont)); }
inline Brush* find_brush(uint32_t h) { return static_cast<Brush*>(find_object(h, kObjBrush)); }
inline Pen* find_pen(uint32_t h) { Object* o = find_object(h); return o && (o->type == kObjPen || o->type == kObjExtPen) ? static_cast<Pen*>(o) : nullptr; }
Dc* find_dc_with_bitmap(uint32_t bitmap);      // the DC that has the bitmap selected
Object* register_object(UniquePointer<Object>&& obj, uint8_t type, uint64_t pid, bool stock = false);
template <class T>
T* make_object(uint8_t type, uint64_t pid, bool stock = false) {
  allocators::IAllocator* a = alloc();
  if (!a) return nullptr;
  UniquePointer<Object> p = UniquePointer<Object>::template make_as<T>(*a, *a);
  if (!p) return nullptr;
  return static_cast<T*>(register_object(util::move(p), type, pid, stock));
}
bool destroy_object(uint32_t handle);      // removes it from the table (no checks)
uint32_t stock_handle(int index);          // GetStockObject(index), 0 if out of range
uint32_t default_bitmap_handle();          // the 1x1 monochrome bitmap every memory DC starts with
void sink_changed(const Rect& dirty);      // tells the screen sink
Bitmap* window_surface_bitmap(uint32_t surface);
void window_surface_changed(const Bitmap& bitmap, const Rect& dirty);      // tells the window surface sink
Bitmap& screen_bitmap();
void dib_sync_in(Bitmap& b);               // a DIB section: reads the process's memory into the mirror
void dib_sync_out(Bitmap& b);              // ... and writes the mirror back
void release_dib_memory(Bitmap& b);
bool allocate_dib_memory(Process& process, Bitmap& b, uint64_t bytes);
// A view of the program's section `section` (a handle of `process`) for the bits of a DIB section, from byte `offset`: *address is where they begin. Lets go of the
// system lock while it maps (so call it before anything of the handler's is half made). use_dib_section_view gives the view to the bitmap, which unmaps it.
bool map_section_for_dib(Process& process, uint64_t section, uint64_t offset, uint64_t bytes, uint64_t* view, uint64_t* address);
void use_dib_section_view(Process& process, Bitmap& b, uint64_t view, uint64_t address);

// ---- Regions (Win32kGdiRegion.cpp). All results are canonical (banded, coalesced).
bool rgn_normalize(ArrayView<const Rect> in, List<Rect>* out);
bool rgn_combine(ArrayView<const Rect> a, ArrayView<const Rect> b, int32_t mode, List<Rect>* out);      // RGN_AND 1, OR 2, XOR 3, DIFF 4
Rect rgn_bound(ArrayView<const Rect> r);
bool rgn_has_point(ArrayView<const Rect> r, int32_t x, int32_t y);
bool rgn_hits_rect(ArrayView<const Rect> r, const Rect& q);
bool rgn_contains_rect(ArrayView<const Rect> r, const Rect& q);
void rgn_offset(List<Rect>& r, int32_t dx, int32_t dy);
bool rgn_assign(List<Rect>& dst, ArrayView<const Rect> src);
bool rgn_from_rect(List<Rect>* out, const Rect& r);
bool rgn_ellipse(List<Rect>* out, const Rect& r);
bool rgn_round_rect(List<Rect>* out, const Rect& r, int32_t ew, int32_t eh);
bool rgn_polygon(List<Rect>* out, ArrayView<const Point> pts, ArrayView<const uint32_t> counts, bool winding, const Rect& limit);
bool rgn_equal(ArrayView<const Rect> a, ArrayView<const Rect> b);
Region* new_region(uint64_t pid, ArrayView<const Rect> rects);
int32_t region_complexity(ArrayView<const Rect> r);      // NULLREGION 1, SIMPLEREGION 2, COMPLEXREGION 3

// ---- Device contexts and coordinate transforms (Win32kGdiDc.cpp).
Dc* new_dc(uint64_t pid);
void dc_release_selection(Dc& dc);
void dc_apply_default_state(Dc& dc);
bool dc_select_object(Dc& dc, Object* obj, uint32_t* previous);
struct DPoint { double x = 0, y = 0; };
Point lp_to_dp(const Dc& dc, Point p);
Point dp_to_lp(const Dc& dc, Point p);
DPoint lp_to_dp_f(const Dc& dc, double x, double y);
DPoint dp_to_lp_f(const Dc& dc, double x, double y);
Rect lp_to_dp_rect(const Dc& dc, const Rect& r);      // normalized
bool xform_invert(const Xform& in, Xform* out);
bool dc_is_axis_aligned(const Dc& dc);
bool dc_world_active(const Dc& dc);
Surface dc_surface(Dc& dc, Bitmap** bitmap);      // the surface a DC draws on (invalid if none)
bool dc_effective_clip(Dc& dc, const Surface& s, List<Rect>* out);      // surface coordinates

// ---- The pixel engine (Win32kGdiDraw.cpp).
// A drawing session on a DC: resolves the surface and the clip once, tracks what changed, and pushes it (screen sink, DIB section memory) when it ends.
class Painter {
 public:
  Painter(Dc& dc);
  ~Painter();
  Painter(const Painter&) = delete;
  Painter& operator=(const Painter&) = delete;
  bool ok() const { return ok_; }
  Dc& dc() { return dc_; }
  Surface& surface() { return surface_; }
  // Device coordinates of the DC.
  void plot(int32_t x, int32_t y, ColorRef c, int32_t rop2);
  void plot_raw(int32_t x, int32_t y, ColorRef c);      // R2_COPYPEN
  ColorRef read(int32_t x, int32_t y);                  // outside the surface or the clip reads as CLR_INVALID
  bool visible(int32_t x, int32_t y) const;
  const List<Rect>& clip() const { return clip_; }
  const Rect& clip_bound() const { return clip_bound_; }
  void mark(const Rect& device_rect);
  int32_t ox() const { return ox_; }
  int32_t oy() const { return oy_; }
  Bitmap* bitmap() { return bitmap_; }
  // Work limit of one call: line drawing spends one unit per pixel and stops when the budget is gone (a hostile polyline cannot hold the system lock for minutes).
  bool spend(uint64_t pixels) { if (pixels > budget_) { budget_ = 0; return false; } budget_ -= pixels; return true; }

 private:
  Dc& dc_;
  Bitmap* bitmap_ = nullptr;
  Surface surface_;
  List<Rect> clip_;
  Rect clip_bound_;
  Rect dirty_;
  int32_t ox_ = 0, oy_ = 0;
  bool ok_ = false;
  uint64_t budget_ = uint64_t{1} << 27;
};
constexpr ColorRef kClrInvalid = 0xFFFFFFFFu;
ColorRef rop2_apply(int32_t rop2, ColorRef dst, ColorRef src);
uint32_t rop3_apply(uint32_t rop, ColorRef pat, ColorRef src, ColorRef dst);      // rop: the 32-bit ROP code (index in bits 16..23)
// The pattern (brush) source of a DC.
struct PatternSource {
  const Brush* brush = nullptr;
  ColorRef solid = 0;
  ColorRef text = 0, back = 0;
  bool opaque = true;
  int32_t org_x = 0, org_y = 0;
  bool is_null = false;
  ColorRef at(int32_t x, int32_t y, bool* transparent) const;
};
bool pattern_of_dc(const Dc& dc, PatternSource* out);
bool pattern_of_brush(const Dc& dc, const Brush* brush, PatternSource* out);
void fill_rect_pattern(Painter& p, const Rect& r, const PatternSource& src, uint32_t rop);
void fill_rect_solid(Painter& p, const Rect& r, ColorRef c, int32_t rop2);
void draw_line_pen(Painter& p, Point a, Point b, bool include_last);
void stroke_polyline(Painter& p, ArrayView<const Point> pts, bool closed);      // with the DC's pen, in device coordinates
void fill_polygons(Painter& p, ArrayView<const Point> pts, ArrayView<const uint32_t> counts, bool winding, const PatternSource& src);
bool fill_region_pattern(Painter& p, ArrayView<const Rect> device_rects, const PatternSource& src, uint32_t rop);
bool pen_is_null(const Dc& dc);
bool stroke_to_region(const Dc& dc, ArrayView<const Point> pts, bool closed, List<Rect>* out);      // the area the DC's pen covers along a polyline (device coordinates)
bool pen_geometry(const Dc& dc, const Pen** pen);
void flatten_bezier(Point p0, Point p1, Point p2, Point p3, List<Point>* out);
ColorRef resolve_color(const Dc& dc, ColorRef c);      // PALETTEINDEX / PALETTERGB / DIBINDEX -> RGB

// ---- Paths (Win32kGdiPath.cpp).
void path_move(Dc& dc, Point device);
void path_line(Dc& dc, Point device);
void path_bezier(Dc& dc, Point c1, Point c2, Point end);
void path_close(Dc& dc);
bool path_recording(const Dc& dc);
bool path_flatten(const Path& path, List<Point>* pts, List<uint32_t>* counts, List<uint8_t>* closed);

// ---- Blits (Win32kGdiBlt.cpp).
struct BlitParams {
  uint32_t rop = 0xCC0020;           // ROP3 (index in bits 16..23); with a mask: the rop for mask bits set
  uint32_t rop_back = 0xAA0029;      // with a mask: the rop for mask bits clear
  const Surface* mask = nullptr;
  int32_t mask_x = 0, mask_y = 0;
  ColorRef text = 0, back = 0xFFFFFF;      // the colours a monochrome source is shown in
  int32_t mode = 0;                  // 0 raster operation, 1 AlphaBlend, 2 TransparentBlt
  uint32_t blend = 0;                // BLENDFUNCTION
  ColorRef key = 0;
};
// Copies the pixels of `src` (rectangle srect, pixel coordinates; reversed edges mirror) onto the DC in drect (device coordinates), stretching if the sizes differ.
bool blit_surface(Dc& dst, const Rect& drect, const Surface& src, const Rect& srect, const BlitParams& params);
bool stretch_surface(Dc& dst, const Rect& drect, const Surface& src, const Rect& srect, uint32_t rop, ColorRef text, ColorRef back);

// ---- DIBs (Win32kGdiDib.cpp).
struct DibInfo {
  explicit DibInfo(allocators::IAllocator& a) : colors(a) {}
  int32_t width = 0, height = 0;      // height: positive in the source (bottom-up flag below)
  bool bottom_up = true;
  uint32_t bpp = 0, compression = 0, size_image = 0, clr_used = 0, clr_important = 0, header_size = 0, planes = 1;
  int32_t x_ppm = 0, y_ppm = 0;
  uint32_t masks[3] = {0, 0, 0};
  bool core = false;
  List<uint32_t> colors;      // COLORREFs
  uint64_t colors_bytes = 0, bits_offset = 0;      // bits_offset: where the pixels start in a packed DIB (header + masks + colour table)
};
// Reads the BITMAPINFO at `address` (header, bit fields, colour table); `usage` DIB_RGB_COLORS 0, DIB_PAL_COLORS 1, DIB_PAL_INDICES 2. `max_info` bounds what is read.
bool read_bitmap_info(Call& c, uint64_t address, uint32_t usage, uint32_t max_info, DibInfo* out, const Dc* dc);
uint64_t dib_stride(uint32_t width, uint32_t bpp);
// Decodes `bits` (the DIB's pixel data: uncompressed, or RLE) into a surface-format Surface the caller provides (32 bpp COLORREF array).
bool decode_dib(const DibInfo& info, ConstByteView bits, List<uint32_t>* pixels_top_down);
bool bitmap_from_dib(Call& c, Bitmap* b, const DibInfo& info, ConstByteView bits);

// ---- Text (Win32kGdiText.cpp).
struct TextMetricsData {
  int32_t height = 0, ascent = 0, descent = 0, internal_leading = 0, external_leading = 0, ave_width = 0, max_width = 0, weight = 400, overhang = 0, aspect_x = 96, aspect_y = 96;
  uint16_t first = 0x20, last = 0xFFFF, default_char = 0x3F, break_char = 0x20;
  uint8_t italic = 0, underlined = 0, struck = 0, pitch_family = 0, charset = 0;
};
void text_metrics_for(const Dc& dc, TextMetricsData* out);
int32_t char_advance(const Dc& dc, char32_t ch);
int32_t text_width(const Dc& dc, ArrayView<const char16_t> text, ArrayView<const int32_t> dx);
bool draw_text(Dc& dc, int32_t x, int32_t y, uint32_t flags, const Rect* rect, ArrayView<const char16_t> text, ArrayView<const int32_t> dx, Size* extent);

// ---- Registration helpers: every handler sees the Call through these wrappers.
void register_region_services();
void register_object_services();
void register_dc_services();
void register_draw_services();
void register_path_services();
void register_blt_services();
void register_dib_services();
void register_text_services();
void register_misc_services();

inline uint64_t ret_bool(bool v) { return v ? 1 : 0; }
inline uint64_t ret_i32(int32_t v) { return uint64_t(uint32_t(v)); }
inline uint32_t handle_arg(Call& c, uint32_t i) { return c.arg32(i); }

// RAII-less guard around Call for helpers that need to report errors.
Dc* dc_arg(Call& c, uint32_t handle);      // the DC of the calling process (sets ERROR_INVALID_HANDLE otherwise)

#define GDI_REGISTER(NAME, FN) ::process::win32k::register_handler(WindowsSyscallNumber::NtGdi##NAME, FN)

void caller_dpi(Call& call, uint32_t* scale, uint32_t* dpi);      // DPI virtualization of the calling thread (set_caller_dpi_hook; 1 and 96 without one)

}  // namespace process::win32k::gdi
