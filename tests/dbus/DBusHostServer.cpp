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
// Services are started on demand from the .service files of --services <folder> (copied into a file system in memory at the start): HostLauncher
// forks and execs their programs with DBUS_STARTER_ADDRESS, DBUS_STARTER_BUS_TYPE and the bus's own address variable set, as dbus-daemon does.
//
// Usage: DBusHostServer <socket path> <system|session> [seconds] [--services <folder>] [--reply-timeout-ms <n>] [--activation-timeout-ms <n>]
//   (runs until SIGTERM/SIGINT, or for that many seconds)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <dirent.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <chrono>
#include <vector>
#include "DBus/Bus.hpp"
#include "DBus/Notifications.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"

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

// Starts a service's program as dbus-daemon does: a child process with the starter variables, polled with waitpid().
class HostLauncher final : public dbus::IServiceLauncher {
 public:
  HostLauncher(std::string address, bool system) : address_(std::move(address)), system_(system) {}
  uint64_t start(dbus::BusKind, StringView name, ArrayView<const StringView> argv, String* why) override {
    std::vector<std::string> args;
    for (const StringView& a : argv) args.push_back(std::string(a.data(), a.size()));
    if (access(args[0].c_str(), X_OK) != 0) {      // (dbus-daemon learns that exec failed through a pipe; looking first comes to the same)
      why->assign(strerror(errno));
      return 0;
    }
    printf("activate: %.*s -> %s\n", int(name.size()), name.data(), args[0].c_str());
    fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
      setenv("DBUS_STARTER_ADDRESS", address_.c_str(), 1);
      setenv("DBUS_STARTER_BUS_TYPE", system_ ? "system" : "session", 1);
      setenv(system_ ? "DBUS_SYSTEM_BUS_ADDRESS" : "DBUS_SESSION_BUS_ADDRESS", address_.c_str(), 1);
      std::vector<char*> v;
      for (std::string& a : args) v.push_back(a.data());
      v.push_back(nullptr);
      execv(v[0], v.data());
      _exit(127);
    }
    children_.push_back({pid, State::Running, 0});
    return children_.size();
  }
  State poll(uint64_t handle, uint32_t* code, String*) override {
    Child& c = children_[handle - 1];
    if (c.state == State::Running) {
      int status = 0;
      if (waitpid(c.pid, &status, WNOHANG) == c.pid) {
        c.state = State::Exited;
        c.code = WIFEXITED(status) ? uint32_t(WEXITSTATUS(status)) : 128u + uint32_t(WTERMSIG(status));
      }
    }
    *code = c.code;
    return c.state;
  }
  void forget(uint64_t) override {}      // (a service that runs on is left to run; reap() collects it)
  void reap() {
    for (Child& c : children_) {
      int status = 0;
      if (c.state == State::Running && waitpid(c.pid, &status, WNOHANG) == c.pid) c.state = State::Exited;
    }
  }
  void stop_all() {
    for (Child& c : children_)
      if (c.state == State::Running) kill(c.pid, SIGTERM);
  }

 private:
  struct Child {
    pid_t pid;
    State state;
    uint32_t code;
  };
  std::string address_;
  bool system_;
  std::vector<Child> children_;
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
  double seconds = 0;
  const char* services = nullptr;
  long reply_timeout_ms = -1, activation_timeout_ms = -1;
  for (int i = 3; i < argc; ++i) {
    if (strcmp(argv[i], "--services") == 0 && i + 1 < argc) services = argv[++i];
    else if (strcmp(argv[i], "--reply-timeout-ms") == 0 && i + 1 < argc) reply_timeout_ms = atol(argv[++i]);
    else if (strcmp(argv[i], "--activation-timeout-ms") == 0 && i + 1 < argc) activation_timeout_ms = atol(argv[++i]);
    else seconds = atof(argv[i]);
  }
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

  // the service files, copied into a file system in memory (what the bus reads them from)
  filesystem::RamFileSystem files(A);
  HostLauncher launcher(std::string("unix:path=") + argv[1], system);
  if (services) {
    files.create_directory("/services");
    if (DIR* d = opendir(services)) {
      while (dirent* e = readdir(d)) {
        const std::string leaf = e->d_name;
        if (leaf.size() < 9 || leaf.substr(leaf.size() - 8) != ".service") continue;
        FILE* f = fopen((std::string(services) + "/" + leaf).c_str(), "rb");
        if (!f) continue;
        std::vector<uint8_t> text(65536);
        const size_t n = fread(text.data(), 1, text.size(), f);
        fclose(f);
        UniquePointer<filesystem::IFile> file;
        uint64_t written = 0;
        if (files.open_file(StringView(("/services/" + leaf).c_str()), filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok)
          file->write(0, ConstByteView(text.data(), n), &written);
      }
      closedir(d);
    }
    const StringView folders[] = {"/services"};
    bus.set_activation(&launcher, &files, ArrayView<const StringView>(folders, 1));
  }
  if (reply_timeout_ms >= 0) bus.set_reply_timeout_us(uint64_t(reply_timeout_ms) * 1000);
  if (activation_timeout_ms > 0) bus.set_activation_timeout_us(uint64_t(activation_timeout_ms) * 1000);

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
    launcher.reap();
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
  launcher.stop_all();
  unlink(argv[1]);
  return 0;
}
