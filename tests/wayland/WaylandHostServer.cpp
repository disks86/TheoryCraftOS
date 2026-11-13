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

// The kernel's Wayland compositor (src/Wayland) on the development machine, for real Wayland clients (libwayland-client programs, foot, GTK): it listens
// on <runtime dir>/<name> (what a client finds through XDG_RUNTIME_DIR and WAYLAND_DISPLAY), passes descriptors with SCM_RIGHTS as the kernel's transport
// does (a pool is the client's memfd, mapped here and read in place; the keymap is a sealed memfd made here), and stands in for the desktop at 1280x800 with
// frames of 4 + 24 pixels: it draws every window about 60 times a second (which answers the frame callbacks), activates each new toplevel, and gives the
// toplevel titled "wl_client" a scripted click and the keys h, i. Used by tests/wayland_interop.sh.
//
//   WaylandHostServer <runtime dir> <name> [<seconds>] [<dump.ppm>]   serves until the time is up or SIGTERM; the windows' pictures go to the PPM at the end

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <set>
#include <string>
#include <vector>
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Kernel/Clipboard.hpp"
#include "Wayland/Media/WaylandClipboardPng.hpp"
#include "Wayland/WaylandServer.hpp"

namespace {

struct Allocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    void* p = nullptr;
    return posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size ? size : 1) == 0 ? p : nullptr;
  }
  void deallocate(void* p) override { free(p); }
};
Allocator g_allocator;
volatile sig_atomic_t g_stop = 0;

uint64_t now_ms() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1000 + uint64_t(t.tv_nsec) / 1000000;
}

// A descriptor of this process: mapped (read-only) when the server asks; the mapping of the previous call goes with the next one.
class HostFd final : public wayland::Memory {
 public:
  explicit HostFd(int fd) : fd_(fd) {}
  ~HostFd() {
    if (map_) munmap(map_, length_);
    if (fd_ >= 0) close(fd_);
  }
  ByteView map(uint64_t size) override {
    struct stat st;
    if (fd_ < 0 || fstat(fd_, &st) != 0 || uint64_t(st.st_size) < size || size == 0) return ByteView();
    void* m = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) return ByteView();
    if (map_) munmap(map_, length_);
    map_ = m;
    length_ = size;
    return ByteView(static_cast<uint8_t*>(m), size);
  }
  int fd() const { return fd_; }

 private:
  int fd_;
  void* map_ = nullptr;
  uint64_t length_ = 0;
};

wayland::Fd make_fd(void*, ConstByteView contents) {
  const int fd = memfd_create("wayland-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) return wayland::Fd();
  if (write(fd, contents.data(), contents.size()) != ssize_t(contents.size())) {
    close(fd);
    return wayland::Fd();
  }
  fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
  return wayland::Fd::make_as<HostFd>(g_allocator, fd);
}

struct Connection {
  int fd;
  uint32_t client;
  std::vector<uint8_t> pending;
  std::vector<wayland::Fd> pending_fds;
  bool gone = false;
};

void dump(wayland::Server& server, const char* path, bool quiet) {
  List<uint32_t> windows(g_allocator);
  server.windows(&windows);
  FILE* f = fopen(path, "wb");
  if (!f) return;
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
    if (!quiet) printf("window %u \"%s\" frame %d,%d %ux%u inside %ux%u%s%s%s\n", id, title.c_str(), info.window.x, info.window.y, info.window.width, info.window.height,
           info.surface.width, info.surface.height, info.caption ? " decorated" : "", info.popup ? " popup" : "", info.maximized ? " maximized" : "");
  }
  if (total_w == 0) total_w = 1;
  // each window as the desktop draws it (its subsurfaces too)
  std::vector<std::vector<uint32_t>> pictures;
  for (const Shot& s : shots) {
    pictures.emplace_back(size_t(s.w) * s.h, 0x808080u);
    graphics::Surface target;
    target.pixels = ArrayView<uint32_t>(pictures.back().data(), pictures.back().size());
    target.width = target.pitch = s.w;
    target.height = s.h;
    graphics::SoftwareRenderer2D renderer(target);
    uint32_t w = 0, h = 0;
    server.draw_window(s.id, renderer, 0, 0, &w, &h);
  }
  fprintf(f, "P6\n%u %u\n255\n", total_w, max_h);
  for (uint32_t y = 0; y < max_h; ++y) {
    for (size_t k = 0; k < shots.size(); ++k) {
      const Shot& s = shots[k];
      for (uint32_t x = 0; x < s.w + 4; ++x) {
        const uint32_t v = x < s.w && y < s.h ? pictures[k][size_t(y) * s.w + x] : 0x808080u;
        const uint8_t rgb[3] = {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        fwrite(rgb, 1, 3, f);
      }
    }
  }
  fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <runtime dir> <name> [<seconds>] [<dump.ppm>]\n", argv[0]);
    return 2;
  }
  const std::string path = std::string(argv[1]) + "/" + argv[2];
  const uint64_t seconds = argc > 3 ? uint64_t(atoi(argv[3])) : 60;
  signal(SIGTERM, [](int) { g_stop = 1; });
  signal(SIGINT, [](int) { g_stop = 1; });
  signal(SIGPIPE, SIG_IGN);
  wayland::Server server(g_allocator, &now_ms);
  server.set_fd_maker(&make_fd, nullptr);
  // the desktop's clipboard: TCOS_CLIPBOARD_TEXT seeds it; what it holds at the end is printed ("clipboard: ...")
  kernel::Clipboard clipboard(g_allocator);
  if (const char* seed = getenv("TCOS_CLIPBOARD_TEXT")) clipboard.set_text(seed);
  if (getenv("TCOS_CLIPBOARD_IMAGE")) {      // a 3 x 2 picture: red, green, blue / half-transparent white, black, transparent
    const uint32_t pixels[6] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0x80FFFFFFu, 0xFF000000u, 0x00000000u};
    clipboard.set_image(pixels, 3, 2);
  }
  const uint64_t clipboard_start = clipboard.sequence();
  server.set_clipboard_images(&wayland::clipboard_to_png, &wayland::png_to_clipboard);      // (pictures as image/png)
  server.set_clipboard(&clipboard);
  struct Reading { uint32_t job; int fd; std::vector<uint8_t> data; };
  std::vector<Reading> readings;
  server.attach(4, 24);
  const desktop::IWindowsSession::Monitor monitor[1] = {{{0, 0, 1280, 800}, {0, 0, 1280, 768}}};
  server.set_display(1280, 800, ArrayView<const desktop::IWindowsSession::Monitor>(monitor, 1));

  const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  snprintf(address.sun_path, sizeof address.sun_path, "%s", path.c_str());
  unlink(path.c_str());
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(listener, 16) != 0) {
    perror("bind");
    return 1;
  }
  printf("listening on %s\n", path.c_str());
  fflush(stdout);
  std::vector<Connection> connections;
  const uint64_t end = now_ms() + seconds * 1000;
  std::vector<uint8_t> buffer(256 * 1024);
  std::vector<uint32_t> canvas(2048 * 2048);
  graphics::Surface target;
  target.pixels = ArrayView<uint32_t>(canvas.data(), canvas.size());
  target.width = target.height = target.pitch = 2048;
  std::set<uint32_t> seen;
  uint64_t next_draw = 0, next_dump = 0;
  struct Script { uint32_t window = 0; int step = 0; uint64_t at = 0; } script;
  while (!g_stop && now_ms() < end) {
    std::vector<pollfd> fds;
    fds.push_back({listener, POLLIN, 0});
    for (const Connection& c : connections) fds.push_back({c.fd, short(POLLIN | (c.pending.empty() ? 0 : POLLOUT)), 0});
    poll(fds.data(), fds.size(), 5);
    if (fds[0].revents & POLLIN) {
      const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (fd >= 0) {
        const uint32_t client = server.open_client(0);
        if (client) connections.push_back({fd, client, {}, {}});
        else close(fd);
      }
    }
    for (Connection& c : connections) {
      for (;;) {
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 28)];
        iovec iov{buffer.data(), buffer.size()};
        msghdr m{};
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = control;
        m.msg_controllen = sizeof control;
        const ssize_t got = recvmsg(c.fd, &m, MSG_CMSG_CLOEXEC);
        std::vector<wayland::Fd> received;
        for (cmsghdr* h = CMSG_FIRSTHDR(&m); got >= 0 && h; h = CMSG_NXTHDR(&m, h)) {
          if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
          const size_t n = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
          for (size_t i = 0; i < n; ++i) {
            int fd;
            memcpy(&fd, CMSG_DATA(h) + i * sizeof(int), sizeof fd);
            received.push_back(wayland::Fd::make_as<HostFd>(g_allocator, fd));
          }
        }
        if (got > 0 || !received.empty())
          server.receive(c.client, ConstByteView(buffer.data(), uint64_t(got > 0 ? got : 0)), ArrayView<wayland::Fd>(received.data(), received.size()));
        if (got <= 0) {
          if (got == 0) c.gone = true;
          break;
        }
      }
    }
    // what the server asks of its transport: pipes read (a client's selection for the clipboard), the clipboard written into a client's pipe
    server.tick();
    {
      List<wayland::TransportRequest> requests(g_allocator);
      server.take_requests(&requests);
      for (wayland::TransportRequest& r : requests.view()) {
        if (r.kind == wayland::TransportRequest::Kind::MakePipe) {
          int p[2];
          if (pipe2(p, O_CLOEXEC | O_NONBLOCK) != 0) continue;
          readings.push_back({r.job, p[0], {}});
          server.pipe_ready(r.job, wayland::Fd::make_as<HostFd>(g_allocator, p[1]));
        } else if (r.fd && r.data) {
          const int fd = static_cast<const HostFd&>(*r.fd).fd();
          const int flags = fcntl(fd, F_GETFL);
          fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
          if (write(fd, r.data->view().data(), r.data->size()) != ssize_t(r.data->size())) printf("clipboard write failed\n");
        }
      }
    }
    for (size_t i = 0; i < readings.size();) {
      uint8_t chunk[4096];
      const ssize_t n = read(readings[i].fd, chunk, sizeof chunk);
      if (n > 0) {
        readings[i].data.insert(readings[i].data.end(), chunk, chunk + n);
        continue;
      }
      if (n < 0 && errno == EAGAIN) { ++i; continue; }
      server.pipe_read_done(readings[i].job, ConstByteView(readings[i].data.data(), readings[i].data.size()));
      close(readings[i].fd);
      readings.erase(readings.begin() + long(i));
    }
    // the desktop: new windows activated, everything drawn now and then (the frame callbacks), the script
    if (now_ms() >= next_draw) {
      next_draw = now_ms() + 16;
      List<uint32_t> changed(g_allocator), windows(g_allocator);
      List<desktop::IWindowsSession::Repaint> repainted(g_allocator);
      server.take_changes(&changed, &repainted);
      server.windows(&windows);
      for (uint32_t id : windows.view()) {
        desktop::WindowsWindowInfo info;
        String title(g_allocator);
        if (!server.window_info(id, &info, &title)) continue;
        if (!seen.count(id)) {
          seen.insert(id);
          printf("mapped %u \"%s\" %ux%u%s%s\n", id, title.c_str(), info.surface.width, info.surface.height, info.caption ? " decorated" : "", info.popup ? " popup" : "");
          fflush(stdout);
          if (!info.popup) server.activate_window(id);
          if (title.view() == StringView("wl_client") && script.window == 0) { script.window = id; script.at = now_ms() + 100; }
        }
        graphics::SoftwareRenderer2D renderer(target);
        uint32_t w = 0, h = 0;
        server.draw_window(id, renderer, 0, 0, &w, &h);
      }
      // the pictures, while there are windows (the last ones are what the PPM shows)
      if (argc > 4 && windows.count() > 0 && now_ms() >= next_dump) {
        next_dump = now_ms() + 250;
        dump(server, argv[4], true);
      }
      if (script.window && now_ms() >= script.at && script.step < 8) {
        desktop::WindowsWindowInfo info;
        String title(g_allocator);
        if (server.window_info(script.window, &info, &title)) {
          const int32_t cx = info.surface.x + int32_t(info.surface.width / 2), cy = info.surface.y + int32_t(info.surface.height / 2);
          switch (script.step) {
            case 0: server.mouse_position(cx, cy); break;
            case 1: server.mouse_button(1, true); break;
            case 2: server.mouse_button(1, false); break;
            case 3: server.key(0x0B, true, 0); break;      // h
            case 4: server.key(0x0B, false, 0); break;
            case 5: server.key(0x0C, true, 0); break;      // i
            case 6: server.key(0x0C, false, 0); break;
            case 7: server.mouse_wheel(-1); break;
          }
        }
        ++script.step;
        script.at = now_ms() + 30;
      }
    }
    for (Connection& c : connections) {      // (also the clients that did nothing: input or another client may have sent them events)
      if (c.gone) continue;
      ByteBuffer out(g_allocator);
      List<wayland::Fd> fds_out(g_allocator);
      const bool closing = server.take_output(c.client, &out, &fds_out);
      c.pending.insert(c.pending.end(), out.view().data(), out.view().data() + out.size());
      for (wayland::Fd& f : fds_out.view()) c.pending_fds.push_back(f);
      while (!c.pending.empty()) {
        iovec iov{c.pending.data(), c.pending.size()};
        msghdr m{};
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 28)];
        const size_t nfds = c.pending_fds.size() < 28 ? c.pending_fds.size() : 28;
        if (nfds) {
          m.msg_control = control;
          m.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
          cmsghdr* h = CMSG_FIRSTHDR(&m);
          h->cmsg_level = SOL_SOCKET;
          h->cmsg_type = SCM_RIGHTS;
          h->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
          for (size_t i = 0; i < nfds; ++i) {
            const int fd = static_cast<const HostFd&>(*c.pending_fds[i]).fd();      // (every descriptor here is a HostFd)
            memcpy(CMSG_DATA(h) + i * sizeof(int), &fd, sizeof fd);
          }
        }
        const ssize_t sent = sendmsg(c.fd, &m, MSG_NOSIGNAL);
        if (sent <= 0) break;
        c.pending_fds.erase(c.pending_fds.begin(), c.pending_fds.begin() + long(nfds));
        c.pending.erase(c.pending.begin(), c.pending.begin() + sent);
      }
      if (closing && c.pending.empty()) c.gone = true;
    }
    for (size_t i = 0; i < connections.size();) {
      if (!connections[i].gone) { ++i; continue; }
      close(connections[i].fd);
      server.close_client(connections[i].client);
      printf("client %u gone\n", connections[i].client);
      fflush(stdout);
      connections.erase(connections.begin() + long(i));
    }
  }

  for (Connection& c : connections) {
    close(c.fd);
    server.close_client(c.client);
  }
  String text(g_allocator);
  if (clipboard.sequence() != clipboard_start && clipboard.get_text(&text)) printf("clipboard: %s\n", text.c_str());
  uint32_t iw = 0, ih = 0;
  if (clipboard.sequence() != clipboard_start && clipboard.image_size(&iw, &ih)) {
    std::vector<uint32_t> px(size_t(iw) * ih);
    if (clipboard.get_image(px.data(), px.size())) printf("clipboard image: %ux%u first %08x last %08x\n", iw, ih, px.front(), px.back());
  }
  server.set_clipboard(nullptr);
  close(listener);
  unlink(path.c_str());
  return 0;
}
