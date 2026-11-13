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

// The compositor's core: connections, the wire, objects, the registry and its globals, protocol errors.
#include "Wayland/WaylandInternal.hpp"

namespace wayland {

namespace {

struct GlobalInfo {
  const char* name;
  uint32_t version;
};

// The globals, by name (1-based). The interface of each is the matching Iface.
constexpr uint32_t kGlobalCount = 23;
constexpr GlobalInfo kGlobals[kGlobalCount] = {
    {"wl_compositor", 6}, {"wl_subcompositor", 1}, {"wl_shm", 2}, {"wl_seat", 8},
    {"wl_output", 4}, {"wl_data_device_manager", 3}, {"xdg_wm_base", 6}, {"zxdg_decoration_manager_v1", 1},
    {"zwp_linux_dmabuf_v1", 4},      // (only offered with set_dmabuf; 3 without an fd maker: version 4's feedback needs its format table)
    {"wp_linux_drm_syncobj_manager_v1", 1},      // (with set_dmabuf too: the transport makes the timelines)
    {"wp_viewporter", 1}, {"wp_fractional_scale_manager_v1", 1},
    {"xdg_activation_v1", 1}, {"wp_cursor_shape_manager_v1", 1}, {"zwp_primary_selection_device_manager_v1", 1}, {"zwp_relative_pointer_manager_v1", 1},
    {"zwp_pointer_constraints_v1", 1}, {"wp_presentation", 1}, {"zwp_idle_inhibit_manager_v1", 1}, {"zxdg_exporter_v2", 1}, {"zxdg_importer_v2", 1},
    {"zwp_tablet_manager_v2", 1}, {"zwlr_layer_shell_v1", 4},
};
constexpr uint32_t kDmabufGlobal = 9, kSyncobjGlobal = 10;

}  // namespace

Server::Server(allocators::IAllocator& allocator, Clock clock)
    : allocator_(allocator), clock_(clock), surfaces_(allocator), stack_(allocator), changes_(allocator), constraints_(allocator), tokens_(allocator), exports_(allocator), fetch_mime_(allocator), desktop_text_(allocator), requests_(allocator), touches_(allocator) {
  monitors_[0].rect = monitors_[0].work = monitors_[0].panel = {0, 0, static_cast<int32_t>(screen_w_), static_cast<int32_t>(screen_h_)};
}

Server::~Server() {
  for (uint32_t i = 1; i < kMaxClients; ++i) if (clients_[i]) close_client(i);
}

void Server::set_wake(Wake wake, void* context) {
  kernel::ScopedMutex lock(mutex_);
  wake_ = wake;
  wake_context_ = context;
}

void Server::set_fd_maker(MakeFd make, void* context) {
  kernel::ScopedMutex lock(mutex_);
  make_fd_ = make;
  make_fd_context_ = context;
  keymap_ = Fd();
  format_table_ = Fd();
}

void Server::wake() {
  Wake w = nullptr;
  void* context = nullptr;
  {
    kernel::ScopedMutex lock(mutex_);
    if (!wake_wanted_) return;
    wake_wanted_ = false;
    w = wake_;
    context = wake_context_;
  }
  if (w) w(context);
}

uint32_t Server::now() {
  if (clock_) return static_cast<uint32_t>(clock_());
  return static_cast<uint32_t>(++fake_clock_);
}

uint32_t Server::next_serial() {
  if (++serial_ == 0) serial_ = 1;
  return serial_;
}

Server::Client* Server::client_of(uint8_t number) { return number < kMaxClients ? clients_[number].get() : nullptr; }

// ------------------------------------------------------------------ the transport

uint32_t Server::open_client(uint64_t pid) {
  kernel::ScopedMutex lock(mutex_);
  for (uint32_t i = 1; i < kMaxClients; ++i) {
    if (clients_[i]) continue;
    clients_[i] = UniquePointer<Client>::make(allocator_, allocator_);
    if (!clients_[i]) return 0;
    Client& c = *clients_[i];
    c.number = static_cast<uint8_t>(i);
    c.pid = pid;
    Resource display;
    display.iface = Iface::Display;
    if (!c.objects.put(kDisplayId, display)) {
      clients_[i].reset();
      return 0;
    }
    return i;
  }
  return 0;
}

void Server::receive(uint32_t client, ConstByteView bytes, ArrayView<Fd> fds) {
  {
    kernel::ScopedMutex lock(mutex_);
    Client* c = client < kMaxClients ? clients_[client].get() : nullptr;
    if (!c || c->closing) return;
    for (Fd& f : fds) {
      if (c->fds_in.count() >= kMaxFds || !c->fds_in.enqueue(util::move(f))) {
        post_error(*c, kDisplayId, kErrorNoMemory, "too many descriptors");
        break;
      }
    }
    if (!c->closing && !c->in.append(bytes)) post_error(*c, kDisplayId, kErrorNoMemory, "out of memory");
    if (!c->closing) process(*c);
    poll_waiting();      // (commits waiting for an acquire point another client's request may have reached)
  }
  wake();      // (other clients may have been sent events)
}

bool Server::take_output(uint32_t client, ByteBuffer* out, List<Fd>* fds) {
  kernel::ScopedMutex lock(mutex_);
  Client* c = client < kMaxClients ? clients_[client].get() : nullptr;
  if (!c) return true;
  if (c->out.size() > 0) {
    if (!out->append(c->out.view())) return true;
    c->out.clear();
  }
  for (Fd& f : c->fds_out.view()) fds->push_back(util::move(f));
  c->fds_out.clear();
  return c->closing;
}

bool Server::has_output(uint32_t client) {
  kernel::ScopedMutex lock(mutex_);
  Client* c = client < kMaxClients ? clients_[client].get() : nullptr;
  return c && (c->out.size() > 0 || c->fds_out.count() > 0);
}

uint32_t Server::client_count() {
  kernel::ScopedMutex lock(mutex_);
  uint32_t n = 0;
  for (uint32_t i = 1; i < kMaxClients; ++i) if (clients_[i]) ++n;
  return n;
}

void Server::close_client(uint32_t client) {
  {
    kernel::ScopedMutex lock(mutex_);
    Client* c = client < kMaxClients ? clients_[client].get() : nullptr;
    if (!c) return;
    // its surfaces go (their windows with them); the rest is in its object table
    List<Surface*> mine(allocator_);
    for (Surface& s : surfaces_) if (s.client == c->number) mine.push_back(&s);
    for (Surface* s : mine.view()) destroy_surface(*c, *s);
    const uint8_t number = c->number;
    if (drag_.client == number) drag_.source = 0, end_drag(false);
    for (uint64_t i = constraints_.count(); i-- > 0;) if (constraints_[i].client == number) constraints_.remove_at(i);
    for (uint64_t i = touches_.count(); i-- > 0;) if (touches_[i].client == number) touches_.remove_at(i);      // (its fingers: nobody's now)
    if (pen_.client == number) pen_ = PenFocus();      // (and the pen)
    if (psel_client_ == number) psel_client_ = 0, psel_source_ = 0;
    if (psel_sent_client_ == number) psel_sent_client_ = 0;
    if (sel_sent_client_ == number) sel_sent_client_ = 0;
    clients_[client].reset();
    if (sel_client_ == number) {      // (its selection goes with it)
      sel_client_ = 0;
      sel_source_ = 0;
      fetch_job_ = 0;
      if (keyboard_focus_)
        if (Client* fc = client_of(keyboard_focus_->client)) send_selection(*fc);
    }
  }
  wake();
}

// ------------------------------------------------------------------ messages

void Server::process(Client& c) {
  const ConstByteView all = c.in.view();
  uint64_t at = 0;
  while (!c.closing && all.size() - at >= 8) {
    ByteReader header(all.subview(at, 8));
    const uint32_t object = header.le32();
    const uint32_t word = header.le32();
    const uint32_t size = word >> 16;
    const uint16_t opcode = static_cast<uint16_t>(word & 0xFFFF);
    if (size < 8 || size % 4 != 0 || size > kMaxMessage) {
      post_error(c, kDisplayId, kErrorInvalidMethod, "bad message size");
      break;
    }
    if (all.size() - at < size) break;
    Reader in(all.subview(at + 8, size - 8), c);
    at += size;
    Resource* r = c.objects.find(object);
    if (!r) {
      post_error(c, kDisplayId, kErrorInvalidObject, "no such object");
      break;
    }
    dispatch(c, object, *r, opcode, in);
    if (!c.closing && !in.ok()) post_error(c, object, kErrorInvalidMethod, "malformed request");
  }
  if (c.closing) {
    c.in.clear();
    return;
  }
  // keep the part of a message that has not come in whole
  const uint64_t rest = all.size() - at;
  if (at > 0) {
    ByteView v = c.in.writable_view();
    for (uint64_t i = 0; i < rest; ++i) v[i] = v[at + i];
    c.in.truncate(rest);
  }
}

void Server::send(Client& c, const Event& e) {
  if (c.closing || !e.ok()) return;
  const ConstByteView args = e.arguments();
  const uint32_t size = static_cast<uint32_t>(8 + args.size());
  if (c.out.size() + size > kMaxOutputBytes) {      // it does not read: it goes
    c.closing = true;
    return;
  }
  if (!c.out.put32(e.object()) || !c.out.put32((size << 16) | e.opcode()) || !c.out.append(args)) {
    c.closing = true;
    return;
  }
  if (e.descriptor()) {
    if (!c.fds_out.push_back(e.descriptor())) c.closing = true;
  }
  wake_wanted_ = true;
}

void Server::post_error(Client& c, uint32_t object, uint32_t code, StringView message) {
  if (c.closing) return;
  Event e(kDisplayId, 0);
  e.u(object).u(code).string(message);
  send(c, e);
  c.closing = true;      // (what was owed is still sent: the error is the last of it)
}

void Server::delete_id(Client& c, uint32_t id) {
  Event e(kDisplayId, 1);
  e.u(id);
  send(c, e);
}

Server::Resource* Server::new_object(Client& c, uint32_t id, Iface iface, uint32_t version, uint32_t parent) {
  if (id == 0 || id > kLastClientId || c.objects.contains(id)) {
    post_error(c, parent, kErrorInvalidObject, "invalid new id");
    return nullptr;
  }
  if (c.objects.count() >= kMaxObjects) {
    post_error(c, kDisplayId, kErrorNoMemory, "too many objects");
    return nullptr;
  }
  Resource r;
  r.iface = iface;
  r.version = version;
  if (!c.objects.put(id, r)) {
    post_error(c, kDisplayId, kErrorNoMemory, "out of memory");
    return nullptr;
  }
  return c.objects.find(id);
}

Server::Resource* Server::new_server_object(Client& c, Iface iface, uint32_t version, uint32_t* id) {
  if (c.objects.count() >= kMaxObjects || c.next_server_id < kFirstServerId) {      // (ids are not reused: after 16M of them the client is out)
    post_error(c, kDisplayId, kErrorNoMemory, "too many objects");
    return nullptr;
  }
  Resource r;
  r.iface = iface;
  r.version = version;
  const uint32_t made = c.next_server_id++;
  if (!c.objects.put(made, r)) {
    post_error(c, kDisplayId, kErrorNoMemory, "out of memory");
    return nullptr;
  }
  *id = made;
  return c.objects.find(made);
}

void Server::set_dmabuf(bool offered) {
  kernel::ScopedMutex lock(mutex_);
  dmabuf_ = offered;
}

Server::Resource* Server::object_of(Client& c, uint32_t id, Iface iface, uint32_t from) {
  Resource* r = c.objects.find(id);
  if (!r || r->iface != iface) {
    post_error(c, from, kErrorInvalidObject, "wrong object");
    return nullptr;
  }
  return r;
}

void Server::destroy_object(Client& c, uint32_t id) {
  c.objects.remove(id);
  if (id <= kLastClientId) delete_id(c, id);      // (an id the server gave is not the client's to reuse: no delete_id, as libwayland)
}

// ------------------------------------------------------------------ the registry

void Server::bind(Client& c, uint32_t name, StringView interface, uint32_t version, uint32_t id) {
  const uint32_t most = name == kDmabufGlobal && !make_fd_ ? 3 : name >= 1 && name <= kGlobalCount ? kGlobals[name - 1].version : 0;
  if (name == 0 || name > kGlobalCount || ((name == kDmabufGlobal || name == kSyncobjGlobal) && !dmabuf_) || interface != StringView(kGlobals[name - 1].name) || version == 0 || version > most) {
    post_error(c, kDisplayId, kErrorInvalidObject, "invalid global");
    return;
  }
  constexpr Iface kIfaces[kGlobalCount] = {Iface::Compositor, Iface::Subcompositor, Iface::Shm, Iface::Seat,
                                           Iface::Output, Iface::DataDeviceManager, Iface::WmBase, Iface::DecorationManager, Iface::LinuxDmabuf, Iface::SyncobjManager,
                                           Iface::Viewporter, Iface::FractionalScaleManager, Iface::Activation, Iface::CursorShapeManager,
                                           Iface::PrimaryManager, Iface::RelativePointerManager, Iface::PointerConstraints, Iface::Presentation,
                                           Iface::IdleInhibitManager, Iface::Exporter, Iface::Importer, Iface::TabletManager, Iface::LayerShell};
  const Iface iface = kIfaces[name - 1];
  if (!new_object(c, id, iface, version, kDisplayId)) return;
  announce(c, id, iface, version);
}

// What a freshly bound global tells its client first.
void Server::announce(Client& c, uint32_t object, Iface iface, uint32_t version) {
  switch (iface) {
    case Iface::Shm: {
      const uint32_t formats[4] = {kFormatArgb8888, kFormatXrgb8888, kFormatAbgr8888, kFormatXbgr8888};
      for (uint32_t f : formats) {
        Event e(object, 0);
        e.u(f);
        send(c, e);
      }
      for (uint32_t f : yuv_formats()) {      // (the planes one after another in the pool: yuv_shm_layout)
        Event e(object, 0);
        e.u(f);
        send(c, e);
      }
      return;
    }
    case Iface::Seat: {
      Event caps(object, 0);
      caps.u(7);      // pointer | keyboard | touch (fingers go to a client that has a wl_touch, else they move the pointer)
      send(c, caps);
      if (version >= 2) {
        Event n(object, 1);
        n.string("seat0");
        send(c, n);
      }
      return;
    }
    case Iface::Output: {
      const Rect m = monitors_[0].rect;
      Event g(object, 0);
      // x, y, physical size in mm (96 dpi), subpixel unknown, make, model, transform normal
      g.i(m.x).i(m.y).i(m.w * 254 / 960).i(m.h * 254 / 960).i(0).string("TheoryCraftOS").string("Display").i(0);
      send(c, g);
      Event mode(object, 1);
      mode.u(3).i(m.w).i(m.h).i(60000);      // current | preferred, 60 Hz
      send(c, mode);
      if (version >= 2) {
        Event scale(object, 3);
        scale.i(1);
        send(c, scale);
      }
      if (version >= 4) {
        Event n(object, 4);
        n.string("OUT-1");
        send(c, n);
        Event d(object, 5);
        d.string("TheoryCraftOS desktop");
        send(c, d);
      }
      if (version >= 2) send(c, Event(object, 2));      // done
      return;
    }
    case Iface::Presentation: {      // clock_id: CLOCK_MONOTONIC (the clock of the times presented gives)
      Event e(object, 0);
      e.u(1);
      send(c, e);
      return;
    }
    case Iface::LinuxDmabuf: {
      // the formats, all linear: format events before version 3, modifier events in 3; from 4 on the client asks for feedback instead
      if (version >= 4) return;
      const uint32_t formats[4] = {kDrmArgb8888, kDrmXrgb8888, kDrmAbgr8888, kDrmXbgr8888};
      auto offer = [&](uint32_t f) {
        Event e(object, version >= 3 ? 1 : 0);
        e.u(f);
        if (version >= 3) e.u(uint32_t(kDrmModLinear >> 32)).u(uint32_t(kDrmModLinear));
        send(c, e);
      };
      for (uint32_t f : formats) offer(f);
      for (uint32_t f : yuv_formats()) offer(f);
      return;
    }
    default: return;
  }
}

// ------------------------------------------------------------------ dispatch

void Server::dispatch(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  switch (r.iface) {
    case Iface::Display:
      if (opcode == 0) {      // sync(callback): answered at once, after everything before it
        const uint32_t callback = in.u();
        if (!in.ok() || !new_object(c, callback, Iface::Callback, 1, id)) return;
        Event done(callback, 0);
        done.u(serial_);
        send(c, done);
        c.objects.remove(callback);
        delete_id(c, callback);
      } else if (opcode == 1) {      // get_registry
        const uint32_t registry = in.u();
        if (!in.ok() || !new_object(c, registry, Iface::Registry, 1, id)) return;
        for (uint32_t g = 0; g < kGlobalCount; ++g) {
          if ((g + 1 == kDmabufGlobal || g + 1 == kSyncobjGlobal) && !dmabuf_) continue;
          Event e(registry, 0);
          e.u(g + 1).string(kGlobals[g].name).u(g + 1 == kDmabufGlobal && !make_fd_ ? 3 : kGlobals[g].version);
          send(c, e);
        }
      } else {
        post_error(c, id, kErrorInvalidMethod, "wl_display has no such request");
      }
      return;
    case Iface::Registry: {
      if (opcode != 0) { post_error(c, id, kErrorInvalidMethod, "wl_registry has no such request"); return; }
      const uint32_t name = in.u();
      const StringView interface = in.string();
      const uint32_t version = in.u();
      const uint32_t object = in.u();
      if (!in.ok()) return;
      bind(c, name, interface, version, object);
      return;
    }
    case Iface::Compositor: {
      const uint32_t object = in.u();
      if (!in.ok()) return;
      if (opcode == 0) {      // create_surface
        Resource* nr = new_object(c, object, Iface::Surface, r.version, id);
        if (!nr) return;
        Surface* s = surfaces_.emplace(allocator_);
        if (!s || !s->stack.push_back(nullptr)) {
          if (s) surfaces_.erase(s);
          c.objects.remove(object);
          post_error(c, kDisplayId, kErrorNoMemory, "out of memory");
          return;
        }
        s->client = c.number;
        s->id = object;
        nr->surface = s;
        if (r.version >= 6) {      // (version 6: the scale and transform the client should draw with: the output's)
          Event scale(object, 2);
          scale.i(1);
          send(c, scale);
          Event transform(object, 3);
          transform.u(0);
          send(c, transform);
        }
      } else if (opcode == 1) {      // create_region
        Resource* nr = new_object(c, object, Iface::Region, 1, id);
        if (!nr) return;
        nr->region = SharedPointer<RegionRects>::make(allocator_, allocator_);
        if (!nr->region) post_error(c, kDisplayId, kErrorNoMemory, "out of memory");
      } else {
        post_error(c, id, kErrorInvalidMethod, "wl_compositor has no such request");
      }
      return;
    }
    case Iface::Surface: surface_request(c, id, r, opcode, in); return;
    case Iface::Region: region_request(c, id, r, opcode, in); return;
    case Iface::Subcompositor:
    case Iface::Subsurface: subsurface_request(c, id, r, opcode, in); return;
    case Iface::Shm:
    case Iface::ShmPool:
    case Iface::Buffer: shm_request(c, id, r, opcode, in); return;
    case Iface::Seat:
    case Iface::Keyboard:
    case Iface::Touch: seat_request(c, id, r, opcode, in); return;
    case Iface::Pointer: pointer_request(c, id, r, opcode, in); return;
    case Iface::Output:
      if (opcode == 0) destroy_object(c, id);      // release
      else post_error(c, id, kErrorInvalidMethod, "wl_output has no such request");
      return;
    case Iface::DataDeviceManager:
    case Iface::DataSource:
    case Iface::DataDevice:
    case Iface::DataOffer: data_request(c, id, r, opcode, in); return;
    case Iface::WmBase: wm_base_request(c, id, r, opcode, in); return;
    case Iface::Positioner: positioner_request(c, id, r, opcode, in); return;
    case Iface::XdgSurface: xdg_surface_request(c, id, r, opcode, in); return;
    case Iface::Toplevel: toplevel_request(c, id, r, opcode, in); return;
    case Iface::Popup: popup_request(c, id, r, opcode, in); return;
    case Iface::DecorationManager:
    case Iface::Decoration: decoration_request(c, id, r, opcode, in); return;
    case Iface::LinuxDmabuf:
    case Iface::BufferParams:
    case Iface::DmabufFeedback: dmabuf_request(c, id, r, opcode, in); return;
    case Iface::SyncobjManager:
    case Iface::SyncobjTimeline:
    case Iface::SyncobjSurface: syncobj_request(c, id, r, opcode, in); return;
    case Iface::Viewporter:
    case Iface::Viewport:
    case Iface::FractionalScaleManager:
    case Iface::FractionalScale: viewport_request(c, id, r, opcode, in); return;
    case Iface::Activation: case Iface::ActivationToken: case Iface::CursorShapeManager: case Iface::CursorShapeDevice: case Iface::PrimaryManager:
    case Iface::PrimarySource: case Iface::PrimaryDevice: case Iface::PrimaryOffer: case Iface::RelativePointerManager: case Iface::RelativePointer:
    case Iface::PointerConstraints: case Iface::LockedPointer: case Iface::ConfinedPointer: case Iface::Presentation: case Iface::PresentationFeedback:
    case Iface::IdleInhibitManager: case Iface::IdleInhibitor: case Iface::Exporter: case Iface::Exported: case Iface::Importer: case Iface::Imported:
      extras_request(c, id, r, opcode, in);
      return;
    case Iface::TabletManager: case Iface::TabletSeat: case Iface::Tablet: case Iface::TabletTool:
      tablet_request(c, id, r, opcode, in);
      return;
    case Iface::LayerShell: case Iface::LayerSurface:
      layer_request(c, id, r, opcode, in);
      return;
    case Iface::Callback:
      post_error(c, id, kErrorInvalidMethod, "wl_callback has no requests");
      return;
  }
}

Fd Server::keymap_fd() {
  if (!keymap_ && make_fd_) keymap_ = make_fd_(make_fd_context_, keymap_text());
  return keymap_;
}

}  // namespace wayland
