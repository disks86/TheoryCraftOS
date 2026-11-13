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

// The kernel's X server (src/X11) on the development machine, for real X clients (libX11, libxcb, xprop, ...): it listens on the host's abstract socket
// "\0/tmp/.X11-unix/X<display>" (what libxcb tries first for DISPLAY=:<display>; no file is made) and feeds the bytes to x11::Server, which stands in
// for the desktop at 1280x800 with frames of 6 + 19 pixels. Used by tests/x11_interop.sh.
//
//   X11HostServer <display> [<seconds>] [<dump.ppm>]   serves until the time is up or SIGTERM; the top-level windows' pictures go to the PPM at the end

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include "X11/X11Server.hpp"

namespace {

struct Allocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { free(p); }
};
Allocator g_allocator;
volatile sig_atomic_t g_stop = 0;

uint64_t now_ms() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1000 + uint64_t(t.tv_nsec) / 1000000;
}

struct Connection {
  int fd;
  uint32_t client;
  std::vector<uint8_t> pending;
  bool gone = false;
};

void dump(x11::Server& server, const char* path) {
  List<uint32_t> windows(g_allocator);
  server.windows(&windows);
  FILE* f = fopen(path, "wb");
  if (!f) return;
  // the windows side by side, each its picture
  uint32_t total_w = 0, max_h = 1;
  struct Shot { uint32_t id, w, h; };
  std::vector<Shot> shots;
  for (uint32_t id : windows.view()) {
    desktop::WindowsWindowInfo info;
    String title(g_allocator);
    if (!server.window_info(id, &info, &title)) continue;
    shots.push_back({id, info.surface.width, info.surface.height});
    total_w += info.surface.width + 4;
    if (info.surface.height > max_h) max_h = info.surface.height;
    printf("window 0x%x \"%s\" frame %d,%d %ux%u inside %ux%u%s%s\n", id, title.c_str(), info.window.x, info.window.y, info.window.width, info.window.height,
           info.surface.width, info.surface.height, info.caption ? " decorated" : "", info.popup ? " popup" : "");
  }
  if (total_w == 0) total_w = 1;
  fprintf(f, "P6\n%u %u\n255\n", total_w, max_h);
  for (uint32_t y = 0; y < max_h; ++y) {
    for (const Shot& s : shots) {
      for (uint32_t x = 0; x < s.w + 4; ++x) {
        uint32_t v = x < s.w && y < s.h ? server.picture_pixel(s.id, int32_t(x), int32_t(y)) : 0x808080u;
        const uint8_t rgb[3] = {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        fwrite(rgb, 1, 3, f);
      }
    }
  }
  fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <display> [<seconds>] [<dump.ppm>]\n", argv[0]);
    return 2;
  }
  const int display = atoi(argv[1]);
  const uint64_t seconds = argc > 2 ? uint64_t(atoi(argv[2])) : 60;
  signal(SIGTERM, [](int) { g_stop = 1; });
  signal(SIGINT, [](int) { g_stop = 1; });
  signal(SIGPIPE, SIG_IGN);
  x11::Server server(g_allocator, &now_ms);
  server.attach(6, 19);
  const desktop::IWindowsSession::Monitor monitor[1] = {{{0, 0, 1280, 800}, {0, 0, 1280, 768}}};
  server.set_display(1280, 800, ArrayView<const desktop::IWindowsSession::Monitor>(monitor, 1));

  const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const int n = snprintf(address.sun_path + 1, sizeof address.sun_path - 1, "/tmp/.X11-unix/X%d", display);
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), socklen_t(offsetof(sockaddr_un, sun_path) + 1 + n)) != 0 || ::listen(listener, 16) != 0) {
    perror("bind");
    return 1;
  }
  printf("listening on @/tmp/.X11-unix/X%d\n", display);
  fflush(stdout);
  std::vector<Connection> connections;
  const bool xdnd = getenv("TCOS_XDND") != nullptr;
  const bool xi2 = getenv("TCOS_XI2") != nullptr;
  uint32_t touch_step = 0;
  bool desktop_dropped = false;
  uint32_t drag_steps = 0;
  const uint64_t end = now_ms() + seconds * 1000;
  std::vector<uint8_t> buffer(256 * 1024);
  while (!g_stop && now_ms() < end) {
    std::vector<pollfd> fds;
    fds.push_back({listener, POLLIN, 0});
    for (const Connection& c : connections) fds.push_back({c.fd, short(POLLIN | (c.pending.empty() ? 0 : POLLOUT)), 0});
    poll(fds.data(), fds.size(), 50);
    if (fds[0].revents & POLLIN) {
      const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (fd >= 0) {
        const uint32_t client = server.open_client(0);
        if (client) connections.push_back({fd, client, {}});
        else close(fd);
      }
    }
    for (Connection& c : connections) {
      for (;;) {
        const ssize_t got = read(c.fd, buffer.data(), buffer.size());
        if (got > 0) server.receive(c.client, ConstByteView(buffer.data(), uint64_t(got)));
        else {
          if (got == 0) c.gone = true;
          break;
        }
      }
    }
    for (Connection& c : connections) {      // (also the clients that did nothing: another's request may have sent them events)
      if (c.gone) continue;
      ByteBuffer out(g_allocator);
      const bool closing = server.take_output(c.client, &out);
      c.pending.insert(c.pending.end(), out.view().data(), out.view().data() + out.size());
      while (!c.pending.empty()) {
        const ssize_t sent = write(c.fd, c.pending.data(), c.pending.size());
        if (sent <= 0) break;
        c.pending.erase(c.pending.begin(), c.pending.begin() + sent);
      }
      if (closing && c.pending.empty()) c.gone = true;
    }
    for (size_t i = 0; i < connections.size();) {
      if (!connections[i].gone) { ++i; continue; }
      close(connections[i].fd);
      server.close_client(connections[i].client);
      connections.erase(connections.begin() + long(i));
    }
    if (xi2 && touch_step < 40) {      // TCOS_XI2: the desktop's touchscreen for tests/x11/xi2_client.c (a finger on "xi2-target" at (30, 40) of it)
      List<uint32_t> windows(g_allocator);
      server.windows(&windows);
      for (uint32_t id : windows.view()) {
        desktop::WindowsWindowInfo info;
        String title(g_allocator);
        if (!server.window_info(id, &info, &title) || title.view() != StringView("xi2-target") || !info.visible) continue;
        const int32_t x = info.surface.x + 30, y = info.surface.y + 40;
        if (touch_step == 0) {
          if (!server.touch(hardware::TouchPhase::Down, 1, id, x, y)) continue;      // (not selected yet: next time)
          printf("touch down taken\n");
        } else if (touch_step < 4) {
          server.touch(hardware::TouchPhase::Motion, 1, 0, x + int32_t(touch_step), y);
        } else if (touch_step == 4) {
          server.touch(hardware::TouchPhase::Up, 1, 0, x + 3, y);
          touch_step = 40;
          break;
        }
        ++touch_step;
      }
      fflush(stdout);
    }
    if (xdnd) {      // TCOS_XDND: the desktop's part of tests/x11/xdnd_client.c (its file dragged onto "xdnd-target"; the client's file taken)
      List<uint32_t> windows(g_allocator);
      server.windows(&windows);
      for (uint32_t id : windows.view()) {
        desktop::WindowsWindowInfo info;
        String title(g_allocator);
        if (desktop_dropped || !server.window_info(id, &info, &title) || title.view() != StringView("xdnd-target")) continue;
        if (server.desktop_drag(id, info.surface.x + 10 + int32_t(drag_steps % 5), info.surface.y + 10, "/Desktop/Documents/a b.txt") && drag_steps > 3) {
          desktop_dropped = server.desktop_drop();
          printf("desktop drop: %s\n", desktop_dropped ? "taken" : "refused");
        }
        ++drag_steps;
      }
      String items(g_allocator);
      int32_t x = 0, y = 0;
      if (server.program_drag(&items, &x, &y)) {
        printf("program drag over the desktop at %d,%d: %s\n", x, y, items.c_str());
        server.program_drag_answer(true);
      }
      if (server.take_program_drop(&items, &x, &y)) {
        printf("program drop at %d,%d: %s\n", x, y, items.c_str());
        server.program_drop_done(true);
      }
      fflush(stdout);
    }
  }
  if (argc > 3) dump(server, argv[3]);
  for (Connection& c : connections) {
    close(c.fd);
    server.close_client(c.client);
  }
  close(listener);
  return 0;
}
