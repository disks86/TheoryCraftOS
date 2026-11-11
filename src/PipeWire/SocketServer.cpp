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

#include "PipeWire/SocketServer.hpp"
#include "FileSystem/SystemLayout.hpp"

namespace pipewire {

using process::UnixName;
using process::UnixStatus;

SocketServer::SocketServer(process::UnixSocketManager& sockets, allocators::IAllocator& allocator)
    : sockets_(sockets), allocator_(allocator), connections_(allocator), chunk_(allocator) {
  chunk_.put_zeros(kReadChunk);
}

SocketServer::~SocketServer() {
  stop();
  for (UniquePointer<Connection>& c : connections_.view()) {
    c->protocol->close_client(c->client);
    sockets_.release(c->socket);
  }
  connections_.clear();
  for (uint32_t i = 0; i < listener_count_; ++i) sockets_.release(listeners_[i].socket);
  listener_count_ = 0;
}

bool SocketServer::bind_one(UnixName::Kind kind, StringView name, StringView host_path, IProtocolServer& protocol) {
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
  listeners_[listener_count_++] = Listener{id, &protocol};
  return true;
}

bool SocketServer::listen(IProtocolServer& protocol, StringView linux_path, StringView host_path, filesystem::IFileSystem* files) {
  bool known = false;
  for (uint32_t i = 0; i < protocol_count_; ++i) known |= protocols_[i] == &protocol;
  if (!known) {
    if (protocol_count_ == kMaxListeners) return false;
    protocols_[protocol_count_++] = &protocol;
  }
  bool any = bind_one(UnixName::Kind::Abstract, linux_path, StringView(), protocol);
  if (files && !host_path.empty()) {
    // the folders above the socket, then the socket's file made afresh (one left from an earlier start would say "address in use")
    String folder(allocator_);
    for (uint64_t i = 1; i < host_path.size(); ++i)
      if (host_path[i] == '/') {
        if (!folder.assign(host_path.first(i))) return any;
        filesystem::layout::create_folder(*files, folder.c_str());
      }
    String path(allocator_);
    if (!path.assign(host_path)) return any;
    files->remove(path.view());
    UniquePointer<filesystem::IFile> file;
    if (files->open_file(path.view(), filesystem::OpenMode::CreateOrOpen, &file) == filesystem::Status::Ok) {
      file.reset();
      any |= bind_one(UnixName::Kind::Path, linux_path, path.view(), protocol);
    }
  }
  return any;
}

bool listen_standard_sockets(SocketServer& server, IProtocolServer& native, IProtocolServer& pulse, filesystem::IFileSystem* files, allocators::IAllocator& allocator) {
  auto one = [&](IProtocolServer& protocol, const char* leaf) {
    String linux_path(allocator), host_path(allocator);
    if (!linux_path.append(filesystem::layout::kLinuxRuntimeFolder) || !linux_path.push_back('/') || !linux_path.append(leaf)) return false;
    if (!host_path.append(filesystem::layout::kRuntimeFolder) || !host_path.push_back('/') || !host_path.append(leaf)) return false;
    return server.listen(protocol, linux_path.view(), host_path.view(), files);
  };
  const bool a = one(native, filesystem::layout::kPipeWireSocket);
  const bool b = one(pulse, filesystem::layout::kPulseSocket);
  return a && b;
}

bool SocketServer::pump() {
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
      if (!accepted.push_back(Accepted{id, pid, listeners_[i].protocol})) {
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
    while (c.sent < c.pending.size()) {
      uint32_t sent = 0;
      const UnixStatus st = sockets_.send_locked(c.socket, c.pending.view().subview(c.sent), nullptr, 0, nullptr, &sent);
      if (st == UnixStatus::WouldBlock) break;
      if (st != UnixStatus::Ok) { c.dead = true; break; }
      c.sent += sent;
      moved = true;
      if (sent == 0) break;
    }
    uint64_t taken = 0;
    while (!c.dead && taken < kMaxReadPerRound) {
      process::UnixSocketManager::Received got;
      const UnixStatus st = sockets_.receive_locked(c.socket, chunk_.writable_view(), false, &got, nullptr);
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
    const uint32_t client = a.protocol->open_client(a.pid);
    UniquePointer<Connection> c = client ? UniquePointer<Connection>::make(allocator_, allocator_) : UniquePointer<Connection>();
    if (!c) {
      if (client) a.protocol->close_client(client);
      sockets_.release(a.socket);
      continue;
    }
    c->socket = a.socket;
    c->client = client;
    c->protocol = a.protocol;
    if (!connections_.push_back(util::move(c))) {
      a.protocol->close_client(client);
      sockets_.release(a.socket);
    }
  }
  for (UniquePointer<Connection>& p : connections_.view()) {
    Connection& c = *p;
    if (c.inbox.size() > 0) {
      c.protocol->receive(c.client, c.inbox.view());
      c.inbox.clear();
    }
  }
  // the protocols' own work (graph changes, flow control), then what each connection is owed
  for (uint32_t i = 0; i < protocol_count_; ++i) protocols_[i]->tick();
  for (UniquePointer<Connection>& p : connections_.view()) {
    Connection& c = *p;
    if (c.sent == c.pending.size()) {
      c.pending.clear();
      c.sent = 0;
    }
    const uint64_t before = c.pending.size();
    if (c.protocol->take_output(c.client, &c.pending)) c.closing = true;
    if (c.pending.size() != before) moved = true;
    if (c.pending.size() - c.sent > kMaxPending) c.dead = true;
  }
  for (uint64_t i = 0; i < connections_.count();) {
    Connection& c = *connections_[i];
    if (c.dead || (c.closing && c.sent == c.pending.size())) {
      c.protocol->close_client(c.client);
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
  if (!ready_locked()) sync.wait(timeout_ns);
  sync.unlock();
}

void SocketServer::thread_main(void* self) {
  SocketServer& s = *static_cast<SocketServer*>(self);
  while (!s.stopping_) {
    for (uint32_t rounds = 0; rounds < 16 && s.pump(); ++rounds) {}
    // the protocols want to be ticked this often (the PulseAudio streams' flow control every 10 ms)
    uint64_t interval_us = 100000;
    for (uint32_t i = 0; i < s.protocol_count_; ++i)
      if (s.protocols_[i]->tick_interval_us() < interval_us) interval_us = s.protocols_[i]->tick_interval_us();
    s.wait(interval_us * 1000);
  }
}

bool SocketServer::start() {
  if (thread_) return true;
  stopping_ = false;
  thread_ = kernel::spawn(&SocketServer::thread_main, this, "pipewire");
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

}  // namespace pipewire
