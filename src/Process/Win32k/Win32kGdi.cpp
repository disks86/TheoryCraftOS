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
#include "Process/Win32k/Win32kUserMisc.hpp"
#include "Process/Win32k/Win32kGdi.hpp"
#include <new>
#include "Process/AddressSpace.hpp"
#include "Process/Win32k/Win32kGdiInternal.hpp"
#include "Process/WindowsObjects.hpp"

namespace process::win32k {

namespace gdi {

namespace {

struct Slot {
  UniquePointer<Object> obj;
  uint8_t gen = 0;
};

constexpr uint32_t kMaxHandles = 0x10000;
constexpr ColorRef kDesktopColor = make_rgb(58, 110, 165);

// The backing store of a top-level window the desktop composites (not in the handle table: a program cannot select it).
struct WindowSurface {
  uint32_t id = 0, window = 0;
  UniquePointer<Bitmap> bitmap;
};

struct State {
  explicit State(allocators::IAllocator& a) : allocator(a), slots(a), free_list(a), surfaces(a) {}
  allocators::IAllocator& allocator;
  List<Slot> slots;
  List<uint32_t> free_list;
  UniquePointer<Bitmap> screen;
  List<WindowSurface> surfaces;
  uint32_t next_surface = 1;
  uint32_t stock[21] = {};
  uint32_t screen_w = 1024, screen_h = 768;
  const DibMemory* dib_memory = nullptr;
  bool exiting = false;
};

alignas(State) unsigned char g_state_memory[sizeof(State)];
bool g_live = false;

// The one place the raw storage is seen as a State (placement-new'd by state() and destroyed by shutdown()).
State* state_ptr() { return reinterpret_cast<State*>(g_state_memory); }

void init_stock(State& st);
uint32_t g_first_screen_w = 1024, g_first_screen_h = 768;      // the size a new state's screen has (set_screen_size before the state exists)

State* state() {
  allocators::IAllocator* a = system_allocator();
  if (!a) return nullptr;
  if (g_live && &state_ptr()->allocator != a) g_live = false;      // the allocator it used is gone with its system: drop the stale state
  if (!g_live) {
    new (g_state_memory) State(*a);
    g_live = true;
    State& st = *state_ptr();
    st.screen_w = g_first_screen_w;
    st.screen_h = g_first_screen_h;
    st.slots.push_back(Slot{});      // index 0 is never a handle
    init_stock(st);
  }
  return state_ptr();
}

Brush* stock_brush(uint32_t style, ColorRef color, int index) {
  Brush* b = make_object<Brush>(kObjBrush, 0, true);
  if (!b) return nullptr;
  b->style = style;
  b->color = color;
  if (index == 18) b->dc_brush = true;
  return b;
}

Pen* stock_pen(uint32_t style, ColorRef color, int index) {
  Pen* p = make_object<Pen>(kObjPen, 0, true);
  if (!p) return nullptr;
  p->style = style;
  p->color = color;
  p->width = 1;
  if (index == 19) p->dc_pen = true;
  return p;
}

void copy_face(Font& f, const char* name) {
  uint32_t i = 0;
  for (; name[i] && i < 31; ++i) f.face[i] = char16_t(uint8_t(name[i]));
  f.face[i] = 0;
}

Font* stock_font(const char* face, int32_t height, uint8_t pitch_family, uint32_t weight, uint8_t charset) {
  Font* f = make_object<Font>(kObjFont, 0, true);
  if (!f) return nullptr;
  copy_face(*f, face);
  f->lf_height = height;
  f->height = uint32_t(iabs(height));
  f->weight = weight;
  f->pitch_family = pitch_family;
  f->charset = charset;
  return f;
}

void init_stock(State& st) {
  Object* o[21] = {};
  o[0] = stock_brush(0, 0xFFFFFF, 0);
  o[1] = stock_brush(0, make_rgb(192, 192, 192), 1);
  o[2] = stock_brush(0, make_rgb(128, 128, 128), 2);
  o[3] = stock_brush(0, make_rgb(64, 64, 64), 3);
  o[4] = stock_brush(0, 0, 4);
  o[5] = stock_brush(1, 0, 5);
  o[6] = stock_pen(0, 0xFFFFFF, 6);
  o[7] = stock_pen(0, 0, 7);
  o[8] = stock_pen(5, 0, 8);
  o[10] = stock_font("Terminal", 12, 0x31, 400, 255);
  o[11] = stock_font("Courier", 12, 0x31, 400, 0);
  o[12] = stock_font("MS Sans Serif", 12, 0x22, 400, 0);
  o[13] = stock_font("System", 16, 0x22, 700, 0);
  o[14] = stock_font("System", 16, 0x22, 700, 0);
  o[16] = stock_font("System", 16, 0x31, 400, 0);
  o[17] = stock_font("MS Shell Dlg", -12, 0x22, 400, 1);
  o[18] = stock_brush(0, 0xFFFFFF, 18);
  o[19] = stock_pen(0, 0, 19);
  if (Palette* p = make_object<Palette>(kObjPalette, 0, true)) {
    static const uint8_t k[20][3] = {{0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {128, 128, 0}, {0, 0, 128}, {128, 0, 128}, {0, 128, 128}, {192, 192, 192}, {192, 220, 192}, {166, 202, 240},
                                     {255, 251, 240}, {160, 160, 164}, {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {0, 0, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
    for (const auto& e : k) p->entries.push_back(make_rgb(e[0], e[1], e[2]));
    o[15] = p;
  }
  if (Bitmap* b = make_object<Bitmap>(kObjBitmap, 0, true)) {
    b->width = b->height = 1;
    b->bpp = 1;
    b->stride = 4;
    b->data.put_zeros(4);
    b->colors.push_back(0);
    b->colors.push_back(0xFFFFFF);
    o[20] = b;
  }
  for (int i = 0; i < 21; ++i) st.stock[i] = o[i] ? o[i]->handle : 0;
}

void destroy_pid_objects(State& st, uint64_t pid) {
  // device contexts first: they let go of what they have selected
  for (int pass = 0; pass < 2; ++pass) {
    for (uint64_t i = 1; i < st.slots.count(); ++i) {
      Slot& s = st.slots[i];
      if (!s.obj || s.obj->stock || s.obj->pid != pid) continue;
      if ((s.obj->type == kObjDc) != (pass == 0)) continue;
      destroy_object(s.obj->handle);
    }
  }
}

void on_process_exit(uint64_t pid) {
  if (!g_live) return;
  State& st = *state_ptr();
  st.exiting = true;
  destroy_pid_objects(st, pid);
  st.exiting = false;
}

}  // namespace

allocators::IAllocator* alloc() { return system_allocator(); }

void shutdown() {
  if (!g_live) return;
  state_ptr()->~State();
  g_live = false;
}

Object* register_object(UniquePointer<Object>&& obj, uint8_t type, uint64_t pid, bool stock) {
  State* st = state();
  if (!st || !obj) return nullptr;
  uint32_t index;
  if (st->free_list.count() > 0) {
    index = st->free_list[st->free_list.count() - 1];
    st->free_list.remove_at(st->free_list.count() - 1);
  } else {
    if (st->slots.count() >= kMaxHandles) return nullptr;
    index = uint32_t(st->slots.count());
    if (!st->slots.push_back(Slot{})) return nullptr;
  }
  Slot& slot = st->slots[index];
  const uint32_t gen = stock ? 1 : uint32_t(slot.gen) + 1;
  Object* o = obj.get();
  o->type = type;
  o->stock = stock;
  o->pid = pid;
  o->handle = (gen & 0xFF) << 24 | (stock ? kStockFlag : 0) | uint32_t(type) << 16 | index;
  slot.gen = uint8_t(gen);
  slot.obj = util::move(obj);
  return o;
}

Object* find_object(uint32_t handle) {
  if (!g_live || handle == 0) return nullptr;
  State& st = *state_ptr();
  const uint32_t index = handle & 0xFFFF;
  if (index == 0 || index >= st.slots.count()) return nullptr;
  Slot& s = st.slots[index];
  return s.obj && s.obj->handle == handle ? s.obj.get() : nullptr;
}

Object* find_object(uint32_t handle, uint8_t type) {
  Object* o = find_object(handle);
  if (!o) return nullptr;
  if (type == kObjPen) return (o->type == kObjPen || o->type == kObjExtPen) ? o : nullptr;
  return o->type == type ? o : nullptr;
}

Dc* find_dc_with_bitmap(uint32_t bitmap) {
  if (!g_live) return nullptr;
  State& st = *state_ptr();
  for (uint64_t i = 1; i < st.slots.count(); ++i) {
    Slot& s = st.slots[i];
    if (s.obj && s.obj->type == kObjDc && static_cast<Dc*>(s.obj.get())->bitmap == bitmap) return static_cast<Dc*>(s.obj.get());
  }
  return nullptr;
}

bool destroy_object(uint32_t handle) {
  Object* o = find_object(handle);
  if (!o) return false;
  State& st = *state_ptr();
  if (o->type == kObjDc) dc_release_selection(*static_cast<Dc*>(o));
  if (o->type == kObjBitmap) release_dib_memory(*static_cast<Bitmap*>(o));
  Slot& s = st.slots[handle & 0xFFFF];
  s.obj.reset();
  if (!s.obj) st.free_list.push_back(handle & 0xFFFF);
  return true;
}

uint32_t stock_handle(int index) {
  State* st = state();
  return st && index >= 0 && index < 20 ? st->stock[index] : 0;
}
uint32_t default_bitmap_handle() {
  State* st = state();
  return st ? st->stock[20] : 0;
}

IScreenSink* g_screen_sink = nullptr;      // (outside the state: the desktop sets it before any Windows program made the state)

void sink_changed(const Rect& dirty) {
  if (!g_live || !g_screen_sink) return;
  const Rect r = rect_and(dirty, Rect{0, 0, int32_t(state_ptr()->screen_w), int32_t(state_ptr()->screen_h)});
  if (!rect_empty(r)) g_screen_sink->screen_changed(r);
}

Bitmap& screen_bitmap() {
  State& st = *state();
  if (!st.screen) {
    st.screen = UniquePointer<Bitmap>::make(st.allocator, st.allocator);
    Bitmap& b = *st.screen;
    b.width = int32_t(st.screen_w);
    b.height = int32_t(st.screen_h);
    b.bpp = 32;
    b.stride = st.screen_w * 4;
    b.is_screen = true;
    b.data.put_zeros(uint64_t{b.stride} * st.screen_h);
    Surface s = surface_of(b);
    for (int32_t y = 0; y < b.height; ++y) for (int32_t x = 0; x < b.width; ++x) s.put(x, y, kDesktopColor);
  }
  return *st.screen;
}

// ---- Process memory of DIB sections.

void set_dib_memory(const DibMemory* hooks) {
  if (State* st = state()) st->dib_memory = hooks;
}

bool allocate_dib_memory(Process& process, Bitmap& b, uint64_t bytes) {
  State* st = state();
  if (!st || bytes == 0) return false;
  uint64_t address = 0;
  if (st->dib_memory && st->dib_memory->allocate) {
    if (!st->dib_memory->allocate(process, bytes, &address)) return false;
  } else if (AddressSpace* space = process.address_space()) {
    uint64_t rounded = 0;
    if (!AddressSpace::round_up(bytes, &rounded) || !space->find_free(rounded, 0x20000000ull, &address) || !space->map(address, rounded, kProtRead | kProtWrite)) return false;
  } else {
    return false;
  }
  b.proc = &process;
  b.user_address = address;
  b.user_mapped = true;
  return true;
}

void release_dib_memory(Bitmap& b) {
  if (!b.dib || !b.proc || b.user_address == 0 || !b.user_mapped) return;
  State* st = state();
  if (b.section_view != 0) {      // (an ended process takes its views with it)
    if (st && !st->exiting && !b.proc->exited()) unmap_view_later(*b.proc, b.section_view);
    b.section_view = 0;
  } else if (st && !st->exiting) {
    if (st->dib_memory && st->dib_memory->release) st->dib_memory->release(*b.proc, b.user_address, b.data.size());
    else if (AddressSpace* space = b.proc->address_space()) space->unmap(b.user_address, b.data.size());
  }
  b.user_address = 0;
  b.proc = nullptr;
}

void dib_sync_in(Bitmap& b) {
  if (!b.dib || !b.proc || b.user_address == 0 || b.data.size() == 0) return;
  b.proc->read_user(b.user_address, b.data.writable_view());
}

void dib_sync_out(Bitmap& b) {
  if (!b.dib || !b.proc || b.user_address == 0 || b.data.size() == 0) return;
  b.proc->write_user(b.user_address, b.data.view());
}

bool map_section_for_dib(Process& process, uint64_t section, uint64_t offset, uint64_t bytes, uint64_t* view, uint64_t* address) {
  ObjectManager* objects = system_objects();
  if (bytes == 0 || !objects) return false;
  objects->sync().unlock();      // (the handler holds the system lock, and mapping a view takes it)
  const uint32_t status = windowsabi::map_section_handle(process, section, offset, bytes, view, address);
  objects->sync().lock();
  return status == 0;
}

void use_dib_section_view(Process& process, Bitmap& b, uint64_t view, uint64_t address) {
  b.proc = &process;
  b.user_address = address;
  b.user_mapped = true;
  b.section_view = view;
}

// ---- Floating point.

double g_floor(double v) {
  if (v != v) return 0;
  if (v >= 9.0e15 || v <= -9.0e15) return v;
  const double d = double(int64_t(v));
  return d > v ? d - 1 : d;
}
double g_sqrt(double v) {
  if (!(v > 0)) return 0;
  double x = v < 1 ? 1 : v;
  if (x > 1e6) { double y = 1e3; while (y * y < x) y *= 2; x = y; }
  for (int i = 0; i < 100; ++i) {
    const double n = 0.5 * (x + v / x);
    if (n == x) break;
    x = n;
  }
  return x;
}
namespace {
constexpr double kPi = 3.14159265358979323846;
double sin_reduced(double v) {      // |v| <= pi
  const double v2 = v * v;
  double term = v, sum = v;
  for (int i = 1; i < 14; ++i) {
    term *= -v2 / double((2 * i) * (2 * i + 1));
    sum += term;
  }
  return sum;
}
double atan_unit(double x) {      // |x| <= 1
  x = x / (1 + g_sqrt(1 + x * x));
  x = x / (1 + g_sqrt(1 + x * x));
  const double x2 = x * x;
  double term = x, sum = x;
  for (int i = 1; i < 12; ++i) {
    term *= -x2;
    sum += term / double(2 * i + 1);
  }
  return sum * 4;
}
}  // namespace
double g_sin(double v) {
  if (v != v) return 0;
  const double turns = g_floor(v / (2 * kPi) + 0.5);
  v -= turns * 2 * kPi;
  if (v > kPi / 2) v = kPi - v;
  else if (v < -kPi / 2) v = -kPi - v;
  return sin_reduced(v);
}
double g_cos(double v) { return g_sin(v + kPi / 2); }
double g_atan2(double y, double x) {
  if (x == 0 && y == 0) return 0;
  const double ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
  double a = ax >= ay ? atan_unit(ay / ax) : kPi / 2 - atan_unit(ax / ay);
  if (x < 0) a = kPi - a;
  return y < 0 ? -a : a;
}

// ---- The screen.

}  // namespace gdi

namespace gdi {

uint32_t screen_width() { return state() ? state()->screen_w : 0; }
uint32_t screen_height() { return state() ? state()->screen_h : 0; }

bool set_screen_size(uint32_t width, uint32_t height) {
  if (width < 16 || height < 16 || width > 16384 || height > 16384) return false;
  State* st = state();
  if (!st) {      // no system yet (the desktop sizes the screen before any Windows program ran): the state is made at this size
    g_first_screen_w = width;
    g_first_screen_h = height;
    return true;
  }
  st->screen_w = width;
  st->screen_h = height;
  st->screen.reset();
  screen_bitmap();
  return true;
}

ConstByteView screen_pixels() {
  if (!state()) return ConstByteView();
  return screen_bitmap().data.view();
}

void set_screen_sink(IScreenSink* sink) { g_screen_sink = sink; }

// ---- Window surfaces

namespace {
IWindowSurfaceSink* g_window_sink = nullptr;

WindowSurface* find_surface(uint32_t id) {
  State* st = state();
  if (!st || id == 0) return nullptr;
  for (WindowSurface& s : st->surfaces.view()) if (s.id == id) return &s;
  return nullptr;
}

// A surface of width x height pixels, cleared to the desktop colour (the window paints it when told to).
bool make_surface_bitmap(State& st, WindowSurface& s, uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width > 16384 || height > 16384) return false;
  UniquePointer<Bitmap> b = UniquePointer<Bitmap>::make(st.allocator, st.allocator);
  if (!b) return false;
  b->width = int32_t(width);
  b->height = int32_t(height);
  b->bpp = 32;
  b->stride = width * 4;
  b->window_surface = s.window;
  if (!b->data.put_zeros(uint64_t{b->stride} * height)) return false;
  Surface v = surface_of(*b);
  for (int32_t y = 0; y < b->height; ++y) for (int32_t x = 0; x < b->width; ++x) v.put(x, y, kDesktopColor);
  s.bitmap = util::move(b);
  return true;
}
}  // namespace

uint32_t create_window_surface(uint32_t window, uint32_t width, uint32_t height) {
  State* st = state();
  if (!st) return 0;
  WindowSurface s;
  s.id = st->next_surface++;
  s.window = window;
  if (!make_surface_bitmap(*st, s, width, height)) return 0;
  const uint32_t id = s.id;
  if (!st->surfaces.push_back(util::move(s))) return 0;
  return id;
}

bool resize_window_surface(uint32_t id, uint32_t width, uint32_t height) {
  WindowSurface* s = find_surface(id);
  if (!s) return false;
  if (s->bitmap && uint32_t(s->bitmap->width) == width && uint32_t(s->bitmap->height) == height) return true;
  return make_surface_bitmap(*state(), *s, width, height);
}

void destroy_window_surface(uint32_t id) {
  State* st = state();
  if (!st || id == 0) return;
  for (uint64_t i = 0; i < st->surfaces.count(); ++i) {
    if (st->surfaces[i].id != id) continue;
    // DCs still aimed at it draw nowhere from now on (the window manager releases them with the window)
    for (Slot& slot : st->slots.view()) if (slot.obj && slot.obj->type == kObjDc) { Dc* d = static_cast<Dc*>(slot.obj.get()); if (d->surface == id) d->surface_clip.clear(); }
    st->surfaces.remove_at(i);
    return;
  }
}

bool window_surface_size(uint32_t id, uint32_t* width, uint32_t* height) {
  WindowSurface* s = find_surface(id);
  if (!s || !s->bitmap) return false;
  *width = uint32_t(s->bitmap->width);
  *height = uint32_t(s->bitmap->height);
  return true;
}

// The pixels as 32-bit words (B, G, R, X bytes = 0xXXRRGGBB on this little-endian machine): the desktop draws them as they are. The surface's storage comes from the
// allocator, which hands out blocks aligned for any type; a misaligned block (never expected) gives an empty view rather than stopping in cast().
ArrayView<uint32_t> window_surface_pixels(uint32_t id, uint32_t* width, uint32_t* height) {
  WindowSurface* s = find_surface(id);
  if (!s || !s->bitmap) return ArrayView<uint32_t>();
  ByteView bytes = s->bitmap->data.writable_view();
  if ((reinterpret_cast<uintptr_t>(bytes.data()) & 3) != 0 || (bytes.size() & 3) != 0) return ArrayView<uint32_t>();
  *width = uint32_t(s->bitmap->width);
  *height = uint32_t(s->bitmap->height);
  return bytes.cast<uint32_t>();
}

Bitmap* window_surface_bitmap(uint32_t id) {
  WindowSurface* s = find_surface(id);
  return s ? s->bitmap.get() : nullptr;
}

void set_window_surface_sink(IWindowSurfaceSink* sink) { g_window_sink = sink; }

ByteView window_surface_bytes(uint32_t id, uint32_t* width, uint32_t* height) {
  WindowSurface* s = find_surface(id);
  if (!s || !s->bitmap) return ByteView();
  *width = uint32_t(s->bitmap->width);
  *height = uint32_t(s->bitmap->height);
  return s->bitmap->data.writable_view();
}

void window_surface_damaged(uint32_t id, const Rect& dirty) {
  const WindowSurface* s = find_surface(id);
  if (s && s->bitmap) window_surface_changed(*s->bitmap, dirty);
}

void window_surface_changed(const Bitmap& b, const Rect& dirty) {
  if (!g_window_sink || b.window_surface == 0) return;
  const Rect r = rect_and(dirty, Rect{0, 0, b.width, b.height});
  if (!rect_empty(r)) g_window_sink->surface_changed(b.window_surface, r);
}

uint32_t create_screen_dc(uint64_t pid, int32_t origin_x, int32_t origin_y, ArrayView<const Rect> clip, uint32_t surface) {
  Dc* dc = new_dc(pid);
  if (!dc) return 0;
  dc->screen = true;
  dc->surface = surface;
  dc->origin_x = origin_x;
  dc->origin_y = origin_y;
  for (const Rect& r : clip) if (!rect_empty(r)) dc->surface_clip.push_back(r);
  return dc->handle;
}

bool update_screen_dc(uint32_t dc, int32_t origin_x, int32_t origin_y, ArrayView<const Rect> clip, uint32_t surface) {
  Dc* d = find_dc(dc);
  if (!d || !d->screen) return false;
  d->surface = surface;
  d->origin_x = origin_x;
  d->origin_y = origin_y;
  d->surface_clip.clear();
  for (const Rect& r : clip) if (!rect_empty(r)) d->surface_clip.push_back(r);
  return true;
}

bool is_dc(uint32_t handle) { return find_dc(handle) != nullptr; }
bool is_screen_dc(uint32_t handle) {
  const Dc* d = find_dc(handle);
  return d && d->screen;
}

bool delete_dc(uint32_t dc) {
  Dc* d = find_dc(dc);
  return d && destroy_object(dc);
}

uint32_t create_region(uint64_t pid, ArrayView<const Rect> rects) {
  Region* r = new_region(pid, rects);
  return r ? r->handle : 0;
}

bool region_rects(uint32_t region, List<Rect>* out) {
  Region* r = find_region(region);
  if (!r) return false;
  for (const Rect& e : r->rects) if (!out->push_back(e)) return false;
  return true;
}

bool replace_region(uint32_t region, ArrayView<const Rect> rects) {
  Region* r = find_region(region);
  return r && rgn_normalize(rects, &r->rects);
}

bool is_region(uint32_t handle) { return find_region(handle) != nullptr; }

bool bitmap_info(uint32_t bitmap, int32_t* width, int32_t* height, uint32_t* bits_per_pixel) {
  Bitmap* b = find_bitmap(bitmap);
  if (!b) return false;
  *width = b->width;
  *height = b->height;
  *bits_per_pixel = b->bpp;
  return true;
}

bool bitmap_pixels(uint32_t bitmap, List<uint32_t>* out) {
  Bitmap* b = find_bitmap(bitmap);
  if (!b) return false;
  dib_sync_in(*b);
  Surface s = surface_of(*b);
  if (!s.valid()) return false;
  bool has_alpha = false;
  if (b->bpp == 32) {
    for (int32_t y = 0; y < b->height && !has_alpha; ++y) for (int32_t x = 0; x < b->width; ++x) if ((s.get_raw32(x, y) >> 24) != 0) { has_alpha = true; break; }
  }
  for (int32_t y = 0; y < b->height; ++y) {
    for (int32_t x = 0; x < b->width; ++x) {
      const ColorRef c = s.get(x, y);
      const uint32_t a = has_alpha ? (s.get_raw32(x, y) >> 24) : 0xFF;
      if (!out->push_back(a << 24 | cr_r(c) << 16 | cr_g(c) << 8 | cr_b(c))) return false;
    }
  }
  return true;
}

uint32_t create_bitmap(uint64_t pid, int32_t width, int32_t height, ArrayView<const uint32_t> argb) {
  if (width <= 0 || height <= 0 || width > 32768 || height > 32768 || uint64_t(width) * uint64_t(height) != argb.size()) return 0;
  Bitmap* b = make_object<Bitmap>(kObjBitmap, pid);
  if (!b) return 0;
  b->width = width;
  b->height = height;
  b->bpp = 32;
  b->stride = uint32_t(width) * 4;
  if (!b->data.put_zeros(uint64_t{b->stride} * uint32_t(height))) { destroy_object(b->handle); return 0; }
  Surface s = surface_of(*b);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const uint32_t p = argb[uint64_t(y) * uint32_t(width) + uint32_t(x)];
      s.put_raw32(x, y, p);
    }
  }
  return b->handle;
}

Dc* dc_arg(Call& c, uint32_t handle) {
  Dc* dc = find_dc(handle);
  if (!dc || (!dc->stock && dc->pid != c.pid() && dc->pid != 0)) {
    c.set_last_error(kErrorInvalidHandle);
    return nullptr;
  }
  return dc;
}

}  // namespace gdi

namespace {
// The system colour brushes and pens of user_misc (GetSysColorBrush / GetSysColorPen): solid GDI objects of a colour owned by the asking process.
uint32_t make_solid_brush(uint64_t pid, uint32_t colorref) {
  gdi::Brush* b = gdi::make_object<gdi::Brush>(gdi::kObjBrush, pid);
  if (!b) return 0;
  b->style = 0;
  b->color = colorref;
  return b->handle;
}
uint32_t make_solid_pen(uint64_t pid, uint32_t colorref) {
  gdi::Pen* p = gdi::make_object<gdi::Pen>(gdi::kObjPen, pid);
  if (!p) return 0;
  p->style = 0;
  p->width = 1;
  p->color = colorref;
  return p->handle;
}
}  // namespace

void install_gdi() {
  user_misc::set_solid_brush_factory(make_solid_brush);
  user_misc::set_solid_pen_factory(make_solid_pen);
  register_process_exit_hook(gdi::on_process_exit);
  gdi::register_region_services();
  gdi::register_object_services();
  gdi::register_dc_services();
  gdi::register_draw_services();
  gdi::register_path_services();
  gdi::register_blt_services();
  gdi::register_dib_services();
  gdi::register_text_services();
  gdi::register_misc_services();
}

}  // namespace process::win32k
