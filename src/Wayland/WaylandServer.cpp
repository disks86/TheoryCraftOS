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
constexpr uint32_t kGlobalCount = 8;
constexpr GlobalInfo kGlobals[kGlobalCount] = {
    {"wl_compositor", 6}, {"wl_subcompositor", 1}, {"wl_shm", 2}, {"wl_seat", 8},
    {"wl_output", 4}, {"wl_data_device_manager", 3}, {"xdg_wm_base", 5}, {"zxdg_decoration_manager_v1", 1},
};

}  // namespace

Server::Server(allocators::IAllocator& allocator, Clock clock)
    : allocator_(allocator), clock_(clock), surfaces_(allocator), stack_(allocator), changes_(allocator) {
  monitors_[0] = {{0, 0, static_cast<int32_t>(screen_w_), static_cast<int32_t>(screen_h_)}, {0, 0, static_cast<int32_t>(screen_w_), static_cast<int32_t>(screen_h_)}};
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
    clients_[client].reset();
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
  delete_id(c, id);
}

// ------------------------------------------------------------------ the registry

void Server::bind(Client& c, uint32_t name, StringView interface, uint32_t version, uint32_t id) {
  if (name == 0 || name > kGlobalCount || interface != StringView(kGlobals[name - 1].name) || version == 0 || version > kGlobals[name - 1].version) {
    post_error(c, kDisplayId, kErrorInvalidObject, "invalid global");
    return;
  }
  constexpr Iface kIfaces[kGlobalCount] = {Iface::Compositor, Iface::Subcompositor, Iface::Shm, Iface::Seat,
                                           Iface::Output, Iface::DataDeviceManager, Iface::WmBase, Iface::DecorationManager};
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
      return;
    }
    case Iface::Seat: {
      Event caps(object, 0);
      caps.u(3);      // pointer | keyboard
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
          Event e(registry, 0);
          e.u(g + 1).string(kGlobals[g].name).u(kGlobals[g].version);
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
    case Iface::DataDeviceManager: {
      const uint32_t object = in.u();
      if (opcode == 1) in.u();      // (the seat)
      if (!in.ok()) return;
      if (opcode == 0) new_object(c, object, Iface::DataSource, r.version, id);
      else if (opcode == 1) new_object(c, object, Iface::DataDevice, r.version, id);
      else post_error(c, id, kErrorInvalidMethod, "wl_data_device_manager has no such request");
      return;
    }
    case Iface::DataSource:      // offer(mime), destroy, set_actions: nothing is transferred yet
      if (opcode == 1) destroy_object(c, id);
      else if (opcode == 0) in.string();
      else if (opcode == 2) in.u();
      else post_error(c, id, kErrorInvalidMethod, "wl_data_source has no such request");
      return;
    case Iface::DataDevice:      // start_drag, set_selection, release
      if (opcode == 2) destroy_object(c, id);
      else if (opcode > 2) post_error(c, id, kErrorInvalidMethod, "wl_data_device has no such request");
      return;
    case Iface::WmBase: wm_base_request(c, id, r, opcode, in); return;
    case Iface::Positioner: positioner_request(c, id, r, opcode, in); return;
    case Iface::XdgSurface: xdg_surface_request(c, id, r, opcode, in); return;
    case Iface::Toplevel: toplevel_request(c, id, r, opcode, in); return;
    case Iface::Popup: popup_request(c, id, r, opcode, in); return;
    case Iface::DecorationManager:
    case Iface::Decoration: decoration_request(c, id, r, opcode, in); return;
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
