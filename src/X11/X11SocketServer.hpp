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

// The X server's connections: the system's AF_UNIX sockets (src/Process/UnixSockets.hpp), carried by a kernel thread of the server's own.
//
// Display n is listened for under the two names libxcb and Xlib try for DISPLAY=:n -
//   the abstract name "/tmp/.X11-unix/Xn"   (one namespace for Linux and Windows programs: a Windows program reaches it through ws2_32's AF_UNIX with a
//                                            sun_path that starts with a NUL)
//   the path /tmp/.X11-unix/Xn               (of the Linux programs' tree: /Linux/tmp/.X11-unix/Xn on the system's file system)
// Each accepted connection is a client of the Server; bytes go in as they come, and what the server owes a client goes out as the client's socket has room
// (a client that does not read only stalls its own output; one that lets Server::kMaxOutputBytes pile up is dropped by the server).
//
// Locking: the sockets are moved under the system's lock (UnixSocketManager's ISystemSync); the server is fed with that lock let go (the server's own mutex
// and the system lock are never held together). The server's wake-up (input from the desktop, events for other clients) sets a flag and notifies under the
// system lock, so the thread cannot miss it between looking and going to sleep.
#pragma once
#include <stdint.h>
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Thread.hpp"
#include "Process/UnixSockets.hpp"
#include "X11/X11Server.hpp"

namespace x11 {

class SocketServer {
 public:
  static constexpr uint32_t kMaxListeners = 2;
  static constexpr uint64_t kReadChunk = 64 * 1024;
  static constexpr uint64_t kMaxReadPerRound = 1024 * 1024;      // from one client per round (the others get their turn)

  SocketServer(Server& server, process::UnixSocketManager& sockets, allocators::IAllocator& allocator);
  ~SocketServer();
  SocketServer(const SocketServer&) = delete;
  SocketServer& operator=(const SocketServer&) = delete;

  // Binds display `display`'s names (the path one only when `files` is given: the system's file system, where /Linux/tmp is). False when no name could be
  // bound (another server has them).
  bool listen(uint32_t display, filesystem::IFileSystem* files);
  // One round: new connections, the bytes that came, the output owed. True when anything moved (call again before waiting).
  bool pump();
  // Sleeps until a socket changes or the server wants to send, at most `timeout_ns`.
  void wait(uint64_t timeout_ns);
  // The server's thread: pump() and wait() until stop(). False when the thread could not be made.
  bool start();
  void stop();
  uint32_t connections() const { return static_cast<uint32_t>(connections_.count()); }

 private:
  struct Connection {
    explicit Connection(allocators::IAllocator& allocator) : inbox(allocator), pending(allocator) {}
    uint32_t socket = 0, client = 0;
    ByteBuffer inbox;       // read, not yet given to the server
    ByteBuffer pending;     // owed to the client; [0, sent) already went
    uint64_t sent = 0;
    bool dead = false;      // the socket is closed or broken
    bool closing = false;   // the server is done with it: close once pending is sent
  };
  static void on_wake(void* self);
  static void thread_main(void* self);
  bool ready_locked();
  void close(Connection& c);

  Server& server_;
  process::UnixSocketManager& sockets_;
  allocators::IAllocator& allocator_;
  uint32_t listeners_[kMaxListeners] = {};
  uint32_t listener_count_ = 0;
  List<UniquePointer<Connection>> connections_;
  ByteBuffer chunk_;
  bool woken_ = false;      // (under the system lock)
  volatile bool stopping_ = false;
  kernel::Thread* thread_ = nullptr;
};

}  // namespace x11
