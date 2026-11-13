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

// The XInputExtension, version 2.2, as far as touches need it. A client (libXi) asks the version (GetExtensionVersion of XI 1, XIQueryVersion), lists the
// devices (XIQueryDevice: the master pointer 2 and keyboard 3, and the touchscreen 4, a slave of the pointer with a direct touch class of ten touches)
// and selects events on windows (XISelectEvents, read back with XIGetSelectedEvents). A finger the desktop hands the server (IWindowsSession::touch) is
// taken when the window under it, or the nearest ancestor, has a selection of XI_TouchBegin: that client gets TouchBegin, TouchUpdate as it moves and
// TouchEnd (also when the desktop cancels it), as GenericEvents (the XIDeviceEvent layout, no buttons or valuators), the event coordinates in that
// window's. A finger no window selects is refused and the desktop moves the pointer with it, as before; so core clients see the pointer as always.
// Only one client may select touch events of a device on a window (BadAccess, as the specification asks). Nothing else of XI2 is sent: no XI_Motion,
// XI_ButtonPress, raw events or hierarchy events, no device grabs, and XIAllowEvents (touch acceptance) does nothing (with no grabs, the owner is the one
// client the touch went to).
#include "X11/X11Internal.hpp"

namespace x11 {

namespace {

// the XI 2 event types of the first mask word, and devices
constexpr uint32_t kXiTouchBegin = 18, kXiTouchUpdate = 19, kXiTouchEnd = 20;
constexpr uint16_t kAllDevices = 0, kAllMasterDevices = 1, kMasterPointer = 2, kMasterKeyboard = 3, kTouchscreen = 4;
constexpr uint8_t kGenericEvent = 35;
constexpr uint16_t kVersionMajor = 2, kVersionMinor = 2;
constexpr uint32_t kMaxMasks = 64;

bool device_known(uint16_t d) { return d <= kTouchscreen; }
bool touch_device(uint16_t d) { return d == kAllDevices || d == kAllMasterDevices || d == kMasterPointer || d == kTouchscreen; }

}  // namespace

void Server::xinput_request(Client& c, uint8_t minor, WireReader& r) {
  const uint8_t major = kXInputOpcode;
  switch (minor) {
    case 1: {      // GetExtensionVersion (XI 1): what libXi asks first
      WireWriter o = reply(c, 1, 0);
      o.u16(kVersionMajor);
      o.u16(kVersionMinor);
      o.u8(1);      // present
      o.zeros(19);
      return;
    }
    case 47: {      // XIQueryVersion: the lower of the two
      uint16_t want_major = r.u16(), want_minor = r.u16();
      if (!r.ok()) { error(c, error::kLength, 0, major, minor); return; }
      if (want_major > kVersionMajor || (want_major == kVersionMajor && want_minor > kVersionMinor)) want_major = kVersionMajor, want_minor = kVersionMinor;
      if (want_major < 2) { error(c, error::kValue, want_major, major, minor); return; }
      c.xi_version = static_cast<uint16_t>(want_major << 8 | want_minor);
      WireWriter o = reply(c, 47, 0);
      o.u16(want_major);
      o.u16(want_minor);
      o.zeros(20);
      return;
    }
    case 48: {      // XIQueryDevice
      const uint16_t wanted = r.u16();
      if (!r.ok()) { error(c, error::kLength, 0, major, minor); return; }
      if (!device_known(wanted)) { error(c, error::kValue, wanted, major, minor); return; }      // (XI's BadDevice is an extension error: the core one)
      const uint32_t abs_x = intern("Abs MT Position X"), abs_y = intern("Abs MT Position Y"), rel_x = intern("Rel X"), rel_y = intern("Rel Y");
      const uint32_t left = intern("Button Left"), middle = intern("Button Middle"), right = intern("Button Right");
      ByteBuffer body(allocator_);
      WireWriter b(body, c.big);
      uint16_t count = 0;
      auto device = [&](uint16_t id, uint16_t use, uint16_t attachment, StringView name, uint16_t classes) {
        b.u16(id);
        b.u16(use);
        b.u16(attachment);
        b.u16(classes);
        b.u16(static_cast<uint16_t>(name.size()));
        b.u8(1);      // enabled
        b.u8(0);
        b.bytes(name.bytes());
        b.zeros(pad4(name.size()) - name.size());
        ++count;
      };
      auto buttons = [&](uint16_t source, uint16_t n) {      // ButtonClass: state mask (a word), labels
        b.u16(1);
        b.u16(static_cast<uint16_t>(2 + 1 + n));
        b.u16(source);
        b.u16(n);
        b.u32(0);
        const uint32_t labels[3] = {left, middle, right};
        for (uint16_t i = 0; i < n; ++i) b.u32(i < 3 ? labels[i] : 0);
      };
      auto valuator = [&](uint16_t source, uint16_t number, uint32_t label, int32_t max, bool absolute) {      // ValuatorClass (11 words)
        b.u16(2);
        b.u16(11);
        b.u16(source);
        b.u16(number);
        b.u32(label);
        b.i32(0); b.u32(0);                                 // min (FP3232)
        b.i32(absolute ? max : -1); b.u32(0);               // max
        b.i32(0); b.u32(0);                                 // value
        b.u32(absolute ? 1 : 0);                            // resolution
        b.u8(absolute ? 1 : 0);                             // mode: Absolute / Relative
        b.zeros(3);
      };
      const bool all = wanted == kAllDevices, masters = wanted == kAllMasterDevices;
      if (all || masters || wanted == kMasterPointer) {
        device(kMasterPointer, 1 /* XIMasterPointer */, kMasterKeyboard, "Virtual core pointer", 3);
        buttons(kMasterPointer, 3);
        valuator(kMasterPointer, 0, rel_x, 0, false);
        valuator(kMasterPointer, 1, rel_y, 0, false);
      }
      if (all || masters || wanted == kMasterKeyboard) {
        device(kMasterKeyboard, 2 /* XIMasterKeyboard */, kMasterPointer, "Virtual core keyboard", 1);
        b.u16(0);      // KeyClass: the keycodes 8 .. 255
        b.u16(static_cast<uint16_t>(2 + (kMaxKeycode - kMinKeycode + 1)));
        b.u16(kMasterKeyboard);
        b.u16(static_cast<uint16_t>(kMaxKeycode - kMinKeycode + 1));
        for (uint32_t k = kMinKeycode; k <= kMaxKeycode; ++k) b.u32(k);
      }
      if (all || wanted == kTouchscreen) {
        device(kTouchscreen, 3 /* XISlavePointer */, kMasterPointer, "TheoryCraftOS touchscreen", 4);
        buttons(kTouchscreen, 1);
        valuator(kTouchscreen, 0, abs_x, static_cast<int32_t>(screen_w_) - 1, true);
        valuator(kTouchscreen, 1, abs_y, static_cast<int32_t>(screen_h_) - 1, true);
        b.u16(8);      // TouchClass: direct, ten touches
        b.u16(2);
        b.u16(kTouchscreen);
        b.u8(1);
        b.u8(kMaxXiTouches < 10 ? kMaxXiTouches : 10);
      }
      if (!b.ok()) { error(c, error::kAlloc, 0, major, minor); return; }
      WireWriter o = reply(c, 48, static_cast<uint32_t>(body.size() / 4));
      o.u16(count);
      o.zeros(22);
      o.bytes(body.view());
      return;
    }
    case 46: {      // XISelectEvents
      const uint32_t window = r.u32();
      const uint16_t n = r.u16();
      r.skip(2);
      if (!r.ok()) { error(c, error::kLength, 0, major, minor); return; }
      Window* w = window_of(c, window, major);
      if (!w) return;
      if (n == 0 || n > kMaxMasks) { error(c, error::kValue, n, major, minor); return; }
      struct Mask { uint16_t device; uint32_t bits; };
      Mask masks[kMaxMasks];
      for (uint16_t i = 0; i < n; ++i) {
        const uint16_t device = r.u16(), words = r.u16();
        ConstByteView bytes = r.take(uint64_t{words} * 4);
        if (!r.ok()) { error(c, error::kLength, 0, major, minor); return; }
        if (!device_known(device)) { error(c, error::kValue, device, major, minor); return; }
        uint32_t bits = 0;
        for (uint32_t k = 0; k < 4 && k < bytes.size(); ++k) bits |= static_cast<uint32_t>(bytes[k]) << (8 * k);
        const uint32_t touch = (1u << kXiTouchBegin) | (1u << kXiTouchUpdate) | (1u << kXiTouchEnd);
        if ((bits & touch) && (bits & touch) != touch) { error(c, error::kValue, bits, major, minor); return; }      // (all three or none)
        if (bits & touch)      // one client per device and window may take its touches
          for (const XiSelection& s : w->xi.view())
            if (s.client != c.index && (s.mask & touch) && (s.device == device || s.device == kAllDevices || device == kAllDevices)) {
              error(c, error::kAccess, window, major, minor);
              return;
            }
        masks[i] = {device, bits};
      }
      for (uint16_t i = 0; i < n; ++i) {
        XiSelection* entry = nullptr;
        for (XiSelection& s : w->xi.view()) if (s.client == c.index && s.device == masks[i].device) entry = &s;
        if (!entry) {
          if (masks[i].bits == 0) continue;
          if (!w->xi.push_back(XiSelection{})) { error(c, error::kAlloc, 0, major, minor); return; }
          entry = &w->xi[w->xi.count() - 1];
          entry->client = c.index;
          entry->device = masks[i].device;
        }
        entry->mask = masks[i].bits;
      }
      for (uint64_t i = 0; i < w->xi.count();) {
        if (w->xi[i].mask == 0) w->xi.remove_at(i);
        else ++i;
      }
      return;
    }
    case 60: {      // XIGetSelectedEvents
      Window* w = window_of(c, r.u32(), major);
      if (!w) return;
      uint16_t n = 0;
      for (const XiSelection& s : w->xi.view()) n += s.client == c.index ? 1 : 0;
      WireWriter o = reply(c, 60, uint32_t{n} * 2);
      o.u16(n);
      o.zeros(22);
      for (const XiSelection& s : w->xi.view()) {
        if (s.client != c.index) continue;
        o.u16(s.device);
        o.u16(1);
        const uint8_t bytes[4] = {static_cast<uint8_t>(s.mask), static_cast<uint8_t>(s.mask >> 8), static_cast<uint8_t>(s.mask >> 16),
                                  static_cast<uint8_t>(s.mask >> 24)};
        o.bytes(ConstByteView(bytes, 4));
      }
      return;
    }
    case 45: {      // XIGetClientPointer
      r.u32();
      WireWriter o = reply(c, 45, 0);
      o.u8(1);
      o.u8(0);
      o.u16(kMasterPointer);
      o.zeros(20);
      return;
    }
    case 44: return;      // XISetClientPointer (there is one pointer)
    case 53: return;      // XIAllowEvents (no grabs: nothing waits)
    default: error(c, error::kRequest, 0, major, minor); return;
  }
}

void Server::xi_client_gone(uint8_t client) {
  for (Window& w : windows_)
    for (uint64_t i = 0; i < w.xi.count();) {
      if (w.xi[i].client == client) w.xi.remove_at(i);
      else ++i;
    }
  for (XiTouchSlot& t : xi_touches_) if (t.live && t.client == client) t.live = false;      // (its fingers are nobody's: their ends are dropped)
}

// An XIDeviceEvent (80 bytes: no buttons, no valuators) to the client that owns the touch.
void Server::xi_touch_event(uint16_t type, const XiTouchSlot& t, int32_t x, int32_t y) {
  if (t.client == 0 || t.client >= kMaxClients || !clients_[t.client] || !clients_[t.client]->setup_done) return;
  Client& c = *clients_[t.client];
  if (c.closing && c.out.size() > kMaxOutputBytes) return;
  Window* w = find_window(t.window);
  int32_t ox = 0, oy = 0;
  if (w) inside_origin(w, &ox, &oy);
  WireWriter o(c.out, c.big);
  o.u8(kGenericEvent);
  o.u8(kXInputOpcode);
  o.u16(c.sequence);
  o.u32(12);      // (80 - 32) / 4
  o.u16(type);
  o.u16(t.device == kTouchscreen ? kTouchscreen : kMasterPointer);
  o.u32(now());
  o.u32(t.touch_id);
  o.u32(kRootWindow);
  o.u32(t.window);
  o.u32(0);       // child
  o.i32(x * 65536);
  o.i32(y * 65536);
  o.i32((x - ox) * 65536);
  o.i32((y - oy) * 65536);
  o.u16(0);       // buttons_len
  o.u16(0);       // valuators_len
  o.u16(kTouchscreen);
  o.u16(0);
  o.u32(0);       // flags
  const uint32_t mods = key_state() & 0xFF;
  o.u32(mods); o.u32(0); o.u32(0); o.u32(mods);      // base, latched, locked, effective
  o.zeros(4);     // group
  if (!o.ok() || c.out.size() > kMaxOutputBytes) c.closing = true;
}

bool Server::touch(hardware::TouchPhase phase, uint32_t id, uint32_t window, int32_t x, int32_t y) {
  (void)window;
  bool taken = false;
  {
    kernel::ScopedMutex lock(mutex_);
    XiTouchSlot* slot = nullptr;
    for (XiTouchSlot& t : xi_touches_) if (t.live && t.desktop_id == id) slot = &t;
    if (phase == hardware::TouchPhase::Down) {
      if (slot) return false;
      // the nearest window under the finger that selected touches, and the client that did
      const uint32_t touch = 1u << kXiTouchBegin;
      Window* owner = nullptr;
      const XiSelection* sel = nullptr;
      for (Window* w = window_at(x, y); w && !owner; w = w->parent)
        for (const XiSelection& s : w->xi.view())
          if ((s.mask & touch) && touch_device(s.device) && s.client < kMaxClients && clients_[s.client]) {
            owner = w;
            sel = &s;
            break;
          }
      if (!owner) return false;
      for (XiTouchSlot& t : xi_touches_) if (!t.live) { slot = &t; break; }
      if (!slot) return false;      // (more fingers than touches: the desktop's pointer)
      slot->live = true;
      slot->desktop_id = id;
      slot->touch_id = next_xi_touch_++;
      if (next_xi_touch_ == 0) next_xi_touch_ = 1;
      slot->window = owner->id;
      slot->client = sel->client;
      slot->device = sel->device;
      xi_touch_event(kXiTouchBegin, *slot, x, y);
      taken = true;
    } else if (slot) {
      const bool end = phase == hardware::TouchPhase::Up || phase == hardware::TouchPhase::Cancel;
      xi_touch_event(end ? kXiTouchEnd : kXiTouchUpdate, *slot, x, y);
      if (end) slot->live = false;
      taken = true;
    }
  }
  wake();
  return taken;
}

}  // namespace x11
