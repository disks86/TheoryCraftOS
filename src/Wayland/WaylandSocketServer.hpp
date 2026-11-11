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

// The Wayland compositor's connections: the system's AF_UNIX sockets (src/Process/UnixSockets.hpp), carried by a kernel thread of the compositor's own.
//
// It listens at $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY as Linux programs see it (/run/user/0/wayland-0; on the system's file system that is
// /Linux/run/user/0/wayland-0). Each accepted connection is a client of the Server; bytes and descriptors go in as they come, and what the server owes a client
// goes out as the client's socket has room, its descriptors with the first of those bytes.
//
// Descriptors: one a client passes (SCM_RIGHTS) that is a file in memory (memfd_create, Process/MemoryFile.hpp) is memory the server reads in place (a
// wl_shm pool); any other kind is handed to the server as one with no memory (the request that takes it fails). The ones the server sends (the keymap) are
// files in memory made here, sealed.
//
// Locking: the sockets are moved under the system's lock (UnixSocketManager's ISystemSync); the server is fed with that lock let go (the server's mutex
// and the system lock are never held together). The server's wake-up sets a flag and notifies under the system lock, so the thread cannot miss it between
// looking and going to sleep.
#pragma once
#include <stdint.h>
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Thread.hpp"
#include "Process/UnixSockets.hpp"
#include "Wayland/WaylandServer.hpp"

namespace wayland {

class SocketServer {
 public:
  static constexpr uint64_t kReadChunk = 64 * 1024;
  static constexpr uint64_t kMaxReadPerRound = 1024 * 1024;      // from one client per round (the others get their turn)

  SocketServer(Server& server, process::UnixSocketManager& sockets, allocators::IAllocator& allocator);
  ~SocketServer();
  SocketServer(const SocketServer&) = delete;
  SocketServer& operator=(const SocketServer&) = delete;

  // Binds the socket file (made afresh in `files`, the system's file system, where /Linux/run is). False when it could not be bound.
  bool listen(filesystem::IFileSystem& files);
  // One round: new connections, what came, what is owed. True when anything moved (call again before waiting).
  bool pump();
  // Sleeps until a socket changes or the server wants to send, at most `timeout_ns`.
  void wait(uint64_t timeout_ns);
  // The compositor's thread: pump() and wait() until stop(). False when the thread could not be made.
  bool start();
  void stop();
  uint32_t connections() const { return static_cast<uint32_t>(connections_.count()); }

 private:
  struct Connection {
    explicit Connection(allocators::IAllocator& allocator) : inbox(allocator), fds(allocator), pending(allocator), pending_rights(allocator) {}
    uint32_t socket = 0, client = 0;
    ByteBuffer inbox;                          // read, not yet given to the server
    List<Fd> fds;                              // descriptors that came with it
    ByteBuffer pending;                        // owed to the client; [0, sent) already went
    uint64_t sent = 0;
    Queue<process::UnixInFlight> pending_rights;      // to go with the next bytes sent
    bool dead = false;                         // the socket is closed or broken
    bool closing = false;                      // the server is done with it: close once everything is sent
  };
  static void on_wake(void* self);
  static void thread_main(void* self);
  static Fd make_fd(void* self, ConstByteView contents);
  bool ready_locked();
  void close(Connection& c);

  Server& server_;
  process::UnixSocketManager& sockets_;
  allocators::IAllocator& allocator_;
  uint32_t listener_ = 0;
  bool listening_ = false;
  List<UniquePointer<Connection>> connections_;
  ByteBuffer chunk_;
  bool woken_ = false;      // (under the system lock)
  volatile bool stopping_ = false;
  kernel::Thread* thread_ = nullptr;
};

}  // namespace wayland
