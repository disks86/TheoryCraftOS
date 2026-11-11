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

#include "X11/X11SocketServer.hpp"
#include "FileSystem/SystemLayout.hpp"

namespace x11 {

using process::UnixName;
using process::UnixStatus;

SocketServer::SocketServer(Server& server, process::UnixSocketManager& sockets, allocators::IAllocator& allocator)
    : server_(server), sockets_(sockets), allocator_(allocator), connections_(allocator), chunk_(allocator) {
  chunk_.put_zeros(kReadChunk);
  server_.set_wake(&SocketServer::on_wake, this);
}

SocketServer::~SocketServer() {
  stop();
  server_.set_wake(nullptr, nullptr);
  for (UniquePointer<Connection>& c : connections_.view()) {
    server_.close_client(c->client);
    sockets_.release(c->socket);
  }
  connections_.clear();
  for (uint32_t i = 0; i < listener_count_; ++i) sockets_.release(listeners_[i]);
  listener_count_ = 0;
}

void SocketServer::on_wake(void* self) {
  SocketServer& s = *static_cast<SocketServer*>(self);
  process::ISystemSync& sync = s.sockets_.sync();
  sync.lock();
  s.woken_ = true;
  sync.notify_all();
  sync.unlock();
}

bool SocketServer::listen(uint32_t display, filesystem::IFileSystem* files) {
  if (display > 99) return false;
  // "X<n>" after the folder
  char leaf[4] = {'X', 0, 0, 0};
  uint32_t leaf_length = 1;
  if (display >= 10) leaf[leaf_length++] = static_cast<char>('0' + display / 10);
  leaf[leaf_length++] = static_cast<char>('0' + display % 10);
  const StringView name_leaf(leaf, leaf_length);
  auto bind_one = [&](UnixName::Kind kind, StringView name, StringView host_path) {
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
    listeners_[listener_count_++] = id;
    return true;
  };
  String linux_path(allocator_), host_path(allocator_);
  if (!linux_path.append(filesystem::layout::kLinuxX11SocketFolder) || !linux_path.push_back('/') || !linux_path.append(name_leaf)) return false;
  bool any = bind_one(UnixName::Kind::Abstract, linux_path.view(), StringView());
  if (files) {
    // the socket's file, made afresh (one left from an earlier start would say "address in use")
    if (!host_path.append(filesystem::layout::kX11SocketFolder) || !host_path.push_back('/') || !host_path.append(name_leaf)) return any;
    filesystem::layout::create_folder(*files, filesystem::layout::kLinux);
    filesystem::layout::create_folder(*files, filesystem::layout::kLinuxTmp);
    filesystem::layout::create_folder(*files, filesystem::layout::kX11SocketFolder);
    files->remove(host_path.view());
    UniquePointer<filesystem::IFile> file;
    if (files->open_file(host_path.view(), filesystem::OpenMode::CreateOrOpen, &file) == filesystem::Status::Ok) {
      file.reset();
      any |= bind_one(UnixName::Kind::Path, linux_path.view(), host_path.view());
    }
  }
  return any;
}

bool SocketServer::pump() {
  bool moved = false;
  List<uint32_t> accepted(allocator_);
  List<uint64_t> accepted_pids(allocator_);
  process::ISystemSync& sync = sockets_.sync();
  sync.lock();
  woken_ = false;
  for (uint32_t i = 0; i < listener_count_; ++i) {
    for (;;) {
      uint32_t id = 0;
      if (sockets_.accept_locked(listeners_[i], &id) != UnixStatus::Ok) break;
      uint64_t pid = 0;
      sockets_.peer_pid_locked(id, &pid);
      if (!accepted.push_back(id) || !accepted_pids.push_back(pid)) {
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
    // what is owed, as far as the socket takes it
    while (c.sent < c.pending.size()) {
      uint32_t sent = 0;
      const UnixStatus st = sockets_.send_locked(c.socket, c.pending.view().subview(c.sent), nullptr, 0, nullptr, &sent);
      if (st == UnixStatus::WouldBlock) break;
      if (st != UnixStatus::Ok) { c.dead = true; break; }
      c.sent += sent;
      moved = true;
      if (sent == 0) break;
    }
    // what came
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
  // new clients
  for (uint64_t i = 0; i < accepted.count(); ++i) {
    const uint32_t client = server_.open_client(accepted_pids[i]);
    UniquePointer<Connection> c = client ? UniquePointer<Connection>::make(allocator_, allocator_) : UniquePointer<Connection>();
    if (!c) {
      if (client) server_.close_client(client);
      sockets_.release(accepted[i]);      // (the table is full: the client sees the connection end)
      continue;
    }
    c->socket = accepted[i];
    c->client = client;
    if (!connections_.push_back(util::move(c))) {
      server_.close_client(client);
      sockets_.release(accepted[i]);
    }
  }
  // the server: requests in, replies and events out
  for (UniquePointer<Connection>& p : connections_.view()) {
    Connection& c = *p;
    if (c.inbox.size() > 0) {
      server_.receive(c.client, c.inbox.view());
      c.inbox.clear();
    }
    if (c.sent == c.pending.size()) {
      c.pending.clear();
      c.sent = 0;
    }
    const uint64_t before = c.pending.size();
    if (server_.take_output(c.client, &c.pending)) c.closing = true;
    if (c.pending.size() != before) moved = true;
  }
  // the connections that are over: gone, or done and everything sent
  for (uint64_t i = 0; i < connections_.count();) {
    Connection& c = *connections_[i];
    if (c.dead || (c.closing && c.sent == c.pending.size())) {
      close(c);
      connections_.remove_at(i);
      moved = true;
    } else {
      ++i;
    }
  }
  return moved;
}

void SocketServer::close(Connection& c) {
  server_.close_client(c.client);
  sockets_.release(c.socket);
}

bool SocketServer::ready_locked() {
  if (woken_) return true;
  for (uint32_t i = 0; i < listener_count_; ++i)
    if (sockets_.readiness_locked(listeners_[i]).readable) return true;
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
    for (uint32_t rounds = 0; rounds < 64 && s.pump(); ++rounds) {}
    s.wait(100u * 1000 * 1000);
  }
}

bool SocketServer::start() {
  if (thread_) return true;
  stopping_ = false;
  thread_ = kernel::spawn(&SocketServer::thread_main, this, "x11-server");
  return thread_ != nullptr;
}

void SocketServer::stop() {
  if (!thread_) return;
  stopping_ = true;
  on_wake(this);
  kernel::join(thread_);
  thread_ = nullptr;
}

}  // namespace x11
