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

// The compositor's other protocols, and the window manager's pings:
//
//   xdg_activation_v1                 a client hands another the right to take the focus: tokens are made here (get_activation_token, commit -> done)
//                                     and spent once (activate: the surface's window is activated, as the desktop would on a click).
//   wp_cursor_shape_v1                a named cursor (34 shapes) instead of a cursor surface: the desktop's own cursors (the OCR_* shapes it has).
//   zwp_primary_selection_v1          the middle-button selection: like wl_data_device's selection, between Wayland clients (the desktop has none).
//   zwp_relative_pointer_v1           the mouse's motion as it came (games, 3D views), with the pointer's position or without it.
//   zwp_pointer_constraints_v1        a lock (the pointer stays where it is; only relative motion) or a confinement (the pointer stays in the surface or a
//                                     region of it), while the surface has the pointer and its window is active. The desktop then holds the pointer for the
//                                     program (cursor_confined) as for a Windows program's ClipCursor; Ctrl+Alt gives it back. One-shot constraints end at
//                                     their first release, persistent ones come back.
//   wp_presentation                   when a commit's picture was shown (presented: the desktop drew it; its clock is CLOCK_MONOTONIC) or never was
//                                     (discarded: a newer commit came first).
//   zwp_idle_inhibit_manager_v1       inhibitors are counted (idle_inhibited): the desktop has no idle blanking yet to hold off.
//   zxdg_foreign_v2                   a toplevel exported by one client (a handle) is made the parent of another client's toplevel (a dialog over another
//                                     program's window: the desktop shows it as owned).
//   xdg_wm_base.ping                  sent when the user activates or closes a window; a client that does not answer within kPingPatience is shown as not
//                                     responding (its windows' titles say so, as on Windows) until it does.
#include "Wayland/WaylandInternal.hpp"

namespace wayland {

namespace {

constexpr uint64_t kPingPatience = 5000;      // milliseconds
constexpr uint32_t kMaxTokens = 64;

// wp_cursor_shape_device_v1.shape -> the desktop's system cursor (the Windows OCR_* numbers): 1 .. 34
uint32_t system_cursor(uint32_t shape) {
  constexpr uint32_t kArrow = 32512, kIBeam = 32513, kWait = 32514, kCross = 32515, kSizeNwse = 32642, kSizeNesw = 32643, kSizeWe = 32644, kSizeNs = 32645,
                     kSizeAll = 32646, kNo = 32648, kHand = 32649, kAppStarting = 32650, kHelp = 32651;
  switch (shape) {
    case 3: return kHelp;
    case 4: return kHand;
    case 5: return kAppStarting;
    case 6: return kWait;
    case 7: case 8: return kCross;
    case 9: case 10: return kIBeam;
    case 13: case 16: case 17: case 32: return kSizeAll;
    case 14: case 15: return kNo;
    case 18: case 25: case 26: case 30: return kSizeWe;
    case 19: case 22: case 27: case 31: return kSizeNs;
    case 20: case 24: case 28: return kSizeNesw;
    case 21: case 23: case 29: return kSizeNwse;
    default: return kArrow;      // default, context menu, alias, copy, zoom: the arrow
  }
}

}  // namespace

// ------------------------------------------------------------------ activation (also the desktop's activate_window)

void Server::activate_locked(Surface* s) {
  if (s && s->role != Role::Toplevel) s = nullptr;      // (popups are not activated: their toplevel stays active)
  Surface* old = window_surface(active_window_);
  if (s) raise(*s);
  if (old != s) {
    if (old && old->role == Role::Toplevel) {
      old->activated = false;
      if (old->initial_configure) configure_toplevel(*old);
      note_changed(*old);
    }
    dismiss_popups(s);
    active_window_ = s ? s->window : 0;
    if (s) {
      s->activated = true;
      if (s->initial_configure) configure_toplevel(*s);
      note_changed(*s);
    }
  }
  // the keyboard: the topmost popup that grabbed it, else the window
  Surface* focus = s;
  for (Surface* w : stack_.view()) if (s && w->role == Role::Popup && w->grab && root_toplevel(w) == s) focus = w;
  set_keyboard_focus(focus);
  update_constraints();
  if (s)
    if (Client* c = client_of(s->client)) ping(*c);
}

// ------------------------------------------------------------------ the primary selection

void Server::send_primary_selection(Client& c) {
  psel_sent_client_ = c.number;
  DataSourceInfo* src = nullptr;
  if (Client* sc = client_of(psel_client_))
    if (Resource* r = sc->objects.find(psel_source_); r && r->iface == Iface::PrimarySource) src = r->source.get();
  ObjectCursor cursor;
  List<uint32_t> devices(allocator_);
  for (bool more = c.objects.first(&cursor); more; more = c.objects.next(&cursor)) {
    const KeyValue<uint32_t, Resource>* kv = c.objects.element(cursor);
    if (kv->value.iface == Iface::PrimaryDevice) devices.push_back(kv->key);
  }
  for (uint32_t d : devices.view()) {
    uint32_t offer = 0;
    if (src) {
      SharedPointer<OfferInfo> info = SharedPointer<OfferInfo>::make(allocator_);
      Resource* r = info ? new_server_object(c, Iface::PrimaryOffer, 1, &offer) : nullptr;
      if (!r) return;
      info->source_client = psel_client_;
      info->source_id = psel_source_;
      r->offer = info;
      Event made(d, 0);      // data_offer(new id)
      made.u(offer);
      send(c, made);
      for (const String& t : src->mimes.view()) {
        Event e(offer, 0);
        e.string(t.view());
        send(c, e);
      }
    }
    Event sel(d, 1);      // selection(offer or null)
    sel.u(offer);
    send(c, sel);
  }
}

// ------------------------------------------------------------------ pointer constraints and relative motion

Server::Constraint* Server::active_constraint() {
  for (Constraint& k : constraints_.view()) if (k.active) return &k;
  return nullptr;
}

void Server::update_constraints() {
  for (Constraint& k : constraints_.view()) {
    if (k.defunct) continue;
    bool should = false;
    if (k.surface && pointer_focus_ == k.surface && buttons_ == 0 && !drag_.active) {
      Surface* top = root_toplevel(k.surface);
      should = top && top->window == active_window_ && active_window_ != 0 && (!k.region || k.region->contains(pointer_lx_, pointer_ly_));
    }
    if (should == k.active) continue;
    Client* c = client_of(k.client);
    k.active = should;
    if (c) send(*c, Event(k.id, should ? 0 : 1));      // locked / confined, or unlocked / unconfined
    if (!should) {
      if (k.lock && k.hint_set && k.surface) {      // (a lock's hint: where the pointer is put back)
        int32_t ox = 0, oy = 0;
        surface_origin(*k.surface, &ox, &oy);
        pointer_x_ = max32(0, min32(ox + k.hint_x, int32_t(screen_w_) - 1));
        pointer_y_ = max32(0, min32(oy + k.hint_y, int32_t(screen_h_) - 1));
      }
      if (!k.persistent) k.defunct = true;
    }
  }
}

void Server::constraint_gone(Surface& s) {
  for (Constraint& k : constraints_.view())
    if (k.surface == &s) {
      if (k.active)
        if (Client* c = client_of(k.client)) send(*c, Event(k.id, 1));
      k.active = false;
      k.defunct = true;
      k.surface = nullptr;
    }
}

void Server::relative_motion(int32_t dx, int32_t dy) {
  if ((dx == 0 && dy == 0) || !pointer_focus_) return;
  Client* c = client_of(pointer_focus_->client);
  if (!c) return;
  const uint64_t us = clock_ ? clock_() * 1000 : ++fake_clock_ * 1000;
  ObjectCursor cursor;
  List<uint32_t> targets(allocator_);
  for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
    const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
    if (kv->value.iface == Iface::RelativePointer) targets.push_back(kv->key);
  }
  for (uint32_t t : targets.view()) {
    Event e(t, 0);      // relative_motion(utime_hi, utime_lo, dx, dy, dx_unaccel, dy_unaccel)
    e.u(uint32_t(us >> 32)).u(uint32_t(us)).fixed(dx).fixed(dy).fixed(dx).fixed(dy);
    send(*c, e);
  }
  if (targets.count()) pointer_frame(*c);
}

bool Server::cursor_confined() {
  kernel::ScopedMutex lock(mutex_);
  return active_constraint() != nullptr;
}

// ------------------------------------------------------------------ presentation time

void Server::presented(Surface& s, bool shown) {
  Client* c = client_of(s.client);
  if (!c) {
    s.feedback.clear();
    return;
  }
  const uint64_t ms = clock_ ? clock_() : fake_clock_;
  if (shown) ++present_seq_;
  for (uint32_t f : s.feedback.view()) {
    if (shown) {
      Event e(f, 1);      // presented(tv_sec_hi, tv_sec_lo, tv_nsec, refresh, seq_hi, seq_lo, flags)
      const uint64_t sec = ms / 1000;
      e.u(uint32_t(sec >> 32)).u(uint32_t(sec)).u(uint32_t(ms % 1000) * 1000000u).u(16666667u).u(uint32_t(present_seq_ >> 32)).u(uint32_t(present_seq_)).u(0);
      send(*c, e);
    } else {
      send(*c, Event(f, 2));      // discarded
    }
    c->objects.remove(f);
    delete_id(*c, f);
  }
  s.feedback.clear();
}

// ------------------------------------------------------------------ pings

void Server::ping(Client& c) {
  if (c.ping_serial != 0) return;      // (one at a time)
  ObjectCursor cursor;
  List<uint32_t> bases(allocator_);
  for (bool more = c.objects.first(&cursor); more; more = c.objects.next(&cursor)) {
    const KeyValue<uint32_t, Resource>* kv = c.objects.element(cursor);
    if (kv->value.iface == Iface::WmBase) bases.push_back(kv->key);
  }
  if (bases.count() == 0) return;
  c.ping_serial = next_serial();
  c.ping_sent = clock_ ? clock_() : 0;
  for (uint32_t b : bases.view()) {
    Event e(b, 0);
    e.u(c.ping_serial);
    send(c, e);
  }
}

void Server::check_pings() {
  if (!clock_) return;      // (without a clock nothing is late)
  const uint64_t now_ms = clock_();
  for (uint32_t i = 1; i < kMaxClients; ++i) {
    Client* c = clients_[i].get();
    if (!c || c->ping_serial == 0 || c->not_responding || now_ms - c->ping_sent < kPingPatience) continue;
    c->not_responding = true;
    for (Surface& s : surfaces_) if (s.client == c->number && s.role == Role::Toplevel && s.window) note_changed(s);
  }
}

bool Server::idle_inhibited() {
  kernel::ScopedMutex lock(mutex_);
  return idle_inhibitors_ != 0;
}

void Server::forget_exports(Surface& s) {
  for (uint64_t i = 0; i < exports_.count();) {
    if (exports_[i]->surface != &s) {
      ++i;
      continue;
    }
    exports_[i]->surface = nullptr;      // (imports of it hear "destroyed" and their children lose the parent)
    for (uint32_t k = 1; k < kMaxClients; ++k) {
      Client* c = clients_[k].get();
      if (!c) continue;
      ObjectCursor cursor;
      List<uint32_t> gone(allocator_);
      for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
        const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
        if (kv->value.iface == Iface::Imported && kv->value.text && kv->value.text->view() == exports_[i]->handle.view()) gone.push_back(kv->key);
      }
      for (uint32_t g : gone.view()) send(*c, Event(g, 0));      // destroyed
    }
    for (Surface& other : surfaces_)
      if (s.window != 0 && other.parent_window == s.window) other.parent_window = 0, note_changed(other);
    exports_.remove_at(i);
  }
}

// ------------------------------------------------------------------ requests

void Server::extras_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  auto surface_of = [&](uint32_t object) -> Surface* {
    Resource* sr = object_of(c, object, Iface::Surface, id);
    return sr ? sr->surface : nullptr;
  };
  switch (r.iface) {
    // --- xdg_activation_v1
    case Iface::Activation:
      if (opcode == 0) {
        destroy_object(c, id);
      } else if (opcode == 1) {      // get_activation_token(new id)
        const uint32_t object = in.u();
        if (in.ok()) new_object(c, object, Iface::ActivationToken, 1, id);
      } else if (opcode == 2) {      // activate(token, surface)
        const StringView token = in.string();
        const uint32_t surface = in.object();
        if (!in.ok()) return;
        Surface* s = surface_of(surface);
        if (!s) return;
        for (uint64_t i = 0; i < tokens_.count(); ++i)
          if (tokens_[i].view() == token) {      // (spent: a token works once)
            tokens_.remove_at(i);
            Surface* top = root_toplevel(s);
            if (top && top->mapped) activate_locked(top);
            break;
          }
      } else {
        post_error(c, id, kErrorInvalidMethod, "xdg_activation_v1 has no such request");
      }
      return;
    case Iface::ActivationToken:
      switch (opcode) {
        case 0: in.u(); in.object(); return;      // set_serial(serial, seat)
        case 1: in.string(); return;              // set_app_id
        case 2: in.object(); return;              // set_surface
        case 3: {                                 // commit -> done(token)
          if (r.text) {
            post_error(c, id, 0, "the token was committed already");      // (already_used)
            return;
          }
          r.text = SharedPointer<String>::make(allocator_, allocator_);
          if (!r.text) return;
          r.text->append("tcos-activation-");
          r.text->append_uint(next_token_++);
          r.text->append("-");
          r.text->append_uint(next_serial());
          if (tokens_.count() >= kMaxTokens) tokens_.remove_at(0);
          String kept(allocator_);
          if (kept.assign(r.text->view())) tokens_.push_back(util::move(kept));
          Event e(id, 0);
          e.string(r.text->view());
          send(c, e);
          return;
        }
        case 4: destroy_object(c, id); return;
        default: post_error(c, id, kErrorInvalidMethod, "xdg_activation_token_v1 has no such request"); return;
      }
    // --- wp_cursor_shape_v1
    case Iface::CursorShapeManager:
      if (opcode == 0) {
        destroy_object(c, id);
      } else if (opcode == 1 || opcode == 2) {      // get_pointer(new id, pointer), get_tablet_tool_v2(new id, tool)
        const uint32_t object = in.u();
        in.object();
        if (in.ok()) new_object(c, object, Iface::CursorShapeDevice, 1, id);
      } else {
        post_error(c, id, kErrorInvalidMethod, "wp_cursor_shape_manager_v1 has no such request");
      }
      return;
    case Iface::CursorShapeDevice:
      if (opcode == 0) {
        destroy_object(c, id);
      } else if (opcode == 1) {      // set_shape(serial, shape)
        in.u();
        const uint32_t shape = in.u();
        if (!in.ok()) return;
        if (shape < 1 || shape > 34) {
          post_error(c, id, 1, "invalid shape");
          return;
        }
        if (!pointer_focus_ || pointer_focus_->client != c.number) return;      // (only the client the pointer is over sets it)
        cursor_set_ = true;
        cursor_surface_ = nullptr;
        cursor_shape_ = system_cursor(shape);
        ++cursor_handle_;
      } else {
        post_error(c, id, kErrorInvalidMethod, "wp_cursor_shape_device_v1 has no such request");
      }
      return;
    // --- zwp_primary_selection_v1
    case Iface::PrimaryManager:
      if (opcode == 0) {      // create_source(new id)
        const uint32_t object = in.u();
        if (!in.ok()) return;
        SharedPointer<DataSourceInfo> info = SharedPointer<DataSourceInfo>::make(allocator_, allocator_);
        if (Resource* nr = info ? new_object(c, object, Iface::PrimarySource, 1, id) : nullptr) nr->source = util::move(info);
      } else if (opcode == 1) {      // get_device(new id, seat)
        const uint32_t object = in.u();
        in.object();
        if (!in.ok() || !new_object(c, object, Iface::PrimaryDevice, 1, id)) return;
        if (keyboard_focus_ && keyboard_focus_->client == c.number) send_primary_selection(c);
      } else if (opcode == 2) {
        destroy_object(c, id);
      } else {
        post_error(c, id, kErrorInvalidMethod, "zwp_primary_selection_device_manager_v1 has no such request");
      }
      return;
    case Iface::PrimarySource:
      if (opcode == 0) {      // offer(mime)
        const StringView type = in.string();
        if (!in.ok() || !r.source || r.source->mimes.count() >= 64 || type.size() > 255) return;
        String t(allocator_);
        if (t.assign(type)) r.source->mimes.push_back(util::move(t));
      } else if (opcode == 1) {      // destroy
        if (psel_client_ == c.number && psel_source_ == id) {
          psel_client_ = 0, psel_source_ = 0;
          if (keyboard_focus_)
            if (Client* fc = client_of(keyboard_focus_->client)) send_primary_selection(*fc);
        }
        destroy_object(c, id);
      } else {
        post_error(c, id, kErrorInvalidMethod, "zwp_primary_selection_source_v1 has no such request");
      }
      return;
    case Iface::PrimaryDevice:
      if (opcode == 0) {      // set_selection(source, serial)
        const uint32_t source = in.object();
        in.u();
        if (!in.ok()) return;
        if (source && !object_of(c, source, Iface::PrimarySource, id)) return;
        if (psel_client_ && psel_source_ && !(psel_client_ == c.number && psel_source_ == source))
          if (Client* oc = client_of(psel_client_))
            if (Resource* old = oc->objects.find(psel_source_); old && old->iface == Iface::PrimarySource) send(*oc, Event(psel_source_, 1));      // cancelled
        psel_client_ = source ? c.number : 0;
        psel_source_ = source;
        if (keyboard_focus_)
          if (Client* fc = client_of(keyboard_focus_->client)) send_primary_selection(*fc);
      } else if (opcode == 1) {
        destroy_object(c, id);
      } else {
        post_error(c, id, kErrorInvalidMethod, "zwp_primary_selection_device_v1 has no such request");
      }
      return;
    case Iface::PrimaryOffer:
      if (opcode == 0) {      // receive(mime, fd): the source's client gets the descriptor as it is
        const StringView type = in.string();
        Fd fd = in.fd();
        if (!in.ok() || !r.offer) return;
        Client* sc = client_of(r.offer->source_client);
        Resource* sr = sc ? sc->objects.find(r.offer->source_id) : nullptr;
        if (!sr || sr->iface != Iface::PrimarySource) return;
        Event e(r.offer->source_id, 0);      // send(mime, fd)
        e.string(type).fd(fd);
        send(*sc, e);
      } else if (opcode == 1) {
        destroy_object(c, id);
      } else {
        post_error(c, id, kErrorInvalidMethod, "zwp_primary_selection_offer_v1 has no such request");
      }
      return;
    // --- zwp_relative_pointer_v1, zwp_pointer_constraints_v1
    case Iface::RelativePointerManager:
      if (opcode == 0) {
        destroy_object(c, id);
      } else if (opcode == 1) {      // get_relative_pointer(new id, pointer)
        const uint32_t object = in.u();
        in.object();
        if (in.ok()) new_object(c, object, Iface::RelativePointer, 1, id);
      } else {
        post_error(c, id, kErrorInvalidMethod, "zwp_relative_pointer_manager_v1 has no such request");
      }
      return;
    case Iface::RelativePointer:
      if (opcode == 0) destroy_object(c, id);
      else post_error(c, id, kErrorInvalidMethod, "zwp_relative_pointer_v1 has no such request");
      return;
    case Iface::PointerConstraints:
      if (opcode == 0) {
        destroy_object(c, id);
      } else if (opcode == 1 || opcode == 2) {      // lock_pointer / confine_pointer(new id, surface, pointer, region, lifetime)
        const uint32_t object = in.u(), surface = in.object();
        in.object();
        const uint32_t region = in.object(), lifetime = in.u();
        if (!in.ok()) return;
        Surface* s = surface_of(surface);
        if (!s) return;
        if (lifetime != 1 && lifetime != 2) {
          post_error(c, id, 2, "invalid lifetime");      // (bad_lifetime)
          return;
        }
        for (const Constraint& k : constraints_.view())
          if (k.surface == s && !k.defunct) {
            post_error(c, id, 1, "the surface is constrained already");      // (already_constrained)
            return;
          }
        Constraint k;
        k.client = c.number;
        k.id = object;
        k.surface = s;
        k.lock = opcode == 1;
        k.persistent = lifetime == 2;
        copy_region(c, region, &k.region, id);
        if (c.closing || !new_object(c, object, k.lock ? Iface::LockedPointer : Iface::ConfinedPointer, 1, id)) return;
        if (!constraints_.push_back(util::move(k))) {
          post_error(c, kDisplayId, kErrorNoMemory, "out of memory");
          return;
        }
        update_constraints();
      } else {
        post_error(c, id, kErrorInvalidMethod, "zwp_pointer_constraints_v1 has no such request");
      }
      return;
    case Iface::LockedPointer:
    case Iface::ConfinedPointer: {
      Constraint* k = nullptr;
      uint64_t at = 0;
      for (uint64_t i = 0; i < constraints_.count(); ++i)
        if (constraints_[i].client == c.number && constraints_[i].id == id) k = &constraints_[i], at = i;
      const bool lock = r.iface == Iface::LockedPointer;
      if (opcode == 0) {      // destroy: the pointer is the desktop's again
        if (k) constraints_.remove_at(at);
        destroy_object(c, id);
        return;
      }
      if (lock && opcode == 1) {      // set_cursor_position_hint(x, y)
        const int32_t x = in.fixed_pixels(), y = in.fixed_pixels();
        if (in.ok() && k) k->hint_set = true, k->hint_x = x, k->hint_y = y;
        return;
      }
      if ((lock && opcode == 2) || (!lock && opcode == 1)) {      // set_region(region)
        const uint32_t region = in.object();
        if (!in.ok() || !k) return;
        SharedPointer<RegionRects> rects;
        copy_region(c, region, &rects, id);
        k->region = util::move(rects);
        update_constraints();
        return;
      }
      post_error(c, id, kErrorInvalidMethod, lock ? "zwp_locked_pointer_v1 has no such request" : "zwp_confined_pointer_v1 has no such request");
      return;
    }
    // --- wp_presentation
    case Iface::Presentation:
      if (opcode == 0) {
        destroy_object(c, id);
      } else if (opcode == 1) {      // feedback(surface, new id)
        const uint32_t surface = in.object(), object = in.u();
        if (!in.ok()) return;
        Surface* s = surface_of(surface);
        if (!s || !new_object(c, object, Iface::PresentationFeedback, 1, id)) return;
        if (!s->pending_feedback.push_back(object)) post_error(c, kDisplayId, kErrorNoMemory, "out of memory");
      } else {
        post_error(c, id, kErrorInvalidMethod, "wp_presentation has no such request");
      }
      return;
    case Iface::PresentationFeedback:
      post_error(c, id, kErrorInvalidMethod, "wp_presentation_feedback has no requests");
      return;
    // --- zwp_idle_inhibit_manager_v1
    case Iface::IdleInhibitManager:
      if (opcode == 0) {
        destroy_object(c, id);
      } else if (opcode == 1) {      // create_inhibitor(new id, surface)
        const uint32_t object = in.u(), surface = in.object();
        if (!in.ok() || !surface_of(surface)) return;
        if (new_object(c, object, Iface::IdleInhibitor, 1, id)) ++idle_inhibitors_;
      } else {
        post_error(c, id, kErrorInvalidMethod, "zwp_idle_inhibit_manager_v1 has no such request");
      }
      return;
    case Iface::IdleInhibitor:
      if (opcode == 0) {
        if (idle_inhibitors_) --idle_inhibitors_;
        destroy_object(c, id);
      } else {
        post_error(c, id, kErrorInvalidMethod, "zwp_idle_inhibitor_v1 has no such request");
      }
      return;
    // --- zxdg_foreign_v2
    case Iface::Exporter:
      if (opcode == 0) {
        destroy_object(c, id);
      } else if (opcode == 1) {      // export_toplevel(new id, surface) -> handle
        const uint32_t object = in.u(), surface = in.object();
        if (!in.ok()) return;
        Surface* s = surface_of(surface);
        if (!s) return;
        if (s->role != Role::Toplevel) {
          post_error(c, id, 0, "not a toplevel");      // (invalid_surface)
          return;
        }
        UniquePointer<Exported> e = UniquePointer<Exported>::make(allocator_, allocator_);
        Resource* nr = e ? new_object(c, object, Iface::Exported, 1, id) : nullptr;
        if (!nr) return;
        e->client = c.number, e->id = object, e->surface = s;
        e->handle.append("tcos-toplevel-");
        e->handle.append_uint(next_export_++);
        e->handle.append("-");
        e->handle.append_uint(uint64_t(next_serial()) * 2654435761u % 1000000007u);
        nr->text = SharedPointer<String>::make(allocator_, allocator_);
        if (nr->text) nr->text->assign(e->handle.view());
        Event h(object, 0);
        h.string(e->handle.view());
        send(c, h);
        exports_.push_back(util::move(e));
      } else {
        post_error(c, id, kErrorInvalidMethod, "zxdg_exporter_v2 has no such request");
      }
      return;
    case Iface::Exported:
      if (opcode == 0) {
        for (uint64_t i = 0; i < exports_.count(); ++i)
          if (exports_[i]->client == c.number && exports_[i]->id == id) {
            if (Surface* s = exports_[i]->surface) {
              exports_[i]->surface = nullptr;
              forget_exports(*s);      // (the imports hear it is gone)
            }
            for (uint64_t k = 0; k < exports_.count(); ++k)
              if (exports_[k]->client == c.number && exports_[k]->id == id) { exports_.remove_at(k); break; }
            break;
          }
        destroy_object(c, id);
      } else {
        post_error(c, id, kErrorInvalidMethod, "zxdg_exported_v2 has no such request");
      }
      return;
    case Iface::Importer:
      if (opcode == 0) {
        destroy_object(c, id);
      } else if (opcode == 1) {      // import_toplevel(new id, handle)
        const uint32_t object = in.u();
        const StringView handle = in.string();
        if (!in.ok()) return;
        Resource* nr = new_object(c, object, Iface::Imported, 1, id);
        if (!nr) return;
        nr->text = SharedPointer<String>::make(allocator_, allocator_);
        if (nr->text) nr->text->assign(handle);
        bool known = false;
        for (const UniquePointer<Exported>& e : exports_.view()) known |= e->surface && e->handle.view() == handle;
        if (!known) send(c, Event(object, 0));      // destroyed: no such toplevel
      } else {
        post_error(c, id, kErrorInvalidMethod, "zxdg_importer_v2 has no such request");
      }
      return;
    case Iface::Imported:
      if (opcode == 0) {
        destroy_object(c, id);
      } else if (opcode == 1) {      // set_parent_of(surface)
        const uint32_t surface = in.object();
        if (!in.ok()) return;
        Surface* child = surface_of(surface);
        if (!child) return;
        if (child->role != Role::Toplevel) {
          post_error(c, id, 0, "not a toplevel");      // (invalid_surface)
          return;
        }
        for (const UniquePointer<Exported>& e : exports_.view())
          if (e->surface && r.text && e->handle.view() == r.text->view()) {
            child->parent_window = e->surface->window;
            note_changed(*child);
          }
      } else {
        post_error(c, id, kErrorInvalidMethod, "zxdg_imported_v2 has no such request");
      }
      return;
    default: return;
  }
}

}  // namespace wayland
