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

// The clipboard. The data of every format is kept as bytes (the user-mode side keeps the handles: it passes the handle it was given along, and gets it back). The text formats
// are synthesized from each other (CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT, CF_LOCALE); the metafile and bitmap families are reported as available the way Windows does, but their
// data is only converted by user mode. Delayed rendering: a format set without data is requested from the owner with WM_RENDERFORMAT when someone asks for it, and the asker
// waits (the system lock given up) until the owner has set it. Ownership, the open state, the sequence number, the viewer chain and the format listeners follow Windows.
//
// Service conventions follow Wine's win32u: SetClipboardData returns an NTSTATUS (0 or a failure; the error of GetLastError is set as well), GetClipboardData returns the handle
// the data was set with (1 if it was set without one, 0 if the format is not there) and fills the get_clipboard_params block.
namespace process::win32k::user_misc {

namespace {

constexpr uint32_t kCfText = 1, kCfBitmap = 2, kCfMetafilePict = 3, kCfOemText = 7, kCfDib = 8, kCfUnicodeText = 13, kCfEnhMetafile = 14, kCfLocale = 16, kCfDibV5 = 17;
constexpr uint64_t kMaxClipboardBytes = 64ull << 20;
constexpr uint32_t kDefaultLocale = 0x0409;

struct Format {
  uint32_t id;
  bool has_data = false;
  uint64_t handle = 0;
  ByteBuffer data;
  Format(allocators::IAllocator& a, uint32_t format) : id(format), data(a) {}
};

struct ClipboardState {
  Hive<Format> store;
  List<Format*> formats;
  List<uint32_t> listeners;
  uint32_t sequence = 1, sequence_at_open = 0;
  uint64_t open_thread = 0, open_pid = 0;
  uint32_t open_window = 0, owner_window = 0, viewer = 0;
  uint64_t owner_pid = 0;
  uint32_t rendering_format = 0;      // a format the owner has been asked for and may set although it does not have the clipboard open
  uint64_t render_timeout_ns = 5ull * 1000 * 1000 * 1000;
  explicit ClipboardState(allocators::IAllocator& a) : store(a), formats(a), listeners(a) {}
};
UniquePointer<ClipboardState> g_clipboard;
ClipboardState* st() { return lazy_state(g_clipboard); }

Format* find_format(ClipboardState& s, uint32_t id) {
  for (Format* f : s.formats.view()) if (f->id == id) return f;
  return nullptr;
}

void clear_formats(ClipboardState& s) {
  for (Format* f : s.formats.view()) s.store.erase(f);
  s.formats.clear();
}

// The cp1252 characters of 0x80..0x9F (0 = the code point is the byte).
constexpr char16_t kCp1252[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019,
                                  0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
char16_t ansi_to_wide(uint8_t b) { return b >= 0x80 && b < 0xA0 && kCp1252[b - 0x80] ? kCp1252[b - 0x80] : char16_t(b); }
uint8_t wide_to_ansi(char16_t ch) {
  if (ch < 0x80 || (ch >= 0xA0 && ch < 0x100)) return uint8_t(ch);
  for (uint32_t i = 0; i < 32; ++i) if (kCp1252[i] == ch) return uint8_t(0x80 + i);
  return '?';
}

bool is_text(uint32_t id) { return id == kCfText || id == kCfOemText || id == kCfUnicodeText; }

// Which formats are available: the ones set, then the synthesized ones in the order Windows lists them (text group with CF_LOCALE first, then the metafiles, then the bitmaps).
void available_formats(ClipboardState& s, List<uint32_t>* out) {
  for (Format* f : s.formats.view()) out->push_back(f->id);
  auto has = [&](uint32_t id) { return find_format(s, id) != nullptr; };
  if (has(kCfText) || has(kCfOemText) || has(kCfUnicodeText)) {
    if (!has(kCfLocale)) out->push_back(kCfLocale);
    static const uint32_t text_formats[] = {kCfText, kCfOemText, kCfUnicodeText};
    for (uint32_t id : text_formats) if (!has(id)) out->push_back(id);
  }
  if (has(kCfMetafilePict) != has(kCfEnhMetafile)) out->push_back(has(kCfMetafilePict) ? kCfEnhMetafile : kCfMetafilePict);
  static const uint32_t bitmap_formats[] = {kCfBitmap, kCfDib, kCfDibV5};
  if (has(kCfBitmap) || has(kCfDib) || has(kCfDibV5)) for (uint32_t id : bitmap_formats) if (!has(id)) out->push_back(id);
}

bool is_available(ClipboardState& s, uint32_t id) {
  List<uint32_t> list(*system_allocator());
  available_formats(s, &list);
  for (uint32_t f : list.view()) if (f == id) return true;
  return false;
}

// The bytes of a text format converted from another text format that has data. False if there is no source with data.
bool synthesize_text(ClipboardState& s, uint32_t id, ByteBuffer* out) {
  uint32_t order[2] = {0, 0};
  if (id == kCfUnicodeText) { order[0] = kCfText; order[1] = kCfOemText; }
  else if (id == kCfText) { order[0] = kCfUnicodeText; order[1] = kCfOemText; }
  else { order[0] = kCfUnicodeText; order[1] = kCfText; }
  for (uint32_t source_id : order) {
    Format* source = find_format(s, source_id);
    if (!source || !source->has_data) continue;
    ConstByteView bytes = source->data.view();
    if (source_id == kCfUnicodeText) {
      for (uint64_t i = 0; i + 1 < bytes.size(); i += 2) {
        const char16_t ch = char16_t(bytes[i] | bytes[i + 1] << 8);
        if (ch == 0) break;
        if (id == kCfUnicodeText) { out->put16(ch); } else { out->put8(wide_to_ansi(ch)); }
      }
    } else {
      for (uint64_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == 0) break;
        if (id == kCfUnicodeText) out->put16(ansi_to_wide(bytes[i])); else out->put8(bytes[i]);
      }
    }
    return id == kCfUnicodeText ? out->put16(0) : out->put8(0);
  }
  return false;
}

// The data of a format, set or synthesized, into `out`. `*found`: the format is available (with data or not); the return value: there is data.
bool format_data(ClipboardState& s, uint32_t id, ByteBuffer* out, bool* found, uint64_t* handle) {
  *found = false;
  *handle = 0;
  if (Format* f = find_format(s, id)) {
    *found = true;
    *handle = f->handle;
    if (!f->has_data) return false;
    return out->append(f->data.view());
  }
  if (!is_available(s, id)) return false;
  *found = true;
  *handle = 1;
  if (id == kCfLocale) return out->put32(kDefaultLocale);
  if (is_text(id)) return synthesize_text(s, id, out);
  return false;      // metafile / bitmap conversions are done by user mode
}

void notify_change(ClipboardState& s) {
  for (uint32_t window : s.listeners.view()) user::post_message(window, kWmClipboardUpdate, 0, 0);
  if (s.viewer != 0) user::post_message(s.viewer, kWmDrawClipboard, 0, 0);
}

void close_open_state(ClipboardState& s, bool notify) {
  const bool changed = s.sequence != s.sequence_at_open;
  s.open_thread = 0;
  s.open_pid = 0;
  s.open_window = 0;
  if (notify && changed) notify_change(s);
}

bool opened_by(ClipboardState& s, Call& c) { return s.open_thread != 0 && s.open_thread == c.tid(); }

// Does the caller set the format the owner was asked to render?
bool is_rendering(ClipboardState& s, Call& c, uint32_t format) {
  if (s.rendering_format != format || s.owner_window == 0) return false;
  return user::window_process(s.owner_window) == c.pid() && user::window_thread(s.owner_window) == c.tid();
}

// --- services
uint64_t svc_open(Call& c) {
  ClipboardState* s = st();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  if (hwnd != 0 && !user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  if (s->open_thread != 0 && s->open_thread != c.tid()) return c.fail(kErrorAccessDenied);
  if (s->open_thread == 0) s->sequence_at_open = s->sequence;
  s->open_thread = c.tid();
  s->open_pid = c.pid();
  s->open_window = hwnd;
  return 1;
}

uint64_t svc_close(Call& c) {
  ClipboardState* s = st();
  if (!s || !opened_by(*s, c)) return c.fail(kErrorClipboardNotOpen);
  close_open_state(*s, true);
  return 1;
}

uint64_t svc_empty(Call& c) {
  ClipboardState* s = st();
  if (!s || !opened_by(*s, c)) return c.fail(kErrorClipboardNotOpen);
  if (s->owner_window != 0) user::post_message(s->owner_window, kWmDestroyClipboard, 0, 0);
  clear_formats(*s);
  s->owner_window = s->open_window;
  s->owner_pid = s->open_pid;
  ++s->sequence;
  return 1;
}

uint64_t not_open_status(Call& c) {
  c.set_last_error(kErrorClipboardNotOpen);
  return kStatusInvalidDeviceState;
}

uint64_t svc_set_data(Call& c) {
  ClipboardState* s = st();
  if (!s) return kStatusUnsuccessful;
  const uint32_t format = c.arg32(0);
  const uint64_t handle = c.arg(1);
  if (format == 0) { c.set_last_error(kErrorInvalidParameter); return kStatusInvalidParameter; }
  if (!opened_by(*s, c) && !is_rendering(*s, c, format)) return not_open_status(c);
  uint8_t params[24];
  if (!c.read(c.arg(2), ByteView(params, 24))) { c.set_last_error(kErrorNoAccess); return kStatusAccessViolation; }
  uint64_t data_at = 0, size = 0;
  for (uint32_t i = 0; i < 8; ++i) { data_at |= uint64_t{params[i]} << (8 * i); size |= uint64_t{params[8 + i]} << (8 * i); }
  const bool cache_only = params[16] != 0;
  if (size > kMaxClipboardBytes) { c.set_last_error(kErrorNotEnoughMemory); return kStatusNoMemory; }
  if (cache_only) {      // only the user-mode handle of a format that is there is recorded
    if (Format* f = find_format(*s, format)) f->handle = handle;
    return kStatusSuccess;
  }
  const bool delayed = handle == 0 && data_at == 0;
  Format* f = find_format(*s, format);
  ByteBuffer data(*c.allocator());
  if (!delayed) {
    if (!data.put_zeros(size)) { c.set_last_error(kErrorNotEnoughMemory); return kStatusNoMemory; }
    if (size != 0 && !c.read(data_at, data.writable_view())) { c.set_last_error(kErrorNoAccess); return kStatusAccessViolation; }
  }
  if (!f) {
    f = s->store.emplace(*c.allocator(), format);
    if (!f) { c.set_last_error(kErrorNotEnoughMemory); return kStatusNoMemory; }
    if (!s->formats.push_back(f)) { s->store.erase(f); c.set_last_error(kErrorNotEnoughMemory); return kStatusNoMemory; }
  }
  f->data.clear();
  f->data.append(data.view());
  f->has_data = !delayed;
  f->handle = handle;
  ++s->sequence;
  const uint32_t seq = s->sequence;
  c.write_u32(c.arg(2) + 20, seq);
  wake_all();      // somebody may be waiting for this format to be rendered
  return kStatusSuccess;
}

// The caller wants a format that is only promised: the owner is asked to render it and the caller waits.
bool render_format(Call& c, ClipboardState& s, uint32_t format) {
  if (s.owner_window == 0 || !user::is_window(s.owner_window)) return false;
  s.rendering_format = format;
  user::post_message(s.owner_window, kWmRenderFormat, format, 0);
  struct Wait { ClipboardState* state; uint32_t format; } wait{&s, format};
  const bool done = wait_until(c, s.render_timeout_ns, [&] { Format* f = find_format(*wait.state, wait.format); return f == nullptr || f->has_data; });
  if (ClipboardState* again = g_clipboard.get()) again->rendering_format = 0;
  Format* f = g_clipboard ? find_format(*g_clipboard, format) : nullptr;
  return done && f && f->has_data;
}

uint64_t svc_get_data(Call& c) {
  ClipboardState* s = st();
  if (!s) return 0;
  const uint32_t format = c.arg32(0);
  if (!opened_by(*s, c)) return c.fail(kErrorClipboardNotOpen);
  uint8_t params[32];
  if (!c.read(c.arg(1), ByteView(params, 32))) return c.fail(kErrorNoAccess);
  uint64_t out_at = 0, capacity = 0;
  for (uint32_t i = 0; i < 8; ++i) { out_at |= uint64_t{params[i]} << (8 * i); capacity |= uint64_t{params[8 + i]} << (8 * i); }
  if (Format* f = find_format(*s, format)) {
    if (!f->has_data && !render_format(c, *s, format)) return 0;
  }
  ByteBuffer data(*c.allocator());
  bool found = false;
  uint64_t handle = 0;
  if (!is_available(*s, format)) return 0;
  const bool has = format_data(*s, format, &data, &found, &handle);
  if (!found) return 0;
  uint64_t size_out = has ? data.size() : 0;
  if (has && out_at != 0) {
    const uint64_t n = capacity < data.size() ? capacity : data.size();
    if (n != 0 && !c.write(out_at, data.view().first(n))) return c.fail(kErrorNoAccess);
  }
  if (!c.write_u64(c.arg(1) + 16, size_out) || !c.write_u32(c.arg(1) + 24, s->sequence)) return c.fail(kErrorNoAccess);
  return has ? (handle ? handle : 1) : 0;
}

uint64_t svc_enum_formats(Call& c) {
  ClipboardState* s = st();
  if (!s || !opened_by(*s, c)) return c.fail(kErrorClipboardNotOpen);
  List<uint32_t> list(*c.allocator());
  available_formats(*s, &list);
  const uint32_t format = c.arg32(0);
  c.set_last_error(kErrorSuccess);
  if (format == 0) return list.count() ? list[0] : 0;
  for (uint64_t i = 0; i < list.count(); ++i) if (list[i] == format) return i + 1 < list.count() ? list[i + 1] : 0;
  return 0;
}

uint64_t svc_is_available(Call& c) {
  ClipboardState* s = st();
  return s && is_available(*s, c.arg32(0)) ? 1 : 0;
}

uint64_t svc_count(Call&) {
  ClipboardState* s = st();
  if (!s) return 0;
  List<uint32_t> list(*system_allocator());
  available_formats(*s, &list);
  return list.count();
}

uint64_t svc_get_owner(Call&) { ClipboardState* s = st(); return s ? s->owner_window : 0; }
uint64_t svc_get_open_window(Call&) { ClipboardState* s = st(); return s ? s->open_window : 0; }
uint64_t svc_get_viewer(Call&) { ClipboardState* s = st(); return s ? s->viewer : 0; }
uint64_t svc_get_sequence(Call&) { ClipboardState* s = st(); return s ? s->sequence : 0; }

uint64_t svc_set_viewer(Call& c) {
  ClipboardState* s = st();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  const uint32_t previous = s->viewer;
  s->viewer = hwnd;
  user::post_message(hwnd, kWmDrawClipboard, 0, 0);
  return previous;
}

uint64_t svc_change_chain(Call& c) {
  ClipboardState* s = st();
  const uint32_t hwnd = c.arg32(0), next = c.arg32(1);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  if (s->viewer == hwnd) { s->viewer = next; return 1; }
  if (s->viewer != 0) user::post_message(s->viewer, kWmChangeCbChain, hwnd, next);      // the viewers pass it along the chain themselves
  return 1;
}

uint64_t svc_add_listener(Call& c) {
  ClipboardState* s = st();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  if (user::window_process(hwnd) != c.pid()) return c.fail(kErrorAccessDenied);
  for (uint32_t w : s->listeners.view()) if (w == hwnd) return 1;
  return s->listeners.push_back(hwnd) ? 1 : c.fail(kErrorNotEnoughMemory);
}

uint64_t svc_remove_listener(Call& c) {
  ClipboardState* s = st();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  for (uint64_t i = 0; i < s->listeners.count(); ++i) if (s->listeners[i] == hwnd) { s->listeners.remove_at(i); return 1; }
  return c.fail(kErrorInvalidParameter);
}

uint64_t svc_get_updated_formats(Call& c) {
  ClipboardState* s = st();
  if (!s) return 0;
  List<uint32_t> list(*c.allocator());
  available_formats(*s, &list);
  if (c.arg(2) == 0) return c.fail(kErrorInvalidParameter);
  if (!c.write_u32(c.arg(2), uint32_t(list.count()))) return c.fail(kErrorNoAccess);
  if (c.arg(0) == 0 || c.arg32(1) < list.count()) return c.fail(kErrorInsufficientBuffer);
  for (uint64_t i = 0; i < list.count(); ++i) if (!c.write_u32(c.arg(0) + i * 4, list[i])) return c.fail(kErrorNoAccess);
  return 1;
}

uint64_t svc_get_priority_format(Call& c) {
  ClipboardState* s = st();
  const int32_t count = c.argi(1);
  if (!s) return 0;
  if (count <= 0 || c.arg(0) == 0) return c.fail(kErrorInvalidParameter);
  List<uint32_t> list(*c.allocator());
  available_formats(*s, &list);
  if (list.count() == 0) return 0;
  for (int32_t i = 0; i < count; ++i) {
    uint32_t format = 0;
    if (!c.read_u32(c.arg(0) + uint64_t(i) * 4, &format)) return c.fail(kErrorNoAccess);
    if (is_available(*s, format)) return format;
  }
  return 0xFFFFFFFFu;
}

}  // namespace

void set_clipboard_render_timeout_ns(uint64_t ns) { if (ClipboardState* s = st()) s->render_timeout_ns = ns; }

uint32_t clipboard_sequence() { ClipboardState* s = st(); return s ? s->sequence : 0; }

bool clipboard_unicode_text(List<char16_t>* out) {
  ClipboardState* s = st();
  if (!s) return false;
  ByteBuffer data(*system_allocator());
  bool found = false;
  uint64_t handle = 0;
  if (!format_data(*s, kCfUnicodeText, &data, &found, &handle)) return false;
  ConstByteView bytes = data.view();
  for (uint64_t i = 0; i + 1 < bytes.size(); i += 2) {
    const char16_t ch = char16_t(bytes[i] | bytes[i + 1] << 8);
    if (ch == 0) break;
    out->push_back(ch);
  }
  return true;
}

bool clipboard_set_unicode_text(ArrayView<const char16_t> text) {
  ClipboardState* s = st();
  if (!s) return false;
  clear_formats(*s);
  Format* f = s->store.emplace(*system_allocator(), kCfUnicodeText);
  if (!f) return false;
  for (char16_t ch : text) f->data.put16(ch);
  f->data.put16(0);
  f->has_data = true;
  f->handle = 0;
  if (!s->formats.push_back(f)) { s->store.erase(f); return false; }
  s->owner_window = 0;
  ++s->sequence;
  notify_change(*s);
  return true;
}

void clipboard_window_destroyed(uint32_t hwnd) {
  ClipboardState* s = g_clipboard.get();
  if (!s) return;
  for (uint64_t i = 0; i < s->listeners.count();) { if (s->listeners[i] == hwnd) s->listeners.remove_at(i); else ++i; }
  if (s->viewer == hwnd) s->viewer = 0;
  if (s->open_window == hwnd) close_open_state(*s, false);
  if (s->owner_window == hwnd) {
    s->owner_window = 0;      // the promised formats can no longer be rendered
    for (uint64_t i = 0; i < s->formats.count();) {
      if (!s->formats[i]->has_data) { Format* f = s->formats[i]; s->formats.remove_at(i); s->store.erase(f); } else ++i;
    }
  }
}

void clipboard_process_exited(uint64_t pid) {
  ClipboardState* s = g_clipboard.get();
  if (!s) return;
  if (s->open_pid == pid) close_open_state(*s, false);
  if (s->owner_pid == pid && s->owner_window != 0) clipboard_window_destroyed(s->owner_window);
  if (s->owner_pid == pid) s->owner_window = 0;
}

void install_clipboard() {
  using N = WindowsSyscallNumber;
  register_handler(N::NtUserOpenClipboard, svc_open);
  register_handler(N::NtUserCloseClipboard, svc_close);
  register_handler(N::NtUserEmptyClipboard, svc_empty);
  register_handler(N::NtUserSetClipboardData, svc_set_data);
  register_handler(N::NtUserGetClipboardData, svc_get_data);
  register_handler(N::NtUserEnumClipboardFormats, svc_enum_formats);
  register_handler(N::NtUserIsClipboardFormatAvailable, svc_is_available);
  register_handler(N::NtUserCountClipboardFormats, svc_count);
  register_handler(N::NtUserGetClipboardOwner, svc_get_owner);
  register_handler(N::NtUserGetOpenClipboardWindow, svc_get_open_window);
  register_handler(N::NtUserGetClipboardViewer, svc_get_viewer);
  register_handler(N::NtUserGetClipboardSequenceNumber, svc_get_sequence);
  register_handler(N::NtUserSetClipboardViewer, svc_set_viewer);
  register_handler(N::NtUserChangeClipboardChain, svc_change_chain);
  register_handler(N::NtUserAddClipboardFormatListener, svc_add_listener);
  register_handler(N::NtUserRemoveClipboardFormatListener, svc_remove_listener);
  register_handler(N::NtUserGetUpdatedClipboardFormats, svc_get_updated_formats);
  register_handler(N::NtUserGetPriorityClipboardFormat, svc_get_priority_format);
}

}  // namespace process::win32k::user_misc
