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

#include "OpenGl/GlService.hpp"

namespace opengl {

namespace {
GlService* g_service = nullptr;
}

void set_system_gl(GlService* service) { g_service = service; }
GlService* system_gl() { return g_service; }

GlService::Context* GlService::Process::find(uint32_t handle) {
  for (auto& c : contexts) if (c && c->handle == handle) return c.get();
  return nullptr;
}
GlService::Context* GlService::Process::current(uint64_t thread) {
  for (auto& c : contexts) if (c && c->thread == thread) return c.get();
  return nullptr;
}
GlService::Surface* GlService::Process::surface(uint64_t window) {
  for (auto& s : surfaces) if (s && s->window == window) return s.get();
  return nullptr;
}

SharedPointer<GlService::Process> GlService::process(uint64_t pid, bool create) {
  kernel::ScopedMutex g(lock_);
  for (auto& p : processes_) if (p->pid == pid) return p;
  if (!create) return SharedPointer<Process>();
  SharedPointer<Process> made = SharedPointer<Process>::make(a_, a_);
  if (!made) return made;
  made->pid = pid;
  if (!processes_.push_back(made)) return SharedPointer<Process>();
  return made;
}

GlStatus GlService::create_context(uint64_t pid, const ContextRequest& request, uint32_t* handle) {
  SharedPointer<Process> p = process(pid, true);
  if (!p) return GlStatus::NoMemory;
  kernel::ScopedMutex g(p->lock);
  SharedPointer<ShareGroup> group;
  if (request.share) {
    Context* other = p->find(request.share);
    if (!other) return GlStatus::InvalidHandle;
    group = other->group;
  } else {
    group = SharedPointer<ShareGroup>::make(a_, a_);
  }
  if (!group) return GlStatus::NoMemory;
  UniquePointer<Context> c = UniquePointer<Context>::make(a_);
  if (!c) return GlStatus::NoMemory;
  ContextConfig config;
  config.major = request.major;
  config.minor = request.minor;
  config.core_profile = request.core_profile;
  config.forward_compatible = request.forward_compatible;
  config.debug = request.debug;
  c->api = UniquePointer<GlApi>::make(a_, a_, *group, config);
  if (!c->api) return GlStatus::NoMemory;
  c->group = group;
  c->request = request;
  c->handle = p->next_handle++;
  *handle = c->handle;
  if (!p->contexts.push_back(static_cast<UniquePointer<Context>&&>(c))) return GlStatus::NoMemory;
  return GlStatus::Ok;
}

GlStatus GlService::delete_context(uint64_t pid, uint32_t handle) {
  SharedPointer<Process> p = process(pid, false);
  if (!p) return GlStatus::InvalidHandle;
  kernel::ScopedMutex g(p->lock);
  for (uint64_t i = 0; i < p->contexts.count(); ++i) {
    if (p->contexts[i] && p->contexts[i]->handle == handle) {
      p->contexts.remove_at(i);      // (the API goes before its share group: members in that order)
      return GlStatus::Ok;
    }
  }
  return GlStatus::InvalidHandle;
}

GlStatus GlService::share_objects(uint64_t pid, uint32_t source, uint32_t context) {
  SharedPointer<Process> p = process(pid, false);
  if (!p) return GlStatus::InvalidHandle;
  kernel::ScopedMutex g(p->lock);
  Context* from = p->find(source);
  Context* to = p->find(context);
  if (!from || !to) return GlStatus::InvalidHandle;
  if (from->group.get() == to->group.get()) return GlStatus::Ok;
  // The target keeps its state (it may even be current) and takes the source's objects.
  to->api->join_group(*from->group);
  to->group = from->group;
  return GlStatus::Ok;
}

GlService::Surface* GlService::surface_for(Process& p, const SurfaceRequest& r) {
  Surface* s = p.surface(r.window);
  if (s) {
    if (s->surface->width() != r.width || s->surface->height() != r.height) s->surface->resize(r.width, r.height);
    return s;
  }
  UniquePointer<Surface> made = UniquePointer<Surface>::make(a_);
  if (!made) return nullptr;
  SurfaceConfig config;
  config.width = r.width ? r.width : 1;
  config.height = r.height ? r.height : 1;
  config.double_buffered = r.double_buffered;
  config.depth_bits = r.depth_bits;
  config.stencil_bits = r.stencil_bits;
  config.alpha_bits = r.alpha_bits;
  config.accum_bits = r.accum_bits;
  made->surface = UniquePointer<GlSurface>::make(a_, a_, config);
  if (!made->surface || !made->surface->valid()) return nullptr;
  made->window = r.window;
  Surface* result = made.get();
  if (!p.surfaces.push_back(static_cast<UniquePointer<Surface>&&>(made))) return nullptr;
  return result;
}

GlStatus GlService::make_current(uint64_t pid, uint64_t thread, uint32_t context, const SurfaceRequest& request) {
  SharedPointer<Process> p = process(pid, context != 0);
  if (!p) return context ? GlStatus::NoMemory : GlStatus::Ok;
  kernel::ScopedMutex g(p->lock);
  Context* old = p->current(thread);
  if (context == 0) {
    if (old) {
      old->thread = 0;
      old->api->bind_surface(nullptr);
    }
    return GlStatus::Ok;
  }
  Context* c = p->find(context);
  if (!c) return GlStatus::InvalidHandle;
  if (c->thread && c->thread != thread) return GlStatus::Busy;
  Surface* s = request.window ? surface_for(*p, request) : nullptr;
  if (request.window && !s) return GlStatus::NoMemory;
  if (old && old != c) {
    old->thread = 0;
    old->api->bind_surface(nullptr);
  }
  c->thread = thread;
  c->window = request.window;
  c->api->bind_surface(s ? s->surface.get() : nullptr);
  return GlStatus::Ok;
}

GlStatus GlService::execute(uint64_t pid, uint64_t thread, ConstByteView stream, IClientMemory& memory, uint64_t* result) {
  *result = 0;
  SharedPointer<Process> p = process(pid, false);
  if (!p) return GlStatus::NoContext;
  kernel::ScopedMutex g(p->lock);
  Context* c = p->current(thread);
  if (!c) return GlStatus::NoContext;
  *result = c->api->execute(stream, memory);
  return GlStatus::Ok;
}

GlStatus GlService::swap(uint64_t pid, uint64_t window, uint32_t width, uint32_t height, bool do_swap, PresentFunction present, void* present_context) {
  SharedPointer<Process> p = process(pid, false);
  if (!p) return GlStatus::InvalidHandle;
  ByteBuffer rows(a_);
  uint32_t w = 0, h = 0;
  {
    kernel::ScopedMutex g(p->lock);
    Surface* s = p->surface(window);
    if (!s) return GlStatus::InvalidHandle;
    if (do_swap) s->surface->swap();
    const webgl::Image& front = s->surface->front();
    if (present && front.defined()) {
      // GL keeps the bottom row first.
      const uint64_t row = uint64_t{front.w} * 4;
      if (!rows.put_zeros(row * front.h)) return GlStatus::NoMemory;
      ByteView dst = rows.writable_view();
      const ConstByteView src = front.px.view();
      for (uint32_t y = 0; y < front.h; ++y) dst.subview(uint64_t{y} * row, row).copy_from(src.subview(uint64_t{front.h - 1 - y} * row, row));
      w = front.w;
      h = front.h;
    }
    // A changed window size takes effect from the next frame (the contexts drawing to it see the new images when they next run).
    if (width && height && (width != s->surface->width() || height != s->surface->height())) s->surface->resize(width, height);
  }
  if (present && w) present(present_context, window, rows.view(), w, h);
  return GlStatus::Ok;
}

bool GlService::take_flushed(uint64_t pid, uint64_t window) {
  SharedPointer<Process> p = process(pid, false);
  if (!p) return false;
  kernel::ScopedMutex g(p->lock);
  Surface* s = p->surface(window);
  return s && s->surface->take_flushed();
}

GlStatus GlService::read_front(uint64_t pid, uint64_t window, ByteView out, uint32_t pitch, uint32_t* width, uint32_t* height) {
  SharedPointer<Process> p = process(pid, false);
  if (!p) return GlStatus::InvalidHandle;
  kernel::ScopedMutex g(p->lock);
  Surface* s = p->surface(window);
  if (!s) return GlStatus::InvalidHandle;
  const webgl::Image& front = s->surface->front();
  *width = front.w;
  *height = front.h;
  if (out.empty()) return GlStatus::Ok;
  if (pitch < uint64_t{front.w} * 4 || out.size() / pitch < front.h) return GlStatus::InvalidParameter;
  const ConstByteView px = front.px.view();
  for (uint32_t y = 0; y < front.h; ++y) {
    // GL keeps the bottom row first.
    const ConstByteView src = px.subview(uint64_t{front.h - 1 - y} * front.w * 4, uint64_t{front.w} * 4);
    ByteView dst = out.subview(uint64_t{y} * pitch, uint64_t{front.w} * 4);
    for (uint64_t x = 0; x < front.w; ++x) {
      dst[x * 4] = src[x * 4 + 2];
      dst[x * 4 + 1] = src[x * 4 + 1];
      dst[x * 4 + 2] = src[x * 4];
      dst[x * 4 + 3] = src[x * 4 + 3];
    }
  }
  return GlStatus::Ok;
}

void GlService::process_exited(uint64_t pid) {
  SharedPointer<Process> gone;
  {
    kernel::ScopedMutex g(lock_);
    for (uint64_t i = 0; i < processes_.count(); ++i) {
      if (processes_[i]->pid == pid) {
        gone = processes_[i];
        processes_.remove_at(i);
        break;
      }
    }
  }
  if (!gone) return;
  kernel::ScopedMutex g(gone->lock);      // (a thread still inside a call finishes first)
  gone->contexts.clear();
  gone->surfaces.clear();
}

uint32_t GlService::context_count(uint64_t pid) {
  SharedPointer<Process> p = process(pid, false);
  if (!p) return 0;
  kernel::ScopedMutex g(p->lock);
  return static_cast<uint32_t>(p->contexts.count());
}

}  // namespace opengl
