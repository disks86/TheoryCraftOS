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

#include "Graphics/DeviceScale.hpp"
#include "Process/Win32k/Win32kDesktopBridge.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Process/Win32k/Win32kUser.hpp"
#include "Process/Win32k/Win32kUserMisc.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Utf16.hpp"

namespace process::win32k {

namespace {

// USB HID usages 0x04.. of the keyboard page and the virtual key / set-1 scan code / E0 prefix of each (US layout). 0: no key.
struct KeyRow { uint8_t vk, scan; bool extended; };
constexpr KeyRow kKeys[] = {
    /* 0x00 */ {0, 0, false}, {0, 0, false}, {0, 0, false}, {0, 0, false},
    /* 0x04 A..Z */ {'A', 0x1E, false}, {'B', 0x30, false}, {'C', 0x2E, false}, {'D', 0x20, false}, {'E', 0x12, false}, {'F', 0x21, false}, {'G', 0x22, false},
    {'H', 0x23, false}, {'I', 0x17, false}, {'J', 0x24, false}, {'K', 0x25, false}, {'L', 0x26, false}, {'M', 0x32, false}, {'N', 0x31, false}, {'O', 0x18, false},
    {'P', 0x19, false}, {'Q', 0x10, false}, {'R', 0x13, false}, {'S', 0x1F, false}, {'T', 0x14, false}, {'U', 0x16, false}, {'V', 0x2F, false}, {'W', 0x11, false},
    {'X', 0x2D, false}, {'Y', 0x15, false}, {'Z', 0x2C, false},
    /* 0x1E 1..9 0 */ {'1', 0x02, false}, {'2', 0x03, false}, {'3', 0x04, false}, {'4', 0x05, false}, {'5', 0x06, false}, {'6', 0x07, false}, {'7', 0x08, false},
    {'8', 0x09, false}, {'9', 0x0A, false}, {'0', 0x0B, false},
    /* 0x28 Enter, Escape, Backspace, Tab, Space */ {0x0D, 0x1C, false}, {0x1B, 0x01, false}, {0x08, 0x0E, false}, {0x09, 0x0F, false}, {0x20, 0x39, false},
    /* 0x2D - = [ ] \ (non-US #) ; ' ` , . / */ {0xBD, 0x0C, false}, {0xBB, 0x0D, false}, {0xDB, 0x1A, false}, {0xDD, 0x1B, false}, {0xDC, 0x2B, false},
    {0xDC, 0x2B, false}, {0xBA, 0x27, false}, {0xDE, 0x28, false}, {0xC0, 0x29, false}, {0xBC, 0x33, false}, {0xBE, 0x34, false}, {0xBF, 0x35, false},
    /* 0x39 Caps Lock */ {0x14, 0x3A, false},
    /* 0x3A F1..F12 */ {0x70, 0x3B, false}, {0x71, 0x3C, false}, {0x72, 0x3D, false}, {0x73, 0x3E, false}, {0x74, 0x3F, false}, {0x75, 0x40, false},
    {0x76, 0x41, false}, {0x77, 0x42, false}, {0x78, 0x43, false}, {0x79, 0x44, false}, {0x7A, 0x57, false}, {0x7B, 0x58, false},
    /* 0x46 Print Screen, Scroll Lock, Pause */ {0x2C, 0x37, true}, {0x91, 0x46, false}, {0x13, 0x45, false},
    /* 0x49 Insert, Home, Page Up, Delete, End, Page Down */ {0x2D, 0x52, true}, {0x24, 0x47, true}, {0x21, 0x49, true}, {0x2E, 0x53, true}, {0x23, 0x4F, true},
    {0x22, 0x51, true},
    /* 0x4F Right, Left, Down, Up */ {0x27, 0x4D, true}, {0x25, 0x4B, true}, {0x28, 0x50, true}, {0x26, 0x48, true},
    /* 0x53 Num Lock, keypad / * - + Enter */ {0x90, 0x45, false}, {0x6F, 0x35, true}, {0x6A, 0x37, false}, {0x6D, 0x4A, false}, {0x6B, 0x4E, false},
    {0x0D, 0x1C, true},
    /* 0x59 keypad 1..9 0 . */ {0x61, 0x4F, false}, {0x62, 0x50, false}, {0x63, 0x51, false}, {0x64, 0x4B, false}, {0x65, 0x4C, false}, {0x66, 0x4D, false},
    {0x67, 0x47, false}, {0x68, 0x48, false}, {0x69, 0x49, false}, {0x60, 0x52, false}, {0x6E, 0x53, false},
    /* 0x64 non-US \, Application */ {0xE2, 0x56, false}, {0x5D, 0x5D, true},
};
// 0xE0..0xE7: left Control, Shift, Alt, GUI, right Control, Shift, Alt, GUI
constexpr KeyRow kModifiers[] = {{0xA2, 0x1D, false}, {0xA0, 0x2A, false}, {0xA4, 0x38, false}, {0x5B, 0x5B, true},
                                 {0xA3, 0x1D, true}, {0xA1, 0x36, false}, {0xA5, 0x38, true}, {0x5C, 0x5C, true}};

}  // namespace

bool windows_key_of_usage(uint8_t usage, WindowsKey* out) {
  KeyRow row{0, 0, false};
  if (usage < sizeof kKeys / sizeof kKeys[0]) row = kKeys[usage];
  else if (usage >= 0x68 && usage <= 0x73) row = KeyRow{uint8_t(0x7C + (usage - 0x68)), uint8_t(0x64 + (usage - 0x68)), false};      // F13..F24
  else if (usage >= 0xE0 && usage <= 0xE7) row = kModifiers[usage - 0xE0];
  if (row.vk == 0) return false;
  out->vk = row.vk;
  out->scan = row.scan;
  out->extended = row.extended;
  return true;
}

namespace {

desktop::WindowsRect windows_rect(const gdi::Rect& r) {
  return {r.left, r.top, uint32_t(r.right > r.left ? r.right - r.left : 0), uint32_t(r.bottom > r.top ? r.bottom - r.top : 0)};
}

gdi::Rect gdi_rect(const desktop::WindowsRect& r) { return {r.x, r.y, int32_t(uint32_t(r.x) + r.width), int32_t(uint32_t(r.y) + r.height)}; }

}  // namespace

void DesktopBridge::attach(int32_t frame_border, int32_t frame_title) {
  {
    user_misc::SystemLock lock;
    gdi::set_window_surface_sink(this);
    attached_ = true;
  }
  frame_border_ = frame_border;
  frame_title_ = frame_title;
  user::set_redirection(true, this, graphics::units_to_device(frame_border, scale_), graphics::units_to_device(frame_title, scale_), frame_border, frame_title);      // (takes the lock itself)
}

// win32k's screen is in device pixels: the desktop's units times its scale (DPI virtualization gives unaware programs the units back).
void DesktopBridge::set_display(uint32_t width, uint32_t height, ArrayView<const Monitor> monitors) {
  const uint32_t scale = monitors.size() > 0 ? graphics::clamp_scale_percent(monitors[0].scale_percent) : 100;
  const bool rescaled = scale != scale_;
  scale_ = scale;
  // the device pixels past the area's last unit (400 rows at 150 % are 266 units, which map to 399): the far edge goes to the device's
  area_w_ = static_cast<int32_t>(width > 32767 ? 32767 : width);
  area_h_ = static_cast<int32_t>(height > 32767 ? 32767 : height);
  const int32_t mapped_w = graphics::units_to_device(area_w_, scale), mapped_h = graphics::units_to_device(area_h_, scale);
  const uint32_t device_w = monitors.size() > 0 ? monitors[0].device_width : 0, device_h = monitors.size() > 0 ? monitors[0].device_height : 0;
  spare_x_ = device_w > uint32_t(mapped_w) && device_w - uint32_t(mapped_w) < 8 ? static_cast<int32_t>(device_w) - mapped_w : 0;
  spare_y_ = device_h > uint32_t(mapped_h) && device_h - uint32_t(mapped_h) < 8 ? static_cast<int32_t>(device_h) - mapped_h : 0;
  if (rescaled && attached_)
    user::set_redirection(true, this, graphics::units_to_device(frame_border_, scale), graphics::units_to_device(frame_title_, scale), frame_border_, frame_title_);
  // where each top-level is in units now (through the monitors as they were): monitors at different scales before or after put them back there
  const bool was_mixed = mixed_;
  List<uint32_t> tops(*system_allocator());
  List<gdi::Rect> places(*system_allocator());
  {
    List<uint32_t> all(*system_allocator());
    user::top_levels(&all);
    for (uint32_t hwnd : all.view()) {
      gdi::Rect r;
      if (!user::window_screen_rect(hwnd, &r)) continue;
      tops.push_back(hwnd);
      places.push_back(down(r));
    }
  }
  // the monitors: their units, scales and place in win32k's pixels
  mon_count_ = 0;
  mixed_ = false;
  for (const Monitor& m : monitors) {
    if (mon_count_ == 8) break;
    Mon& e = mons_[mon_count_++];
    e.units = gdi_rect(m.rect);
    e.scale = graphics::clamp_scale_percent(m.monitor_scale != 0 ? m.monitor_scale : m.scale_percent);
    if (e.scale != scale) mixed_ = true;
  }
  int32_t right = 0, bottom = 0;
  for (uint32_t i = 0; i < mon_count_; ++i) {
    Mon& e = mons_[i];
    e.ox = mixed_ ? graphics::units_to_device(e.units.left, scale) : edge_up(e.units.left, false);
    e.oy = mixed_ ? graphics::units_to_device(e.units.top, scale) : edge_up(e.units.top, true);
    e.pixels = {e.ox, e.oy, px(e.units.right, &e, false), px(e.units.bottom, &e, true)};
    if (e.pixels.right > right) right = e.pixels.right;
    if (e.pixels.bottom > bottom) bottom = e.pixels.bottom;
  }
  gdi::Rect rects[8], works[8];
  uint32_t scales[8] = {};
  uint32_t n = 0;
  for (const Monitor& m : monitors) {
    if (n == 8) break;
    const Mon* e = mon(int32_t(n));
    const gdi::Rect u = gdi_rect(m.rect), wk = gdi_rect(m.work);
    rects[n] = {px(u.left, e, false), px(u.top, e, true), px(u.right, e, false), px(u.bottom, e, true)};
    works[n] = {px(wk.left, e, false), px(wk.top, e, true), px(wk.right, e, false), px(wk.bottom, e, true)};
    scales[n] = mixed_ ? e->scale : 0;
    ++n;
  }
  {
    user_misc::SystemLock lock;
    const uint64_t w = n > 0 ? uint64_t(right) : uint64_t(edge_up(area_w_, false)), h = n > 0 ? uint64_t(bottom) : uint64_t(edge_up(area_h_, true));
    if (w != gdi::screen_width() || h != gdi::screen_height()) gdi::set_screen_size(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    user_misc::set_monitors(ArrayView<const gdi::Rect>(rects, n), ArrayView<const gdi::Rect>(works, n), ArrayView<const uint32_t>(scales, mixed_ ? n : 0));
  }
  if (!was_mixed && !mixed_) {
    user::set_device_scale_from_desktop(scale);      // (one scale before and after: takes the lock; moves the windows when the scale changed)
  } else {
    // the system scale is the primary monitor's; every top-level goes back where it was in units, then follows its monitor's scale
    user::set_system_scale(n > 0 ? mons_[0].scale : scale);
    for (uint64_t i = 0; i < tops.count(); ++i) {
      const gdi::Rect to = up(places[i]);
      note_placed(tops[i], to);
      gdi::Rect now;
      if (user::window_screen_rect(tops[i], &now) && now.left == to.left && now.top == to.top && now.right == to.right && now.bottom == to.bottom)
        user::refresh_monitor_scale(tops[i], true);      // (not moving: no change will come, but its monitor's scale may be another now)
      else
        user::move_top_level(tops[i], to);
    }
  }
}

int32_t DesktopBridge::mon_at_units(int32_t x, int32_t y) const {
  if (mon_count_ == 0) return -1;
  int32_t best = 0;
  int64_t best_d = INT64_MAX;
  for (uint32_t i = 0; i < mon_count_; ++i) {
    const gdi::Rect& r = mons_[i].units;
    const int64_t dx = x < r.left ? int64_t(r.left) - x : x >= r.right ? int64_t(x) - r.right + 1 : 0;
    const int64_t dy = y < r.top ? int64_t(r.top) - y : y >= r.bottom ? int64_t(y) - r.bottom + 1 : 0;
    if (dx * dx + dy * dy < best_d) { best_d = dx * dx + dy * dy; best = int32_t(i); }
  }
  return best;
}

int32_t DesktopBridge::mon_at_pixels(int32_t x, int32_t y) const {
  if (mon_count_ == 0) return -1;
  int32_t best = 0;
  int64_t best_d = INT64_MAX;
  for (uint32_t i = 0; i < mon_count_; ++i) {
    const gdi::Rect& r = mons_[i].pixels;
    const int64_t dx = x < r.left ? int64_t(r.left) - x : x >= r.right ? int64_t(x) - r.right + 1 : 0;
    const int64_t dy = y < r.top ? int64_t(r.top) - y : y >= r.bottom ? int64_t(y) - r.bottom + 1 : 0;
    if (dx * dx + dy * dy < best_d) { best_d = dx * dx + dy * dy; best = int32_t(i); }
  }
  return best;
}

// Through monitor m: its top left in pixels plus the units past its top left at its scale (mapped by the same edges as the whole area would be, so
// one scale for all monitors gives exactly units_to_device and the far edge).
int32_t DesktopBridge::px(int32_t v, const Mon* m, bool vertical) const {
  if (!m || !mixed_) return edge_up(v, vertical);
  const int32_t origin = vertical ? m->units.top : m->units.left;
  const int64_t d = int64_t(vertical ? m->oy : m->ox) + graphics::units_to_device(v, m->scale) - graphics::units_to_device(origin, m->scale);
  return d > INT32_MAX ? INT32_MAX : d < INT32_MIN ? INT32_MIN : static_cast<int32_t>(d);
}

int32_t DesktopBridge::ux(int32_t v, const Mon* m, bool vertical) const {
  if (!m || !mixed_) return user::dpi_down_value(v, scale_);
  const int32_t origin = vertical ? m->units.top : m->units.left;
  const int64_t d = int64_t(v) - (vertical ? m->oy : m->ox) + graphics::units_to_device(origin, m->scale);
  return graphics::device_to_units(d > INT32_MAX ? INT32_MAX : d < INT32_MIN ? INT32_MIN : static_cast<int32_t>(d), m->scale);
}

void DesktopBridge::up_point(int32_t x, int32_t y, int32_t* ox, int32_t* oy) const {
  const Mon* m = mixed_ ? mon(mon_at_units(x, y)) : nullptr;
  *ox = m ? px(x, m, false) : graphics::units_to_device(x, scale_);
  *oy = m ? px(y, m, true) : graphics::units_to_device(y, scale_);
}

void DesktopBridge::down_point(int32_t x, int32_t y, int32_t* ox, int32_t* oy) const {
  const Mon* m = mixed_ ? mon(mon_at_pixels(x, y)) : nullptr;
  *ox = ux(x, m, false);
  *oy = ux(y, m, true);
}

int32_t DesktopBridge::edge_up(int32_t v, bool vertical) const {
  const int32_t d = graphics::units_to_device(v, scale_);
  const int32_t area = vertical ? area_h_ : area_w_, spare = vertical ? spare_y_ : spare_x_;
  return v >= area && spare > 0 && d <= INT32_MAX - spare ? d + spare : d;
}

gdi::Rect DesktopBridge::up(const gdi::Rect& r) const {
  const Mon* m = mixed_ ? mon(mon_at_units(r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2)) : nullptr;
  return {px(r.left, m, false), px(r.top, m, true), px(r.right, m, false), px(r.bottom, m, true)};
}

gdi::Rect DesktopBridge::down(const gdi::Rect& r) const {
  const Mon* m = mixed_ ? mon(mon_at_pixels(r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2)) : nullptr;
  return {ux(r.left, m, false), ux(r.top, m, true), ux(r.right, m, false), ux(r.bottom, m, true)};
}

void DesktopBridge::detach() {
  if (!attached_) return;
  user::set_redirection(false, nullptr, 0, 0);
  user_misc::SystemLock lock;
  gdi::set_window_surface_sink(nullptr);
  attached_ = false;
}

DesktopBridge::Pending* DesktopBridge::pending_slot(uint32_t window) {
  for (uint32_t i = 0; i < pending_count_; ++i) if (pending_[i].window == window) return &pending_[i];
  if (pending_count_ == kMaxPending) {
    overflow_ = true;
    return nullptr;
  }
  Pending* p = &pending_[pending_count_++];
  *p = Pending();
  p->window = window;
  return p;
}

void DesktopBridge::top_level_changed(uint32_t hwnd) {
  if (Pending* p = pending_slot(hwnd)) p->changed = true;
}

void DesktopBridge::surface_changed(uint32_t window0, const gdi::Rect& dirty0) {
  uint32_t window = window0;
  gdi::Rect dirty = dirty0;
  uint32_t top = 0;
  gdi::Rect top_dirty;
  if (user::mixed_damage_locked(window0, dirty0, &top, &top_dirty)) {      // (a mixed DPI child's own surface: what changed is its top-level's)
    window = top;
    dirty = top_dirty;
  }
  Pending* p = pending_slot(window);
  if (!p) return;
  if (!p->repaint) p->dirty = dirty;
  else {
    if (dirty.left < p->dirty.left) p->dirty.left = dirty.left;
    if (dirty.top < p->dirty.top) p->dirty.top = dirty.top;
    if (dirty.right > p->dirty.right) p->dirty.right = dirty.right;
    if (dirty.bottom > p->dirty.bottom) p->dirty.bottom = dirty.bottom;
  }
  p->repaint = true;
}

void DesktopBridge::take_changes(List<uint32_t>* changed, List<Repaint>* repainted) {
  changed->clear();
  repainted->clear();
  bool overflow = false;
  {
    user_misc::SystemLock lock;
    for (uint32_t i = 0; i < pending_count_; ++i) {
      const Pending& p = pending_[i];
      if (p.changed) changed->push_back(p.window);
      if (p.repaint) repainted->push_back(Repaint{p.window, windows_rect(p.dirty)});
    }
    pending_count_ = 0;
    overflow = overflow_;
    overflow_ = false;
  }
  // monitors at different scales: a top-level that went somewhere follows its monitor's scale (outside the lock: it may move the window)
  if (mixed_)
    for (uint32_t hwnd : changed->view()) {
      gdi::Rect r;
      if (user::window_screen_rect(hwnd, &r)) user::refresh_monitor_scale(hwnd, was_placed(hwnd, r));
    }
  if (!overflow) return;
  // the table was full: every window counts as changed and wholly repainted (a window that is gone was noted and is not visible: it is told apart by window_info)
  user::top_levels(changed);
  repainted->clear();
  for (uint32_t w : changed->view()) repainted->push_back(Repaint{w, {0, 0, 0xFFFFu, 0xFFFFu}});
}

void DesktopBridge::drag_requested(uint32_t hwnd, uint32_t edge) {
  drag_window_ = hwnd;
  drag_edge_ = edge;
}

uint32_t DesktopBridge::take_drag(uint32_t* edge) {
  user_misc::SystemLock lock;
  const uint32_t w = drag_window_;
  *edge = drag_edge_;
  drag_window_ = drag_edge_ = 0;
  return w;
}

void DesktopBridge::windows(List<uint32_t>* out) {
  user::top_levels(out);      // (topmost first: turned around)
  for (uint64_t i = 0, j = out->count(); i + 1 < j; ++i, --j) {
    const uint32_t t = (*out)[i];
    (*out)[i] = (*out)[j - 1];
    (*out)[j - 1] = t;
  }
}

bool DesktopBridge::window_info(uint32_t window, desktop::WindowsWindowInfo* out, String* title) {
  *out = desktop::WindowsWindowInfo();
  user::TopLevelInfo info;
  if (!title) return user::top_level_info(window, &info, nullptr) && fill_info(info, out);
  WideString wide(title->allocator());
  if (!user::top_level_info(window, &info, &wide) || !fill_info(info, out)) return false;
  title->clear();
  const ArrayView<const char16_t> text = wide.view();
  for (uint64_t i = 0; i < text.size(); ++i) {      // UTF-16 to UTF-8 (a lone surrogate is U+FFFD)
    char32_t c = text[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (text[++i] - 0xDC00);
    else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
    char bytes[4];
    uint32_t n = 0;
    if (c < 0x80) bytes[n++] = static_cast<char>(c);
    else if (c < 0x800) { bytes[n++] = static_cast<char>(0xC0 | c >> 6); bytes[n++] = static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { bytes[n++] = static_cast<char>(0xE0 | c >> 12); bytes[n++] = static_cast<char>(0x80 | (c >> 6 & 0x3F)); bytes[n++] = static_cast<char>(0x80 | (c & 0x3F)); }
    else { bytes[n++] = static_cast<char>(0xF0 | c >> 18); bytes[n++] = static_cast<char>(0x80 | (c >> 12 & 0x3F)); bytes[n++] = static_cast<char>(0x80 | (c >> 6 & 0x3F)); bytes[n++] = static_cast<char>(0x80 | (c & 0x3F)); }
    if (!title->append(StringView(bytes, n))) return false;
  }
  return true;
}

bool DesktopBridge::fill_info(const user::TopLevelInfo& info, desktop::WindowsWindowInfo* out) const {
  out->exists = info.exists;
  out->visible = info.visible;
  out->minimized = info.minimized;
  out->maximized = info.maximized;
  out->active = info.active;
  out->caption = info.caption;
  out->sizable = info.sizable;
  out->minimize_box = info.minimize_box;
  out->maximize_box = info.maximize_box;
  out->tool = info.tool;
  out->topmost = info.topmost;
  // A window without a caption that floats over another program's window (a menu, tooltip or drop-down list) is not activated from the desktop's side.
  out->popup = !info.caption && (info.no_activate || info.tool || (info.popup && info.owner != 0 && info.topmost));
  out->owned = info.owner != 0;
  out->icon = info.icon;
  out->window = windows_rect(down(info.window));      // (units: the device pixels over the scale)
  out->surface = windows_rect(down(info.surface));
  return true;
}

// The surface is drawn from where it lives, under the lock (no program draws on it meanwhile). Its pixels are B, G, R, X bytes, which is the desktop's own format
// (0x..RRGGBB words): Replace copies each row as it is.
// The surface of an unaware program's window is in units (drawn as it is); an aware one's is in device pixels, shrunk here to units (the desktop at scale
// 1, or a renderer that cannot draw on the device) or drawn 1:1 by draw_window_scaled. A system-aware program's, after the scale grew, is in pixels of
// the scale it started at: resampled to units here, or enlarged by the ratio on the device.
bool DesktopBridge::draw_window(uint32_t window, graphics::IRenderer2D& renderer, int32_t x, int32_t y, uint32_t* width, uint32_t* height) {
  user_misc::SystemLock lock;
  const uint32_t id = user::top_level_surface_locked(window);
  if (id == 0) return false;
  uint32_t w = 0, h = 0;
  const ArrayView<uint32_t> pixels = gdi::window_surface_pixels(id, &w, &h);
  if (pixels.empty() || pixels.size() < uint64_t{w} * h) return false;
  graphics::Surface s;
  s.pixels = pixels;
  s.width = s.pitch = w;
  s.height = h;
  const uint32_t k = user::top_level_surface_scale_locked(window);
  const uint32_t km = user::top_level_monitor_scale_locked(window);      // (win32k's pixels per unit where the window is: its monitor's scale)
  const bool resample = k != km;      // (a surface in units is drawn as it is)
  const uint32_t uw = resample ? uint32_t(graphics::device_to_units(graphics::units_to_device(int32_t(w), k), km)) : w;
  const uint32_t uh = resample ? uint32_t(graphics::device_to_units(graphics::units_to_device(int32_t(h), k), km)) : h;
  renderer.set_blend(graphics::BlendMode::Replace);
  if (resample) renderer.draw_image_scaled(s, {0, 0, float(w), float(h)}, {x, y, uw, uh}, graphics::ImageFilter::Bilinear);
  else renderer.draw_image(s, {0, 0, w, h}, x, y);
  renderer.set_blend(graphics::BlendMode::Alpha);
  *width = uw;
  *height = uh;
  return true;
}

// A surface whose pixels are the device's at this scale (pixels per unit: its monitor's scale over its own scale) goes on that device 1:1; any
// other is left to draw_window (through units: enlarged or shrunk for the device).
bool DesktopBridge::draw_window_scaled(uint32_t window, graphics::IRenderer2D& device, int32_t x, int32_t y, uint32_t scale, uint32_t* width, uint32_t* height) {
  user_misc::SystemLock lock;
  if (scale <= 100 && !mixed_) return false;
  const uint32_t k = user::top_level_surface_scale_locked(window), km = user::top_level_monitor_scale_locked(window);
  if (k == 0 || km * 100 / k != scale || km * 100 % k != 0 || scale == 100) return false;
  const uint32_t id = user::top_level_surface_locked(window);
  if (id == 0) return false;
  uint32_t w = 0, h = 0;
  const ArrayView<uint32_t> pixels = gdi::window_surface_pixels(id, &w, &h);
  if (pixels.empty() || pixels.size() < uint64_t{w} * h) return false;
  graphics::Surface s;
  s.pixels = pixels;
  s.width = s.pitch = w;
  s.height = h;
  device.set_blend(graphics::BlendMode::Replace);
  device.draw_image(s, {0, 0, w, h}, x, y);
  device.set_blend(graphics::BlendMode::Alpha);
  *width = uint32_t(graphics::device_to_units(int32_t(w), scale));
  *height = uint32_t(graphics::device_to_units(int32_t(h), scale));
  return true;
}

void DesktopBridge::move_window(uint32_t window, const desktop::WindowsRect& rect) {
  const gdi::Rect to = up(gdi_rect(rect));
  note_placed(window, to);
  user::move_top_level(window, to);
}

void DesktopBridge::note_placed(uint32_t hwnd, const gdi::Rect& r) {
  for (Placement& p : placed_) if (p.hwnd == hwnd) { p.rect = r; return; }
  placed_[placed_next_] = Placement{hwnd, r};
  placed_next_ = (placed_next_ + 1) % 32;
}

bool DesktopBridge::was_placed(uint32_t hwnd, const gdi::Rect& r) const {
  for (const Placement& p : placed_)
    if (p.hwnd == hwnd) return p.rect.left == r.left && p.rect.top == r.top && p.rect.right == r.right && p.rect.bottom == r.bottom;
  return false;
}
void DesktopBridge::activate_window(uint32_t window) { user::activate_top_level(window); }

void DesktopBridge::window_command(uint32_t window, Command command) {
  constexpr uint32_t kSc[] = {0xF060 /* SC_CLOSE */, 0xF020 /* SC_MINIMIZE */, 0xF030 /* SC_MAXIMIZE */, 0xF120 /* SC_RESTORE */};
  user::command_top_level(window, kSc[static_cast<uint8_t>(command)]);
}

// A key press types a character too (win32k's TranslateMessage leaves that to the input path): the US layout's, Ctrl+letter its control character.
void DesktopBridge::key(uint8_t hid_usage, bool pressed, uint8_t modifiers) {
  WindowsKey k;
  if (!windows_key_of_usage(hid_usage, &k)) return;
  user::inject_key(k.vk, k.scan, pressed, k.extended);
  if (!pressed) return;
  char c = hardware::key_to_ascii(hid_usage, modifiers);
  const bool control = (modifiers & hardware::modifier::kControl) != 0;
  if (control && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c = static_cast<char>(c & 0x1F);
  else if (control) c = 0;
  if (c != 0) user::inject_char(char32_t(static_cast<uint8_t>(c)));
}

void DesktopBridge::mouse_motion(int32_t dx, int32_t dy) {      // (at the scale of the monitor the cursor is on)
  int32_t cx = 0, cy = 0;
  user::cursor_position(&cx, &cy);
  const Mon* m = mixed_ ? mon(mon_at_pixels(cx, cy)) : nullptr;
  const uint32_t k = m ? m->scale : scale_;
  user::inject_mouse_motion(graphics::units_to_device(dx, k), graphics::units_to_device(dy, k));
}
void DesktopBridge::mouse_position(int32_t x, int32_t y) {
  int32_t px_x = 0, px_y = 0;
  up_point(x, y, &px_x, &px_y);
  user::inject_mouse_move(px_x, px_y);
}
void DesktopBridge::mouse_button(uint32_t button, bool down) { user::inject_mouse_button(button, down); }
void DesktopBridge::mouse_wheel(int32_t notches) { user::inject_mouse_wheel(notches * 120, false); }      // (WHEEL_DELTA per notch)
void DesktopBridge::cursor(int32_t* x, int32_t* y) {
  user::cursor_position(x, y);
  const int32_t cx = *x, cy = *y;
  down_point(cx, cy, x, y);
}
bool DesktopBridge::cursor_confined() { return user::cursor_confined(); }
uint32_t DesktopBridge::capture_window() { return user::mouse_capture_window(); }

void DesktopBridge::pointer(Pointer* out) {
  user::PointerCursor c;
  user::pointer_cursor(&c);
  out->hidden = c.hidden;
  out->cursor = c.cursor;
  out->system_id = c.system_id;
}

bool DesktopBridge::cursor_picture(uint32_t cursor, uint32_t* system_id, List<uint32_t>* argb, uint32_t* width, uint32_t* height, int32_t* hot_x, int32_t* hot_y) {
  return user::cursor_picture(cursor, system_id, argb, width, height, hot_x, hot_y);
}

bool DesktopBridge::touch(hardware::TouchPhase phase, uint32_t id, uint32_t window, int32_t x, int32_t y) {
  int32_t px_x = 0, px_y = 0;
  up_point(x, y, &px_x, &px_y);
  return user::touch_input(window, static_cast<uint8_t>(phase), id, px_x, px_y);
}

bool DesktopBridge::desktop_drag(uint32_t window, int32_t x, int32_t y, StringView text) {
  drop_ok_ = false;
  if (window == 0 || text.empty() || text[0] != '/') return false;
  allocators::IAllocator* a = system_allocator();
  if (!a) return false;
  if (!drop_names_) {
    drop_names_ = UniquePointer<WideString>::make(*a, *a);
    if (!drop_names_) return false;
  }
  String name(*a);
  ByteBuffer units(*a);
  if (!filesystem::layout::windows_view_of(text, &name) || !util::append_utf16le(&units, name.view())) return false;
  drop_names_->clear();
  ByteReader in(units.view());
  while (in.remaining() >= 2) {
    const char16_t c = char16_t(in.le16());
    if (c == 0 || !drop_names_->append(c)) return false;      // (a NUL would end the name early)
  }
  if (!drop_names_->append(char16_t(0)) || !drop_names_->append(char16_t(0))) return false;
  int32_t px_x = 0, px_y = 0;
  up_point(x, y, &px_x, &px_y);
  if (!user::drop_target(window, px_x, px_y, &drop_target_)) return false;
  drop_ok_ = true;
  return true;
}

bool DesktopBridge::desktop_drop() {
  const bool ok = drop_ok_ && drop_names_ && user::drop_files(drop_target_, drop_names_->view());
  drop_ok_ = false;
  return ok;
}

void DesktopBridge::desktop_drag_end() { drop_ok_ = false; }

}  // namespace process::win32k
