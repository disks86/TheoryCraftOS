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

// The kernel's X server (src/X11): connection setup in both byte orders, windows, properties, drawing, the window manager's view of the top-level
// windows (desktop::IWindowsSession), input, errors, truncated and bit-flipped request streams, and the AF_UNIX transport with several client threads
// and a desktop thread at once. Run: tests/run_host_tests.sh X11ServerTest

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Process/UnixSockets.hpp"
#include "X11/X11Server.hpp"
#include "X11/X11SocketServer.hpp"

using x11::Server;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
static TestAllocator A;

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// ------------------------------------------------------------------ a raw X client

struct Request {
  bool big;
  std::vector<uint8_t> b;
  explicit Request(bool big_endian, uint8_t opcode, uint8_t data = 0) : big(big_endian) { b = {opcode, data, 0, 0}; }
  Request& u8(uint8_t v) { b.push_back(v); return *this; }
  Request& u16(uint16_t v) { if (big) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); } else { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); } return *this; }
  Request& u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (big ? 24 - 8 * i : 8 * i))); return *this; }
  Request& str(const std::string& s) { for (char c : s) b.push_back(uint8_t(c)); return *this; }
  Request& pad() { while (b.size() % 4) b.push_back(0); return *this; }
  std::vector<uint8_t> done() {
    pad();
    const uint16_t words = uint16_t(b.size() / 4);
    if (big) { b[2] = uint8_t(words >> 8); b[3] = uint8_t(words); } else { b[2] = uint8_t(words); b[3] = uint8_t(words >> 8); }
    return b;
  }
};

struct Packet {
  std::vector<uint8_t> b;
  bool big = false;
  uint8_t code() const { return b[0] & 0x7F; }
  uint16_t u16(size_t at) const { return big ? uint16_t(b[at] << 8 | b[at + 1]) : uint16_t(b[at] | b[at + 1] << 8); }
  uint32_t u32(size_t at) const {
    return big ? uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3]
               : uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
  }
  int16_t i16(size_t at) const { return int16_t(u16(at)); }
};

struct Client {
  Server& s;
  bool big;
  uint32_t id = 0;
  uint32_t base = 0;
  uint32_t next = 1;
  uint16_t sequence = 0;
  std::vector<uint8_t> out;      // what the server sent, not parsed yet
  bool closing = false;
  Client(Server& server, bool big_endian) : s(server), big(big_endian) { id = s.open_client(0); }
  ~Client() { if (id) s.close_client(id); }
  uint32_t new_id() { return base | next++; }
  void send(const std::vector<uint8_t>& bytes) {
    s.receive(id, ConstByteView(bytes.data(), bytes.size()));
    // count the requests in what was sent (one call = whole requests)
    size_t at = 0;
    while (at + 4 <= bytes.size()) {
      size_t words = big ? (bytes[at + 2] << 8 | bytes[at + 3]) : (bytes[at + 2] | bytes[at + 3] << 8);
      if (words == 0) {
        if (at + 8 > bytes.size()) { ++sequence; break; }
        words = big ? (size_t(bytes[at + 4]) << 24 | size_t(bytes[at + 5]) << 16 | size_t(bytes[at + 6]) << 8 | bytes[at + 7])
                    : (size_t(bytes[at + 4]) | size_t(bytes[at + 5]) << 8 | size_t(bytes[at + 6]) << 16 | size_t(bytes[at + 7]) << 24);
      }
      ++sequence;
      if (words == 0) break;
      at += words * 4;
    }
    pull();
  }
  void pull() {
    ByteBuffer buf(A);
    closing = s.take_output(id, &buf) || closing;
    out.insert(out.end(), buf.view().data(), buf.view().data() + buf.size());
  }
  bool setup() {
    std::vector<uint8_t> b = {uint8_t(big ? 'B' : 'l'), 0};
    Request r(big, 0);
    r.b.clear();
    r.u8(big ? 'B' : 'l').u8(0).u16(11).u16(0).u16(4).u16(0).u16(0).str("MIT-").pad();
    s.receive(id, ConstByteView(r.b.data(), r.b.size()));
    pull();
    if (out.size() < 8 || out[0] != 1) return false;
    Packet p{std::vector<uint8_t>(out.begin(), out.begin() + 8), big};
    const size_t total = 8 + size_t(p.u16(6)) * 4;
    if (out.size() < total) return false;
    Packet whole{std::vector<uint8_t>(out.begin(), out.begin() + total), big};
    base = whole.u32(12);
    out.erase(out.begin(), out.begin() + total);
    return true;
  }
  // The next packet (reply, event or error), or an empty one.
  Packet next_packet() {
    pull();
    Packet p;
    p.big = big;
    if (out.size() < 32) return p;
    size_t size = 32;
    if (out[0] == 1) {
      Packet h{std::vector<uint8_t>(out.begin(), out.begin() + 8), big};
      size += size_t(h.u32(4)) * 4;
    }
    if (out.size() < size) return p;
    p.b.assign(out.begin(), out.begin() + size);
    out.erase(out.begin(), out.begin() + size);
    return p;
  }
  // Skips events until a reply or error comes.
  Packet reply() {
    for (;;) {
      Packet p = next_packet();
      if (p.b.empty() || p.b[0] == 0 || p.b[0] == 1) return p;
    }
  }
  // The first event of this type among the waiting packets (the others are dropped), or empty.
  Packet event(uint8_t code) {
    for (;;) {
      Packet p = next_packet();
      if (p.b.empty() || ((p.b[0] & 0x7F) == code)) return p;
    }
  }
  void drain() { while (!next_packet().b.empty()) {} }

  uint32_t intern(const std::string& name, bool only_if_exists = false) {
    send(Request(big, 16, only_if_exists).u16(uint16_t(name.size())).u16(0).str(name).done());
    Packet p = reply();
    return p.b.size() >= 12 && p.b[0] == 1 ? p.u32(8) : 0;
  }
  uint32_t create_window(uint32_t parent, int16_t x, int16_t y, uint16_t w, uint16_t h, uint32_t event_mask, uint32_t background = 0xFFFFFFu, uint16_t border = 0) {
    const uint32_t wid = new_id();
    send(Request(big, 1, 0).u32(wid).u32(parent).u16(uint16_t(x)).u16(uint16_t(y)).u16(w).u16(h).u16(border).u16(1).u32(0).u32(0x2 | 0x800)
             .u32(background).u32(event_mask).done());
    return wid;
  }
  void change_property(uint32_t window, uint32_t property, uint32_t type, uint8_t format, const std::vector<uint32_t>& units, uint8_t mode = 0) {
    Request r(big, 18, mode);
    r.u32(window).u32(property).u32(type).u8(format).u8(0).u8(0).u8(0).u32(uint32_t(units.size()));
    for (uint32_t u : units) {
      if (format == 8) r.u8(uint8_t(u));
      else if (format == 16) r.u16(uint16_t(u));
      else r.u32(u);
    }
    send(r.done());
  }
  void change_string(uint32_t window, uint32_t property, uint32_t type, const std::string& text) {
    std::vector<uint32_t> units(text.begin(), text.end());
    for (uint32_t& u : units) u &= 0xFF;
    change_property(window, property, type, 8, units);
  }
  uint32_t create_gc(uint32_t drawable, uint32_t foreground, uint32_t function = 3) {
    const uint32_t gc = new_id();
    send(Request(big, 55).u32(gc).u32(drawable).u32(0x1 | 0x4 | 0x10000).u32(function).u32(foreground).u32(1).done());
    return gc;
  }
  void fill_rect(uint32_t drawable, uint32_t gc, int16_t x, int16_t y, uint16_t w, uint16_t h) {
    send(Request(big, 70).u32(drawable).u32(gc).u16(uint16_t(x)).u16(uint16_t(y)).u16(w).u16(h).done());
  }
  void sync() {      // GetInputFocus: its reply comes after everything before it
    send(Request(big, 43).done());
  }
};

constexpr uint32_t kRoot = Server::kRootWindow;
constexpr uint32_t kExposure = 1u << 15, kStructure = 1u << 17, kKeyPress = 1, kButtonPress = 4, kButtonRelease = 8, kFocus = 1u << 21,
                   kPointerMotion = 1u << 6, kProperty = 1u << 22, kEnter = 1u << 4, kLeave = 1u << 5;

// ------------------------------------------------------------------ tests

static void test_setup() {
  for (bool big : {false, true}) {
    Server s(A);
    Client c(s, big);
    CHECK(c.id != 0);
    CHECK(c.setup());
    CHECK(c.base != 0 && (c.base & 0x1FFFFF) == 0);
    // a request right after: replies come in the client's order
    c.sync();
    Packet p = c.reply();
    CHECK(p.b.size() == 32 && p.b[0] == 1 && p.u16(2) == 1);
  }
  {      // not an X client
    Server s(A);
    Client c(s, false);
    const uint8_t junk[12] = {'G', 'E', 'T', ' ', '/', ' ', 'H', 'T', 'T', 'P', '/', '1'};
    s.receive(c.id, ConstByteView(junk, sizeof junk));
    c.pull();
    CHECK(c.closing && c.out.empty());
  }
  {      // another protocol version: a failure reply, then closed
    Server s(A);
    Client c(s, true);
    Request r(true, 0);
    r.b.clear();
    r.u8('B').u8(0).u16(10).u16(0).u16(0).u16(0).u16(0);
    s.receive(c.id, ConstByteView(r.b.data(), r.b.size()));
    c.pull();
    CHECK(c.closing && c.out.size() >= 8 && c.out[0] == 0);
  }
  {      // the setup in pieces of one byte
    Server s(A);
    Client c(s, false);
    Request r(false, 0);
    r.b.clear();
    r.u8('l').u8(0).u16(11).u16(0).u16(18).u16(16).u16(0).str("MIT-MAGIC-COOKIE-1").pad().str("0123456789abcdef");
    for (size_t i = 0; i < r.b.size(); ++i) {
      s.receive(c.id, ConstByteView(&r.b[i], 1));
      c.pull();
      if (i + 1 < r.b.size()) CHECK(c.out.empty());
    }
    CHECK(c.out.size() > 8 && c.out[0] == 1);
  }
}

static void test_windows_and_properties() {
  Server s(A);
  s.attach(6, 19);
  const Server::Monitor monitors[1] = {{{0, 0, 1280, 800}, {0, 0, 1280, 760}}};
  s.set_display(1280, 800, ArrayView<const Server::Monitor>(monitors, 1));
  Client c(s, false);
  CHECK(c.setup());
  const uint32_t w = c.create_window(kRoot, 100, 50, 300, 200, kExposure | kStructure | kFocus | kProperty);
  c.change_string(w, 39 /* WM_NAME */, 31 /* STRING */, "Hello \xE9t\xE9");      // Latin-1
  // WM_NORMAL_HINTS with PPosition: the frame goes where it was asked
  c.change_property(w, 40, 41, 32, {4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  c.drain();
  List<uint32_t> listed(A);
  s.windows(&listed);
  CHECK(listed.count() == 0);      // (not mapped yet)
  c.send(Request(false, 8).u32(w).done());
  CHECK(!c.event(19 /* MapNotify */).b.empty());
  c.drain();
  s.windows(&listed);
  CHECK(listed.count() == 1 && listed[0] == w);
  desktop::WindowsWindowInfo info;
  String title(A);
  CHECK(s.window_info(w, &info, &title));
  CHECK(info.visible && info.caption && info.active && !info.popup);
  CHECK(title.view().equals("Hello \xC3\xA9t\xC3\xA9"));
  // the requested position is the frame's; the inside is inset by the frame
  CHECK(info.window.x == 100 && info.window.y == 50);
  CHECK(info.surface.x == 106 && info.surface.y == 75 && info.surface.width == 300 && info.surface.height == 200);
  CHECK(info.window.width == 312 && info.window.height == 231);
  // _NET_WM_NAME wins (UTF-8)
  const uint32_t net_name = c.intern("_NET_WM_NAME"), utf8 = c.intern("UTF8_STRING");
  c.change_string(w, net_name, utf8, "\xE2\x9C\x93 Done");
  CHECK(s.window_info(w, &info, &title) && title.view().equals("\xE2\x9C\x93 Done"));
  List<uint32_t> changed(A);
  List<desktop::IWindowsSession::Repaint> repainted(A);
  s.take_changes(&changed, &repainted);
  bool saw = false;
  for (uint32_t id : changed.view()) saw |= id == w;
  CHECK(saw);
  // GetProperty: a part, then the rest with delete
  const uint32_t prop = c.intern("TEST_PROP");
  c.change_property(w, prop, 6 /* CARDINAL */, 32, {1, 2, 3, 4, 5});
  c.change_property(w, prop, 6, 32, {6}, 2 /* append */);
  c.drain();
  c.send(Request(false, 20, 0).u32(w).u32(prop).u32(0).u32(1).u32(2).done());
  Packet p = c.reply();
  CHECK(p.b.size() == 32 + 8 && p.b[1] == 32 && p.u32(8) == 6 && p.u32(12) == 12 && p.u32(16) == 2 && p.u32(32) == 2 && p.u32(36) == 3);
  c.send(Request(false, 20, 1).u32(w).u32(prop).u32(6).u32(3).u32(100).done());
  p = c.reply();
  CHECK(p.b.size() == 32 + 12 && p.u32(12) == 0 && p.u32(16) == 3 && p.u32(32) == 4 && p.u32(40) == 6);
  CHECK(!c.event(28 /* PropertyNotify deleted */).b.empty());
  c.send(Request(false, 20, 0).u32(w).u32(prop).u32(0).u32(0).u32(1).done());
  p = c.reply();
  CHECK(p.b.size() == 32 && p.u32(8) == 0);      // gone
  // a mismatched append is BadMatch
  c.change_property(w, prop, 6, 32, {1});
  c.change_property(w, prop, 6, 8, {1}, 2);
  p = c.reply();
  CHECK(p.b.size() == 32 && p.b[0] == 0 && p.b[1] == 8);
  // GetAtomName / InternAtom only-if-exists
  CHECK(c.intern("NO_SUCH_ATOM_YET", true) == 0);
  c.send(Request(false, 17).u32(prop).done());
  p = c.reply();
  CHECK(p.b.size() == 32 + 12 && p.u16(8) == 9 && std::string(p.b.begin() + 32, p.b.begin() + 41) == "TEST_PROP");
  // QueryTree and GetGeometry
  const uint32_t child = c.create_window(w, 10, 10, 50, 40, kExposure);
  c.send(Request(false, 15).u32(w).done());
  p = c.reply();
  CHECK(p.b.size() == 36 && p.u32(8) == kRoot && p.u32(12) == kRoot && p.u16(16) == 1 && p.u32(32) == child);
  c.send(Request(false, 14).u32(child).done());
  p = c.reply();
  CHECK(p.b[1] == 24 && p.i16(12) == 10 && p.u16(16) == 50 && p.u16(18) == 40);
  // the desktop moves and sizes the frame: the client hears ConfigureNotify with the inside's new place
  c.drain();
  s.move_window(w, {200, 100, 412, 331});
  p = c.event(22);
  CHECK(!p.b.empty() && p.i16(16) == 206 && p.i16(18) == 125 && p.u16(20) == 400 && p.u16(22) == 300);
  // maximize: the work area
  s.window_command(w, desktop::IWindowsSession::Command::Maximize);
  CHECK(s.window_info(w, &info, &title) && info.maximized && info.window.x == 0 && info.window.y == 0 && info.window.width == 1280 && info.window.height == 760);
  s.window_command(w, desktop::IWindowsSession::Command::Restore);
  CHECK(s.window_info(w, &info, &title) && !info.maximized && info.surface.x == 206 && info.surface.width == 400);
  s.window_command(w, desktop::IWindowsSession::Command::Minimize);
  CHECK(s.window_info(w, &info, &title) && info.minimized);
  // destroy: gone for the desktop too
  c.send(Request(false, 4).u32(w).done());
  CHECK(!s.window_info(w, &info, &title));
  s.windows(&listed);
  CHECK(listed.count() == 0);
}

static void test_window_manager_hints() {
  Server s(A);
  s.attach(6, 19);
  Client c(s, true);
  CHECK(c.setup());
  // override-redirect: a popup with no frame
  const uint32_t menu = c.new_id();
  c.send(Request(true, 1, 0).u32(menu).u32(kRoot).u16(5).u16(5).u16(80).u16(60).u16(0).u16(1).u32(0).u32(0x200).u32(1).done());
  c.send(Request(true, 8).u32(menu).done());
  desktop::WindowsWindowInfo info;
  String title(A);
  CHECK(s.window_info(menu, &info, &title) && info.popup && !info.caption && info.window.x == 5 && info.surface.width == 80);
  // _MOTIF_WM_HINTS without decorations, a transient (owned) dialog, fixed size
  const uint32_t main = c.create_window(kRoot, 0, 0, 200, 100, kStructure);
  const uint32_t dialog = c.create_window(kRoot, 0, 0, 120, 80, kStructure);
  const uint32_t motif = c.intern("_MOTIF_WM_HINTS");
  c.change_property(main, motif, motif, 32, {2, 0, 0, 0, 0});
  c.change_property(dialog, 68 /* WM_TRANSIENT_FOR */, 33, 32, {main});
  c.change_property(dialog, 40, 41, 32, {16 | 32, 0, 0, 0, 0, 120, 80, 120, 80, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  c.send(Request(true, 8).u32(main).done());
  c.send(Request(true, 8).u32(dialog).done());
  CHECK(s.window_info(main, &info, &title) && !info.caption && !info.popup && info.sizable);
  CHECK(s.window_info(dialog, &info, &title) && info.caption && info.owned && !info.sizable && !info.maximize_box);
  // placement: an unpositioned window is cascaded inside the work area, its frame fully on screen
  CHECK(info.window.x >= 0 && info.window.y >= 0);
  // _NET_WM_MOVERESIZE: the desktop is asked to drag the frame
  const uint32_t moveresize = c.intern("_NET_WM_MOVERESIZE");
  Request ev(true, 25, 0);
  ev.u32(kRoot).u32((1u << 19) | (1u << 20));
  ev.u8(33).u8(32).u16(0).u32(dialog).u32(moveresize).u32(10).u32(10).u32(8 /* move */).u32(1).u32(0);
  c.send(ev.done());
  uint32_t edge = 0;
  CHECK(s.take_drag(&edge) == dialog && edge == 9);
  // _GTK_SHOW_WINDOW_MENU (GTK's own title bar): the desktop opens the window menu there, in the window's coordinates; it is in _NET_SUPPORTED
  const uint32_t show_menu = c.intern("_GTK_SHOW_WINDOW_MENU");
  {
    c.drain();
    c.send(Request(true, 20, 0).u32(kRoot).u32(c.intern("_NET_SUPPORTED")).u32(0).u32(0).u32(1000).done());
    Packet sp = c.reply();
    bool listed = false;
    for (uint32_t i = 0; sp.b.size() >= 32 && i < sp.u32(16) && 32 + 4 * i + 4 <= sp.b.size(); ++i) listed |= sp.u32(32 + 4 * i) == show_menu;
    CHECK(listed);
  }
  {
    desktop::WindowsWindowInfo di;
    String dt(A);
    CHECK(s.window_info(dialog, &di, &dt));
    int32_t mx = 0, my = 0;
    CHECK(s.take_window_menu(&mx, &my) == 0);
    Request m(true, 25, 0);
    m.u32(kRoot).u32((1u << 19) | (1u << 20));
    m.u8(33).u8(32).u16(0).u32(dialog).u32(show_menu).u32(2).u32(uint32_t(di.surface.x + 25)).u32(uint32_t(di.surface.y + 9)).u32(0).u32(0);
    c.send(m.done());
    CHECK(s.take_window_menu(&mx, &my) == dialog && mx == 25 && my == 9);
    if (mx != 25 || my != 9) printf("menu at %d,%d\n", mx, my);
  }
  // _NET_WM_STATE add maximized (EWMH client message)
  const uint32_t state = c.intern("_NET_WM_STATE"), vert = c.intern("_NET_WM_STATE_MAXIMIZED_VERT"), horz = c.intern("_NET_WM_STATE_MAXIMIZED_HORZ");
  Request st(true, 25, 0);
  st.u32(kRoot).u32((1u << 19) | (1u << 20)).u8(33).u8(32).u16(0).u32(main).u32(state).u32(1).u32(vert).u32(horz).u32(1).u32(0);
  c.send(st.done());
  CHECK(s.window_info(main, &info, &title) && info.maximized);
  // selecting SubstructureRedirect on the root: the window manager is the server's
  c.drain();
  c.send(Request(true, 2).u32(kRoot).u32(0x800).u32(1u << 20).done());
  Packet p = c.reply();
  CHECK(p.b.size() == 32 && p.b[0] == 0 && p.b[1] == 10);
}

// Docks and struts: a dock goes in the top band with no taskbar button and keeps the space its strut asks for (counted from the edge of the desktop's panel
// area, so what the taskbar already covers is not kept twice); a partial strut whose range misses the monitor keeps nothing; a desktop window goes below
// everything.
static void test_struts() {
  Server s(A);
  s.attach(6, 19);
  // two monitors side by side; the first has a 40-pixel taskbar along its bottom
  const Server::Monitor monitors[2] = {{{0, 0, 1280, 800}, {0, 0, 1280, 760}, {0, 0, 1280, 760}}, {{1280, 0, 1024, 768}, {1280, 0, 1024, 768}, {}}};
  s.set_display(2304, 800, ArrayView<const Server::Monitor>(monitors, 2));
  Client c(s, true);
  CHECK(c.setup());
  const uint32_t type = c.intern("_NET_WM_WINDOW_TYPE"), dock = c.intern("_NET_WM_WINDOW_TYPE_DOCK"), backdrop = c.intern("_NET_WM_WINDOW_TYPE_DESKTOP");
  const uint32_t strut = c.intern("_NET_WM_STRUT"), partial = c.intern("_NET_WM_STRUT_PARTIAL");
  desktop::WindowsWindowInfo info;
  String title(A);
  // a top panel on the first monitor
  const uint32_t top = c.create_window(kRoot, 0, 0, 1280, 30, kStructure);
  c.change_property(top, type, 4 /* ATOM */, 32, {dock});
  c.change_property(top, partial, 6 /* CARDINAL */, 32, {0, 0, 30, 0, 0, 0, 0, 0, 0, 1279, 0, 0});
  c.send(Request(true, 8).u32(top).done());
  CHECK(s.window_info(top, &info, &title));
  CHECK(info.layer == 1 && info.tool && !info.caption && info.reserve_edge == 2 && info.reserve == 30);
  CHECK(info.window.x == 0 && info.window.y == 0 && info.window.width == 1280);
  // a bottom dock 50 high along the taskbar's edge: only the 10 pixels past the taskbar are kept
  const uint32_t bottom = c.create_window(kRoot, 0, 750, 1280, 50, kStructure);
  c.change_property(bottom, type, 4, 32, {dock});
  c.change_property(bottom, strut, 6, 32, {0, 0, 0, 50});
  c.send(Request(true, 8).u32(bottom).done());
  CHECK(s.window_info(bottom, &info, &title) && info.reserve_edge == 4 && info.reserve == 10);
  // the strut changes: the desktop hears about it
  List<uint32_t> changed(A);
  List<desktop::IWindowsSession::Repaint> repainted(A);
  s.take_changes(&changed, &repainted);
  c.change_property(bottom, strut, 6, 32, {0, 0, 0, 0});
  c.drain();
  s.take_changes(&changed, &repainted);
  bool saw = false;
  for (uint32_t id : changed.view()) saw |= id == bottom;
  CHECK(saw);
  CHECK(s.window_info(bottom, &info, &title) && info.reserve_edge == 0 && info.reserve == 0);
  // a left panel on the second monitor: its left edge is 1280 pixels from the screen's, so a strut of 1280 + 48 keeps 48
  const uint32_t left = c.create_window(kRoot, 1280, 0, 48, 768, kStructure);
  c.change_property(left, type, 4, 32, {dock});
  c.change_property(left, partial, 6, 32, {1328, 0, 0, 0, 0, 767, 0, 0, 0, 0, 0, 0});
  c.send(Request(true, 8).u32(left).done());
  CHECK(s.window_info(left, &info, &title) && info.reserve_edge == 1 && info.reserve == 48);
  // a partial strut whose range is off the monitor keeps nothing
  c.change_property(left, partial, 6, 32, {1328, 0, 0, 0, 900, 1000, 0, 0, 0, 0, 0, 0});
  CHECK(s.window_info(left, &info, &title) && info.reserve_edge == 0);
  // a short or wrongly typed strut is ignored, as is a huge one
  c.change_property(left, partial, 6, 32, {1328, 0});
  c.change_property(left, strut, 6, 32, {0xFFFFFFFFu, 0, 0, 0});
  CHECK(s.window_info(left, &info, &title) && info.reserve_edge == 0);
  c.change_property(left, strut, 6, 8, {48, 0, 0, 0});
  CHECK(s.window_info(left, &info, &title) && info.reserve_edge == 0);
  // a desktop window: the background band
  const uint32_t desk = c.create_window(kRoot, 0, 0, 1280, 800, kStructure);
  c.change_property(desk, type, 4, 32, {backdrop});
  c.send(Request(true, 8).u32(desk).done());
  CHECK(s.window_info(desk, &info, &title) && info.layer == -2 && info.tool && !info.caption && info.reserve_edge == 0);
  // an ordinary window stays in the normal band, keeping nothing
  const uint32_t plain = c.create_window(kRoot, 10, 10, 100, 100, kStructure);
  c.send(Request(true, 8).u32(plain).done());
  CHECK(s.window_info(plain, &info, &title) && info.layer == 0 && !info.tool && info.reserve_edge == 0);
}

static void test_drawing() {
  Server s(A);
  Client c(s, false);
  CHECK(c.setup());
  const uint32_t w = c.create_window(kRoot, 0, 0, 64, 48, kExposure, 0x000000);
  c.send(Request(false, 8).u32(w).done());
  CHECK(s.picture_pixel(w, 5, 5) == 0xFF000000u);      // the background
  const uint32_t gc = c.create_gc(w, 0xFF0000);
  c.fill_rect(w, gc, 10, 10, 20, 10);
  CHECK(s.picture_pixel(w, 10, 10) == 0xFFFF0000u && s.picture_pixel(w, 29, 19) == 0xFFFF0000u && s.picture_pixel(w, 30, 10) == 0xFF000000u);
  // a child with its own background clips its parent's drawing (ClipByChildren)
  const uint32_t child = c.create_window(w, 40, 0, 10, 10, kExposure, 0x0000FF);
  c.send(Request(false, 8).u32(child).done());
  CHECK(s.picture_pixel(w, 45, 5) == 0xFF0000FFu);
  c.fill_rect(w, gc, 35, 0, 20, 20);
  CHECK(s.picture_pixel(w, 45, 5) == 0xFF0000FFu && s.picture_pixel(w, 38, 5) == 0xFFFF0000u);
  // drawing into the child lands at its place in the top-level's picture, clipped to it
  const uint32_t gc2 = c.create_gc(child, 0x00FF00);
  c.fill_rect(child, gc2, -5, -5, 100, 100);
  CHECK(s.picture_pixel(w, 40, 0) == 0xFF00FF00u && s.picture_pixel(w, 49, 9) == 0xFF00FF00u && s.picture_pixel(w, 50, 5) == 0xFFFF0000u);
  // unmapping the child gives its area back to the parent's background, with an Expose
  c.drain();
  c.send(Request(false, 10).u32(child).done());
  CHECK(s.picture_pixel(w, 45, 5) == 0xFF000000u);
  CHECK(!c.event(12).b.empty());
  // XOR twice is nothing
  const uint32_t gx = c.create_gc(w, 0xFFFFFF, 6);
  c.fill_rect(w, gx, 0, 0, 8, 8);
  CHECK(s.picture_pixel(w, 1, 1) == 0xFFFFFFFFu);
  c.fill_rect(w, gx, 0, 0, 8, 8);
  CHECK(s.picture_pixel(w, 1, 1) == 0xFF000000u);
  // PutImage (ZPixmap 32 bpp, little-endian) and GetImage give the same pixels back
  Request put(false, 72, 2);
  put.u32(w).u32(gc).u16(2).u16(2).u16(3).u16(4).u8(0).u8(24).u16(0);
  for (uint32_t v : {0x112233u, 0x445566u, 0x778899u, 0xAABBCCu}) put.u8(uint8_t(v)).u8(uint8_t(v >> 8)).u8(uint8_t(v >> 16)).u8(0);
  c.send(put.done());
  CHECK(s.picture_pixel(w, 3, 4) == 0xFF112233u && s.picture_pixel(w, 4, 5) == 0xFFAABBCCu);
  c.drain();
  c.send(Request(false, 73, 2).u32(w).u16(3).u16(4).u16(2).u16(2).u32(0xFFFFFFFF).done());
  Packet p = c.reply();
  CHECK(p.b.size() == 32 + 16 && p.b[1] == 24 && p.u32(32) == 0x112233u && p.u32(44) == 0xAABBCCu);
  // a bitmap (XYBitmap) with foreground / background
  Request bm(false, 72, 0);
  bm.u32(w).u32(gc).u16(3).u16(1).u16(20).u16(30).u8(0).u8(1).u16(0).u8(0x5).u8(0).u8(0).u8(0);      // bits 1 0 1
  c.send(bm.done());
  CHECK(s.picture_pixel(w, 20, 30) == 0xFFFF0000u && s.picture_pixel(w, 21, 30) == 0xFF000001u /* the GC's default background, 1 */ && s.picture_pixel(w, 22, 30) == 0xFFFF0000u);
  // a pixmap, drawn into and copied to the window
  const uint32_t pm = c.new_id();
  c.send(Request(false, 53, 24).u32(pm).u32(w).u16(4).u16(4).done());
  const uint32_t pgc = c.create_gc(pm, 0x00FFFF);
  c.fill_rect(pm, pgc, 0, 0, 4, 4);
  c.drain();
  c.send(Request(false, 62).u32(pm).u32(w).u32(gc).u16(0).u16(0).u16(50).u16(40).u16(4).u16(4).done());
  CHECK(s.picture_pixel(w, 50, 40) == 0xFF00FFFFu && s.picture_pixel(w, 53, 43) == 0xFF00FFFFu);
  CHECK(!c.event(14 /* NoExpose */).b.empty());
  // lines, arcs, polygons, text: they draw something and stay inside
  c.send(Request(false, 65, 0).u32(w).u32(gc).u16(0).u16(47).u16(63).u16(0).done());
  CHECK(s.picture_pixel(w, 0, 47) == 0xFFFF0000u && s.picture_pixel(w, 63, 0) == 0xFFFF0000u);
  c.send(Request(false, 71).u32(w).u32(gc).u16(30).u16(20).u16(10).u16(10).u16(0).u16(360 * 64).done());
  CHECK(s.picture_pixel(w, 35, 25) == 0xFFFF0000u);
  c.send(Request(false, 69).u32(w).u32(gc).u8(0).u8(0).u16(0).u16(0).u16(40).u16(8).u16(40).u16(4).u16(44).done());
  CHECK(s.picture_pixel(w, 2, 41) == 0xFFFF0000u);
  const uint32_t font = c.new_id();
  c.send(Request(false, 45).u32(font).u16(5).u16(0).str("fixed").done());
  c.send(Request(false, 56).u32(gc).u32(0x4000).u32(font).done());
  c.send(Request(false, 76, 1).u32(w).u32(gc).u16(0).u16(12).str("A").done());      // ImageText8 at the top left
  bool lit = false;
  for (int y = 0; y < 16; ++y) for (int x = 0; x < 8; ++x) lit |= s.picture_pixel(w, x, y) == 0xFFFF0000u;
  CHECK(lit);
  // SetClipRectangles keeps a fill inside them
  c.send(Request(false, 59).u32(gc).u16(0).u16(0).u16(60).u16(0).u16(2).u16(2).done());
  c.fill_rect(w, gc, 0, 0, 64, 48);
  CHECK(s.picture_pixel(w, 60, 0) == 0xFFFF0000u && s.picture_pixel(w, 62, 0) != 0xFFFF0000u);
  // ClearArea: the background again, with an Expose when asked
  c.drain();
  c.send(Request(false, 61, 1).u32(w).u16(0).u16(0).u16(0).u16(0).done());
  CHECK(s.picture_pixel(w, 10, 10) == 0xFF000000u);
  CHECK(!c.event(12).b.empty());
  List<uint32_t> changed(A);
  List<desktop::IWindowsSession::Repaint> repainted(A);
  s.take_changes(&changed, &repainted);
  CHECK(repainted.count() >= 1 && repainted[0].window == w);
}

static void test_input() {
  Server s(A);
  s.attach(6, 19);
  const Server::Monitor monitors[1] = {{{0, 0, 800, 600}, {0, 0, 800, 600}}};
  s.set_display(800, 600, ArrayView<const Server::Monitor>(monitors, 1));
  Client c(s, false);
  CHECK(c.setup());
  const uint32_t w = c.create_window(kRoot, 100, 100, 200, 100, kKeyPress | kButtonPress | kButtonRelease | kFocus | kPointerMotion | kEnter | kLeave);
  c.change_property(w, 40, 41, 32, {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});      // USPosition
  c.send(Request(false, 8).u32(w).done());
  c.drain();
  desktop::WindowsWindowInfo info;
  String title(A);
  CHECK(s.window_info(w, &info, &title) && info.surface.x == 106 && info.surface.y == 125);
  // the pointer comes in: Enter, then motion with coordinates inside the window
  s.mouse_position(116, 135);
  Packet p = c.event(7 /* EnterNotify */);
  CHECK(!p.b.empty() && p.u32(12) == w && p.i16(24) == 10 && p.i16(26) == 10);
  p = c.event(6);
  CHECK(!p.b.empty() && p.i16(20) == 116 && p.i16(24) == 10);
  // a press: the left button is X's 1; the right one 3
  s.mouse_button(1, true);
  p = c.event(4);
  CHECK(!p.b.empty() && p.b[1] == 1 && p.u32(12) == w && p.u16(28) == 0);
  // the implicit grab: motion outside the window still goes to it, with Button1 in the state
  s.mouse_position(10, 10);
  p = c.event(6);
  CHECK(!p.b.empty() && p.u32(12) == w && p.i16(24) == 10 - 106 && (p.u16(28) & 0x100));
  CHECK(s.capture_window() == w);
  s.mouse_button(1, false);
  p = c.event(5);
  CHECK(!p.b.empty() && p.b[1] == 1 && (p.u16(28) & 0x100));
  CHECK(s.capture_window() == 0);
  s.mouse_position(150, 150);
  s.mouse_button(2, true);
  p = c.event(4);
  CHECK(!p.b.empty() && p.b[1] == 3);
  s.mouse_button(2, false);
  s.mouse_wheel(1);
  p = c.event(4);
  CHECK(!p.b.empty() && p.b[1] == 4);
  // keys go to the focus (the window got it when it was mapped); keycode = HID usage + 8
  c.drain();
  s.key(0x04, true, 0);
  p = c.event(2);
  CHECK(!p.b.empty() && p.b[1] == 12 && p.u32(12) == w);
  s.key(0xE1, true, 0x02);      // left shift: its own press does not carry Shift yet
  p = c.event(2);
  CHECK(!p.b.empty() && p.b[1] == 0xE9 && (p.u16(28) & 1) == 0);
  s.key(0x05, true, 0x02);
  p = c.event(2);
  CHECK(!p.b.empty() && p.b[1] == 13 && (p.u16(28) & 1) == 1);
  // the keyboard map: a / A
  c.send(Request(false, 101).u8(12).u8(2).u16(0).done());
  p = c.reply();
  CHECK(p.b.size() == 32 + 16 && p.b[1] == 2 && p.u32(32) == 'a' && p.u32(36) == 'A' && p.u32(40) == 'b');
  // activation from the desktop: FocusOut when a desktop window takes it
  s.activate_window(0);
  CHECK(!c.event(10 /* FocusOut */).b.empty());
  s.activate_window(w);
  CHECK(!c.event(9 /* FocusIn */).b.empty());
  // the cursor the window shows: a glyph of the cursor font is a system shape
  const uint32_t font = c.new_id(), cursor = c.new_id();
  c.send(Request(false, 45).u32(font).u16(6).u16(0).str("cursor").done());
  c.send(Request(false, 94).u32(cursor).u32(font).u32(font).u16(152).u16(153).u16(0).u16(0).u16(0).u16(0xFFFF).u16(0xFFFF).u16(0xFFFF).done());
  c.send(Request(false, 2).u32(w).u32(0x4000).u32(cursor).done());
  desktop::IWindowsSession::Pointer ptr;
  s.pointer(&ptr);
  CHECK(ptr.cursor == 0 && ptr.system_id == 32513);
  // close: WM_DELETE_WINDOW when the client takes it
  const uint32_t protocols = c.intern("WM_PROTOCOLS"), del = c.intern("WM_DELETE_WINDOW");
  c.change_property(w, protocols, 4, 32, {del});
  c.drain();
  s.window_command(w, desktop::IWindowsSession::Command::Close);
  p = c.event(33);
  CHECK(!p.b.empty() && p.u32(8) == protocols && p.u32(12) == del);
  CHECK(!c.closing);
}

static void test_errors() {
  Server s(A);
  Client c(s, false);
  CHECK(c.setup());
  c.send(Request(false, 8).u32(0x12345).done());
  Packet p = c.reply();
  CHECK(p.b.size() == 32 && p.b[0] == 0 && p.b[1] == 3 && p.u32(4) == 0x12345 && p.b[10] == 8);      // BadWindow
  c.send(Request(false, 1, 0).u32(0x77).u32(kRoot).u16(0).u16(0).u16(1).u16(1).u16(0).u16(1).u32(0).u32(0).done());
  p = c.reply();
  CHECK(p.b[0] == 0 && p.b[1] == 14);      // BadIDChoice: not the client's range
  c.send(Request(false, 200).done());
  p = c.reply();
  CHECK(p.b[0] == 0 && p.b[1] == 1);       // BadRequest
  c.send(Request(false, 20).u32(kRoot).done());
  p = c.reply();
  CHECK(p.b[0] == 0 && p.b[1] == 16);      // BadLength (GetProperty cut short)
  // BIG-REQUESTS: enabled, then a request with the extended length
  c.send(Request(false, 98).u16(12).u16(0).str("BIG-REQUESTS").done());
  p = c.reply();
  CHECK(p.b[0] == 1 && p.b[8] == 1 && p.b[9] == 128);
  c.send(Request(false, 128, 0).done());
  p = c.reply();
  CHECK(p.b[0] == 1 && p.u32(8) >= 0x10000);
  std::vector<uint8_t> big = {43, 0, 0, 0, 2, 0, 0, 0};      // GetInputFocus with length 0 + 32-bit length 2
  c.send(big);
  p = c.reply();
  CHECK(p.b.size() == 32 && p.b[0] == 1);
  CHECK(!c.closing);
  // without BIG-REQUESTS a zero length cannot be parsed: the client goes
  Client d(s, false);
  CHECK(d.setup());
  d.send({43, 0, 0, 0});
  CHECK(d.closing);
  // KillClient of another client's window
  Client e(s, false);
  CHECK(e.setup());
  const uint32_t w = e.create_window(kRoot, 0, 0, 10, 10, 0);
  c.send(Request(false, 113).u32(w).done());
  e.pull();
  CHECK(e.closing);
  desktop::WindowsWindowInfo info;
  String title(A);
  CHECK(!s.window_info(w, &info, &title));
}

// A recorded session, then the same bytes cut short and with bits flipped: the server must not crash, read outside its buffers or leak (ASan, LSan).
static void test_truncation_and_bit_flips() {
  std::vector<uint8_t> stream;
  {
    Request r(false, 0);
    r.b.clear();
    r.u8('l').u8(0).u16(11).u16(0).u16(0).u16(0).u16(0);
    stream = r.b;
  }
  const uint32_t base = 1u << 21;
  auto add = [&](const std::vector<uint8_t>& v) { stream.insert(stream.end(), v.begin(), v.end()); };
  add(Request(false, 1, 0).u32(base | 1).u32(kRoot).u16(10).u16(10).u16(100).u16(80).u16(0).u16(1).u32(0).u32(0x802).u32(0xFFFFFF).u32(0x3FFFFF).done());
  add(Request(false, 1, 0).u32(base | 2).u32(base | 1).u16(5).u16(5).u16(30).u16(20).u16(2).u16(1).u32(0).u32(0x2).u32(0x00FF00).done());
  add(Request(false, 18, 0).u32(base | 1).u32(39).u32(31).u8(8).u8(0).u8(0).u8(0).u32(5).str("title").done());
  add(Request(false, 8).u32(base | 1).done());
  add(Request(false, 9).u32(base | 1).done());
  add(Request(false, 55).u32(base | 3).u32(base | 1).u32(0x4).u32(0xFF00FF).done());
  add(Request(false, 70).u32(base | 1).u32(base | 3).u16(0).u16(0).u16(50).u16(50).done());
  add(Request(false, 65).u32(base | 1).u32(base | 3).u16(0).u16(0).u16(99).u16(79).u16(5).u16(70).done());
  add(Request(false, 71).u32(base | 1).u32(base | 3).u16(10).u16(10).u16(40).u16(30).u16(0).u16(90 * 64).done());
  add(Request(false, 69).u32(base | 1).u32(base | 3).u8(0).u8(0).u16(0).u16(0).u16(0).u16(60).u16(10).u16(5).u16(70).done());
  add(Request(false, 53, 1).u32(base | 4).u32(base | 1).u16(16).u16(16).done());
  add(Request(false, 72, 0).u32(base | 1).u32(base | 3).u16(8).u16(2).u16(1).u16(1).u8(0).u8(1).u16(0).u32(0xAA).u32(0x55).done());
  add(Request(false, 62).u32(base | 1).u32(base | 1).u32(base | 3).u16(0).u16(0).u16(10).u16(10).u16(40).u16(40).done());
  add(Request(false, 12).u32(base | 2).u16(0xF).u16(0).u32(20).u32(20).u32(40).u32(30).done());
  add(Request(false, 74).u32(base | 1).u32(base | 3).u16(5).u16(30).u8(3).u8(0).str("abc").done());
  add(Request(false, 73, 2).u32(base | 1).u16(0).u16(0).u16(10).u16(10).u32(0xFFFFFFFF).done());
  add(Request(false, 20, 0).u32(base | 1).u32(39).u32(0).u32(0).u32(100).done());
  add(Request(false, 16, 0).u16(6).u16(0).str("FOOBAR").done());
  add(Request(false, 59).u32(base | 3).u16(0).u16(0).u16(1).u16(1).u16(10).u16(10).done());
  add(Request(false, 4).u32(base | 2).done());
  add(Request(false, 4).u32(base | 1).done());

  auto run = [&](const std::vector<uint8_t>& bytes, size_t chunk) {
    Server s(A);
    s.attach(6, 19);
    const uint32_t id = s.open_client(0);
    for (size_t at = 0; at < bytes.size(); at += chunk) {
      const size_t n = bytes.size() - at < chunk ? bytes.size() - at : chunk;
      s.receive(id, ConstByteView(bytes.data() + at, n));
      ByteBuffer out(A);
      if (s.take_output(id, &out)) break;
    }
    // the desktop looks at whatever is there
    List<uint32_t> list(A);
    s.windows(&list);
    desktop::WindowsWindowInfo info;
    String title(A);
    for (uint32_t w : list.view()) s.window_info(w, &info, &title);
    s.mouse_position(30, 30);
    s.mouse_button(1, true);
    s.key(0x04, true, 0);
    s.close_client(id);
  };
  run(stream, stream.size());
  run(stream, 7);
  for (size_t cut = 0; cut < stream.size(); cut += 3) run(std::vector<uint8_t>(stream.begin(), stream.begin() + long(cut)), 64);
  std::mt19937 rng(1234);
  for (int round = 0; round < 3000; ++round) {
    std::vector<uint8_t> v = stream;
    const int flips = 1 + int(rng() % 8);
    for (int i = 0; i < flips; ++i) {
      const size_t at = 12 + rng() % (v.size() - 12);      // (the setup stays valid so the requests are reached)
      v[at] ^= uint8_t(1u << (rng() % 8));
    }
    run(v, 1 + rng() % 200);
  }
  // random requests: every opcode with random bodies
  for (int round = 0; round < 3000; ++round) {
    std::vector<uint8_t> v(stream.begin(), stream.begin() + 12);
    const int n = 1 + int(rng() % 20);
    for (int i = 0; i < n; ++i) {
      const uint8_t opcode = uint8_t(1 + rng() % 128);
      const size_t words = 1 + rng() % 12;
      std::vector<uint8_t> req(words * 4);
      for (uint8_t& b : req) b = uint8_t(rng());
      req[0] = opcode;
      req[2] = uint8_t(words);
      req[3] = 0;
      // ids that exist now and then
      if (words >= 2 && rng() % 2) { uint32_t id = rng() % 3 ? (base | (1 + rng() % 4)) : kRoot; memcpy(&req[4], &id, 4); }
      if (words >= 3 && rng() % 2) { uint32_t id = base | (1 + rng() % 4); memcpy(&req[8], &id, 4); }
      v.insert(v.end(), req.begin(), req.end());
    }
    run(v, v.size());
  }
}

// ------------------------------------------------------------------ the AF_UNIX transport

struct SystemSync final : process::ISystemSync {
  std::mutex m;
  std::condition_variable cv;
  static thread_local std::unique_lock<std::mutex>* mine;
  void lock() override { mine = new std::unique_lock<std::mutex>(m); }
  void unlock() override { delete mine; mine = nullptr; }
  bool wait(uint64_t ns) override {
    if (ns == ~uint64_t{0}) { cv.wait(*mine); return true; }
    return cv.wait_for(*mine, std::chrono::nanoseconds(ns)) == std::cv_status::no_timeout;
  }
  void notify_all() override { cv.notify_all(); }
};
thread_local std::unique_lock<std::mutex>* SystemSync::mine = nullptr;

// A client over a socket of the manager, as a Linux or Windows program's would be.
struct SocketClient {
  process::UnixSocketManager& m;
  uint32_t id = 0;
  bool big;
  std::vector<uint8_t> in;
  explicit SocketClient(process::UnixSocketManager& manager, bool big_endian) : m(manager), big(big_endian) {}
  ~SocketClient() { if (id) m.release(id); }
  bool connect(const std::string& name, bool abstract_name, const std::string& host = std::string()) {
    if (m.create(process::UnixType::Stream, 42, &id) != process::UnixStatus::Ok) return false;
    const process::UnixName un = process::UnixName::from(abstract_name ? process::UnixName::Kind::Abstract : process::UnixName::Kind::Path,
                                                         ConstByteView(reinterpret_cast<const uint8_t*>(name.data()), name.size()));
    m.sync().lock();
    const process::UnixStatus st = m.connect_locked(id, un, StringView(host.data(), host.size()), 42);
    m.sync().unlock();
    return st == process::UnixStatus::Ok;
  }
  bool write(const std::vector<uint8_t>& bytes) {
    size_t at = 0;
    while (at < bytes.size()) {
      m.sync().lock();
      uint32_t sent = 0;
      const process::UnixStatus st = m.send_locked(id, ConstByteView(bytes.data() + at, bytes.size() - at), nullptr, 42, nullptr, &sent);
      if (st == process::UnixStatus::WouldBlock) m.sync().wait(10'000'000);
      m.sync().unlock();
      if (st != process::UnixStatus::Ok && st != process::UnixStatus::WouldBlock) return false;
      at += st == process::UnixStatus::Ok ? sent : 0;
    }
    return true;
  }
  // Reads until `n` bytes are buffered (or the time is up).
  bool read(size_t n, int ms = 5000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    uint8_t buf[4096];
    while (in.size() < n) {
      if (std::chrono::steady_clock::now() > until) return false;
      m.sync().lock();
      process::UnixSocketManager::Received got;
      const process::UnixStatus st = m.receive_locked(id, ByteView(buf, sizeof buf), false, &got, nullptr);
      if (st == process::UnixStatus::WouldBlock) m.sync().wait(10'000'000);
      m.sync().unlock();
      if (st == process::UnixStatus::Ok) {
        if (got.end_of_file) return false;
        in.insert(in.end(), buf, buf + got.got);
      } else if (st != process::UnixStatus::WouldBlock) {
        return false;
      }
    }
    return true;
  }
  uint32_t u32(size_t at) const {
    return big ? uint32_t(in[at]) << 24 | uint32_t(in[at + 1]) << 16 | uint32_t(in[at + 2]) << 8 | in[at + 3]
               : uint32_t(in[at]) | uint32_t(in[at + 1]) << 8 | uint32_t(in[at + 2]) << 16 | uint32_t(in[at + 3]) << 24;
  }
  uint16_t u16(size_t at) const { return big ? uint16_t(in[at] << 8 | in[at + 1]) : uint16_t(in[at] | in[at + 1] << 8); }
  // Connection setup; the resource id base.
  uint32_t setup() {
    Request r(big, 0);
    r.b.clear();
    r.u8(big ? 'B' : 'l').u8(0).u16(11).u16(0).u16(0).u16(0).u16(0);
    if (!write(r.b) || !read(8) || in[0] != 1) return 0;
    const size_t total = 8 + size_t(u16(6)) * 4;
    if (!read(total)) return 0;
    const uint32_t base = u32(12);
    in.erase(in.begin(), in.begin() + long(total));
    return base;
  }
  // The next reply or error (events skipped).
  std::vector<uint8_t> reply() {
    for (;;) {
      if (!read(32)) return {};
      size_t size = 32;
      if (in[0] == 1) size += size_t(u32(4)) * 4;
      if (!read(size)) return {};
      std::vector<uint8_t> p(in.begin(), in.begin() + long(size));
      in.erase(in.begin(), in.begin() + long(size));
      if (p[0] == 0 || p[0] == 1) return p;
    }
  }
};

static void test_socket_transport() {
  SystemSync sync;
  process::UnixSocketManager sockets(sync, A);
  Server server(A);
  filesystem::RamFileSystem files(A);
  {
    x11::SocketServer transport(server, sockets, A);
    CHECK(transport.listen(0, &files));
    filesystem::DirectoryEntry entry;
    CHECK(files.stat("/Linux/tmp/.X11-unix/X0", &entry) == filesystem::Status::Ok);
    {      // a second server cannot take display 0
      Server second(A);
      x11::SocketServer other(second, sockets, A);
      CHECK(!other.listen(0, nullptr));
    }
    x11::SocketServer& t = transport;
    std::atomic<bool> stop{false};
    std::thread pump([&] {
      while (!stop) {
        while (t.pump()) {}
        t.wait(5'000'000);
      }
    });
    // by the abstract name (what libxcb tries first; Windows programs reach the same)
    SocketClient a(sockets, false);
    CHECK(a.connect("/tmp/.X11-unix/X0", true));
    const uint32_t base_a = a.setup();
    CHECK(base_a != 0);
    // by the path, as a Linux program names it (the host path is what the kernel resolves it to)
    SocketClient b(sockets, true);
    CHECK(b.connect("/tmp/.X11-unix/X0", false, "/Linux/tmp/.X11-unix/X0"));
    const uint32_t base_b = b.setup();
    CHECK(base_b != 0 && base_b != base_a);
    // a window of one client, seen by the other through QueryTree
    CHECK(a.write(Request(false, 1, 0).u32(base_a | 1).u32(kRoot).u16(0).u16(0).u16(50).u16(50).u16(0).u16(1).u32(0).u32(0).done()));
    CHECK(a.write(Request(false, 43).done()));
    std::vector<uint8_t> p = a.reply();
    CHECK(p.size() == 32 && p[0] == 1);
    CHECK(b.write(Request(true, 15).u32(kRoot).done()));
    p = b.reply();
    CHECK(p.size() >= 32 && p[0] == 1);
    bool found = false;
    for (size_t at = 32; at + 4 <= p.size(); at += 4) found |= (uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 | p[at + 3]) == (base_a | 1);
    CHECK(found);
    stop = true;
    pump.join();
  }
  CHECK(server.client_count() == 0);
}

// Several clients at once on their own threads, the transport on its thread and the desktop on another: windows made, mapped, drawn, moved, input
// delivered, everything torn down; every client's replies come back right and in order.
static void test_threads() {
  SystemSync sync;
  process::UnixSocketManager sockets(sync, A);
  Server server(A);
  server.attach(6, 19);
  x11::SocketServer transport(server, sockets, A);
  CHECK(transport.listen(3, nullptr));
  std::atomic<bool> stop{false};
  std::thread pump([&] {
    while (!stop) {
      while (transport.pump()) {}
      transport.wait(2'000'000);
    }
  });
  std::thread desktop_thread([&] {
    List<uint32_t> changed(A), list(A);
    List<desktop::IWindowsSession::Repaint> repainted(A);
    desktop::WindowsWindowInfo info;
    String title(A);
    uint32_t n = 0;
    while (!stop) {
      server.take_changes(&changed, &repainted);
      server.windows(&list);
      for (uint32_t w : list.view()) {
        if (server.window_info(w, &info, &title) && (n % 7) == 0) server.move_window(w, {info.window.x + 1, info.window.y, info.window.width, info.window.height});
        if ((n % 5) == 0) server.activate_window(w);
      }
      server.mouse_position(int32_t(n % 600), int32_t(n % 400));
      server.mouse_button(1, (n & 1) != 0);
      server.key(uint8_t(0x04 + n % 26), (n & 2) != 0, 0);
      desktop::IWindowsSession::Pointer ptr;
      server.pointer(&ptr);
      ++n;
      std::this_thread::yield();
    }
  });
  constexpr int kClients = 8;
  std::atomic<int> good{0};
  std::vector<std::thread> clients;
  for (int k = 0; k < kClients; ++k) {
    clients.emplace_back([&, k] {
      const bool big = (k & 1) != 0;
      SocketClient c(sockets, big);
      if (!c.connect("/tmp/.X11-unix/X3", true)) return;
      const uint32_t base = c.setup();
      if (!base) return;
      bool ok = true;
      for (int round = 0; round < 25 && ok; ++round) {
        const uint32_t w = base | uint32_t(1 + round);
        std::vector<uint8_t> batch;
        auto add = [&](const std::vector<uint8_t>& v) { batch.insert(batch.end(), v.begin(), v.end()); };
        add(Request(big, 1, 0).u32(w).u32(kRoot).u16(uint16_t(10 * k)).u16(10).u16(120).u16(90).u16(0).u16(1).u32(0).u32(0x802).u32(0x101010).u32(0x3FFFFF).done());
        add(Request(big, 18, 0).u32(w).u32(39).u32(31).u8(8).u8(0).u8(0).u8(0).u32(4).str("win").u8('0' + k).done());
        add(Request(big, 8).u32(w).done());
        add(Request(big, 55).u32(w | 0x100000).u32(w).u32(0x4).u32(0xFF0000).done());
        add(Request(big, 70).u32(w).u32(w | 0x100000).u16(0).u16(0).u16(60).u16(60).done());
        const std::string name = "ATOM_" + std::to_string(k) + "_" + std::to_string(round);
        add(Request(big, 16, 0).u16(uint16_t(name.size())).u16(0).str(name).done());
        if (!c.write(batch)) { ok = false; break; }
        std::vector<uint8_t> p = c.reply();
        ok = p.size() == 32 && p[0] == 1;
        if (!ok) break;
        // GetAtomName of that atom gives the name back
        const uint32_t atom = big ? uint32_t(p[8]) << 24 | uint32_t(p[9]) << 16 | uint32_t(p[10]) << 8 | p[11]
                                  : uint32_t(p[8]) | uint32_t(p[9]) << 8 | uint32_t(p[10]) << 16 | uint32_t(p[11]) << 24;
        if (!c.write(Request(big, 17).u32(atom).done())) { ok = false; break; }
        p = c.reply();
        ok = p.size() >= 32 + name.size() && p[0] == 1 && std::string(p.begin() + 32, p.begin() + 32 + long(name.size())) == name;
        std::vector<uint8_t> teardown;
        auto add2 = [&](const std::vector<uint8_t>& v) { teardown.insert(teardown.end(), v.begin(), v.end()); };
        add2(Request(big, 60).u32(w | 0x100000).done());
        if (round % 3 == 0) add2(Request(big, 4).u32(w).done());
        if (!c.write(teardown)) ok = false;
      }
      if (ok) ++good;
    });
  }
  for (std::thread& t : clients) t.join();
  // the clients are gone: so are their windows, once the transport has seen them go
  for (int i = 0; i < 500 && server.client_count() != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  stop = true;
  pump.join();
  desktop_thread.join();
  CHECK(good == kClients);
  CHECK(server.client_count() == 0);
  List<uint32_t> list(A);
  server.windows(&list);
  CHECK(list.count() == 0);
}

int main() {
  test_setup();
  test_windows_and_properties();
  test_window_manager_hints();
  test_struts();
  test_drawing();
  test_input();
  test_errors();
  test_truncation_and_bit_flips();
  test_socket_transport();
  test_threads();
  if (g_failed) {
    printf("X11ServerTest: %d FAILED\n", g_failed.load());
    return 1;
  }
  printf("X11ServerTest: all passed\n");
  return 0;
}
