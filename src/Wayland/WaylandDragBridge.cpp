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

// Drag and drop between Wayland clients and the desktop's own windows (the File Explorer's files, text).
//
// The desktop's drag over a client's window (IWindowsSession::desktop_drag, from Desktop/Win32Windows.cpp as the pointer moves): a drag without a client
// source, which the compositor serves itself. The surface under the pointer is entered with an offer of text/uri-list (a file a Linux program can see:
// file:///mnt/desktop/..., file:///mnt/c/..., file:///...; filesystem::layout::linux_view_of) and text/plain (the Linux path, else the text), source
// actions copy; motion follows; the client accepts a type. Let go (desktop_drop) over a surface that accepted, it hears drop and reads the data with
// receive (the transport writes it); its finish (or its offer going) ends it. Anywhere else the drag is left (desktop_drag_end).
//
// A client's drag let go outside every surface (drag_button_released with nothing under the pointer): when its source offers text/uri-list or text,
// the source hears dnd_drop_performed and is asked for the data (a pipe the transport makes, as for the selection); the URIs become system paths
// (file URIs only, percent-decoded, through layout::system_path_of_linux), one per line, and the desktop drops them where the button was let go
// (take_program_drop; Win32Windows drops each on the desktop window there). program_drop_done tells the source dnd_finished when something took it, else
// cancelled. One such drop at a time.
//
// Before it is let go, a client's drag that leaves every surface is read the same way at once (program_drag_at), so that the desktop can show what it
// carries under the pointer and ask its windows whether they would take it (program_drag; Win32Windows::pump_program_drag). Its answer
// (program_drag_answer) goes to the source as target (the type read, or null) and action (copy, or none), as a client destination's accept would. Let go
// over the desktop, the drop uses what was read, or the read still under way (no second read).
#include "FileSystem/SystemLayout.hpp"
#include "Desktop/DropItems.hpp"
#include "Utility/FileUri.hpp"
#include "Wayland/WaylandInternal.hpp"

namespace wayland {

namespace {

constexpr uint64_t kMaxDropData = 1u << 20;
constexpr uint32_t kCopy = 1;      // (wl_data_device_manager.dnd_action copy)      // (more than a megabyte of URIs or text is not a drop)

// A text type a source may offer (as WaylandData.cpp takes them for the selection).
bool text_type_of(StringView t) {
  return t == StringView("text/plain;charset=utf-8") || t == StringView("text/plain;charset=UTF-8") || t == StringView("text/plain") ||
         t == StringView("UTF8_STRING") || t == StringView("STRING") || t == StringView("TEXT");
}

}  // namespace

// ------------------------------------------------------------------ the desktop's drag over a client

bool Server::desktop_drag(uint32_t window, int32_t x, int32_t y, StringView text) {
  bool taken = false;
  {
    kernel::ScopedMutex lock(mutex_);
    if (window == 0) {
      if (drag_.desktop && drag_.active) end_drag(false);
    } else if (!(drag_.active || drag_.dropped) || drag_.desktop) {      // (not while a client's drag runs)
      if (!drag_.desktop || !drag_.active) {
        drag_ = Drag();
        drag_.desktop = true;
        drag_.active = true;
        desktop_drag_text_.clear();
        desktop_drag_uri_.clear();
        desktop_drag_linux_.clear();
        const StringView t = text.size() > kMaxDropData ? text.first(0) : text;
        desktop_drag_text_.append(t);
        if (!t.empty() && t[0] == '/' && filesystem::layout::linux_view_of(t, &desktop_drag_linux_)) {      // a file a Linux program can open
          if (!util::file_uri(desktop_drag_linux_.view(), &desktop_drag_uri_)) desktop_drag_uri_.clear();
        } else {
          desktop_drag_linux_.clear();
        }
      }
      if (drag_.dropped) {
        taken = false;
      } else {
        drag_at(x, y);
        taken = drag_taken();
      }
    }
  }
  wake();
  return taken;
}

bool Server::desktop_drop() {
  bool dropped = false;
  {
    kernel::ScopedMutex lock(mutex_);
    if (!drag_.desktop || !drag_.active) return false;
    Client* fc = drag_.focus ? client_of(drag_.focus->client) : nullptr;
    if (!fc || !drag_taken()) {
      end_drag(false);
    } else {
      List<uint32_t> devices(allocator_);
      devices_of(*fc, &devices);
      for (uint32_t d : devices.view()) send(*fc, Event(d, 4));      // drop
      drag_.active = false;
      drag_.dropped = true;      // (until the destination's finish, or its offer going)
      dropped = true;
    }
  }
  wake();
  return dropped;
}

void Server::desktop_drag_end() {
  {
    kernel::ScopedMutex lock(mutex_);
    if (!drag_.desktop || !drag_.active) return;
    end_drag(false);
  }
  wake();
}

// ------------------------------------------------------------------ a client's drag let go over the desktop

void Server::reset_program_drop() {
  program_drop_.job = 0;
  program_drop_.client = 0;
  program_drop_.source = 0;
  program_drop_.mime.clear();
  program_drop_.text.clear();
  program_drop_.ready = program_drop_.taken = program_drop_.uri = false;
}

bool Server::choose_drop_type(const DataSourceInfo& src, String* mime, bool* uri) {
  const String* chosen = nullptr;
  *uri = false;
  for (const String& t : src.mimes.view())
    if (t.view() == StringView("text/uri-list")) chosen = &t, *uri = true;
  if (!chosen)
    for (const String& t : src.mimes.view())
      if (text_type_of(t.view()) && (!chosen || t.view() == StringView("text/plain;charset=utf-8"))) chosen = &t;
  return chosen && mime->assign(chosen->view());
}

bool Server::start_program_drop() {
  if (!attached_ || program_drop_.job != 0 || program_drop_.ready) return false;
  DataSourceInfo* src = source_of(drag_.client, drag_.source);
  if (!src) return false;
  ProgramDrag& g = program_drag_;
  const bool reading = g.job != 0 && g.client == drag_.client && g.source == drag_.source;      // (the drag over the desktop asked for the data)
  const bool read_already = reading && g.read;
  if (read_already && g.text.empty()) return false;      // (nothing in it is for the desktop: cancelled)
  bool uri = false;
  if (!choose_drop_type(*src, &program_drop_.mime, &uri)) return false;
  program_drop_.client = drag_.client;
  program_drop_.source = drag_.source;
  program_drop_.uri = uri;
  program_drop_.x = pointer_x_;
  program_drop_.y = pointer_y_;
  if (read_already) {      // what the drag over the desktop read is the drop
    if (!program_drop_.text.assign(g.text.view())) {
      reset_program_drop();
      return false;
    }
    program_drop_.job = g.job;      // (a job of its own: none is coming)
    program_drop_.ready = true;
  } else if (reading) {
    program_drop_.job = g.job;      // (the read under way is the drop's: its pipe and data come to program_drop_ now)
  } else {
    TransportRequest r;
    r.kind = TransportRequest::Kind::MakePipe;
    r.job = next_job_++;
    program_drop_.job = r.job;
    request(util::move(r));
  }
  reset_program_drag();
  if (Client* sc = client_of(drag_.client)) send(*sc, Event(drag_.source, 3));      // dnd_drop_performed
  drag_ = Drag();      // (over for the compositor: the source waits for dnd_finished or cancelled, program_drop_done)
  pointer_moved(false);
  return true;
}

bool Server::drop_items(ConstByteView data, bool uri, String* out) {
  const ConstByteView bytes = data.size() > kMaxDropData ? data.first(0) : data;
  return desktop::drop_items_of(bytes, uri, out);
}

void Server::program_drop_read(ConstByteView data) {
  ProgramDrop& p = program_drop_;
  if (!drop_items(data, p.uri, &p.text)) {      // nothing for the desktop: the source hears it was cancelled
    if (Client* c = client_of(p.client); c && source_of(p.client, p.source)) send(*c, Event(p.source, 2));
    reset_program_drop();
    wake_wanted_ = true;
    return;
  }
  p.ready = true;
}

// ------------------------------------------------------------------ a client's drag over the desktop

void Server::reset_program_drag() {
  ProgramDrag& g = program_drag_;
  g.job = 0;
  g.client = 0;
  g.source = 0;
  g.mime.clear();
  g.text.clear();
  g.uri = g.read = g.over = false;
  g.answer = -1;
}

void Server::program_drag_at(int32_t x, int32_t y, bool over) {
  ProgramDrag& g = program_drag_;
  if (!drag_.active || drag_.desktop || drag_.source == 0 || !attached_) return;
  const bool same = g.job != 0 && g.client == drag_.client && g.source == drag_.source;
  if (!over) {
    if (same && g.over && g.answer == 1)      // (it was the desktop's: a surface's accept decides now)
      if (Client* sc = client_of(g.client); sc && source_of(g.client, g.source)) {
        Event t(g.source, 0);      // target(null)
        t.u(0);
        send(*sc, t);
      }
    if (same) {
      g.over = false;
      g.answer = -1;
    }
    return;
  }
  if (!same) {      // first time over the desktop: read what it carries
    reset_program_drag();
    DataSourceInfo* src = source_of(drag_.client, drag_.source);
    bool uri = false;
    if (!src || !choose_drop_type(*src, &g.mime, &uri)) return;
    g.client = drag_.client;
    g.source = drag_.source;
    g.uri = uri;
    TransportRequest r;
    r.kind = TransportRequest::Kind::MakePipe;
    r.job = next_job_++;
    g.job = r.job;
    request(util::move(r));
  }
  g.over = true;
  g.x = x;
  g.y = y;
}

bool Server::program_drag(String* text, int32_t* x, int32_t* y) {
  kernel::ScopedMutex lock(mutex_);
  const ProgramDrag& g = program_drag_;
  if (g.job == 0 || !g.over || !g.read || g.text.empty() || !drag_.active || drag_.client != g.client || drag_.source != g.source) return false;
  if (!text->assign(g.text.view())) return false;
  *x = g.x;
  *y = g.y;
  return true;
}

void Server::program_drag_answer(bool would_take) {
  {
    kernel::ScopedMutex lock(mutex_);
    ProgramDrag& g = program_drag_;
    if (g.job == 0 || !g.over || !drag_.active || drag_.client != g.client || drag_.source != g.source) return;
    Client* sc = client_of(g.client);
    DataSourceInfo* src = source_of(g.client, g.source);
    if (!sc || !src) return;
    const bool copies = src->actions == 0 || (src->actions & kCopy);      // (the desktop only copies)
    const int8_t answer = would_take && copies ? 1 : 0;
    if (answer == g.answer) return;
    g.answer = answer;
    Event t(g.source, 0);      // target(mime or null)
    if (answer) t.string(g.mime.view());
    else t.u(0);
    send(*sc, t);
    Resource* r = sc->objects.find(g.source);
    if (r && r->version >= 3) {
      Event a(g.source, 5);      // action
      a.u(answer ? kCopy : 0);
      send(*sc, a);
    }
  }
  wake();
}

bool Server::take_program_drop(String* text, int32_t* x, int32_t* y) {
  kernel::ScopedMutex lock(mutex_);
  if (!program_drop_.ready || program_drop_.taken) return false;
  program_drop_.taken = true;
  if (!text->assign(program_drop_.text.view())) return false;
  *x = program_drop_.x;
  *y = program_drop_.y;
  return true;
}

void Server::program_drop_done(bool taken) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (!program_drop_.ready) return;
    if (Client* c = client_of(program_drop_.client); c && source_of(program_drop_.client, program_drop_.source)) {
      Resource* r = c->objects.find(program_drop_.source);
      const bool v3 = r && r->version >= 3;
      if (taken && v3) send(*c, Event(program_drop_.source, 4));      // dnd_finished
      else if (!taken) send(*c, Event(program_drop_.source, 2));      // cancelled
    }
    reset_program_drop();
  }
  wake();
}

}  // namespace wayland
