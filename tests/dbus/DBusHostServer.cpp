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

// The kernel's D-Bus (src/DBus) served on the development machine's own AF_UNIX socket, so that the real client libraries (libdbus: dbus-send,
// dbus-monitor; sd-bus: busctl; GDBus: gdbus) can be pointed at it: tests/dbus_interop.sh does that. Descriptors passed with SCM_RIGHTS are real host
// descriptors (HostFd), handed on to the recipient as they are. The kernel's Notifications service is on the session bus, with a display that prints what
// it is asked to show ("notify: <app> | <summary> | <body>") on stdout.
//
// Usage: DBusHostServer <socket path> <system|session> [seconds]   (runs until SIGTERM/SIGINT, or for that many seconds)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <chrono>
#include <vector>
#include "DBus/Bus.hpp"
#include "DBus/Notifications.hpp"

namespace {

struct HostAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t align) override {
    void* p = nullptr;
    return posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, size ? size : 1) == 0 ? p : nullptr;
  }
  void deallocate(void* p) override { free(p); }
};
HostAllocator A;

// A host descriptor the bus holds: closed when the last message that carries it has gone.
class HostFd final : public dbus::Fd {
 public:
  explicit HostFd(int fd) : fd_(fd) {}
  ~HostFd() { ::close(fd_); }
  int fd() const { return fd_; }

 private:
  int fd_;
};
// (every descriptor the bus hands this server came from it: each is a HostFd)
int host_fd(const dbus::FdRef& fd) { return static_cast<const HostFd&>(*fd).fd(); }

class PrintingDisplay final : public dbus::INotificationDisplay {
 public:
  uint32_t show(StringView app, StringView summary, StringView body, uint32_t timeout_ms) override {
    printf("notify: %.*s | %.*s | %.*s | %u\n", int(app.size()), app.data(), int(summary.size()), summary.data(), int(body.size()), body.data(), timeout_ms);
    fflush(stdout);
    return ++shown_;
  }
  void close(uint32_t toast) override {
    printf("notify-close: %u\n", toast);
    fflush(stdout);
  }

 private:
  uint32_t shown_ = 0;
};

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Client {
  int fd;
  uint32_t id;
  std::vector<uint8_t> out;
  struct Batch {
    size_t at;
    std::vector<dbus::FdRef> fds;
  };
  std::vector<Batch> batches;      // descriptors that go with byte `at` of `out`
  bool closing = false;
};

uint64_t now_us() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <socket path> <system|session> [seconds]\n", argv[0]);
    return 2;
  }
  const bool system = strcmp(argv[2], "system") == 0;
  const double seconds = argc > 3 ? atof(argv[3]) : 0;
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  signal(SIGPIPE, SIG_IGN);

  dbus::Bus bus(A, system ? dbus::BusKind::System : dbus::BusKind::Session, "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210");
  PrintingDisplay display;
  dbus::NotificationService notifications(bus, display, A);
  if (!system && !notifications.start()) {
    fprintf(stderr, "notifications did not start\n");
    return 1;
  }

  int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", argv[1]);
  unlink(argv[1]);
  if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(listener, 64) != 0) {
    perror("bind/listen");
    return 1;
  }
  std::vector<Client> clients;
  std::vector<uint8_t> chunk(64 * 1024);
  const uint64_t start = now_us();
  while (!g_stop && (seconds <= 0 || double(now_us() - start) < seconds * 1e6)) {
    std::vector<pollfd> fds;
    fds.push_back({listener, POLLIN, 0});
    for (Client& c : clients) fds.push_back({c.fd, short(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
    poll(fds.data(), fds.size(), 20);
    for (;;) {
      int fd = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd < 0) break;
      ucred cred{};
      socklen_t len = sizeof cred;
      getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len);
      const uint32_t id = bus.open_client(uint64_t(cred.pid), cred.uid, true);
      if (!id) { ::close(fd); continue; }
      clients.push_back(Client{fd, id, {}, {}, false});
    }
    for (Client& c : clients) {
      if (c.closing) continue;
      for (;;) {
        msghdr msg{};
        iovec iov{chunk.data(), chunk.size()};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 253)];
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        const ssize_t got = recvmsg(c.fd, &msg, MSG_CMSG_CLOEXEC);
        if (got < 0 && (errno == EAGAIN || errno == EINTR)) break;
        Queue<dbus::FdRef> passed(A);
        for (cmsghdr* h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h))
          if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) {
            const size_t n = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < n; ++i) {
              int received = -1;
              memcpy(&received, CMSG_DATA(h) + i * sizeof(int), sizeof(int));
              passed.enqueue(dbus::FdRef::make_as<HostFd>(A, received));
            }
          }
        if (got <= 0) { c.closing = true; break; }
        if (getenv("DBUS_HOST_DUMP")) {      // (what came, in hex, for looking at a message the bus refused)
          fprintf(stderr, "in %u:", c.id);
          for (ssize_t i = 0; i < got; ++i) fprintf(stderr, " %02x", chunk[size_t(i)]);
          fprintf(stderr, "\n");
        }
        bus.receive(c.id, ConstByteView(chunk.data(), uint64_t(got)), &passed);
      }
    }
    bus.tick(now_us());
    for (Client& c : clients) {
      ByteBuffer out(A);
      List<dbus::Bus::OutFd> outfds(A);
      if (bus.take_output(c.id, &out, &outfds)) c.closing = true;
      const size_t base = c.out.size();
      c.out.insert(c.out.end(), out.view().data(), out.view().data() + out.size());
      for (const dbus::Bus::OutFd& f : outfds.view()) {
        const size_t at = base + f.at;
        if (c.batches.empty() || c.batches.back().at != at) c.batches.push_back(Client::Batch{at, {}});
        c.batches.back().fds.push_back(f.fd);
      }
      // send: up to the next bytes that carry descriptors, then those bytes with them
      while (!c.out.empty()) {
        size_t end = c.out.size();
        const Client::Batch* carrying = nullptr;
        if (!c.batches.empty()) {
          if (c.batches[0].at == 0) {
            carrying = &c.batches[0];
            if (c.batches.size() > 1) end = c.batches[1].at;
          } else {
            end = c.batches[0].at;
          }
        }
        msghdr msg{};
        iovec iov{c.out.data(), end};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 253)];
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (carrying) {
          msg.msg_control = control;
          msg.msg_controllen = CMSG_SPACE(sizeof(int) * carrying->fds.size());
          cmsghdr* h = CMSG_FIRSTHDR(&msg);
          h->cmsg_level = SOL_SOCKET;
          h->cmsg_type = SCM_RIGHTS;
          h->cmsg_len = CMSG_LEN(sizeof(int) * carrying->fds.size());
          for (size_t i = 0; i < carrying->fds.size(); ++i) {
            const int fd = host_fd(carrying->fds[i]);
            memcpy(CMSG_DATA(h) + i * sizeof(int), &fd, sizeof(int));
          }
        }
        const ssize_t sent = sendmsg(c.fd, &msg, MSG_NOSIGNAL);
        if (sent < 0) {
          if (errno != EAGAIN && errno != EINTR) { c.closing = true; c.out.clear(); }
          break;
        }
        if (carrying && sent > 0) c.batches.erase(c.batches.begin());
        c.out.erase(c.out.begin(), c.out.begin() + sent);
        for (Client::Batch& b : c.batches) b.at -= size_t(sent);
        if (size_t(sent) < end) break;
      }
    }
    for (size_t i = 0; i < clients.size();) {
      if (clients[i].closing && clients[i].out.empty()) {
        bus.close_client(clients[i].id);
        ::close(clients[i].fd);
        clients.erase(clients.begin() + long(i));
      } else {
        ++i;
      }
    }
  }
  for (Client& c : clients) {
    bus.close_client(c.id);
    ::close(c.fd);
  }
  unlink(argv[1]);
  return 0;
}
