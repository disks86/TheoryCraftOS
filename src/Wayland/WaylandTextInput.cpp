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

// zwp_text_input_v3 (version 1): text an input method composes, handed to the client that has the keyboard. A client enables a text input while a text
// field of its has the focus and tells it about the field (surrounding text, the cursor's rectangle, the content type); the input method (on this system the
// desktop's: Desktop/Win32Windows.cpp's Unicode entry) sends the text being composed (preedit_string: the client shows it, underlined, at its cursor) and
// the text decided on (commit_string), each batch closed with done(serial), the serial being how many commits the client has made, as the protocol says.
//
// The state requests are double-buffered and applied at commit; enable resets what was set before it (the protocol's rule). A text input is entered when its
// client's surface gets the keyboard focus (and left when it loses it); only an entered, enabled text input receives text. Strings are limited to 4000 bytes
// (the protocol's own limit) and must be whole UTF-8 (libwayland does not check: a cut string is an error here too, as wlroots treats it).
#include "Wayland/WaylandInternal.hpp"

namespace wayland {

namespace {

constexpr uint32_t kMaxText = 4000;

// A string that is whole UTF-8 (no cut sequence, no stray continuation byte).
bool whole_utf8(StringView s) {
  const ConstByteView b = s.bytes();
  for (uint64_t i = 0; i < b.size();) {
    const uint8_t c = b[i];
    const uint32_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (n == 0 || n > b.size() - i) return false;
    for (uint32_t k = 1; k < n; ++k)
      if ((b[i + k] & 0xC0) != 0x80) return false;
    i += n;
  }
  return true;
}

}  // namespace

// The text input of the keyboard's client that is entered and enabled (null: none), with its client.
Server::TextInputInfo* Server::focused_text_input(Client** client, uint32_t* id) {
  if (!keyboard_focus_) return nullptr;
  Client* c = client_of(keyboard_focus_->client);
  if (!c) return nullptr;
  ObjectCursor cursor;
  for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
    const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
    if (kv->value.iface != Iface::TextInput || !kv->value.text_input) continue;
    TextInputInfo* t = kv->value.text_input.get();
    if (t->entered == keyboard_focus_->id && t->enabled) {
      *client = c;
      *id = kv->key;
      return t;
    }
  }
  return nullptr;
}

// Keyboard focus moved from `old` (may be null) to `now`: the text inputs of their clients hear leave and enter.
void Server::text_input_focus(Surface* old, Surface* now) {
  Surface* const both[2] = {old, now};
  for (Surface* s : both) {
    if (!s) continue;
    Client* c = client_of(s->client);
    if (!c) continue;
    List<uint32_t> ids(allocator_);
    ObjectCursor cursor;
    for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor))
      if (c->objects.element(cursor)->value.iface == Iface::TextInput) ids.push_back(c->objects.element(cursor)->key);
    for (uint32_t id : ids.view()) {
      Resource* r = c->objects.find(id);
      if (!r || !r->text_input) continue;
      TextInputInfo& t = *r->text_input;
      if (s == old && t.entered == old->id) {
        Event e(id, 1);      // leave(surface)
        e.u(old->id);
        send(*c, e);
        t.entered = 0;
        t.preedit_shown = false;
      }
      if (s == now && t.entered != now->id) {
        Event e(id, 0);      // enter(surface)
        e.u(now->id);
        send(*c, e);
        t.entered = now->id;
      }
    }
  }
}

void Server::text_input_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  if (r.iface == Iface::TextInputManager) {
    switch (opcode) {
      case 0: destroy_object(c, id); return;      // destroy
      case 1: {                                   // get_text_input(new id, seat)
        const uint32_t object = in.u(), seat = in.object();
        if (!in.ok()) return;
        if (!object_of(c, seat, Iface::Seat, id)) return;
        SharedPointer<TextInputInfo> info = SharedPointer<TextInputInfo>::make(allocator_, allocator_);
        if (!info) {
          post_error(c, kDisplayId, kErrorNoMemory, "out of memory");
          return;
        }
        Resource* nr = new_object(c, object, Iface::TextInput, 1, id);
        if (!nr) return;
        nr->text_input = util::move(info);
        if (keyboard_focus_ && keyboard_focus_->client == c.number) {      // (the client has the keyboard already)
          Event e(object, 0);
          e.u(keyboard_focus_->id);
          send(c, e);
          nr->text_input->entered = keyboard_focus_->id;
        }
        return;
      }
      default: post_error(c, id, kErrorInvalidMethod, "zwp_text_input_manager_v3 has no such request"); return;
    }
  }
  if (!r.text_input) return;
  TextInputInfo& t = *r.text_input;
  switch (opcode) {
    case 0: destroy_object(c, id); return;      // destroy
    case 1:                                     // enable: the state from before it is reset
      t.pending = TextInputInfo::State(allocator_);
      t.pending.enabled = true;
      t.pending.enable_seen = true;
      return;
    case 2:      // disable
      t.pending.enabled = false;
      t.pending.enable_seen = true;
      return;
    case 3: {      // set_surrounding_text(text, cursor, anchor)
      const StringView text = in.string();
      const int32_t cur = in.i(), anchor = in.i();
      if (!in.ok()) return;
      if (text.size() > kMaxText || !whole_utf8(text) || cur < 0 || anchor < 0 || uint64_t(cur) > text.size() || uint64_t(anchor) > text.size()) {
        post_error(c, id, kErrorInvalidMethod, "invalid surrounding text");
        return;
      }
      t.pending.surrounding.clear();
      t.pending.surrounding.append(text);
      t.pending.cursor = cur;
      t.pending.anchor = anchor;
      t.pending.surrounding_set = true;
      return;
    }
    case 4: {      // set_text_change_cause(cause): 0 input method, 1 other
      in.u();
      return;
    }
    case 5: {      // set_content_type(hint, purpose)
      const uint32_t hint = in.u(), purpose = in.u();
      if (!in.ok()) return;
      t.pending.hint = hint;
      t.pending.purpose = purpose > 13 ? 0 : purpose;
      return;
    }
    case 6: {      // set_cursor_rectangle(x, y, width, height): surface coordinates
      const int32_t x = in.i(), y = in.i(), w = in.i(), h = in.i();
      if (!in.ok()) return;
      t.pending.rect = {max32(-kMaxSize, min32(x, kMaxSize)), max32(-kMaxSize, min32(y, kMaxSize)), max32(0, min32(w, kMaxSize)), max32(0, min32(h, kMaxSize))};
      t.pending.rect_set = true;
      return;
    }
    case 7: {      // commit
      ++t.commits;
      if (t.pending.enable_seen) {
        const bool was = t.enabled;
        t.enabled = t.pending.enabled;
        if (t.enabled && !was) t.current = TextInputInfo::State(allocator_);
        if (!t.enabled) t.preedit_shown = false;
      }
      if (t.pending.surrounding_set) {
        t.current.surrounding.clear();
        t.current.surrounding.append(t.pending.surrounding.view());
        t.current.cursor = t.pending.cursor;
        t.current.anchor = t.pending.anchor;
      }
      if (t.pending.rect_set) t.current.rect = t.pending.rect;
      t.current.hint = t.pending.hint;
      t.current.purpose = t.pending.purpose;
      t.pending = TextInputInfo::State(allocator_);
      t.pending.hint = t.current.hint;
      t.pending.purpose = t.current.purpose;
      return;
    }
    default: post_error(c, id, kErrorInvalidMethod, "zwp_text_input_v3 has no such request"); return;
  }
}

bool Server::text_input(TextInputState* out) {
  kernel::ScopedMutex lock(mutex_);
  *out = TextInputState();
  Client* c = nullptr;
  uint32_t id = 0;
  TextInputInfo* t = focused_text_input(&c, &id);
  if (!t) return false;
  int32_t ox = 0, oy = 0;
  surface_origin(*keyboard_focus_, &ox, &oy);
  out->enabled = true;
  out->window = keyboard_focus_->window;
  const Rect& r = t->current.rect;
  out->cursor = {ox + r.x, oy + r.y, static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h)};
  out->purpose = t->current.purpose;
  out->hint = t->current.hint;
  return true;
}

bool Server::text_preedit(StringView text, uint32_t cursor) {
  {
    kernel::ScopedMutex lock(mutex_);
    Client* c = nullptr;
    uint32_t id = 0;
    TextInputInfo* t = focused_text_input(&c, &id);
    if (!t || text.size() > kMaxText) return false;
    Event e(id, 2);      // preedit_string(text, cursor_begin, cursor_end): a null text hides the preedit
    if (text.size() == 0) e.u(0);
    else e.string(text);
    const int32_t at = cursor > text.size() ? static_cast<int32_t>(text.size()) : static_cast<int32_t>(cursor);
    e.i(text.size() == 0 ? 0 : at).i(text.size() == 0 ? 0 : at);
    send(*c, e);
    Event done(id, 5);
    done.u(t->commits);
    send(*c, done);
    t->preedit_shown = text.size() > 0;
  }
  wake();
  return true;
}

bool Server::text_commit(StringView text) {
  {
    kernel::ScopedMutex lock(mutex_);
    Client* c = nullptr;
    uint32_t id = 0;
    TextInputInfo* t = focused_text_input(&c, &id);
    if (!t || text.size() > kMaxText || !whole_utf8(text)) return false;
    // (no preedit_string in this batch: the preedit goes, the text comes in its place)
    Event e(id, 3);      // commit_string(text)
    e.string(text);
    send(*c, e);
    Event done(id, 5);
    done.u(t->commits);
    send(*c, done);
    t->preedit_shown = false;
  }
  wake();
  return true;
}

}  // namespace wayland
