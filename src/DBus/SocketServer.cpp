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

#include "DBus/SocketServer.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Kernel/Delay.hpp"
#include "Process/Process.hpp"

namespace dbus {

using process::UnixInFlight;
using process::UnixName;
using process::UnixStatus;

namespace {

// A descriptor a connection passed (an SCM_RIGHTS entry, with the use it holds). The bus keeps it while a message carries it; the last copy lets go of the
// use. The bus is only called from the transport's thread with the system's lock let go, so that is where this goes too.
class KernelFd final : public Fd {
 public:
  KernelFd(process::UnixSocketManager& sockets, UnixInFlight entry) : sockets_(sockets), entry_(util::move(entry)) {}
  ~KernelFd() {
    if (entry_.token == 0) return;
    if (entry_.kind == process::DescriptorKind::UnixSocket) sockets_.release(uint32_t(entry_.token));
    else process::descriptor_token_release(entry_.kind, entry_.token);
  }
  KernelFd(const KernelFd&) = delete;
  KernelFd& operator=(const KernelFd&) = delete;
  const UnixInFlight& entry() const { return entry_; }

 private:
  process::UnixSocketManager& sockets_;
  UnixInFlight entry_;
};

// Every descriptor a bus hands this transport came from it (receive() is given only KernelFds), so each is one.
const KernelFd& kernel_fd(const FdRef& fd) { return static_cast<const KernelFd&>(*fd); }

}  // namespace

SocketServer::SocketServer(process::UnixSocketManager& sockets, allocators::IAllocator& allocator)
    : sockets_(sockets), allocator_(allocator), connections_(allocator), chunk_(allocator) {
  chunk_.put_zeros(kReadChunk);
}

SocketServer::~SocketServer() {
  stop();
  for (UniquePointer<Connection>& c : connections_.view()) {
    c->bus->close_client(c->client);
    release_entries(c->received);
    drop_rights(*c);
    sockets_.release(c->socket);
  }
  connections_.clear();
  for (uint32_t i = 0; i < listener_count_; ++i) sockets_.release(listeners_[i].socket);
  listener_count_ = 0;
}

void SocketServer::release_entries(Queue<UnixInFlight>& entries) {
  UnixInFlight e;
  while (entries.dequeue(&e)) {
    if (e.token != 0) {
      if (e.kind == process::DescriptorKind::UnixSocket) sockets_.release(uint32_t(e.token));
      else process::descriptor_token_release(e.kind, e.token);
    }
    e.object = SharedPointer<process::OpenObject>();
  }
}

void SocketServer::drop_rights(Connection& c) {
  for (UniquePointer<Connection::Batch>& b : c.batches.view()) release_entries(b->rights);
  c.batches.clear();
}

// The descriptors of a take of output: copies, each with a use of its own, to go with byte base + at of `pending` (descriptors of one message together).
void SocketServer::queue_fds(Connection& c, uint64_t base, const List<Bus::OutFd>& fds) {
  for (const Bus::OutFd& f : fds.view()) {
    const uint64_t at = base + f.at;
    Connection::Batch* batch = c.batches.count() ? c.batches[c.batches.count() - 1].get() : nullptr;
    if (!batch || batch->at != at) {
      UniquePointer<Connection::Batch> made = UniquePointer<Connection::Batch>::make(allocator_, allocator_);
      if (!made) { c.dead = true; return; }
      made->at = at;
      batch = made.get();
      if (!c.batches.push_back(util::move(made))) { c.dead = true; return; }
    }
    UnixInFlight entry = kernel_fd(f.fd).entry();
    if (entry.kind == process::DescriptorKind::Closed) continue;
    if (entry.token != 0) {
      if (entry.kind == process::DescriptorKind::UnixSocket) sockets_.add_ref(uint32_t(entry.token));
      else process::descriptor_token_add(entry.kind, entry.token);
    }
    const process::DescriptorKind kind = entry.kind;
    const uint64_t token = entry.token;
    if (!batch->rights.enqueue(util::move(entry))) {
      if (token != 0) {
        if (kind == process::DescriptorKind::UnixSocket) sockets_.release(uint32_t(token));
        else process::descriptor_token_release(kind, token);
      }
      c.dead = true;
    }
    if (batch->rights.count() > process::UnixSocketManager::kMaxRights) c.dead = true;
  }
}

void SocketServer::send_owed_locked(Connection& c, bool* moved) {
  while (!c.dead && c.sent < c.pending.size()) {
    uint64_t end = c.pending.size();
    Connection::Batch* carrying = nullptr;
    if (c.batches.count()) {
      if (c.batches[0]->at <= c.sent) {
        carrying = c.batches[0].get();
        if (c.batches.count() > 1) end = c.batches[1]->at;
      } else {
        end = c.batches[0]->at;
      }
    }
    if (end <= c.sent) end = c.pending.size();
    uint32_t sent = 0;
    const UnixStatus st = sockets_.send_locked(c.socket, c.pending.view().subview(c.sent, end - c.sent), nullptr, 0, carrying ? &carrying->rights : nullptr, &sent);
    if (st == UnixStatus::WouldBlock) break;
    if (st != UnixStatus::Ok) { c.dead = true; break; }
    if (carrying && sent > 0) c.batches.remove_at(0);
    c.sent += sent;
    *moved = true;
    if (sent == 0) break;
  }
}

bool SocketServer::bind_one(UnixName::Kind kind, StringView name, StringView host_path, Bus& bus) {
  if (listener_count_ == kMaxListeners) return false;
  uint32_t id = 0;
  if (sockets_.create(process::UnixType::Stream, 0, &id) != UnixStatus::Ok) return false;
  const UnixName un = UnixName::from(kind, name.bytes());
  UnixStatus st;
  {
    process::ISystemSync& sync = sockets_.sync();
    sync.lock();
    st = sockets_.bind_locked(id, un, host_path);
    if (st == UnixStatus::Ok) st = sockets_.listen_locked(id, process::UnixSocketManager::kMaxBacklog);
    sync.unlock();
  }
  if (st != UnixStatus::Ok) {
    sockets_.release(id);
    return false;
  }
  listeners_[listener_count_++] = Listener{id, &bus};
  return true;
}

bool SocketServer::listen(Bus& bus, StringView linux_path, StringView host_path, filesystem::IFileSystem* files) {
  bool known = false;
  for (uint32_t i = 0; i < bus_count_; ++i) known |= buses_[i] == &bus;
  if (!known) {
    if (bus_count_ == kMaxListeners) return false;
    buses_[bus_count_++] = &bus;
  }
  bool any = bind_one(UnixName::Kind::Abstract, linux_path, StringView(), bus);
  if (files && !host_path.empty()) {
    String folder(allocator_);
    for (uint64_t i = 1; i < host_path.size(); ++i)
      if (host_path[i] == '/') {
        if (!folder.assign(host_path.first(i))) return any;
        filesystem::layout::create_folder(*files, folder.c_str());
      }
    String path(allocator_);
    if (!path.assign(host_path)) return any;
    files->remove(path.view());      // (one left from an earlier start would say "address in use")
    UniquePointer<filesystem::IFile> file;
    if (files->open_file(path.view(), filesystem::OpenMode::CreateOrOpen, &file) == filesystem::Status::Ok) {
      file.reset();
      any |= bind_one(UnixName::Kind::Path, linux_path, path.view(), bus);
    }
  }
  return any;
}

bool listen_standard_sockets(SocketServer& server, Bus& system, Bus& session, filesystem::IFileSystem* files, allocators::IAllocator& allocator) {
  namespace layout = filesystem::layout;
  const bool a = server.listen(system, layout::kLinuxSystemBusSocket, layout::kSystemBusSocket, files);
  String linux_path(allocator), host_path(allocator);
  bool b = false;
  if (linux_path.append(layout::kLinuxRuntimeFolder) && linux_path.push_back('/') && linux_path.append(layout::kSessionBusSocket) &&
      host_path.append(layout::kRuntimeFolder) && host_path.push_back('/') && host_path.append(layout::kSessionBusSocket))
    b = server.listen(session, linux_path.view(), host_path.view(), files);
  return a && b;
}

bool SocketServer::pump(uint64_t now_us) {
  bool moved = false;
  List<Accepted> accepted(allocator_);
  process::ISystemSync& sync = sockets_.sync();
  sync.lock();
  for (uint32_t i = 0; i < listener_count_; ++i) {
    for (;;) {
      uint32_t id = 0;
      if (sockets_.accept_locked(listeners_[i].socket, &id) != UnixStatus::Ok) break;
      uint64_t pid = 0;
      sockets_.peer_pid_locked(id, &pid);
      if (!accepted.push_back(Accepted{id, pid, listeners_[i].bus})) {
        sync.unlock();
        sockets_.release(id);
        sync.lock();
        continue;
      }
      moved = true;
    }
  }
  for (UniquePointer<Connection>& p : connections_.view()) {
    Connection& c = *p;
    if (c.dead) continue;
    send_owed_locked(c, &moved);
    uint64_t taken = 0;
    while (!c.dead && taken < kMaxReadPerRound) {
      process::UnixSocketManager::Received got;
      const UnixStatus st = sockets_.receive_locked(c.socket, chunk_.writable_view(), false, &got, &c.received);
      if (st == UnixStatus::WouldBlock) break;
      if (st != UnixStatus::Ok || got.end_of_file) { c.dead = true; break; }
      if (got.got == 0) break;
      if (!c.inbox.append(chunk_.view().first(got.got))) { c.dead = true; break; }
      taken += got.got;
      moved = true;
    }
  }
  sync.unlock();
  for (const Accepted& a : accepted) {
    const uint32_t client = a.bus->open_client(a.pid, 0, true);
    UniquePointer<Connection> c = client ? UniquePointer<Connection>::make(allocator_, allocator_) : UniquePointer<Connection>();
    if (!c) {
      if (client) a.bus->close_client(client);
      sockets_.release(a.socket);
      continue;
    }
    c->socket = a.socket;
    c->client = client;
    c->bus = a.bus;
    if (!connections_.push_back(util::move(c))) {
      a.bus->close_client(client);
      sockets_.release(a.socket);
    }
  }
  for (UniquePointer<Connection>& p : connections_.view()) {
    Connection& c = *p;
    if (c.inbox.size() == 0 && c.received.count() == 0) continue;
    Queue<FdRef> fds(allocator_);
    UnixInFlight e;
    while (c.received.dequeue(&e)) {
      const process::DescriptorKind kind = e.kind;
      const uint64_t token = e.token;
      FdRef fd = FdRef::make_as<KernelFd>(allocator_, sockets_, util::move(e));
      if (!fd) {      // (not made: the use is still this transport's)
        c.dead = true;
        if (token != 0) {
          if (kind == process::DescriptorKind::UnixSocket) sockets_.release(uint32_t(token));
          else process::descriptor_token_release(kind, token);
        }
        continue;
      }
      if (!fds.enqueue(util::move(fd))) c.dead = true;      // (the KernelFd lets go of it)
    }
    c.bus->receive(c.client, c.inbox.view(), &fds);
    c.inbox.clear();
  }
  for (uint32_t i = 0; i < bus_count_; ++i) buses_[i]->tick(now_us);
  for (UniquePointer<Connection>& p : connections_.view()) {
    Connection& c = *p;
    if (c.sent == c.pending.size()) {
      c.pending.clear();
      c.sent = 0;
    }
    if (c.sent >= kCompactAt) {      // (a peer that reads as fast as it gets never empties the buffer: what went is dropped from the front)
      ByteBuffer rest(allocator_);
      if (!rest.append(c.pending.view().subview(c.sent))) { c.dead = true; continue; }
      c.pending.clear();
      c.pending.append(rest.view());
      for (UniquePointer<Connection::Batch>& b : c.batches.view()) b->at = b->at > c.sent ? b->at - c.sent : 0;
      c.sent = 0;
    }
    if (c.pending.size() - c.sent >= kTakeBelow) continue;
    // (the batches' offsets count from the start of `pending`)
    const uint64_t before = c.pending.size();
    List<Bus::OutFd> fds(allocator_);
    if (c.bus->take_output(c.client, &c.pending, &fds)) c.closing = true;
    if (fds.count()) queue_fds(c, 0, fds);
    if (c.pending.size() != before) moved = true;
  }
  sync.lock();
  for (UniquePointer<Connection>& p : connections_.view()) send_owed_locked(*p, &moved);
  sync.unlock();
  for (uint64_t i = 0; i < connections_.count();) {
    Connection& c = *connections_[i];
    if (c.dead || (c.closing && c.sent == c.pending.size())) {
      c.bus->close_client(c.client);
      release_entries(c.received);
      drop_rights(c);
      sockets_.release(c.socket);
      connections_.remove_at(i);
      moved = true;
    } else {
      ++i;
    }
  }
  return moved;
}

bool SocketServer::ready_locked() {
  for (uint32_t i = 0; i < listener_count_; ++i)
    if (sockets_.readiness_locked(listeners_[i].socket).readable) return true;
  for (const UniquePointer<Connection>& p : connections_.view()) {
    const process::UnixReadiness r = sockets_.readiness_locked(p->socket);
    if (r.readable || r.error || r.hangup) return true;
    if (p->sent < p->pending.size() && r.writable) return true;
  }
  return false;
}

void SocketServer::wait(uint64_t timeout_ns) {
  process::ISystemSync& sync = sockets_.sync();
  sync.lock();
  if (!stopping_ && !ready_locked()) sync.wait(timeout_ns);
  sync.unlock();
}

void SocketServer::thread_main(void* self) {
  SocketServer& s = *static_cast<SocketServer*>(self);
  while (!s.stopping_) {
    for (uint32_t rounds = 0; rounds < 16 && s.pump(hardware::uptime_us()); ++rounds) {}
    s.wait(100ull * 1000 * 1000);      // (100 ms: the notifications' timers need no finer tick)
  }
}

bool SocketServer::start() {
  if (thread_) return true;
  stopping_ = false;
  thread_ = kernel::spawn(&SocketServer::thread_main, this, "dbus");
  return thread_ != nullptr;
}

void SocketServer::stop() {
  if (!thread_) return;
  stopping_ = true;
  process::ISystemSync& sync = sockets_.sync();
  sync.lock();
  sync.notify_all();
  sync.unlock();
  kernel::join(thread_);
  thread_ = nullptr;
}

}  // namespace dbus
