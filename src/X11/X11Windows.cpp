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

// The X server's windows, clients and atoms, and the window manager inside it: what the desktop sees of the top-level windows (desktop::IWindowsSession).

#include "X11/X11Internal.hpp"

namespace x11 {

// ------------------------------------------------------------------ regions

void Region::intersect(const Rect& r) {
  for (uint64_t i = 0; i < rects_.count();) {
    const Rect is = x11::intersect(rects_[i], r);
    if (is.empty()) {
      rects_.remove_at(i);
    } else {
      rects_[i] = is;
      ++i;
    }
  }
}

void Region::subtract(const Rect& r) {
  if (r.empty()) return;
  uint64_t n = rects_.count();
  for (uint64_t i = 0; i < n;) {
    const Rect a = rects_[i];
    const Rect is = x11::intersect(a, r);
    if (is.empty()) {
      ++i;
      continue;
    }
    rects_.remove_at(i);
    --n;
    // what is left of `a`: the band above, the band below, and left and right of the cut within its rows (appended after the ones still to look at)
    if (is.y > a.y) rects_.push_back({a.x, a.y, a.w, is.y - a.y});
    if (is.y + is.h < a.y + a.h) rects_.push_back({a.x, is.y + is.h, a.w, a.y + a.h - (is.y + is.h)});
    if (is.x > a.x) rects_.push_back({a.x, is.y, is.x - a.x, is.h});
    if (is.x + is.w < a.x + a.w) rects_.push_back({is.x + is.w, is.y, a.x + a.w - (is.x + is.w), is.h});
  }
}

Rect Region::bounds() const {
  Rect b;
  for (const Rect& r : rects_.view()) b = unite(b, r);
  return b;
}

// ------------------------------------------------------------------ the server

namespace {

// The atoms the window manager part uses, made at start so they have fixed numbers after the predefined ones.
enum WellKnown : uint32_t {
  kWmProtocols = kLastPredefinedAtom + 1, kWmDeleteWindow, kWmTakeFocus, kWmState, kWmChangeState, kUtf8String, kNetWmName, kNetSupported,
  kNetSupportingWmCheck, kNetActiveWindow, kNetClientList, kNetClientListStacking, kNetNumberOfDesktops, kNetCurrentDesktop, kNetDesktopGeometry,
  kNetWorkarea, kNetWmState, kNetWmStateMaximizedVert, kNetWmStateMaximizedHorz, kNetWmStateFullscreen, kNetWmStateAbove, kNetWmStateHidden,
  kNetWmStateSkipTaskbar, kNetWmStateFocused, kNetWmWindowType, kNetWmWindowTypeNormal, kNetWmWindowTypeDialog, kNetWmWindowTypeUtility,
  kNetWmWindowTypeToolbar, kNetWmWindowTypeMenu, kNetWmWindowTypePopupMenu, kNetWmWindowTypeDropdownMenu, kNetWmWindowTypeTooltip,
  kNetWmWindowTypeNotification, kNetWmWindowTypeCombo, kNetWmWindowTypeDnd, kNetWmWindowTypeSplash, kNetWmWindowTypeDock, kNetWmWindowTypeDesktop,
  kNetWmIcon, kNetWmPid, kNetFrameExtents, kNetRequestFrameExtents, kNetCloseWindow, kNetWmMoveresize, kNetMoveresizeWindow, kMotifWmHints,
  kNetWmAllowedActions, kNetWmActionMove, kNetWmActionResize, kNetWmActionMinimize, kNetWmActionMaximizeHorz, kNetWmActionMaximizeVert,
  kNetWmActionClose, kNetWmActionFullscreen, kNetWmPing, kGtkShowWindowMenu, kLastWellKnown
};

constexpr const char* kWellKnownNames[] = {
    "WM_PROTOCOLS", "WM_DELETE_WINDOW", "WM_TAKE_FOCUS", "WM_STATE", "WM_CHANGE_STATE", "UTF8_STRING", "_NET_WM_NAME", "_NET_SUPPORTED",
    "_NET_SUPPORTING_WM_CHECK", "_NET_ACTIVE_WINDOW", "_NET_CLIENT_LIST", "_NET_CLIENT_LIST_STACKING", "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP",
    "_NET_DESKTOP_GEOMETRY", "_NET_WORKAREA", "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_FULLSCREEN",
    "_NET_WM_STATE_ABOVE", "_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_FOCUSED", "_NET_WM_WINDOW_TYPE",
    "_NET_WM_WINDOW_TYPE_NORMAL", "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_UTILITY", "_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_MENU",
    "_NET_WM_WINDOW_TYPE_POPUP_MENU", "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", "_NET_WM_WINDOW_TYPE_TOOLTIP", "_NET_WM_WINDOW_TYPE_NOTIFICATION",
    "_NET_WM_WINDOW_TYPE_COMBO", "_NET_WM_WINDOW_TYPE_DND", "_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP",
    "_NET_WM_ICON", "_NET_WM_PID", "_NET_FRAME_EXTENTS", "_NET_REQUEST_FRAME_EXTENTS", "_NET_CLOSE_WINDOW", "_NET_WM_MOVERESIZE",
    "_NET_MOVERESIZE_WINDOW", "_MOTIF_WM_HINTS", "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_MOVE", "_NET_WM_ACTION_RESIZE", "_NET_WM_ACTION_MINIMIZE",
    "_NET_WM_ACTION_MAXIMIZE_HORZ", "_NET_WM_ACTION_MAXIMIZE_VERT", "_NET_WM_ACTION_CLOSE", "_NET_WM_ACTION_FULLSCREEN", "_NET_WM_PING",
    "_GTK_SHOW_WINDOW_MENU"};
static_assert(sizeof kWellKnownNames / sizeof kWellKnownNames[0] == kLastWellKnown - kWmProtocols, "one name per well-known atom");

constexpr uint32_t kMaxChanges = 256;

}  // namespace

Server::Server(allocators::IAllocator& allocator, Clock clock)
    : allocator_(allocator), clock_(clock), resources_(allocator), windows_(allocator), gcs_(allocator), pixmaps_(allocator), cursors_(allocator),
      fonts_(allocator), atoms_(allocator), selection_owners_(allocator), changes_(allocator) {
  for (const char* name : kPredefinedAtoms) intern(name);
  for (const char* name : kWellKnownNames) intern(name);
  root_ = windows_.emplace(allocator_);
  if (root_) {
    root_->id = kRootWindow;
    root_->mapped = true;
    root_->width = screen_w_;
    root_->height = screen_h_;
    root_->background = 1;
    root_->background_pixel = 0xFF000000u;
    Resource r;
    r.kind = Kind::Window;
    r.window = root_;
    resources_.put(kRootWindow, r);
  }
  Resource colormap;
  colormap.kind = Kind::Colormap;
  resources_.put(kDefaultColormap, colormap);
  // the window manager's check window (EWMH): an unmapped child of the root naming the window manager
  wm_check_ = windows_.emplace(allocator_);
  if (root_ && wm_check_) {
    wm_check_->id = kWmCheckWindow;
    wm_check_->parent = root_;
    wm_check_->input_only = true;
    wm_check_->depth = 0;
    wm_check_->x = wm_check_->y = -100;
    root_->children.push_back(wm_check_);
    Resource r;
    r.kind = Kind::Window;
    r.window = wm_check_;
    resources_.put(kWmCheckWindow, r);
    const uint32_t check[] = {kWmCheckWindow};
    set_property32(*root_, kNetSupportingWmCheck, atom::kWindow, check);
    set_property32(*wm_check_, kNetSupportingWmCheck, atom::kWindow, check);
    set_property(*wm_check_, kNetWmName, kUtf8String, 8, StringView("TheoryCraftOS").bytes());
    uint32_t supported[kLastWellKnown - kWmProtocols];
    uint32_t n = 0;
    for (uint32_t a = kNetSupported; a < kLastWellKnown; ++a) supported[n++] = a;
    set_property32(*root_, kNetSupported, atom::kAtom, ArrayView<const uint32_t>(supported, n));
    const uint32_t one[] = {1}, zero[] = {0};
    set_property32(*root_, kNetNumberOfDesktops, atom::kCardinal, one);
    set_property32(*root_, kNetCurrentDesktop, atom::kCardinal, zero);
    set_property32(*root_, kNetActiveWindow, atom::kWindow, zero);
  }
  monitors_[0] = {{0, 0, static_cast<int32_t>(screen_w_), static_cast<int32_t>(screen_h_)}, {0, 0, static_cast<int32_t>(screen_w_), static_cast<int32_t>(screen_h_)}};
  monitor_count_ = 1;
  update_root_lists();
}

Server::~Server() = default;

void Server::set_wake(Wake wake, void* context) {
  kernel::ScopedMutex lock(mutex_);
  wake_ = wake;
  wake_context_ = context;
}

void Server::wake() {
  Wake w = nullptr;
  void* context = nullptr;
  {
    kernel::ScopedMutex lock(mutex_);
    w = wake_;
    context = wake_context_;
  }
  if (w) w(context);
}

uint32_t Server::now() {
  const uint64_t ms = clock_ ? clock_() : (fake_clock_ += 1);
  last_time_ = static_cast<uint32_t>(ms);
  if (last_time_ == 0) last_time_ = 1;      // (0 is CurrentTime)
  return last_time_;
}

uint32_t Server::intern(StringView name) {
  for (uint64_t i = 0; i < atoms_.count(); ++i)
    if (atoms_[i].view().equals(name)) return static_cast<uint32_t>(i + 1);
  String s(allocator_);
  if (!s.assign(name) || !atoms_.push_back(util::move(s))) return 0;
  return static_cast<uint32_t>(atoms_.count());
}

StringView Server::atom_name(uint32_t atom) const {
  if (atom == 0 || atom > atoms_.count()) return StringView();
  return atoms_[atom - 1].view();
}

// ------------------------------------------------------------------ clients

uint32_t Server::open_client(uint64_t pid) {
  kernel::ScopedMutex lock(mutex_);
  for (uint32_t i = 1; i < kMaxClients; ++i) {
    if (clients_[i]) continue;
    clients_[i] = UniquePointer<Client>::make(allocator_, allocator_);
    if (!clients_[i]) return 0;
    clients_[i]->index = static_cast<uint8_t>(i);
    clients_[i]->pid = pid;
    return i;
  }
  return 0;
}

void Server::receive(uint32_t client, ConstByteView bytes) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (client == 0 || client >= kMaxClients || !clients_[client]) return;
    Client& c = *clients_[client];
    if (c.closing) return;
    if (!c.in.append(bytes)) {
      c.closing = true;
      return;
    }
    process(c);
  }
}

bool Server::take_output(uint32_t client, ByteBuffer* out) {
  kernel::ScopedMutex lock(mutex_);
  if (client == 0 || client >= kMaxClients || !clients_[client]) return true;
  Client& c = *clients_[client];
  if (c.out.size() > 0) {
    if (!out->append(c.out.view())) return true;
    c.out.clear();
  }
  return c.closing;
}

bool Server::has_output(uint32_t client) {
  kernel::ScopedMutex lock(mutex_);
  if (client == 0 || client >= kMaxClients || !clients_[client]) return false;
  return clients_[client]->out.size() > 0 || clients_[client]->closing;
}

uint32_t Server::client_count() {
  kernel::ScopedMutex lock(mutex_);
  uint32_t n = 0;
  for (const UniquePointer<Client>& c : clients_) if (c) ++n;
  return n;
}

void Server::close_client(uint32_t client) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (client == 0 || client >= kMaxClients || !clients_[client]) return;
    free_client_resources(static_cast<uint8_t>(client));
    clients_[client].reset();
  }
  wake();      // (the others may have been sent events: DestroyNotify, SelectionClear, focus)
}

// Everything the client made goes: its windows (with whatever is inside them), then the rest; its grabs, selections and event selections end.
void Server::free_client_resources(uint8_t client) {
  release_grabs_of(client);
  for (uint64_t i = 0; i < selection_owners_.count();) {
    if (selection_owners_[i].client == client) selection_owners_.remove_at(i);
    else ++i;
  }
  List<uint32_t> ids(allocator_);
  ObjectCursor cursor;
  for (bool more = resources_.first(&cursor); more; more = resources_.next(&cursor)) {
    const KeyValue<uint32_t, Resource>* kv = resources_.element(cursor);
    if (kv->value.owner == client && kv->value.kind == Kind::Window) ids.push_back(kv->key);
  }
  for (uint32_t id : ids.view()) free_resource(id);      // (a window already gone with its parent is skipped)
  ids.clear();
  for (bool more = resources_.first(&cursor); more; more = resources_.next(&cursor)) {
    const KeyValue<uint32_t, Resource>* kv = resources_.element(cursor);
    if (kv->value.owner == client) ids.push_back(kv->key);
  }
  for (uint32_t id : ids.view()) free_resource(id);
  // the event selections it had on other clients' windows
  for (Window& w : windows_) {
    for (uint64_t i = 0; i < w.selections.count();) {
      if (w.selections[i].client == client) w.selections.remove_at(i);
      else ++i;
    }
  }
}

void Server::free_resource(uint32_t id) {
  Resource* r = resources_.find(id);
  if (!r) return;
  switch (r->kind) {
    case Kind::Window:
      if (r->window != root_ && r->window != wm_check_) destroy_window(*r->window);
      return;
    case Kind::Pixmap: {
      Pixmap* p = r->pixmap;
      resources_.remove(id);
      pixmaps_.erase(p);
      return;
    }
    case Kind::Gc: {
      Gc* g = r->gc;
      resources_.remove(id);
      gcs_.erase(g);
      return;
    }
    case Kind::Cursor: {
      Cursor* k = r->cursor;
      resources_.remove(id);
      cursors_.erase(k);
      return;
    }
    case Kind::Font: {
      Font* f = r->font;
      resources_.remove(id);
      fonts_.erase(f);
      return;
    }
    case Kind::Colormap:
      if (id != kDefaultColormap) resources_.remove(id);
      return;
  }
}

// ------------------------------------------------------------------ the window tree

Server::Window* Server::find_window(uint32_t id) {
  Resource* r = resources_.find(id);
  return r && r->kind == Kind::Window ? r->window : nullptr;
}

Server::Window* Server::top_of(Window* w) {
  if (!w || w == root_) return nullptr;
  while (w->parent && w->parent != root_) w = w->parent;
  return w->parent == root_ ? w : nullptr;
}

bool Server::viewable(const Window* w) const {
  for (; w; w = w->parent) if (!w->mapped) return false;
  return true;
}

bool Server::is_ancestor(const Window* ancestor, const Window* w) const {
  for (; w; w = w->parent) if (w == ancestor) return true;
  return false;
}

void Server::inside_origin(const Window* w, int32_t* x, int32_t* y) const {
  int32_t ax = 0, ay = 0;
  for (; w && w != root_; w = w->parent) {
    ax += w->x;
    ay += w->y;
    if (w->parent != root_) {      // (a top-level's position is its inside's already)
      ax += static_cast<int32_t>(w->border);
      ay += static_cast<int32_t>(w->border);
    }
  }
  *x = ax;
  *y = ay;
}

bool Server::selected(const Window& w, uint32_t event_mask) const {
  for (const Selection& s : w.selections.view()) if (s.mask & event_mask) return true;
  return false;
}

uint32_t Server::event_mask_of(const Window& w, uint8_t client) const {
  for (const Selection& s : w.selections.view()) if (s.client == client) return s.mask;
  return 0;
}

void Server::send(Client& c, const Packet& p, bool with_sequence) {
  if (c.closing && c.out.size() > kMaxOutputBytes) return;
  Packet q = p;
  if (with_sequence) q.u16(2, c.sequence);
  bool ok = true;
  q.write(c.out, c.big, &ok);
  if (!ok || c.out.size() > kMaxOutputBytes) c.closing = true;
}

void Server::deliver(Window& w, uint32_t event_mask, const Packet& p, uint8_t only_client) {
  for (const Selection& s : w.selections.view()) {
    if (!(s.mask & event_mask) || (only_client && s.client != only_client)) continue;
    if (s.client < kMaxClients && clients_[s.client] && clients_[s.client]->setup_done) send(*clients_[s.client], p);
  }
}

void Server::structure_event(Window& w, Packet& p, uint32_t event_offset) {
  p.u32(event_offset, w.id);
  deliver(w, mask::kStructureNotify, p);
  if (w.parent) {
    p.u32(event_offset, w.parent->id);
    deliver(*w.parent, mask::kSubstructureNotify, p);
  }
}

void Server::map_window(Window& w) {
  if (w.mapped || &w == root_) return;
  const bool top_level = w.parent == root_;
  if (top_level && !w.input_only) {
    place(w);
    // what the client asked for before mapping (EWMH: _NET_WM_STATE set on a withdrawn window)
    if (Property* state = find_property(w, kNetWmState); state && state->format == 32) {
      ByteReader in(state->data.view());
      while (in.remaining() >= 4) {
        const uint32_t a = in.le32();
        if (a == kNetWmStateMaximizedVert || a == kNetWmStateMaximizedHorz) w.maximized = true;
        else if (a == kNetWmStateFullscreen) w.fullscreen = true;
        else if (a == kNetWmStateAbove) w.above = true;
        else if (a == kNetWmStateHidden) w.iconic = true;
      }
    }
    // WM_HINTS initial state Iconic
    if (Property* hints = find_property(w, atom::kWmHints); hints && hints->format == 32 && hints->data.size() >= 12) {
      ByteReader in(hints->data.view());
      const uint32_t flags = in.le32();
      in.le32();
      const uint32_t initial = in.le32();
      if ((flags & 2) && initial == 3) w.iconic = true;
    }
  }
  w.mapped = true;
  Packet p;
  p.u8(0, event::kMapNotify);
  p.u32(8, w.id);
  p.u8(12, w.override_redirect ? 1 : 0);
  structure_event(w, p, 4);
  if (top_level && !w.input_only) {
    ensure_picture(w);
    if (w.maximized || w.fullscreen) {
      const bool max = w.maximized, full = w.fullscreen;
      w.maximized = w.fullscreen = false;
      if (full) set_fullscreen(w, true);
      else if (max) set_maximized(w, true);
    }
    update_wm_state(w);
    note_changed(w);
    update_root_lists();
    if (!w.override_redirect && !w.iconic) {      // (as window managers do: a newly mapped window gets the focus)
      bool takes_input = true;
      if (Property* hints = find_property(w, atom::kWmHints); hints && hints->format == 32 && hints->data.size() >= 8) {
        ByteReader in(hints->data.view());
        const uint32_t flags = in.le32(), input = in.le32();
        if ((flags & 1) && input == 0) takes_input = false;
      }
      if (takes_input) set_focus(w.id, 1);
      if (has_protocol(w, kWmTakeFocus)) send_client_message(w, kWmProtocols, kWmTakeFocus, now());
    }
  }
  if (viewable(&w)) expose_tree(w);
  pointer_moved();
}

void Server::unmap_window(Window& w, bool from_destroy) {
  if (!w.mapped || &w == root_) return;
  const bool was_viewable = viewable(&w);
  int32_t ox = 0, oy = 0;
  const Rect outer{w.x, w.y, static_cast<int32_t>(w.width + 2 * w.border), static_cast<int32_t>(w.height + 2 * w.border)};
  (void)ox;
  (void)oy;
  w.mapped = false;
  Packet p;
  p.u8(0, event::kUnmapNotify);
  p.u32(8, w.id);
  p.u8(12, 0);
  structure_event(w, p, 4);
  if (w.parent == root_) {
    note_changed(w);
    if (!from_destroy) update_wm_state(w);
    update_root_lists();
  } else if (was_viewable && w.parent) {
    expose_area(*w.parent, outer);
  }
  // the focus inside it reverts (as the focus' revert-to says), a grab inside it ends
  if (Window* f = find_window(focus_); f && is_ancestor(&w, f)) {
    if (focus_revert_ == 2 && w.parent && w.parent != root_ && viewable(w.parent)) set_focus(w.parent->id, focus_revert_);
    else set_focus(focus_revert_ == 0 ? 0 : 1, focus_revert_);
  }
  if (Window* g = find_window(pointer_grab_.window); pointer_grab_.active && g && is_ancestor(&w, g)) pointer_grab_ = Grab();
  if (Window* g = find_window(keyboard_grab_.window); keyboard_grab_.active && g && is_ancestor(&w, g)) keyboard_grab_ = Grab();
  pointer_moved();
}

void Server::destroy_window(Window& w) {
  if (&w == root_ || &w == wm_check_) return;
  if (w.mapped) unmap_window(w, true);
  destroy_tree(w);
  pointer_moved();
}

void Server::destroy_tree(Window& w) {
  while (w.children.count() > 0) destroy_tree(*w.children[w.children.count() - 1]);
  Packet p;
  p.u8(0, event::kDestroyNotify);
  p.u32(8, w.id);
  structure_event(w, p, 4);
  if (w.parent) {
    List<Window*>& siblings = w.parent->children;
    for (uint64_t i = 0; i < siblings.count(); ++i) {
      if (siblings[i] == &w) {
        siblings.remove_at(i);
        break;
      }
    }
    if (w.parent == root_) {
      note_changed(w);      // (the desktop asks and hears that it is gone)
      update_root_lists();
    }
  }
  if (focus_ == w.id) set_focus(focus_revert_ == 2 && w.parent && w.parent != root_ ? w.parent->id : (focus_revert_ == 0 ? 0 : 1), focus_revert_);
  if (pointer_grab_.window == w.id) pointer_grab_ = Grab();
  if (keyboard_grab_.window == w.id) keyboard_grab_ = Grab();
  for (SelectionOwner& s : selection_owners_.view()) if (s.window == w.id) s.window = 0;
  if (drag_window_ == w.id) drag_window_ = 0;
  if (menu_window_ == w.id) menu_window_ = 0;
  if (pointer_window_ == w.id) pointer_window_ = w.parent ? w.parent->id : kRootWindow;
  resources_.remove(w.id);
  windows_.erase(&w);
}

void Server::restack(Window& w, Window* sibling, uint32_t mode) {
  if (!w.parent) return;
  List<Window*>& list = w.parent->children;
  uint64_t at = 0;
  for (uint64_t i = 0; i < list.count(); ++i) if (list[i] == &w) at = i;
  list.remove_at(at);
  uint64_t sib = list.count();
  if (sibling) for (uint64_t i = 0; i < list.count(); ++i) if (list[i] == sibling) sib = i;
  const bool raise = mode == 0 || mode == 2 || mode == 4;      // Above, TopIf, Opposite: up; Below, BottomIf: down
  uint64_t to;
  if (sibling && sib < list.count()) to = raise ? sib + 1 : sib;
  else to = raise ? list.count() : 0;
  list.insert(to, &w);
  if (w.parent == root_) {
    note_changed(w);
    update_root_lists();
  } else if (viewable(&w)) {
    expose_area(*w.parent, {w.x, w.y, static_cast<int32_t>(w.width + 2 * w.border), static_cast<int32_t>(w.height + 2 * w.border)});
  }
}

void Server::move_resize(Window& w, int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t border, bool from_desktop) {
  (void)from_desktop;
  if (width == 0) width = 1;
  if (height == 0) height = 1;
  if (width > 32767) width = 32767;
  if (height > 32767) height = 32767;
  const Rect old{w.x, w.y, static_cast<int32_t>(w.width + 2 * w.border), static_cast<int32_t>(w.height + 2 * w.border)};
  const bool sized = width != w.width || height != w.height || border != w.border;
  const bool moved = x != w.x || y != w.y;
  if (!sized && !moved) return;
  w.x = x;
  w.y = y;
  w.width = width;
  w.height = height;
  w.border = border;
  if (w.parent == root_) {
    if (sized) {
      ensure_picture(w);
      if (viewable(&w)) expose_tree(w);
    }
    note_changed(w);
  } else if (w.parent && viewable(w.parent)) {
    expose_area(*w.parent, old);
    expose_area(*w.parent, {w.x, w.y, static_cast<int32_t>(w.width + 2 * w.border), static_cast<int32_t>(w.height + 2 * w.border)});
  }
  pointer_moved();
}

void Server::send_configure_notify(Window& w) {
  Packet p;
  p.u8(0, event::kConfigureNotify);
  p.u32(8, w.id);
  uint32_t below = 0;
  if (w.parent) {
    const List<Window*>& list = w.parent->children;
    for (uint64_t i = 0; i < list.count(); ++i) if (list[i] == &w && i > 0) below = list[i - 1]->id;
  }
  p.u32(12, below);
  p.i16(16, w.x);
  p.i16(18, w.y);
  p.u16(20, static_cast<uint16_t>(w.width));
  p.u16(22, static_cast<uint16_t>(w.height));
  p.u16(24, static_cast<uint16_t>(w.border));
  p.u8(26, w.override_redirect ? 1 : 0);
  structure_event(w, p, 4);
}

// ------------------------------------------------------------------ properties

Server::Property* Server::find_property(Window& w, uint32_t atom) {
  for (UniquePointer<Property>& p : w.properties.view()) if (p->name == atom) return p.get();
  return nullptr;
}

void Server::set_property(Window& w, uint32_t atom, uint32_t type, uint8_t format, ConstByteView data) {
  Property* p = find_property(w, atom);
  if (!p) {
    UniquePointer<Property> made = UniquePointer<Property>::make(allocator_, allocator_);
    if (!made || !w.properties.push_back(util::move(made))) return;
    p = w.properties[w.properties.count() - 1].get();
    p->name = atom;
  }
  p->type = type;
  p->format = format;
  p->data.clear();
  p->data.append(data);
  property_changed(w, atom, false);
}

void Server::set_property32(Window& w, uint32_t atom, uint32_t type, ArrayView<const uint32_t> values) {
  ByteBuffer bytes(allocator_);
  for (uint32_t v : values) bytes.put32(v);
  set_property(w, atom, type, 32, bytes.view());
}

void Server::delete_property(Window& w, uint32_t atom) {
  for (uint64_t i = 0; i < w.properties.count(); ++i) {
    if (w.properties[i]->name != atom) continue;
    w.properties.remove_at(i);
    property_changed(w, atom, true);
    return;
  }
}

void Server::property_changed(Window& w, uint32_t atom, bool deleted) {
  Packet p;
  p.u8(0, event::kPropertyNotify);
  p.u32(4, w.id);
  p.u32(8, atom);
  p.u32(12, now());
  p.u8(16, deleted ? 1 : 0);
  deliver(w, mask::kPropertyChange, p);
  if (w.parent != root_ || w.input_only) return;
  switch (atom) {
    case kNetWmIcon:
      w.icon = deleted ? 0 : next_icon_++;
      if (next_icon_ == 0) next_icon_ = 0x80000001u;
      note_changed(w);
      break;
    case atom::kWmName: case kNetWmName: case atom::kWmHints: case atom::kWmNormalHints: case kMotifWmHints: case atom::kWmTransientFor:
    case kNetWmWindowType: case kWmProtocols:
      note_changed(w);
      break;
    default: break;
  }
}

bool Server::has_protocol(Window& w, uint32_t protocol) {
  Property* p = find_property(w, kWmProtocols);
  if (!p || p->format != 32) return false;
  ByteReader in(p->data.view());
  while (in.remaining() >= 4) if (in.le32() == protocol) return true;
  return false;
}

void Server::send_client_message(Window& w, uint32_t type, uint32_t d0, uint32_t d1) {
  if (w.owner == 0 || w.owner >= kMaxClients || !clients_[w.owner]) return;
  Packet p;
  p.u8(0, event::kClientMessage);
  p.u8(1, 32);
  p.u32(4, w.id);
  p.u32(8, type);
  p.u32(12, d0);
  p.u32(16, d1);
  p.u32(20, 0);
  p.u32(24, 0);
  p.u32(28, 0);
  send(*clients_[w.owner], p);
}

// ------------------------------------------------------------------ the window manager

namespace {

// A CARD32 of a format-32 property (little-endian units), false when there is none at that index.
bool word_at(ConstByteView data, uint32_t index, uint32_t* out) {
  if (data.size() / 4 <= index) return false;
  ByteReader in(data.subview(uint64_t{index} * 4, 4));
  *out = in.le32();
  return true;
}

}  // namespace

bool Server::property32(Window& w, uint32_t atom, uint32_t index, uint32_t* out) {
  Property* p = find_property(w, atom);
  return p && p->format == 32 && word_at(p->data.view(), index, out);
}

uint32_t Server::window_type(Window& w) {
  uint32_t type = 0;
  return property32(w, kNetWmWindowType, 0, &type) ? type : 0;
}

bool Server::decorated(Window& top) {
  if (top.override_redirect || top.fullscreen) return false;
  const uint32_t type = window_type(top);
  if (type == kNetWmWindowTypeDock || type == kNetWmWindowTypeDesktop || type == kNetWmWindowTypeSplash || type == kNetWmWindowTypeMenu ||
      type == kNetWmWindowTypePopupMenu || type == kNetWmWindowTypeDropdownMenu || type == kNetWmWindowTypeTooltip ||
      type == kNetWmWindowTypeNotification || type == kNetWmWindowTypeCombo || type == kNetWmWindowTypeDnd)
    return false;
  uint32_t flags = 0, decorations = 0;
  if (property32(top, kMotifWmHints, 0, &flags) && (flags & 2) && property32(top, kMotifWmHints, 2, &decorations))
    return (decorations & (1u | 8u)) != 0;      // MWM_DECOR_ALL or MWM_DECOR_TITLE
  return true;
}

void Server::frame_margins(Window& top, int32_t* left, int32_t* top_margin, int32_t* right, int32_t* bottom) {
  const bool d = attached_ && decorated(top);
  *left = *right = *bottom = d ? frame_border_ : 0;
  *top_margin = d ? frame_border_ + frame_title_ : 0;
}

void Server::place(Window& top) {
  if (top.placed) return;
  top.placed = true;
  int32_t l, t, r, b;
  frame_margins(top, &l, &t, &r, &b);
  uint32_t flags = 0;
  property32(top, atom::kWmNormalHints, 0, &flags);
  const bool positioned = (flags & (1u | 4u)) != 0 || top.override_redirect || top.x != 0 || top.y != 0;
  if (positioned) {
    if (top.override_redirect || top.win_gravity == 10) return;      // (the inside goes where it was asked to: no frame, or StaticGravity)
    top.x += l;
    top.y += t;
    return;
  }
  // a cascade over the primary monitor's work area
  const Rect work = monitors_[0].work;
  const int32_t fw = static_cast<int32_t>(top.width) + l + r, fh = static_cast<int32_t>(top.height) + t + b;
  int32_t x = work.x + (work.w - fw) / 2, y = work.y + (work.h - fh) / 3;
  const int32_t step = 24 * static_cast<int32_t>(place_counter_++ % 8);
  x += step - 84;
  y += step - 84;
  if (x + fw > work.x + work.w) x = work.x + work.w - fw;
  if (y + fh > work.y + work.h) y = work.y + work.h - fh;
  if (x < work.x) x = work.x;
  if (y < work.y) y = work.y;
  top.x = x + l;
  top.y = y + t;
}

void Server::update_wm_state(Window& top) {
  const uint32_t state[2] = {top.mapped ? (top.iconic ? 3u : 1u) : 0u, 0};
  set_property32(top, kWmState, kWmState, state);
  uint32_t net[6];
  uint32_t n = 0;
  if (top.maximized) { net[n++] = kNetWmStateMaximizedVert; net[n++] = kNetWmStateMaximizedHorz; }
  if (top.fullscreen) net[n++] = kNetWmStateFullscreen;
  if (top.above) net[n++] = kNetWmStateAbove;
  if (top.iconic) net[n++] = kNetWmStateHidden;
  if (top.mapped && focus_ != 0 && focus_ != 1 && top_of(find_window(focus_)) == &top) net[n++] = kNetWmStateFocused;
  set_property32(top, kNetWmState, atom::kAtom, ArrayView<const uint32_t>(net, n));
  int32_t l, t, r, b;
  frame_margins(top, &l, &t, &r, &b);
  const uint32_t extents[4] = {static_cast<uint32_t>(l), static_cast<uint32_t>(r), static_cast<uint32_t>(t), static_cast<uint32_t>(b)};
  set_property32(top, kNetFrameExtents, atom::kCardinal, extents);
  const uint32_t actions[] = {kNetWmActionMove, kNetWmActionResize, kNetWmActionMinimize, kNetWmActionMaximizeHorz, kNetWmActionMaximizeVert,
                              kNetWmActionClose, kNetWmActionFullscreen};
  set_property32(top, kNetWmAllowedActions, atom::kAtom, actions);
}

void Server::update_root_lists() {
  if (!root_) return;
  List<uint32_t> list(allocator_);
  for (Window* w : root_->children.view()) if (w->mapped && !w->input_only && !w->override_redirect) list.push_back(w->id);
  set_property32(*root_, kNetClientList, atom::kWindow, list.view());
  set_property32(*root_, kNetClientListStacking, atom::kWindow, list.view());
  const uint32_t geometry[2] = {screen_w_, screen_h_};
  set_property32(*root_, kNetDesktopGeometry, atom::kCardinal, geometry);
  const Rect work = monitors_[0].work;
  const uint32_t area[4] = {static_cast<uint32_t>(work.x), static_cast<uint32_t>(work.y), static_cast<uint32_t>(work.w), static_cast<uint32_t>(work.h)};
  set_property32(*root_, kNetWorkarea, atom::kCardinal, area);
}

void Server::set_focus(uint32_t focus, uint8_t revert_to) {
  focus_revert_ = revert_to;
  if (focus == focus_) return;
  Window* old_window = focus_ > 1 ? find_window(focus_) : nullptr;
  Window* new_window = focus > 1 ? find_window(focus) : nullptr;
  if (focus > 1 && !new_window) focus = 1;
  Window* old_top = top_of(old_window);
  Window* new_top = top_of(new_window);
  focus_ = focus;
  Packet out;
  out.u8(0, event::kFocusOut);
  out.u8(1, 3);      // Nonlinear
  out.u8(8, 0);
  if (old_window) {
    out.u32(4, old_window->id);
    deliver(*old_window, mask::kFocusChange, out);
  }
  Packet in = out;
  in.u8(0, event::kFocusIn);
  if (new_window) {
    in.u32(4, new_window->id);
    deliver(*new_window, mask::kFocusChange, in);
  }
  if (old_top != new_top) {
    if (old_top) { note_changed(*old_top); update_wm_state(*old_top); }
    if (new_top) { note_changed(*new_top); update_wm_state(*new_top); }
    const uint32_t active[1] = {new_top ? new_top->id : 0};
    if (root_) set_property32(*root_, kNetActiveWindow, atom::kWindow, active);
  }
}

void Server::ensure_picture(Window& top) {
  if (top.input_only || top.parent != root_) return;
  uint32_t w = top.width, h = top.height;
  if (uint64_t{w} * h > kMaxPicturePixels) h = static_cast<uint32_t>(kMaxPicturePixels / w);
  if (top.picture && top.picture->width == w && top.picture->height == h) return;
  SharedPointer<Picture> fresh = SharedPointer<Picture>::make(allocator_, allocator_);
  if (!fresh || !fresh->pixels.reserve(uint64_t{w} * h)) {
    top.picture = SharedPointer<Picture>();
    return;
  }
  fresh->width = w;
  fresh->height = h;
  fresh->depth = top.depth;
  const uint32_t fill = top.background == 1 ? top.background_pixel : stored_pixel(top.depth, 0);
  for (uint64_t i = 0; i < uint64_t{w} * h; ++i) fresh->pixels.push_back(fill);
  if (top.picture && top.bit_gravity != 0) {      // (not ForgetGravity: what was there stays at the top left)
    const Picture& old = *top.picture;
    const uint32_t cw = old.width < w ? old.width : w, ch = old.height < h ? old.height : h;
    for (uint32_t y = 0; y < ch; ++y)
      for (uint32_t x = 0; x < cw; ++x) fresh->pixels[uint64_t{y} * w + x] = old.pixels[uint64_t{y} * old.width + x];
  }
  top.picture = util::move(fresh);
  note_repaint(top, {0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)});
}

Server::Change* Server::change_of(uint32_t window) {
  for (Change& c : changes_.view()) if (c.window == window) return &c;
  if (changes_.count() >= kMaxChanges) {
    changes_overflow_ = true;
    return nullptr;
  }
  Change c;
  c.window = window;
  if (!changes_.push_back(c)) {
    changes_overflow_ = true;
    return nullptr;
  }
  return &changes_[changes_.count() - 1];
}

void Server::note_changed(Window& top) {
  if (Change* c = change_of(top.id)) c->changed = true;
}

void Server::note_repaint(Window& top, const Rect& dirty) {
  if (dirty.empty()) return;
  if (Change* c = change_of(top.id)) {
    c->dirty = c->repaint ? unite(c->dirty, dirty) : dirty;
    c->repaint = true;
  }
}

Rect Server::work_area_for(Window& top) {
  int32_t cx = top.x + static_cast<int32_t>(top.width / 2), cy = top.y + static_cast<int32_t>(top.height / 2);
  for (uint32_t i = 0; i < monitor_count_; ++i)
    if (contains(monitors_[i].rect, cx, cy)) return monitors_[i].work;
  return monitors_[0].work;
}

void Server::set_maximized(Window& top, bool on) {
  if (top.maximized == on) return;
  if (on) {
    if (!top.fullscreen) top.restore = {top.x, top.y, static_cast<int32_t>(top.width), static_cast<int32_t>(top.height)};
    const Rect work = work_area_for(top);
    int32_t l, t, r, b;
    top.maximized = true;
    frame_margins(top, &l, &t, &r, &b);
    if (!top.fullscreen)
      move_resize(top, work.x + l, work.y + t, static_cast<uint32_t>(max32(1, work.w - l - r)), static_cast<uint32_t>(max32(1, work.h - t - b)), top.border, true);
  } else {
    top.maximized = false;
    if (!top.fullscreen && !top.restore.empty())
      move_resize(top, top.restore.x, top.restore.y, static_cast<uint32_t>(top.restore.w), static_cast<uint32_t>(top.restore.h), top.border, true);
  }
  send_configure_notify(top);
  update_wm_state(top);
  note_changed(top);
}

void Server::set_fullscreen(Window& top, bool on) {
  if (top.fullscreen == on) return;
  if (on) {
    if (!top.maximized) top.restore = {top.x, top.y, static_cast<int32_t>(top.width), static_cast<int32_t>(top.height)};
    int32_t cx = top.x + static_cast<int32_t>(top.width / 2), cy = top.y + static_cast<int32_t>(top.height / 2);
    Rect screen = monitors_[0].rect;
    for (uint32_t i = 0; i < monitor_count_; ++i) if (contains(monitors_[i].rect, cx, cy)) screen = monitors_[i].rect;
    top.fullscreen = true;
    move_resize(top, screen.x, screen.y, static_cast<uint32_t>(screen.w), static_cast<uint32_t>(screen.h), top.border, true);
  } else {
    top.fullscreen = false;
    if (top.maximized) {
      top.maximized = false;
      const Rect keep = top.restore;
      set_maximized(top, true);
      top.restore = keep;
    } else if (!top.restore.empty()) {
      move_resize(top, top.restore.x, top.restore.y, static_cast<uint32_t>(top.restore.w), static_cast<uint32_t>(top.restore.h), top.border, true);
    }
  }
  send_configure_notify(top);
  update_wm_state(top);
  note_changed(top);
}

void Server::set_iconic(Window& top, bool on) {
  if (top.iconic == on) return;
  top.iconic = on;
  update_wm_state(top);
  note_changed(top);
}

void Server::close_request(Window& top) {
  if (has_protocol(top, kWmDeleteWindow)) {
    send_client_message(top, kWmProtocols, kWmDeleteWindow, now());
    return;
  }
  // no WM_DELETE_WINDOW: the client is disconnected (XKillClient), as window managers do
  if (top.owner != 0 && top.owner < kMaxClients && clients_[top.owner]) {
    free_client_resources(top.owner);
    clients_[top.owner]->closing = true;
  }
}

// A client message to the root that a window manager answers (EWMH / ICCCM): `window` is the message's window.
void Server::window_manager_message(uint32_t window, uint32_t type, const uint32_t (&data)[5]) {
  Window* w = find_window(window);
  Window* top = w && w->parent == root_ ? w : nullptr;
  switch (type) {
    case kNetWmState: {
      if (!top) return;
      for (uint32_t i = 1; i <= 2; ++i) {
        const uint32_t a = data[i];
        auto want = [&](bool current) { return data[0] == 2 ? !current : data[0] == 1; };
        if (a == kNetWmStateMaximizedVert || a == kNetWmStateMaximizedHorz) {
          if (i == 2 && data[1] != a && (data[1] == kNetWmStateMaximizedVert || data[1] == kNetWmStateMaximizedHorz)) continue;      // (the pair is one change)
          set_maximized(*top, want(top->maximized));
        } else if (a == kNetWmStateFullscreen) {
          set_fullscreen(*top, want(top->fullscreen));
        } else if (a == kNetWmStateAbove) {
          top->above = want(top->above);
          update_wm_state(*top);
          note_changed(*top);
        } else if (a == kNetWmStateHidden) {
          set_iconic(*top, want(top->iconic));
        }
      }
      return;
    }
    case kWmChangeState:
      if (top && data[0] == 3) set_iconic(*top, true);
      return;
    case kNetActiveWindow:
      if (!top || !top->mapped) return;
      set_iconic(*top, false);
      restack(*top, nullptr, 0);
      set_focus(top->id, 1);
      return;
    case kNetCloseWindow:
      if (top) close_request(*top);
      return;
    case kNetWmMoveresize: {
      if (!top || top->maximized || top->fullscreen) return;
      constexpr uint32_t kEdges[9] = {4, 3, 5, 2, 8, 6, 7, 1, 9};      // EWMH direction -> the desktop's edge (9: move)
      if (data[2] <= 8) {
        drag_window_ = top->id;
        drag_edge_ = kEdges[data[2]];
        // the button that started it is the desktop's now: the client's implicit grab ends
        if (pointer_grab_.active && pointer_grab_.implicit) pointer_grab_ = Grab();
      } else if (data[2] == 11 && drag_window_ == top->id) {
        drag_window_ = 0;
      }
      return;
    }
    case kNetMoveresizeWindow: {
      if (!top || top->maximized || top->fullscreen) return;
      const uint32_t flags = data[0];
      int32_t l, t, r, b;
      frame_margins(*top, &l, &t, &r, &b);
      const int32_t x = (flags & (1u << 8)) ? static_cast<int32_t>(data[1]) + l : top->x;
      const int32_t y = (flags & (1u << 9)) ? static_cast<int32_t>(data[2]) + t : top->y;
      const uint32_t width = (flags & (1u << 10)) ? data[3] : top->width;
      const uint32_t height = (flags & (1u << 11)) ? data[4] : top->height;
      move_resize(*top, x, y, width, height, top->border, false);
      send_configure_notify(*top);
      return;
    }
    case kGtkShowWindowMenu:      // (GTK's title bar, drawn by the program: device, x and y on the root) the desktop opens the frame's window menu there
      if (!top || !top->mapped) return;
      menu_window_ = top->id;
      menu_x_ = static_cast<int32_t>(data[1]) - top->x;
      menu_y_ = static_cast<int32_t>(data[2]) - top->y;
      if (pointer_grab_.active && pointer_grab_.implicit) pointer_grab_ = Grab();      // (the button that opened it is the desktop's now)
      return;
    case kNetRequestFrameExtents:
      if (w) {
        int32_t l = 0, t = 0, r = 0, b = 0;
        if (top) frame_margins(*top, &l, &t, &r, &b);
        const uint32_t extents[4] = {static_cast<uint32_t>(l), static_cast<uint32_t>(r), static_cast<uint32_t>(t), static_cast<uint32_t>(b)};
        set_property32(*w, kNetFrameExtents, atom::kCardinal, extents);
      }
      return;
    default: return;
  }
}

// ------------------------------------------------------------------ the desktop's view (desktop::IWindowsSession)

void Server::attach(int32_t frame_border, int32_t frame_title) {
  kernel::ScopedMutex lock(mutex_);
  attached_ = true;
  frame_border_ = frame_border;
  frame_title_ = frame_title;
  for (Window* w : root_->children.view()) {
    if (w->input_only) continue;
    note_changed(*w);
    if (w->mapped) update_wm_state(*w);
  }
}

void Server::set_display(uint32_t width, uint32_t height, ArrayView<const Monitor> monitors) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (width == 0 || height == 0 || width > 32767 || height > 32767) return;
    const bool resized = width != screen_w_ || height != screen_h_;
    screen_w_ = width;
    screen_h_ = height;
    monitor_count_ = 0;
    for (const Monitor& m : monitors) {
      if (monitor_count_ == 8) break;
      monitors_[monitor_count_++] = {{m.rect.x, m.rect.y, static_cast<int32_t>(m.rect.width), static_cast<int32_t>(m.rect.height)},
                                     {m.work.x, m.work.y, static_cast<int32_t>(m.work.width), static_cast<int32_t>(m.work.height)}};
    }
    if (monitor_count_ == 0) {
      monitors_[0] = {{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)}, {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)}};
      monitor_count_ = 1;
    }
    if (root_ && resized) {
      root_->width = width;
      root_->height = height;
      send_configure_notify(*root_);
    }
    update_root_lists();
  }
  wake();
}

void Server::take_changes(List<uint32_t>* changed, List<Repaint>* repainted) {
  kernel::ScopedMutex lock(mutex_);
  changed->clear();
  repainted->clear();
  if (changes_overflow_) {
    for (Window* w : root_->children.view()) {
      if (w->input_only) continue;
      changed->push_back(w->id);
      if (w->picture) repainted->push_back({w->id, {0, 0, w->picture->width, w->picture->height}});
    }
  }
  for (const Change& c : changes_.view()) {
    if (c.changed) changed->push_back(c.window);
    if (c.repaint)
      repainted->push_back({c.window, {c.dirty.x, c.dirty.y, static_cast<uint32_t>(c.dirty.w), static_cast<uint32_t>(c.dirty.h)}});
  }
  changes_.clear();
  changes_overflow_ = false;
}

uint32_t Server::take_window_menu(int32_t* x, int32_t* y) {
  kernel::ScopedMutex lock(mutex_);
  const uint32_t w = menu_window_;
  *x = menu_x_;
  *y = menu_y_;
  menu_window_ = 0;
  return w;
}

uint32_t Server::take_drag(uint32_t* edge) {
  kernel::ScopedMutex lock(mutex_);
  const uint32_t w = drag_window_;
  *edge = drag_edge_;
  drag_window_ = 0;
  return w;
}

void Server::windows(List<uint32_t>* out) {
  kernel::ScopedMutex lock(mutex_);
  out->clear();
  for (Window* w : root_->children.view())
    if (w->mapped && !w->input_only) out->push_back(w->id);
}

namespace {

// Latin-1 (STRING) to UTF-8.
void append_latin1(String* out, ConstByteView text) {
  for (uint8_t b : text) {
    if (b == 0) break;
    if (b < 0x80) {
      out->push_back(static_cast<char>(b));
    } else {
      out->push_back(static_cast<char>(0xC0 | (b >> 6)));
      out->push_back(static_cast<char>(0x80 | (b & 0x3F)));
    }
  }
}

}  // namespace

bool Server::window_info(uint32_t window, desktop::WindowsWindowInfo* out, String* title) {
  kernel::ScopedMutex lock(mutex_);
  *out = desktop::WindowsWindowInfo();
  title->clear();
  Window* w = find_window(window);
  if (!w || w->parent != root_ || w->input_only || w == wm_check_) return false;
  out->exists = true;
  out->visible = w->mapped;
  out->minimized = w->iconic;
  out->maximized = w->maximized && !w->fullscreen;
  Window* f = focus_ > 1 ? find_window(focus_) : nullptr;
  out->active = f && top_of(f) == w;
  out->caption = decorated(*w);
  uint32_t flags = 0, min_w = 0, min_h = 0, max_w = 0, max_h = 0;
  bool sizable = true;
  if (property32(*w, atom::kWmNormalHints, 0, &flags) && (flags & 16) && (flags & 32) && property32(*w, atom::kWmNormalHints, 5, &min_w) &&
      property32(*w, atom::kWmNormalHints, 6, &min_h) && property32(*w, atom::kWmNormalHints, 7, &max_w) &&
      property32(*w, atom::kWmNormalHints, 8, &max_h) && min_w == max_w && min_h == max_h && min_w != 0)
    sizable = false;
  out->sizable = sizable && !w->fullscreen;
  out->minimize_box = true;
  out->maximize_box = sizable;
  const uint32_t type = window_type(*w);
  bool skip_taskbar = false;
  if (Property* state = find_property(*w, kNetWmState); state && state->format == 32) {
    ByteReader in(state->data.view());
    while (in.remaining() >= 4) if (in.le32() == kNetWmStateSkipTaskbar) skip_taskbar = true;
  }
  out->tool = type == kNetWmWindowTypeUtility || type == kNetWmWindowTypeToolbar || skip_taskbar;
  out->topmost = w->above;
  out->popup = w->override_redirect || type == kNetWmWindowTypeMenu || type == kNetWmWindowTypePopupMenu || type == kNetWmWindowTypeDropdownMenu ||
               type == kNetWmWindowTypeTooltip || type == kNetWmWindowTypeCombo || type == kNetWmWindowTypeDnd || type == kNetWmWindowTypeNotification;
  uint32_t transient = 0;
  out->owned = property32(*w, atom::kWmTransientFor, 0, &transient) && transient != 0;
  out->icon = w->icon;
  int32_t l, t, r, b;
  frame_margins(*w, &l, &t, &r, &b);
  out->surface = {w->x, w->y, w->width, w->height};
  out->window = {w->x - l, w->y - t, w->width + static_cast<uint32_t>(l + r), w->height + static_cast<uint32_t>(t + b)};
  if (Property* p = find_property(*w, kNetWmName); p && p->format == 8 && p->data.size() > 0) {
    title->append(StringView(p->data.view()));
  } else if (Property* p2 = find_property(*w, atom::kWmName); p2 && p2->format == 8) {
    if (p2->type == kUtf8String) title->append(StringView(p2->data.view()));
    else append_latin1(title, p2->data.view());
  }
  return true;
}

bool Server::draw_window(uint32_t window, graphics::IRenderer2D& renderer, int32_t x, int32_t y, uint32_t* width, uint32_t* height) {
  kernel::ScopedMutex lock(mutex_);
  Window* w = find_window(window);
  if (!w || w->parent != root_ || !w->picture || w->picture->pixels.count() < uint64_t{w->picture->width} * w->picture->height) return false;
  Picture& p = *w->picture;
  graphics::Surface s;
  s.pixels = p.pixels.view();
  s.width = s.pitch = p.width;
  s.height = p.height;
  renderer.set_blend(graphics::BlendMode::Replace);
  renderer.draw_image(s, {0, 0, p.width, p.height}, x, y);
  renderer.set_blend(graphics::BlendMode::Alpha);
  *width = p.width;
  *height = p.height;
  return true;
}

void Server::move_window(uint32_t window, const desktop::WindowsRect& rect) {
  {
    kernel::ScopedMutex lock(mutex_);
    Window* w = find_window(window);
    if (!w || w->parent != root_ || w->maximized || w->fullscreen) return;
    int32_t l, t, r, b;
    frame_margins(*w, &l, &t, &r, &b);
    int32_t width = static_cast<int32_t>(rect.width) - l - r, height = static_cast<int32_t>(rect.height) - t - b;
    // the size hints' limits
    uint32_t flags = 0, v = 0;
    if (property32(*w, atom::kWmNormalHints, 0, &flags)) {
      if ((flags & 16) && property32(*w, atom::kWmNormalHints, 5, &v) && width < static_cast<int32_t>(v)) width = static_cast<int32_t>(v);
      if ((flags & 16) && property32(*w, atom::kWmNormalHints, 6, &v) && height < static_cast<int32_t>(v)) height = static_cast<int32_t>(v);
      if ((flags & 32) && property32(*w, atom::kWmNormalHints, 7, &v) && v > 0 && width > static_cast<int32_t>(v)) width = static_cast<int32_t>(v);
      if ((flags & 32) && property32(*w, atom::kWmNormalHints, 8, &v) && v > 0 && height > static_cast<int32_t>(v)) height = static_cast<int32_t>(v);
    }
    const uint32_t before_w = w->width, before_h = w->height;
    const int32_t before_x = w->x, before_y = w->y;
    move_resize(*w, rect.x + l, rect.y + t, static_cast<uint32_t>(max32(1, width)), static_cast<uint32_t>(max32(1, height)), w->border, true);
    if (w->x != before_x || w->y != before_y || w->width != before_w || w->height != before_h) send_configure_notify(*w);
    note_changed(*w);      // (the frame learns where the window is, also when the hints kept it from going where the frame wanted)
  }
  wake();
}

void Server::activate_window(uint32_t window) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (window == 0) {
      Window* f = focus_ > 1 ? find_window(focus_) : nullptr;
      if (f) set_focus(0, 0);
    } else if (Window* w = find_window(window); w && w->parent == root_ && w->mapped) {
      restack(*w, nullptr, 0);
      Window* f = focus_ > 1 ? find_window(focus_) : nullptr;
      if (!f || top_of(f) != w) set_focus(w->id, 1);
      if (has_protocol(*w, kWmTakeFocus)) send_client_message(*w, kWmProtocols, kWmTakeFocus, now());
    }
  }
  wake();
}

void Server::window_command(uint32_t window, Command command) {
  {
    kernel::ScopedMutex lock(mutex_);
    Window* w = find_window(window);
    if (!w || w->parent != root_) return;
    switch (command) {
      case Command::Close: close_request(*w); break;
      case Command::Minimize: set_iconic(*w, true); break;
      case Command::Maximize:
        set_iconic(*w, false);
        set_maximized(*w, true);
        break;
      case Command::Restore:
        if (w->iconic) set_iconic(*w, false);
        else set_maximized(*w, false);
        break;
    }
  }
  wake();
}

void Server::pointer(Pointer* out) {
  kernel::ScopedMutex lock(mutex_);
  *out = Pointer();
  uint32_t id = 0;
  if (pointer_grab_.active && pointer_grab_.cursor) {
    id = pointer_grab_.cursor;
  } else {
    for (Window* w = find_window(pointer_window_); w; w = w->parent)
      if (w->cursor) { id = w->cursor; break; }
  }
  Resource* r = id ? resources_.find(id) : nullptr;
  if (!r || r->kind != Kind::Cursor) return;
  if (r->cursor->argb.count() == 0) out->system_id = r->cursor->system_id;
  else out->cursor = id;
}

bool Server::cursor_picture(uint32_t handle, uint32_t* system_id, List<uint32_t>* argb, uint32_t* width, uint32_t* height, int32_t* hot_x, int32_t* hot_y) {
  kernel::ScopedMutex lock(mutex_);
  argb->clear();
  *width = *height = 0;
  *hot_x = *hot_y = 0;
  if (handle & 0x80000000u) {      // a window's icon (_NET_WM_ICON: width, height, then ARGB rows; the one nearest 32 pixels)
    for (Window* w : root_->children.view()) {
      if (w->icon != handle) continue;
      Property* p = find_property(*w, kNetWmIcon);
      if (!p || p->format != 32) return false;
      ConstByteView data = p->data.view();
      uint64_t best = ~uint64_t{0}, best_at = 0;
      uint32_t bw = 0, bh = 0;
      for (uint64_t at = 0; data.size() >= 8 && at <= data.size() - 8;) {
        ByteReader in(data.subview(at, 8));
        const uint32_t iw = in.le32(), ih = in.le32();
        if (iw == 0 || ih == 0 || iw > 1024 || ih > 1024) break;
        const uint64_t bytes = uint64_t{iw} * ih * 4;
        if (bytes > data.size() - at - 8) break;
        const uint64_t distance = iw > 32 ? iw - 32 : 32 - iw;
        if (distance < best) { best = distance; best_at = at + 8; bw = iw; bh = ih; }
        at += 8 + bytes;
      }
      if (bw == 0 || !argb->reserve(uint64_t{bw} * bh)) return false;
      ByteReader in(data.subview(best_at, uint64_t{bw} * bh * 4));
      for (uint64_t i = 0; i < uint64_t{bw} * bh; ++i) argb->push_back(in.le32());
      *width = bw;
      *height = bh;
      *system_id = 0;
      return true;
    }
    return false;
  }
  Resource* r = resources_.find(handle);
  if (!r || r->kind != Kind::Cursor) return false;
  const Cursor& c = *r->cursor;
  *system_id = c.system_id;
  if (c.argb.count() == 0) return true;
  if (!argb->reserve(c.argb.count())) return false;
  for (uint32_t px : c.argb.view()) argb->push_back(px);
  *width = c.width;
  *height = c.height;
  *hot_x = c.hot_x;
  *hot_y = c.hot_y;
  return true;
}

uint32_t Server::picture_pixel(uint32_t window, int32_t x, int32_t y) {
  kernel::ScopedMutex lock(mutex_);
  Window* w = find_window(window);
  if (!w || !w->picture || x < 0 || y < 0 || static_cast<uint32_t>(x) >= w->picture->width || static_cast<uint32_t>(y) >= w->picture->height) return 0;
  return w->picture->pixels[uint64_t{static_cast<uint32_t>(y)} * w->picture->width + static_cast<uint32_t>(x)];
}

}  // namespace x11
