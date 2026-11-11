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

// The X server's requests: connection setup, the request loop, and every request of the core protocol (plus BIG-REQUESTS). Drawing requests are in
// X11Drawing.cpp. Requests come from the client and are not trusted: every field is read through WireReader (bounds-checked), every count is checked
// against what the request holds before it is used, and every resource id is looked up and its kind checked.

#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"
#include "X11/X11Internal.hpp"

namespace x11 {

namespace {

constexpr uint8_t kBigRequestsOpcode = 128;
constexpr StringView kVendor = "TheoryCraftOS";
constexpr uint32_t kRelease = 12101000;

// The font every name opens: 8x16 cells.
constexpr int16_t kFontAscent = 12, kFontDescent = 4, kFontWidth = 8;
constexpr const char* kFontNames[] = {"fixed", "8x16", "cursor", "-misc-fixed-medium-r-normal--16-120-100-100-c-80-iso8859-1",
                                      "-misc-fixed-medium-r-normal--16-120-100-100-c-80-iso10646-1"};

uint32_t popcount(uint32_t v) {
  uint32_t n = 0;
  for (; v; v &= v - 1) ++n;
  return n;
}

// A STRING8 of a request: its bytes, or empty and !r.ok() when the request is shorter than it says.
ConstByteView string_of(WireReader& r, uint64_t length) { return r.take(length); }

// 16-bit colour channel from an 8-bit one, and back (TrueColor 8 bits per channel).
uint16_t channel16(uint32_t v8) { return static_cast<uint16_t>((v8 & 0xFF) * 257); }
uint32_t pixel_of_rgb(uint16_t r, uint16_t g, uint16_t b) { return (uint32_t{r} >> 8) << 16 | (uint32_t{g} >> 8) << 8 | (uint32_t{b} >> 8); }

void put_charinfo(WireWriter& w, int16_t left, int16_t right, int16_t width, int16_t ascent, int16_t descent) {
  w.i16(left);
  w.i16(right);
  w.i16(width);
  w.i16(ascent);
  w.i16(descent);
  w.u16(0);
}

}  // namespace

// ------------------------------------------------------------------ replies and errors

void Server::error(Client& c, uint8_t code, uint32_t value, uint8_t major, uint16_t minor) {
  Packet p;
  p.u8(0, 0);
  p.u8(1, code);
  p.u32(4, value);
  p.u16(8, minor);
  p.u8(10, major);
  send(c, p);
}

WireWriter Server::reply(Client& c, uint8_t data, uint32_t extra_words) {
  WireWriter w(c.out, c.big);
  w.u8(1);
  w.u8(data);
  w.u16(c.sequence);
  w.u32(extra_words);
  return w;
}

bool Server::new_id(Client& c, uint32_t id, uint8_t major) {
  const uint32_t base = uint32_t{c.index} << kIdBits;
  if ((id & ~((1u << kIdBits) - 1)) != base || resources_.contains(id)) {
    error(c, error::kIdChoice, id, major);
    return false;
  }
  return true;
}

Server::Window* Server::window_of(Client& c, uint32_t id, uint8_t major) {
  Window* w = find_window(id);
  if (!w) error(c, error::kWindow, id, major);
  return w;
}

bool Server::drawable_of(Client& c, uint32_t id, uint8_t major, Window** window, Pixmap** pixmap) {
  *window = nullptr;
  *pixmap = nullptr;
  Resource* r = resources_.find(id);
  if (r && r->kind == Kind::Window) *window = r->window;
  else if (r && r->kind == Kind::Pixmap) *pixmap = r->pixmap;
  else {
    error(c, error::kDrawable, id, major);
    return false;
  }
  return true;
}

Server::Gc* Server::gc_of(Client& c, uint32_t id, uint8_t major) {
  Resource* r = resources_.find(id);
  if (!r || r->kind != Kind::Gc) {
    error(c, error::kGContext, id, major);
    return nullptr;
  }
  return r->gc;
}

// ------------------------------------------------------------------ connection setup and the request loop

bool Server::setup(Client& c) {
  ConstByteView in = c.in.view();
  if (in.size() < 12) return false;
  if (in[0] != 'B' && in[0] != 'l') {      // not an X client: nothing can be said to it in an order it reads
    c.closing = true;
    return false;
  }
  c.big = in[0] == 'B';
  WireReader r(in, c.big);
  r.skip(2);
  const uint16_t major = r.u16();
  r.u16();
  const uint16_t name_length = r.u16(), data_length = r.u16();
  r.skip(2);
  const uint64_t need = 12 + pad4(name_length) + pad4(data_length);
  if (in.size() < need) return false;
  c.setup_done = true;
  WireWriter w(c.out, c.big);
  if (major != 11) {
    const StringView reason = "only protocol version 11 is supported";
    w.u8(0);
    w.u8(static_cast<uint8_t>(reason.size()));
    w.u16(11);
    w.u16(0);
    w.u16(static_cast<uint16_t>(pad4(reason.size()) / 4));
    w.bytes(reason.bytes());
    w.pad4();
    c.closing = true;
    c.sequence = 0;
    return true;      // (consumed; the caller drops the client once this is sent)
  }
  // (any authorization is accepted: the server is reached only from inside this machine)
  w.u8(1);
  w.u8(0);
  w.u16(11);
  w.u16(0);
  w.u16(0);      // length, patched below
  w.u32(kRelease);
  w.u32(uint32_t{c.index} << kIdBits);
  w.u32((1u << kIdBits) - 1);
  w.u32(0);                                   // motion buffer
  w.u16(static_cast<uint16_t>(kVendor.size()));
  w.u16(0xFFFF);                              // maximum request length (words)
  w.u8(1);                                    // screens
  w.u8(4);                                    // pixmap formats
  w.u8(0);                                    // image byte order: LSBFirst
  w.u8(0);                                    // bitmap bit order: LeastSignificant
  w.u8(32);                                   // bitmap scanline unit
  w.u8(32);                                   // bitmap scanline pad
  w.u8(kMinKeycode);
  w.u8(kMaxKeycode);
  w.zeros(4);
  w.bytes(kVendor.bytes());
  w.pad4();
  constexpr uint8_t kFormats[4][2] = {{1, 1}, {8, 8}, {24, 32}, {32, 32}};      // depth, bits per pixel (scanline pad 32)
  for (const auto& f : kFormats) {
    w.u8(f[0]);
    w.u8(f[1]);
    w.u8(32);
    w.zeros(5);
  }
  // the screen
  w.u32(kRootWindow);
  w.u32(kDefaultColormap);
  w.u32(0xFFFFFF);                            // white pixel
  w.u32(0);                                   // black pixel
  uint32_t masks = 0;
  for (const Selection& s : root_->selections.view()) masks |= s.mask;
  w.u32(masks);
  w.u16(static_cast<uint16_t>(screen_w_));
  w.u16(static_cast<uint16_t>(screen_h_));
  w.u16(static_cast<uint16_t>(screen_w_ * 254 / 960));      // millimetres at 96 dots per inch
  w.u16(static_cast<uint16_t>(screen_h_ * 254 / 960));
  w.u16(1);
  w.u16(1);
  w.u32(kVisual24);
  w.u8(0);                                    // backing stores: Never
  w.u8(0);                                    // save unders
  w.u8(24);                                   // root depth
  w.u8(4);                                    // allowed depths
  auto visual = [&](uint32_t id, uint8_t depth) {
    w.u8(depth);
    w.u8(0);
    w.u16(1);
    w.zeros(4);
    w.u32(id);
    w.u8(4);                                  // TrueColor
    w.u8(8);                                  // bits per RGB value
    w.u16(256);                               // colormap entries
    w.u32(0xFF0000);
    w.u32(0x00FF00);
    w.u32(0x0000FF);
    w.zeros(4);
  };
  visual(kVisual24, 24);
  constexpr uint8_t kBareDepths[2] = {1, 8};      // (pixmaps only: no visuals)
  for (uint8_t depth : kBareDepths) {
    w.u8(depth);
    w.u8(0);
    w.u16(0);
    w.zeros(4);
  }
  visual(kVisual32, 32);
  w.patch_u16(6, static_cast<uint16_t>((w.written() - 8) / 4));
  if (!w.ok()) c.closing = true;
  c.sequence = 0;
  return true;
}

void Server::process(Client& c) {
  uint64_t used = 0;
  if (!c.setup_done) {
    if (!setup(c)) return;
    const ConstByteView in = c.in.view();
    WireReader r(in, c.big);
    r.skip(6);
    const uint16_t name_length = r.u16(), data_length = r.u16();
    used = 12 + pad4(name_length) + pad4(data_length);
  }
  while (!c.closing) {
    const ConstByteView in = c.in.view();
    if (used > in.size()) break;
    const ConstByteView rest = in.subview(used);
    if (rest.size() < 4) break;
    WireReader h(rest, c.big);
    const uint8_t opcode = h.u8(), data = h.u8();
    uint64_t words = h.u16();
    uint64_t header = 4;
    if (words == 0) {
      if (!c.big_requests) {      // (a length of 0 without BIG-REQUESTS: the request cannot be found the end of)
        ++c.sequence;
        error(c, error::kLength, 0, opcode);
        c.closing = true;
        break;
      }
      if (rest.size() < 8) break;
      words = h.u32();
      header = 8;
      if (words < 2) {
        ++c.sequence;
        error(c, error::kLength, 0, opcode);
        c.closing = true;
        break;
      }
    }
    if (words > kMaxRequestBytes / 4) {      // more than this server takes: it cannot be skipped without reading it all, so the client goes
      c.closing = true;
      break;
    }
    const uint64_t bytes = words * 4;
    if (rest.size() < bytes) break;
    ++c.sequence;
    WireReader r(rest.subview(header, bytes - header), c.big);
    dispatch(c, opcode, data, r, bytes - header);
    used += bytes;
    if (c.out.size() > kMaxOutputBytes) c.closing = true;
  }
  // keep the part of a request that has not all come yet
  const uint64_t size = c.in.size();
  if (used >= size) {
    c.in.clear();
  } else if (used > 0) {
    ByteView all = c.in.writable_view();
    for (uint64_t i = used; i < size; ++i) all[i - used] = all[i];
    c.in.truncate(size - used);
  }
}

// ------------------------------------------------------------------ windows

void Server::create_window(Client& c, uint8_t depth, WireReader& r) {
  const uint32_t wid = r.u32(), parent_id = r.u32();
  const int16_t x = r.i16(), y = r.i16();
  const uint16_t width = r.u16(), height = r.u16(), border = r.u16(), klass_in = r.u16();
  uint32_t visual = r.u32();
  const uint32_t value_mask = r.u32();
  if (!r.ok() || r.remaining() < uint64_t{popcount(value_mask & 0x7FFF)} * 4) {
    error(c, error::kLength, 0, 1);
    return;
  }
  Window* parent = window_of(c, parent_id, 1);
  if (!parent || !new_id(c, wid, 1)) return;
  if (width == 0 || height == 0) {
    error(c, error::kValue, 0, 1);
    return;
  }
  uint16_t klass = klass_in == 0 ? (parent->input_only ? 2 : 1) : klass_in;
  if (klass > 2) {
    error(c, error::kValue, klass, 1);
    return;
  }
  const bool input_only = klass == 2;
  if (input_only) {
    if (border != 0 || depth != 0) {
      error(c, error::kMatch, 0, 1);
      return;
    }
    if (visual == 0) visual = parent->visual;
  } else {
    if (parent->input_only) {
      error(c, error::kMatch, 0, 1);
      return;
    }
    if (depth == 0) depth = parent->depth;
    if (visual == 0) visual = parent->visual;
    if (!((depth == 24 && visual == kVisual24) || (depth == 32 && visual == kVisual32))) {
      error(c, error::kMatch, 0, 1);
      return;
    }
  }
  Window* w = windows_.emplace(allocator_);
  if (!w || !parent->children.push_back(w)) {
    if (w) windows_.erase(w);
    error(c, error::kAlloc, 0, 1);
    return;
  }
  w->id = wid;
  w->owner = c.index;
  w->parent = parent;
  w->x = x;
  w->y = y;
  w->width = width;
  w->height = height;
  w->border = border;
  w->input_only = input_only;
  w->depth = input_only ? 0 : depth;
  w->visual = visual;
  w->background = 0;      // None
  w->border_pixel = parent->border_pixel;
  w->colormap = parent->colormap;
  w->placed = parent != root_;
  Resource res;
  res.kind = Kind::Window;
  res.owner = c.index;
  res.window = w;
  if (!resources_.put(wid, res)) {
    parent->children.remove_at(parent->children.count() - 1);
    windows_.erase(w);
    error(c, error::kAlloc, 0, 1);
    return;
  }
  change_window_attributes(c, *w, value_mask, r, 1);
  Packet p;
  p.u8(0, event::kCreateNotify);
  p.u32(4, parent->id);
  p.u32(8, wid);
  p.i16(12, x);
  p.i16(14, y);
  p.u16(16, width);
  p.u16(18, height);
  p.u16(20, border);
  p.u8(22, w->override_redirect ? 1 : 0);
  deliver(*parent, mask::kSubstructureNotify, p);
}

void Server::change_window_attributes(Client& c, Window& w, uint32_t value_mask, WireReader& r, uint8_t major) {
  if (r.remaining() < uint64_t{popcount(value_mask & 0x7FFF)} * 4) {
    error(c, error::kLength, 0, major);
    return;
  }
  for (uint32_t bit = 0; bit < 15; ++bit) {
    if (!(value_mask & (1u << bit))) continue;
    const uint32_t v = r.u32();
    switch (bit) {
      case 0:      // background-pixmap
        if (v == 0) {
          w.background = 0;
          w.background_picture = SharedPointer<Picture>();
        } else if (v == 1) {
          w.background = 3;
        } else {
          Resource* p = resources_.find(v);
          if (!p || p->kind != Kind::Pixmap) { error(c, error::kPixmap, v, major); return; }
          if (p->pixmap->picture->depth != w.depth) { error(c, error::kMatch, v, major); return; }
          w.background = 2;
          w.background_picture = p->pixmap->picture;
        }
        break;
      case 1:
        w.background = 1;
        w.background_pixel = stored_pixel(w.depth, v);
        w.background_picture = SharedPointer<Picture>();
        break;
      case 2: break;      // border-pixmap (borders are drawn with the border pixel)
      case 3: w.border_pixel = stored_pixel(w.depth, v); break;
      case 4: w.bit_gravity = static_cast<uint8_t>(v); break;
      case 5: w.win_gravity = static_cast<uint8_t>(v); break;
      case 9: w.override_redirect = (v & 1) != 0; break;
      case 11: {      // event-mask: this client's selection on the window
        const uint32_t m = v & mask::kAll;
        if (&w == root_ && (m & (mask::kSubstructureRedirect | mask::kResizeRedirect))) {      // (the window manager is this server)
          error(c, error::kAccess, w.id, major);
          return;
        }
        constexpr uint32_t kExclusive = mask::kSubstructureRedirect | mask::kResizeRedirect | mask::kButtonPress;
        for (const Selection& s : w.selections.view()) {
          if (s.client != c.index && (s.mask & m & kExclusive)) {
            error(c, error::kAccess, w.id, major);
            return;
          }
        }
        bool found = false;
        for (uint64_t i = 0; i < w.selections.count(); ++i) {
          if (w.selections[i].client != c.index) continue;
          found = true;
          if (m == 0) w.selections.remove_at(i);
          else w.selections[i].mask = m;
          break;
        }
        if (!found && m != 0) w.selections.push_back({c.index, m});
        break;
      }
      case 12: w.do_not_propagate = v & mask::kAll; break;
      case 13:
        if (v != 0) {
          Resource* m = resources_.find(v);
          if (!m || m->kind != Kind::Colormap) { error(c, error::kColormap, v, major); return; }
        }
        w.colormap = v == 0 && w.parent ? w.parent->colormap : v;
        break;
      case 14:
        if (v != 0) {
          Resource* k = resources_.find(v);
          if (!k || k->kind != Kind::Cursor) { error(c, error::kCursor, v, major); return; }
        }
        w.cursor = v;
        break;
      default: break;      // backing-store, backing-planes, backing-pixel, save-under: there is no backing store
    }
  }
}

void Server::get_window_attributes(Client& c, Window& w) {
  WireWriter o = reply(c, 0, 3);
  o.u32(w.visual);
  o.u16(w.input_only ? 2 : 1);
  o.u8(w.bit_gravity);
  o.u8(w.win_gravity);
  o.u32(0xFFFFFFFFu);
  o.u32(0);
  o.u8(0);
  o.u8(1);
  o.u8(!w.mapped ? 0 : viewable(&w) ? 2 : 1);
  o.u8(w.override_redirect ? 1 : 0);
  o.u32(w.input_only ? 0 : w.colormap);
  uint32_t all = 0;
  for (const Selection& s : w.selections.view()) all |= s.mask;
  o.u32(all);
  o.u32(event_mask_of(w, c.index));
  o.u16(static_cast<uint16_t>(w.do_not_propagate));
  o.u16(0);
}

void Server::configure_window(Client& c, Window& w, WireReader& r) {
  const uint16_t value_mask = r.u16();
  r.skip(2);
  if (!r.ok() || r.remaining() < uint64_t{popcount(value_mask & 0x7F)} * 4) {
    error(c, error::kLength, 0, 12);
    return;
  }
  int32_t x = w.x, y = w.y;
  uint32_t width = w.width, height = w.height, border = w.border, sibling_id = 0, stack_mode = 0;
  const bool top_level = w.parent == root_ && w.placed && !w.input_only;
  int32_t l = 0, t = 0, rr = 0, b = 0;
  if (top_level) frame_margins(w, &l, &t, &rr, &b);
  for (uint32_t bit = 0; bit < 7; ++bit) {
    if (!(value_mask & (1u << bit))) continue;
    const uint32_t v = r.u32();
    switch (bit) {
      case 0: x = static_cast<int16_t>(v); if (top_level && w.win_gravity != 10) x += l; break;
      case 1: y = static_cast<int16_t>(v); if (top_level && w.win_gravity != 10) y += t; break;
      case 2: width = v & 0xFFFF; break;
      case 3: height = v & 0xFFFF; break;
      case 4: border = v & 0xFFFF; break;
      case 5: sibling_id = v; break;
      case 6: stack_mode = v & 0xFF; break;
    }
  }
  if (((value_mask & 4) && width == 0) || ((value_mask & 8) && height == 0) || stack_mode > 4) {
    error(c, error::kValue, 0, 12);
    return;
  }
  if (&w == root_) return;
  Window* sibling = nullptr;
  if (value_mask & 0x20) {
    if (!(value_mask & 0x40)) { error(c, error::kMatch, 0, 12); return; }
    sibling = window_of(c, sibling_id, 12);
    if (!sibling) return;
    if (sibling->parent != w.parent || sibling == &w) { error(c, error::kMatch, sibling_id, 12); return; }
  }
  if (w.input_only && (value_mask & 0x10) && border != 0) { error(c, error::kMatch, 0, 12); return; }
  if (top_level && (w.maximized || w.fullscreen)) {      // (the window manager keeps a maximized window where it is)
    x = w.x;
    y = w.y;
    width = w.width;
    height = w.height;
  }
  move_resize(w, x, y, width, height, border, false);
  if (value_mask & 0x40) restack(w, sibling, stack_mode);
  send_configure_notify(w);
}

void Server::reparent_window(Client& c, Window& w, Window& parent, int32_t x, int32_t y) {
  if (&w == root_ || is_ancestor(&w, &parent) || (parent.input_only && !w.input_only)) {
    error(c, error::kMatch, w.id, 7);
    return;
  }
  const bool was_mapped = w.mapped;
  if (was_mapped) unmap_window(w, false);
  Window* old_parent = w.parent;
  if (old_parent) {
    for (uint64_t i = 0; i < old_parent->children.count(); ++i)
      if (old_parent->children[i] == &w) { old_parent->children.remove_at(i); break; }
    if (old_parent == root_) {
      note_changed(w);
      w.picture = SharedPointer<Picture>();
      update_root_lists();
    }
  }
  if (!parent.children.push_back(&w)) {
    error(c, error::kAlloc, 0, 7);
    if (old_parent) old_parent->children.push_back(&w);
    return;
  }
  w.parent = &parent;
  w.x = x;
  w.y = y;
  w.placed = &parent != root_;
  Packet p;
  p.u8(0, event::kReparentNotify);
  p.u32(8, w.id);
  p.u32(12, parent.id);
  p.i16(16, x);
  p.i16(18, y);
  p.u8(20, w.override_redirect ? 1 : 0);
  p.u32(4, w.id);
  deliver(w, mask::kStructureNotify, p);
  if (old_parent) {
    p.u32(4, old_parent->id);
    deliver(*old_parent, mask::kSubstructureNotify, p);
  }
  p.u32(4, parent.id);
  deliver(parent, mask::kSubstructureNotify, p);
  if (was_mapped) map_window(w);
}

// ------------------------------------------------------------------ properties

void Server::change_property(Client& c, uint8_t mode, WireReader& r) {
  const uint32_t window = r.u32(), property = r.u32(), type = r.u32();
  const uint8_t format = r.u8();
  r.skip(3);
  const uint32_t units = r.u32();
  if (!r.ok()) { error(c, error::kLength, 0, 18); return; }
  if (format != 8 && format != 16 && format != 32) { error(c, error::kValue, format, 18); return; }
  if (mode > 2) { error(c, error::kValue, mode, 18); return; }
  const uint64_t bytes = uint64_t{units} * (format / 8);
  if (bytes > r.remaining()) { error(c, error::kLength, 0, 18); return; }
  Window* w = window_of(c, window, 18);
  if (!w) return;
  if (atom_name(property).empty()) { error(c, error::kAtom, property, 18); return; }
  if (atom_name(type).empty()) { error(c, error::kAtom, type, 18); return; }
  ConstByteView raw = r.take(bytes);
  // the units in the server's order (little-endian)
  ByteBuffer value(allocator_);
  if (format == 8 || !c.big) {
    if (!value.append(raw)) { error(c, error::kAlloc, 0, 18); return; }
  } else {
    WireReader in(raw, true);
    for (uint64_t i = 0; i < units; ++i) {
      if (format == 16) value.put16(in.u16());
      else value.put32(in.u32());
    }
  }
  Property* p = find_property(*w, property);
  if (p && mode != 0) {
    if (p->type != type || p->format != format) { error(c, error::kMatch, 0, 18); return; }
    if (p->data.size() + value.size() > kMaxPropertyBytes) { error(c, error::kAlloc, 0, 18); return; }
    if (mode == 2) {
      if (!p->data.append(value.view())) { error(c, error::kAlloc, 0, 18); return; }
    } else {
      ByteBuffer joined(allocator_);
      if (!joined.append(value.view()) || !joined.append(p->data.view())) { error(c, error::kAlloc, 0, 18); return; }
      p->data.clear();
      p->data.append(joined.view());
    }
    property_changed(*w, property, false);
    return;
  }
  if (value.size() > kMaxPropertyBytes) { error(c, error::kAlloc, 0, 18); return; }
  set_property(*w, property, type, format, value.view());
}

void Server::get_property(Client& c, bool remove, WireReader& r) {
  const uint32_t window = r.u32(), property = r.u32(), type = r.u32(), long_offset = r.u32(), long_length = r.u32();
  if (!r.ok()) { error(c, error::kLength, 0, 20); return; }
  Window* w = window_of(c, window, 20);
  if (!w) return;
  if (atom_name(property).empty()) { error(c, error::kAtom, property, 20); return; }
  if (type != 0 && atom_name(type).empty()) { error(c, error::kAtom, type, 20); return; }
  Property* p = find_property(*w, property);
  if (!p) {
    WireWriter o = reply(c, 0, 0);
    o.u32(0);
    o.u32(0);
    o.u32(0);
    o.zeros(12);
    return;
  }
  const uint64_t size = p->data.size();
  if (type != 0 && type != p->type) {
    WireWriter o = reply(c, p->format, 0);
    o.u32(p->type);
    o.u32(static_cast<uint32_t>(size));
    o.u32(0);
    o.zeros(12);
    return;
  }
  const uint64_t start = uint64_t{long_offset} * 4;
  if (start > size) { error(c, error::kValue, long_offset, 20); return; }
  uint64_t length = size - start;
  if (length > uint64_t{long_length} * 4) length = uint64_t{long_length} * 4;
  const uint32_t unit = p->format / 8;
  length -= length % unit;
  const uint64_t after = size - start - length;
  WireWriter o = reply(c, p->format, static_cast<uint32_t>(pad4(length) / 4));
  o.u32(p->type);
  o.u32(static_cast<uint32_t>(after));
  o.u32(static_cast<uint32_t>(length / unit));
  o.zeros(12);
  ConstByteView value = p->data.view().subview(start, length);
  if (unit == 1) {
    o.bytes(value);
  } else {
    ByteReader in(value);
    for (uint64_t i = 0; i < length / unit; ++i) {
      if (unit == 2) o.u16(in.le16());
      else o.u32(in.le32());
    }
  }
  o.pad4();
  if (remove && after == 0) delete_property(*w, property);
}

// ------------------------------------------------------------------ graphics contexts

void Server::change_gc(Client& c, Gc& gc, uint32_t value_mask, WireReader& r, uint8_t major) {
  if (r.remaining() < uint64_t{popcount(value_mask & 0x7FFFFF)} * 4) {
    error(c, error::kLength, 0, major);
    return;
  }
  for (uint32_t bit = 0; bit < 23; ++bit) {
    if (!(value_mask & (1u << bit))) continue;
    const uint32_t v = r.u32();
    auto picture_of = [&](uint32_t id, uint8_t want_depth, SharedPointer<Picture>* out) {
      Resource* p = resources_.find(id);
      if (!p || p->kind != Kind::Pixmap) { error(c, error::kPixmap, id, major); return false; }
      if (want_depth && p->pixmap->picture->depth != want_depth) { error(c, error::kMatch, id, major); return false; }
      *out = p->pixmap->picture;
      return true;
    };
    switch (bit) {
      case 0: if (v > 15) { error(c, error::kValue, v, major); return; } gc.function = static_cast<uint8_t>(v); break;
      case 1: gc.plane_mask = v; break;
      case 2: gc.foreground = v; break;
      case 3: gc.background = v; break;
      case 4: gc.line_width = v & 0xFFFF; break;
      case 5: gc.line_style = static_cast<uint8_t>(v); break;
      case 6: gc.cap_style = static_cast<uint8_t>(v); break;
      case 7: gc.join_style = static_cast<uint8_t>(v); break;
      case 8: if (v > 3) { error(c, error::kValue, v, major); return; } gc.fill_style = static_cast<uint8_t>(v); break;
      case 9: gc.fill_rule = static_cast<uint8_t>(v & 1); break;
      case 10: if (!picture_of(v, gc.depth, &gc.tile)) return; break;
      case 11: if (!picture_of(v, 1, &gc.stipple)) return; break;
      case 12: gc.ts_x = static_cast<int16_t>(v); break;
      case 13: gc.ts_y = static_cast<int16_t>(v); break;
      case 14: {
        Resource* f = resources_.find(v);
        if (!f || f->kind != Kind::Font) { error(c, error::kFont, v, major); return; }
        gc.font = v;
        break;
      }
      case 15: gc.subwindow_mode = static_cast<uint8_t>(v & 1); break;
      case 16: gc.graphics_exposures = (v & 1) != 0; break;
      case 17: gc.clip_x = static_cast<int16_t>(v); break;
      case 18: gc.clip_y = static_cast<int16_t>(v); break;
      case 19:
        gc.clip_rects.reset();
        if (v == 0) gc.clip_mask = SharedPointer<Picture>();
        else if (!picture_of(v, 1, &gc.clip_mask)) return;
        break;
      case 22: gc.arc_mode = static_cast<uint8_t>(v & 1); break;
      default: break;      // dash-offset, dashes: lines are solid
    }
  }
}

// ------------------------------------------------------------------ events

namespace {

// Where the 16- and 32-bit fields of a core event are (offset, 2 or 4), for SendEvent: the event comes in the sender's order and goes out in each receiver's.
void event_layout(uint8_t code, uint8_t format, uint32_t* wide16, uint32_t* wide32) {
  uint32_t w16 = 1u << 2, w32 = 0;      // the sequence number
  auto f16 = [&](uint32_t at) { w16 |= 1u << at; };
  auto f32 = [&](uint32_t at) { w32 |= 1u << at; };
  switch (code) {
    case event::kKeyPress: case event::kKeyRelease: case event::kButtonPress: case event::kButtonRelease: case event::kMotionNotify:
    case event::kEnterNotify: case event::kLeaveNotify:
      f32(4); f32(8); f32(12); f32(16); f16(20); f16(22); f16(24); f16(26); f16(28); break;
    case event::kFocusIn: case event::kFocusOut: f32(4); break;
    case event::kKeymapNotify: w16 = 0; break;
    case event::kExpose: f32(4); f16(8); f16(10); f16(12); f16(14); f16(16); break;
    case event::kGraphicsExpose: f32(4); f16(8); f16(10); f16(12); f16(14); f16(16); f16(18); break;
    case event::kNoExpose: f32(4); f16(8); break;
    case event::kVisibilityNotify: f32(4); break;
    case event::kCreateNotify: f32(4); f32(8); f16(12); f16(14); f16(16); f16(18); f16(20); break;
    case event::kDestroyNotify: case event::kUnmapNotify: case event::kMapNotify: case event::kMapRequest: f32(4); f32(8); break;
    case event::kReparentNotify: f32(4); f32(8); f32(12); f16(16); f16(18); break;
    case event::kConfigureNotify: f32(4); f32(8); f32(12); f16(16); f16(18); f16(20); f16(22); f16(24); break;
    case event::kConfigureRequest: f32(4); f32(8); f32(12); f16(16); f16(18); f16(20); f16(22); f16(24); f16(26); break;
    case event::kGravityNotify: f32(4); f32(8); f16(12); f16(14); break;
    case event::kResizeRequest: f32(4); f16(8); f16(10); break;
    case event::kCirculateNotify: case event::kCirculateRequest: f32(4); f32(8); f32(12); break;
    case event::kPropertyNotify: f32(4); f32(8); f32(12); break;
    case event::kSelectionClear: f32(4); f32(8); f32(12); break;
    case event::kSelectionRequest: f32(4); f32(8); f32(12); f32(16); f32(20); f32(24); break;
    case event::kSelectionNotify: f32(4); f32(8); f32(12); f32(16); f32(20); break;
    case event::kColormapNotify: f32(4); f32(8); break;
    case event::kClientMessage:
      f32(4); f32(8);
      if (format == 16) for (uint32_t i = 12; i < 32; i += 2) f16(i);
      else if (format == 32) for (uint32_t i = 12; i < 32; i += 4) f32(i);
      break;
    case event::kMappingNotify: break;
    default: break;      // an extension's event: passed as it is
  }
  *wide16 = w16;
  *wide32 = w32;
}

}  // namespace

void Server::send_event_request(Client& c, bool propagate, WireReader& r) {
  const uint32_t destination = r.u32(), event_mask = r.u32();
  ConstByteView raw = r.take(32);
  if (!r.ok()) { error(c, error::kLength, 0, 25); return; }
  const uint8_t code = raw[0] & 0x7F;
  if (code < 2 || code > 127) { error(c, error::kValue, code, 25); return; }
  // the event in the canonical order
  Packet p;
  event_layout(code, raw[1], &p.wide16, &p.wide32);
  for (uint32_t i = 0; i < 32; ++i) {
    if (p.wide32 & (1u << i)) {
      WireReader in(raw.subview(i, 4), c.big);
      const uint32_t v = in.u32();
      for (uint32_t k = 0; k < 4; ++k) p.bytes[i + k] = static_cast<uint8_t>(v >> (8 * k));
      i += 3;
    } else if (p.wide16 & (1u << i)) {
      WireReader in(raw.subview(i, 2), c.big);
      const uint16_t v = in.u16();
      p.bytes[i] = static_cast<uint8_t>(v);
      p.bytes[i + 1] = static_cast<uint8_t>(v >> 8);
      i += 1;
    } else {
      p.bytes[i] = raw[i];
    }
  }
  p.bytes[0] = static_cast<uint8_t>(code | 0x80);
  Window* target = nullptr;
  if (destination == 0) {
    target = find_window(pointer_window_);
  } else if (destination == 1) {
    target = focus_ > 1 ? find_window(focus_) : focus_ == 1 ? find_window(pointer_window_) : nullptr;
    if (focus_ > 1 && target) {      // the pointer's window when it is inside the focus
      Window* under = find_window(pointer_window_);
      if (under && is_ancestor(target, under)) target = under;
    }
  } else {
    target = window_of(c, destination, 25);
    if (!target) return;
  }
  if (!target) return;
  // a message to the root that only a window manager would act on: this server is it
  if (code == event::kClientMessage && target == root_ && (event_mask & (mask::kSubstructureRedirect | mask::kSubstructureNotify))) {
    uint32_t data[5];
    for (uint32_t i = 0; i < 5; ++i) data[i] = p.get32(12 + 4 * i);
    window_manager_message(p.get32(4), p.get32(8), data);
  }
  if (event_mask == 0) {
    if (target->owner != 0 && target->owner < kMaxClients && clients_[target->owner] && clients_[target->owner]->setup_done)
      send(*clients_[target->owner], p, code != event::kKeymapNotify);
    return;
  }
  for (Window* w = target; w;) {
    bool any = false;
    for (const Selection& s : w->selections.view()) {
      if (!(s.mask & event_mask)) continue;
      any = true;
      if (s.client < kMaxClients && clients_[s.client] && clients_[s.client]->setup_done) send(*clients_[s.client], p, code != event::kKeymapNotify);
    }
    if (any || !propagate || (w->do_not_propagate & event_mask)) break;
    w = w->parent;
  }
}

// ------------------------------------------------------------------ fonts

void Server::query_font(Client& c, bool with_infos, StringView name, bool last) {
  // QueryFont (with_infos: the per-character metrics) or one reply of ListFontsWithInfo (name; last: the closing one)
  constexpr uint32_t kChars = 256;
  if (last) {
    WireWriter o = reply(c, 0, 7);
    o.zeros(52);
    return;
  }
  const uint32_t extra = with_infos ? 7 + 3 * kChars : 7 + static_cast<uint32_t>(pad4(name.size()) / 4);
  WireWriter o = reply(c, with_infos ? 0 : static_cast<uint8_t>(name.size()), extra);
  put_charinfo(o, 0, kFontWidth, kFontWidth, kFontAscent, kFontDescent);      // min bounds
  o.zeros(4);
  put_charinfo(o, 0, kFontWidth, kFontWidth, kFontAscent, kFontDescent);      // max bounds
  o.zeros(4);
  o.u16(0);                  // min char
  o.u16(kChars - 1);         // max char
  o.u16('?');                // default char
  o.u16(0);                  // properties
  o.u8(0);                   // LeftToRight
  o.u8(0);                   // min byte1
  o.u8(0);                   // max byte1
  o.u8(1);                   // all chars exist
  o.i16(kFontAscent);
  o.i16(kFontDescent);
  if (with_infos) {
    o.u32(kChars);
    for (uint32_t i = 0; i < kChars; ++i) put_charinfo(o, 0, kFontWidth, kFontWidth, kFontAscent, kFontDescent);
  } else {
    o.u32(1);                // replies hint
    o.bytes(name.bytes());
    o.pad4();
  }
}

// ------------------------------------------------------------------ cursors

void Server::create_cursor(Client& c, WireReader& r) {
  const uint32_t cid = r.u32(), source = r.u32(), mask_id = r.u32();
  const uint16_t fr = r.u16(), fg = r.u16(), fb = r.u16(), br = r.u16(), bg = r.u16(), bb = r.u16();
  const uint16_t hx = r.u16(), hy = r.u16();
  if (!r.ok()) { error(c, error::kLength, 0, 93); return; }
  if (!new_id(c, cid, 93)) return;
  Resource* s = resources_.find(source);
  if (!s || s->kind != Kind::Pixmap) { error(c, error::kPixmap, source, 93); return; }
  const Picture& src = *s->pixmap->picture;
  if (src.depth != 1) { error(c, error::kMatch, source, 93); return; }
  const Picture* m = nullptr;
  if (mask_id != 0) {
    Resource* mr = resources_.find(mask_id);
    if (!mr || mr->kind != Kind::Pixmap) { error(c, error::kPixmap, mask_id, 93); return; }
    m = mr->pixmap->picture.get();
    if (m->depth != 1 || m->width != src.width || m->height != src.height) { error(c, error::kMatch, mask_id, 93); return; }
  }
  if (hx >= src.width || hy >= src.height) { error(c, error::kMatch, 0, 93); return; }
  if (src.width > 256 || src.height > 256) { error(c, error::kAlloc, 0, 93); return; }
  Cursor* k = cursors_.emplace(allocator_);
  if (!k || !k->argb.reserve(uint64_t{src.width} * src.height)) {
    if (k) cursors_.erase(k);
    error(c, error::kAlloc, 0, 93);
    return;
  }
  const uint32_t fore = 0xFF000000u | pixel_of_rgb(fr, fg, fb), back = 0xFF000000u | pixel_of_rgb(br, bg, bb);
  for (uint64_t i = 0; i < uint64_t{src.width} * src.height; ++i) {
    const bool shown = !m || m->pixels[i];
    k->argb.push_back(shown ? (src.pixels[i] ? fore : back) : 0);
  }
  k->id = cid;
  k->owner = c.index;
  k->width = src.width;
  k->height = src.height;
  k->hot_x = hx;
  k->hot_y = hy;
  Resource res;
  res.kind = Kind::Cursor;
  res.owner = c.index;
  res.cursor = k;
  if (!resources_.put(cid, res)) { cursors_.erase(k); error(c, error::kAlloc, 0, 93); }
}

void Server::create_glyph_cursor(Client& c, WireReader& r) {
  const uint32_t cid = r.u32(), source_font = r.u32(), mask_font = r.u32();
  const uint16_t source_char = r.u16(), mask_char = r.u16();
  const uint16_t fr = r.u16(), fg = r.u16(), fb = r.u16(), br = r.u16(), bg = r.u16(), bb = r.u16();
  if (!r.ok()) { error(c, error::kLength, 0, 94); return; }
  if (!new_id(c, cid, 94)) return;
  Resource* sf = resources_.find(source_font);
  if (!sf || sf->kind != Kind::Font) { error(c, error::kFont, source_font, 94); return; }
  if (mask_font != 0) {
    Resource* mf = resources_.find(mask_font);
    if (!mf || mf->kind != Kind::Font) { error(c, error::kFont, mask_font, 94); return; }
  }
  Cursor* k = cursors_.emplace(allocator_);
  if (!k) { error(c, error::kAlloc, 0, 94); return; }
  k->id = cid;
  k->owner = c.index;
  if (sf->font->cursor_font) {
    k->system_id = system_cursor_of_glyph(source_char);
  } else {      // a character of the text font as the picture (fore where it is set, back around it)
    uint8_t rows[hardware::Font8x16::kHeight];
    uint8_t mask_rows[hardware::Font8x16::kHeight];
    hardware::Font8x16::glyph_of(static_cast<char32_t>(source_char), rows);
    hardware::Font8x16::glyph_of(static_cast<char32_t>(mask_font ? mask_char : source_char), mask_rows);
    const uint32_t fore = 0xFF000000u | pixel_of_rgb(fr, fg, fb), back = 0xFF000000u | pixel_of_rgb(br, bg, bb);
    k->argb.reserve(8 * 16);
    for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 8; ++x) {
        const bool set = rows[y] & (0x80 >> x), shown = (mask_rows[y] | rows[y]) & (0x80 >> x);
        k->argb.push_back(shown ? (set ? fore : back) : 0);
      }
    k->width = 8;
    k->height = 16;
  }
  Resource res;
  res.kind = Kind::Cursor;
  res.owner = c.index;
  res.cursor = k;
  if (!resources_.put(cid, res)) { cursors_.erase(k); error(c, error::kAlloc, 0, 94); }
}

void Server::kill_client(Client& c, uint32_t resource) {
  if (resource == 0) return;      // AllTemporary: there are no retained clients
  Resource* r = resources_.find(resource);
  if (!r) { error(c, error::kValue, resource, 113); return; }
  const uint8_t owner = r->owner;
  if (owner == 0 || owner >= kMaxClients || !clients_[owner]) return;
  free_client_resources(owner);
  clients_[owner]->closing = true;
}

// ------------------------------------------------------------------ the dispatcher

void Server::dispatch(Client& c, uint8_t opcode, uint8_t data, WireReader& r, uint64_t length) {
  switch (opcode) {
    case 1: create_window(c, data, r); return;
    case 2: {
      Window* w = window_of(c, r.u32(), 2);
      const uint32_t m = r.u32();
      if (w) change_window_attributes(c, *w, m, r, 2);
      return;
    }
    case 3: if (Window* w = window_of(c, r.u32(), 3)) get_window_attributes(c, *w); return;
    case 4: if (Window* w = window_of(c, r.u32(), 4)) destroy_window(*w); return;
    case 5:
      if (Window* w = window_of(c, r.u32(), 5)) {
        while (w->children.count() > 0) {
          Window* child = w->children[w->children.count() - 1];
          if (child == wm_check_) break;
          destroy_window(*child);
        }
      }
      return;
    case 6: window_of(c, r.u32(), 6); return;      // ChangeSaveSet: no reparenting, nothing to save
    case 7: {
      Window* w = window_of(c, r.u32(), 7);
      Window* parent = w ? window_of(c, r.u32(), 7) : nullptr;
      const int16_t x = r.i16(), y = r.i16();
      if (w && parent) reparent_window(c, *w, *parent, x, y);
      return;
    }
    case 8: if (Window* w = window_of(c, r.u32(), 8)) map_window(*w); return;
    case 9:
      if (Window* w = window_of(c, r.u32(), 9)) {
        List<Window*> order(allocator_);
        for (uint64_t i = w->children.count(); i > 0; --i) order.push_back(w->children[i - 1]);
        for (Window* child : order.view()) if (find_window(child->id) == child && child != wm_check_) map_window(*child);
      }
      return;
    case 10: if (Window* w = window_of(c, r.u32(), 10)) unmap_window(*w, false); return;
    case 11:
      if (Window* w = window_of(c, r.u32(), 11)) {
        List<Window*> order(allocator_);
        for (Window* child : w->children.view()) order.push_back(child);
        for (Window* child : order.view()) if (find_window(child->id) == child) unmap_window(*child, false);
      }
      return;
    case 12: if (Window* w = window_of(c, r.u32(), 12)) configure_window(c, *w, r); return;
    case 13: {
      Window* w = window_of(c, r.u32(), 13);
      if (!w || data > 1) { if (w) error(c, error::kValue, data, 13); return; }
      Window* pick = nullptr;
      if (data == 0) { for (Window* ch : w->children.view()) if (ch->mapped) { pick = ch; break; } }
      else for (uint64_t i = w->children.count(); i > 0; --i) if (w->children[i - 1]->mapped) { pick = w->children[i - 1]; break; }
      if (!pick) return;
      restack(*pick, nullptr, data == 0 ? 0 : 1);
      Packet p;
      p.u8(0, event::kCirculateNotify);
      p.u32(8, pick->id);
      p.u8(16, data == 0 ? 0 : 1);
      structure_event(*pick, p, 4);
      return;
    }
    case 14: {
      Window* w;
      Pixmap* px;
      if (!drawable_of(c, r.u32(), 14, &w, &px)) return;
      WireWriter o = reply(c, w ? w->depth : px->picture->depth, 0);
      o.u32(kRootWindow);
      o.i16(static_cast<int16_t>(w ? w->x : 0));
      o.i16(static_cast<int16_t>(w ? w->y : 0));
      o.u16(static_cast<uint16_t>(w ? w->width : px->picture->width));
      o.u16(static_cast<uint16_t>(w ? w->height : px->picture->height));
      o.u16(static_cast<uint16_t>(w ? w->border : 0));
      o.zeros(10);
      return;
    }
    case 15: {
      Window* w = window_of(c, r.u32(), 15);
      if (!w) return;
      const uint32_t n = static_cast<uint32_t>(w->children.count());
      WireWriter o = reply(c, 0, n);
      o.u32(kRootWindow);
      o.u32(w->parent ? w->parent->id : 0);
      o.u16(static_cast<uint16_t>(n));
      o.zeros(14);
      for (Window* ch : w->children.view()) o.u32(ch->id);
      return;
    }
    case 16: {
      const uint16_t n = r.u16();
      r.skip(2);
      ConstByteView name = string_of(r, n);
      if (!r.ok()) { error(c, error::kLength, 0, 16); return; }
      uint32_t atom = 0;
      for (uint64_t i = 0; i < atoms_.count(); ++i) if (atoms_[i].view().equals(StringView(name))) atom = static_cast<uint32_t>(i + 1);
      if (atom == 0 && !data) {
        if (atoms_.count() >= 0x1FFFFFFF) { error(c, error::kAlloc, 0, 16); return; }
        atom = intern(StringView(name));
        if (atom == 0) { error(c, error::kAlloc, 0, 16); return; }
      }
      WireWriter o = reply(c, 0, 0);
      o.u32(atom);
      o.zeros(20);
      return;
    }
    case 17: {
      const uint32_t atom = r.u32();
      const StringView name = atom_name(atom);
      if (name.empty()) { error(c, error::kAtom, atom, 17); return; }
      WireWriter o = reply(c, 0, static_cast<uint32_t>(pad4(name.size()) / 4));
      o.u16(static_cast<uint16_t>(name.size()));
      o.zeros(22);
      o.bytes(name.bytes());
      o.pad4();
      return;
    }
    case 18: change_property(c, data, r); return;
    case 19: {
      Window* w = window_of(c, r.u32(), 19);
      const uint32_t atom = r.u32();
      if (!w) return;
      if (atom_name(atom).empty()) { error(c, error::kAtom, atom, 19); return; }
      delete_property(*w, atom);
      return;
    }
    case 20: get_property(c, data != 0, r); return;
    case 21: {
      Window* w = window_of(c, r.u32(), 21);
      if (!w) return;
      const uint32_t n = static_cast<uint32_t>(w->properties.count());
      WireWriter o = reply(c, 0, n);
      o.u16(static_cast<uint16_t>(n));
      o.zeros(22);
      for (const UniquePointer<Property>& p : w->properties.view()) o.u32(p->name);
      return;
    }
    case 22: {
      const uint32_t owner = r.u32(), selection = r.u32();
      uint32_t time = r.u32();
      Window* w = nullptr;
      if (owner != 0 && !(w = window_of(c, owner, 22))) return;
      if (atom_name(selection).empty()) { error(c, error::kAtom, selection, 22); return; }
      const uint32_t current = now();
      if (time == 0) time = current;
      SelectionOwner* entry = nullptr;
      for (SelectionOwner& s : selection_owners_.view()) if (s.selection == selection) entry = &s;
      if (entry && (time < entry->time || time > current)) return;      // (an older or a future time: ignored)
      if (entry && entry->window != 0 && entry->client != (w ? c.index : 0) && entry->client < kMaxClients && clients_[entry->client]) {
        Packet p;
        p.u8(0, event::kSelectionClear);
        p.u32(4, time);
        p.u32(8, entry->window);
        p.u32(12, selection);
        send(*clients_[entry->client], p);
      }
      if (!entry) {
        if (!selection_owners_.push_back(SelectionOwner{})) return;
        entry = &selection_owners_[selection_owners_.count() - 1];
      }
      entry->selection = selection;
      entry->window = w ? owner : 0;
      entry->client = w ? c.index : 0;
      entry->time = time;
      return;
    }
    case 23: {
      const uint32_t selection = r.u32();
      if (atom_name(selection).empty()) { error(c, error::kAtom, selection, 23); return; }
      uint32_t owner = 0;
      for (const SelectionOwner& s : selection_owners_.view()) if (s.selection == selection) owner = s.window;
      WireWriter o = reply(c, 0, 0);
      o.u32(owner);
      o.zeros(20);
      return;
    }
    case 24: {
      const uint32_t requestor = r.u32(), selection = r.u32(), target = r.u32(), property = r.u32(), time = r.u32();
      Window* w = window_of(c, requestor, 24);
      if (!w) return;
      if (atom_name(selection).empty() || atom_name(target).empty() || (property && atom_name(property).empty())) {
        error(c, error::kAtom, selection, 24);
        return;
      }
      const SelectionOwner* owner = nullptr;
      for (const SelectionOwner& s : selection_owners_.view()) if (s.selection == selection && s.window != 0) owner = &s;
      Packet p;
      if (owner && owner->client < kMaxClients && clients_[owner->client]) {
        p.u8(0, event::kSelectionRequest);
        p.u32(4, time);
        p.u32(8, owner->window);
        p.u32(12, requestor);
        p.u32(16, selection);
        p.u32(20, target);
        p.u32(24, property);
        send(*clients_[owner->client], p);
      } else {
        p.u8(0, event::kSelectionNotify);
        p.u32(4, time);
        p.u32(8, requestor);
        p.u32(12, selection);
        p.u32(16, target);
        p.u32(20, 0);
        if (w->owner < kMaxClients && clients_[w->owner]) send(*clients_[w->owner], p);
      }
      return;
    }
    case 25: send_event_request(c, data != 0, r); return;
    case 26: {      // GrabPointer
      const uint32_t window = r.u32();
      const uint16_t event_mask = r.u16();
      r.u8();
      r.u8();
      r.u32();      // confine-to
      const uint32_t cursor = r.u32();
      r.u32();      // time
      Window* w = window_of(c, window, 26);
      if (!w) return;
      uint8_t status = 0;
      if (pointer_grab_.active && pointer_grab_.client != c.index) status = 1;
      else if (!viewable(w)) status = 3;
      if (status == 0) {
        pointer_grab_.active = true;
        pointer_grab_.implicit = false;
        pointer_grab_.owner_events = data != 0;
        pointer_grab_.client = c.index;
        pointer_grab_.window = window;
        pointer_grab_.mask = event_mask;
        pointer_grab_.cursor = cursor;
      }
      WireWriter o = reply(c, status, 0);
      o.zeros(24);
      return;
    }
    case 27:
      if (pointer_grab_.active && pointer_grab_.client == c.index) {
        pointer_grab_ = Grab();
        pointer_moved();
      }
      return;
    case 28: case 29: case 33: case 34: case 35: case 36: case 37: return;      // passive grabs, AllowEvents, GrabServer: nothing is frozen
    case 30:
      if (pointer_grab_.active && pointer_grab_.client == c.index) {
        pointer_grab_.cursor = r.u32();
        r.u32();
        pointer_grab_.mask = r.u16();
      }
      return;
    case 31: {      // GrabKeyboard
      const uint32_t window = r.u32();
      Window* w = window_of(c, window, 31);
      if (!w) return;
      uint8_t status = 0;
      if (keyboard_grab_.active && keyboard_grab_.client != c.index) status = 1;
      else if (!viewable(w)) status = 3;
      if (status == 0) {
        keyboard_grab_.active = true;
        keyboard_grab_.owner_events = data != 0;
        keyboard_grab_.client = c.index;
        keyboard_grab_.window = window;
      }
      WireWriter o = reply(c, status, 0);
      o.zeros(24);
      return;
    }
    case 32: if (keyboard_grab_.active && keyboard_grab_.client == c.index) keyboard_grab_ = Grab(); return;
    case 38: {      // QueryPointer
      Window* w = window_of(c, r.u32(), 38);
      if (!w) return;
      int32_t ox, oy;
      inside_origin(w, &ox, &oy);
      uint32_t child = 0;
      for (Window* under = find_window(pointer_window_); under; under = under->parent)
        if (under->parent == w) { child = under->id; break; }
      WireWriter o = reply(c, 1, 0);
      o.u32(kRootWindow);
      o.u32(child);
      o.i16(static_cast<int16_t>(pointer_x_));
      o.i16(static_cast<int16_t>(pointer_y_));
      o.i16(static_cast<int16_t>(pointer_x_ - ox));
      o.i16(static_cast<int16_t>(pointer_y_ - oy));
      o.u16(key_state());
      o.zeros(6);
      return;
    }
    case 39: {
      if (!window_of(c, r.u32(), 39)) return;
      WireWriter o = reply(c, 0, 0);
      o.u32(0);
      o.zeros(20);
      return;
    }
    case 40: {      // TranslateCoordinates
      Window* src = window_of(c, r.u32(), 40);
      Window* dst = src ? window_of(c, r.u32(), 40) : nullptr;
      const int16_t x = r.i16(), y = r.i16();
      if (!src || !dst) return;
      int32_t sx, sy, dx, dy;
      inside_origin(src, &sx, &sy);
      inside_origin(dst, &dx, &dy);
      const int32_t tx = sx + x - dx, ty = sy + y - dy;
      uint32_t child = 0;
      for (uint64_t i = dst->children.count(); i > 0; --i) {
        const Window* ch = dst->children[i - 1];
        const Rect outer = dst == root_ ? Rect{ch->x, ch->y, static_cast<int32_t>(ch->width), static_cast<int32_t>(ch->height)}
                                        : Rect{ch->x, ch->y, static_cast<int32_t>(ch->width + 2 * ch->border), static_cast<int32_t>(ch->height + 2 * ch->border)};
        if (ch->mapped && contains(outer, tx, ty)) { child = ch->id; break; }
      }
      WireWriter o = reply(c, 1, 0);
      o.u32(child);
      o.i16(static_cast<int16_t>(tx));
      o.i16(static_cast<int16_t>(ty));
      o.zeros(16);
      return;
    }
    case 41: {      // WarpPointer
      const uint32_t src = r.u32(), dst = r.u32();
      r.skip(8);
      const int16_t dx = r.i16(), dy = r.i16();
      if (!r.ok()) { error(c, error::kLength, 0, 41); return; }
      if (src != 0 && !window_of(c, src, 41)) return;
      if (dst != 0) {
        Window* d = window_of(c, dst, 41);
        if (!d) return;
        int32_t ox, oy;
        inside_origin(d, &ox, &oy);
        pointer_x_ = ox + dx;
        pointer_y_ = oy + dy;
      } else {
        pointer_x_ += dx;
        pointer_y_ += dy;
      }
      pointer_x_ = max32(0, min32(pointer_x_, static_cast<int32_t>(screen_w_) - 1));
      pointer_y_ = max32(0, min32(pointer_y_, static_cast<int32_t>(screen_h_) - 1));
      pointer_moved();
      motion();
      return;
    }
    case 42: {      // SetInputFocus
      const uint32_t focus = r.u32();
      if (data > 2) { error(c, error::kValue, data, 42); return; }
      if (focus > 1) {
        Window* w = window_of(c, focus, 42);
        if (!w) return;
        if (!viewable(w)) { error(c, error::kMatch, focus, 42); return; }
      }
      set_focus(focus, data);
      return;
    }
    case 43: {
      WireWriter o = reply(c, focus_revert_, 0);
      o.u32(focus_);
      o.zeros(20);
      return;
    }
    case 44: {
      WireWriter o = reply(c, 0, 2);
      o.bytes(ConstByteView(keys_, 32));
      return;
    }
    case 45: {      // OpenFont
      const uint32_t fid = r.u32();
      const uint16_t n = r.u16();
      r.skip(2);
      ConstByteView name = string_of(r, n);
      if (!r.ok()) { error(c, error::kLength, 0, 45); return; }
      if (!new_id(c, fid, 45)) return;
      Font* f = fonts_.emplace();
      if (!f) { error(c, error::kAlloc, 0, 45); return; }
      f->id = fid;
      f->owner = c.index;
      f->cursor_font = StringView(name).equals("cursor");
      Resource res;
      res.kind = Kind::Font;
      res.owner = c.index;
      res.font = f;
      if (!resources_.put(fid, res)) { fonts_.erase(f); error(c, error::kAlloc, 0, 45); }
      return;
    }
    case 46: {
      const uint32_t fid = r.u32();
      Resource* f = resources_.find(fid);
      if (!f || f->kind != Kind::Font) { error(c, error::kFont, fid, 46); return; }
      free_resource(fid);
      return;
    }
    case 47: {
      const uint32_t id = r.u32();
      Resource* f = resources_.find(id);
      if (!f || (f->kind != Kind::Font && f->kind != Kind::Gc)) { error(c, error::kFont, id, 47); return; }
      query_font(c, true, StringView(), false);
      return;
    }
    case 48: {      // QueryTextExtents
      const uint32_t id = r.u32();
      Resource* f = resources_.find(id);
      if (!f || (f->kind != Kind::Font && f->kind != Kind::Gc)) { error(c, error::kFont, id, 48); return; }
      uint64_t chars = r.remaining() / 2;
      if (data && chars > 0) --chars;
      WireWriter o = reply(c, 0, 0);
      o.i16(kFontAscent);
      o.i16(kFontDescent);
      o.i16(kFontAscent);
      o.i16(kFontDescent);
      o.i32(static_cast<int32_t>(chars * kFontWidth));
      o.i32(0);
      o.i32(static_cast<int32_t>(chars * kFontWidth));
      o.zeros(4);
      return;
    }
    case 49: case 50: {      // ListFonts, ListFontsWithInfo
      const uint16_t max_names = r.u16(), n = r.u16();
      ConstByteView pattern = string_of(r, n);
      if (!r.ok()) { error(c, error::kLength, 0, opcode); return; }
      StringView matches[sizeof kFontNames / sizeof kFontNames[0]];
      uint32_t count = 0;
      for (const char* name : kFontNames)
        if (count < max_names && font_pattern_matches(StringView(pattern), name)) matches[count++] = name;
      if (opcode == 50) {
        for (uint32_t i = 0; i < count; ++i) query_font(c, false, matches[i], false);
        query_font(c, false, StringView(), true);
        return;
      }
      uint64_t bytes = 0;
      for (uint32_t i = 0; i < count; ++i) bytes += 1 + matches[i].size();
      WireWriter o = reply(c, 0, static_cast<uint32_t>(pad4(bytes) / 4));
      o.u16(static_cast<uint16_t>(count));
      o.zeros(22);
      for (uint32_t i = 0; i < count; ++i) {
        o.u8(static_cast<uint8_t>(matches[i].size()));
        o.bytes(matches[i].bytes());
      }
      o.pad4();
      return;
    }
    case 51: return;
    case 52: {
      WireWriter o = reply(c, 0, 0);
      o.u16(0);
      o.zeros(22);
      return;
    }
    case 53: {      // CreatePixmap
      const uint32_t pid = r.u32(), drawable = r.u32();
      const uint16_t width = r.u16(), height = r.u16();
      if (!r.ok()) { error(c, error::kLength, 0, 53); return; }
      Window* w;
      Pixmap* px;
      if (!drawable_of(c, drawable, 53, &w, &px) || !new_id(c, pid, 53)) return;
      if (data != 1 && data != 8 && data != 24 && data != 32) { error(c, error::kValue, data, 53); return; }
      if (width == 0 || height == 0) { error(c, error::kValue, 0, 53); return; }
      if (uint64_t{width} * height > kMaxPicturePixels) { error(c, error::kAlloc, 0, 53); return; }
      SharedPointer<Picture> picture = SharedPointer<Picture>::make(allocator_, allocator_);
      if (!picture || !picture->pixels.reserve(uint64_t{width} * height)) { error(c, error::kAlloc, 0, 53); return; }
      picture->width = width;
      picture->height = height;
      picture->depth = data;
      const uint32_t zero = stored_pixel(data, 0);
      for (uint64_t i = 0; i < uint64_t{width} * height; ++i) picture->pixels.push_back(zero);
      Pixmap* p = pixmaps_.emplace();
      if (!p) { error(c, error::kAlloc, 0, 53); return; }
      p->id = pid;
      p->owner = c.index;
      p->picture = util::move(picture);
      Resource res;
      res.kind = Kind::Pixmap;
      res.owner = c.index;
      res.pixmap = p;
      if (!resources_.put(pid, res)) { pixmaps_.erase(p); error(c, error::kAlloc, 0, 53); }
      return;
    }
    case 54: {
      const uint32_t pid = r.u32();
      Resource* p = resources_.find(pid);
      if (!p || p->kind != Kind::Pixmap) { error(c, error::kPixmap, pid, 54); return; }
      free_resource(pid);
      return;
    }
    case 55: {      // CreateGC
      const uint32_t cid = r.u32(), drawable = r.u32(), value_mask = r.u32();
      if (!r.ok()) { error(c, error::kLength, 0, 55); return; }
      Window* w;
      Pixmap* px;
      if (!drawable_of(c, drawable, 55, &w, &px) || !new_id(c, cid, 55)) return;
      Gc* gc = gcs_.emplace();
      if (!gc) { error(c, error::kAlloc, 0, 55); return; }
      gc->id = cid;
      gc->owner = c.index;
      gc->depth = w ? (w->input_only ? 24 : w->depth) : px->picture->depth;
      Resource res;
      res.kind = Kind::Gc;
      res.owner = c.index;
      res.gc = gc;
      if (!resources_.put(cid, res)) { gcs_.erase(gc); error(c, error::kAlloc, 0, 55); return; }
      change_gc(c, *gc, value_mask, r, 55);
      return;
    }
    case 56: {
      Gc* gc = gc_of(c, r.u32(), 56);
      const uint32_t m = r.u32();
      if (gc) change_gc(c, *gc, m, r, 56);
      return;
    }
    case 57: {      // CopyGC
      Gc* src = gc_of(c, r.u32(), 57);
      Gc* dst = src ? gc_of(c, r.u32(), 57) : nullptr;
      const uint32_t m = r.u32();
      if (!src || !dst) return;
      if (src->depth != dst->depth) { error(c, error::kMatch, 0, 57); return; }
      if (m & (1u << 0)) dst->function = src->function;
      if (m & (1u << 1)) dst->plane_mask = src->plane_mask;
      if (m & (1u << 2)) dst->foreground = src->foreground;
      if (m & (1u << 3)) dst->background = src->background;
      if (m & (1u << 4)) dst->line_width = src->line_width;
      if (m & (1u << 5)) dst->line_style = src->line_style;
      if (m & (1u << 6)) dst->cap_style = src->cap_style;
      if (m & (1u << 7)) dst->join_style = src->join_style;
      if (m & (1u << 8)) dst->fill_style = src->fill_style;
      if (m & (1u << 9)) dst->fill_rule = src->fill_rule;
      if (m & (1u << 10)) dst->tile = src->tile;
      if (m & (1u << 11)) dst->stipple = src->stipple;
      if (m & (1u << 12)) dst->ts_x = src->ts_x;
      if (m & (1u << 13)) dst->ts_y = src->ts_y;
      if (m & (1u << 14)) dst->font = src->font;
      if (m & (1u << 15)) dst->subwindow_mode = src->subwindow_mode;
      if (m & (1u << 16)) dst->graphics_exposures = src->graphics_exposures;
      if (m & (1u << 17)) dst->clip_x = src->clip_x;
      if (m & (1u << 18)) dst->clip_y = src->clip_y;
      if (m & (1u << 19)) {
        dst->clip_mask = src->clip_mask;
        dst->clip_rects.reset();
        if (src->clip_rects) {
          dst->clip_rects = UniquePointer<List<Rect>>::make(allocator_, allocator_);
          if (dst->clip_rects) for (const Rect& rect : src->clip_rects->view()) dst->clip_rects->push_back(rect);
        }
      }
      if (m & (1u << 22)) dst->arc_mode = src->arc_mode;
      return;
    }
    case 58: gc_of(c, r.u32(), 58); return;      // SetDashes: lines are solid
    case 59: {      // SetClipRectangles
      Gc* gc = gc_of(c, r.u32(), 59);
      const int16_t cx = r.i16(), cy = r.i16();
      if (!gc) return;
      if (data > 3) { error(c, error::kValue, data, 59); return; }
      if (r.remaining() % 8 != 0) { error(c, error::kLength, 0, 59); return; }
      UniquePointer<List<Rect>> rects = UniquePointer<List<Rect>>::make(allocator_, allocator_);
      if (!rects) { error(c, error::kAlloc, 0, 59); return; }
      while (r.remaining() >= 8) {
        const int16_t x = r.i16(), y = r.i16();
        const uint16_t w = r.u16(), h = r.u16();
        if (w && h && !rects->push_back({x, y, w, h})) { error(c, error::kAlloc, 0, 59); return; }
      }
      gc->clip_x = cx;
      gc->clip_y = cy;
      gc->clip_mask = SharedPointer<Picture>();
      gc->clip_rects = util::move(rects);
      return;
    }
    case 60: {
      const uint32_t id = r.u32();
      if (gc_of(c, id, 60)) free_resource(id);
      return;
    }
    case 61: {      // ClearArea
      Window* w = window_of(c, r.u32(), 61);
      const int16_t x = r.i16(), y = r.i16();
      uint16_t width = r.u16(), height = r.u16();
      if (!w) return;
      if (w->input_only) { error(c, error::kMatch, w->id, 61); return; }
      Rect area{x, y, width ? width : static_cast<int32_t>(w->width) - x, height ? height : static_cast<int32_t>(w->height) - y};
      area = intersect(area, {0, 0, static_cast<int32_t>(w->width), static_cast<int32_t>(w->height)});
      if (area.empty()) return;
      paint_background(*w, area);
      if (data && viewable(w)) {
        Packet p;
        p.u8(0, event::kExpose);
        p.u32(4, w->id);
        p.u16(8, static_cast<uint16_t>(area.x));
        p.u16(10, static_cast<uint16_t>(area.y));
        p.u16(12, static_cast<uint16_t>(area.w));
        p.u16(14, static_cast<uint16_t>(area.h));
        p.u16(16, 0);
        deliver(*w, mask::kExposure, p);
      }
      return;
    }
    case 62: copy_area(c, r, false); return;
    case 63: copy_area(c, r, true); return;
    case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 74: case 75: case 76: case 77:
      poly_request(c, opcode, data, r);
      return;
    case 72: put_image(c, data, r, length); return;
    case 73: get_image(c, data, r); return;
    case 78: {      // CreateColormap
      const uint32_t mid = r.u32(), window = r.u32(), visual = r.u32();
      if (!r.ok()) { error(c, error::kLength, 0, 78); return; }
      if (!window_of(c, window, 78) || !new_id(c, mid, 78)) return;
      if (visual != kVisual24 && visual != kVisual32) { error(c, error::kMatch, visual, 78); return; }
      if (data != 0) { error(c, error::kMatch, data, 78); return; }      // (TrueColor colormaps cannot be writable)
      Resource res;
      res.kind = Kind::Colormap;
      res.owner = c.index;
      if (!resources_.put(mid, res)) error(c, error::kAlloc, 0, 78);
      return;
    }
    case 79: {
      const uint32_t mid = r.u32();
      Resource* m = resources_.find(mid);
      if (!m || m->kind != Kind::Colormap) { error(c, error::kColormap, mid, 79); return; }
      free_resource(mid);
      return;
    }
    case 80: {
      const uint32_t mid = r.u32(), src = r.u32();
      if (!new_id(c, mid, 80)) return;
      Resource* m = resources_.find(src);
      if (!m || m->kind != Kind::Colormap) { error(c, error::kColormap, src, 80); return; }
      Resource res;
      res.kind = Kind::Colormap;
      res.owner = c.index;
      if (!resources_.put(mid, res)) error(c, error::kAlloc, 0, 80);
      return;
    }
    case 81: case 82: {
      const uint32_t mid = r.u32();
      Resource* m = resources_.find(mid);
      if (!m || m->kind != Kind::Colormap) error(c, error::kColormap, mid, opcode);
      return;
    }
    case 83: {
      if (!window_of(c, r.u32(), 83)) return;
      WireWriter o = reply(c, 0, 1);
      o.u16(1);
      o.zeros(22);
      o.u32(kDefaultColormap);
      return;
    }
    case 84: case 85: case 86: case 87: case 88: case 89: case 90: case 91: case 92: {
      const uint32_t mid = r.u32();
      Resource* m = resources_.find(mid);
      if (!m || m->kind != Kind::Colormap) { error(c, error::kColormap, mid, opcode); return; }
      if (opcode == 84) {      // AllocColor
        const uint16_t red = r.u16(), green = r.u16(), blue = r.u16();
        if (!r.ok()) { error(c, error::kLength, 0, 84); return; }
        const uint32_t pixel = pixel_of_rgb(red, green, blue);
        WireWriter o = reply(c, 0, 0);
        o.u16(channel16(pixel >> 16));
        o.u16(channel16(pixel >> 8));
        o.u16(channel16(pixel));
        o.u16(0);
        o.u32(pixel);
        o.zeros(12);
      } else if (opcode == 85 || opcode == 92) {      // AllocNamedColor, LookupColor
        const uint16_t n = r.u16();
        r.skip(2);
        ConstByteView name = string_of(r, n);
        if (!r.ok()) { error(c, error::kLength, 0, opcode); return; }
        uint16_t red, green, blue;
        if (!lookup_color(StringView(name), &red, &green, &blue)) { error(c, error::kName, 0, opcode); return; }
        const uint32_t pixel = pixel_of_rgb(red, green, blue);
        WireWriter o = reply(c, 0, 0);
        if (opcode == 85) o.u32(pixel);
        o.u16(red);
        o.u16(green);
        o.u16(blue);
        o.u16(channel16(pixel >> 16));
        o.u16(channel16(pixel >> 8));
        o.u16(channel16(pixel));
        o.zeros(opcode == 85 ? 8 : 12);
      } else if (opcode == 91) {      // QueryColors
        const uint64_t n = r.remaining() / 4;
        if (n > 65535) { error(c, error::kLength, 0, 91); return; }
        WireWriter o = reply(c, 0, static_cast<uint32_t>(2 * n));
        o.u16(static_cast<uint16_t>(n));
        o.zeros(22);
        for (uint64_t i = 0; i < n; ++i) {
          const uint32_t pixel = r.u32();
          o.u16(channel16(pixel >> 16));
          o.u16(channel16(pixel >> 8));
          o.u16(channel16(pixel));
          o.u16(0);
        }
      } else if (opcode == 86 || opcode == 87) {
        error(c, error::kAlloc, 0, opcode);      // (TrueColor: no writable cells)
      } else if (opcode == 89 || opcode == 90) {
        error(c, error::kAccess, 0, opcode);
      }
      return;      // (88 FreeColors: nothing was allocated)
    }
    case 93: create_cursor(c, r); return;
    case 94: create_glyph_cursor(c, r); return;
    case 95: {
      const uint32_t id = r.u32();
      Resource* k = resources_.find(id);
      if (!k || k->kind != Kind::Cursor) { error(c, error::kCursor, id, 95); return; }
      free_resource(id);
      for (Window& w : windows_) if (w.cursor == id) w.cursor = 0;
      if (pointer_grab_.cursor == id) pointer_grab_.cursor = 0;
      return;
    }
    case 96: {
      const uint32_t id = r.u32();
      Resource* k = resources_.find(id);
      if (!k || k->kind != Kind::Cursor) error(c, error::kCursor, id, 96);
      return;
    }
    case 97: {      // QueryBestSize
      Window* w;
      Pixmap* px;
      if (!drawable_of(c, r.u32(), 97, &w, &px)) return;
      uint16_t width = r.u16(), height = r.u16();
      if (data > 2) { error(c, error::kValue, data, 97); return; }
      if (data == 0) {
        if (width > 64) width = 64;
        if (height > 64) height = 64;
      }
      WireWriter o = reply(c, 0, 0);
      o.u16(width);
      o.u16(height);
      o.zeros(20);
      return;
    }
    case 98: {      // QueryExtension
      const uint16_t n = r.u16();
      r.skip(2);
      ConstByteView name = string_of(r, n);
      if (!r.ok()) { error(c, error::kLength, 0, 98); return; }
      const bool big = StringView(name).equals("BIG-REQUESTS");
      WireWriter o = reply(c, 0, 0);
      o.u8(big ? 1 : 0);
      o.u8(big ? kBigRequestsOpcode : 0);
      o.u8(0);
      o.u8(0);
      o.zeros(20);
      return;
    }
    case 99: {
      const StringView name = "BIG-REQUESTS";
      WireWriter o = reply(c, 1, static_cast<uint32_t>(pad4(1 + name.size()) / 4));
      o.zeros(24);
      o.u8(static_cast<uint8_t>(name.size()));
      o.bytes(name.bytes());
      o.pad4();
      return;
    }
    case 100: {      // ChangeKeyboardMapping: the map stays (US); clients hear that it was asked
      const uint8_t first = r.u8();
      if (first < kMinKeycode || uint32_t{first} + data - 1 > kMaxKeycode) { error(c, error::kValue, first, 100); return; }
      for (UniquePointer<Client>& other : clients_) {
        if (!other || !other->setup_done) continue;
        Packet p;
        p.u8(0, event::kMappingNotify);
        p.u8(4, 1);
        p.u8(5, first);
        p.u8(6, data);
        send(*other, p);
      }
      return;
    }
    case 101: {      // GetKeyboardMapping
      const uint8_t first = r.u8(), count = r.u8();
      if (first < kMinKeycode || count == 0 || uint32_t{first} + count - 1 > kMaxKeycode) { error(c, error::kValue, first, 101); return; }
      WireWriter o = reply(c, kKeysymsPerKeycode, count * kKeysymsPerKeycode);
      o.zeros(24);
      for (uint32_t k = first; k < uint32_t{first} + count; ++k) {
        uint32_t plain = 0, shifted = 0;
        if (!keysyms_of_keycode(static_cast<uint8_t>(k), &plain, &shifted)) plain = shifted = 0;
        o.u32(plain);
        o.u32(shifted == plain ? 0 : shifted);
      }
      return;
    }
    case 102: case 104: case 105: case 107: case 109: case 111: case 112: case 115: return;      // keyboard control, bell, screen saver, hosts: accepted
    case 103: {
      WireWriter o = reply(c, 1, 5);
      o.u32(0);
      o.u8(0);
      o.u8(50);
      o.u16(400);
      o.u16(100);
      o.u16(0);
      for (uint32_t i = 0; i < 32; ++i) o.u8(0xFF);
      return;
    }
    case 106: {
      WireWriter o = reply(c, 0, 0);
      o.u16(2);
      o.u16(1);
      o.u16(4);
      o.zeros(18);
      return;
    }
    case 108: {
      WireWriter o = reply(c, 0, 0);
      o.u16(0);
      o.u16(0);
      o.u8(1);
      o.u8(1);
      o.zeros(18);
      return;
    }
    case 110: {
      WireWriter o = reply(c, 0, 0);
      o.u16(0);
      o.zeros(22);
      return;
    }
    case 113: kill_client(c, r.u32()); return;
    case 114: {      // RotateProperties
      Window* w = window_of(c, r.u32(), 114);
      const uint16_t n = r.u16();
      const int16_t delta = r.i16();
      if (!w) return;
      if (r.remaining() < uint64_t{n} * 4) { error(c, error::kLength, 0, 114); return; }
      List<uint32_t> names(allocator_);
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t a = r.u32();
        if (atom_name(a).empty()) { error(c, error::kAtom, a, 114); return; }
        for (uint32_t other : names.view()) if (other == a) { error(c, error::kMatch, a, 114); return; }
        if (!find_property(*w, a)) { error(c, error::kMatch, a, 114); return; }
        names.push_back(a);
      }
      if (n == 0 || delta % n == 0) return;
      const int32_t shift = ((delta % n) + n) % n;
      // property names[i]'s value becomes that of names[(i + delta) mod n]: rename the properties
      List<Property*> props(allocator_);
      for (uint32_t a : names.view()) props.push_back(find_property(*w, a));
      for (uint32_t i = 0; i < n; ++i) props[i]->name = names[(i + static_cast<uint32_t>(shift)) % n];
      for (uint32_t a : names.view()) property_changed(*w, a, false);
      return;
    }
    case 116: {
      WireWriter o = reply(c, 0, 0);
      o.zeros(24);
      return;
    }
    case 117: {
      WireWriter o = reply(c, 5, 2);
      o.zeros(24);
      for (uint8_t b = 1; b <= 5; ++b) o.u8(b);
      o.pad4();
      return;
    }
    case 118: {
      WireWriter o = reply(c, 0, 0);
      o.zeros(24);
      return;
    }
    case 119: {
      WireWriter o = reply(c, 2, 4);
      o.zeros(24);
      const uint8_t map[16] = {0xE1 + 8, 0xE5 + 8, 0x39 + 8, 0, 0xE0 + 8, 0xE4 + 8, 0xE2 + 8, 0xE6 + 8, 0x53 + 8, 0, 0, 0, 0xE3 + 8, 0xE7 + 8, 0, 0};
      o.bytes(ConstByteView(map, 16));
      return;
    }
    case 127: return;
    case kBigRequestsOpcode: {
      if (data != 0) { error(c, error::kRequest, 0, opcode, data); return; }
      c.big_requests = true;
      WireWriter o = reply(c, 0, 0);
      o.u32(static_cast<uint32_t>(kMaxRequestBytes / 4));
      o.zeros(20);
      return;
    }
    default: error(c, error::kRequest, 0, opcode); return;
  }
}

}  // namespace x11
