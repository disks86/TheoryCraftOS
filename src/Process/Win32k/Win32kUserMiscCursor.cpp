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

// Cursors and icons (one kind of object, HCURSOR == HICON), the cursor of the screen (position, clip rectangle, show counter) and the system cursors. The images are GDI bitmaps
// the icon owns (SetCursorIconData takes them over); GetIconInfo hands out copies. Drawing an icon (DrawIconEx) needs a way to blit to a device context that the gdi:: contract
// does not have, and is not part of this module.
namespace process::win32k::user_misc {

namespace {

constexpr uint32_t kMaxFrames = 1024, kMaxSteps = 4096, kMaxNameChars = 512, kDefaultCursorSize = 32;
constexpr uint32_t kLrShared = 0x8000;

struct Frame {
  uint32_t width = 0, height = 0, color = 0, alpha = 0, mask = 0;
  int32_t hot_x = 0, hot_y = 0;
};

struct CursorIcon {
  uint64_t pid;
  bool is_icon;
  bool system = false;
  uint32_t flags = 0, delay = 0, num_steps = 0;
  uint64_t rsrc = 0, param = 0, resource_id = 0;
  bool resource_is_id = false;
  WideString module, resource_name;
  List<Frame> frames;
  List<uint32_t> sequence, rates;
  CursorIcon(allocators::IAllocator& a, uint64_t owner, bool icon) : pid(owner), is_icon(icon), module(a), resource_name(a), frames(a), sequence(a), rates(a) {}
};

struct SystemEntry { uint32_t id, handle; };

struct CursorState {
  Registry<CursorIcon> icons;
  List<SystemEntry> system_cursors, system_icons;
  int32_t show_count = 0;
  Rect clip;
  bool clip_set = false;
  uint64_t clip_pid = 0;
  explicit CursorState(allocators::IAllocator& a) : icons(a, 0x0002), system_cursors(a), system_icons(a) {}
};
UniquePointer<CursorState> g_cursors;
CursorState* st() { return lazy_state(g_cursors); }

CursorIcon* find_icon(CursorState& s, uint64_t handle) { return (handle >> 32) == 0 ? s.icons.find(uint32_t(handle)) : nullptr; }

bool names_equal(const WideString& a, const WideString& b) { return a.equals(b.view(), true); }

uint32_t le32(const uint8_t* raw, uint32_t o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; }
uint64_t le64(const uint8_t* raw, uint32_t o) { return uint64_t(le32(raw, o)) | uint64_t(le32(raw, o + 4)) << 32; }

// The name of a resource in a UNICODE_STRING: an integer resource has Length 0 and the id in the buffer pointer.
bool read_resource_name(Call& c, uint64_t address, WideString* text, bool* is_id, uint64_t* id) {
  *is_id = false;
  *id = 0;
  if (address == 0) return true;
  uint16_t length = 0;
  uint64_t buffer = 0;
  if (!c.read_u16(address, &length) || !c.read_u64(address + 8, &buffer)) return false;
  if (length == 0) { *is_id = buffer != 0; *id = buffer; return true; }
  if (length / 2 > kMaxNameChars) return false;
  return c.read_wide(buffer, length / 2, text);
}

bool bitmap_ok(uint32_t bitmap) {
  int32_t w = 0, h = 0;
  uint32_t bpp = 0;
  return gdi::bitmap_info(bitmap, &w, &h, &bpp);
}

// A copy of a bitmap for the caller (GetIconInfo): 32-bit pixels, the same size.
uint32_t copy_bitmap(Call& c, uint32_t bitmap) {
  if (bitmap == 0) return 0;
  int32_t w = 0, h = 0;
  uint32_t bpp = 0;
  if (!gdi::bitmap_info(bitmap, &w, &h, &bpp)) return 0;
  List<uint32_t> pixels(*c.allocator());
  if (!gdi::bitmap_pixels(bitmap, &pixels)) return 0;
  return gdi::create_bitmap(c.pid(), w, h, pixels.view());
}

void frame_size(const CursorIcon& icon, uint32_t step, uint32_t* w, uint32_t* h) {
  if (icon.frames.count() == 0) { *w = *h = kDefaultCursorSize; return; }
  uint32_t index = step;
  if (step < icon.sequence.count()) index = icon.sequence[step];
  const Frame& f = icon.frames[index < icon.frames.count() ? index : 0];
  *w = f.width;
  *h = f.height;
}

// --- services
uint64_t svc_set_cursor_icon_data(Call& c) {
  CursorState* s = st();
  CursorIcon* icon = s ? find_icon(*s, c.arg(0)) : nullptr;
  if (!icon) return c.fail(kErrorInvalidCursorHandle);
  WideString module(*c.allocator()), name(*c.allocator());
  bool is_id = false, module_is_id = false;
  uint64_t id = 0, module_id = 0;
  if (c.arg(1) != 0 && !read_resource_name(c, c.arg(1), &module, &module_is_id, &module_id)) return c.fail(kErrorNoAccess);
  if (c.arg(2) != 0 && !read_resource_name(c, c.arg(2), &name, &is_id, &id)) return c.fail(kErrorNoAccess);
  uint8_t raw[48];
  if (!c.read(c.arg(3), ByteView(raw, 48))) return c.fail(kErrorNoAccess);
  const uint32_t flags = le32(raw, 0), steps = le32(raw, 4), frames = le32(raw, 8), delay = le32(raw, 12);
  const uint64_t frames_at = le64(raw, 16), seq_at = le64(raw, 24), rates_at = le64(raw, 32), rsrc = le64(raw, 40);
  if (frames == 0 || frames > kMaxFrames || steps > kMaxSteps || (steps == 0 && frames != 1)) return c.fail(kErrorInvalidParameter);
  const uint32_t step_count = steps == 0 ? 1 : steps;
  List<Frame> new_frames(*c.allocator());
  for (uint32_t i = 0; i < frames; ++i) {
    uint8_t f[40];
    if (!c.read(frames_at + uint64_t{i} * 40, ByteView(f, 40))) return c.fail(kErrorNoAccess);
    Frame frame;
    frame.width = le32(f, 0);
    frame.height = le32(f, 4);
    const uint64_t color = le64(f, 8), alpha = le64(f, 16), mask = le64(f, 24);
    if (frame.width == 0 || frame.height == 0 || frame.width > 4096 || frame.height > 4096 || (color >> 32) || (alpha >> 32) || (mask >> 32)) return c.fail(kErrorInvalidParameter);
    frame.color = uint32_t(color);
    frame.alpha = uint32_t(alpha);
    frame.mask = uint32_t(mask);
    if ((frame.mask == 0 && frame.color == 0) || (frame.color && !bitmap_ok(frame.color)) || (frame.alpha && !bitmap_ok(frame.alpha)) || (frame.mask && !bitmap_ok(frame.mask)))
      return c.fail(kErrorInvalidParameter);
    frame.hot_x = int32_t(le32(f, 32));
    frame.hot_y = int32_t(le32(f, 36));
    new_frames.push_back(frame);
  }
  List<uint32_t> seq(*c.allocator()), rates(*c.allocator());
  if (steps != 0 && seq_at != 0) {
    for (uint32_t i = 0; i < step_count; ++i) {
      uint32_t v = 0;
      if (!c.read_u32(seq_at + uint64_t{i} * 4, &v)) return c.fail(kErrorNoAccess);
      if (v >= frames) return c.fail(kErrorInvalidParameter);
      seq.push_back(v);
    }
  }
  if (steps != 0 && rates_at != 0) {
    for (uint32_t i = 0; i < step_count; ++i) {
      uint32_t v = 0;
      if (!c.read_u32(rates_at + uint64_t{i} * 4, &v)) return c.fail(kErrorNoAccess);
      rates.push_back(v);
    }
  }
  icon->frames.clear();
  icon->sequence.clear();
  icon->rates.clear();
  for (const Frame& f : new_frames.view()) icon->frames.push_back(f);
  for (uint32_t v : seq.view()) icon->sequence.push_back(v);
  for (uint32_t v : rates.view()) icon->rates.push_back(v);
  icon->flags = flags;
  icon->num_steps = step_count;
  icon->delay = delay;
  icon->rsrc = rsrc;
  icon->module.clear();
  icon->module.append(module.view());
  icon->resource_name.clear();
  icon->resource_name.append(name.view());
  icon->resource_is_id = is_id;
  icon->resource_id = id;
  return 1;
}

uint64_t svc_find_existing(Call& c) {
  CursorState* s = st();
  if (!s) return 0;
  WideString module(*c.allocator()), name(*c.allocator());
  bool is_id = false, module_is_id = false;
  uint64_t id = 0, module_id = 0;
  if (c.arg(0) != 0 && !read_resource_name(c, c.arg(0), &module, &module_is_id, &module_id)) return c.fail(kErrorNoAccess);
  if (c.arg(1) != 0 && !read_resource_name(c, c.arg(1), &name, &is_id, &id)) return c.fail(kErrorNoAccess);
  List<uint32_t> all(*c.allocator());
  s->icons.handles(&all);
  for (uint32_t h : all.view()) {
    CursorIcon* icon = s->icons.find(h);
    if (!icon || icon->rsrc == 0 || icon->rsrc != c.arg(2) || !(icon->flags & kLrShared)) continue;
    if (!names_equal(icon->module, module)) continue;
    if (icon->resource_is_id != is_id) continue;
    if (is_id ? icon->resource_id == id : names_equal(icon->resource_name, name)) return h;
  }
  return 0;
}

uint64_t svc_destroy_cursor(Call& c) {
  CursorState* s = st();
  CursorIcon* icon = s ? find_icon(*s, c.arg(0)) : nullptr;
  if (!icon) return c.fail(kErrorInvalidCursorHandle);
  if (icon->system) return c.fail(kErrorAccessDenied);      // a system cursor is never destroyed
  if ((icon->flags & kLrShared) && c.arg32(1) == 0) return 1;      // a shared resource stays (only a forced destroy removes it)
  const uint32_t handle = c.arg32(0);
  if (ThreadRecord* t = find_thread_record(c.tid())) if (t->cursor == handle) t->cursor = 0;
  s->icons.destroy(handle);
  return 1;
}

uint64_t svc_get_icon_info(Call& c) {
  CursorState* s = st();
  CursorIcon* icon = s ? find_icon(*s, c.arg(0)) : nullptr;
  if (!icon || icon->frames.count() == 0) return c.fail(kErrorInvalidCursorHandle);
  const uint32_t index = icon->sequence.count() > 0 ? icon->sequence[0] : 0;
  const Frame frame = icon->frames[index < icon->frames.count() ? index : 0];
  const uint32_t mask_copy = copy_bitmap(c, frame.mask), color_copy = copy_bitmap(c, frame.color);
  uint8_t info[32] = {};
  info[0] = icon->is_icon ? 1 : 0;
  const int32_t hx = icon->is_icon ? int32_t(frame.width / 2) : frame.hot_x, hy = icon->is_icon ? int32_t(frame.height / 2) : frame.hot_y;
  for (int i = 0; i < 4; ++i) {
    info[4 + i] = uint8_t(uint32_t(hx) >> (8 * i));
    info[8 + i] = uint8_t(uint32_t(hy) >> (8 * i));
    info[16 + i] = uint8_t(mask_copy >> (8 * i));
    info[24 + i] = uint8_t(color_copy >> (8 * i));
  }
  if (!c.write(c.arg(1), ConstByteView(info, 32))) return c.fail(kErrorNoAccess);
  auto put_name = [&](uint64_t us, const WideString& text, bool is_id, uint64_t id) -> bool {
    if (us == 0) return true;
    uint16_t capacity = 0;
    uint64_t buffer = 0;
    if (!c.read_u16(us + 2, &capacity) || !c.read_u64(us + 8, &buffer)) return false;
    if (is_id) return c.write_u16(us, 0) && c.write_u64(us + 8, id);
    const int32_t n = c.write_wide(buffer, text.view(), capacity / 2, false);
    return n >= 0 && c.write_u16(us, uint16_t(n * 2));
  };
  if (!put_name(c.arg(2), icon->module, false, 0) || !put_name(c.arg(3), icon->resource_name, icon->resource_is_id, icon->resource_id)) return c.fail(kErrorNoAccess);
  if (c.arg(4) != 0) {
    int32_t w = 0, h = 0;
    uint32_t bpp = 1;
    if (frame.color) gdi::bitmap_info(frame.color, &w, &h, &bpp);
    if (!c.write_u32(c.arg(4), bpp)) return c.fail(kErrorNoAccess);
  }
  return 1;
}

uint64_t svc_get_icon_size(Call& c) {
  CursorState* s = st();
  CursorIcon* icon = s ? find_icon(*s, c.arg(0)) : nullptr;
  if (!icon) return c.fail(kErrorInvalidCursorHandle);
  uint32_t w = 0, h = 0;
  frame_size(*icon, c.arg32(1), &w, &h);
  if (!c.write_u32(c.arg(2), w) || !c.write_u32(c.arg(3), h)) return c.fail(kErrorNoAccess);
  return 1;
}

uint64_t svc_get_cursor_frame_info(Call& c) {
  CursorState* s = st();
  CursorIcon* icon = s ? find_icon(*s, c.arg(0)) : nullptr;
  if (!icon) return c.fail(kErrorInvalidCursorHandle);
  const uint32_t step = c.arg32(1);
  const uint32_t steps = icon->num_steps == 0 ? 1 : icon->num_steps;
  if (step >= steps) return c.fail(kErrorInvalidParameter);
  const uint32_t rate = icon->rates.count() > step ? icon->rates[step] : icon->delay;
  if ((c.arg(2) != 0 && !c.write_u32(c.arg(2), rate)) || (c.arg(3) != 0 && !c.write_u32(c.arg(3), steps))) return c.fail(kErrorNoAccess);
  return c.arg(0);
}

uint64_t svc_set_cursor(Call& c) {
  CursorState* s = st();
  if (!s) return 0;
  const uint64_t handle = c.arg(0);
  if (handle != 0 && !find_icon(*s, handle)) return c.fail(kErrorInvalidCursorHandle);
  ThreadRecord* t = thread_record(c.tid(), c.pid());
  if (!t) return 0;
  const uint32_t previous = t->cursor;
  t->cursor = uint32_t(handle);
  return previous;
}

uint64_t svc_get_cursor(Call& c) {
  ThreadRecord* t = find_thread_record(c.tid());
  return t ? t->cursor : 0;
}

uint64_t svc_show_cursor(Call& c) {
  CursorState* s = st();
  if (!s) return 0;
  s->show_count += c.arg_bool(0) ? 1 : -1;
  return uint64_t(int64_t(s->show_count)) & 0xFFFFFFFFu;
}

Rect screen_rect() { return Rect{0, 0, int32_t(gdi::screen_width()), int32_t(gdi::screen_height())}; }

uint64_t svc_set_system_cursor(Call& c) {
  CursorState* s = st();
  CursorIcon* source = s ? find_icon(*s, c.arg(0)) : nullptr;
  if (!source) return c.fail(kErrorInvalidCursorHandle);
  const uint32_t id = c.arg32(1);
  if (id < 32512 || id > 32651) return c.fail(kErrorInvalidParameter);
  const uint32_t target_handle = system_cursor(c.pid(), id);
  CursorIcon* target = s->icons.find(target_handle);
  if (!target) return 0;
  if (target_handle != c.arg32(0)) {      // the system cursor takes over the images of the cursor, which is gone afterwards
    target->frames.clear();
    target->sequence.clear();
    target->rates.clear();
    for (const Frame& f : source->frames.view()) target->frames.push_back(f);
    for (uint32_t v : source->sequence.view()) target->sequence.push_back(v);
    for (uint32_t v : source->rates.view()) target->rates.push_back(v);
    target->num_steps = source->num_steps;
    target->delay = source->delay;
    if (!source->system) s->icons.destroy(c.arg32(0));
  }
  return 1;
}

uint64_t svc_get_cursor_info(Call& c) {
  uint32_t size = 0, flags = 0, cursor = 0;
  {
    SystemLock lock;
    CursorState* s = st();
    if (!s) return 0;
    if (!c.read_u32(c.arg(0), &size)) return c.fail(kErrorNoAccess);
    if (size != 24) return c.fail(kErrorInvalidParameter);
    flags = s->show_count >= 0 ? 1 : 0;
    ThreadRecord* t = find_thread_record(c.tid());
    cursor = t ? t->cursor : 0;
  }
  int32_t x = 0, y = 0;
  user::cursor_position(&x, &y);
  uint8_t out[24] = {};
  out[0] = 24;
  for (int i = 0; i < 4; ++i) {
    out[4 + i] = uint8_t(flags >> (8 * i));
    out[8 + i] = uint8_t(cursor >> (8 * i));
    out[16 + i] = uint8_t(uint32_t(x) >> (8 * i));
    out[20 + i] = uint8_t(uint32_t(y) >> (8 * i));
  }
  SystemLock lock;
  if (!c.write(c.arg(0), ConstByteView(out, 24))) return c.fail(kErrorNoAccess);
  return 1;
}

uint64_t svc_get_cursor_pos(Call& c) {
  int32_t x = 0, y = 0;
  user::cursor_position(&x, &y);
  const int32_t pt[2] = {x, y};
  SystemLock lock;
  if (!c.write_array(c.arg(0), pt, 2)) return c.fail(kErrorNoAccess);
  return 1;
}

Rect clip_now() {
  CursorState* s = st();
  if (s && s->clip_set) return s->clip;
  return screen_rect();
}

uint64_t svc_set_cursor_pos(Call& c) {
  int32_t x = c.argi(0), y = c.argi(1);
  Rect clip;
  {
    SystemLock lock;
    clip = clip_now();
  }
  if (x < clip.left) x = clip.left;
  if (x > clip.right - 1) x = clip.right - 1;
  if (y < clip.top) y = clip.top;
  if (y > clip.bottom - 1) y = clip.bottom - 1;
  user::inject_mouse_move(x, y);
  return 1;
}

uint64_t svc_clip_cursor(Call& c) {
  Rect clip;
  bool set = false;
  if (c.arg(0) != 0) {
    SystemLock lock;
    Rect r;
    if (!c.read_struct(c.arg(0), &r)) return c.fail(kErrorNoAccess);
    const Rect screen = screen_rect();
    clip = Rect{r.left < screen.left ? screen.left : r.left, r.top < screen.top ? screen.top : r.top, r.right > screen.right ? screen.right : r.right,
                r.bottom > screen.bottom ? screen.bottom : r.bottom};
    if (clip.left >= clip.right || clip.top >= clip.bottom) return c.fail(kErrorInvalidParameter);
    set = true;
  }
  {
    SystemLock lock;
    CursorState* s = st();
    if (!s) return 0;
    s->clip_set = set;
    s->clip = clip;
    s->clip_pid = c.pid();
  }
  int32_t x = 0, y = 0;
  user::cursor_position(&x, &y);
  const Rect limit = set ? clip : screen_rect();
  const int32_t nx = x < limit.left ? limit.left : x > limit.right - 1 ? limit.right - 1 : x;
  const int32_t ny = y < limit.top ? limit.top : y > limit.bottom - 1 ? limit.bottom - 1 : y;
  if (nx != x || ny != y) user::inject_mouse_move(nx, ny);
  return 1;
}

uint64_t svc_get_clip_cursor(Call& c) {
  SystemLock lock;
  const Rect r = clip_now();
  if (!c.write_struct(c.arg(0), r)) return c.fail(kErrorNoAccess);
  return 1;
}

}  // namespace

bool icon_exists(uint32_t icon) { return g_cursors && g_cursors->icons.contains(icon); }

uint32_t create_cursor_icon(uint64_t pid, bool is_icon) {
  CursorState* s = st();
  if (!s) return 0;
  uint32_t handle = 0;
  return s->icons.create(&handle, pid, is_icon) ? handle : 0;
}

uint64_t icon_param(uint32_t icon) {
  CursorState* s = st();
  CursorIcon* i = s ? s->icons.find(icon) : nullptr;
  return i ? i->param : 0;
}

uint64_t set_icon_param(uint32_t icon, uint64_t param) {
  CursorState* s = st();
  CursorIcon* i = s ? s->icons.find(icon) : nullptr;
  if (!i) return 0;
  const uint64_t previous = i->param;
  i->param = param;
  return previous;
}

namespace {
uint32_t stock_object(uint64_t pid, uint32_t id, bool icon) {
  CursorState* s = st();
  if (!s) return 0;
  List<SystemEntry>& table = icon ? s->system_icons : s->system_cursors;
  for (const SystemEntry& e : table.view()) if (e.id == id && s->icons.find(e.handle)) return e.handle;
  uint32_t handle = 0;
  CursorIcon* object = s->icons.create(&handle, pid, icon);
  if (!object) return 0;
  object->system = true;
  object->resource_is_id = true;
  object->resource_id = id;
  table.push_back(SystemEntry{id, handle});
  return handle;
}
}  // namespace

uint32_t system_cursor(uint64_t pid, uint32_t ocr_id) { return stock_object(pid, ocr_id, false); }
uint32_t system_icon(uint64_t pid, uint32_t oic_id) { return stock_object(pid, oic_id, true); }

Rect cursor_clip_rect() {
  SystemLock lock;
  return clip_now();
}

void cursors_process_exited(uint64_t pid) {
  CursorState* s = g_cursors.get();
  if (!s) return;
  List<uint32_t> all(*system_allocator());
  s->icons.handles(&all);
  for (uint32_t h : all.view()) {
    CursorIcon* icon = s->icons.find(h);
    if (!icon || icon->pid != pid) continue;
    if (icon->system) continue;      // (system cursors belong to the system, whichever process asked first)
    s->icons.destroy(h);
  }
  if (s->clip_set && s->clip_pid == pid) s->clip_set = false;      // a process that clipped the cursor and ends releases it
}

void install_cursors() {
  using N = WindowsSyscallNumber;
  register_handler(N::NtUserSetCursorIconData, svc_set_cursor_icon_data);
  register_handler(N::NtUserFindExistingCursorIcon, svc_find_existing);
  register_handler(N::NtUserDestroyCursor, svc_destroy_cursor);
  register_handler(N::NtUserGetIconInfo, svc_get_icon_info);
  register_handler(N::NtUserGetIconSize, svc_get_icon_size);
  register_handler(N::NtUserGetCursorFrameInfo, svc_get_cursor_frame_info);
  register_handler(N::NtUserSetCursor, svc_set_cursor);
  register_handler(N::NtUserGetCursor, svc_get_cursor);
  register_handler(N::NtUserShowCursor, svc_show_cursor);
  register_handler(N::NtUserSetSystemCursor, svc_set_system_cursor);
  // These call into the window manager's cursor functions, which take the system lock themselves: the handlers take it only where they touch their own state.
  register_handler(N::NtUserGetCursorInfo, svc_get_cursor_info, false);
  if (!handler_of(uint32_t(N::NtUserGetCursorPos))) register_handler(N::NtUserGetCursorPos, svc_get_cursor_pos, false);      // (the window manager has its own when both modules are installed)
  if (!handler_of(uint32_t(N::NtUserSetCursorPos))) register_handler(N::NtUserSetCursorPos, svc_set_cursor_pos, false);      // (the window manager has its own when both modules are installed)
  if (!handler_of(uint32_t(N::NtUserClipCursor))) register_handler(N::NtUserClipCursor, svc_clip_cursor, false);      // (the window manager has its own when both modules are installed)
  if (!handler_of(uint32_t(N::NtUserGetClipCursor))) register_handler(N::NtUserGetClipCursor, svc_get_clip_cursor, false);      // (the window manager has its own when both modules are installed)
}

}  // namespace process::win32k::user_misc
