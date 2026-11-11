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

// The data device (wl_data_device_manager 3): the selection (copy and paste) between clients and with the desktop's clipboard, and drag and drop between
// Wayland clients.
//
// The selection is a client's wl_data_source or the desktop's clipboard text. The client with the keyboard focus is offered it (wl_data_offer and its types,
// then wl_data_device.selection) when the focus comes to it and whenever it changes. A client that pastes asks the offer for one type with a descriptor
// (receive); the compositor passes that descriptor on to the source's client (wl_data_source.send), which writes the data into it, or writes the clipboard's
// text into it itself. The clipboard follows the selection both ways: a client's selection that has text is read at once (through a pipe the transport makes)
// and put on the desktop's clipboard, and text the desktop's programs copy becomes the selection (types text/plain;charset=utf-8, text/plain, UTF8_STRING,
// STRING, TEXT). The clipboard is read and written only outside the compositor's lock (its observer keeps the Windows clipboard the same and takes the system's
// locks): tick() looks at it, pipe_read_done() writes it.
//
// A drag (start_drag, while a button is held) takes the pointer: the surface under it is entered (wl_data_device.enter with an offer of the source's types;
// a drag without a source only within its client), followed (motion) and left; the destination accepts a type and says the actions it takes (version 3:
// copy, move, ask; the source's and the destination's preference choose one, told to both with action). Releasing the button over a destination that accepted
// drops (drop, dnd_drop_performed); the destination receives the data as for a paste and says finish (dnd_finished). Anything else cancels the source.
// The drag's icon surface is not drawn. Drags from or to the desktop's own windows (files of the File Explorer) are not bridged.
#include "Wayland/WaylandInternal.hpp"

namespace wayland {

namespace {

constexpr const char* kDesktopTypes[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"};
constexpr uint32_t kActionCopy = 1, kActionMove = 2, kActionAsk = 4, kActionAll = 7;
constexpr uint32_t kMaxTypes = 64;
constexpr uint64_t kMaxTypeLength = 255;

bool text_type(StringView t) {
  for (const char* d : kDesktopTypes) if (t == StringView(d)) return true;
  return t == StringView("text/plain;charset=UTF-8");
}

}  // namespace

void Server::request(TransportRequest r) {
  if (requests_.push_back(util::move(r))) wake_wanted_ = true;
}

void Server::take_requests(List<TransportRequest>* out) {
  kernel::ScopedMutex lock(mutex_);
  for (TransportRequest& r : requests_.view()) out->push_back(util::move(r));
  requests_.clear();
}

void Server::set_clipboard(kernel::Clipboard* clipboard) {
  uint64_t seq = 0;
  String text(allocator_);
  bool has = false;
  if (clipboard) seq = clipboard->sequence(), has = clipboard->get_text(&text);      // (outside the lock: see above)
  kernel::ScopedMutex lock(mutex_);
  clipboard_ = clipboard;
  clipboard_seen_ = seq;
  sel_desktop_ = has && sel_client_ == 0;
  desktop_text_ = util::move(text);
}

void Server::tick() {
  kernel::Clipboard* clipboard = nullptr;
  uint64_t seen = 0;
  {
    kernel::ScopedMutex lock(mutex_);
    poll_waiting();
    clipboard = clipboard_;
    seen = clipboard_seen_;
  }
  if (clipboard) {
    const uint64_t seq = clipboard->sequence();
    if (seq != seen) {
      String copied(allocator_);
      const bool text = clipboard->get_text(&copied);
      kernel::ScopedMutex lock(mutex_);
      if (clipboard_ == clipboard && clipboard_seen_ == seen) {
        clipboard_seen_ = seq;
        desktop_text_ = util::move(copied);
        // text the desktop copied is the selection now (a client's earlier one is cancelled); anything else leaves no desktop selection
        if (text) {
          if (DataSourceInfo* old = source_of(sel_client_, sel_source_); old && sel_client_) {
            (void)old;
            if (Client* oc = client_of(sel_client_)) send(*oc, Event(sel_source_, 2));      // cancelled
          }
          sel_client_ = 0;
          sel_source_ = 0;
          sel_desktop_ = true;
          fetch_job_ = 0;
        } else if (sel_desktop_) {
          sel_desktop_ = false;
        }
        if (keyboard_focus_)
          if (Client* fc = client_of(keyboard_focus_->client)) send_selection(*fc);
      }
    }
  }
  wake();
}

Server::DataSourceInfo* Server::source_of(uint8_t client, uint32_t source) {
  if (client == 0 || source == 0) return nullptr;
  Client* c = client_of(client);
  Resource* r = c ? c->objects.find(source) : nullptr;
  return r && r->iface == Iface::DataSource && r->source ? r->source.get() : nullptr;
}

void Server::devices_of(Client& c, List<uint32_t>* out) {
  ObjectCursor cursor;
  for (bool more = c.objects.first(&cursor); more; more = c.objects.next(&cursor)) {
    const KeyValue<uint32_t, Resource>* kv = c.objects.element(cursor);
    if (kv->value.iface == Iface::DataDevice) out->push_back(kv->key);
  }
}

uint32_t Server::make_offer(Client& c, uint32_t device, uint8_t source_client, uint32_t source_id, bool dnd) {
  Resource* dev = c.objects.find(device);
  const uint32_t version = dev ? dev->version : 1;
  SharedPointer<OfferInfo> info = SharedPointer<OfferInfo>::make(allocator_);
  if (!info) return 0;
  info->source_client = source_client;
  info->source_id = source_id;
  info->dnd = dnd;
  uint32_t id = 0;
  Resource* r = new_server_object(c, Iface::DataOffer, version, &id);
  if (!r) return 0;
  r->offer = info;
  Event made(device, 0);      // data_offer(new id)
  made.u(id);
  send(c, made);
  if (source_id == 0) {
    for (const char* t : kDesktopTypes) {
      Event e(id, 0);
      e.string(StringView(t));
      send(c, e);
    }
  } else if (DataSourceInfo* src = source_of(source_client, source_id)) {
    for (const String& t : src->mimes.view()) {
      Event e(id, 0);
      e.string(t.view());
      send(c, e);
    }
    if (dnd && version >= 3) {
      Event a(id, 1);      // source_actions
      a.u(src->actions ? src->actions : kActionCopy);
      send(c, a);
    }
  }
  return id;
}

void Server::send_selection(Client& c) {
  List<uint32_t> devices(allocator_);
  devices_of(c, &devices);
  sel_sent_client_ = c.number;
  const bool present = sel_desktop_ || source_of(sel_client_, sel_source_) != nullptr;
  for (uint32_t d : devices.view()) {
    const uint32_t offer = present ? make_offer(c, d, sel_desktop_ ? 0 : sel_client_, sel_desktop_ ? 0 : sel_source_, false) : 0;
    Event e(d, 5);      // selection(offer or null)
    e.u(offer);
    send(c, e);
  }
}

void Server::set_selection(Client* c, uint32_t source) {
  if (sel_client_ && sel_source_ && !(c && c->number == sel_client_ && source == sel_source_))
    if (source_of(sel_client_, sel_source_))
      if (Client* oc = client_of(sel_client_)) send(*oc, Event(sel_source_, 2));      // cancelled: another selection took its place
  sel_client_ = source && c ? c->number : 0;
  sel_source_ = source;
  sel_desktop_ = false;
  fetch_job_ = 0;
  if (source) fetch_selection_text();
  if (keyboard_focus_)
    if (Client* fc = client_of(keyboard_focus_->client)) send_selection(*fc);
}

// A client's selection with text goes to the desktop's clipboard: the transport makes a pipe; pipe_ready hands the source its end (send), pipe_read_done
// what came.
void Server::fetch_selection_text() {
  DataSourceInfo* src = source_of(sel_client_, sel_source_);
  if (!clipboard_ || !src) return;
  const String* chosen = nullptr;
  for (const String& t : src->mimes.view())
    if (text_type(t.view()) && (!chosen || t.view() == StringView("text/plain;charset=utf-8"))) chosen = &t;
  if (!chosen || !fetch_mime_.assign(chosen->view())) return;
  TransportRequest r;
  r.kind = TransportRequest::Kind::MakePipe;
  r.job = next_job_++;
  fetch_job_ = r.job;
  request(util::move(r));
}

void Server::pipe_ready(uint32_t job, Fd write_end) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (job == 0 || job != fetch_job_ || !source_of(sel_client_, sel_source_)) return;      // (the selection changed meanwhile: the pipe is closed unused)
    Client* c = client_of(sel_client_);
    Event e(sel_source_, 1);      // send(mime, fd)
    e.string(fetch_mime_.view()).fd(write_end);
    send(*c, e);
  }
  wake();
}

void Server::pipe_read_done(uint32_t job, ConstByteView data) {
  kernel::Clipboard* clipboard = nullptr;
  {
    kernel::ScopedMutex lock(mutex_);
    if (job == 0 || job != fetch_job_) return;
    fetch_job_ = 0;
    clipboard = clipboard_;
  }
  if (!clipboard || data.size() == 0) return;
  if (!clipboard->set_text(StringView(reinterpret_cast<const char*>(data.data()), data.size()))) return;      // (outside the lock: see above)
  const uint64_t seq = clipboard->sequence();
  kernel::ScopedMutex lock(mutex_);
  if (clipboard_ == clipboard) clipboard_seen_ = seq;      // (the selection stays the client's: the change was ours)
}

void Server::source_gone(Client& c, uint32_t source) {
  if (sel_client_ == c.number && sel_source_ == source) {
    sel_client_ = 0;
    sel_source_ = 0;
    fetch_job_ = 0;
    if (keyboard_focus_)
      if (Client* fc = client_of(keyboard_focus_->client)) send_selection(*fc);
  }
  if ((drag_.active || drag_.dropped) && drag_.client == c.number && drag_.source == source) {
    drag_.source = 0;      // (nothing to tell it any more)
    end_drag(false);
  }
}

// ------------------------------------------------------------------ requests

void Server::data_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  switch (r.iface) {
    case Iface::DataDeviceManager: {
      const uint32_t object = in.u();
      if (opcode == 1) in.object();      // (the seat)
      if (!in.ok()) return;
      if (opcode == 0) {      // create_data_source
        SharedPointer<DataSourceInfo> info = SharedPointer<DataSourceInfo>::make(allocator_, allocator_);
        if (!info) {
          post_error(c, kDisplayId, kErrorNoMemory, "out of memory");
          return;
        }
        if (Resource* nr = new_object(c, object, Iface::DataSource, r.version, id)) nr->source = util::move(info);
      } else if (opcode == 1) {      // get_data_device
        if (!new_object(c, object, Iface::DataDevice, r.version, id)) return;
        if (keyboard_focus_ && keyboard_focus_->client == c.number) send_selection(c);      // (it has the focus already)
      } else {
        post_error(c, id, kErrorInvalidMethod, "wl_data_device_manager has no such request");
      }
      return;
    }
    case Iface::DataSource: {
      if (!r.source) return;
      switch (opcode) {
        case 0: {      // offer(mime)
          const StringView type = in.string();
          if (!in.ok()) return;
          if (r.source->mimes.count() >= kMaxTypes || type.size() > kMaxTypeLength) return;      // (more is ignored)
          String t(allocator_);
          if (t.assign(type)) r.source->mimes.push_back(util::move(t));
          return;
        }
        case 1: source_gone(c, id), destroy_object(c, id); return;      // destroy
        case 2: {      // set_actions(actions) (3)
          const uint32_t actions = in.u();
          if (!in.ok()) return;
          if (actions & ~kActionAll) {
            post_error(c, id, 0, "invalid action mask");
            return;
          }
          r.source->actions = actions;
          return;
        }
        default: post_error(c, id, kErrorInvalidMethod, "wl_data_source has no such request"); return;
      }
    }
    case Iface::DataDevice:
      switch (opcode) {
        case 0: {      // start_drag(source, origin, icon, serial)
          const uint32_t source = in.object(), origin = in.object(), icon = in.object();
          in.u();
          if (!in.ok()) return;
          Resource* sr = source ? object_of(c, source, Iface::DataSource, id) : nullptr;
          if (source && !sr) return;
          Resource* orr = object_of(c, origin, Iface::Surface, id);
          if (!orr) return;
          Resource* ir = icon ? object_of(c, icon, Iface::Surface, id) : nullptr;
          if (icon && !ir) return;
          if (sr && sr->source && sr->source->used) {
            post_error(c, id, 1, "the source was used already");
            return;
          }
          if (sr && sr->source) sr->source->used = true;
          drag_.icon = ir ? ir->surface : nullptr;
          start_drag(c, source, orr->surface);
          return;
        }
        case 1: {      // set_selection(source, serial)
          const uint32_t source = in.object();
          in.u();
          if (!in.ok()) return;
          Resource* sr = source ? object_of(c, source, Iface::DataSource, id) : nullptr;
          if (source && !sr) return;
          if (sr && sr->source) {
            if (sr->source->used) {
              post_error(c, id, 1, "the source was used already");
              return;
            }
            sr->source->used = true;
          }
          set_selection(&c, source);
          return;
        }
        case 2: destroy_object(c, id); return;      // release
        default: post_error(c, id, kErrorInvalidMethod, "wl_data_device has no such request"); return;
      }
    case Iface::DataOffer: {
      if (!r.offer) return;
      OfferInfo& o = *r.offer;
      switch (opcode) {
        case 0: {      // accept(serial, mime or null)
          in.u();
          bool present = false;
          const StringView type = in.string(&present);
          if (!in.ok()) return;
          if (!o.dnd || drag_.offer != id || drag_.client == 0) return;
          o.accepted = present;
          if (Client* sc = client_of(o.source_client); sc && source_of(o.source_client, o.source_id)) {
            Event e(o.source_id, 0);      // target(mime or null)
            if (present) e.string(type);
            else e.u(0);
            send(*sc, e);
          }
          return;
        }
        case 1: {      // receive(mime, fd)
          const StringView type = in.string();
          Fd fd = in.fd();
          if (!in.ok()) return;
          if (o.source_id == 0 && !o.dnd) {      // the desktop's clipboard: its text, written by the transport
            if (!sel_desktop_ || !text_type(type)) return;      // (the descriptor is let go: the client reads nothing)
            TransportRequest w;
            w.kind = TransportRequest::Kind::Write;
            w.job = next_job_++;
            w.fd = util::move(fd);
            w.data = SharedPointer<ByteBuffer>::make(allocator_, allocator_);
            if (!w.data || !w.data->append(desktop_text_.view().bytes())) return;
            request(util::move(w));
            return;
          }
          Client* sc = client_of(o.source_client);
          if (!sc || !source_of(o.source_client, o.source_id)) return;      // (the source is gone)
          Event e(o.source_id, 1);      // send(mime, fd)
          e.string(type).fd(fd);
          send(*sc, e);
          return;
        }
        case 2:      // destroy
          if (o.dnd && drag_.dropped && drag_.offer == id) {      // (dropped and never finished: the source hears it was cancelled)
            if (Client* sc = client_of(o.source_client); sc && source_of(o.source_client, o.source_id)) send(*sc, Event(o.source_id, 2));
            drag_ = Drag();
          }
          destroy_object(c, id);
          return;
        case 3:      // finish (3)
          if (!o.dnd || !drag_.dropped || drag_.offer != id) {
            post_error(c, id, 0, "finish without a drop");
            return;
          }
          if (Client* sc = client_of(o.source_client); sc && source_of(o.source_client, o.source_id)) send(*sc, Event(o.source_id, 4));      // dnd_finished
          drag_ = Drag();
          return;
        case 4: {      // set_actions(actions, preferred) (3)
          const uint32_t actions = in.u(), preferred = in.u();
          if (!in.ok()) return;
          if ((actions & ~kActionAll) || (preferred & ~kActionAll)) {
            post_error(c, id, 1, "invalid action mask");
            return;
          }
          if (preferred && ((preferred & (preferred - 1)) || !(preferred & actions))) {
            post_error(c, id, 2, "invalid preferred action");
            return;
          }
          if (!o.dnd) {
            post_error(c, id, 3, "not a drag's offer");
            return;
          }
          o.actions = actions;
          o.preferred = preferred;
          const uint32_t chosen = drag_action(o);
          Event a(id, 2);      // action
          a.u(chosen);
          send(c, a);
          if (Client* sc = client_of(o.source_client); sc && source_of(o.source_client, o.source_id)) {
            Event sa(o.source_id, 5);
            sa.u(chosen);
            send(*sc, sa);
          }
          return;
        }
        default: post_error(c, id, kErrorInvalidMethod, "wl_data_offer has no such request"); return;
      }
    }
    default: return;
  }
}

// ------------------------------------------------------------------ drag and drop

uint32_t Server::drag_action(const OfferInfo& o) const {
  const DataSourceInfo* src = const_cast<Server*>(this)->source_of(o.source_client, o.source_id);
  const uint32_t source = src && src->actions ? src->actions : kActionCopy;      // (a source of version 1 or 2 copies)
  const uint32_t destination = o.actions ? o.actions : kActionCopy;
  const uint32_t both = source & destination;
  if (o.preferred & both) return o.preferred;
  if (both & kActionCopy) return kActionCopy;
  if (both & kActionMove) return kActionMove;
  return both & kActionAsk;
}

void Server::start_drag(Client& c, uint32_t source, Surface* origin) {
  if (drag_.active || drag_.dropped || buttons_ == 0 || !origin || !pointer_focus_ || pointer_focus_->client != c.number) {
    if (source) send(c, Event(source, 2));      // cancelled: no button is held on the client's surface
    drag_.icon = nullptr;
    return;
  }
  Surface* icon = drag_.icon;
  drag_ = Drag();
  drag_.active = true;
  drag_.client = c.number;
  drag_.source = source;
  drag_.origin = origin;
  drag_.icon = icon;
  set_pointer_focus(nullptr, 0, 0);      // (the pointer is the drag's: the origin's client sees it leave)
  drag_moved();
}

void Server::drag_moved() {
  int32_t lx = 0, ly = 0;
  Surface* under = surface_at(pointer_x_, pointer_y_, &lx, &ly);
  if (under && drag_.source == 0 && under->client != drag_.client) under = nullptr;      // (a drag without data stays in its client)
  if (under != drag_.focus) {
    drag_focus(under, lx, ly);
    return;
  }
  if (!under || (lx == drag_.lx && ly == drag_.ly)) return;
  drag_.lx = lx;
  drag_.ly = ly;
  Client* c = client_of(under->client);
  if (!c) return;
  List<uint32_t> devices(allocator_);
  devices_of(*c, &devices);
  const uint32_t time = now();
  for (uint32_t d : devices.view()) {
    Event e(d, 3);      // motion
    e.u(time).fixed(lx).fixed(ly);
    send(*c, e);
  }
}

void Server::drag_focus(Surface* s, int32_t lx, int32_t ly) {
  if (drag_.focus) {
    if (Client* c = client_of(drag_.focus->client)) {
      List<uint32_t> devices(allocator_);
      devices_of(*c, &devices);
      for (uint32_t d : devices.view()) send(*c, Event(d, 2));      // leave
    }
    if (Client* sc = client_of(drag_.client); sc && source_of(drag_.client, drag_.source)) {
      Event t(drag_.source, 0);      // target(null): nothing accepts it now
      t.u(0);
      send(*sc, t);
    }
  }
  drag_.focus = s;
  drag_.offer = 0;
  drag_.lx = lx;
  drag_.ly = ly;
  if (!s) return;
  Client* c = client_of(s->client);
  if (!c) return;
  List<uint32_t> devices(allocator_);
  devices_of(*c, &devices);
  const uint32_t serial = next_serial();
  for (uint32_t d : devices.view()) {
    const uint32_t offer = drag_.source ? make_offer(*c, d, drag_.client, drag_.source, true) : 0;
    if (offer) drag_.offer = offer;
    Event e(d, 1);      // enter(serial, surface, x, y, offer)
    e.u(serial).u(s->id).fixed(lx).fixed(ly).u(offer);
    send(*c, e);
  }
}

void Server::drag_button_released() {
  Client* fc = drag_.focus ? client_of(drag_.focus->client) : nullptr;
  Resource* offer = fc && drag_.offer ? fc->objects.find(drag_.offer) : nullptr;
  const bool accepted = offer && offer->iface == Iface::DataOffer && offer->offer && offer->offer->accepted && drag_action(*offer->offer) != 0;
  if (!accepted) {
    end_drag(false);
    return;
  }
  List<uint32_t> devices(allocator_);
  devices_of(*fc, &devices);
  for (uint32_t d : devices.view()) send(*fc, Event(d, 4));      // drop
  if (Client* sc = client_of(drag_.client); sc && source_of(drag_.client, drag_.source)) send(*sc, Event(drag_.source, 3));      // dnd_drop_performed
  drag_.active = false;
  drag_.dropped = true;      // (until the destination's finish, or its offer going)
  pointer_moved(false);
}

void Server::end_drag(bool dropped) {
  (void)dropped;
  if (drag_.focus)
    if (Client* c = client_of(drag_.focus->client)) {
      List<uint32_t> devices(allocator_);
      devices_of(*c, &devices);
      for (uint32_t d : devices.view()) send(*c, Event(d, 2));      // leave
    }
  if (Client* sc = client_of(drag_.client); sc && source_of(drag_.client, drag_.source)) send(*sc, Event(drag_.source, 2));      // cancelled
  const bool was_active = drag_.active;
  drag_ = Drag();
  if (was_active) pointer_moved(false);
}

}  // namespace wayland
