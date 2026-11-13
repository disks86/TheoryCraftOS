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

// Pens (zwp_tablet_manager_v2, version 1): a client's tablet seat is told of one tablet ("TheoryCraftOS pen") and two tools, a pen and its eraser, each
// with pressure and tilt; no pads (the desktop has none to give). The desktop hands the compositor the pen (Server::pen); over a surface of a client with a
// tablet seat the tool of the end near the screen comes into proximity of that surface (proximity_in), then each report is motion, pressure, tilt, down or
// up as the tip touches or lifts, the barrel buttons (BTN_STYLUS, BTN_STYLUS2) and a frame. While the tip touches, the surface keeps the pen wherever it goes
// (an implicit grab, as a pointer's button); otherwise it moves to whatever surface is under it, leaving the old one (proximity_out). Over a surface of a
// client without one (or nowhere) the pen is refused, and the desktop moves the pointer with it. Out of range: proximity_out.
#include "Wayland/WaylandInternal.hpp"

namespace wayland {

namespace {
constexpr uint32_t kToolPen = 0x140, kToolEraser = 0x141;
constexpr uint32_t kCapabilityTilt = 1, kCapabilityPressure = 2;
constexpr uint32_t kBtnStylus = 0x14B, kBtnStylus2 = 0x14C;
// zwp_tablet_tool_v2's events
enum : uint16_t { kToolType = 0, kToolCapability = 3, kToolDone = 4, kProximityIn = 6, kProximityOut = 7, kDown = 8, kUp = 9, kMotion = 10, kPressure = 11, kTilt = 13,
                  kButton = 17, kFrame = 18 };
}  // namespace

void Server::announce_tablet(Client& c, uint32_t seat_id, Resource& seat) {
  uint32_t tablet = 0, pen = 0, eraser = 0;
  if (!new_server_object(c, Iface::Tablet, seat.version, &tablet)) return;
  Event added(seat_id, 0);      // tablet_added
  added.u(tablet);
  send(c, added);
  Event name(tablet, 0);
  name.string(StringView("TheoryCraftOS pen"));
  send(c, name);
  send(c, Event(tablet, 3));      // done
  for (uint32_t k = 0; k < 2; ++k) {
    uint32_t tool = 0;
    if (!new_server_object(c, Iface::TabletTool, seat.version, &tool)) return;
    Event t(seat_id, 1);      // tool_added
    t.u(tool);
    send(c, t);
    Event type(tool, kToolType);
    type.u(k == 0 ? kToolPen : kToolEraser);
    send(c, type);
    const uint32_t capabilities[2] = {kCapabilityTilt, kCapabilityPressure};
    for (uint32_t capability : capabilities) {
      Event cap(tool, kToolCapability);
      cap.u(capability);
      send(c, cap);
    }
    send(c, Event(tool, kToolDone));
    (k == 0 ? pen : eraser) = tool;
  }
  // (looked up again: making the objects may have moved the seat's entry in the table)
  if (Resource* r = c.objects.find(seat_id)) {
    r->tablet = tablet;
    r->pen_tool = pen;
    r->eraser_tool = eraser;
  }
}

void Server::tablet_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  switch (r.iface) {
    case Iface::TabletManager:
      if (opcode == 0) {      // get_tablet_seat(new id, seat)
        const uint32_t object = in.u();
        in.object();
        if (!in.ok()) return;
        if (!new_object(c, object, Iface::TabletSeat, r.version, id)) return;
        if (Resource* seat = c.objects.find(object)) announce_tablet(c, object, *seat);
      } else if (opcode == 1) {
        destroy_object(c, id);
      } else {
        post_error(c, id, kErrorInvalidMethod, "zwp_tablet_manager_v2 has no such request");
      }
      return;
    case Iface::TabletSeat:
    case Iface::Tablet:
      if (opcode == 0) destroy_object(c, id);
      else post_error(c, id, kErrorInvalidMethod, "no such request");
      return;
    case Iface::TabletTool:
      if (opcode == 0) {      // set_cursor(serial, surface, x, y): the desktop draws its own pointer (the pen moves it only for clients that do not read pens)
        in.u();
        in.object();
        in.i();
        in.i();
        return;
      }
      if (opcode == 1) {
        if (pen_.client == c.number && pen_.tool == id) pen_ = PenFocus();      // (the tool goes: so does its proximity)
        destroy_object(c, id);
        return;
      }
      post_error(c, id, kErrorInvalidMethod, "zwp_tablet_tool_v2 has no such request");
      return;
    default: return;
  }
}

void Server::pen_leave() {
  Client* c = pen_.client ? client_of(pen_.client) : nullptr;
  if (c && c->objects.find(pen_.tool)) {
    if (pen_.tip) send(*c, Event(pen_.tool, kUp));
    send(*c, Event(pen_.tool, kProximityOut));
    Event f(pen_.tool, kFrame);
    f.u(now());
    send(*c, f);
  }
  pen_ = PenFocus();
}

bool Server::pen(const desktop::PenInput& p) {
  bool taken = false;
  {
    kernel::ScopedMutex lock(mutex_);
    if (!p.in_range) {
      taken = pen_.client != 0;
      pen_leave();
    } else {
      // where the pen goes: the surface that has it while the tip touches, else the one under it
      Client* c = nullptr;
      uint32_t surface = 0, tool = 0, tablet = 0;
      int32_t lx = 0, ly = 0;
      const bool grabbed = pen_.client != 0 && pen_.tip && p.tip && (p.inverted == pen_.eraser);
      if (grabbed) {
        c = client_of(pen_.client);
        surface = pen_.surface;
        tool = pen_.tool;
        lx = p.x - pen_.origin_x;
        ly = p.y - pen_.origin_y;
      } else if (Surface* s = surface_at(p.x, p.y, &lx, &ly)) {
        c = client_of(s->client);
        surface = s->id;
        ObjectCursor cursor;
        for (bool more = c ? c->objects.first(&cursor) : false; more && !tool; more = c->objects.next(&cursor)) {
          const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
          if (kv->value.iface != Iface::TabletSeat) continue;
          const uint32_t t = p.inverted ? kv->value.eraser_tool : kv->value.pen_tool;
          if (t && c->objects.find(t) && c->objects.find(kv->value.tablet)) tool = t, tablet = kv->value.tablet;
        }
      }
      if (!c || !tool) {
        pen_leave();      // (refused: a surface whose client reads no pens, or none)
      } else {
        if (!grabbed && (pen_.client != c->number || pen_.surface != surface || pen_.tool != tool)) {
          pen_leave();
          Event in(tool, kProximityIn);
          in.u(next_serial()).u(tablet).u(surface);
          send(*c, in);
          pen_.client = c->number;
          pen_.surface = surface;
          pen_.tool = tool;
          pen_.eraser = p.inverted;
          pen_.origin_x = p.x - lx;
          pen_.origin_y = p.y - ly;
        }
        Event motion(tool, kMotion);
        motion.fixed(lx).fixed(ly);
        send(*c, motion);
        Event pressure(tool, kPressure);
        pressure.u(p.pressure);
        send(*c, pressure);
        Event tilt(tool, kTilt);
        tilt.fixed(p.tilt_x).fixed(p.tilt_y);
        send(*c, tilt);
        if (p.tip != pen_.tip) {
          if (p.tip) {
            Event down(tool, kDown);
            down.u(next_serial());
            send(*c, down);
          } else {
            send(*c, Event(tool, kUp));
          }
          pen_.tip = p.tip;
        }
        const uint8_t changed = static_cast<uint8_t>(p.buttons ^ pen_.buttons);
        const struct { uint8_t bit; uint32_t code; } kButtons[2] = {{hardware::pen_button::kBarrel, kBtnStylus}, {hardware::pen_button::kSecondary, kBtnStylus2}};
        for (const auto& b : kButtons) {
          if (!(changed & b.bit)) continue;
          Event e(tool, kButton);
          e.u(next_serial()).u(b.code).u((p.buttons & b.bit) ? 1 : 0);
          send(*c, e);
        }
        pen_.buttons = p.buttons;
        Event f(tool, kFrame);
        f.u(now());
        send(*c, f);
        taken = true;
      }
    }
  }
  wake();
  return taken;
}

}  // namespace wayland
