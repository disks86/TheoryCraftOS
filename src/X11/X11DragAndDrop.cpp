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

// Drag and drop between X clients and the desktop (XDND, version 5 offered; the server keeps to what versions 3 to 5 have in common).
//
// The desktop's drag into a client (desktop_drag, as the pointer moves over a top-level that has XdndAware): the server is the source. Its window is
// the window manager's check window (wm_check_), which owns XdndSelection while the drag lasts. The top-level gets XdndEnter (the types: text/uri-list
// for a file a Linux program can see, as file:///mnt/desktop/...; then the text types), XdndPosition (root coordinates, action copy) as the pointer
// moves, one at a time (the next waits for the client's XdndStatus, as the protocol asks), and XdndLeave when the pointer goes. Let go over a client
// that accepted it, it gets XdndDrop and asks for the data with ConvertSelection, which the server answers itself (xdnd_convert: the property on the
// requestor, then SelectionNotify); XdndFinished ends it.
//
// A client's drag over the desktop: the root has XdndAware, so a client that drags outside its own windows (where the root is the top-level under the
// pointer) sends the root XdndEnter, XdndPosition and XdndLeave or XdndDrop. The server asks the source for the data at once (ConvertSelection of
// XdndSelection into its own window, a SelectionRequest to the owner; the owner's SelectionNotify to that window brings it, xdnd_selection_notify), so
// the desktop can show it while it is held (program_drag) and say whether it would take it (program_drag_answer, which XdndStatus tells the source).
// Dropped, the items (URIs made system paths, desktop::drop_items_of) go to the desktop (take_program_drop) and XdndFinished says whether one was
// taken (program_drop_done). One drag each way at a time; INCR transfers are not taken.
#include "Desktop/DropItems.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "X11/X11Internal.hpp"

namespace x11 {

namespace {

constexpr uint32_t kXdndVersion = 5;
constexpr uint64_t kMaxDragData = 1u << 20;      // (a megabyte of URIs or text at most)

bool text_target(uint32_t target, uint32_t utf8_string, uint32_t text_plain, uint32_t text_utf8) {
  return target == utf8_string || target == text_plain || target == text_utf8 || target == atom::kString;
}

}  // namespace

void Server::xdnd_setup() {
  xa_.aware = intern("XdndAware");
  xa_.enter = intern("XdndEnter");
  xa_.position = intern("XdndPosition");
  xa_.status = intern("XdndStatus");
  xa_.leave = intern("XdndLeave");
  xa_.drop = intern("XdndDrop");
  xa_.finished = intern("XdndFinished");
  xa_.selection = intern("XdndSelection");
  xa_.action_copy = intern("XdndActionCopy");
  xa_.type_list = intern("XdndTypeList");
  xa_.uri_list = intern("text/uri-list");
  xa_.text_plain = intern("text/plain");
  xa_.text_utf8 = intern("text/plain;charset=utf-8");
  xa_.targets = intern("TARGETS");
  xa_.data = intern("TCOS_XDND_DATA");
  xa_.incr = intern("INCR");
  xa_.utf8 = intern("UTF8_STRING");
  if (root_) {
    const uint32_t version[] = {kXdndVersion};
    set_property32(*root_, xa_.aware, atom::kAtom, version);      // (drops on the desktop: see above)
  }
}

void Server::send_client_message5(Window& w, uint32_t type, const uint32_t (&data)[5]) {
  if (w.owner == 0 || w.owner >= kMaxClients || !clients_[w.owner] || !clients_[w.owner]->setup_done) return;
  Packet p;
  p.u8(0, event::kClientMessage);
  p.u8(1, 32);
  p.u32(4, w.id);
  p.u32(8, type);
  for (uint32_t i = 0; i < 5; ++i) p.u32(12 + 4 * i, data[i]);
  send(*clients_[w.owner], p);
}

// ------------------------------------------------------------------ the desktop's drag into a client

void Server::out_drag_reset() {
  out_drag_ = OutDrag();
  for (SelectionOwner& s : selection_owners_.view())
    if (s.selection == xa_.selection && s.window == kWmCheckWindow && s.client == 0) s.window = 0;
}

void Server::out_drag_leave() {
  if (out_drag_.active)
    if (Window* t = find_window(out_drag_.target)) {
      const uint32_t d[5] = {kWmCheckWindow, 0, 0, 0, 0};
      send_client_message5(*t, xa_.leave, d);
    }
  out_drag_reset();
}

void Server::out_drag_position() {
  Window* t = find_window(out_drag_.target);
  if (!t) return;
  const uint32_t d[5] = {kWmCheckWindow, 0, (static_cast<uint32_t>(out_drag_.x & 0xFFFF) << 16) | static_cast<uint32_t>(out_drag_.y & 0xFFFF), now(),
                         xa_.action_copy};
  send_client_message5(*t, xa_.position, d);
  out_drag_.waiting = true;
  out_drag_.pending = false;
}

bool Server::desktop_drag(uint32_t window, int32_t x_units, int32_t y_units, StringView text) {
  bool taken = false;
  {
    kernel::ScopedMutex lock(mutex_);
    const int32_t x = up(x_units), y = up(y_units);      // (the desktop's units -> device pixels)
    if (out_drag_.dropped) return false;      // (the last drop is not finished)
    Window* top = window != 0 ? find_window(window) : nullptr;
    uint32_t version = 0;
    if (!top || top->parent != root_ || !property32(*top, xa_.aware, 0, &version) || version < 3 || text.empty() || text.size() > kMaxDragData) {
      out_drag_leave();      // (left the session's windows, or a window that takes no drops)
    } else {
      if (!out_drag_.active || !out_text_.view().equals(text)) {
        out_text_.clear();
        out_uri_.clear();
        out_linux_.clear();
        out_text_.append(text);
        if (text[0] == '/' && filesystem::layout::linux_view_of(text, &out_linux_)) {
          if (!util::file_uri(out_linux_.view(), &out_uri_)) out_uri_.clear();
        } else {
          out_linux_.clear();
        }
      }
      if (out_drag_.active && out_drag_.target != window) out_drag_leave();
      if (!out_drag_.active) {
        out_drag_.active = true;
        out_drag_.target = window;
        out_drag_.version = version < kXdndVersion ? version : kXdndVersion;
        // the server's window owns XdndSelection; its XdndTypeList names what it offers
        SelectionOwner* entry = nullptr;
        for (SelectionOwner& s : selection_owners_.view()) if (s.selection == xa_.selection) entry = &s;
        if (!entry && selection_owners_.push_back(SelectionOwner{})) entry = &selection_owners_[selection_owners_.count() - 1];
        if (entry) {
          if (entry->window != 0 && entry->client != 0 && entry->client < kMaxClients && clients_[entry->client]) {
            Packet clear;
            clear.u8(0, event::kSelectionClear);
            clear.u32(4, now());
            clear.u32(8, entry->window);
            clear.u32(12, xa_.selection);
            send(*clients_[entry->client], clear);
          }
          entry->selection = xa_.selection;
          entry->window = kWmCheckWindow;
          entry->client = 0;
          entry->time = now();
        }
        uint32_t types[4];
        uint32_t n = 0;
        if (!out_uri_.empty()) types[n++] = xa_.uri_list;
        types[n++] = xa_.text_utf8;
        types[n++] = xa_.utf8;
        types[n++] = xa_.text_plain;
        if (wm_check_) set_property32(*wm_check_, xa_.type_list, atom::kAtom, ArrayView<const uint32_t>(types, n));
        const uint32_t d[5] = {kWmCheckWindow, (out_drag_.version << 24) | (n > 3 ? 1u : 0u), types[0], types[1], types[2]};
        send_client_message5(*top, xa_.enter, d);
      }
      if (x != out_drag_.x || y != out_drag_.y || (!out_drag_.waiting && !out_drag_.pending)) {
        out_drag_.x = x;
        out_drag_.y = y;
        if (out_drag_.waiting) out_drag_.pending = true;
        else out_drag_position();
      }
      taken = out_drag_.accepted;
    }
  }
  wake();
  return taken;
}

bool Server::desktop_drop() {
  bool dropped = false;
  {
    kernel::ScopedMutex lock(mutex_);
    if (!out_drag_.active) return false;
    Window* t = find_window(out_drag_.target);
    if (!t || !out_drag_.accepted) {
      out_drag_leave();
    } else {
      const uint32_t d[5] = {kWmCheckWindow, 0, now(), 0, 0};
      send_client_message5(*t, xa_.drop, d);
      out_drag_.active = false;
      out_drag_.dropped = true;      // (until XdndFinished, or the target going)
      dropped = true;
    }
  }
  wake();
  return dropped;
}

void Server::desktop_drag_end() {
  {
    kernel::ScopedMutex lock(mutex_);
    if (!out_drag_.active) return;
    out_drag_leave();
  }
  wake();
}

// ConvertSelection of XdndSelection while the server owns it: what the desktop's drag carries, in the property the requestor named.
bool Server::xdnd_convert(Client& c, Window& requestor, uint32_t selection, uint32_t target, uint32_t property, uint32_t time) {
  if (selection != xa_.selection || !(out_drag_.active || out_drag_.dropped)) return false;
  bool owned = false;
  for (const SelectionOwner& s : selection_owners_.view()) owned |= s.selection == selection && s.window == kWmCheckWindow && s.client == 0;
  if (!owned) return false;
  if (property == 0) property = target;      // (an old client: the target names the property)
  bool done = true;
  if (target == xa_.targets) {
    uint32_t types[5];
    uint32_t n = 0;
    types[n++] = xa_.targets;
    if (!out_uri_.empty()) types[n++] = xa_.uri_list;
    types[n++] = xa_.text_utf8;
    types[n++] = xa_.utf8;
    types[n++] = xa_.text_plain;
    set_property32(requestor, property, atom::kAtom, ArrayView<const uint32_t>(types, n));
  } else if (target == xa_.uri_list && !out_uri_.empty()) {
    set_property(requestor, property, target, 8, out_uri_.view().bytes());
  } else if (text_target(target, xa_.utf8, xa_.text_plain, xa_.text_utf8)) {
    const StringView what = !out_linux_.empty() ? out_linux_.view() : out_text_.view();
    set_property(requestor, property, target, 8, what.bytes());
  } else {
    done = false;
  }
  Packet p;
  p.u8(0, event::kSelectionNotify);
  p.u32(4, time);
  p.u32(8, requestor.id);
  p.u32(12, selection);
  p.u32(16, target);
  p.u32(20, done ? property : 0);
  send(c, p);
  return true;
}

// ------------------------------------------------------------------ a client's drag over the desktop

void Server::in_drag_reset() {
  in_drag_ = InDrag();
  in_text_.clear();
  if (wm_check_) delete_property(*wm_check_, xa_.data);
}

// XdndStatus to the source: taken when the data has come, has something for the desktop and the desktop said it would take it; positions wanted
// all the time (an empty rectangle).
void Server::in_drag_status() {
  Window* src = find_window(in_drag_.source);
  if (!src) return;
  const bool accept = in_drag_.read && !in_text_.empty() && in_drag_.answer == 1;
  const uint32_t d[5] = {kRootWindow, (accept ? 1u : 0u) | 2u, 0, 0, accept ? xa_.action_copy : 0u};
  send_client_message5(*src, xa_.status, d);
}

void Server::in_drag_finish(bool taken) {
  if (Window* src = find_window(in_drag_.source)) {
    const uint32_t d[5] = {kRootWindow, taken ? 1u : 0u, taken ? xa_.action_copy : 0u, 0, 0};
    send_client_message5(*src, xa_.finished, d);
  }
  in_drag_reset();
}

void Server::in_drag_read(ConstByteView data) {
  in_drag_.read = true;
  const ConstByteView bytes = data.size() > kMaxDragData ? data.first(0) : data;
  desktop::drop_items_of(bytes, in_drag_.uri, &in_text_);
  if (!in_drag_.dropped) {
    in_drag_status();
    return;
  }
  if (in_text_.empty()) in_drag_finish(false);      // (let go before the data came, and nothing in it is for the desktop)
  else in_drag_.ready = true;
}

bool Server::xdnd_client_message(Window& target, const Packet& p) {
  const uint32_t type = p.get32(8);
  uint32_t d[5];
  for (uint32_t i = 0; i < 5; ++i) d[i] = p.get32(12 + 4 * i);
  if (&target == wm_check_) {      // the client the desktop's drag is over answers the server
    if (type == xa_.status) {
      if (!out_drag_.active || d[0] != out_drag_.target) return true;
      out_drag_.accepted = (d[1] & 1) != 0 && (d[4] == 0 || d[4] == xa_.action_copy || out_drag_.version < 2);
      out_drag_.waiting = false;
      if (out_drag_.pending) out_drag_position();
      return true;
    }
    if (type == xa_.finished) {
      if (out_drag_.dropped && d[0] == out_drag_.target) out_drag_reset();
      return true;
    }
    return false;
  }
  if (&target != root_ || (type != xa_.enter && type != xa_.position && type != xa_.leave && type != xa_.drop)) return false;
  if (type == xa_.enter) {
    if (in_drag_.active && in_drag_.dropped) return true;      // (the last drop waits for the desktop)
    in_drag_reset();
    Window* src = find_window(d[0]);
    if (!src || src->owner == 0) return true;
    // the types: in the message, or (more than three) the source's XdndTypeList
    bool uri = false, text = false;
    uint32_t text_type = 0;
    auto consider = [&](uint32_t t) {
      if (t == xa_.uri_list) uri = true;
      else if (text_target(t, xa_.utf8, xa_.text_plain, xa_.text_utf8) && (!text || t == xa_.text_utf8 || t == xa_.utf8)) text = true, text_type = t;
    };
    if (d[1] & 1) {
      if (Property* list = find_property(*src, xa_.type_list); list && list->format == 32) {
        ByteReader in(list->data.view());
        while (in.remaining() >= 4) consider(in.le32());
      }
    } else {
      for (uint32_t i = 2; i < 5; ++i) if (d[i]) consider(d[i]);
    }
    in_drag_.active = true;
    in_drag_.source = d[0];
    in_drag_.client = src->owner;
    in_drag_.version = d[1] >> 24;
    if (!uri && !text) return true;      // (nothing the desktop takes: it is never accepted)
    in_drag_.uri = uri;
    in_drag_.type = uri ? xa_.uri_list : text_type;
    // the data, at once: the owner of XdndSelection is asked to put it on the server's window
    const SelectionOwner* owner = nullptr;
    for (const SelectionOwner& s : selection_owners_.view()) if (s.selection == xa_.selection && s.window != 0) owner = &s;
    if (owner && owner->client != 0 && owner->client < kMaxClients && clients_[owner->client] && clients_[owner->client]->setup_done) {
      Packet r;
      r.u8(0, event::kSelectionRequest);
      r.u32(4, now());
      r.u32(8, owner->window);
      r.u32(12, kWmCheckWindow);
      r.u32(16, xa_.selection);
      r.u32(20, in_drag_.type);
      r.u32(24, xa_.data);
      send(*clients_[owner->client], r);
      in_drag_.asked = true;
    }
    return true;
  }
  if (!in_drag_.active || d[0] != in_drag_.source) return true;      // (another source's: not this drag)
  if (type == xa_.position) {
    if (in_drag_.dropped) return true;
    in_drag_.x = static_cast<int16_t>(d[2] >> 16);
    in_drag_.y = static_cast<int16_t>(d[2] & 0xFFFF);
    in_drag_status();
    return true;
  }
  if (type == xa_.leave) {
    if (!in_drag_.dropped) in_drag_reset();
    return true;
  }
  if (type == xa_.drop) {
    if (in_drag_.dropped) return true;
    in_drag_.dropped = true;
    if (!in_drag_.asked) in_drag_finish(false);      // (nothing the desktop takes)
    else if (in_drag_.read) {
      if (in_text_.empty()) in_drag_finish(false);
      else in_drag_.ready = true;
    }
    return true;
  }
  return true;
}

bool Server::xdnd_selection_notify(Window& target, const Packet& p) {
  if (&target != wm_check_) return false;
  if (!in_drag_.active || !in_drag_.asked || in_drag_.read || p.get32(12) != xa_.selection) return true;
  const uint32_t property = p.get32(20);
  Property* data = property != 0 && wm_check_ ? find_property(*wm_check_, property) : nullptr;
  if (!data || data->format != 8 || data->type == xa_.incr) {
    in_drag_read(ConstByteView());      // (refused, or too big for one property)
  } else {
    ByteBuffer copy(allocator_);
    copy.append(data->data.view());
    in_drag_read(copy.view());
  }
  if (wm_check_ && property != 0) delete_property(*wm_check_, property);
  return true;
}

void Server::xdnd_client_gone(uint8_t client) {
  if (in_drag_.active && in_drag_.client == client) in_drag_reset();
  if ((out_drag_.active || out_drag_.dropped))
    if (Window* t = find_window(out_drag_.target); !t || t->owner == client) out_drag_reset();
}

bool Server::take_program_drop(String* text, int32_t* x, int32_t* y) {
  kernel::ScopedMutex lock(mutex_);
  if (!in_drag_.ready || in_drag_.given) return false;
  if (!text->assign(in_text_.view())) return false;
  in_drag_.given = true;
  *x = down(in_drag_.x);
  *y = down(in_drag_.y);
  return true;
}

void Server::program_drop_done(bool taken) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (!in_drag_.ready) return;
    in_drag_finish(taken);
  }
  wake();
}

bool Server::program_drag(String* text, int32_t* x, int32_t* y) {
  kernel::ScopedMutex lock(mutex_);
  if (!in_drag_.active || in_drag_.dropped || !in_drag_.read || in_text_.empty()) return false;
  if (!text->assign(in_text_.view())) return false;
  *x = down(in_drag_.x);
  *y = down(in_drag_.y);
  return true;
}

void Server::program_drag_answer(bool would_take) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (!in_drag_.active || in_drag_.dropped || !in_drag_.read) return;
    const int8_t answer = would_take ? 1 : 0;
    if (answer == in_drag_.answer) return;
    in_drag_.answer = answer;
    in_drag_status();
  }
  wake();
}

}  // namespace x11
