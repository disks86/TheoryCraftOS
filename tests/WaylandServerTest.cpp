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

// The kernel's Wayland compositor (src/Wayland) and the files in memory it reads its clients' pictures from (src/Process/MemoryFile): the registry and
// its globals, shm pools and buffers, toplevels framed by the desktop (configure, ack, map, move, resize, maximize, minimize, close, decorations),
// popups and their positioners, subsurfaces (sync and desync), drawing straight from the client's memory, frame callbacks, the seat (pointer focus and
// the implicit grab, buttons, the wheel, the cursor a client sets, keyboard focus, keys, modifiers, the keymap), protocol errors, truncated and bit-flipped
// streams, and the AF_UNIX transport with descriptors passed as SCM_RIGHTS, with several client threads and a desktop thread at once; zwp_linux_dmabuf_v1
// (formats, params, created / failed, the errors, fuzzed sessions) and GPU buffers over the kernel's Direct3D layer: exports and imports, drawing in place on
// the image's device and from the CPU's view, an image outliving its process, client threads exporting while the desktop draws.
// Run: tests/run_host_tests.sh WaylandServerTest

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Graphics/Graphics2D/Device/DeviceRenderer2D.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Graphics/Graphics3D/LockedDevice.hpp"
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Process/DxgBufferFile.hpp"
#include "Process/MemoryFile.hpp"
#include "Process/ProcessFiles.hpp"
#include "Process/UnixSockets.hpp"
#include "Media/Image.hpp"
#include "Media/ImageCodecs.hpp"
#include "Wayland/Dxg/WaylandDxgImage.hpp"
#include "Wayland/Media/WaylandClipboardPng.hpp"
#include "Wayland/WaylandServer.hpp"
#include "Wayland/WaylandSocketServer.hpp"

using wayland::Fd;
using wayland::Server;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    ++live;
    void* p = nullptr;
    if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size ? size : 1) != 0) { --live; return nullptr; }
    return p;
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
static TestAllocator A;

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// ------------------------------------------------------------------ memory a test client shares with the server

struct TestMemory final : wayland::Memory {
  std::vector<uint8_t>* bytes;
  bool not_memory = false;
  explicit TestMemory(std::vector<uint8_t>* b) : bytes(b) {}
  ByteView map(uint64_t size) override {
    if (not_memory || !bytes || bytes->size() < size) return ByteView();
    return ByteView(bytes->data(), bytes->size());
  }
};

// What the server sent with the keymap (the test's fd maker keeps the contents).
static std::vector<std::vector<uint8_t>*> g_made;
static Fd make_test_fd(void*, ConstByteView contents) {
  auto* v = new std::vector<uint8_t>(contents.data(), contents.data() + contents.size());
  g_made.push_back(v);
  return Fd::make_as<TestMemory>(A, v);
}
static void free_made() {
  for (auto* v : g_made) delete v;
  g_made.clear();
}

// ------------------------------------------------------------------ a raw Wayland client

struct Msg {
  std::vector<uint8_t> b;
  Msg(uint32_t object, uint16_t opcode) { u(object); u(opcode); }
  Msg& u(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); return *this; }
  Msg& i(int32_t v) { return u(uint32_t(v)); }
  Msg& s(const std::string& text) {
    u(uint32_t(text.size() + 1));
    for (char c : text) b.push_back(uint8_t(c));
    b.push_back(0);
    while (b.size() % 4) b.push_back(0);
    return *this;
  }
  std::vector<uint8_t> done() {
    std::vector<uint8_t> out = b;
    const uint32_t word = uint32_t(out.size()) << 16 | uint32_t(out[4] | out[5] << 8);
    for (int k = 0; k < 4; ++k) out[4 + k] = uint8_t(word >> (8 * k));
    return out;
  }
};

struct Ev {
  uint32_t object = 0;
  uint16_t opcode = 0;
  std::vector<uint8_t> a;
  bool fd = false;
  uint32_t u(size_t word) const { size_t at = word * 4; return at + 4 <= a.size() ? uint32_t(a[at] | a[at + 1] << 8 | a[at + 2] << 16 | uint32_t(a[at + 3]) << 24) : 0; }
  int32_t i(size_t word) const { return int32_t(u(word)); }
  std::string s(size_t word) const {      // the string at that word
    const uint32_t n = u(word);
    if (n == 0) return std::string();
    return std::string(a.begin() + long(word * 4 + 4), a.begin() + long(word * 4 + 4 + n - 1));
  }
};

struct WClient {
  Server& s;
  uint32_t number = 0;
  uint32_t next = 2;
  std::vector<uint8_t> in;
  std::deque<Ev> events;
  std::vector<Fd> fds;
  bool closing = false;
  std::map<std::string, std::pair<uint32_t, uint32_t>> globals;      // interface -> name, version
  explicit WClient(Server& server) : s(server) { number = s.open_client(7); }
  ~WClient() { if (number) s.close_client(number); }
  uint32_t new_id() { return next++; }
  void send(const std::vector<uint8_t>& bytes, std::vector<Fd> with = {}) {
    s.receive(number, ConstByteView(bytes.data(), bytes.size()), ArrayView<Fd>(with.data(), with.size()));
    pull();
  }
  void send(Msg m, std::vector<Fd> with = {}) { send(m.done(), std::move(with)); }
  void pull() {
    ByteBuffer buf(A);
    List<Fd> got(A);
    closing = s.take_output(number, &buf, &got) || closing;
    for (Fd& f : got.view()) fds.push_back(f);
    in.insert(in.end(), buf.view().data(), buf.view().data() + buf.size());
    while (in.size() >= 8) {
      const uint32_t object = uint32_t(in[0] | in[1] << 8 | in[2] << 16 | uint32_t(in[3]) << 24);
      const uint32_t word = uint32_t(in[4] | in[5] << 8 | in[6] << 16 | uint32_t(in[7]) << 24);
      const uint32_t size = word >> 16;
      if (size < 8 || in.size() < size) break;
      Ev e;
      e.object = object;
      e.opcode = uint16_t(word & 0xFFFF);
      e.a.assign(in.begin() + 8, in.begin() + size);
      events.push_back(e);
      in.erase(in.begin(), in.begin() + size);
    }
  }
  // The first event of that object and opcode (it and those before it of the same object are dropped); nullopt-ish: object 0.
  Ev take(uint32_t object, uint16_t opcode) {
    pull();
    for (auto it = events.begin(); it != events.end(); ++it) {
      if (it->object == object && it->opcode == opcode) {
        Ev e = *it;
        events.erase(it);
        return e;
      }
    }
    return Ev();
  }
  bool has(uint32_t object, uint16_t opcode) {
    pull();
    for (const Ev& e : events) if (e.object == object && e.opcode == opcode) return true;
    return false;
  }
  int count(uint32_t object, uint16_t opcode) {
    pull();
    int n = 0;
    for (const Ev& e : events) if (e.object == object && e.opcode == opcode) ++n;
    return n;
  }
  void drop_events() { pull(); events.clear(); }
  // wl_display.get_registry, then the globals
  uint32_t registry() {
    const uint32_t r = new_id();
    send(Msg(1, 1).u(r));
    for (Ev& e : events) if (e.object == r && e.opcode == 0) globals[e.s(1)] = {e.u(0), e.u(1 + (e.u(1) + 3) / 4 + 1)};
    return r;
  }
  uint32_t bind(uint32_t registry_id, const std::string& interface, uint32_t version = 0) {
    const auto g = globals[interface];
    const uint32_t id = new_id();
    send(Msg(registry_id, 0).u(g.first).s(interface).u(version ? version : g.second).u(id));
    return id;
  }
  // wl_display.sync: true when its callback came back (everything before it was carried out)
  bool roundtrip() {
    const uint32_t cb = new_id();
    send(Msg(1, 0).u(cb));
    return take(cb, 0).object == cb;
  }
  bool error(uint32_t* object = nullptr, uint32_t* code = nullptr) {
    Ev e = take(1, 0);
    if (e.object != 1) return false;
    if (object) *object = e.u(0);
    if (code) *code = e.u(1);
    return true;
  }
};

// A client with the usual globals bound.
struct App : WClient {
  uint32_t reg = 0, compositor = 0, subcompositor = 0, shm = 0, seat = 0, output = 0, wm = 0, deco = 0, ddm = 0;
  explicit App(Server& server, bool all = true) : WClient(server) {
    reg = registry();
    compositor = bind(reg, "wl_compositor");
    subcompositor = bind(reg, "wl_subcompositor");
    shm = bind(reg, "wl_shm");
    wm = bind(reg, "xdg_wm_base");
    if (all) {
      seat = bind(reg, "wl_seat");
      output = bind(reg, "wl_output");
      deco = bind(reg, "zxdg_decoration_manager_v1");
      ddm = bind(reg, "wl_data_device_manager");
    }
    drop_events();
  }
  // A pool of `bytes` bytes (the client's memory), shared with the server.
  struct Pool {
    uint32_t id = 0;
    std::vector<uint8_t>* memory = nullptr;
  };
  std::vector<std::vector<uint8_t>*> memories;
  ~App() { for (auto* m : memories) delete m; }
  Pool pool(size_t bytes) {
    Pool p;
    p.memory = new std::vector<uint8_t>(bytes);
    memories.push_back(p.memory);
    p.id = new_id();
    send(Msg(shm, 0).u(p.id).i(int32_t(bytes)), {Fd::make_as<TestMemory>(A, p.memory)});
    return p;
  }
  uint32_t buffer(const Pool& p, int32_t offset, int32_t w, int32_t h, int32_t stride, uint32_t format = 1) {
    const uint32_t id = new_id();
    send(Msg(p.id, 0).u(id).i(offset).i(w).i(h).i(stride).u(format));
    return id;
  }
  static void fill(const Pool& p, int32_t offset, int32_t w, int32_t h, int32_t stride, uint32_t color) {
    for (int32_t y = 0; y < h; ++y)
      for (int32_t x = 0; x < w; ++x) memcpy(p.memory->data() + offset + y * stride + x * 4, &color, 4);
  }
  uint32_t surface() {
    const uint32_t id = new_id();
    send(Msg(compositor, 0).u(id));
    return id;
  }
  struct Window {
    uint32_t surface = 0, xdg = 0, toplevel = 0, decoration = 0, serial = 0;
    int32_t cw = 0, ch = 0;      // what the last configure asked for
  };
  // A toplevel up to its first configure (acknowledged).
  Window toplevel(bool decorate = true, const std::string& title = "test") {
    Window w;
    w.surface = surface();
    w.xdg = new_id();
    send(Msg(wm, 2).u(w.xdg).u(w.surface));
    w.toplevel = new_id();
    send(Msg(w.xdg, 1).u(w.toplevel));
    if (decorate) {
      w.decoration = new_id();
      send(Msg(deco, 1).u(w.decoration).u(w.toplevel));
    }
    send(Msg(w.toplevel, 2).s(title));
    send(Msg(w.surface, 6));      // the initial commit
    configured(w);
    return w;
  }
  // Takes the configure that came (toplevel + xdg_surface) and acknowledges it.
  bool configured(Window& w) {
    Ev c = take(w.toplevel, 0);
    Ev x = take(w.xdg, 0);
    if (c.object == 0 || x.object == 0) return false;
    w.cw = c.i(0);
    w.ch = c.i(1);
    w.serial = x.u(0);
    send(Msg(w.xdg, 4).u(w.serial));
    return true;
  }
  // Attaches a fresh buffer of w x h filled with `color` and commits it.
  uint32_t show(Window& w, int32_t width, int32_t height, uint32_t color, Pool* keep = nullptr) {
    Pool p = pool(size_t(width) * height * 4);
    fill(p, 0, width, height, width * 4, color);
    const uint32_t b = buffer(p, 0, width, height, width * 4);
    send(Msg(w.surface, 1).u(b).i(0).i(0));
    send(Msg(w.surface, 2).i(0).i(0).i(width).i(height));
    send(Msg(w.surface, 6));
    if (keep) *keep = p;
    return b;
  }
};

static desktop::IWindowsSession::Monitor monitor(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t taskbar) {
  desktop::IWindowsSession::Monitor m;
  m.rect = {x, y, w, h};
  m.work = {x, y, w, h - taskbar};
  return m;
}

static std::vector<uint32_t> listed(Server& s) {
  List<uint32_t> list(A);
  s.windows(&list);
  return std::vector<uint32_t>(list.view().data(), list.view().data() + list.count());
}

// ------------------------------------------------------------------ MemoryFile

static void test_memory_file() {
  using process::MemoryFile;
  using process::BlockRef;
  using filesystem::Status;
  {
    MemoryFile f(A, true);
    uint64_t size = 99;
    CHECK(!f.block(&size) && size == 0);      // (empty: no block yet)
    uint64_t done = 0;
    const uint8_t hello[5] = {'h', 'e', 'l', 'l', 'o'};
    CHECK(f.write(10, ConstByteView(hello, 5), &done) == Status::Ok && done == 5 && f.size() == 15);
    uint8_t back[20] = {};
    uint64_t got = 0;
    CHECK(f.read(0, ByteView(back, 20), &got) == Status::Ok && got == 15 && back[0] == 0 && back[10] == 'h' && back[14] == 'o');
    CHECK(f.read(15, ByteView(back, 20), &got) == Status::Ok && got == 0);
    BlockRef b = f.block(&size);
    CHECK(b && size == 15 && b.get()->capacity() == 4096 && b.get()->bytes()[11] == 'e');
    // writes through the block are the file's
    b.get()->bytes()[0] = 'X';
    CHECK(f.read(0, ByteView(back, 1), &got) == Status::Ok && back[0] == 'X');
    // growing within the block keeps it; past it moves the contents, the old block stays valid for whoever holds it
    CHECK(f.resize(4000) == Status::Ok);
    uint64_t s2 = 0;
    BlockRef same = f.block(&s2);
    CHECK(same.get() == b.get() && s2 == 4000);
    CHECK(f.resize(10000) == Status::Ok && f.size() == 10000);
    BlockRef bigger = f.block(&s2);
    CHECK(bigger.get() != b.get() && bigger.get()->capacity() >= 10000 && bigger.get()->bytes()[0] == 'X' && bigger.get()->bytes()[14] == 'o');
    CHECK(b.get()->bytes()[14] == 'o');      // (still readable: this reference keeps it)
    // shrinking clears what is cut off
    CHECK(f.resize(12) == Status::Ok && f.resize(20) == Status::Ok);
    CHECK(f.read(0, ByteView(back, 20), &got) == Status::Ok && got == 20 && back[11] == 'e' && back[12] == 0 && back[14] == 0);
    // fallocate
    CHECK(f.allocate(0, 50) == Status::Ok && f.size() == 50);
    CHECK(f.allocate(100, 0) == Status::InvalidArgument);
    // seals
    CHECK(f.add_seals(MemoryFile::kSealShrink));
    CHECK(f.resize(10) == Status::AccessDenied && f.resize(60) == Status::Ok);
    CHECK(f.add_seals(MemoryFile::kSealGrow | MemoryFile::kSealSeal));
    CHECK(f.resize(61) == Status::AccessDenied && f.write(60, ConstByteView(hello, 1), &done) == Status::AccessDenied);
    CHECK(!f.add_seals(MemoryFile::kSealWrite));      // (sealed against more seals)
    CHECK(f.seals() == (MemoryFile::kSealShrink | MemoryFile::kSealGrow | MemoryFile::kSealSeal));
    // limits: sizes that would wrap or exceed the maximum
    MemoryFile g(A, false);
    CHECK(!g.add_seals(MemoryFile::kSealGrow));      // (no MFD_ALLOW_SEALING)
    CHECK(g.resize(MemoryFile::kMaxSize + 1) == Status::NoSpace);
    CHECK(g.write(~uint64_t{0} - 2, ConstByteView(hello, 5), &done) == Status::NoSpace);
    CHECK(g.allocate(~uint64_t{0} - 10, 20) == Status::NoSpace);
  }
  CHECK(A.live == 0);
  // several threads reading the block while another grows and shrinks the file (the holders' memory stays valid)
  {
    MemoryFile f(A, false);
    CHECK(f.resize(4096) == filesystem::Status::Ok);
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int k = 0; k < 4; ++k)
      readers.emplace_back([&] {
        while (!stop) {
          uint64_t size = 0;
          BlockRef b = f.block(&size);
          if (b) {
            volatile uint8_t sum = 0;
            for (uint64_t i = 0; i < size && i < b.get()->capacity(); i += 512) sum = uint8_t(sum + b.get()->bytes()[i]);
          }
          uint8_t buf[64];
          uint64_t got = 0;
          f.read(100, ByteView(buf, sizeof buf), &got);
        }
      });
    for (int round = 0; round < 300; ++round) {
      f.resize(uint64_t(4096 + (round % 7) * 8192));
      uint64_t done = 0;
      const uint8_t v = uint8_t(round);
      f.write(uint64_t(round * 13 % 4000), ConstByteView(&v, 1), &done);
    }
    stop = true;
    for (std::thread& t : readers) t.join();
  }
  CHECK(A.live == 0);
}

// ------------------------------------------------------------------ the registry

static void test_registry() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.set_display(1280, 800, {});
  {
    WClient c(server);
    CHECK(c.number != 0);
    const uint32_t r = c.registry();
    const char* names[] = {"wl_compositor", "wl_subcompositor", "wl_shm", "wl_seat", "wl_output", "wl_data_device_manager", "xdg_wm_base", "zxdg_decoration_manager_v1"};
    for (const char* n : names) CHECK(c.globals.count(n) == 1);
    CHECK(c.globals["xdg_wm_base"].second == 6 && c.globals["wl_seat"].second == 8);
    c.drop_events();
    const uint32_t shm = c.bind(r, "wl_shm");
    int formats = 0;
    for (const Ev& e : c.events) if (e.object == shm && e.opcode == 0) ++formats;
    CHECK(formats == 4 + 17);      // (RGB, then the YUV formats)
    const uint32_t seat = c.bind(r, "wl_seat");
    Ev caps = c.take(seat, 0);
    CHECK(caps.object == seat && caps.u(0) == 7);      // (pointer, keyboard, touch)
    CHECK(c.take(seat, 1).s(0) == "seat0");
    const uint32_t output = c.bind(r, "wl_output");
    Ev mode = c.take(output, 1);
    CHECK(mode.object == output && mode.i(1) == 1280 && mode.i(2) == 800);
    CHECK(c.has(output, 2));      // done
    // sync: the callback's done, then its id is given back
    const uint32_t cb = c.new_id();
    c.send(Msg(1, 0).u(cb));
    CHECK(c.take(cb, 0).object == cb);
    Ev del = c.take(1, 1);
    CHECK(del.object == 1 && del.u(0) == cb);
    CHECK(!c.closing);
    // a bind of a version the global does not have is an error
    const uint32_t bad = c.new_id();
    c.send(Msg(r, 0).u(c.globals["wl_compositor"].first).s("wl_compositor").u(99).u(bad));
    CHECK(c.error() && c.closing);
  }
  {      // a bind under the wrong interface name
    WClient c(server);
    const uint32_t r = c.registry();
    c.send(Msg(r, 0).u(c.globals["wl_shm"].first).s("wl_compositor").u(1).u(c.new_id()));
    CHECK(c.error());
  }
  CHECK(server.client_count() == 0);
}

// ------------------------------------------------------------------ toplevels and the desktop

static void test_toplevel() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  const desktop::IWindowsSession::Monitor m = monitor(0, 0, 1280, 800, 40);
  server.set_display(1280, 800, ArrayView<const desktop::IWindowsSession::Monitor>(&m, 1));
  App app(server);
  App::Window w = app.toplevel(true, "Hello Wayland");
  CHECK(w.cw == 0 && w.ch == 0);      // (the client chooses the first size)
  CHECK(listed(server).empty());      // (not shown before it has a picture)
  App::Pool pool;
  const uint32_t buffer = app.show(w, 300, 200, 0xFF336699u, &pool);
  std::vector<uint32_t> list = listed(server);
  CHECK(list.size() == 1);
  if (list.empty()) return;
  const uint32_t win = list[0];
  desktop::WindowsWindowInfo info;
  String title(A);
  CHECK(server.window_info(win, &info, &title));
  CHECK(info.visible && info.caption && info.sizable && info.minimize_box && info.maximize_box && !info.popup);
  CHECK(title.view() == StringView("Hello Wayland"));
  CHECK(info.surface.width == 300 && info.surface.height == 200);
  CHECK(info.window.width == 300 + 8 && info.window.height == 200 + 4 + 24 + 4);
  CHECK(info.surface.x == info.window.x + 4 && info.surface.y == info.window.y + 28);
  CHECK(info.window.x >= 0 && info.window.y >= 0 && info.window.y + int32_t(info.window.height) <= 760);
  CHECK(server.window_pixel(win, 10, 10) == 0xFF336699u);
  // the surface entered the output
  CHECK(app.take(w.surface, 0).u(0) == app.output);
  // what the desktop is told
  List<uint32_t> changed(A);
  List<desktop::IWindowsSession::Repaint> repainted(A);
  server.take_changes(&changed, &repainted);
  bool saw = false;
  for (uint32_t c : changed.view()) saw |= c == win;
  CHECK(saw && repainted.count() >= 1);
  // drawing: straight from the client's memory, then the frame callback is answered
  const uint32_t frame = app.new_id();
  app.send(Msg(w.surface, 3).u(frame));
  App::fill(pool, 0, 300, 200, 1200, 0xFF00FF00u);
  app.send(Msg(w.surface, 2).i(5).i(6).i(10).i(10));
  app.send(Msg(w.surface, 6));
  server.take_changes(&changed, &repainted);
  CHECK(repainted.count() == 1 && repainted[0].window == win && repainted[0].dirty.x == 5 && repainted[0].dirty.y == 6 && repainted[0].dirty.width == 10);
  CHECK(!app.has(frame, 0));
  std::vector<uint32_t> target(400 * 300, 0);
  graphics::Surface ts;
  ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
  ts.width = 400;
  ts.height = 300;
  ts.pitch = 400;
  {
    graphics::SoftwareRenderer2D renderer(ts);
    uint32_t dw = 0, dh = 0;
    CHECK(server.draw_window(win, renderer, 50, 40, &dw, &dh));
    CHECK(dw == 300 && dh == 200);
  }
  CHECK(target[40 * 400 + 50] == 0xFF00FF00u && target[239 * 400 + 349] == 0xFF00FF00u && target[39 * 400 + 50] == 0);
  CHECK(app.take(frame, 0).object == frame);
  // the same buffer again is not released; a new one releases the old
  CHECK(!app.has(buffer, 0));
  app.show(w, 300, 200, 0xFF112233u);
  CHECK(app.has(buffer, 0));
  // the user moves the frame: no configure (the size is the same), the window is where the frame is
  app.drop_events();
  server.move_window(win, {100, 120, info.window.width, info.window.height});
  CHECK(server.window_info(win, &info, &title) && info.window.x == 100 && info.window.y == 120 && info.surface.x == 104 && info.surface.y == 148);
  CHECK(!app.has(w.toplevel, 0));
  // ... and sizes it: the client is asked, the frame shows the new size until the client answers with a picture of it
  server.move_window(win, {100, 120, 508, 432});
  CHECK(app.configured(w) && w.cw == 500 && w.ch == 400);
  CHECK(server.window_info(win, &info, &title) && info.surface.width == 500 && info.surface.height == 400);
  app.show(w, 500, 400, 0xFF445566u);
  CHECK(server.window_info(win, &info, &title) && info.surface.width == 500);
  // a client that answers a different size (its minimum): that is what is shown
  app.send(Msg(w.toplevel, 8).i(450).i(350));
  server.move_window(win, {100, 120, 208, 132});
  CHECK(app.configured(w) && w.cw == 450 && w.ch == 350);
  // activation: keyboard focus and the activated state. A new window is active from the moment it is shown (it gets the keys); the desktop taking the
  // activation away and giving it back is seen by the client.
  CHECK(server.window_info(win, &info, &title) && info.active);
  server.activate_window(0);
  CHECK(server.window_info(win, &info, &title) && !info.active);
  app.drop_events();
  server.activate_window(win);
  Ev c = app.take(w.toplevel, 0);
  CHECK(c.object == w.toplevel);
  bool activated = false;
  for (size_t k = 0; k < c.u(2) / 4; ++k) activated |= c.u(3 + k) == 4;
  CHECK(activated);
  CHECK(server.window_info(win, &info, &title) && info.active);
  // maximize (the desktop's button): the work area inside the frame
  app.drop_events();
  server.window_command(win, desktop::IWindowsSession::Command::Maximize);
  CHECK(app.configured(w) && w.cw == 1280 - 8 && w.ch == 760 - 32);
  app.show(w, w.cw, w.ch, 0xFF000000u);
  CHECK(server.window_info(win, &info, &title) && info.maximized && info.window.x == 0 && info.window.y == 0 && int32_t(info.window.height) == 760);
  server.window_command(win, desktop::IWindowsSession::Command::Restore);
  CHECK(app.configured(w) && w.cw == 450 && w.ch == 350);
  // minimize and back
  server.window_command(win, desktop::IWindowsSession::Command::Minimize);
  CHECK(server.window_info(win, &info, &title) && info.minimized);
  server.window_command(win, desktop::IWindowsSession::Command::Restore);
  CHECK(server.window_info(win, &info, &title) && !info.minimized);
  // the client minimizes itself, maximizes itself
  app.send(Msg(w.toplevel, 13));
  CHECK(server.window_info(win, &info, &title) && info.minimized);
  server.window_command(win, desktop::IWindowsSession::Command::Restore);
  app.drop_events();
  app.send(Msg(w.toplevel, 9));
  CHECK(app.configured(w) && w.cw == 1272);
  app.send(Msg(w.toplevel, 10));
  CHECK(app.configured(w) && w.cw == 450);
  // fullscreen: the monitor, no frame
  app.send(Msg(w.toplevel, 11).u(0));
  CHECK(app.configured(w) && w.cw == 1280 && w.ch == 800);
  app.show(w, 1280, 800, 0xFF000000u);
  CHECK(server.window_info(win, &info, &title) && !info.caption && info.window.width == 1280 && info.window.x == 0);
  app.send(Msg(w.toplevel, 12));
  CHECK(app.configured(w) && w.cw == 450);
  app.show(w, 450, 350, 0xFF000000u);
  // a move started by the client (its own title bar): the desktop drags the frame
  app.send(Msg(w.toplevel, 5).u(app.seat).u(1));
  uint32_t edge = 0;
  CHECK(server.take_drag(&edge) == win && edge == 9);
  app.send(Msg(w.toplevel, 6).u(app.seat).u(1).u(10));      // bottom right
  CHECK(server.take_drag(&edge) == win && edge == 8);
  CHECK(server.take_drag(&edge) == 0);
  // the window menu the client asks for (a right click on its own title bar): the desktop opens it where the client said (of its window geometry)
  int32_t mx = 0, my = 0;
  CHECK(server.take_window_menu(&mx, &my) == 0);
  app.send(Msg(w.toplevel, 4).u(app.seat).u(1).i(37).i(12));
  CHECK(server.take_window_menu(&mx, &my) == win && mx == 37 && my == 12);
  CHECK(server.take_window_menu(&mx, &my) == 0);
  // the close button: the client is asked
  server.window_command(win, desktop::IWindowsSession::Command::Close);
  CHECK(app.has(w.toplevel, 1));
  // fixed size: not sizable
  app.send(Msg(w.toplevel, 7).i(450).i(350));
  app.send(Msg(w.toplevel, 8).i(450).i(350));
  CHECK(server.window_info(win, &info, &title) && !info.sizable && !info.maximize_box);
  // the decoration object goes: the client draws its own frame from now on
  app.send(Msg(w.decoration, 0));
  CHECK(server.window_info(win, &info, &title) && !info.caption && info.window.width == info.surface.width);
  // a null buffer unmaps it; then it starts over with an initial commit
  app.send(Msg(w.surface, 1).u(0).i(0).i(0));
  app.send(Msg(w.surface, 6));
  CHECK(listed(server).empty());
  CHECK(server.window_info(win, &info, &title) && !info.visible);
  app.drop_events();
  app.send(Msg(w.surface, 6));
  CHECK(app.configured(w));
  app.show(w, 100, 100, 0xFFFFFFFFu);
  CHECK(listed(server).size() == 1);
  // the toplevel goes: so does the window
  app.send(Msg(w.toplevel, 0));
  CHECK(listed(server).empty() && !server.window_info(win, &info, &title));
  CHECK(app.roundtrip() && !app.closing);
}

// A window with window geometry (a client-side shadow around it): the desktop sees the geometry; drawing leaves the shadow out.
static void test_geometry_and_csd() {
  Server server(A);
  server.attach(4, 24);
  server.set_display(1024, 768, {});
  App app(server, false);
  App::Window w = app.toplevel(false, "csd");
  App::Pool p = app.pool(220 * 170 * 4);
  App::fill(p, 0, 220, 170, 880, 0x40000000u);              // shadow
  for (int y = 10; y < 160; ++y)
    for (int x = 10; x < 210; ++x) {
      const uint32_t v = 0xFFAA0000u;
      memcpy(p.memory->data() + y * 880 + x * 4, &v, 4);
    }
  const uint32_t b = app.buffer(p, 0, 220, 170, 880, 0);      // ARGB
  app.send(Msg(w.xdg, 3).i(10).i(10).i(200).i(150));
  app.send(Msg(w.surface, 1).u(b).i(0).i(0));
  app.send(Msg(w.surface, 6));
  std::vector<uint32_t> list = listed(server);
  CHECK(list.size() == 1);
  if (list.empty()) return;
  desktop::WindowsWindowInfo info;
  String title(A);
  CHECK(server.window_info(list[0], &info, &title) && !info.caption && info.surface.width == 200 && info.surface.height == 150);
  CHECK(info.window.width == 200 && info.window.x == info.surface.x);
  CHECK(server.window_pixel(list[0], 0, 0) == 0xFFAA0000u);
  std::vector<uint32_t> target(300 * 300, 0xFF0000FFu);
  graphics::Surface ts;
  ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
  ts.width = ts.height = ts.pitch = 300;
  {
    graphics::SoftwareRenderer2D renderer(ts);
    renderer.set_clip({20, 20, 200, 150});      // (the desktop's view clips to the frame's inside)
    uint32_t dw = 0, dh = 0;
    CHECK(server.draw_window(list[0], renderer, 20, 20, &dw, &dh) && dw == 200 && dh == 150);
  }
  CHECK(target[20 * 300 + 20] == 0xFFAA0000u && target[19 * 300 + 20] == 0xFF0000FFu && target[169 * 300 + 219] == 0xFFAA0000u);
  // the pointer at the shadow's place is not over it; at the geometry it is
  const uint32_t pointer = app.new_id();
  app.seat = app.bind(app.reg, "wl_seat");
  app.send(Msg(app.seat, 0).u(pointer));
  app.drop_events();
  server.mouse_position(info.surface.x + 5, info.surface.y + 5);
  Ev enter = app.take(pointer, 0);
  CHECK(enter.object == pointer && enter.u(1) == w.surface && enter.i(2) == 15 * 256 && enter.i(3) == 15 * 256);
}

// ------------------------------------------------------------------ popups

static void test_popups() {
  Server server(A);
  server.attach(4, 24);
  const desktop::IWindowsSession::Monitor m = monitor(0, 0, 800, 600, 0);
  server.set_display(800, 600, ArrayView<const desktop::IWindowsSession::Monitor>(&m, 1));
  App app(server);
  App::Window w = app.toplevel(true, "menu owner");
  app.show(w, 400, 300, 0xFFFFFFFFu);
  const uint32_t win = listed(server)[0];
  server.move_window(win, {100, 100, 408, 332});      // inside at (104, 128)
  server.activate_window(win);
  // a menu below a button at (20, 30, 60, 20) of the window
  const uint32_t pos = app.new_id();
  app.send(Msg(app.wm, 1).u(pos));
  app.send(Msg(pos, 1).i(150).i(100));
  app.send(Msg(pos, 2).i(20).i(30).i(60).i(20));
  app.send(Msg(pos, 3).u(6));      // anchor bottom_left
  app.send(Msg(pos, 4).u(8));      // gravity bottom_right
  app.send(Msg(pos, 5).u(1 | 2 | 4 | 8));
  const uint32_t ps = app.surface(), pxdg = app.new_id(), popup = app.new_id();
  app.send(Msg(app.wm, 2).u(pxdg).u(ps));
  app.send(Msg(pxdg, 2).u(popup).u(w.xdg).u(pos));
  app.send(Msg(popup, 1).u(app.seat).u(1));      // grab
  app.send(Msg(ps, 6));
  Ev pc = app.take(popup, 0);
  CHECK(pc.object == popup && pc.i(0) == 20 && pc.i(1) == 50 && pc.i(2) == 150 && pc.i(3) == 100);
  Ev px = app.take(pxdg, 0);
  app.send(Msg(pxdg, 4).u(px.u(0)));
  const uint32_t keyboard = app.new_id();
  app.send(Msg(app.seat, 1).u(keyboard));
  app.drop_events();
  App::Window pw;
  pw.surface = ps;
  app.show(pw, 150, 100, 0xFF00FF00u);
  std::vector<uint32_t> list = listed(server);
  CHECK(list.size() == 2);
  if (list.size() != 2) return;
  const uint32_t pwin = list[1];
  desktop::WindowsWindowInfo info;
  String title(A);
  CHECK(server.window_info(pwin, &info, &title) && info.popup && info.owned && !info.caption && info.surface.x == 104 + 20 && info.surface.y == 128 + 50);
  // the grab took the keyboard
  Ev enter = app.take(keyboard, 1);
  CHECK(enter.object == keyboard && enter.u(1) == ps);
  // it moves with its window
  server.move_window(win, {200, 100, 408, 332});
  CHECK(server.window_info(pwin, &info, &title) && info.surface.x == 204 + 20);
  // near the right edge: flipped to the left of the anchor
  server.move_window(win, {400, 100, 408, 332});
  const uint32_t pos2 = app.new_id();
  app.send(Msg(app.wm, 1).u(pos2));
  app.send(Msg(pos2, 1).i(150).i(100));
  app.send(Msg(pos2, 2).i(380).i(30).i(10).i(10));
  app.send(Msg(pos2, 3).u(8));
  app.send(Msg(pos2, 4).u(8));
  app.send(Msg(pos2, 5).u(4));
  app.send(Msg(popup, 2).u(pos2).u(77));
  Ev rep = app.take(popup, 2);
  CHECK(rep.object == popup && rep.u(0) == 77);
  pc = app.take(popup, 0);
  CHECK(pc.object == popup && pc.i(0) == 380 - 150);      // (404 + 390 + 150 would pass 800)
  // a press outside the popup closes it
  server.mouse_position(250, 300);
  server.mouse_button(1, true);
  server.mouse_button(1, false);
  CHECK(app.has(popup, 1));      // popup_done
  CHECK(listed(server).size() == 1);
  // the keyboard is back with the window
  CHECK(app.take(keyboard, 1).u(1) == w.surface);
  // a popup without a parent is an error at its first commit (another protocol may give it one before: zwlr_layer_surface_v1.get_popup)
  const uint32_t s3 = app.surface(), x3 = app.new_id();
  app.send(Msg(app.wm, 2).u(x3).u(s3));
  app.send(Msg(x3, 2).u(app.new_id()).u(0).u(pos));
  CHECK(!app.closing);
  app.send(Msg(s3, 6));
  uint32_t eo = 0, ec = 0;
  CHECK(app.error(&eo, &ec) && eo == app.wm && ec == 3 && app.closing);
}

// ------------------------------------------------------------------ subsurfaces

static void test_subsurfaces() {
  Server server(A);
  server.set_display(800, 600, {});
  App app(server);
  App::Window w = app.toplevel(false, "sub");
  App::Pool main_pool;
  app.show(w, 200, 200, 0xFF000000u, &main_pool);
  const uint32_t win = listed(server)[0];
  // a child at (50, 60), synchronized: shown with the parent's next commit
  const uint32_t child = app.surface(), sub = app.new_id();
  app.send(Msg(app.subcompositor, 1).u(sub).u(child).u(w.surface));
  app.send(Msg(sub, 1).i(50).i(60));
  App::Pool cp = app.pool(40 * 30 * 4);
  App::fill(cp, 0, 40, 30, 160, 0xFFFF0000u);
  const uint32_t cb = app.buffer(cp, 0, 40, 30, 160);
  app.send(Msg(child, 1).u(cb).i(0).i(0));
  app.send(Msg(child, 6));
  auto draw = [&](int32_t x, int32_t y) {
    std::vector<uint32_t> target(200 * 200, 0);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = ts.height = ts.pitch = 200;
    graphics::SoftwareRenderer2D renderer(ts);
    uint32_t dw = 0, dh = 0;
    server.draw_window(win, renderer, 0, 0, &dw, &dh);
    return target[size_t(y) * 200 + size_t(x)];
  };
  CHECK(draw(55, 65) == 0xFF000000u);      // (not yet: synchronized)
  app.send(Msg(w.surface, 6));
  CHECK(draw(55, 65) == 0xFFFF0000u && draw(49, 65) == 0xFF000000u && draw(89, 89) == 0xFFFF0000u && draw(90, 90) == 0xFF000000u);
  // the pointer over the child goes to the child, in its coordinates
  const uint32_t pointer = app.new_id();
  app.send(Msg(app.seat, 0).u(pointer));
  desktop::WindowsWindowInfo info;
  String title(A);
  server.window_info(win, &info, &title);
  app.drop_events();
  server.mouse_position(info.surface.x + 52, info.surface.y + 61);
  Ev e = app.take(pointer, 0);
  CHECK(e.u(1) == child && e.i(2) == 2 * 256 && e.i(3) == 256);
  // desync: a commit shows at once; below the parent: hidden behind it
  app.send(Msg(sub, 5));
  App::fill(cp, 0, 40, 30, 160, 0xFF0000FFu);
  app.send(Msg(child, 2).i(0).i(0).i(40).i(30));
  app.send(Msg(child, 6));
  CHECK(draw(55, 65) == 0xFF0000FFu);
  app.send(Msg(sub, 3).u(w.surface));
  CHECK(draw(55, 65) == 0xFF000000u);
  // a surface cannot be its own parent's parent
  const uint32_t sub2 = app.new_id();
  app.send(Msg(app.subcompositor, 1).u(sub2).u(w.surface).u(child));
  CHECK(app.error());
}

// ------------------------------------------------------------------ the seat

// Touches: a finger over a client with a wl_touch goes to it (down, motion, up, each with a frame, coordinates of the surface it went down on), over a
// client without one it is refused (the desktop moves the pointer), cancel, and a client that goes away loses its contacts.
// Pens (zwp_tablet_v2): a tablet seat is told of a tablet and a pen and an eraser tool; the pen over a client with a tablet seat comes into proximity of
// its surface (motion, pressure, tilt, frame), the tip is down / up, the barrel buttons are BTN_STYLUS; while the tip touches the surface keeps the pen;
// over a client without a tablet seat it is refused; out of range, proximity_out.
static void test_pen() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.set_display(1000, 800, {});
  App a(server), b(server);
  App::Window wa = a.toplevel(false, "a"), wb = b.toplevel(false, "b");
  a.show(wa, 300, 300, 0xFF111111u);
  b.show(wb, 300, 300, 0xFF222222u);
  std::vector<uint32_t> list = listed(server);
  CHECK(list.size() == 2);
  if (list.size() != 2) return;
  server.move_window(list[0], {0, 0, 300, 300});
  server.move_window(list[1], {400, 0, 300, 300});
  const uint32_t manager = a.bind(a.reg, "zwp_tablet_manager_v2");
  const uint32_t tseat = a.new_id();
  a.send(Msg(manager, 0).u(tseat).u(a.seat));
  // the tablet and its two tools, each described and done
  Ev tablet_added = a.take(tseat, 0);
  const uint32_t tablet = tablet_added.u(0);
  CHECK(tablet >= 0xFF000000u && a.take(tablet, 0).s(0) == "TheoryCraftOS pen" && a.has(tablet, 3));
  Ev t1 = a.take(tseat, 1), t2 = a.take(tseat, 1);
  const uint32_t pen = t1.u(0), eraser = t2.u(0);
  CHECK(pen && eraser && pen != eraser);
  CHECK(a.take(pen, 0).u(0) == 0x140 && a.take(eraser, 0).u(0) == 0x141 && a.has(pen, 4) && a.has(eraser, 4));
  a.drop_events();
  // hovering over a: proximity_in (serial, tablet, surface), motion, pressure 0, tilt, frame
  desktop::PenInput p;
  p.in_range = true;
  p.x = 30, p.y = 40;
  p.tilt_x = 10, p.tilt_y = -5;
  CHECK(server.pen(p));
  Ev in = a.take(pen, 6);
  CHECK(in.object == pen && in.u(1) == tablet && in.u(2) == wa.surface);
  Ev motion = a.take(pen, 10);
  CHECK(motion.i(0) == 30 * 256 && motion.i(1) == 40 * 256);
  CHECK(a.take(pen, 11).u(0) == 0);
  Ev tilt = a.take(pen, 13);
  CHECK(tilt.i(0) == 10 * 256 && tilt.i(1) == -5 * 256);
  CHECK(a.has(pen, 18));
  // the tip touches with pressure; the barrel button
  p.tip = true;
  p.pressure = 40000;
  p.buttons = hardware::pen_button::kBarrel;
  a.drop_events();
  CHECK(server.pen(p));
  CHECK(a.has(pen, 8) && a.take(pen, 11).u(0) == 40000);
  Ev button = a.take(pen, 17);
  CHECK(button.u(1) == 0x14B && button.u(2) == 1);
  // dragged over b while touching: still a's surface, in a's coordinates
  a.drop_events();
  p.x = 450;
  CHECK(server.pen(p));
  Ev drag = a.take(pen, 10);
  CHECK(drag.object == pen && drag.i(0) == 450 * 256);
  CHECK(!a.has(pen, 7));
  // lifted over b: up, then (b reads no pens) refused, and a hears proximity_out
  a.drop_events();
  p.tip = false;
  p.buttons = 0;
  p.pressure = 0;
  CHECK(!server.pen(p));
  CHECK(a.has(pen, 7));
  // back over a, then out of range: proximity_out
  p.x = 50;
  CHECK(server.pen(p));
  a.drop_events();
  p.in_range = false;
  CHECK(server.pen(p));
  CHECK(a.has(pen, 7));
  CHECK(!server.pen(p));      // (already out)
  // the eraser end: the eraser tool
  p.in_range = true;
  p.inverted = true;
  a.drop_events();
  CHECK(server.pen(p));
  CHECK(a.take(eraser, 6).u(2) == wa.surface && !a.has(pen, 6));
  // nowhere: refused (and the eraser leaves)
  p.x = 900, p.y = 700;
  a.drop_events();
  CHECK(!server.pen(p));
  CHECK(a.has(eraser, 7));
  free_made();
}

static void test_touch() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.set_display(1000, 800, {});
  App a(server), b(server);
  App::Window wa = a.toplevel(false, "a"), wb = b.toplevel(false, "b");
  a.show(wa, 300, 300, 0xFF111111u);
  b.show(wb, 300, 300, 0xFF222222u);
  std::vector<uint32_t> list = listed(server);
  CHECK(list.size() == 2);
  if (list.size() != 2) return;
  server.move_window(list[0], {0, 0, 300, 300});
  server.move_window(list[1], {400, 0, 300, 300});
  const uint32_t ta = a.new_id();
  a.send(Msg(a.seat, 2).u(ta));      // get_touch: only a reads touches
  a.drop_events();
  b.drop_events();
  // a finger on a: taken, down with a's surface and the place in it
  CHECK(server.touch(hardware::TouchPhase::Down, 5, list[0], 30, 40));
  Ev down = a.take(ta, 0);
  CHECK(down.object == ta && down.u(2) == wa.surface && down.i(3) == 5 && down.i(4) == 30 * 256 && down.i(5) == 40 * 256);
  CHECK(a.has(ta, 3));      // frame
  // it moves out of a's window: still a's, in a's coordinates
  CHECK(server.touch(hardware::TouchPhase::Motion, 5, 0, 350, 45));
  Ev motion = a.take(ta, 2);
  CHECK(motion.object == ta && motion.i(1) == 5 && motion.i(2) == 350 * 256 && motion.i(3) == 45 * 256);
  // a second finger on b: b has no wl_touch, refused (and b hears nothing)
  CHECK(!server.touch(hardware::TouchPhase::Down, 6, list[1], 420, 10));
  CHECK(!server.touch(hardware::TouchPhase::Motion, 6, 0, 421, 10));      // (and its motion is nobody's)
  // up: a's touch ends
  CHECK(server.touch(hardware::TouchPhase::Up, 5, 0, 350, 45));
  Ev up = a.take(ta, 1);
  CHECK(up.object == ta && up.i(2) == 5);
  CHECK(!server.touch(hardware::TouchPhase::Up, 5, 0, 350, 45));      // (no longer anyone's)
  // nowhere: refused
  CHECK(!server.touch(hardware::TouchPhase::Down, 7, 0, 900, 700));
  // cancel: a hears cancel, and its contacts are gone
  CHECK(server.touch(hardware::TouchPhase::Down, 8, list[0], 10, 10));
  CHECK(server.touch(hardware::TouchPhase::Down, 9, list[0], 20, 20));
  a.drop_events();
  CHECK(server.touch(hardware::TouchPhase::Cancel, 8, 0, 10, 10));
  CHECK(a.has(ta, 4));
  CHECK(!server.touch(hardware::TouchPhase::Motion, 9, 0, 21, 20));      // (the protocol cancels all of a's)
  // a client that goes away mid-touch: its contacts are nobody's
  CHECK(server.touch(hardware::TouchPhase::Down, 10, list[0], 10, 10));
  server.close_client(a.number);
  a.number = 0;
  CHECK(!server.touch(hardware::TouchPhase::Motion, 10, 0, 11, 10));
  free_made();
}


static void test_seat() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.set_display(1000, 800, {});
  App a(server), b(server);
  App::Window wa = a.toplevel(false, "a"), wb = b.toplevel(false, "b");
  a.show(wa, 300, 300, 0xFF111111u);
  b.show(wb, 300, 300, 0xFF222222u);
  std::vector<uint32_t> list = listed(server);
  CHECK(list.size() == 2);
  if (list.size() != 2) return;
  server.move_window(list[0], {0, 0, 300, 300});
  server.move_window(list[1], {400, 0, 300, 300});
  const uint32_t pa = a.new_id(), ka = a.new_id(), pb = b.new_id(), kb = b.new_id();
  a.send(Msg(a.seat, 0).u(pa));
  a.send(Msg(a.seat, 1).u(ka));
  b.send(Msg(b.seat, 0).u(pb));
  b.send(Msg(b.seat, 1).u(kb));
  // the keymap: an xkb_v1 text with its NUL, in a descriptor that came with it; then the repeat rate
  Ev km = a.take(ka, 0);
  CHECK(km.object == ka && km.u(0) == 1 && !a.fds.empty());
  if (!a.fds.empty()) {
    ByteView text = a.fds[0]->map(km.u(1));
    CHECK(text.size() >= km.u(1) && text[km.u(1) - 1] == 0 && std::string(reinterpret_cast<const char*>(text.data())).find("xkb_symbols") != std::string::npos);
  }
  Ev rep = a.take(ka, 5);
  CHECK(rep.i(0) == 25 && rep.i(1) == 600);
  CHECK(Server::evdev_code(0x04) == 30 && Server::evdev_code(0x28) == 28 && Server::evdev_code(0xE1) == 42 && Server::evdev_code(0x00) == 0);
  a.drop_events();
  b.drop_events();
  // the pointer over a's window: enter, motion, frame
  server.mouse_position(10, 20);
  Ev enter = a.take(pa, 0);
  CHECK(enter.object == pa && enter.u(1) == wa.surface && enter.i(2) == 10 * 256 && enter.i(3) == 20 * 256);
  CHECK(a.has(pa, 5));
  server.mouse_position(12, 20);
  Ev motion = a.take(pa, 2);
  CHECK(motion.object == pa && motion.i(1) == 12 * 256);
  // over to b: a's leave, b's enter
  server.mouse_position(410, 30);
  CHECK(a.take(pa, 1).u(1) == wa.surface);
  CHECK(b.take(pb, 0).u(1) == wb.surface);
  // a press and a drag out of b's window: the implicit grab keeps b's focus, with coordinates outside its surface
  server.mouse_button(1, true);
  Ev press = b.take(pb, 3);
  CHECK(press.object == pb && press.u(2) == 0x110 && press.u(3) == 1);
  CHECK(server.capture_window() == list[1]);
  server.mouse_position(100, 30);
  Ev drag = b.take(pb, 2);
  CHECK(drag.object == pb && drag.i(1) == -300 * 256);
  CHECK(!a.has(pa, 0));
  server.mouse_button(1, false);
  CHECK(b.take(pb, 3).u(3) == 0);
  CHECK(b.has(pb, 1) && a.has(pa, 0));      // (the release ends the grab: the focus moves to what is under the pointer)
  CHECK(server.capture_window() == 0);
  // the right and middle buttons, the wheel
  a.drop_events();
  server.mouse_button(2, true);
  CHECK(a.take(pa, 3).u(2) == 0x111);
  server.mouse_button(2, false);
  a.drop_events();
  server.mouse_button(3, true);
  CHECK(a.take(pa, 3).u(2) == 0x112);
  server.mouse_button(3, false);
  server.mouse_wheel(1);
  CHECK(a.take(pa, 6).u(0) == 0);
  CHECK(a.take(pa, 9).i(1) == -120);
  CHECK(a.take(pa, 4).i(2) == -15 * 256);
  // the cursor a client sets
  desktop::IWindowsSession::Pointer ptr;
  server.pointer(&ptr);
  CHECK(!ptr.hidden && ptr.cursor == 0 && ptr.system_id == 32512);
  const uint32_t cs = a.surface();
  App::Pool cp = a.pool(16 * 16 * 4);
  App::fill(cp, 0, 16, 16, 64, 0x80402010u);      // premultiplied, half transparent
  const uint32_t cbuf = a.buffer(cp, 0, 16, 16, 64, 0);
  a.send(Msg(cs, 1).u(cbuf).i(0).i(0));
  a.send(Msg(cs, 6));
  a.send(Msg(pa, 0).u(enter.u(0)).u(cs).i(3).i(4));
  server.pointer(&ptr);
  CHECK(!ptr.hidden && ptr.cursor != 0);
  uint32_t sys = 0, cw = 0, ch = 0;
  int32_t hx = 0, hy = 0;
  List<uint32_t> argb(A);
  CHECK(server.cursor_picture(ptr.cursor, &sys, &argb, &cw, &ch, &hx, &hy) && cw == 16 && ch == 16 && hx == 3 && hy == 4);
  CHECK(argb.count() == 256 && argb[0] == 0x80804020u);      // (straight alpha)
  a.send(Msg(pa, 0).u(enter.u(0)).u(0).i(0).i(0));
  server.pointer(&ptr);
  CHECK(ptr.hidden);
  // b cannot set a's cursor (the pointer is not over b)
  b.send(Msg(pb, 0).u(1).u(0).i(0).i(0));
  server.pointer(&ptr);
  CHECK(ptr.hidden);
  // the pointer leaves for a desktop window
  a.drop_events();
  server.mouse_left();
  CHECK(a.has(pa, 1));
  // the keyboard: the active window's client
  server.activate_window(list[0]);
  Ev kenter = a.take(ka, 1);
  CHECK(kenter.object == ka && kenter.u(1) == wa.surface);
  Ev mods = a.take(ka, 4);
  CHECK(mods.object == ka && mods.u(1) == 0);
  server.key(0xE1, true, hardware::modifier::kLeftShift);      // shift down
  Ev k1 = a.take(ka, 3);
  CHECK(k1.u(2) == 42 && k1.u(3) == 1);
  CHECK(a.take(ka, 4).u(1) == 1);      // Shift
  server.key(0x04, true, hardware::modifier::kLeftShift);
  CHECK(a.take(ka, 3).u(2) == 30);
  server.key(0x04, true, hardware::modifier::kLeftShift);      // (the desktop's repeat: dropped, the client repeats itself)
  CHECK(!a.has(ka, 3));
  server.key(0x04, false, hardware::modifier::kLeftShift);
  CHECK(a.take(ka, 3).u(3) == 0);
  server.key(0xE1, false, 0);
  a.take(ka, 3);
  CHECK(a.take(ka, 4).u(1) == 0);
  server.key(0x39, true, 0);      // caps lock: locked Lock
  CHECK(a.take(ka, 4).u(3) == 2);
  server.key(0x39, false, 0);
  CHECK(!b.has(kb, 3));
  // a key held while the focus moves comes with the enter
  server.key(0x05, true, 0);
  server.activate_window(list[1]);
  CHECK(a.take(ka, 2).u(1) == wa.surface);
  Ev be = b.take(kb, 1);
  CHECK(be.object == kb && be.u(2) == 4 && be.u(3) == 48);
  CHECK(b.take(kb, 4).u(3) == 2);      // (caps lock is still on)
  server.key(0x05, false, 0);
  CHECK(b.take(kb, 3).u(2) == 48);
  // no window active: nobody has the keyboard
  server.activate_window(0);
  CHECK(b.take(kb, 2).u(1) == wb.surface);
  free_made();
}

// ------------------------------------------------------------------ protocol errors

static void test_errors() {
  Server server(A);
  server.set_display(800, 600, {});
  auto expect_error = [&](const std::function<void(App&)>& f, uint32_t want_code, int line) {
    App app(server);
    f(app);
    uint32_t object = 0, code = 0;
    const bool got = app.error(&object, &code);
    if (!got || code != want_code || !app.closing) {
      printf("FAILED line %d: expected error %u, got %d (%u)\n", line, want_code, got, code);
      ++g_failed;
    }
  };
  // an unknown object, an unknown request
  expect_error([](App& a) { a.send(Msg(999, 0)); }, 0, __LINE__);
  expect_error([](App& a) { a.send(Msg(a.compositor, 7).u(a.new_id())); }, 1, __LINE__);
  // a new id that is in use
  expect_error([](App& a) { a.send(Msg(a.compositor, 0).u(a.shm)); }, 0, __LINE__);
  // shm: a descriptor that is not memory, a pool too big for it, a missing descriptor, bad buffers
  expect_error([](App& a) {
    auto* v = new std::vector<uint8_t>(100);
    a.memories.push_back(v);
    Fd fd = Fd::make_as<TestMemory>(A, v);
    a.send(Msg(a.shm, 0).u(a.new_id()).i(4096), {fd});
  }, 2, __LINE__);
  expect_error([](App& a) { a.send(Msg(a.shm, 0).u(a.new_id()).i(4096)); }, 1, __LINE__);      // (no descriptor came: malformed)
  expect_error([](App& a) { App::Pool p = a.pool(4096); a.buffer(p, 0, 16, 16, 64, 12345); }, 0, __LINE__);
  expect_error([](App& a) { App::Pool p = a.pool(4096); a.buffer(p, 0, 33, 16, 128); }, 1, __LINE__);      // (33 * 16 rows of 128 > 4096)
  expect_error([](App& a) { App::Pool p = a.pool(4096); a.buffer(p, 2, 16, 16, 64); }, 1, __LINE__);
  expect_error([](App& a) { App::Pool p = a.pool(4096); a.buffer(p, 0, 16, 16, 60); }, 1, __LINE__);
  expect_error([](App& a) { App::Pool p = a.pool(4096); a.buffer(p, 0, -1, 16, 64); }, 1, __LINE__);
  expect_error([](App& a) { App::Pool p = a.pool(4096); a.buffer(p, 0x7FFFFFFC, 1, 1, 4); }, 1, __LINE__);
  expect_error([](App& a) { App::Pool p = a.pool(8192); a.send(Msg(p.id, 2).i(4096)); }, 1, __LINE__);      // (a pool cannot shrink)
  // xdg: a second role, a buffer before the first configure, a bad geometry, an incomplete positioner
  expect_error([](App& a) {
    const uint32_t s = a.surface(), x = a.new_id();
    a.send(Msg(a.subcompositor, 1).u(a.new_id()).u(s).u(a.surface()));
    a.send(Msg(a.wm, 2).u(x).u(s));
  }, 0, __LINE__);
  expect_error([](App& a) {
    const uint32_t s = a.surface(), x = a.new_id(), t = a.new_id();
    a.send(Msg(a.wm, 2).u(x).u(s));
    a.send(Msg(x, 1).u(t));
    App::Pool p = a.pool(4096);
    a.send(Msg(s, 1).u(a.buffer(p, 0, 16, 16, 64)).i(0).i(0));
    a.send(Msg(s, 6));
  }, 3, __LINE__);
  expect_error([](App& a) { App::Window w = a.toplevel(); a.send(Msg(w.xdg, 3).i(0).i(0).i(0).i(10)); }, 5, __LINE__);
  expect_error([](App& a) {
    App::Window w = a.toplevel();
    const uint32_t pos = a.new_id(), s = a.surface(), x = a.new_id();
    a.send(Msg(a.wm, 1).u(pos));
    a.send(Msg(a.wm, 2).u(x).u(s));
    a.send(Msg(x, 2).u(a.new_id()).u(w.xdg).u(pos));
  }, 5, __LINE__);
  expect_error([](App& a) { const uint32_t pos = a.new_id(); a.send(Msg(a.wm, 1).u(pos)); a.send(Msg(pos, 1).i(0).i(5)); }, 0, __LINE__);
  expect_error([](App& a) { App::Window w = a.toplevel(); a.send(Msg(a.deco, 1).u(a.new_id()).u(w.toplevel)); a.send(Msg(a.deco, 1).u(a.new_id()).u(w.toplevel)); }, 1, __LINE__);
  expect_error([](App& a) { a.send(Msg(a.surface(), 8).i(0)); }, 0, __LINE__);
  // a malformed string, a bad message size
  expect_error([](App& a) {
    Msg m(a.reg, 0);
    m.u(1).u(8);
    for (int k = 0; k < 8; ++k) m.b.push_back('x');      // (no NUL)
    m.u(1).u(a.new_id());
    a.send(m);
  }, 1, __LINE__);
  expect_error([](App& a) {
    std::vector<uint8_t> raw = {1, 0, 0, 0, 0, 0, 6, 0};      // size 6
    a.send(raw);
  }, 1, __LINE__);
  CHECK(server.client_count() == 0);
}

// ------------------------------------------------------------------ truncated and bit-flipped streams

static void test_fuzz() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(800, 600, {});
  // a valid session's bytes (recorded from what a client sends)
  std::vector<std::vector<uint8_t>> messages;
  std::vector<int> fd_at;      // which messages carry a descriptor
  {
    auto add = [&](Msg m, bool fd = false) { messages.push_back(m.done()); if (fd) fd_at.push_back(int(messages.size()) - 1); };
    add(Msg(1, 1).u(2));
    add(Msg(2, 0).u(1).s("wl_compositor").u(6).u(3));
    add(Msg(2, 0).u(3).s("wl_shm").u(2).u(4));
    add(Msg(2, 0).u(7).s("xdg_wm_base").u(5).u(5));
    add(Msg(2, 0).u(4).s("wl_seat").u(8).u(6));
    add(Msg(2, 0).u(8).s("zxdg_decoration_manager_v1").u(1).u(7));
    add(Msg(2, 0).u(2).s("wl_subcompositor").u(1).u(8));
    add(Msg(3, 0).u(10));
    add(Msg(5, 2).u(11).u(10));
    add(Msg(11, 1).u(12));
    add(Msg(7, 1).u(13).u(12));
    add(Msg(12, 2).s("fuzz"));
    add(Msg(10, 6));
    add(Msg(11, 4).u(1));
    add(Msg(4, 0).u(14).i(65536), true);
    add(Msg(14, 0).u(15).i(0).i(64).i(64).i(256).u(0));
    add(Msg(10, 1).u(15).i(0).i(0));
    add(Msg(10, 2).i(0).i(0).i(64).i(64));
    add(Msg(10, 3).u(16));
    add(Msg(10, 6));
    add(Msg(6, 0).u(17));
    add(Msg(6, 1).u(18));
    add(Msg(3, 0).u(19));
    add(Msg(8, 1).u(20).u(19).u(10));
    add(Msg(20, 1).i(5).i(5));
    add(Msg(3, 1).u(21));
    add(Msg(21, 1).i(0).i(0).i(10).i(10));
    add(Msg(19, 5).u(21));
    add(Msg(19, 6));
    add(Msg(5, 1).u(22));
    add(Msg(22, 1).i(10).i(10));
    add(Msg(22, 2).i(0).i(0).i(5).i(5));
    add(Msg(3, 0).u(23));
    add(Msg(5, 2).u(24).u(23));
    add(Msg(24, 2).u(25).u(11).u(22));
    add(Msg(23, 6));
    add(Msg(12, 9));
    add(Msg(10, 6));
    add(Msg(1, 0).u(26));
  }
  std::vector<uint8_t> all;
  std::vector<size_t> fd_offsets;
  for (size_t k = 0; k < messages.size(); ++k) {
    for (int f : fd_at) if (size_t(f) == k) fd_offsets.push_back(all.size());
    all.insert(all.end(), messages[k].begin(), messages[k].end());
  }
  std::vector<uint8_t> memory(65536, 0x55);
  auto run = [&](const std::vector<uint8_t>& bytes, size_t chunk, bool with_fd) {
    const uint32_t c = server.open_client(1);
    if (!c) { ++g_failed; return; }
    size_t at = 0;
    bool fd_sent = !with_fd;
    while (at < bytes.size()) {
      const size_t n = std::min(chunk, bytes.size() - at);
      std::vector<Fd> fds;
      if (!fd_sent) { fds.push_back(Fd::make_as<TestMemory>(A, &memory)); fd_sent = true; }
      server.receive(c, ConstByteView(bytes.data() + at, n), ArrayView<Fd>(fds.data(), fds.size()));
      at += n;
      ByteBuffer out(A);
      List<Fd> fo(A);
      if (server.take_output(c, &out, &fo)) break;
    }
    // the desktop looks at whatever came of it
    List<uint32_t> list(A), changed(A);
    List<desktop::IWindowsSession::Repaint> rep(A);
    server.windows(&list);
    server.take_changes(&changed, &rep);
    std::vector<uint32_t> target(64 * 64);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = ts.height = ts.pitch = 64;
    for (uint32_t w : list.view()) {
      desktop::WindowsWindowInfo info;
      String title(A);
      server.window_info(w, &info, &title);
      graphics::SoftwareRenderer2D renderer(ts);
      uint32_t dw, dh;
      server.draw_window(w, renderer, 0, 0, &dw, &dh);
      server.move_window(w, {0, 0, 100, 100});
      server.activate_window(w);
    }
    server.mouse_position(30, 40);
    server.mouse_button(1, true);
    server.key(0x04, true, 0);
    server.mouse_button(1, false);
    server.key(0x04, false, 0);
    server.close_client(c);
  };
  // whole, and in pieces of every size up to 64 bytes
  run(all, all.size(), true);
  for (size_t chunk = 1; chunk <= 64; ++chunk) run(all, chunk, true);
  // every prefix
  for (size_t cut = 0; cut < all.size(); cut += 4) run(std::vector<uint8_t>(all.begin(), all.begin() + long(cut)), 4096, true);
  // bit flips, with and without the descriptor
  std::mt19937 rng(1234);
  for (int round = 0; round < 3000; ++round) {
    std::vector<uint8_t> v = all;
    const int flips = 1 + int(rng() % 4);
    for (int f = 0; f < flips; ++f) v[rng() % v.size()] ^= uint8_t(1u << (rng() % 8));
    run(v, 1 + rng() % 200, rng() % 4 != 0);
  }
  // random words
  for (int round = 0; round < 500; ++round) {
    std::vector<uint8_t> v = all;
    for (int f = 0; f < 6; ++f) {
      const size_t at = (rng() % (v.size() / 4)) * 4;
      const uint32_t word = rng() % 3 == 0 ? rng() : rng() % 40;
      memcpy(&v[at], &word, 4);
    }
    run(v, 4096, true);
  }
  CHECK(server.client_count() == 0);
  free_made();
}

// ------------------------------------------------------------------ the AF_UNIX transport, descriptors as SCM_RIGHTS

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

// A Linux program's end of a connection: bytes and, with them, descriptors (open objects of the process's table, as sendmsg puts them in flight).
struct SocketClient {
  process::UnixSocketManager& m;
  uint32_t id = 0;
  std::vector<uint8_t> in;
  std::vector<SharedPointer<process::OpenObject>> received;
  explicit SocketClient(process::UnixSocketManager& manager) : m(manager) {}
  ~SocketClient() { if (id) m.release(id); }
  bool connect() {
    if (m.create(process::UnixType::Stream, 42, &id) != process::UnixStatus::Ok) return false;
    const std::string name = "/run/user/0/wayland-0";
    const process::UnixName un = process::UnixName::from(process::UnixName::Kind::Path, ConstByteView(reinterpret_cast<const uint8_t*>(name.data()), name.size()));
    m.sync().lock();
    const process::UnixStatus st = m.connect_locked(id, un, "/Linux/run/user/0/wayland-0", 42);
    m.sync().unlock();
    return st == process::UnixStatus::Ok;
  }
  bool write(const std::vector<uint8_t>& bytes, SharedPointer<process::OpenObject> fd = SharedPointer<process::OpenObject>()) {
    Queue<process::UnixInFlight> rights(A);
    if (fd) {
      process::UnixInFlight e;
      e.kind = process::DescriptorKind::File;
      e.object = fd;
      rights.enqueue(e);
    }
    size_t at = 0;
    while (at < bytes.size()) {
      m.sync().lock();
      uint32_t sent = 0;
      const process::UnixStatus st = m.send_locked(id, ConstByteView(bytes.data() + at, bytes.size() - at), nullptr, 42, rights.count() ? &rights : nullptr, &sent);
      if (st == process::UnixStatus::WouldBlock) m.sync().wait(10'000'000);
      m.sync().unlock();
      if (st != process::UnixStatus::Ok && st != process::UnixStatus::WouldBlock) return false;
      at += st == process::UnixStatus::Ok ? sent : 0;
    }
    return true;
  }
  bool read(size_t n, int ms = 5000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    uint8_t buf[4096];
    while (in.size() < n) {
      if (std::chrono::steady_clock::now() > until) return false;
      Queue<process::UnixInFlight> rights(A);
      m.sync().lock();
      process::UnixSocketManager::Received got;
      const process::UnixStatus st = m.receive_locked(id, ByteView(buf, sizeof buf), false, &got, &rights);
      if (st == process::UnixStatus::WouldBlock) m.sync().wait(10'000'000);
      m.sync().unlock();
      process::UnixInFlight e;
      while (rights.dequeue(&e)) received.push_back(e.object);
      if (st == process::UnixStatus::Ok) {
        if (got.end_of_file) return false;
        in.insert(in.end(), buf, buf + got.got);
      } else if (st != process::UnixStatus::WouldBlock) {
        return false;
      }
    }
    return true;
  }
  // The next event of that object and opcode (the others before it are dropped).
  Ev next(uint32_t object, uint16_t opcode) {
    for (;;) {
      if (!read(8)) return Ev();
      const uint32_t size = uint32_t(in[6] | in[7] << 8);
      if (size < 8 || !read(size)) return Ev();
      Ev e;
      e.object = uint32_t(in[0] | in[1] << 8 | in[2] << 16 | uint32_t(in[3]) << 24);
      e.opcode = uint16_t(in[4] | in[5] << 8);
      e.a.assign(in.begin() + 8, in.begin() + size);
      in.erase(in.begin(), in.begin() + size);
      if (e.object == object && e.opcode == opcode) return e;
    }
  }
};

// A file in memory, as memfd_create makes it, as an open object.
static SharedPointer<process::OpenObject> memfd(uint64_t size) {
  SharedPointer<process::OpenObject> o = SharedPointer<process::OpenObject>::make(A, A);
  UniquePointer<process::MemoryFile> f = UniquePointer<process::MemoryFile>::make(A, A, true);
  f->resize(size);
  o->memory = f.get();
  o->file = util::move(f);
  o->readable = o->writable = true;
  return o;
}

static void test_socket_transport() {
  SystemSync sync;
  process::UnixSocketManager sockets(sync, A);
  Server server(A);
  server.set_display(800, 600, {});
  filesystem::RamFileSystem files(A);
  {
    wayland::SocketServer transport(server, sockets, A);
    CHECK(transport.listen(files));
    filesystem::DirectoryEntry entry;
    CHECK(files.stat("/Linux/run/user/0/wayland-0", &entry) == filesystem::Status::Ok);
    std::atomic<bool> stop{false};
    std::thread pump([&] {
      while (!stop) {
        while (transport.pump()) {}
        transport.wait(5'000'000);
      }
    });
    SocketClient c(sockets);
    CHECK(c.connect());
    std::vector<uint8_t> batch;
    auto add = [&](Msg m) { std::vector<uint8_t> v = m.done(); batch.insert(batch.end(), v.begin(), v.end()); };
    add(Msg(1, 1).u(2));
    add(Msg(2, 0).u(1).s("wl_compositor").u(6).u(3));
    add(Msg(2, 0).u(3).s("wl_shm").u(1).u(4));
    add(Msg(2, 0).u(7).s("xdg_wm_base").u(5).u(5));
    add(Msg(2, 0).u(4).s("wl_seat").u(8).u(6));
    add(Msg(6, 1).u(7));      // a keyboard: its keymap comes as a descriptor
    add(Msg(3, 0).u(10));
    add(Msg(5, 2).u(11).u(10));
    add(Msg(11, 1).u(12));
    add(Msg(10, 6));
    CHECK(c.write(batch));
    Ev km = c.next(7, 0);
    CHECK(km.object == 7 && km.u(0) == 1);
    CHECK(c.received.size() == 1 && c.received[0] && c.received[0]->memory);
    if (!c.received.empty() && c.received[0] && c.received[0]->memory) {      // the keymap file: sealed, readable as the program would map it
      uint64_t size = 0;
      process::BlockRef b = c.received[0]->memory->block(&size);
      CHECK(b && size == km.u(1) && std::string(reinterpret_cast<const char*>(b.get()->bytes().data())).find("xkb_keycodes") != std::string::npos);
      CHECK((c.received[0]->memory->seals() & process::MemoryFile::kSealWrite) != 0);
    }
    Ev conf = c.next(11, 0);
    CHECK(conf.object == 11);
    // a pool in a file in memory, passed with SCM_RIGHTS; the picture is the client's memory
    SharedPointer<process::OpenObject> shm = memfd(64 * 64 * 4);
    batch.clear();
    add(Msg(11, 4).u(conf.u(0)));
    CHECK(c.write(batch));
    CHECK(c.write(Msg(4, 0).u(13).i(64 * 64 * 4).done(), shm));
    {
      uint64_t size = 0;
      process::BlockRef b = shm->memory->block(&size);
      for (uint64_t k = 0; k < size; k += 4) { const uint32_t v = 0xFF123456u; memcpy(b.get()->bytes().data() + k, &v, 4); }
    }
    batch.clear();
    add(Msg(13, 0).u(14).i(0).i(64).i(64).i(256).u(1));
    add(Msg(10, 1).u(14).i(0).i(0));
    add(Msg(10, 6));
    add(Msg(1, 0).u(15));
    CHECK(c.write(batch));
    CHECK(c.next(15, 0).object == 15);
    std::vector<uint32_t> list = listed(server);
    CHECK(list.size() == 1);
    if (!list.empty()) {
      CHECK(server.window_pixel(list[0], 3, 3) == 0xFF123456u);
      // the client writes into its memory: the server sees it in place (no copy)
      uint64_t size = 0;
      process::BlockRef b = shm->memory->block(&size);
      const uint32_t v = 0xFFABCDEFu;
      memcpy(b.get()->bytes().data() + (3 * 64 + 3) * 4, &v, 4);
      CHECK(server.window_pixel(list[0], 3, 3) == 0xFFABCDEFu);
    }
    // a descriptor that is not a file in memory: invalid_fd
    SocketClient d(sockets);
    CHECK(d.connect());
    batch.clear();
    add(Msg(1, 1).u(2));
    add(Msg(2, 0).u(3).s("wl_shm").u(1).u(3));
    CHECK(d.write(batch));
    SharedPointer<process::OpenObject> plain = SharedPointer<process::OpenObject>::make(A, A);
    CHECK(d.write(Msg(3, 0).u(4).i(4096).done(), plain));
    Ev err = d.next(1, 0);
    CHECK(err.object == 1 && err.u(1) == 2);
    stop = true;
    pump.join();
  }
  CHECK(server.client_count() == 0);
}

// Several clients at once on their own threads, the transport on its thread and the desktop on another: windows made, configured, drawn into, shown,
// moved, activated, input delivered, frame callbacks answered by the desktop's drawing, everything torn down.
static void test_threads() {
  SystemSync sync;
  process::UnixSocketManager sockets(sync, A);
  Server server(A);
  server.attach(4, 24);
  server.set_display(1280, 800, {});
  filesystem::RamFileSystem files(A);
  wayland::SocketServer transport(server, sockets, A);
  CHECK(transport.listen(files));
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
    std::vector<uint32_t> target(256 * 256);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = ts.height = ts.pitch = 256;
    uint32_t n = 0;
    while (!stop) {
      server.take_changes(&changed, &repainted);
      server.windows(&list);
      for (uint32_t w : list.view()) {
        if (server.window_info(w, &info, &title) && (n % 7) == 0) server.move_window(w, {info.window.x + 1, info.window.y, info.window.width, info.window.height});
        if ((n % 5) == 0) server.activate_window(w);
        graphics::SoftwareRenderer2D renderer(ts);
        uint32_t dw = 0, dh = 0;
        server.draw_window(w, renderer, 0, 0, &dw, &dh);      // (answers the frame callbacks)
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
      SocketClient c(sockets);
      if (!c.connect()) return;
      std::vector<uint8_t> batch;
      auto add = [&](Msg m) { std::vector<uint8_t> v = m.done(); batch.insert(batch.end(), v.begin(), v.end()); };
      add(Msg(1, 1).u(2));
      add(Msg(2, 0).u(1).s("wl_compositor").u(6).u(3));
      add(Msg(2, 0).u(3).s("wl_shm").u(1).u(4));
      add(Msg(2, 0).u(7).s("xdg_wm_base").u(5).u(5));
      add(Msg(2, 0).u(4).s("wl_seat").u(8).u(6));
      add(Msg(6, 0).u(7));
      add(Msg(6, 1).u(8));
      if (!c.write(batch)) return;
      SharedPointer<process::OpenObject> shm = memfd(128 * 128 * 4);
      if (!c.write(Msg(4, 0).u(9).i(128 * 128 * 4).done(), shm)) return;
      bool ok = true;
      uint32_t next = 100;
      for (int round = 0; round < 12 && ok; ++round) {
        const uint32_t s = next++, x = next++, t = next++, b = next++, cb = next++, sync_cb = next++;
        batch.clear();
        add(Msg(3, 0).u(s));
        add(Msg(5, 2).u(x).u(s));
        add(Msg(x, 1).u(t));
        add(Msg(t, 2).s("thread " + std::to_string(k)));
        add(Msg(s, 6));
        if (!c.write(batch)) { ok = false; break; }
        Ev conf = c.next(x, 0);
        if (conf.object != x) { ok = false; break; }
        batch.clear();
        add(Msg(x, 4).u(conf.u(0)));
        add(Msg(9, 0).u(b).i(0).i(128).i(128).i(512).u(1));
        add(Msg(s, 1).u(b).i(0).i(0));
        add(Msg(s, 2).i(0).i(0).i(128).i(128));
        add(Msg(s, 3).u(cb));
        add(Msg(s, 6));
        if (!c.write(batch)) { ok = false; break; }
        // the desktop draws it and answers (unless it went away first; then the round trip says it was carried out)
        Ev done = c.next(cb, 0);
        ok = done.object == cb;
        batch.clear();
        add(Msg(t, 0));
        add(Msg(x, 0));
        add(Msg(s, 0));
        add(Msg(b, 0));
        add(Msg(1, 0).u(sync_cb));
        if (!c.write(batch)) { ok = false; break; }
        if (c.next(sync_cb, 0).object != sync_cb) ok = false;
      }
      if (ok) ++good;
    });
  }
  for (std::thread& t : clients) t.join();
  for (int i = 0; i < 500 && server.client_count() != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  stop = true;
  pump.join();
  desktop_thread.join();
  CHECK(good == kClients);
  CHECK(server.client_count() == 0);
  CHECK(listed(server).empty());
}

// ------------------------------------------------------------------ zwp_linux_dmabuf_v1 (a stand-in GPU image; the real one is test_dxg_* below)

// A GPU image as the transport would make one: its picture is in `pixels` (0xAARRGGBB); `busy` makes draw and read fail as a GPU still working on it would.
struct FakeGpu final : wayland::Memory, wayland::GpuImage {
  uint32_t w, h;
  std::vector<uint32_t> pixels;
  bool busy = false;
  int draws = 0;
  FakeGpu(uint32_t width, uint32_t height, uint32_t color) : w(width), h(height), pixels(size_t(width) * height, color) {}
  ByteView map(uint64_t) override { return ByteView(); }
  wayland::GpuImage* gpu() override { return this; }
  uint32_t width() const override { return w; }
  uint32_t height() const override { return h; }
  uint32_t pitch() const override { return w * 4; }
  bool draw(graphics::IRenderer2D& renderer, const graphics::Rect2D& source, int32_t x, int32_t y) override {
    if (busy) return false;
    ++draws;
    graphics::Surface s;
    s.pixels = ArrayView<uint32_t>(pixels.data(), pixels.size());
    s.width = w, s.height = h, s.pitch = w;
    renderer.draw_image(s, source, x, y);
    return true;
  }
  bool read(const graphics::Rect2D& r, ArrayView<uint32_t> out) override {
    if (busy || r.x < 0 || r.y < 0 || uint64_t(r.x) + r.width > w || uint64_t(r.y) + r.height > h || out.size() < uint64_t(r.width) * r.height) return false;
    size_t at = 0;
    for (uint32_t y = 0; y < r.height; ++y)
      for (uint32_t x = 0; x < r.width; ++x) out[at++] = pixels[size_t(r.y + y) * w + size_t(r.x) + x];
    return true;
  }
};

constexpr uint32_t kAR24 = 0x34325241, kXR24 = 0x34325258, kAB24 = 0x34324241, kXB24 = 0x34324258;
constexpr size_t kOffered = 4 + 17;      // (the RGB formats and the YUV ones: WaylandYuv.hpp)

// zwp_linux_buffer_params_v1 helpers: params, add plane 0 (fd), create_immed / create.
struct DmabufApp : App {
  uint32_t dmabuf = 0;
  explicit DmabufApp(Server& s, uint32_t version = 3) : App(s) {
    dmabuf = bind(reg, "zwp_linux_dmabuf_v1", version);
    drop_events();
  }
  uint32_t params(Fd plane, uint32_t offset, uint32_t stride, uint64_t modifier = 0) {
    const uint32_t p = new_id();
    send(Msg(dmabuf, 1).u(p));
    if (plane) send(Msg(p, 1).u(0).u(offset).u(stride).u(uint32_t(modifier >> 32)).u(uint32_t(modifier)), {plane});
    return p;
  }
  uint32_t immed(uint32_t p, int32_t w, int32_t h, uint32_t format = kXR24, uint32_t flags = 0) {
    const uint32_t b = new_id();
    send(Msg(p, 3).u(b).i(w).i(h).u(format).u(flags));
    return b;
  }
  void commit(Window& win, uint32_t buffer, int32_t w, int32_t h) {
    send(Msg(win.surface, 1).u(buffer).i(0).i(0));
    send(Msg(win.surface, 2).i(0).i(0).i(w).i(h));
    send(Msg(win.surface, 6));
  }
};

static void test_dmabuf() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(1024, 768, {});
  {      // without a maker of descriptors (no format table to send) version 4 is not offered
    Server bare(A);
    bare.set_dmabuf(true);
    WClient c(bare);
    c.registry();
    CHECK(c.globals["zwp_linux_dmabuf_v1"].second == 3);
  }
  {      // not offered until the transport can make GPU images
    WClient c(server);
    c.registry();
    CHECK(c.globals.count("zwp_linux_dmabuf_v1") == 0);
    const uint32_t r = c.new_id();
    c.send(Msg(1, 1).u(r));
    c.send(Msg(r, 0).u(9).s("zwp_linux_dmabuf_v1").u(3).u(c.new_id()));
    CHECK(c.error());
  }
  server.set_dmabuf(true);
  {      // the formats: modifier events (all linear) from version 3, format events before
    WClient c(server);
    const uint32_t r = c.registry();
    CHECK(c.globals.count("zwp_linux_dmabuf_v1") == 1 && c.globals["zwp_linux_dmabuf_v1"].second == 4);
    c.drop_events();
    const uint32_t d3 = c.bind(r, "zwp_linux_dmabuf_v1", 3);
    std::vector<uint32_t> formats;
    for (const Ev& e : c.events) if (e.object == d3 && e.opcode == 1 && e.u(1) == 0 && e.u(2) == 0) formats.push_back(e.u(0));
    CHECK(formats.size() == kOffered && c.count(d3, 0) == 0);
    CHECK(std::find(formats.begin(), formats.end(), kAR24) != formats.end() && std::find(formats.begin(), formats.end(), kXB24) != formats.end());
    c.drop_events();
    const uint32_t d2 = c.bind(r, "zwp_linux_dmabuf_v1", 2);
    CHECK(c.count(d2, 0) == int(kOffered) && c.count(d2, 1) == 0);
    // version 4: no format events; feedback objects say it all (the format table as a sealed file, /dev/dxg as the device, one tranche)
    c.drop_events();
    const uint32_t d4 = c.bind(r, "zwp_linux_dmabuf_v1", 4);
    CHECK(c.count(d4, 0) == 0 && c.count(d4, 1) == 0);
    const size_t fds_before = c.fds.size();
    const uint32_t fb = c.new_id();
    c.send(Msg(d4, 2).u(fb));
    std::vector<uint16_t> order;
    for (const Ev& e : c.events) if (e.object == fb) order.push_back(e.opcode);
    CHECK((order == std::vector<uint16_t>{1, 2, 4, 5, 6, 3, 0}));
    Ev table = c.take(fb, 1);
    CHECK(table.u(0) == kOffered * 16 && c.fds.size() == fds_before + 1);
    if (c.fds.size() == fds_before + 1) {
      ByteView t = c.fds.back()->map(64);
      CHECK(t.size() >= 64);
      if (t.size() >= 64) {
        uint32_t f0 = 0, f3 = 0;
        uint64_t m0 = 1;
        memcpy(&f0, t.data(), 4);
        memcpy(&m0, t.data() + 8, 8);
        memcpy(&f3, t.data() + 48, 4);
        CHECK(f0 == kAR24 && m0 == 0 && f3 == kXB24);
      }
    }
    Ev main_device = c.take(fb, 2);
    CHECK(main_device.u(0) == 8 && main_device.u(1) == ((10u << 8) | 243u) && main_device.u(2) == 0);
    Ev tranche = c.take(fb, 5);
    CHECK(tranche.u(0) == kOffered * 2 && tranche.u(1) == 0x00010000u && tranche.u(2) == 0x00030002u);
    c.send(Msg(fb, 0));      // destroy
    CHECK(c.take(1, 1).u(0) == fb);
    // per-surface feedback: the same
    const uint32_t comp = c.bind(r, "wl_compositor");
    const uint32_t surf = c.new_id();
    c.send(Msg(comp, 0).u(surf));
    const uint32_t sfb = c.new_id();
    c.send(Msg(d4, 3).u(sfb).u(surf));
    CHECK(c.has(sfb, 0) && c.has(sfb, 2) && !c.closing);
    c.send(Msg(d4, 3).u(c.new_id()).u(comp));      // (not a surface)
    CHECK(c.error() && c.closing);
  }
  {      // feedback is not a version 3 request
    WClient c(server);
    const uint32_t r = c.registry();
    const uint32_t d3 = c.bind(r, "zwp_linux_dmabuf_v1", 3);
    c.send(Msg(d3, 2).u(c.new_id()));
    uint32_t code = 9;
    CHECK(c.error(nullptr, &code) && code == 1);
  }
  {      // a toplevel showing a GPU buffer; drawn by the image itself; released when replaced
    DmabufApp app(server);
    App::Window w = app.toplevel(true, "GPU");
    Fd first = Fd::make_as<FakeGpu>(A, 120u, 80u, 0xFF204060u);
    FakeGpu& g1 = static_cast<FakeGpu&>(*first);
    const uint32_t b1 = app.immed(app.params(first, 0, 480), 120, 80);
    app.commit(w, b1, 120, 80);
    CHECK(!app.closing);
    std::vector<uint32_t> list = listed(server);
    CHECK(list.size() == 1);
    if (list.empty()) return;
    const uint32_t win = list[0];
    desktop::WindowsWindowInfo info;
    String title(A);
    CHECK(server.window_info(win, &info, &title) && info.surface.width == 120 && info.surface.height == 80);
    CHECK(server.window_pixel(win, 5, 5) == 0xFF204060u);
    g1.pixels[size_t(7) * 120 + 9] = 0xFFFFFFFFu;      // (read where it is: no copy was taken)
    CHECK(server.window_pixel(win, 9, 7) == 0xFFFFFFFFu);
    const uint32_t frame = app.new_id();
    app.send(Msg(w.surface, 3).u(frame));
    app.send(Msg(w.surface, 6));
    std::vector<uint32_t> target(200 * 150, 0);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = 200, ts.height = 150, ts.pitch = 200;
    {
      graphics::SoftwareRenderer2D r(ts);
      uint32_t dw = 0, dh = 0;
      CHECK(server.draw_window(win, r, 10, 20, &dw, &dh) && dw == 120 && dh == 80);
    }
    CHECK(g1.draws == 1 && target[size_t(20 + 3) * 200 + 10 + 3] == 0xFF204060u && target[size_t(20 + 7) * 200 + 10 + 9] == 0xFFFFFFFFu);
    CHECK(app.has(frame, 0));
    // a picture still being drawn on the GPU: nothing drawn, the desktop is asked to come back
    List<uint32_t> changed(A);
    List<desktop::IWindowsSession::Repaint> repainted(A);
    server.take_changes(&changed, &repainted);
    g1.busy = true;
    {
      graphics::SoftwareRenderer2D r(ts);
      uint32_t dw = 0, dh = 0;
      CHECK(server.draw_window(win, r, 0, 0, &dw, &dh));
    }
    server.take_changes(&changed, &repainted);
    CHECK(repainted.count() == 1 && repainted[0].window == win && repainted[0].dirty.width == 120);
    CHECK(server.window_pixel(win, 5, 5) == 0);
    g1.busy = false;
    // `create` instead: the buffer comes as `created` with an id of the server's; the first buffer is released when it replaces it
    Fd second = Fd::make_as<FakeGpu>(A, 100u, 60u, 0xFF00FF00u);
    const uint32_t p2 = app.params(second, 0, 400);
    app.send(Msg(p2, 2).i(100).i(60).u(kAR24).u(0));
    Ev created = app.take(p2, 0);
    CHECK(created.object == p2 && created.u(0) >= 0xFF000000u);
    const uint32_t b2 = created.u(0);
    app.commit(w, b2, 100, 60);
    CHECK(app.has(b1, 0));      // release
    CHECK(server.window_info(win, &info, &title) && info.surface.width == 100 && server.window_pixel(win, 50, 30) == 0xFF00FF00u);
    app.send(Msg(p2, 0));
    // destroying a server-made buffer gives no delete_id (its id is not the client's)
    app.drop_events();
    app.send(Msg(w.surface, 1).u(0).i(0).i(0));
    app.send(Msg(w.surface, 6));
    app.send(Msg(b2, 0));
    bool deleted = false;
    app.pull();
    for (const Ev& e : app.events) deleted |= e.object == 1 && e.opcode == 1 && e.u(0) == b2;
    CHECK(!deleted && !app.closing);
    CHECK(listed(server).empty());      // (a null buffer unmapped it)
    // the image outlives the buffer while a surface holds it, and goes with the last of them
  }
  // what `create` answers with failed, and create_immed with invalid_wl_buffer
  {
    DmabufApp app(server);
    std::vector<uint8_t> plain(4096);
    const uint32_t p = app.params(Fd::make_as<TestMemory>(A, &plain), 0, 64);      // (memory, not a GPU image)
    app.send(Msg(p, 2).i(16).i(16).u(kXR24).u(0));
    CHECK(app.has(p, 1) && !app.closing);
    const uint32_t q = app.params(Fd::make_as<FakeGpu>(A, 32u, 32u, 0u), 0, 100);      // (another stride than the image's)
    app.send(Msg(q, 2).i(16).i(16).u(kXR24).u(0));
    CHECK(app.has(q, 1) && !app.closing);
    const uint32_t f = app.params(Fd::make_as<FakeGpu>(A, 32u, 32u, 0u), 0, 128);      // (y-inverted)
    app.send(Msg(f, 2).i(32).i(32).u(kXR24).u(1));
    CHECK(app.has(f, 1) && !app.closing);
    const uint32_t m = app.params(Fd::make_as<FakeGpu>(A, 32u, 32u, 0u), 0, 128, 0x0100000000000001ull);      // (a tiled layout)
    app.send(Msg(m, 2).i(32).i(32).u(kXR24).u(0));
    CHECK(app.has(m, 1) && !app.closing);
    const uint32_t big = app.params(Fd::make_as<FakeGpu>(A, 32u, 32u, 0u), 0, 128);      // (larger than the image, but within its memory)
    app.send(Msg(big, 2).i(33).i(16).u(kXR24).u(0));
    CHECK(app.has(big, 1) && !app.closing);
    const uint32_t implicit = app.params(Fd::make_as<FakeGpu>(A, 32u, 32u, 0u), 0, 128, 0x00FFFFFFFFFFFFFFull);      // (DRM_FORMAT_MOD_INVALID: taken as linear)
    app.send(Msg(implicit, 2).i(32).i(32).u(kAB24).u(0));
    CHECK(app.take(implicit, 0).object == implicit);
    const uint32_t i2 = app.params(Fd::make_as<TestMemory>(A, &plain), 0, 64);
    app.immed(i2, 16, 16);
    uint32_t object = 0, code = 0;
    CHECK(app.error(&object, &code) && object == i2 && code == 7);
  }
  // protocol errors
  struct Case {
    const char* what;
    uint32_t code;
    std::function<void(DmabufApp&)> run;
  };
  const Case cases[] = {
      {"plane 4", 1, [](DmabufApp& a) { const uint32_t p = a.new_id(); a.send(Msg(a.dmabuf, 1).u(p)); a.send(Msg(p, 1).u(4).u(0).u(64).u(0).u(0), {Fd::make_as<FakeGpu>(A, 16u, 16u, 0u)}); }},
      {"plane set twice", 2, [](DmabufApp& a) { const uint32_t p = a.params(Fd::make_as<FakeGpu>(A, 16u, 16u, 0u), 0, 64); a.send(Msg(p, 1).u(0).u(0).u(64).u(0).u(0), {Fd::make_as<FakeGpu>(A, 16u, 16u, 0u)}); }},
      {"no plane", 3, [](DmabufApp& a) { const uint32_t p = a.params(Fd(), 0, 0); a.immed(p, 16, 16); }},
      {"format", 4, [](DmabufApp& a) { a.immed(a.params(Fd::make_as<FakeGpu>(A, 16u, 16u, 0u), 0, 64), 16, 16, 0x12345678u /* no such format */); }},
      {"size", 5, [](DmabufApp& a) { a.immed(a.params(Fd::make_as<FakeGpu>(A, 16u, 16u, 0u), 0, 64), 0, 16); }},
      {"bounds", 6, [](DmabufApp& a) { a.immed(a.params(Fd::make_as<FakeGpu>(A, 16u, 16u, 0u), 1024, 64), 16, 16); }},
      {"used", 0, [](DmabufApp& a) { const uint32_t p = a.params(Fd::make_as<FakeGpu>(A, 16u, 16u, 0u), 0, 64); a.immed(p, 16, 16); a.immed(p, 16, 16); }},
      {"add after create", 0, [](DmabufApp& a) { const uint32_t p = a.params(Fd::make_as<FakeGpu>(A, 16u, 16u, 0u), 0, 64); a.immed(p, 16, 16); a.send(Msg(p, 1).u(0).u(0).u(64).u(0).u(0), {Fd::make_as<FakeGpu>(A, 16u, 16u, 0u)}); }},
      {"no descriptor", 1, [](DmabufApp& a) { const uint32_t p = a.new_id(); a.send(Msg(a.dmabuf, 1).u(p)); a.send(Msg(p, 1).u(0).u(0).u(64).u(0).u(0)); }},
      {"immed on version 1", 1, [](DmabufApp& a) { a.immed(a.params(Fd::make_as<FakeGpu>(A, 16u, 16u, 0u), 0, 64), 16, 16); }},
  };
  for (const Case& k : cases) {
    DmabufApp app(server, std::string(k.what) == "immed on version 1" ? 1 : 3);
    k.run(app);
    uint32_t object = 0, code = 0;
    const bool err = app.error(&object, &code);
    if (!err || code != k.code) printf("   dmabuf error case '%s': error %d code %u\n", k.what, err, code);
    CHECK(err && code == k.code && app.closing);
  }
  CHECK(server.client_count() == 0);
}

// Truncated, chunked and bit-flipped dmabuf sessions: nothing the server is sent breaks it.
static void test_dmabuf_fuzz() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(800, 600, {});
  server.set_dmabuf(true);
  std::vector<uint8_t> all;
  auto add = [&](Msg m) { std::vector<uint8_t> v = m.done(); all.insert(all.end(), v.begin(), v.end()); };
  add(Msg(1, 1).u(2));
  add(Msg(2, 0).u(1).s("wl_compositor").u(6).u(3));
  add(Msg(2, 0).u(7).s("xdg_wm_base").u(5).u(4));
  add(Msg(2, 0).u(9).s("zwp_linux_dmabuf_v1").u(3).u(5));
  add(Msg(3, 0).u(10));
  add(Msg(4, 2).u(11).u(10));
  add(Msg(11, 1).u(12));
  add(Msg(10, 6));
  add(Msg(11, 4).u(1));
  add(Msg(5, 1).u(13));
  add(Msg(13, 1).u(0).u(0).u(256).u(0).u(0));      // (the descriptor)
  add(Msg(13, 3).u(14).i(64).i(64).u(kXR24).u(0));
  add(Msg(10, 1).u(14).i(0).i(0));
  add(Msg(10, 6));
  add(Msg(5, 1).u(15));
  add(Msg(15, 1).u(0).u(0).u(256).u(0).u(0));
  add(Msg(15, 2).i(32).i(32).u(kAR24).u(0));
  add(Msg(15, 0));
  add(Msg(14, 0));
  add(Msg(1, 0).u(16));
  int sessions = 0;
  auto run = [&](const std::vector<uint8_t>& bytes, size_t chunk) {
    const uint32_t c = server.open_client(1);
    if (!c) { ++g_failed; return; }
    ++sessions;
    std::vector<Fd> fds = {Fd::make_as<FakeGpu>(A, 64u, 64u, 0xFF112233u), Fd::make_as<FakeGpu>(A, 64u, 64u, 0xFF445566u)};
    server.receive(c, ConstByteView(), ArrayView<Fd>(fds.data(), fds.size()));
    for (size_t at = 0; at < bytes.size();) {
      const size_t n = std::min(chunk, bytes.size() - at);
      server.receive(c, ConstByteView(bytes.data() + at, n), ArrayView<Fd>());
      at += n;
      ByteBuffer out(A);
      List<Fd> fo(A);
      if (server.take_output(c, &out, &fo)) break;
    }
    List<uint32_t> list(A);
    server.windows(&list);
    std::vector<uint32_t> target(64 * 64);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = ts.height = ts.pitch = 64;
    for (uint32_t w : list.view()) {
      graphics::SoftwareRenderer2D renderer(ts);
      uint32_t dw, dh;
      server.draw_window(w, renderer, 0, 0, &dw, &dh);
      server.window_pixel(w, 1, 1);
    }
    server.close_client(c);
  };
  run(all, all.size());
  for (size_t chunk = 1; chunk <= 32; ++chunk) run(all, chunk);
  for (size_t cut = 0; cut < all.size(); cut += 4) run(std::vector<uint8_t>(all.begin(), all.begin() + long(cut)), 4096);
  std::mt19937 rng(99);
  for (int round = 0; round < 1500; ++round) {
    std::vector<uint8_t> v = all;
    const int flips = 1 + int(rng() % 4);
    for (int f = 0; f < flips; ++f) v[rng() % v.size()] ^= uint8_t(1u << (rng() % 8));
    run(v, 1 + rng() % 100);
  }
  printf("   dmabuf fuzz: %d sessions\n", sessions);
  CHECK(server.client_count() == 0);
  free_made();
}

// ------------------------------------------------------------------ YUV buffers (WaylandYuv.hpp): wl_shm and dmabuf, converted at commit

constexpr uint32_t kNV12 = 0x3231564E, kYU12 = 0x32315559, kYUYV = 0x56595559, kP010 = 0x30313050;

// BT.601 limited range in floating point: the reference the integer converter must stay within 2 of.
static uint32_t ref_rgb(int y, int u, int v) {
  const double c = 1.164383 * (y - 16);
  auto clamp = [](double d) { return uint32_t(d < 0 ? 0 : d > 255 ? 255 : d + 0.5); };
  const uint32_t r = clamp(c + 1.596027 * (v - 128)), g = clamp(c - 0.391762 * (u - 128) - 0.812968 * (v - 128)), b = clamp(c + 2.017232 * (u - 128));
  return 0xFF000000u | r << 16 | g << 8 | b;
}
static bool near_rgb(uint32_t a, uint32_t b) {
  for (int shift = 0; shift < 32; shift += 8) {
    const int d = int((a >> shift) & 0xFF) - int((b >> shift) & 0xFF);
    if (d > 2 || d < -2) return false;
  }
  return true;
}
static uint8_t sy(uint32_t x, uint32_t y) { return uint8_t(16 + (x * 37 + y * 11) % 220); }
static uint8_t su(uint32_t x, uint32_t y) { return uint8_t(16 + (x * 53 + y * 29) % 225); }
static uint8_t sv(uint32_t x, uint32_t y) { return uint8_t(16 + (x * 17 + y * 71) % 225); }
static uint32_t expected_px(const wayland::YuvFormat& f, uint32_t x, uint32_t y) {
  const uint32_t cx = x / f.hsub, cy = f.planes == 1 ? y : y / f.vsub;
  return ref_rgb(sy(x, y), su(cx, cy), sv(cx, cy));
}
// Writes the test picture into planes at `at[p]` of `out` with `strides[p]`.
static void write_yuv(const wayland::YuvFormat& f, uint32_t w, uint32_t h, uint8_t* const at[3], const uint32_t strides[3]) {
  for (uint32_t y = 0; y < h; ++y) {
    uint8_t* row = at[0] + size_t(y) * strides[0];
    if (f.planes == 1) {
      for (uint32_t p = 0; p < (w + 1) / 2; ++p) {
        const uint8_t y0 = sy(2 * p, y), y1 = sy(2 * p + 1, y), c0 = f.swapped ? sv(p, y) : su(p, y), c1 = f.swapped ? su(p, y) : sv(p, y);
        uint8_t* q = row + p * 4;
        if (f.luma_first) q[0] = y0, q[1] = c0, q[2] = y1, q[3] = c1;
        else q[0] = c0, q[1] = y0, q[2] = c1, q[3] = y1;
      }
      continue;
    }
    for (uint32_t x = 0; x < w; ++x) {
      if (f.sample == 2) row[x * 2] = 0x3F, row[x * 2 + 1] = sy(x, y);      // (the low bits are noise the converter must drop)
      else row[x] = sy(x, y);
    }
  }
  if (f.planes == 1) return;
  const uint32_t cw = (w + f.hsub - 1) / f.hsub, ch = (h + f.vsub - 1) / f.vsub;
  for (uint32_t y = 0; y < ch; ++y)
    for (uint32_t x = 0; x < cw; ++x) {
      const uint8_t u = su(x, y), v = sv(x, y);
      if (f.planes == 2) {
        uint8_t* q = at[1] + size_t(y) * strides[1] + size_t(x) * 2 * f.sample;
        const uint8_t c0 = f.swapped ? v : u, c1 = f.swapped ? u : v;
        if (f.sample == 2) q[0] = 0, q[1] = c0, q[2] = 0xC0, q[3] = c1;
        else q[0] = c0, q[1] = c1;
      } else {
        at[1][size_t(y) * strides[1] + x] = f.swapped ? v : u;
        at[2][size_t(y) * strides[2] + x] = f.swapped ? u : v;
      }
    }
}

static void test_yuv_convert() {
  int formats = 0, pictures = 0;
  CHECK(wayland::yuv_formats().size() == 17);
  wayland::YuvFormat none;
  CHECK(!wayland::yuv_format(kXR24, &none) && !wayland::yuv_format(0, &none));
  CHECK(ref_rgb(16, 128, 128) == 0xFF000000u && ref_rgb(235, 128, 128) == 0xFFFFFFFFu);
  for (uint32_t code : wayland::yuv_formats()) {
    wayland::YuvFormat f;
    CHECK(wayland::yuv_format(code, &f) && f.code == code && f.planes >= 1 && f.planes <= 3);
    ++formats;
    const uint32_t sizes[][2] = {{1, 1}, {2, 2}, {5, 3}, {16, 8}, {7, 1}};
    for (const auto& sz : sizes) {
      const uint32_t w = sz[0], h = sz[1];
      for (uint32_t pad : {0u, 3u}) {
        std::vector<std::vector<uint8_t>> mem(f.planes);
        uint32_t strides[3] = {};
        uint8_t* at[3] = {};
        wayland::YuvPlane planes[3];
        for (uint32_t p = 0; p < f.planes; ++p) {
          strides[p] = uint32_t(f.row_bytes(p, w)) + pad;
          mem[p].assign(f.plane_bytes(p, w, h, strides[p]), 0x77);
          at[p] = mem[p].data();
        }
        write_yuv(f, w, h, at, strides);
        for (uint32_t p = 0; p < f.planes; ++p) planes[p].bytes = ConstByteView(mem[p].data(), mem[p].size()), planes[p].stride = strides[p];
        std::vector<uint32_t> out(size_t(w) * h, 0x12345678u);
        CHECK(wayland::yuv_to_xrgb(f, w, h, ArrayView<const wayland::YuvPlane>(planes, f.planes), ArrayView<uint32_t>(out.data(), out.size())));
        bool all = true;
        for (uint32_t y = 0; y < h; ++y)
          for (uint32_t x = 0; x < w; ++x) if (!near_rgb(out[size_t(y) * w + x], expected_px(f, x, y))) all = false;
        if (!all) printf("   yuv %08x %ux%u pad %u: wrong colours\n", code, w, h, pad);
        CHECK(all);
        ++pictures;
        // truncation: every plane one byte short, a stride shorter than its row, too few planes, too small an output - false, nothing written
        auto refused = [&](ArrayView<const wayland::YuvPlane> pl, size_t out_size) {
          std::vector<uint32_t> o(out_size, 0xABCDEF01u);
          const bool r = wayland::yuv_to_xrgb(f, w, h, pl, ArrayView<uint32_t>(o.data(), o.size()));
          for (uint32_t v : o) if (v != 0xABCDEF01u) return false;
          return !r;
        };
        for (uint32_t p = 0; p < f.planes; ++p) {
          wayland::YuvPlane cut[3] = {planes[0], planes[1], planes[2]};
          cut[p].bytes = cut[p].bytes.first(cut[p].bytes.size() - 1);
          CHECK(refused(ArrayView<const wayland::YuvPlane>(cut, f.planes), out.size()));
          wayland::YuvPlane narrow[3] = {planes[0], planes[1], planes[2]};
          narrow[p].stride = uint32_t(f.row_bytes(p, w)) - 1;
          CHECK(refused(ArrayView<const wayland::YuvPlane>(narrow, f.planes), out.size()));
        }
        CHECK(refused(ArrayView<const wayland::YuvPlane>(planes, f.planes - 1), out.size()));
        CHECK(refused(ArrayView<const wayland::YuvPlane>(planes, f.planes), out.size() - 1));
      }
    }
  }
  // bit flips in the samples only change colours (every byte value is a sample)
  wayland::YuvFormat nv12;
  CHECK(wayland::yuv_format(kNV12, &nv12));
  std::mt19937 rng(5);
  for (int round = 0; round < 200; ++round) {
    std::vector<uint8_t> y(64 * 16), uv(64 * 8);
    for (auto& b : y) b = uint8_t(rng());
    for (auto& b : uv) b = uint8_t(rng());
    wayland::YuvPlane pl[2] = {{ConstByteView(y.data(), y.size()), 64}, {ConstByteView(uv.data(), uv.size()), 64}};
    std::vector<uint32_t> out(64 * 16);
    CHECK(wayland::yuv_to_xrgb(nv12, 64, 16, ArrayView<const wayland::YuvPlane>(pl, 2), ArrayView<uint32_t>(out.data(), out.size())));
    for (uint32_t v : out) if ((v >> 24) != 0xFF) { CHECK(false); break; }
  }
  // the wl_shm layouts (Weston's rule)
  uint64_t off[3], bytes = 0;
  uint32_t st[3];
  CHECK(wayland::yuv_shm_layout(nv12, 5, 3, 6, off, st, &bytes) && off[1] == 18 && st[1] == 6 && bytes == 30);
  wayland::YuvFormat i420;
  CHECK(wayland::yuv_format(kYU12, &i420));
  CHECK(wayland::yuv_shm_layout(i420, 6, 4, 6, off, st, &bytes) && off[1] == 24 && off[2] == 30 && st[1] == 3 && st[2] == 3 && bytes == 36);
  CHECK(!wayland::yuv_shm_layout(i420, 5, 4, 5, off, st, &bytes));      // (an odd stride cannot be halved)
  printf("   yuv: %d formats, %d pictures\n", formats, pictures);
}

static uint32_t new_window(Server& server, const std::vector<uint32_t>& before) {
  for (uint32_t win : listed(server))
    if (std::find(before.begin(), before.end(), win) == before.end()) return win;
  return 0;
}

static void test_yuv_buffers() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(1024, 768, {});
  server.set_dmabuf(true);
  // wl_shm: an NV12 picture at an odd offset in the pool, the planes one after another
  {
    App app(server);
    std::vector<uint32_t> before = listed(server);
    App::Window w = app.toplevel(true, "nv12");
    wayland::YuvFormat f;
    CHECK(wayland::yuv_format(kNV12, &f));
    const uint32_t W = 33, H = 17, stride = 36, offset = 6;
    uint64_t off[3], bytes = 0;
    uint32_t st[3];
    CHECK(wayland::yuv_shm_layout(f, W, H, stride, off, st, &bytes));
    App::Pool p = app.pool(size_t(offset + bytes));
    uint8_t* at[3] = {p.memory->data() + offset + off[0], p.memory->data() + offset + off[1], nullptr};
    write_yuv(f, W, H, at, st);
    const uint32_t b = app.buffer(p, int32_t(offset), int32_t(W), int32_t(H), int32_t(stride), kNV12);
    app.send(Msg(w.surface, 1).u(b).i(0).i(0));
    app.send(Msg(w.surface, 6));
    CHECK(!app.closing && !app.error());
    const uint32_t win = new_window(server, before);
    CHECK(win != 0);
    bool all = true;
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) if (!near_rgb(server.window_pixel(win, int32_t(x), int32_t(y)), expected_px(f, x, y))) all = false;
    CHECK(all);
    // drawn like any buffer
    std::vector<uint32_t> target(64 * 64, 0);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = ts.height = ts.pitch = 64;
    graphics::SoftwareRenderer2D renderer(ts);
    uint32_t dw = 0, dh = 0;
    CHECK(server.draw_window(win, renderer, 0, 0, &dw, &dh));
    int matching = 0;
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) if (near_rgb(target[size_t(y) * 64 + x], expected_px(f, x, y))) ++matching;
    CHECK(matching > int(W * H / 2));
    // the picture is the one of the last commit: a change of the memory shows at the next commit, not before
    const uint32_t old = server.window_pixel(win, 0, 0);
    for (uint32_t y = 0; y < H; ++y) memset(at[0] + size_t(y) * stride, 235, W);
    for (uint32_t y = 0; y < (H + 1) / 2; ++y) memset(at[1] + size_t(y) * st[1], 128, W + 1);
    CHECK(server.window_pixel(win, 0, 0) == old);
    app.send(Msg(w.surface, 2).i(0).i(0).i(int32_t(W)).i(int32_t(H)));
    app.send(Msg(w.surface, 6));
    CHECK(server.window_pixel(win, 0, 0) == 0xFFFFFFFFu && server.window_pixel(win, int32_t(W - 1), int32_t(H - 1)) == 0xFFFFFFFFu);
    // YUYV and I420 (odd sizes) from the same pool kind
    for (uint32_t code : {kYUYV, kYU12, kP010}) {
      wayland::YuvFormat g;
      CHECK(wayland::yuv_format(code, &g));
      const uint32_t gw = 9, gh = 5, gstride = code == kYUYV ? 20 : code == kP010 ? 20 : 10;      // (P010: the chroma rows, 5 pairs of 16-bit samples, need 20)
      CHECK(wayland::yuv_shm_layout(g, gw, gh, gstride, off, st, &bytes));
      App::Pool q = app.pool(size_t(bytes));
      uint8_t* gat[3] = {q.memory->data() + off[0], q.memory->data() + off[1], q.memory->data() + off[2]};
      write_yuv(g, gw, gh, gat, st);
      const uint32_t gb = app.buffer(q, 0, int32_t(gw), int32_t(gh), int32_t(gstride), code);
      app.send(Msg(w.surface, 1).u(gb).i(0).i(0));
      app.send(Msg(w.surface, 6));
      CHECK(!app.closing);
      bool good = true;
      for (uint32_t y = 0; y < gh; ++y)
        for (uint32_t x = 0; x < gw; ++x) if (!near_rgb(server.window_pixel(win, int32_t(x), int32_t(y)), expected_px(g, x, y))) good = false;
      if (!good) printf("   shm %08x: wrong colours\n", code);
      CHECK(good);
    }
  }
  // wl_shm errors: a stride shorter than a row, an odd stride for I420, a picture past the pool
  {
    struct Bad { uint32_t format; int32_t offset, w, h, stride; size_t pool; };
    const Bad bad[] = {{kNV12, 0, 8, 4, 7, 4096}, {kYU12, 0, 8, 4, 9, 4096}, {kNV12, 0, 8, 4, 8, 47}, {kNV12, 2, 8, 4, 8, 48}, {kYUYV, 0, 8, 4, 15, 4096},
                       {kNV12, -4, 8, 4, 8, 4096}, {kNV12, 0, 0, 4, 8, 4096}, {kYU12, 0, 7, 4, 7, 4096}};
    for (const Bad& k : bad) {
      App app(server, false);
      App::Pool p = app.pool(k.pool);
      app.buffer(p, k.offset, k.w, k.h, k.stride, k.format);
      uint32_t object = 0, code = 99;
      CHECK(app.error(&object, &code) && object == p.id && code == 1);
    }
    App ok(server, false);      // (exactly the size the planes need is fine)
    App::Pool p = ok.pool(48);
    ok.buffer(p, 0, 8, 4, 8, kNV12);
    CHECK(ok.roundtrip() && !ok.error());
  }
  // dmabuf: NV12 in two descriptors, I420 in one at offsets, and what is refused
  {
    std::vector<std::unique_ptr<std::vector<uint8_t>>> keep;      // (outlives the client, whose buffers point into it)
    DmabufApp app(server);
    std::vector<uint32_t> before = listed(server);
    App::Window w = app.toplevel(true, "dmabuf yuv");
    uint32_t win = 0;      // (listed once it has a buffer)
    auto memory = [&](size_t n) {
      keep.push_back(std::make_unique<std::vector<uint8_t>>(n, 0));
      return keep.back().get();
    };
    auto add = [&](uint32_t params, uint32_t plane, Fd fd, uint32_t offset, uint32_t stride, uint64_t modifier = 0) {
      app.send(Msg(params, 1).u(plane).u(offset).u(stride).u(uint32_t(modifier >> 32)).u(uint32_t(modifier)), {fd});
    };
    auto shows = [&](const wayland::YuvFormat& f, uint32_t W, uint32_t H) {
      for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) if (!near_rgb(server.window_pixel(win, int32_t(x), int32_t(y)), expected_px(f, x, y))) return false;
      return true;
    };
    wayland::YuvFormat nv12, i420;
    CHECK(wayland::yuv_format(kNV12, &nv12) && wayland::yuv_format(kYU12, &i420));
    {
      const uint32_t W = 20, H = 10;
      auto* y = memory(64 * H + 100);
      auto* uv = memory(32 * 5 + 16);
      uint8_t* at[3] = {y->data() + 100, uv->data() + 16, nullptr};
      const uint32_t strides[3] = {64, 32, 0};
      write_yuv(nv12, W, H, at, strides);
      const uint32_t params = app.new_id();
      app.send(Msg(app.dmabuf, 1).u(params));
      add(params, 0, Fd::make_as<TestMemory>(A, y), 100, 64);
      add(params, 1, Fd::make_as<TestMemory>(A, uv), 16, 32);
      const uint32_t b = app.immed(params, int32_t(W), int32_t(H), kNV12);
      app.commit(w, b, int32_t(W), int32_t(H));
      win = new_window(server, before);
      CHECK(win != 0 && !app.closing && !app.error() && shows(nv12, W, H));
    }
    {
      const uint32_t W = 11, H = 7, S0 = 12, S1 = 8;
      auto* all = memory(S0 * H + 2 * S1 * 4 + 40);
      uint8_t* at[3] = {all->data(), all->data() + S0 * H + 8, all->data() + S0 * H + 8 + S1 * 4};
      const uint32_t strides[3] = {S0, S1, S1};
      write_yuv(i420, W, H, at, strides);
      Fd one = Fd::make_as<TestMemory>(A, all);
      const uint32_t params = app.new_id();
      app.send(Msg(app.dmabuf, 1).u(params));
      add(params, 0, one, 0, S0);
      add(params, 1, one, S0 * H + 8, S1);
      add(params, 2, one, S0 * H + 8 + S1 * 4, S1);
      const uint32_t b = app.immed(params, int32_t(W), int32_t(H), kYU12);
      app.commit(w, b, int32_t(W), int32_t(H));
      CHECK(!app.closing && !app.error() && shows(i420, W, H));
    }
    {      // `create` of what cannot be imported answers failed: a GPU image plane, a stride shorter than a row, mixed modifiers, no memory
      auto* m = memory(4096);
      for (int k = 0; k < 4; ++k) {
        const uint32_t params = app.new_id();
        app.send(Msg(app.dmabuf, 1).u(params));
        Fd y = k == 0 ? Fd::make_as<FakeGpu>(A, 16u, 16u, 0u) : Fd::make_as<TestMemory>(A, m);
        if (k == 3) static_cast<TestMemory&>(*y).not_memory = true;
        add(params, 0, y, 0, k == 1 ? 15 : 16);
        add(params, 1, Fd::make_as<TestMemory>(A, m), 1024, 16, k == 2 ? 0x00FFFFFFFFFFFFFFull : 0);
        app.send(Msg(params, 2).i(16).i(16).u(kNV12).u(0));
        CHECK(app.take(params, 1).object == params && !app.closing);
      }
    }
  }
  // dmabuf protocol errors: a missing plane, too many planes for RGB, a plane past its descriptor, a plane index past 3
  {
    struct Case { int kind; uint32_t code; };
    const Case cases[] = {{0, 3}, {1, 3}, {2, 6}, {3, 1}};
    for (const Case& k : cases) {
      DmabufApp app(server);
      std::vector<uint8_t> small(100), big(4096);
      const uint32_t params = app.new_id();
      app.send(Msg(app.dmabuf, 1).u(params));
      auto add = [&](uint32_t plane, std::vector<uint8_t>* v, uint32_t offset, uint32_t stride) {
        app.send(Msg(params, 1).u(plane).u(offset).u(stride).u(0).u(0), {Fd::make_as<TestMemory>(A, v)});
      };
      if (k.kind == 0) add(0, &big, 0, 16);
      if (k.kind == 1) add(0, &big, 0, 64), add(1, &big, 1024, 64);
      if (k.kind == 2) add(0, &big, 0, 16), add(1, &small, 0, 16);
      if (k.kind == 3) add(4, &big, 0, 16);
      if (k.kind != 3) app.immed(params, 16, 16, k.kind == 1 ? kXR24 : kNV12);
      uint32_t object = 0, code = 99;
      CHECK(app.error(&object, &code) && object == params && code == k.code);
    }
  }
  CHECK(server.client_count() == 0);
  free_made();
}

// Byte flips over a session that makes NV12 buffers through wl_shm and dmabuf.
static void test_yuv_fuzz() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(800, 600, {});
  server.set_dmabuf(true);
  std::vector<uint8_t> all;
  auto add = [&](Msg m) { std::vector<uint8_t> v = m.done(); all.insert(all.end(), v.begin(), v.end()); };
  add(Msg(1, 1).u(2));
  add(Msg(2, 0).u(1).s("wl_compositor").u(6).u(3));
  add(Msg(2, 0).u(7).s("xdg_wm_base").u(5).u(4));
  add(Msg(2, 0).u(9).s("zwp_linux_dmabuf_v1").u(3).u(5));
  add(Msg(2, 0).u(3).s("wl_shm").u(1).u(6));
  add(Msg(3, 0).u(10));
  add(Msg(4, 2).u(11).u(10));
  add(Msg(11, 1).u(12));
  add(Msg(10, 6));
  add(Msg(11, 4).u(1));
  add(Msg(6, 0).u(13).i(4096));                                  // pool (descriptor 0)
  add(Msg(13, 0).u(14).i(2).i(30).i(20).i(32).u(kNV12));        // NV12 buffer at offset 2
  add(Msg(10, 1).u(14).i(0).i(0));
  add(Msg(10, 6));
  add(Msg(5, 1).u(15));
  add(Msg(15, 1).u(0).u(0).u(32).u(0).u(0));                     // (descriptor 1)
  add(Msg(15, 1).u(1).u(640).u(32).u(0).u(0));                   // (descriptor 2)
  add(Msg(15, 3).u(16).i(30).i(20).u(kNV12).u(0));
  add(Msg(10, 1).u(16).i(0).i(0));
  add(Msg(10, 2).i(0).i(0).i(30).i(20));
  add(Msg(10, 6));
  add(Msg(13, 2).i(8192));                                       // pool grows
  add(Msg(14, 0));
  add(Msg(1, 0).u(17));
  std::vector<uint8_t> m0(8192, 0x80), m1(4096, 0x40), m2(4096, 0xC0);
  int sessions = 0;
  auto run = [&](const std::vector<uint8_t>& bytes, size_t chunk) {
    const uint32_t c = server.open_client(1);
    if (!c) { ++g_failed; return; }
    ++sessions;
    std::vector<Fd> fds = {Fd::make_as<TestMemory>(A, &m0), Fd::make_as<TestMemory>(A, &m1), Fd::make_as<TestMemory>(A, &m2)};
    server.receive(c, ConstByteView(), ArrayView<Fd>(fds.data(), fds.size()));
    for (size_t at = 0; at < bytes.size();) {
      const size_t n = std::min(chunk, bytes.size() - at);
      server.receive(c, ConstByteView(bytes.data() + at, n), ArrayView<Fd>());
      at += n;
      ByteBuffer out(A);
      List<Fd> fo(A);
      if (server.take_output(c, &out, &fo)) break;
    }
    List<uint32_t> list(A);
    server.windows(&list);
    std::vector<uint32_t> target(64 * 64);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = ts.height = ts.pitch = 64;
    for (uint32_t w : list.view()) {
      graphics::SoftwareRenderer2D renderer(ts);
      uint32_t dw, dh;
      server.draw_window(w, renderer, 0, 0, &dw, &dh);
      server.window_pixel(w, 1, 1);
      server.window_pixel(w, 29, 19);
    }
    server.close_client(c);
  };
  run(all, all.size());
  for (size_t chunk = 1; chunk <= 16; ++chunk) run(all, chunk);
  for (size_t cut = 0; cut < all.size(); cut += 4) run(std::vector<uint8_t>(all.begin(), all.begin() + long(cut)), 4096);
  std::mt19937 rng(1234);
  for (int round = 0; round < 1500; ++round) {
    std::vector<uint8_t> v = all;
    const int flips = 1 + int(rng() % 4);
    for (int f = 0; f < flips; ++f) v[rng() % v.size()] ^= uint8_t(1u << (rng() % 8));
    run(v, 1 + rng() % 100);
  }
  printf("   yuv fuzz: %d sessions\n", sessions);
  CHECK(server.client_count() == 0);
  free_made();
}

// ------------------------------------------------------------------ zwlr_layer_shell_v1

struct LayerApp : App {
  uint32_t shell = 0;
  explicit LayerApp(Server& s, uint32_t version = 4) : App(s) {
    shell = bind(reg, "zwlr_layer_shell_v1", version);
    drop_events();
  }
  struct Layer {
    uint32_t surface = 0, layer = 0, serial = 0;
    int32_t cw = 0, ch = 0;
  };
  // get_layer_surface, the state, the initial commit; the configure taken and acknowledged
  Layer make(uint32_t band, uint32_t anchor, uint32_t w, uint32_t h, int32_t zone = 0, std::array<int32_t, 4> margin = {0, 0, 0, 0}, uint32_t keyboard = 0,
             const std::string& name = "panel") {
    Layer l;
    l.surface = surface();
    l.layer = new_id();
    send(Msg(shell, 0).u(l.layer).u(l.surface).u(0).u(band).s(name));
    send(Msg(l.layer, 1).u(anchor));
    send(Msg(l.layer, 0).u(w).u(h));
    send(Msg(l.layer, 2).i(zone));
    send(Msg(l.layer, 3).i(margin[0]).i(margin[1]).i(margin[2]).i(margin[3]));
    if (keyboard) send(Msg(l.layer, 4).u(keyboard));
    send(Msg(l.surface, 6));
    configured(l);
    return l;
  }
  bool configured(Layer& l) {
    Ev c = take(l.layer, 0);
    if (c.object == 0) return false;
    l.serial = c.u(0);
    l.cw = c.i(1);
    l.ch = c.i(2);
    send(Msg(l.layer, 6).u(l.serial));
    return true;
  }
  void draw(Layer& l, int32_t w, int32_t h, uint32_t color) {
    Pool p = pool(size_t(w) * h * 4);
    fill(p, 0, w, h, w * 4, color);
    const uint32_t b = buffer(p, 0, w, h, w * 4);
    send(Msg(l.surface, 1).u(b).i(0).i(0));
    send(Msg(l.surface, 2).i(0).i(0).i(w).i(h));
    send(Msg(l.surface, 6));
  }
};

static bool info_of(Server& server, uint32_t window, desktop::WindowsWindowInfo* info) {
  String title(A);
  return server.window_info(window, info, &title);
}

static void test_layer_shell() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(1024, 768, {});
  constexpr uint32_t T = 1, B = 2, L = 4, R = 8;
  {
    LayerApp app(server);
    CHECK(app.globals["zwlr_layer_shell_v1"].second == 4);
    // a top panel across the screen, 30 high, keeping its height
    LayerApp::Layer panel = app.make(2, T | L | R, 0, 30, 30);
    CHECK(panel.cw == 1024 && panel.ch == 30 && listed(server).empty());
    app.draw(panel, 1024, 30, 0xFF203040u);
    CHECK(!app.closing && listed(server).size() == 1);
    const uint32_t pwin = listed(server)[0];
    desktop::WindowsWindowInfo info;
    CHECK(info_of(server, pwin, &info) && info.visible && !info.caption && info.tool && info.popup && !info.owned && info.layer == 1);
    CHECK(info.reserve_edge == 2 && info.reserve == 30 && info.window.x == 0 && info.window.y == 0 && info.window.width == 1024 && info.window.height == 30);
    CHECK(server.window_pixel(pwin, 5, 5) == 0xFF203040u);
    // a notification at the bottom right with margins (zone 0: inside what the panel leaves)
    LayerApp::Layer note = app.make(3, B | R, 200, 100, 0, {0, 10, 12, 0}, 0, "notifications");
    CHECK(note.cw == 200 && note.ch == 100);
    app.draw(note, 200, 100, 0xFFFFFF00u);
    std::vector<uint32_t> list = listed(server);
    CHECK(list.size() == 2);
    const uint32_t nwin = list.size() == 2 ? list[1] : 0;
    String title(A);
    CHECK(server.window_info(nwin, &info, &title) && info.layer == 2 && info.window.x == 1024 - 10 - 200 && info.window.y == 768 - 12 - 100 && info.reserve == 0);
    CHECK(title.view() == StringView("notifications"));
    // anchored to the top only: below the panel's zone, centred across
    LayerApp::Layer bar = app.make(1, T, 100, 50, 0, {4, 0, 0, 0});
    app.draw(bar, 100, 50, 0xFF00FF00u);
    list = listed(server);
    const uint32_t bwin = list.size() == 3 ? list[2] : 0;
    CHECK(info_of(server, bwin, &info) && info.layer == -1 && info.window.x == (1024 - 100) / 2 && info.window.y == 30 + 4);
    // zone -1 ignores the panel: at the very top
    LayerApp::Layer over = app.make(3, T, 64, 64, -1);
    app.draw(over, 64, 64, 0xFF0000FFu);
    list = listed(server);
    const uint32_t owin = list.size() == 4 ? list[3] : 0;
    CHECK(info_of(server, owin, &info) && info.window.y == 0);
    // the client changes the panel's size: a new configure; the bar below follows
    app.send(Msg(panel.layer, 0).u(0).u(40));
    app.send(Msg(panel.layer, 2).i(40));
    app.send(Msg(panel.surface, 6));
    CHECK(app.configured(panel) && panel.cw == 1024 && panel.ch == 40);
    app.draw(panel, 1024, 40, 0xFF203040u);
    CHECK(info_of(server, pwin, &info) && info.reserve == 40 && info.window.height == 40);
    CHECK(info_of(server, bwin, &info) && info.window.y == 40 + 4);
    // the monitors: anchored to the panel area (the taskbar left out), zones not counted twice
    desktop::IWindowsSession::Monitor m;
    m.rect = {0, 0, 1024, 768};
    m.work = {0, 40, 1024, 688};      // (what the desktop says once it knows the reservation)
    m.panel_area = {0, 0, 1024, 728};
    server.set_display(1024, 768, ArrayView<const desktop::IWindowsSession::Monitor>(&m, 1));
    CHECK(info_of(server, pwin, &info) && info.window.y == 0);
    CHECK(info_of(server, nwin, &info) && info.window.y == 728 - 12 - 100);
    CHECK(info_of(server, bwin, &info) && info.window.y == 44);
    CHECK(!app.has(panel.layer, 0));      // (the same size: no configure)
    // a maximized toplevel gets the work area
    App::Window win = app.toplevel(true, "max");
    app.show(win, 200, 100, 0xFF808080u);
    app.drop_events();
    app.send(Msg(win.toplevel, 9));      // set_maximized
    Ev mc = app.take(win.toplevel, 0);
    CHECK(mc.object == win.toplevel && mc.i(1) == 688 - 24 - 4 - 4);      // (inside the frame: border, title)
    // set_layer (2): to the overlay band
    app.send(Msg(bar.layer, 8).u(3));
    app.send(Msg(bar.surface, 6));
    CHECK(info_of(server, bwin, &info) && info.layer == 2);
    // destroying the panel frees its zone: the bar goes up
    app.send(Msg(panel.layer, 7));
    CHECK(!info_of(server, pwin, &info) || !info.visible);
    CHECK(info_of(server, bwin, &info) && info.window.y == 4);
    app.send(Msg(panel.surface, 0));
    // a null buffer unmaps; a new initial commit starts over
    app.send(Msg(note.surface, 1).u(0).i(0).i(0));
    app.send(Msg(note.surface, 6));
    CHECK(info_of(server, nwin, &info) == false || !info.visible);
    app.send(Msg(note.surface, 6));
    CHECK(app.configured(note) && note.cw == 200);
    app.draw(note, 200, 100, 0xFFFFFF00u);
    CHECK(info_of(server, nwin, &info) && info.visible);
    CHECK(!app.closing);
  }
  // keyboard: exclusive takes it when mapped; on_demand when the desktop activates it; none never
  {
    LayerApp app(server);
    const uint32_t keyboard = app.new_id();
    app.send(Msg(app.seat, 1).u(keyboard));
    app.drop_events();
    std::vector<uint32_t> before = listed(server);
    LayerApp::Layer lock = app.make(3, T | B | L | R, 0, 0, -1, {0, 0, 0, 0}, 1, "lock");
    CHECK(lock.cw == 1024 && lock.ch == 728);      // (the panel area the last block's monitors gave)
    app.draw(lock, lock.cw, lock.ch, 0xFF000000u);
    Ev enter = app.take(keyboard, 1);
    CHECK(enter.object == keyboard && enter.u(1) == lock.surface);
    const uint32_t lwin = new_window(server, before);
    desktop::WindowsWindowInfo info;
    CHECK(info_of(server, lwin, &info) && info.active && !info.popup);
    app.send(Msg(lock.layer, 7));
    app.drop_events();
    before = listed(server);
    LayerApp::Layer menu = app.make(2, T | L, 300, 400, 0, {0, 0, 0, 0}, 2, "launcher");
    app.draw(menu, 300, 400, 0xFF112233u);
    CHECK(!app.has(keyboard, 1));
    const uint32_t mwin = new_window(server, before);
    server.activate_window(mwin);
    Ev e2 = app.take(keyboard, 1);
    CHECK(e2.object == keyboard && e2.u(1) == menu.surface);
    before = listed(server);
    LayerApp::Layer quiet = app.make(2, B, 100, 20);
    app.draw(quiet, 100, 20, 0xFF445566u);
    const uint32_t qwin = new_window(server, before);
    server.activate_window(qwin);
    CHECK(!app.has(keyboard, 1) && info_of(server, qwin, &info) && !info.active && info.popup);
    // a popup of a layer surface: made without a parent, given this one; in the same band
    const uint32_t pos = app.new_id();
    app.send(Msg(app.wm, 1).u(pos));
    app.send(Msg(pos, 1).i(120).i(80));
    app.send(Msg(pos, 2).i(10).i(10).i(20).i(20));
    const uint32_t ps = app.surface(), pxdg = app.new_id(), popup = app.new_id();
    app.send(Msg(app.wm, 2).u(pxdg).u(ps));
    app.send(Msg(pxdg, 2).u(popup).u(0).u(pos));
    app.send(Msg(menu.layer, 5).u(popup));
    app.send(Msg(ps, 6));
    Ev pc = app.take(popup, 0);
    CHECK(pc.object == popup && !app.closing);
    Ev px = app.take(pxdg, 0);
    app.send(Msg(pxdg, 4).u(px.u(0)));
    before = listed(server);
    App::Window pw;
    pw.surface = ps;
    app.show(pw, 120, 80, 0xFF00FFFFu);
    const uint32_t popwin = new_window(server, before);
    CHECK(info_of(server, popwin, &info) && info.popup && info.layer == 1 && info.window.x == pc.i(0) && info.window.y == pc.i(1));
    CHECK(!app.closing);
  }
  // errors
  struct Case {
    const char* what;
    bool on_shell;      // the error is the shell's (else the layer surface's)
    uint32_t code;
    std::function<void(LayerApp&, uint32_t layer, uint32_t surface)> run;
  };
  const Case cases[] = {
      {"another role", true, 0, [](LayerApp& a, uint32_t, uint32_t) { App::Window w = a.toplevel(); a.send(Msg(a.shell, 0).u(a.new_id()).u(w.surface).u(0).u(2).s("x")); }},
      {"a buffer already", true, 2, [](LayerApp& a, uint32_t, uint32_t) {
         const uint32_t s = a.surface();
         App::Pool p = a.pool(64);
         a.send(Msg(s, 1).u(a.buffer(p, 0, 4, 4, 16)).i(0).i(0));
         a.send(Msg(a.shell, 0).u(a.new_id()).u(s).u(0).u(2).s("x"));
       }},
      {"layer 4", true, 1, [](LayerApp& a, uint32_t, uint32_t) { a.send(Msg(a.shell, 0).u(a.new_id()).u(a.surface()).u(0).u(4).s("x")); }},
      {"not an output", true, 0, [](LayerApp& a, uint32_t, uint32_t) { a.send(Msg(a.shell, 0).u(a.new_id()).u(a.surface()).u(a.compositor).u(2).s("x")); }},
      {"anchor 16", false, 2, [](LayerApp& a, uint32_t l, uint32_t) { a.send(Msg(l, 1).u(16)); }},
      {"keyboard 3", false, 3, [](LayerApp& a, uint32_t l, uint32_t) { a.send(Msg(l, 4).u(3)); }},
      {"width 0 unanchored", false, 1, [](LayerApp& a, uint32_t l, uint32_t s) { a.send(Msg(l, 0).u(0).u(10)); a.send(Msg(l, 1).u(1)); a.send(Msg(s, 6)); }},
      {"height 0 unanchored", false, 1, [](LayerApp& a, uint32_t l, uint32_t s) { a.send(Msg(l, 0).u(10).u(0)); a.send(Msg(l, 1).u(4 | 8)); a.send(Msg(s, 6)); }},
      {"buffer before configure", false, 0, [](LayerApp& a, uint32_t l, uint32_t s) {
         a.send(Msg(l, 0).u(4).u(4));
         App::Pool p = a.pool(64);
         a.send(Msg(s, 1).u(a.buffer(p, 0, 4, 4, 16)).i(0).i(0));
         a.send(Msg(s, 6));
       }},
      {"set_layer 4", false, 1, [](LayerApp& a, uint32_t l, uint32_t) { a.send(Msg(l, 8).u(4)); }},
      {"popup with a parent", false, 0, [](LayerApp& a, uint32_t l, uint32_t) {
         App::Window w = a.toplevel();
         const uint32_t pos = a.new_id();
         a.send(Msg(a.wm, 1).u(pos));
         a.send(Msg(pos, 1).i(10).i(10));
         a.send(Msg(pos, 2).i(0).i(0).i(1).i(1));
         const uint32_t ps = a.surface(), pxdg = a.new_id(), popup = a.new_id();
         a.send(Msg(a.wm, 2).u(pxdg).u(ps));
         a.send(Msg(pxdg, 2).u(popup).u(w.xdg).u(pos));
         a.send(Msg(l, 5).u(popup));
       }},
      {"popup without a parent", false, 99, [](LayerApp& a, uint32_t, uint32_t) {
         const uint32_t pos = a.new_id();
         a.send(Msg(a.wm, 1).u(pos));
         a.send(Msg(pos, 1).i(10).i(10));
         a.send(Msg(pos, 2).i(0).i(0).i(1).i(1));
         const uint32_t ps = a.surface(), pxdg = a.new_id(), popup = a.new_id();
         a.send(Msg(a.wm, 2).u(pxdg).u(ps));
         a.send(Msg(pxdg, 2).u(popup).u(0).u(pos));
         a.send(Msg(ps, 6));
       }},
  };
  for (const Case& k : cases) {
    LayerApp app(server);
    const uint32_t s = app.surface(), l = app.new_id();
    app.send(Msg(app.shell, 0).u(l).u(s).u(0).u(2).s("errors"));
    k.run(app, l, s);
    uint32_t object = 0, code = 0;
    const bool err = app.error(&object, &code);
    const uint32_t want_object = k.code == 99 ? app.wm : k.on_shell ? app.shell : l;
    const uint32_t want_code = k.code == 99 ? 3 : k.code;
    if (!err || object != want_object || code != want_code) printf("   layer error case '%s': error %d on %u code %u\n", k.what, err, object, code);
    CHECK(err && object == want_object && code == want_code && app.closing);
  }
  {      // version 3: no on_demand; set_layer from 2 on
    LayerApp app(server, 3);
    const uint32_t s = app.surface(), l = app.new_id();
    app.send(Msg(app.shell, 0).u(l).u(s).u(0).u(2).s("v3"));
    app.send(Msg(l, 4).u(2));
    uint32_t object = 0, code = 0;
    CHECK(app.error(&object, &code) && object == l && code == 3);
  }
  CHECK(server.client_count() == 0);
  free_made();
}

// Bit flips over a session that makes panels, a popup of one and changes them.
static void test_layer_fuzz() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(800, 600, {});
  std::vector<uint8_t> all;
  auto add = [&](Msg m) { std::vector<uint8_t> v = m.done(); all.insert(all.end(), v.begin(), v.end()); };
  add(Msg(1, 1).u(2));
  add(Msg(2, 0).u(1).s("wl_compositor").u(6).u(3));
  add(Msg(2, 0).u(7).s("xdg_wm_base").u(5).u(4));
  add(Msg(2, 0).u(23).s("zwlr_layer_shell_v1").u(4).u(5));
  add(Msg(2, 0).u(3).s("wl_shm").u(1).u(6));
  add(Msg(3, 0).u(10));
  add(Msg(5, 0).u(11).u(10).u(0).u(2).s("panel"));
  add(Msg(11, 1).u(1 | 4 | 8));
  add(Msg(11, 0).u(0).u(30));
  add(Msg(11, 2).i(30));
  add(Msg(11, 3).i(2).i(3).i(4).i(5));
  add(Msg(11, 4).u(2));
  add(Msg(10, 6));
  add(Msg(11, 6).u(1));
  add(Msg(6, 0).u(12).i(4096 * 32));
  add(Msg(12, 0).u(13).i(0).i(64).i(30).i(256).u(1));
  add(Msg(10, 1).u(13).i(0).i(0));
  add(Msg(10, 6));
  add(Msg(4, 1).u(14));                                   // positioner
  add(Msg(14, 1).i(50).i(40));
  add(Msg(14, 2).i(0).i(0).i(10).i(10));
  add(Msg(3, 0).u(15));
  add(Msg(4, 2).u(16).u(15));
  add(Msg(16, 2).u(17).u(0).u(14));                       // popup, no parent
  add(Msg(11, 5).u(17));                                  // get_popup
  add(Msg(15, 6));
  add(Msg(11, 8).u(3));
  add(Msg(11, 0).u(100).u(0));
  add(Msg(11, 1).u(2 | 4));
  add(Msg(10, 6));
  add(Msg(11, 7));
  add(Msg(10, 0));
  add(Msg(1, 0).u(18));
  std::vector<uint8_t> pool(4096 * 32, 0x55);
  int sessions = 0;
  auto run = [&](const std::vector<uint8_t>& bytes, size_t chunk) {
    const uint32_t c = server.open_client(1);
    if (!c) { ++g_failed; return; }
    ++sessions;
    std::vector<Fd> fds = {Fd::make_as<TestMemory>(A, &pool)};
    server.receive(c, ConstByteView(), ArrayView<Fd>(fds.data(), fds.size()));
    for (size_t at = 0; at < bytes.size();) {
      const size_t n = std::min(chunk, bytes.size() - at);
      server.receive(c, ConstByteView(bytes.data() + at, n), ArrayView<Fd>());
      at += n;
      ByteBuffer out(A);
      List<Fd> fo(A);
      if (server.take_output(c, &out, &fo)) break;
    }
    List<uint32_t> list(A);
    server.windows(&list);
    std::vector<uint32_t> target(64 * 64);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = ts.height = ts.pitch = 64;
    for (uint32_t w : list.view()) {
      graphics::SoftwareRenderer2D renderer(ts);
      uint32_t dw, dh;
      server.draw_window(w, renderer, 0, 0, &dw, &dh);
      desktop::WindowsWindowInfo info;
      info_of(server, w, &info);
      server.activate_window(w);
      server.window_pixel(w, 1, 1);
    }
    desktop::IWindowsSession::Monitor m;
    m.rect = {0, 0, 800, 600};
    m.work = {0, 30, 800, 540};
    m.panel_area = {0, 0, 800, 570};
    server.set_display(800, 600, ArrayView<const desktop::IWindowsSession::Monitor>(&m, 1));
    server.close_client(c);
    server.set_display(800, 600, {});
  };
  run(all, all.size());
  for (size_t chunk = 1; chunk <= 16; ++chunk) run(all, chunk);
  for (size_t cut = 0; cut < all.size(); cut += 4) run(std::vector<uint8_t>(all.begin(), all.begin() + long(cut)), 4096);
  std::mt19937 rng(77);
  for (int round = 0; round < 1500; ++round) {
    std::vector<uint8_t> v = all;
    const int flips = 1 + int(rng() % 4);
    for (int f = 0; f < flips; ++f) v[rng() % v.size()] ^= uint8_t(1u << (rng() % 8));
    run(v, 1 + rng() % 100);
  }
  printf("   layer fuzz: %d sessions\n", sessions);
  CHECK(server.client_count() == 0);
  free_made();
}

// ------------------------------------------------------------------ zwp_text_input_v3

static void test_text_input() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(1024, 768, {});
  desktop::IWindowsSession::TextInputState st;
  CHECK(!server.text_input(&st) && !server.text_preedit("x", 1) && !server.text_commit("x"));
  App app(server);
  CHECK(app.globals["zwp_text_input_manager_v3"].second == 1);
  const uint32_t mgr = app.bind(app.reg, "zwp_text_input_manager_v3");
  const uint32_t keyboard = app.new_id();
  app.send(Msg(app.seat, 1).u(keyboard));
  const uint32_t ti = app.new_id();
  app.send(Msg(mgr, 1).u(ti).u(app.seat));
  CHECK(!app.has(ti, 0));      // (no focus yet: no enter)
  App::Window w = app.toplevel(true, "editor");
  app.show(w, 300, 200, 0xFFFFFFFFu);
  const uint32_t win = listed(server)[0];
  server.move_window(win, {100, 100, 308, 232});      // inside at (104, 128)
  server.activate_window(win);
  Ev enter = app.take(ti, 0);
  CHECK(enter.object == ti && enter.u(0) == w.surface);
  // entered but not enabled: the input method sees nothing
  CHECK(!server.text_input(&st));
  // enable with a cursor and a content type, committed
  app.send(Msg(ti, 1));
  app.send(Msg(ti, 6).i(30).i(40).i(2).i(16));
  app.send(Msg(ti, 5).u(0x200).u(1));
  app.send(Msg(ti, 3).s("hello").i(5).i(5));
  CHECK(!server.text_input(&st));      // (not committed yet)
  app.send(Msg(ti, 7));
  CHECK(server.text_input(&st) && st.enabled && st.window == win && st.cursor.x == 104 + 30 && st.cursor.y == 128 + 40 && st.cursor.height == 16 &&
        st.hint == 0x200 && st.purpose == 1);
  // preedit and commit: each closed with done(the number of commits)
  app.drop_events();
  CHECK(server.text_preedit("u1f6", 4));
  Ev pre = app.take(ti, 2);
  CHECK(pre.object == ti && pre.s(0) == "u1f6" && pre.i(3) == 4 && pre.i(4) == 4);      // (the string takes words 0 to 2)
  Ev done = app.take(ti, 5);
  CHECK(done.object == ti && done.u(0) == 1);
  CHECK(server.text_preedit("", 0));
  pre = app.take(ti, 2);
  CHECK(pre.object == ti && pre.u(0) == 0);      // (null: no preedit)
  CHECK(server.text_commit("\xf0\x9f\x98\x80"));
  Ev com = app.take(ti, 3);
  CHECK(com.object == ti && com.s(0) == "\xf0\x9f\x98\x80" && app.take(ti, 5).u(0) == 1);
  CHECK(!server.text_commit("\xf0\x9f"));      // (a cut character is not sent)
  // a second commit: the serial follows
  app.send(Msg(ti, 7));
  app.drop_events();
  CHECK(server.text_commit("a") && app.take(ti, 5).u(0) == 2);
  // disabled: nothing takes text
  app.send(Msg(ti, 2));
  app.send(Msg(ti, 7));
  CHECK(!server.text_input(&st) && !server.text_commit("a"));
  // enabled again, then the focus goes to another client's window: leave; the other client's text input enters
  app.send(Msg(ti, 1));
  app.send(Msg(ti, 7));
  App other(server);
  const uint32_t mgr2 = other.bind(other.reg, "zwp_text_input_manager_v3");
  const uint32_t ti2 = other.new_id();
  other.send(Msg(mgr2, 1).u(ti2).u(other.seat));
  App::Window w2 = other.toplevel(true, "other");
  app.drop_events();
  other.show(w2, 100, 100, 0xFF000000u);
  CHECK(app.take(ti, 1).u(0) == w.surface);
  CHECK(other.take(ti2, 0).u(0) == w2.surface);
  CHECK(!server.text_input(&st));      // (the other one is not enabled)
  other.send(Msg(ti2, 1));
  other.send(Msg(ti2, 7));
  CHECK(server.text_input(&st) && st.window != win);
  CHECK(server.text_commit("b") && other.take(ti2, 3).s(0) == "b");
  // a text input made while its client has the keyboard is entered at once
  const uint32_t ti3 = other.new_id();
  other.send(Msg(mgr2, 1).u(ti3).u(other.seat));
  CHECK(other.take(ti3, 0).u(0) == w2.surface);
  // the focused surface goes: its text inputs are entered on nothing (no leave naming a dead surface)
  other.send(Msg(w2.toplevel, 0));
  other.send(Msg(w2.xdg, 0));
  other.send(Msg(w2.surface, 0));
  CHECK(!server.text_input(&st) && !other.closing);
  // errors: surrounding text cut in the middle of a character, a cursor past it; another seat object
  {
    App bad(server);
    const uint32_t m = bad.bind(bad.reg, "zwp_text_input_manager_v3");
    const uint32_t t = bad.new_id();
    bad.send(Msg(m, 1).u(t).u(bad.seat));
    bad.send(Msg(t, 3).s("ab\xc3").i(0).i(0));
    CHECK(bad.error() && bad.closing);
  }
  {
    App bad(server);
    const uint32_t m = bad.bind(bad.reg, "zwp_text_input_manager_v3");
    const uint32_t t = bad.new_id();
    bad.send(Msg(m, 1).u(t).u(bad.seat));
    bad.send(Msg(t, 3).s("ab").i(3).i(0));
    CHECK(bad.error() && bad.closing);
  }
  {
    App bad(server);
    const uint32_t m = bad.bind(bad.reg, "zwp_text_input_manager_v3");
    bad.send(Msg(m, 1).u(bad.new_id()).u(bad.compositor));
    CHECK(bad.error() && bad.closing);
  }
  CHECK(!app.closing && !other.closing);
}

// Bit flips over a session that enables text inputs, sets their state and is given text.
static void test_text_input_fuzz() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(800, 600, {});
  std::vector<uint8_t> all;
  auto add = [&](Msg m) { std::vector<uint8_t> v = m.done(); all.insert(all.end(), v.begin(), v.end()); };
  add(Msg(1, 1).u(2));
  add(Msg(2, 0).u(1).s("wl_compositor").u(6).u(3));
  add(Msg(2, 0).u(7).s("xdg_wm_base").u(5).u(4));
  add(Msg(2, 0).u(24).s("zwp_text_input_manager_v3").u(1).u(5));
  add(Msg(2, 0).u(4).s("wl_seat").u(8).u(6));
  add(Msg(2, 0).u(3).s("wl_shm").u(1).u(7));
  add(Msg(5, 1).u(8).u(6));                       // text input
  add(Msg(3, 0).u(10));
  add(Msg(4, 2).u(11).u(10));
  add(Msg(11, 1).u(12));
  add(Msg(10, 6));
  add(Msg(11, 4).u(1));
  add(Msg(7, 0).u(13).i(4096 * 16));
  add(Msg(13, 0).u(14).i(0).i(32).i(32).i(128).u(1));
  add(Msg(10, 1).u(14).i(0).i(0));
  add(Msg(10, 6));
  add(Msg(8, 1));
  add(Msg(8, 3).s("caf\xc3\xa9 au lait").i(5).i(2));
  add(Msg(8, 4).u(0));
  add(Msg(8, 5).u(0x1FF).u(4));
  add(Msg(8, 6).i(3).i(4).i(1).i(12));
  add(Msg(8, 7));
  add(Msg(5, 1).u(15).u(6));
  add(Msg(15, 2));
  add(Msg(15, 7));
  add(Msg(15, 0));
  add(Msg(8, 2));
  add(Msg(8, 7));
  add(Msg(1, 0).u(16));
  std::vector<uint8_t> pool(4096 * 16, 0x33);
  int sessions = 0;
  auto run = [&](const std::vector<uint8_t>& bytes, size_t chunk) {
    const uint32_t c = server.open_client(1);
    if (!c) { ++g_failed; return; }
    ++sessions;
    std::vector<Fd> fds = {Fd::make_as<TestMemory>(A, &pool)};
    server.receive(c, ConstByteView(), ArrayView<Fd>(fds.data(), fds.size()));
    for (size_t at = 0; at < bytes.size();) {
      const size_t n = std::min(chunk, bytes.size() - at);
      server.receive(c, ConstByteView(bytes.data() + at, n), ArrayView<Fd>());
      at += n;
      desktop::IWindowsSession::TextInputState st;
      if (server.text_input(&st)) {
        server.text_preedit("u4", 2);
        server.text_commit("\xe2\x82\xac");
      }
      ByteBuffer out(A);
      List<Fd> fo(A);
      if (server.take_output(c, &out, &fo)) break;
    }
    List<uint32_t> list(A);
    server.windows(&list);
    for (uint32_t w : list.view()) server.activate_window(w);
    server.close_client(c);
  };
  run(all, all.size());
  for (size_t chunk = 1; chunk <= 16; ++chunk) run(all, chunk);
  for (size_t cut = 0; cut < all.size(); cut += 4) run(std::vector<uint8_t>(all.begin(), all.begin() + long(cut)), 4096);
  std::mt19937 rng(31);
  for (int round = 0; round < 1500; ++round) {
    std::vector<uint8_t> v = all;
    const int flips = 1 + int(rng() % 4);
    for (int f = 0; f < flips; ++f) v[rng() % v.size()] ^= uint8_t(1u << (rng() % 8));
    run(v, 1 + rng() % 100);
  }
  printf("   text input fuzz: %d sessions\n", sessions);
  CHECK(server.client_count() == 0);
  free_made();
}

// ------------------------------------------------------------------ GPU buffers over the kernel's Direct3D layer (dxgk exports, Wayland/Dxg)

namespace dst = dxgk::status;

// One process of the Direct3D layer with a device on adapter `adapter`.
struct DxgProcess {
  dxgk::Kernel& k;
  dxgk::ProcessId pid;
  dxgk::Handle adapter = 0, device = 0;
  DxgProcess(dxgk::Kernel& kernel, dxgk::ProcessId id, uint32_t adapter_index = 0) : k(kernel), pid(id) {
    CHECK(dst::ok(k.open_process(pid)) && dst::ok(k.open_adapter_from_index(pid, adapter_index, &adapter)) && dst::ok(k.create_device(pid, adapter, &device)));
  }
  dxgk::Handle image(uint32_t w, uint32_t h, uint32_t flags = dxgk::kAllocTexture | dxgk::kAllocRenderTarget | dxgk::kAllocShared, dxgk::Format f = dxgk::Format::A8R8G8B8) {
    dxgk::AllocationDesc d;
    d.flags = flags;
    d.format = f;
    d.width = w, d.height = h;
    dxgk::Handle a = 0;
    return dst::ok(k.create_allocation(pid, device, d, &a)) ? a : 0;
  }
  // Writes the picture: pixel (x, y) is pattern(x, y), as 0xAARRGGBB (stored in the format's order).
  bool paint(dxgk::Handle a, uint32_t w, uint32_t h, const std::function<uint32_t(uint32_t, uint32_t)>& pattern, bool rgba = false) {
    dxgk::LockResult v;
    if (!dst::ok(k.lock(pid, a, dxgk::kLockDiscard, 0, &v))) return false;
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        uint32_t p = pattern(x, y);
        if (rgba) p = (p & 0xFF00FF00u) | (p & 0xFFu) << 16 | (p >> 16 & 0xFFu);
        memcpy(v.data.data() + uint64_t(y) * v.pitch + uint64_t(x) * 4, &p, 4);
      }
    return dst::ok(k.unlock(pid, a));
  }
};

static uint32_t pattern_a(uint32_t x, uint32_t y) { return 0xFF000000u | (x * 4) << 16 | (y * 5) << 8 | ((x + y) & 0xFF); }

// A file object of a GPU image, as /dev/dxg's export makes it (LinuxFiles.cpp).
static SharedPointer<process::OpenObject> dmabuf_object(SharedPointer<dxgk::ExportedAllocation> exported) {
  SharedPointer<process::OpenObject> o = SharedPointer<process::OpenObject>::make(A, A);
  const uint64_t bytes = dxgk::export_info(*exported).bytes;
  UniquePointer<process::DxgBufferFile> f = UniquePointer<process::DxgBufferFile>::make(A, A, util::move(exported), bytes);
  o->gpu_buffer = f.get();
  o->file = util::move(f);
  o->readable = true;
  return o;
}

// The rules of exports, imports and the kernel's reads.
static void test_dxg_exports() {
  graphics::SoftwareDevice device{A, nullptr}, other{A, nullptr};
  dxgk::Kernel k(A);
  CHECK(k.add_adapter(device, dxgk::AdapterConfig()) == 0 && k.add_adapter(other, dxgk::AdapterConfig()) == 1);
  SharedPointer<dxgk::ExportedAllocation> e;
  {
    DxgProcess p(k, 1), q(k, 2), elsewhere(k, 3, 1);
    // what cannot be exported
    CHECK(k.export_allocation(p.pid, 0x1234, &e) == dst::kInvalidHandle);
    CHECK(k.export_allocation(p.pid, p.image(16, 16, dxgk::kAllocTexture | dxgk::kAllocRenderTarget), &e) == dst::kAccessDenied && !e);
    CHECK(k.export_allocation(p.pid, p.image(16, 16, dxgk::kAllocDepthStencil | dxgk::kAllocShared, dxgk::Format::D32Float), &e) == dst::kNotSupported);
    CHECK(k.export_allocation(p.pid, p.image(16, 16, dxgk::kAllocTexture | dxgk::kAllocShared, dxgk::Format::A32B32G32R32F), &e) == dst::kNotSupported);
    dxgk::AllocationDesc buffer;
    buffer.flags = dxgk::kAllocVertexBuffer | dxgk::kAllocShared;
    buffer.size = 4096;
    dxgk::Handle vb = 0;
    CHECK(dst::ok(k.create_allocation(p.pid, p.device, buffer, &vb)) && k.export_allocation(p.pid, vb, &e) == dst::kNotSupported);
    CHECK(k.export_allocation(q.pid, p.image(16, 16), &e) == dst::kInvalidHandle);      // (another process's handle)
    // an export, what it says it is, and an import of it in another process: the same memory
    const dxgk::Handle a = p.image(40, 30);
    CHECK(a && p.paint(a, 40, 30, pattern_a));
    CHECK(dst::ok(k.export_allocation(p.pid, a, &e)) && e);
    if (!e) return;
    const dxgk::ExportInfo& info = dxgk::export_info(*e);
    CHECK(info.desc.width == 40 && info.desc.height == 30 && info.pitch == 160 && info.bytes >= 40 * 30 * 4 && info.adapter == 0);
    dxgk::Handle imported = 0;
    dxgk::AllocationDesc desc;
    CHECK(dst::ok(k.import_allocation(q.pid, q.device, *e, &imported, &desc)) && desc.width == 40 && desc.format == dxgk::Format::A8R8G8B8);
    dxgk::LockResult v;
    CHECK(dst::ok(k.lock(q.pid, imported, dxgk::kLockReadOnly, 0, &v)));
    uint32_t px = 0;
    memcpy(&px, v.data.data() + 7 * 160 + 9 * 4, 4);
    CHECK(px == pattern_a(9, 7));
    CHECK(dst::ok(k.unlock(q.pid, imported)));
    CHECK(k.import_allocation(elsewhere.pid, elsewhere.device, *e, &imported, &desc) == dst::kInvalidParameter);      // (another adapter's memory)
    CHECK(k.import_allocation(q.pid, 0x999, *e, &imported, &desc) == dst::kInvalidHandle);
    // the kernel's reads: counted, given back once each
    dxgk::LockResult r1, r2;
    CHECK(dst::ok(k.read_export(*e, &r1)) && dst::ok(k.read_export(*e, &r2)) && r1.data.data() == r2.data.data() && r1.pitch == 160);
    CHECK(dst::ok(k.end_read_export(*e)) && dst::ok(k.end_read_export(*e)) && k.end_read_export(*e) == dst::kInvalidDeviceState);
    dxgk::AcquiredImage lent;
    CHECK(dst::ok(k.acquire_export_image(*e, &lent)) && lent.device == &device && lent.image);
    CHECK(dst::ok(k.release_export_image(*e)) && k.release_export_image(*e) == dst::kInvalidDeviceState);
    // while the process has the CPU's view of it, the device's image is not lent (the picture may be half written); reads still are
    CHECK(dst::ok(k.lock(p.pid, a, 0, 0, &v)));
    CHECK(k.acquire_export_image(*e, &lent) == dst::kInvalidDeviceState);
    CHECK(dst::ok(k.read_export(*e, &r1)) && dst::ok(k.end_read_export(*e)));
    CHECK(dst::ok(k.unlock(p.pid, a)));
    // a texture that is no render target is read, never lent
    SharedPointer<dxgk::ExportedAllocation> t;
    CHECK(dst::ok(k.export_allocation(p.pid, p.image(8, 8, dxgk::kAllocTexture | dxgk::kAllocShared), &t)) && k.acquire_export_image(*t, &lent) == dst::kNotSupported);
    // an export dropped with a read still out gives it back (and is safe)
    CHECK(dst::ok(k.read_export(*t, &r1)));
    t.reset();
    // the allocation outlives its handle and its process while the export holds it
    CHECK(dst::ok(k.destroy_allocation(p.pid, a)));
  }
  k.close_process(1);
  k.close_process(2);
  k.close_process(3);
  dxgk::LockResult v;
  CHECK(dst::ok(k.read_export(*e, &v)));
  uint32_t px = 0;
  memcpy(&px, v.data.data() + 29 * 160 + 39 * 4, 4);
  CHECK(px == pattern_a(39, 29));
  CHECK(dst::ok(k.end_read_export(*e)));
  e.reset();      // (before the kernel)
}

// A dmabuf passed over the AF_UNIX transport with SCM_RIGHTS, shown by the compositor: drawn in place by a 2D renderer on the image's device (no copy),
// from the CPU's view by one that is not (a CPU renderer, another device), and still shown after the process that made it is gone.
static void test_dxg_compositor() {
  graphics::SoftwareDevice raw{A, nullptr}, other_raw{A, nullptr};
  graphics::LockedDevice device(raw), other(other_raw);
  dxgk::Kernel k(A);
  CHECK(k.add_adapter(device, dxgk::AdapterConfig()) == 0);
  SystemSync sync;
  process::UnixSocketManager sockets(sync, A);
  Server server(A);
  server.attach(4, 24);
  server.set_display(800, 600, {});
  filesystem::RamFileSystem files(A);
  constexpr uint32_t W = 64, H = 48;
  {
    wayland::SocketServer transport(server, sockets, A);
    transport.set_gpu_images(&wayland::make_dxg_image, &wayland::make_dxg_timeline, &k);
    CHECK(transport.listen(files));
    std::atomic<bool> stop{false};
    std::thread pump([&] {
      while (!stop) {
        while (transport.pump()) {}
        transport.wait(5'000'000);
      }
    });
    DxgProcess p(k, 50);
    const dxgk::Handle a = p.image(W, H);
    CHECK(p.paint(a, W, H, pattern_a));
    SharedPointer<dxgk::ExportedAllocation> exported;
    CHECK(dst::ok(k.export_allocation(p.pid, a, &exported)));
    SocketClient c(sockets);
    CHECK(c.connect());
    std::vector<uint8_t> batch;
    auto add = [&](Msg m) { std::vector<uint8_t> v = m.done(); batch.insert(batch.end(), v.begin(), v.end()); };
    add(Msg(1, 1).u(2));
    add(Msg(2, 0).u(1).s("wl_compositor").u(6).u(3));
    add(Msg(2, 0).u(7).s("xdg_wm_base").u(5).u(5));
    add(Msg(2, 0).u(9).s("zwp_linux_dmabuf_v1").u(3).u(6));
    add(Msg(3, 0).u(10));
    add(Msg(5, 2).u(11).u(10));
    add(Msg(11, 1).u(12));
    add(Msg(10, 6));
    CHECK(c.write(batch));
    Ev modifier = c.next(6, 1);
    CHECK(modifier.object == 6);
    Ev conf = c.next(11, 0);
    CHECK(conf.object == 11);
    batch.clear();
    add(Msg(11, 4).u(conf.u(0)));
    add(Msg(6, 1).u(13));
    CHECK(c.write(batch));
    CHECK(c.write(Msg(13, 1).u(0).u(0).u(W * 4).u(0).u(0).done(), dmabuf_object(exported)));
    exported.reset();      // (the descriptor in flight holds it now, as after the program closed its copy)
    batch.clear();
    add(Msg(13, 3).u(14).i(W).i(H).u(kXR24).u(0));
    add(Msg(10, 1).u(14).i(0).i(0));
    add(Msg(10, 2).i(0).i(0).i(W).i(H));
    add(Msg(10, 6));
    add(Msg(1, 0).u(15));
    CHECK(c.write(batch));
    CHECK(c.next(15, 0).object == 15);
    std::vector<uint32_t> list = listed(server);
    CHECK(list.size() == 1);
    if (!list.empty()) {
      const uint32_t win = list[0];
      CHECK(server.window_pixel(win, 9, 7) == pattern_a(9, 7));
      auto compare = [&](const std::vector<uint32_t>& out, uint32_t stride, int32_t ox, int32_t oy, uint32_t mask) {
        uint64_t differing = 0;
        for (uint32_t y = 0; y < H; ++y)
          for (uint32_t x = 0; x < W; ++x) differing += (out[size_t(oy + y) * stride + size_t(ox) + x] & mask) != (pattern_a(x, y) & mask);
        return differing;
      };
      // the desktop's renderer on the image's device: the image is sampled where it is
      graphics::DeviceRenderer2D gpu(device, A, {W + 20, H + 10});
      CHECK(gpu.valid());
      gpu.clear(0xFF303030u);
      uint32_t dw = 0, dh = 0;
      CHECK(server.draw_window(win, gpu, 10, 5, &dw, &dh) && dw == W && dh == H);
      std::vector<uint32_t> out(size_t(W + 20) * (H + 10));
      CHECK(gpu.read_pixels(ArrayView<uint32_t>(out.data(), out.size())));
      const uint64_t d1 = compare(out, W + 20, 10, 5, 0x00FFFFFFu);
      printf("   dmabuf drawn on its device: %llu of %u pixels differ\n", (unsigned long long)d1, W * H);
      CHECK(d1 == 0 && out[0] == 0xFF303030u);
      // a CPU renderer: from the CPU's view
      std::vector<uint32_t> cpu_out(size_t(W) * H, 0);
      graphics::Surface ts;
      ts.pixels = ArrayView<uint32_t>(cpu_out.data(), cpu_out.size());
      ts.width = W, ts.height = H, ts.pitch = W;
      for (int round = 0; round < 2; ++round) {      // (the second time without asking for the device's image)
        graphics::SoftwareRenderer2D cpu(ts);
        CHECK(server.draw_window(win, cpu, 0, 0, &dw, &dh));
        CHECK(compare(cpu_out, W, 0, 0, 0x00FFFFFFu) == 0);
      }
      // another device's renderer: the CPU's view again
      graphics::DeviceRenderer2D foreign(other, A, {W, H});
      CHECK(server.draw_window(win, foreign, 0, 0, &dw, &dh));
      std::vector<uint32_t> fout(size_t(W) * H);
      CHECK(foreign.read_pixels(ArrayView<uint32_t>(fout.data(), fout.size())) && compare(fout, W, 0, 0, 0x00FFFFFFu) == 0);
      // the program draws a new picture into the same image: the next draw shows it (on the device too)
      CHECK(p.paint(a, W, H, [](uint32_t x, uint32_t y) { return 0xFF000000u | (y * 3) << 16 | (x * 2); }));
      gpu.clear(0);
      CHECK(server.draw_window(win, gpu, 10, 5, &dw, &dh));
      CHECK(gpu.read_pixels(ArrayView<uint32_t>(out.data(), out.size())));
      CHECK((out[size_t(5 + 20) * (W + 20) + 10 + 30] & 0xFFFFFF) == ((20u * 3) << 16 | 30u * 2));
      // the process goes; the picture stays (the descriptor holds it) until the client lets go of the buffer
      k.close_process(p.pid);
      CHECK((server.window_pixel(win, 30, 20) & 0xFFFFFF) == ((20u * 3) << 16 | 30u * 2));
      // a descriptor of the GPU that is not the transport's kind of image: not a buffer
    }
    stop = true;
    pump.join();
  }
  CHECK(server.client_count() == 0);
}

// Client processes on their own threads paint and commit their GPU buffers, export new ones, destroy their allocations and exit, while the desktop's thread
// draws every window on the device and on the CPU and reads their pixels.
static void test_dxg_threads() {
  graphics::SoftwareDevice raw{A, nullptr};
  graphics::LockedDevice device(raw);
  dxgk::Kernel k(A);
  CHECK(k.add_adapter(device, dxgk::AdapterConfig()) == 0);
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(1600, 1200, {});
  server.set_dmabuf(true);
  // what the transport hands the server for a dmabuf
  struct ImageFd final : wayland::Memory {
    SharedPointer<wayland::GpuImage> image;
    explicit ImageFd(SharedPointer<wayland::GpuImage> i) : image(util::move(i)) {}
    ByteView map(uint64_t) override { return ByteView(); }
    wayland::GpuImage* gpu() override { return image.get(); }
  };
  constexpr int kClients = 4, kFrames = 80;
  std::atomic<int> running{kClients};
  std::atomic<int> drawn{0};
  std::thread desktop([&] {
    graphics::DeviceRenderer2D gpu(device, A, {256, 256});
    std::vector<uint32_t> canvas(256 * 256);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(canvas.data(), canvas.size());
    ts.width = ts.height = ts.pitch = 256;
    bool on_gpu = false;
    while (running > 0) {
      for (uint32_t w : listed(server)) {
        uint32_t dw, dh;
        on_gpu = !on_gpu;
        if (on_gpu) {
          if (server.draw_window(w, gpu, 0, 0, &dw, &dh)) ++drawn;
        } else {
          graphics::SoftwareRenderer2D cpu(ts);
          if (server.draw_window(w, cpu, 0, 0, &dw, &dh)) ++drawn;
        }
        server.window_pixel(w, 3, 3);
      }
      List<uint32_t> changed(A);
      List<desktop::IWindowsSession::Repaint> repainted(A);
      server.take_changes(&changed, &repainted);
    }
  });
  std::vector<std::thread> clients;
  std::atomic<int> bad{0};
  for (int i = 0; i < kClients; ++i)
    clients.emplace_back([&, i] {
      DxgProcess p(k, 100 + i);
      DmabufApp app(server);
      App::Window w = app.toplevel(true, "gpu-" + std::to_string(i));
      dxgk::Handle a = 0;
      uint32_t buffer = 0;
      for (int f = 0; f < kFrames; ++f) {
        if (f % 10 == 0) {      // a new image now and then: export, pass, the old allocation destroyed (its buffer still shown until replaced)
          const dxgk::Handle next = p.image(64, 64);
          SharedPointer<dxgk::ExportedAllocation> e;
          if (!next || !dst::ok(k.export_allocation(p.pid, next, &e))) { ++bad; break; }
          SharedPointer<process::OpenObject> object = dmabuf_object(util::move(e));
          Fd fd = Fd::make_as<ImageFd>(A, wayland::make_dxg_image(&k, A, object));
          if (a) k.destroy_allocation(p.pid, a);
          a = next;
          if (buffer) app.send(Msg(buffer, 0));
          buffer = app.immed(app.params(fd, 0, 256), 64, 64);
        }
        const uint32_t color = 0xFF000000u | uint32_t(i * 50) << 16 | uint32_t(f * 5);
        if (!p.paint(a, 64, 64, [color](uint32_t, uint32_t) { return color; })) ++bad;
        app.commit(w, buffer, 64, 64);
        if (app.closing) { ++bad; break; }
        std::this_thread::sleep_for(std::chrono::microseconds(500));      // (so that the desktop draws in between)
      }
      k.close_process(p.pid);      // (the last image's export lives on in the buffer)
      app.commit(w, buffer, 64, 64);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      --running;
    });
  for (std::thread& t : clients) t.join();
  desktop.join();
  printf("   dmabuf threads: %d windows drawn\n", drawn.load());
  CHECK(bad == 0 && drawn > kClients * 4);
  CHECK(server.client_count() == 0);
  free_made();
}

// The CPU's view of a dmabuf (mmap's copy): filled from the image at the first map, DMA_BUF_IOCTL_SYNC both ways, writes through a writable shared mapping
// reaching the window at each commit without the ioctl.
static void test_dxg_cpu_mapping() {
  graphics::SoftwareDevice device{A, nullptr};
  dxgk::Kernel k(A);
  CHECK(k.add_adapter(device, dxgk::AdapterConfig()) == 0);
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(800, 600, {});
  server.set_dmabuf(true);
  constexpr uint32_t W = 32, H = 20;
  {
    DxgProcess p(k, 70);
    const dxgk::Handle a = p.image(W, H);
    CHECK(p.paint(a, W, H, pattern_a));
    SharedPointer<dxgk::ExportedAllocation> e;
    CHECK(dst::ok(k.export_allocation(p.pid, a, &e)));
    SharedPointer<process::OpenObject> object = dmabuf_object(e);
    process::DxgBufferFile& file = *object->gpu_buffer;
    using R = process::DxgBufferFile::SyncResult;
    // without the Direct3D layer's copier there is nothing to map
    process::set_dxg_buffer_copier(nullptr);
    CHECK(!file.map_block(false) && file.sync(1) == R::NoCopier);
    process::set_dxg_buffer_copier(&dxgk::copy_export);
    CHECK(file.sync(1) == R::Ok);      // (nothing mapped yet: nothing to move)
    CHECK(file.sync(0) == R::Invalid && file.sync(8 | 1) == R::Invalid);
    process::BlockRef block = file.map_block(false);
    CHECK(block && block.get()->capacity() % 4096 == 0 && block.get()->capacity() >= W * H * 4);
    if (!block) return;
    auto word = [&](uint32_t x, uint32_t y) { uint32_t v = 0; memcpy(&v, block.get()->bytes().data() + (y * W + x) * 4, 4); return v; };
    auto put = [&](uint32_t x, uint32_t y, uint32_t v) { memcpy(block.get()->bytes().data() + (y * W + x) * 4, &v, 4); };
    CHECK(word(5, 7) == pattern_a(5, 7) && word(W - 1, H - 1) == pattern_a(W - 1, H - 1));
    // the GPU side changes: START|READ brings it in
    CHECK(p.paint(a, W, H, [](uint32_t x, uint32_t) { return 0xFF000000u | x; }));
    CHECK(word(9, 3) == pattern_a(9, 3));
    CHECK(file.sync(process::DxgBufferFile::kSyncRead) == R::Ok && word(9, 3) == 0xFF000009u);
    // the CPU writes: END|WRITE sends it to the image (which the process sees through its own handle)
    put(4, 4, 0xFF123456u);
    CHECK(file.sync(process::DxgBufferFile::kSyncEnd | process::DxgBufferFile::kSyncWrite) == R::Ok);
    dxgk::LockResult v;
    uint32_t px = 0;
    CHECK(dst::ok(k.lock(p.pid, a, dxgk::kLockReadOnly, 0, &v)));
    memcpy(&px, v.data.data() + 4 * v.pitch + 16, 4);
    CHECK(px == 0xFF123456u);
    CHECK(dst::ok(k.unlock(p.pid, a)));
    // a second descriptor of the same export shares the copy
    process::BlockRef again = file.map_block(true);
    CHECK(again && again.get() == block.get());
    // shown by the compositor; a CPU write through the writable mapping reaches the window at the next commit, without the ioctl
    struct ImageFd final : wayland::Memory {
      SharedPointer<wayland::GpuImage> image;
      explicit ImageFd(SharedPointer<wayland::GpuImage> i) : image(util::move(i)) {}
      ByteView map(uint64_t) override { return ByteView(); }
      wayland::GpuImage* gpu() override { return image.get(); }
    };
    DmabufApp app(server);
    App::Window w = app.toplevel(true, "mapped");
    const uint32_t b = app.immed(app.params(Fd::make_as<ImageFd>(A, wayland::make_dxg_image(&k, A, object)), 0, W * 4), W, H);
    app.commit(w, b, W, H);
    std::vector<uint32_t> list = listed(server);
    CHECK(list.size() == 1);
    if (!list.empty()) {
      CHECK(server.window_pixel(list[0], 4, 4) == 0xFF123456u);
      put(6, 2, 0xFF00AA00u);
      CHECK(server.window_pixel(list[0], 6, 2) != 0xFF00AA00u);      // (not before the commit)
      app.send(Msg(w.surface, 2).i(0).i(0).i(W).i(H));
      app.send(Msg(w.surface, 6));
      CHECK(server.window_pixel(list[0], 6, 2) == 0xFF00AA00u);
      // the GPU draws a new frame while the mapping is still there but the CPU wrote nothing since: the commit keeps the GPU's frame
      CHECK(p.paint(a, W, H, [](uint32_t, uint32_t) { return 0xFF445566u; }));
      app.send(Msg(w.surface, 2).i(0).i(0).i(W).i(H));
      app.send(Msg(w.surface, 6));
      CHECK(server.window_pixel(list[0], 6, 2) == 0xFF445566u);
      // the mappings go: later commits never send the old copy
      block.reset();
      again.reset();
      CHECK(p.paint(a, W, H, [](uint32_t, uint32_t) { return 0xFF778899u; }));
      app.send(Msg(w.surface, 2).i(0).i(0).i(W).i(H));
      app.send(Msg(w.surface, 6));
      CHECK(server.window_pixel(list[0], 6, 2) == 0xFF778899u);
    }
    CHECK(!app.closing);
  }
  CHECK(server.client_count() == 0);
  free_made();
}

// ------------------------------------------------------------------ wp_linux_drm_syncobj_v1 (explicit sync)

struct FakeTimeline final : wayland::Memory, wayland::GpuTimeline {
  uint64_t at = 0;
  ByteView map(uint64_t) override { return ByteView(); }
  wayland::GpuTimeline* timeline() override { return this; }
  uint64_t value() override { return at; }
  void signal(uint64_t point) override { if (point > at) at = point; }
};

struct SyncApp : DmabufApp {
  uint32_t manager = 0;
  explicit SyncApp(Server& s) : DmabufApp(s) {
    manager = bind(reg, "wp_linux_drm_syncobj_manager_v1");
    drop_events();
  }
  uint32_t timeline(Fd fd) {
    const uint32_t t = new_id();
    send(Msg(manager, 2).u(t), {fd});
    return t;
  }
  uint32_t sync_surface(uint32_t surface) {
    const uint32_t o = new_id();
    send(Msg(manager, 1).u(o).u(surface));
    return o;
  }
  void points(uint32_t so, uint32_t acquire, uint64_t a, uint32_t release, uint64_t r) {
    send(Msg(so, 1).u(acquire).u(uint32_t(a >> 32)).u(uint32_t(a)));
    send(Msg(so, 2).u(release).u(uint32_t(r >> 32)).u(uint32_t(r)));
  }
  uint32_t gpu_buffer(uint32_t color) {
    Fd g = Fd::make_as<FakeGpu>(A, 40u, 30u, color);
    return immed(params(g, 0, 160), 40, 30);
  }
};

static void test_syncobj() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(1024, 768, {});
  {      // offered with GPU buffers only
    WClient c(server);
    c.registry();
    CHECK(c.globals.count("wp_linux_drm_syncobj_manager_v1") == 0);
  }
  server.set_dmabuf(true);
  {
    SyncApp app(server);
    CHECK(app.globals.count("wp_linux_drm_syncobj_manager_v1") == 1 && app.manager != 0);
    App::Window w = app.toplevel(true, "explicit");
    Fd acquire_fd = Fd::make_as<FakeTimeline>(A), release_fd = Fd::make_as<FakeTimeline>(A);
    FakeTimeline& acquire = static_cast<FakeTimeline&>(*acquire_fd);
    FakeTimeline& release = static_cast<FakeTimeline&>(*release_fd);
    const uint32_t ta = app.timeline(acquire_fd), tr = app.timeline(release_fd);
    const uint32_t so = app.sync_surface(w.surface);
    CHECK(!app.closing);
    // a commit whose acquire point is not reached waits: nothing shown
    const uint32_t b1 = app.gpu_buffer(0xFF111111u);
    app.points(so, ta, 1, tr, 1);
    app.commit(w, b1, 40, 30);
    CHECK(listed(server).empty() && !app.closing);
    acquire.at = 1;
    CHECK(app.roundtrip());      // (any request looks again)
    std::vector<uint32_t> list = listed(server);
    CHECK(list.size() == 1);
    if (list.empty()) return;
    const uint32_t win = list[0];
    CHECK(server.window_pixel(win, 3, 3) == 0xFF111111u);
    // the next frame waits while the desktop keeps showing the first; the desktop's drawing looks again
    const uint32_t b2 = app.gpu_buffer(0xFF222222u);
    app.points(so, ta, 2, tr, 2);
    app.commit(w, b2, 40, 30);
    std::vector<uint32_t> target(100 * 100);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = ts.height = ts.pitch = 100;
    uint32_t dw, dh;
    {
      graphics::SoftwareRenderer2D r(ts);
      CHECK(server.draw_window(win, r, 0, 0, &dw, &dh));
    }
    CHECK(target[5 * 100 + 5] == 0xFF111111u && release.at == 0 && !app.has(b1, 0));
    List<uint32_t> changed(A);
    List<desktop::IWindowsSession::Repaint> repainted(A);
    server.take_changes(&changed, &repainted);
    CHECK(repainted.count() == 1 && repainted[0].window == win);      // (the desktop is asked to come back)
    acquire.at = 2;
    {
      graphics::SoftwareRenderer2D r(ts);
      CHECK(server.draw_window(win, r, 0, 0, &dw, &dh));
    }
    CHECK(target[5 * 100 + 5] == 0xFF222222u);
    CHECK(release.at == 1 && app.has(b1, 0));      // (the first buffer's use is over: its release point, and wl_buffer.release)
    // two commits waiting: the newer takes the older's place, whose buffer is let go unused
    const uint32_t b3 = app.gpu_buffer(0xFF333333u), b4 = app.gpu_buffer(0xFF444444u);
    app.points(so, ta, 3, tr, 3);
    app.commit(w, b3, 40, 30);
    app.points(so, ta, 4, tr, 4);
    app.commit(w, b4, 40, 30);
    CHECK(release.at == 3 && app.has(b3, 0) && server.window_pixel(win, 3, 3) == 0xFF222222u);
    acquire.at = 4;
    CHECK(app.roundtrip() && server.window_pixel(win, 3, 3) == 0xFF444444u && release.at == 3);
    // the same buffer again: the earlier use of it is over
    acquire.at = 5;
    app.points(so, ta, 5, tr, 6);
    app.commit(w, b4, 40, 30);
    CHECK(release.at == 4);      // (point 4 of b4's first commit; 6 is b4's current use)
    // explicit sync dropped: implicit again (a commit without points)
    app.send(Msg(so, 0));
    app.commit(w, b2, 40, 30);
    CHECK(!app.closing && release.at == 6 && server.window_pixel(win, 3, 3) == 0xFF222222u);
    // a surface that goes with a commit waiting lets the buffer go
    const uint32_t so2 = app.sync_surface(w.surface);
    const uint32_t b5 = app.gpu_buffer(0xFF555555u);
    app.points(so2, ta, 100, tr, 101);
    app.commit(w, b5, 40, 30);
    CHECK(release.at == 6);
    app.send(Msg(w.toplevel, 0));
    app.send(Msg(w.xdg, 0));
    app.send(Msg(w.surface, 0));
    CHECK(release.at == 101 && !app.closing);
    // requests on the syncobj surface of a surface that went: no_surface
    app.send(Msg(so2, 1).u(ta).u(0).u(7));
    uint32_t object = 0, code = 0;
    CHECK(app.error(&object, &code) && object == so2 && code == 1);
  }
  // the rules
  struct Case {
    const char* what;
    uint32_t code;
    std::function<void(SyncApp&, App::Window&, uint32_t so, uint32_t ta, uint32_t tr)> run;
  };
  const Case cases[] = {
      {"surface exists", 0, [](SyncApp& a, App::Window& w, uint32_t, uint32_t, uint32_t) { a.sync_surface(w.surface); }},
      {"not a timeline", 1, [](SyncApp& a, App::Window&, uint32_t, uint32_t, uint32_t) { std::vector<uint8_t> m(64); a.timeline(Fd::make_as<TestMemory>(A, &m)); }},
      {"unsupported buffer", 2, [](SyncApp& a, App::Window& w, uint32_t so, uint32_t ta, uint32_t tr) {
         App::Pool p = a.pool(40 * 30 * 4);
         const uint32_t b = a.buffer(p, 0, 40, 30, 160);
         a.points(so, ta, 1, tr, 2);
         a.commit(w, b, 40, 30);
       }},
      {"no buffer", 3, [](SyncApp& a, App::Window& w, uint32_t so, uint32_t ta, uint32_t tr) { a.points(so, ta, 1, tr, 2); a.send(Msg(w.surface, 6)); }},
      {"no acquire point", 4, [](SyncApp& a, App::Window& w, uint32_t so, uint32_t, uint32_t tr) {
         a.send(Msg(so, 2).u(tr).u(0).u(2));
         a.commit(w, a.gpu_buffer(0), 40, 30);
       }},
      {"no release point", 5, [](SyncApp& a, App::Window& w, uint32_t so, uint32_t ta, uint32_t) {
         a.send(Msg(so, 1).u(ta).u(0).u(2));
         a.commit(w, a.gpu_buffer(0), 40, 30);
       }},
      {"conflicting points", 6, [](SyncApp& a, App::Window& w, uint32_t so, uint32_t ta, uint32_t) { a.points(so, ta, 5, ta, 5); a.commit(w, a.gpu_buffer(0), 40, 30); }},
  };
  for (const Case& k : cases) {
    SyncApp app(server);
    App::Window w = app.toplevel(true, "rules");
    const uint32_t ta = app.timeline(Fd::make_as<FakeTimeline>(A)), tr = app.timeline(Fd::make_as<FakeTimeline>(A));
    const uint32_t so = app.sync_surface(w.surface);
    k.run(app, w, so, ta, tr);
    uint32_t object = 0, code = 0;
    const bool err = app.error(&object, &code);
    if (!err || code != k.code) printf("   syncobj case '%s': error %d code %u\n", k.what, err, code);
    CHECK(err && code == k.code && app.closing);
  }
  CHECK(server.client_count() == 0);
  free_made();
}

// Real fences of the Direct3D layer: exported, imported in another process (the same fence), read and raised through the compositor's timeline.
static void test_dxg_fences() {
  graphics::SoftwareDevice device{A, nullptr};
  dxgk::Kernel k(A);
  CHECK(k.add_adapter(device, dxgk::AdapterConfig()) == 0);
  {
    DxgProcess p(k, 80), q(k, 81);
    dxgk::Handle fence = 0, semaphore = 0;
    CHECK(dst::ok(k.create_sync(p.pid, p.device, dxgk::SyncType::Fence, 5, 0, &fence)));
    CHECK(dst::ok(k.create_sync(p.pid, p.device, dxgk::SyncType::Semaphore, 0, 4, &semaphore)));
    SharedPointer<dxgk::ExportedSync> e;
    CHECK(k.export_sync(p.pid, semaphore, &e) == dst::kNotSupported && !e);
    CHECK(k.export_sync(p.pid, 0x4242, &e) == dst::kInvalidHandle);
    CHECK(dst::ok(k.export_sync(p.pid, fence, &e)) && e);
    if (!e) return;
    CHECK(dxgk::exported_sync_value(*e) == 5);
    dxgk::Handle imported = 0;
    CHECK(dst::ok(k.import_sync(q.pid, q.device, *e, &imported)));
    const uint64_t nine = 9;
    CHECK(dst::ok(k.signal_sync_cpu(q.pid, ArrayView<const dxgk::Handle>(&imported, 1), ArrayView<const uint64_t>(&nine, 1))));
    uint64_t value = 0;
    CHECK(dst::ok(k.query_sync(p.pid, fence, &value)) && value == 9 && dxgk::exported_sync_value(*e) == 9);
    // through the compositor's timeline (what the transport makes of the descriptor)
    SharedPointer<process::OpenObject> o = SharedPointer<process::OpenObject>::make(A, A);
    UniquePointer<process::DxgSyncFile> f = UniquePointer<process::DxgSyncFile>::make(A, e);
    o->gpu_sync = f.get();
    o->file = util::move(f);
    SharedPointer<wayland::GpuTimeline> t = wayland::make_dxg_timeline(&k, A, o);
    CHECK(t && t->value() == 9);
    if (t) {
      t->signal(4);      // (never back)
      CHECK(t->value() == 9);
      t->signal(12);
      CHECK(dst::ok(k.query_sync(q.pid, imported, &value)) && value == 12);
    }
    // a wait of the process for a point the compositor signals is satisfied
    const uint64_t twelve = 12;
    CHECK(dst::ok(k.wait_sync_cpu(p.pid, ArrayView<const dxgk::Handle>(&fence, 1), ArrayView<const uint64_t>(&twelve, 1), true, 0)));
    // the fence outlives its process while the export holds it
    k.close_process(p.pid);
    k.close_process(q.pid);
    if (t) {
      t->signal(20);
      CHECK(t->value() == 20);
    }
    t.reset();
    o.reset();
  }

}

// ------------------------------------------------------------------ the selection, the desktop's clipboard, drag and drop

struct DataApp : App {
  uint32_t device = 0, pointer = 0;
  explicit DataApp(Server& s) : App(s) {
    device = new_id();
    send(Msg(ddm, 1).u(device).u(seat));
    pointer = new_id();
    send(Msg(seat, 0).u(pointer));
  }
  uint32_t source(std::initializer_list<const char*> types, uint32_t actions = 0) {
    const uint32_t s = new_id();
    send(Msg(ddm, 0).u(s));
    for (const char* t : types) send(Msg(s, 0).s(t));
    if (actions) send(Msg(s, 2).u(actions));
    return s;
  }
  // The offer the last selection event named (0: none), and its types.
  uint32_t selection(std::vector<std::string>* types = nullptr) {
    Ev sel = take(device, 5);
    if (sel.object == 0) return ~0u;
    const uint32_t offer = sel.u(0);
    if (types) {
      types->clear();
      for (const Ev& e : events) if (e.object == offer && e.opcode == 0) types->push_back(e.s(0));
    }
    return offer;
  }
};

static uint32_t shown_window(Server& server, App& app, App::Window& w, uint32_t color) {
  std::vector<uint32_t> before = listed(server);
  app.show(w, 100, 80, color);
  for (uint32_t win : listed(server))
    if (std::find(before.begin(), before.end(), win) == before.end()) return win;
  return 0;
}

static void test_clipboard() {
  kernel::Clipboard clipboard(A);
  CHECK(clipboard.set_text("from the desktop"));
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(1024, 768, {});
  server.set_clipboard(&clipboard);
  {
    DataApp a(server), b(server);
    App::Window wa = a.toplevel(true, "A"), wb = b.toplevel(true, "B");
    const uint32_t win_a = shown_window(server, a, wa, 0xFF0000FFu), win_b = shown_window(server, b, wb, 0xFF00FF00u);
    CHECK(win_a && win_b);
    // A gets the keyboard: it is offered the desktop's text
    a.drop_events();
    server.activate_window(win_a);
    std::vector<std::string> types;
    const uint32_t offer = a.selection(&types);
    CHECK(offer >= 0xFF000000u && offer != ~0u && types.size() == 5 && types[0] == "text/plain;charset=utf-8");
    // pasting: the transport is asked to write the text into A's pipe
    std::vector<uint8_t> pipe_a;
    Fd pa = Fd::make_as<TestMemory>(A, &pipe_a);
    a.send(Msg(offer, 1).s("text/plain;charset=utf-8"), {pa});
    List<wayland::TransportRequest> requests(A);
    server.take_requests(&requests);
    CHECK(requests.count() == 1 && requests[0].kind == wayland::TransportRequest::Kind::Write && requests[0].fd.get() == pa.get() &&
          requests[0].data && std::string(reinterpret_cast<const char*>(requests[0].data->view().data()), requests[0].data->size()) == "from the desktop");
    requests.clear();
    // A copies: it is the selection; its text is read at once for the desktop's clipboard
    const uint32_t src = a.source({"text/plain;charset=utf-8", "image/png"});
    a.send(Msg(a.device, 1).u(src).u(0));
    CHECK(a.selection(&types) != ~0u && types.size() == 2 && types[1] == "image/png");
    server.take_requests(&requests);
    CHECK(requests.count() == 1 && requests[0].kind == wayland::TransportRequest::Kind::MakePipe);
    if (requests.count() == 1) {
      std::vector<uint8_t> pipe_w;
      Fd pw = Fd::make_as<TestMemory>(A, &pipe_w);
      const size_t fds_before = a.fds.size();
      server.pipe_ready(requests[0].job, pw);
      Ev send_ev = a.take(src, 1);
      CHECK(send_ev.object == src && send_ev.s(0) == "text/plain;charset=utf-8" && a.fds.size() == fds_before + 1 && a.fds.back().get() == pw.get());
      const std::string copied = "copied in A";
      server.pipe_read_done(requests[0].job, ConstByteView(reinterpret_cast<const uint8_t*>(copied.data()), copied.size()));
      String text(A);
      CHECK(clipboard.get_text(&text) && text.view() == StringView("copied in A"));
      server.tick();
      CHECK(!a.has(src, 2));      // (the desktop's change was ours: A's selection stays)
    }
    requests.clear();
    // B gets the keyboard: offered A's types; its paste goes to A as it is (the same descriptor)
    b.drop_events();
    server.activate_window(win_b);
    const uint32_t offer_b = b.selection(&types);
    CHECK(offer_b != ~0u && offer_b != 0 && types.size() == 2);
    std::vector<uint8_t> pipe_b;
    Fd pb = Fd::make_as<TestMemory>(A, &pipe_b);
    const size_t a_fds = a.fds.size();
    b.send(Msg(offer_b, 1).s("image/png"), {pb});
    Ev forwarded = a.take(src, 1);
    CHECK(forwarded.object == src && forwarded.s(0) == "image/png" && a.fds.size() == a_fds + 1 && a.fds.back().get() == pb.get());
    // the desktop copies: A's source is cancelled; B (focused) is offered the desktop's text
    CHECK(clipboard.set_text("desktop again"));
    server.tick();
    CHECK(a.has(src, 2));
    CHECK(b.selection(&types) != ~0u && types.size() == 5);
    // A copies again and lets go of its source: the selection is empty (B is told)
    const uint32_t src2 = a.source({"text/plain"});
    a.send(Msg(a.device, 1).u(src2).u(0));
    CHECK(b.selection() != ~0u);
    a.send(Msg(src2, 1));
    CHECK(b.selection() == 0);
    // a source cannot be given twice
    a.send(Msg(a.device, 1).u(src).u(0));
    uint32_t object = 0, code = 9;
    CHECK(a.error(&object, &code) && object == a.device && code == 1);
  }
  CHECK(server.client_count() == 0);
  free_made();
}

// Pictures through the selection: the clipboard's picture offered as image/png, a client's image/png decoded onto the clipboard; broken files change nothing.
static void test_clipboard_images() {
  kernel::Clipboard clipboard(A);
  const uint32_t desk[6] = {0xFF112233u, 0x80445566u, 0xFF778899u, 0x00000000u, 0xFFFFFFFFu, 0xFF010203u};
  CHECK(clipboard.set_image(desk, 3, 2));
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(1024, 768, {});
  server.set_clipboard(&clipboard);
  {
    DataApp a(server);
    App::Window wa = a.toplevel(true, "A");
    const uint32_t win_a = shown_window(server, a, wa, 0xFF0000FFu);
    CHECK(win_a);
    // without the codecs a picture is no selection (A, the new window, has the keyboard: it was told the selection is empty)
    CHECK(a.selection() == 0);
    // with them: the picture is offered as image/png (read at the next tick)
    server.set_clipboard_images(&wayland::clipboard_to_png, &wayland::png_to_clipboard);
    server.tick();
    std::vector<std::string> types;
    const uint32_t offer = a.selection(&types);
    CHECK(offer != ~0u && offer != 0 && types.size() == 1 && types[0] == "image/png");
    // a paste of it: the PNG file, the clipboard's pixels
    std::vector<uint8_t> pipe_a;
    Fd pa = Fd::make_as<TestMemory>(A, &pipe_a);
    a.send(Msg(offer, 1).s("image/png"), {pa});
    List<wayland::TransportRequest> requests(A);
    server.take_requests(&requests);
    CHECK(requests.count() == 1 && requests[0].kind == wayland::TransportRequest::Kind::Write && requests[0].data);
    if (requests.count() == 1 && requests[0].data) {
      media::Image got(A);
      CHECK(media::decode_png(requests[0].data->view(), &got) && got.width() == 3 && got.height() == 2);
      if (got.width() == 3 && got.height() == 2) CHECK(std::equal(desk, desk + 6, got.frame(0).data()));
    }
    requests.clear();
    // asking for text gets nothing (the clipboard holds a picture)
    std::vector<uint8_t> pipe_t;
    Fd pt = Fd::make_as<TestMemory>(A, &pipe_t);
    a.send(Msg(offer, 1).s("text/plain"), {pt});
    server.take_requests(&requests);
    CHECK(requests.count() == 0);
    // A copies a picture (image/png only): it is read and decoded onto the clipboard
    media::Image pic(A);
    CHECK(pic.create(2, 2));
    const uint32_t mine[4] = {0xFFAA0000u, 0xFF00BB00u, 0xFF0000CCu, 0x7F102030u};
    std::copy(mine, mine + 4, pic.frame(0).data());
    pic.set_has_alpha(true);
    ByteBuffer png(A);
    CHECK(media::encode_png(pic, 6, &png));
    const std::vector<uint8_t> good(png.view().data(), png.view().data() + png.size());
    auto copy_from_a = [&](const std::vector<uint8_t>& bytes) {
      const uint32_t src = a.source({"image/png"});
      a.send(Msg(a.device, 1).u(src).u(0));
      a.selection();
      List<wayland::TransportRequest> rq(A);
      server.take_requests(&rq);
      CHECK(rq.count() == 1 && rq[0].kind == wayland::TransportRequest::Kind::MakePipe);
      if (rq.count() != 1) return;
      std::vector<uint8_t> pipe_w;
      Fd pw = Fd::make_as<TestMemory>(A, &pipe_w);
      server.pipe_ready(rq[0].job, pw);
      Ev ev = a.take(src, 1);
      CHECK(ev.object == src && ev.s(0) == "image/png");
      server.pipe_read_done(rq[0].job, ConstByteView(bytes.data(), bytes.size()));
    };
    copy_from_a(good);
    uint32_t w = 0, h = 0;
    CHECK(clipboard.image_size(&w, &h) && w == 2 && h == 2);
    uint32_t back[4] = {};
    CHECK(clipboard.get_image(back, 4) && std::equal(mine, mine + 4, back));
    const uint64_t seq = clipboard.sequence();
    // broken files: truncated at every length and with flipped bits; the clipboard keeps the good picture (or takes one the decoder still reads)
    for (size_t cut = 0; cut < good.size(); cut += 7) copy_from_a(std::vector<uint8_t>(good.begin(), good.begin() + cut));
    CHECK(clipboard.image_size(&w, &h) && w == 2 && h == 2 && clipboard.get_image(back, 4) && std::equal(mine, mine + 4, back));
    for (size_t bit = 0; bit < good.size() * 8; bit += 13) {
      std::vector<uint8_t> flipped = good;
      flipped[bit / 8] ^= uint8_t(1u << (bit % 8));
      copy_from_a(flipped);
    }
    CHECK(clipboard.image_size(&w, &h) && w <= 4096 && h <= 4096);      // (whatever was taken is a picture the clipboard holds)
    (void)seq;
    // a source with text and a picture: the text is what the clipboard takes
    const uint32_t both = a.source({"image/png", "text/plain"});
    a.send(Msg(a.device, 1).u(both).u(0));
    CHECK(a.selection() != ~0u);
    server.take_requests(&requests);
    CHECK(requests.count() == 1);
    if (requests.count() == 1) {
      std::vector<uint8_t> pipe_w;
      Fd pw = Fd::make_as<TestMemory>(A, &pipe_w);
      server.pipe_ready(requests[0].job, pw);
      Ev ev = a.take(both, 1);
      CHECK(ev.object == both && ev.s(0) == "text/plain");
      const std::string t = "words";
      server.pipe_read_done(requests[0].job, ConstByteView(reinterpret_cast<const uint8_t*>(t.data()), t.size()));
      String text(A);
      CHECK(clipboard.get_text(&text) && text.view() == StringView("words"));
    }
    requests.clear();
    // the desktop copies a picture again: offered as image/png, A's source cancelled
    CHECK(clipboard.set_image(desk, 3, 2));
    server.tick();
    CHECK(a.has(both, 2));
    CHECK(a.selection(&types) != ~0u && types.size() == 1 && types[0] == "image/png");
  }
  CHECK(server.client_count() == 0);
  free_made();
}

// The transport's side of a program drop: the pipe it makes for `job` (handed to the server), and the data the source wrote, read back.
static uint32_t take_pipe_job(Server& server) {
  List<wayland::TransportRequest> reqs(A);
  server.take_requests(&reqs);
  for (wayland::TransportRequest& r : reqs.view()) if (r.kind == wayland::TransportRequest::Kind::MakePipe) return r.job;
  return 0;
}
static std::string take_write(Server& server) {
  List<wayland::TransportRequest> reqs(A);
  server.take_requests(&reqs);
  for (wayland::TransportRequest& r : reqs.view())
    if (r.kind == wayland::TransportRequest::Kind::Write && r.data) return std::string(reinterpret_cast<const char*>(r.data->view().data()), r.data->size());
  return "<none>";
}

// Drags between Wayland clients and the desktop's windows (WaylandDragBridge.cpp).
static void test_desktop_drag_bridge() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(1024, 768, {});
  DataApp app(server);
  App::Window w = app.toplevel(false, "target");
  const uint32_t win = shown_window(server, app, w, 0xFF0000FFu);
  server.move_window(win, {100, 100, 100, 80});
  app.drop_events();
  auto offer_types = [&](uint32_t offer) {
    std::vector<std::string> t;
    for (const Ev& e : app.events) if (e.object == offer && e.opcode == 0) t.push_back(e.s(0));
    return t;
  };
  // the desktop's file over the window: entered with a URI list and text, copy only
  CHECK(!server.desktop_drag(win, 150, 130, "/Desktop/Documents/a b.txt"));
  Ev enter = app.take(app.device, 1);
  CHECK(enter.object == app.device && enter.u(1) == w.surface && enter.i(2) == 50 * 256 && enter.i(3) == 30 * 256);
  const uint32_t offer = enter.u(4);
  std::vector<std::string> types = offer_types(offer);
  CHECK((types == std::vector<std::string>{"text/uri-list", "text/plain;charset=utf-8", "text/plain"}) && app.take(offer, 1).u(0) == 1);
  // the client accepts the URI list: the desktop is told it would be taken; it moves
  app.send(Msg(offer, 0).u(enter.u(0)).s("text/uri-list"));
  app.send(Msg(offer, 4).u(1).u(1));
  CHECK(app.take(offer, 2).u(0) == 1);
  CHECK(server.desktop_drag(win, 152, 131, "/Desktop/Documents/a b.txt"));
  Ev motion = app.take(app.device, 3);
  CHECK(motion.object == app.device && motion.i(1) == 52 * 256);
  // dropped: the client reads the URI (percent-encoded, as Linux sees it) and the text form (the Linux path), then finishes
  CHECK(server.desktop_drop() && app.has(app.device, 4));
  std::vector<uint8_t> pipe;
  app.send(Msg(offer, 1).s("text/uri-list"), {Fd::make_as<TestMemory>(A, &pipe)});
  CHECK(take_write(server) == "file:///mnt/desktop/Documents/a%20b.txt\r\n");
  app.send(Msg(offer, 1).s("text/plain"), {Fd::make_as<TestMemory>(A, &pipe)});
  CHECK(take_write(server) == "/mnt/desktop/Documents/a b.txt");
  app.send(Msg(offer, 1).s("image/png"), {Fd::make_as<TestMemory>(A, &pipe)});
  CHECK(take_write(server) == "<none>");      // (not offered)
  app.send(Msg(offer, 3));
  CHECK(!app.closing);
  // text, and a file Linux cannot see: text only, the system path as it is
  app.drop_events();
  server.desktop_drag(win, 150, 130, "hello world");
  enter = app.take(app.device, 1);
  types = offer_types(enter.u(4));
  CHECK((types == std::vector<std::string>{"text/plain;charset=utf-8", "text/plain"}));
  server.desktop_drag(0, 0, 0, StringView());      // (left the session's windows)
  CHECK(app.has(app.device, 2));
  app.drop_events();
  server.desktop_drag(win, 150, 130, "/AppStore/Browser/cache.db");
  enter = app.take(app.device, 1);
  types = offer_types(enter.u(4));
  CHECK(types.size() == 2);
  app.send(Msg(enter.u(4), 0).u(enter.u(0)).s("text/plain"));
  CHECK(server.desktop_drag(win, 151, 130, "/AppStore/Browser/cache.db") && server.desktop_drop());
  app.send(Msg(enter.u(4), 1).s("text/plain"), {Fd::make_as<TestMemory>(A, &pipe)});
  CHECK(take_write(server) == "/AppStore/Browser/cache.db");
  app.send(Msg(enter.u(4), 3));
  // over the window without accepting: the drop is refused, the client left
  app.drop_events();
  CHECK(!server.desktop_drag(win, 150, 130, "/Linux/root/x") && !server.desktop_drop() && app.has(app.device, 2));
  // a drag ended by the desktop (Escape): left
  app.drop_events();
  server.desktop_drag(win, 150, 130, "/Linux/root/x");
  server.desktop_drag_end();
  CHECK(app.has(app.device, 2) && !server.desktop_drop());
  // a client drags its file onto the desktop: read from its source (URIs become system paths, one per line), dropped where it was let go
  app.drop_events();
  server.mouse_position(150, 130);
  server.mouse_button(1, true);
  const uint32_t src = app.source({"text/plain", "text/uri-list"}, 1);
  app.send(Msg(app.device, 0).u(src).u(w.surface).u(0).u(0));
  server.mouse_position(700, 500);
  server.mouse_button(1, false);
  CHECK(app.has(src, 3));      // dnd_drop_performed
  String items(A);
  int32_t dx = 0, dy = 0;
  CHECK(!server.take_program_drop(&items, &dx, &dy));      // (not read yet)
  uint32_t job = take_pipe_job(server);
  CHECK(job != 0);
  std::vector<uint8_t> write_end;
  server.pipe_ready(job, Fd::make_as<TestMemory>(A, &write_end));
  Ev send_ev = app.take(src, 1);
  CHECK(send_ev.object == src && send_ev.s(0) == "text/uri-list");
  const std::string uris = "file:///mnt/desktop/Documents/a%20b.txt\r\n# a comment\r\nfile:///root/x.txt\r\nhttp://example.com/\r\nfile:///bad%2\r\nfile://localhost/mnt/c/Temp/t.txt\r\n";
  server.pipe_read_done(job, ConstByteView(reinterpret_cast<const uint8_t*>(uris.data()), uris.size()));
  CHECK(server.take_program_drop(&items, &dx, &dy) && dx == 700 && dy == 500);
  CHECK(items.view() == StringView("/Desktop/Documents/a b.txt\n/Linux/root/x.txt\n/Windows/Temp/t.txt"));
  CHECK(!server.take_program_drop(&items, &dx, &dy));      // (given once)
  server.program_drop_done(true);
  CHECK(app.has(src, 4) && !app.has(src, 2));      // dnd_finished
  // nothing took it: cancelled
  app.drop_events();
  server.mouse_position(150, 130);
  server.mouse_button(1, true);
  const uint32_t src2 = app.source({"text/plain;charset=utf-8"}, 1);
  app.send(Msg(app.device, 0).u(src2).u(w.surface).u(0).u(0));
  server.mouse_position(700, 500);
  server.mouse_button(1, false);
  job = take_pipe_job(server);
  server.pipe_ready(job, Fd::make_as<TestMemory>(A, &write_end));
  CHECK(app.take(src2, 1).s(0) == "text/plain;charset=utf-8");
  const std::string text = "two\nlines";
  server.pipe_read_done(job, ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
  CHECK(server.take_program_drop(&items, &dx, &dy) && items.view() == StringView("two lines"));      // (text is one item)
  server.program_drop_done(false);
  CHECK(app.has(src2, 2));
  // URIs that are no files: cancelled at once; a source with nothing for the desktop: cancelled at the release; the source going meanwhile
  app.drop_events();
  server.mouse_position(150, 130);
  server.mouse_button(1, true);
  const uint32_t src3 = app.source({"text/uri-list"}, 1);
  app.send(Msg(app.device, 0).u(src3).u(w.surface).u(0).u(0));
  server.mouse_position(700, 500);
  server.mouse_button(1, false);
  job = take_pipe_job(server);
  server.pipe_ready(job, Fd::make_as<TestMemory>(A, &write_end));
  const std::string web = "https://example.com/\r\n";
  server.pipe_read_done(job, ConstByteView(reinterpret_cast<const uint8_t*>(web.data()), web.size()));
  CHECK(app.has(src3, 2) && !server.take_program_drop(&items, &dx, &dy));
  app.drop_events();
  server.mouse_position(150, 130);
  server.mouse_button(1, true);
  const uint32_t src4 = app.source({"image/png"}, 1);
  app.send(Msg(app.device, 0).u(src4).u(w.surface).u(0).u(0));
  server.mouse_position(700, 500);
  server.mouse_button(1, false);
  CHECK(app.has(src4, 2) && take_pipe_job(server) == 0);
  server.mouse_position(150, 130);
  server.mouse_button(1, true);
  const uint32_t src5 = app.source({"text/plain"}, 1);
  app.send(Msg(app.device, 0).u(src5).u(w.surface).u(0).u(0));
  server.mouse_position(700, 500);
  server.mouse_button(1, false);
  job = take_pipe_job(server);
  app.send(Msg(src5, 1));      // destroy
  server.pipe_ready(job, Fd::make_as<TestMemory>(A, &write_end));
  server.pipe_read_done(job, ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
  CHECK(!server.take_program_drop(&items, &dx, &dy) && !app.closing);
  // a client's drag over the desktop before it is let go: read at once, shown to the desktop, whose answer the source hears as target and action
  {
    app.drop_events();
    server.mouse_position(150, 130);
    server.mouse_button(1, true);
    const uint32_t sd = app.source({"text/uri-list", "text/plain"}, 1);
    app.send(Msg(app.device, 0).u(sd).u(w.surface).u(0).u(0));
    CHECK(take_pipe_job(server) == 0 && !server.program_drag(&items, &dx, &dy));      // (over its own surface: not the desktop's)
    server.mouse_position(600, 400);
    const uint32_t dj = take_pipe_job(server);
    CHECK(dj != 0 && !server.program_drag(&items, &dx, &dy));      // (asked for, not read yet)
    server.pipe_ready(dj, Fd::make_as<TestMemory>(A, &write_end));
    CHECK(app.take(sd, 1).s(0) == "text/uri-list");
    const std::string one = "file:///mnt/desktop/Pictures/p.png\r\nfile:///etc/hosts\r\n";
    server.pipe_read_done(dj, ConstByteView(reinterpret_cast<const uint8_t*>(one.data()), one.size()));
    server.mouse_position(610, 405);
    CHECK(server.program_drag(&items, &dx, &dy) && dx == 610 && dy == 405 && items.view() == StringView("/Desktop/Pictures/p.png\n/Linux/etc/hosts"));
    app.drop_events();
    server.program_drag_answer(true);
    Ev target = app.take(sd, 0);
    CHECK(target.object == sd && target.s(0) == "text/uri-list" && app.take(sd, 5).u(0) == 1);
    app.drop_events();
    server.program_drag_answer(true);
    CHECK(!app.has(sd, 0) && !app.has(sd, 5));      // (told once)
    server.program_drag_answer(false);
    CHECK(app.has(sd, 0) && app.take(sd, 5).u(0) == 0);
    server.program_drag_answer(true);
    // back over its surface: not the desktop's any more (the source hears target null), and the desktop no longer sees it
    app.drop_events();
    server.mouse_position(150, 130);
    CHECK(!server.program_drag(&items, &dx, &dy) && app.has(sd, 0));
    server.program_drag_answer(true);      // (ignored: it is not over the desktop)
    CHECK(!app.has(sd, 5));
    // out again: what was read is kept (no second read); let go there, the drop is what was read, at once
    server.mouse_position(620, 410);
    CHECK(take_pipe_job(server) == 0 && server.program_drag(&items, &dx, &dy));
    server.mouse_button(1, false);
    CHECK(app.has(sd, 3) && take_pipe_job(server) == 0);
    CHECK(server.take_program_drop(&items, &dx, &dy) && dx == 620 && items.view() == StringView("/Desktop/Pictures/p.png\n/Linux/etc/hosts"));
    CHECK(!server.program_drag(&items, &dx, &dy));
    server.program_drop_done(true);
    CHECK(app.has(sd, 4));
    // let go while the read is under way: the drop takes that read over
    app.drop_events();
    server.mouse_position(150, 130);
    server.mouse_button(1, true);
    const uint32_t se = app.source({"text/plain"}, 1);
    app.send(Msg(app.device, 0).u(se).u(w.surface).u(0).u(0));
    server.mouse_position(600, 400);
    const uint32_t ej = take_pipe_job(server);
    server.mouse_button(1, false);
    CHECK(ej != 0 && app.has(se, 3) && take_pipe_job(server) == 0);
    server.pipe_ready(ej, Fd::make_as<TestMemory>(A, &write_end));
    CHECK(app.take(se, 1).s(0) == "text/plain");
    const std::string word = "hello";
    server.pipe_read_done(ej, ConstByteView(reinterpret_cast<const uint8_t*>(word.data()), word.size()));
    CHECK(server.take_program_drop(&items, &dx, &dy) && items.view() == StringView("hello"));
    server.program_drop_done(false);
    CHECK(app.has(se, 2));
    // what was read has nothing for the desktop: never shown, and let go there it is cancelled
    app.drop_events();
    server.mouse_position(150, 130);
    server.mouse_button(1, true);
    const uint32_t sf = app.source({"text/uri-list"}, 1);
    app.send(Msg(app.device, 0).u(sf).u(w.surface).u(0).u(0));
    server.mouse_position(600, 400);
    const uint32_t fj = take_pipe_job(server);
    server.pipe_ready(fj, Fd::make_as<TestMemory>(A, &write_end));
    server.pipe_read_done(fj, ConstByteView(reinterpret_cast<const uint8_t*>(web.data()), web.size()));
    CHECK(!server.program_drag(&items, &dx, &dy));
    server.mouse_button(1, false);
    CHECK(app.has(sf, 2) && !app.has(sf, 3) && !server.take_program_drop(&items, &dx, &dy));
    // the source going while it is over the desktop: nothing left of it
    server.mouse_position(150, 130);
    server.mouse_button(1, true);
    const uint32_t sg = app.source({"text/plain"}, 1);
    app.send(Msg(app.device, 0).u(sg).u(w.surface).u(0).u(0));
    server.mouse_position(600, 400);
    const uint32_t gj = take_pipe_job(server);
    app.send(Msg(sg, 1));      // destroy
    server.pipe_ready(gj, Fd::make_as<TestMemory>(A, &write_end));
    server.pipe_read_done(gj, ConstByteView(reinterpret_cast<const uint8_t*>(word.data()), word.size()));
    CHECK(!server.program_drag(&items, &dx, &dy));
    server.program_drag_answer(true);
    server.mouse_button(1, false);
    CHECK(!server.take_program_drop(&items, &dx, &dy) && !app.closing);
  }
  // while a client's drag runs, the desktop's drag is not let in
  server.mouse_position(150, 130);
  server.mouse_button(1, true);
  const uint32_t src6 = app.source({"text/plain"}, 1);
  app.send(Msg(app.device, 0).u(src6).u(w.surface).u(0).u(0));
  CHECK(!server.desktop_drag(win, 150, 130, "/Linux/x"));
  server.mouse_button(1, false);
  server.program_drop_done(false);
  take_pipe_job(server);
  CHECK(!app.closing);
  // fuzzed URI lists: never a crash, every item a path of the system
  std::mt19937 rng(8);
  for (int round = 0; round < 400; ++round) {
    server.mouse_position(150, 130);
    server.mouse_button(1, true);
    const uint32_t s = app.source({"text/uri-list"}, 1);
    app.send(Msg(app.device, 0).u(s).u(w.surface).u(0).u(0));
    server.mouse_position(700, 500);
    server.mouse_button(1, false);
    job = take_pipe_job(server);
    if (job == 0) { CHECK(false); break; }
    server.pipe_ready(job, Fd::make_as<TestMemory>(A, &write_end));
    std::string data = uris;
    const int flips = 1 + int(rng() % 6);
    for (int f = 0; f < flips; ++f) data[rng() % data.size()] ^= char(1u << (rng() % 8));
    if (rng() % 3 == 0) data.resize(rng() % data.size());
    server.pipe_read_done(job, ConstByteView(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
    if (server.take_program_drop(&items, &dx, &dy)) {
      bool rooted = true;
      for (uint64_t at = 0; at < items.size();) {
        uint64_t end = at;
        while (end < items.size() && items.view()[end] != '\n') ++end;
        const StringView item = items.view().substr(at, end - at);
        if (item.empty() || item[0] != '/') rooted = false;
        at = end + 1;
      }
      CHECK(rooted);
      server.program_drop_done(round % 2 == 0);
    }
    app.drop_events();
  }
  CHECK(!app.closing);
}

static void test_drag_and_drop() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(1024, 768, {});
  {
    DataApp a(server), b(server);
    App::Window wa = a.toplevel(false, "A"), wb = b.toplevel(false, "B");
    const uint32_t win_a = shown_window(server, a, wa, 0xFF0000FFu), win_b = shown_window(server, b, wb, 0xFF00FF00u);
    server.move_window(win_a, {10, 10, 100, 80});
    server.move_window(win_b, {300, 10, 100, 80});
    a.drop_events();
    b.drop_events();
    // A starts a drag from its surface while the button is held
    server.mouse_position(40, 40);
    server.mouse_button(1, true);
    const uint32_t src = a.source({"text/uri-list"}, 1 | 2);      // copy | move
    a.send(Msg(a.device, 0).u(src).u(wa.surface).u(0).u(0));
    CHECK(!a.closing && a.has(a.pointer, 1));      // (A's pointer left: the drag has it)
    // over B: entered with an offer of A's types and actions
    server.mouse_position(330, 40);
    Ev enter = b.take(b.device, 1);
    CHECK(enter.object == b.device && enter.u(1) == wb.surface && enter.u(4) >= 0xFF000000u);
    const uint32_t offer = enter.u(4);
    CHECK(b.count(offer, 0) == 1 && b.take(offer, 1).u(0) == 3);      // (the type; source_actions copy | move)
    server.mouse_position(335, 45);
    CHECK(b.has(b.device, 3));      // motion
    // B accepts the type and prefers to move: both are told (A heard target(null) when the drag left its own surface)
    a.drop_events();
    b.send(Msg(offer, 0).u(0).s("text/uri-list"));
    CHECK(a.take(src, 0).s(0) == "text/uri-list");
    b.send(Msg(offer, 4).u(1 | 2).u(2));
    CHECK(b.take(offer, 2).u(0) == 2 && a.take(src, 5).u(0) == 2);
    // the release drops; B receives the data from A; B's finish ends it
    server.mouse_button(1, false);
    CHECK(b.has(b.device, 4) && a.has(src, 3));      // drop, dnd_drop_performed
    std::vector<uint8_t> pipe;
    Fd p = Fd::make_as<TestMemory>(A, &pipe);
    b.send(Msg(offer, 1).s("text/uri-list"), {p});
    CHECK(a.take(src, 1).s(0) == "text/uri-list" && a.fds.back().get() == p.get());
    b.send(Msg(offer, 3));
    CHECK(a.has(src, 4) && !b.closing);      // dnd_finished
    // a drag released over nothing that accepts: cancelled
    server.mouse_position(40, 40);
    server.mouse_button(1, true);
    const uint32_t src2 = a.source({"text/plain"});
    a.send(Msg(a.device, 0).u(src2).u(wa.surface).u(0).u(0));
    server.mouse_position(600, 400);
    server.mouse_button(1, false);
    CHECK(a.has(src2, 3));      // (let go over the desktop: handed to it, test_desktop_drag_bridge)
    server.program_drop_done(false);
    {
      List<wayland::TransportRequest> reqs(A);
      server.take_requests(&reqs);
      for (wayland::TransportRequest& r : reqs.view()) {
        if (r.kind != wayland::TransportRequest::Kind::MakePipe) continue;
        std::vector<uint8_t> end;
        server.pipe_ready(r.job, Fd::make_as<TestMemory>(A, &end));
        const std::string t = "x";
        server.pipe_read_done(r.job, ConstByteView(reinterpret_cast<const uint8_t*>(t.data()), t.size()));
      }
      String items(A);
      int32_t dx = 0, dy = 0;
      CHECK(server.take_program_drop(&items, &dx, &dy));
      server.program_drop_done(false);
    }
    CHECK(a.has(src2, 2));      // nothing on the desktop took it: cancelled
    // a drag without a button held is cancelled at once
    const uint32_t src3 = a.source({"text/plain"});
    a.send(Msg(a.device, 0).u(src3).u(wa.surface).u(0).u(0));
    CHECK(a.has(src3, 2));
    // finishing without a drop is an error
    b.send(Msg(offer, 3));
    uint32_t code = 9;
    CHECK(b.error(nullptr, &code) && code == 0);
  }
  CHECK(server.client_count() == 0);
  free_made();
}

// ------------------------------------------------------------------ buffer scale and transform, wp_viewporter, wp_fractional_scale_v1

static uint32_t pattern_px(uint32_t x, uint32_t y) { return 0xFF000000u | x << 8 | y; }

static void test_mapping() {
  Server server(A);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(1024, 768, {});
  server.set_dmabuf(true);
  constexpr int32_t BW = 40, BH = 20;
  struct Case {
    const char* what;
    std::function<void(App&, App::Window&, uint32_t viewport)> set;
    int32_t sw, sh;                       // the surface's size
    int32_t sx, sy, bx, by;               // a surface pixel and the buffer pixel it shows
  };
  const Case cases[] = {
      {"scale 2", [](App& a, App::Window& w, uint32_t) { a.send(Msg(w.surface, 8).i(2)); }, 20, 10, 5, 3, 11, 7},
      {"90", [](App& a, App::Window& w, uint32_t) { a.send(Msg(w.surface, 7).i(1)); }, 20, 40, 2, 30, 30, 17},
      {"180", [](App& a, App::Window& w, uint32_t) { a.send(Msg(w.surface, 7).i(2)); }, 40, 20, 3, 5, 36, 14},
      {"270", [](App& a, App::Window& w, uint32_t) { a.send(Msg(w.surface, 7).i(3)); }, 20, 40, 2, 30, 9, 2},
      {"flipped", [](App& a, App::Window& w, uint32_t) { a.send(Msg(w.surface, 7).i(4)); }, 40, 20, 3, 5, 36, 5},
      {"flipped 90", [](App& a, App::Window& w, uint32_t) { a.send(Msg(w.surface, 7).i(5)); }, 20, 40, 2, 30, 30, 2},
      {"90 and scale 2", [](App& a, App::Window& w, uint32_t) { a.send(Msg(w.surface, 7).i(1)); a.send(Msg(w.surface, 8).i(2)); }, 10, 20, 1, 15, 31, 17},
      {"viewport crop and stretch", [](App& a, App::Window&, uint32_t v) {
         a.send(Msg(v, 1).i(10 * 256).i(5 * 256).i(20 * 256).i(10 * 256));
         a.send(Msg(v, 2).i(40).i(20));
       }, 40, 20, 39, 19, 29, 14},
      {"viewport crop only", [](App& a, App::Window&, uint32_t v) { a.send(Msg(v, 1).i(10 * 256).i(5 * 256).i(20 * 256).i(10 * 256)); }, 20, 10, 0, 0, 10, 5},
      {"viewport size only", [](App& a, App::Window&, uint32_t v) { a.send(Msg(v, 2).i(80).i(40)); }, 80, 40, 79, 39, 39, 19},
  };
  for (const Case& k : cases) {
    App app(server);
    const uint32_t viewporter = app.bind(app.reg, "wp_viewporter");
    App::Window w = app.toplevel(true, k.what);
    const uint32_t viewport = app.new_id();
    app.send(Msg(viewporter, 1).u(viewport).u(w.surface));
    k.set(app, w, viewport);
    App::Pool p = app.pool(size_t(BW) * BH * 4);
    for (int32_t y = 0; y < BH; ++y)
      for (int32_t x = 0; x < BW; ++x) {
        const uint32_t v = pattern_px(uint32_t(x), uint32_t(y));
        memcpy(p.memory->data() + (y * BW + x) * 4, &v, 4);
      }
    const uint32_t b = app.buffer(p, 0, BW, BH, BW * 4);
    app.send(Msg(w.surface, 1).u(b).i(0).i(0));
    app.send(Msg(w.surface, 6));
    std::vector<uint32_t> list = listed(server);
    CHECK(list.size() == 1 && !app.closing);
    if (list.size() != 1) continue;
    desktop::WindowsWindowInfo info;
    String title(A);
    CHECK(server.window_info(list[0], &info, &title));
    const bool size_ok = int32_t(info.surface.width) == k.sw && int32_t(info.surface.height) == k.sh;
    const uint32_t got = server.window_pixel(list[0], k.sx, k.sy);
    // drawn: the same pixel lands where the surface's is (the renderer samples the buffer through the map; a stretch filters, so only the size is checked there)
    std::vector<uint32_t> target(100 * 100, 0);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = ts.height = ts.pitch = 100;
    {
      graphics::SoftwareRenderer2D r(ts);
      uint32_t dw = 0, dh = 0;
      CHECK(server.draw_window(list[0], r, 0, 0, &dw, &dh) && int32_t(dw) == k.sw && int32_t(dh) == k.sh);
    }
    const bool plain = std::string(k.what).find("scale") == std::string::npos && std::string(k.what).find("stretch") == std::string::npos &&
                       std::string(k.what).find("size only") == std::string::npos;
    const uint32_t drawn = target[size_t(k.sy) * 100 + size_t(k.sx)];
    if (!size_ok || got != pattern_px(uint32_t(k.bx), uint32_t(k.by)) || (plain && drawn != got))
      printf("   mapping '%s': surface %ux%u, pixel %08x (want %08x), drawn %08x\n", k.what, info.surface.width, info.surface.height, got,
             pattern_px(uint32_t(k.bx), uint32_t(k.by)), drawn);
    CHECK(size_ok && got == pattern_px(uint32_t(k.bx), uint32_t(k.by)));
    if (plain) CHECK(drawn == got);
    CHECK(target[100 * 100 - 1] == 0);      // (nothing outside the surface)
  }
  {      // a GPU buffer turned: read through its image
    DmabufApp app(server);
    App::Window w = app.toplevel(true, "gpu turned");
    Fd g = Fd::make_as<FakeGpu>(A, 40u, 20u, 0u);
    FakeGpu& image = static_cast<FakeGpu&>(*g);
    for (uint32_t y = 0; y < 20; ++y)
      for (uint32_t x = 0; x < 40; ++x) image.pixels[y * 40 + x] = pattern_px(x, y);
    const uint32_t b = app.immed(app.params(g, 0, 160), 40, 20);
    app.send(Msg(w.surface, 7).i(2));
    app.commit(w, b, 40, 20);
    std::vector<uint32_t> list = listed(server);
    CHECK(list.size() == 1);
    if (!list.empty()) {
      CHECK(server.window_pixel(list[0], 3, 5) == pattern_px(36, 14));
      std::vector<uint32_t> target(100 * 100, 0);
      graphics::Surface ts;
      ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
      ts.width = ts.height = ts.pitch = 100;
      graphics::SoftwareRenderer2D r(ts);
      uint32_t dw = 0, dh = 0;
      CHECK(server.draw_window(list[0], r, 0, 0, &dw, &dh) && target[5 * 100 + 3] == pattern_px(36, 14) && image.draws == 0);      // (read, not drawn in place)
    }
  }
  {      // fractional scale: the preferred scale is 1 (120); version 6 surfaces are told the preferred buffer scale and transform
    App app(server);
    const uint32_t fsm = app.bind(app.reg, "wp_fractional_scale_manager_v1");
    const uint32_t s = app.surface();
    CHECK(app.take(s, 2).i(0) == 1 && app.has(s, 3));
    const uint32_t f = app.new_id();
    app.send(Msg(fsm, 1).u(f).u(s));
    CHECK(app.take(f, 0).u(0) == 120);
    app.send(Msg(fsm, 1).u(app.new_id()).u(s));
    uint32_t code = 9;
    CHECK(app.error(nullptr, &code) && code == 0);
  }
  // the rules
  struct Rule {
    const char* what;
    uint32_t code;
    std::function<void(App&, App::Window&, uint32_t viewporter, uint32_t viewport)> run;
  };
  const Rule rules[] = {
      {"size not a multiple of the scale", 2, [](App& a, App::Window& w, uint32_t, uint32_t) {
         a.send(Msg(w.surface, 8).i(3));
         a.show(w, 40, 20, 0xFF000000u);
       }},
      {"source outside the buffer", 2, [](App& a, App::Window& w, uint32_t, uint32_t v) {
         a.send(Msg(v, 1).i(30 * 256).i(0).i(20 * 256).i(10 * 256));
         a.show(w, 40, 20, 0xFF000000u);
       }},
      {"fractional source without a destination", 1, [](App& a, App::Window& w, uint32_t, uint32_t v) {
         a.send(Msg(v, 1).i(0).i(0).i(10 * 256 + 128).i(10 * 256));
         a.show(w, 40, 20, 0xFF000000u);
       }},
      {"negative source", 0, [](App& a, App::Window&, uint32_t, uint32_t v) { a.send(Msg(v, 1).i(-256).i(0).i(256).i(256)); }},
      {"zero destination", 0, [](App& a, App::Window&, uint32_t, uint32_t v) { a.send(Msg(v, 2).i(0).i(10)); }},
      {"second viewport", 0, [](App& a, App::Window& w, uint32_t vp, uint32_t) { a.send(Msg(vp, 1).u(a.new_id()).u(w.surface)); }},
      {"bad scale", 0, [](App& a, App::Window& w, uint32_t, uint32_t) { a.send(Msg(w.surface, 8).i(0)); }},
      {"bad transform", 1, [](App& a, App::Window& w, uint32_t, uint32_t) { a.send(Msg(w.surface, 7).i(8)); }},
  };
  for (const Rule& k : rules) {
    App app(server);
    const uint32_t viewporter = app.bind(app.reg, "wp_viewporter");
    App::Window w = app.toplevel(true, "rule");
    const uint32_t viewport = app.new_id();
    app.send(Msg(viewporter, 1).u(viewport).u(w.surface));
    k.run(app, w, viewporter, viewport);
    uint32_t code = 99;
    const bool err = app.error(nullptr, &code);
    if (!err || code != k.code) printf("   mapping rule '%s': error %d code %u\n", k.what, err, code);
    CHECK(err && code == k.code && app.closing);
  }
  {      // the viewport of a surface that went
    App app(server);
    const uint32_t viewporter = app.bind(app.reg, "wp_viewporter");
    const uint32_t s = app.surface();
    const uint32_t v = app.new_id();
    app.send(Msg(viewporter, 1).u(v).u(s));
    app.send(Msg(s, 0));
    app.send(Msg(v, 2).i(10).i(10));
    uint32_t code = 9;
    CHECK(app.error(nullptr, &code) && code == 3);
  }
  CHECK(server.client_count() == 0);
  free_made();
}

// ------------------------------------------------------------------ the other protocols, pings, the minimized state

static uint64_t g_fake_ms = 1000;
static uint64_t fake_clock() { return g_fake_ms; }

static void test_extras() {
  Server server(A, &fake_clock);
  server.set_fd_maker(&make_test_fd, nullptr);
  server.attach(4, 24);
  server.set_display(1024, 768, {});
  {
    DataApp a(server), b(server);
    App::Window wa = a.toplevel(true, "A"), wb = b.toplevel(true, "B");
    const uint32_t win_a = shown_window(server, a, wa, 0xFF0000FFu), win_b = shown_window(server, b, wb, 0xFF00FF00u);
    server.move_window(win_a, {10, 10, 120, 120});
    server.move_window(win_b, {400, 10, 120, 120});
    desktop::WindowsWindowInfo ia, ib;
    String title(A);
    auto info = [&](uint32_t w, desktop::WindowsWindowInfo* out) { CHECK(server.window_info(w, out, &title)); };
    // --- xdg_activation_v1: a token from A lets it take the focus from B, once
    server.activate_window(win_b);
    const uint32_t act_a = a.bind(a.reg, "xdg_activation_v1");
    const uint32_t token = a.new_id();
    a.send(Msg(act_a, 1).u(token));
    a.send(Msg(token, 3));
    const std::string text = a.take(token, 0).s(0);
    CHECK(!text.empty());
    a.send(Msg(act_a, 2).s(text).u(wa.surface));
    info(win_a, &ia);
    info(win_b, &ib);
    CHECK(ia.active && !ib.active);
    server.activate_window(win_b);
    a.send(Msg(act_a, 2).s(text).u(wa.surface));      // (spent)
    a.send(Msg(act_a, 2).s("made-up").u(wa.surface));
    info(win_a, &ia);
    CHECK(!ia.active);
    // --- wp_cursor_shape_v1: the text cursor over A
    server.activate_window(win_a);
    info(win_a, &ia);
    server.mouse_position(ia.surface.x + 20, ia.surface.y + 20);
    const uint32_t csm = a.bind(a.reg, "wp_cursor_shape_manager_v1");
    const uint32_t shape = a.new_id();
    a.send(Msg(csm, 1).u(shape).u(a.pointer));
    a.send(Msg(shape, 1).u(0).u(9));      // text
    desktop::IWindowsSession::Pointer p;
    server.pointer(&p);
    CHECK(!p.hidden && p.cursor == 0 && p.system_id == 32513);
    a.send(Msg(shape, 1).u(0).u(29));      // nwse-resize
    server.pointer(&p);
    CHECK(p.system_id == 32642);
    // --- zwp_relative_pointer_v1 and a persistent lock while A is active and has the pointer
    const uint32_t rpm = a.bind(a.reg, "zwp_relative_pointer_manager_v1"), pc = a.bind(a.reg, "zwp_pointer_constraints_v1");
    const uint32_t rel = a.new_id();
    a.send(Msg(rpm, 1).u(rel).u(a.pointer));
    const uint32_t lock = a.new_id();
    a.send(Msg(pc, 1).u(lock).u(wa.surface).u(a.pointer).u(0).u(2));
    CHECK(a.has(lock, 0) && server.cursor_confined());      // locked
    int32_t cx0 = 0, cy0 = 0, cx = 0, cy = 0;
    server.cursor(&cx0, &cy0);
    a.drop_events();
    server.mouse_motion(7, -3);
    Ev rm = a.take(rel, 0);
    CHECK(rm.object == rel && rm.i(2) == 7 * 256 && rm.i(3) == -3 * 256);
    server.cursor(&cx, &cy);
    CHECK(cx == cx0 && cy == cy0);      // (locked: the pointer stays)
    server.activate_window(win_b);
    CHECK(a.has(lock, 1) && !server.cursor_confined());      // unlocked: A is not active
    server.activate_window(win_a);
    CHECK(a.has(lock, 0) && server.cursor_confined());      // (persistent: back)
    a.send(Msg(pc, 2).u(a.new_id()).u(wa.surface).u(a.pointer).u(0).u(1));
    uint32_t object = 0, code = 0;
    CHECK(a.error(&object, &code) && code == 1);      // already_constrained
  }
  {      // a one-shot confinement: the pointer stays in the surface; it ends with the focus and does not come back
    App app(server);
    const uint32_t pointer = app.new_id();
    app.send(Msg(app.seat, 0).u(pointer));
    App::Window w = app.toplevel(true, "confined");
    const uint32_t win = shown_window(server, app, w, 0xFF101010u);
    server.move_window(win, {50, 50, 120, 120});
    server.activate_window(win);
    desktop::WindowsWindowInfo info;
    String title(A);
    CHECK(server.window_info(win, &info, &title));
    server.mouse_position(info.surface.x + 10, info.surface.y + 10);
    const uint32_t pc = app.bind(app.reg, "zwp_pointer_constraints_v1");
    const uint32_t conf = app.new_id();
    app.send(Msg(pc, 2).u(conf).u(w.surface).u(pointer).u(0).u(1));
    CHECK(app.has(conf, 0) && server.cursor_confined());
    server.mouse_motion(-500, -500);
    int32_t x = 0, y = 0;
    server.cursor(&x, &y);
    CHECK(x == info.surface.x && y == info.surface.y);      // (at the surface's corner, not past it)
    server.activate_window(0);
    CHECK(app.has(conf, 1) && !server.cursor_confined());
    server.activate_window(win);
    app.drop_events();
    server.mouse_motion(1, 1);
    CHECK(!app.has(conf, 0) && !server.cursor_confined());      // (one-shot: over)
  }
  {      // --- zwp_primary_selection_v1 between two clients
    App a(server), b(server);
    App::Window wa = a.toplevel(true, "PA"), wb = b.toplevel(true, "PB");
    const uint32_t win_a = shown_window(server, a, wa, 0xFF0000FFu), win_b = shown_window(server, b, wb, 0xFF00FF00u);
    const uint32_t ma = a.bind(a.reg, "zwp_primary_selection_device_manager_v1"), mb = b.bind(b.reg, "zwp_primary_selection_device_manager_v1");
    const uint32_t da = a.new_id(), db = b.new_id();
    a.send(Msg(ma, 1).u(da).u(a.seat));
    b.send(Msg(mb, 1).u(db).u(b.seat));
    server.activate_window(win_a);
    const uint32_t src = a.new_id();
    a.send(Msg(ma, 0).u(src));
    a.send(Msg(src, 0).s("text/plain;charset=utf-8"));
    a.send(Msg(da, 0).u(src).u(0));
    b.drop_events();
    server.activate_window(win_b);
    Ev sel = b.take(db, 1);
    CHECK(sel.object == db && sel.u(0) >= 0xFF000000u);
    const uint32_t offer = sel.u(0);
    CHECK(b.take(offer, 0).s(0) == "text/plain;charset=utf-8");
    std::vector<uint8_t> pipe;
    Fd fd = Fd::make_as<TestMemory>(A, &pipe);
    b.send(Msg(offer, 0).s("text/plain;charset=utf-8"), {fd});
    Ev sent = a.take(src, 0);
    CHECK(sent.object == src && sent.s(0) == "text/plain;charset=utf-8" && a.fds.back().get() == fd.get());
    // B takes the primary selection: A's source is cancelled
    const uint32_t src_b = b.new_id();
    b.send(Msg(mb, 0).u(src_b));
    b.send(Msg(db, 0).u(src_b).u(0));
    CHECK(a.has(src, 1));
  }
  {      // --- wp_presentation: presented when drawn, discarded when replaced first
    App app(server);
    const uint32_t pres = app.bind(app.reg, "wp_presentation");
    CHECK(app.take(pres, 0).u(0) == 1);      // clock_id: CLOCK_MONOTONIC
    App::Window w = app.toplevel(true, "presented");
    const uint32_t win = shown_window(server, app, w, 0xFF202020u);
    const uint32_t f1 = app.new_id();
    app.send(Msg(pres, 1).u(w.surface).u(f1));
    app.show(w, 100, 80, 0xFF303030u);
    const uint32_t f2 = app.new_id();
    app.send(Msg(pres, 1).u(w.surface).u(f2));
    app.show(w, 100, 80, 0xFF404040u);
    CHECK(app.has(f1, 2));      // discarded: never shown
    std::vector<uint32_t> target(200 * 200);
    graphics::Surface ts;
    ts.pixels = ArrayView<uint32_t>(target.data(), target.size());
    ts.width = ts.height = ts.pitch = 200;
    graphics::SoftwareRenderer2D r(ts);
    uint32_t dw, dh;
    g_fake_ms = 123456;
    CHECK(server.draw_window(win, r, 0, 0, &dw, &dh));
    Ev pr = app.take(f2, 1);
    CHECK(pr.object == f2 && pr.u(1) == 123 && pr.u(2) == 456000000u && pr.u(3) == 16666667u);
    CHECK(app.take(1, 1).u(0) == f1);      // (delete_id: the feedback objects go)
  }
  {      // --- idle inhibitors are counted; xdg-foreign makes another client's window the parent
    App a(server), b(server);
    const uint32_t idle = a.bind(a.reg, "zwp_idle_inhibit_manager_v1");
    const uint32_t s = a.surface();
    const uint32_t inh = a.new_id();
    a.send(Msg(idle, 1).u(inh).u(s));
    CHECK(server.idle_inhibited());
    a.send(Msg(inh, 0));
    CHECK(!server.idle_inhibited());
    App::Window wa = a.toplevel(true, "parent"), wb = b.toplevel(true, "dialog");
    shown_window(server, a, wa, 0xFF0000FFu);
    const uint32_t win_b = shown_window(server, b, wb, 0xFF00FF00u);
    const uint32_t exporter = a.bind(a.reg, "zxdg_exporter_v2"), importer = b.bind(b.reg, "zxdg_importer_v2");
    const uint32_t exported = a.new_id();
    a.send(Msg(exporter, 1).u(exported).u(wa.surface));
    const std::string handle = a.take(exported, 0).s(0);
    CHECK(!handle.empty());
    const uint32_t imported = b.new_id();
    b.send(Msg(importer, 1).u(imported).s(handle));
    b.send(Msg(imported, 1).u(wb.surface));
    desktop::WindowsWindowInfo info;
    String title(A);
    CHECK(server.window_info(win_b, &info, &title) && info.owned);
    a.send(Msg(wa.toplevel, 0));      // the parent goes: the import hears it, the dialog has no parent
    a.send(Msg(wa.xdg, 0));
    a.send(Msg(wa.surface, 0));
    CHECK(b.has(imported, 0));
    CHECK(server.window_info(win_b, &info, &title) && !info.owned);
    const uint32_t unknown = b.new_id();
    b.send(Msg(importer, 1).u(unknown).s("no-such-handle"));
    CHECK(b.has(unknown, 0));
    const uint32_t late = b.new_id();
    b.send(Msg(importer, 1).u(late).s(handle));      // (the export is gone: destroyed at once)
    CHECK(b.has(late, 0));
  }
  {      // --- pings: a client that does not answer is shown as not responding until it does; the minimized state
    App app(server);
    App::Window w = app.toplevel(true, "Editor");
    const uint32_t win = shown_window(server, app, w, 0xFF505050u);
    Ev mapped_ping = app.take(app.wm, 0);      // (being shown activated it, which pings: answered)
    CHECK(mapped_ping.object == app.wm);
    app.send(Msg(app.wm, 3).u(mapped_ping.u(0)));
    g_fake_ms = 200000;
    app.drop_events();
    server.window_command(win, desktop::IWindowsSession::Command::Close);
    Ev ping = app.take(app.wm, 0);
    CHECK(ping.object == app.wm && app.has(w.toplevel, 1));      // ping, and the close
    desktop::WindowsWindowInfo info;
    String title(A);
    g_fake_ms += 6000;
    server.tick();
    CHECK(server.window_info(win, &info, &title) && title.view() == StringView("Editor (Not Responding)"));
    app.send(Msg(app.wm, 3).u(ping.u(0)));      // pong
    CHECK(server.window_info(win, &info, &title) && title.view() == StringView("Editor"));
    app.drop_events();
    server.window_command(win, desktop::IWindowsSession::Command::Minimize);
    Ev conf = app.take(w.toplevel, 0);
    bool suspended = false;
    for (size_t i = 0; i + 3 < conf.a.size() - 12; i += 4) suspended |= conf.u(3 + i / 4) == 9;
    CHECK(conf.object == w.toplevel && suspended);
  }
  CHECK(server.client_count() == 0);
  free_made();
}

int main() {
  test_memory_file();
  test_registry();
  test_toplevel();
  test_geometry_and_csd();
  test_popups();
  test_subsurfaces();
  test_seat();
  test_touch();
  test_pen();
  test_errors();
  test_fuzz();
  test_dmabuf();
  test_dmabuf_fuzz();
  test_yuv_convert();
  test_yuv_buffers();
  test_yuv_fuzz();
  test_layer_shell();
  test_layer_fuzz();
  test_text_input();
  test_text_input_fuzz();
  test_socket_transport();
  test_dxg_exports();
  test_dxg_compositor();
  test_dxg_cpu_mapping();
  test_syncobj();
  test_clipboard();
  test_clipboard_images();
  test_mapping();
  test_extras();
  test_drag_and_drop();
  test_desktop_drag_bridge();
  test_dxg_fences();
  test_dxg_threads();
  test_threads();
  free_made();
  if (g_failed) {
    printf("WaylandServerTest: %d FAILED\n", g_failed.load());
    return 1;
  }
  printf("WaylandServerTest: all passed\n");
  return 0;
}
