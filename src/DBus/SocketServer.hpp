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

// The D-Bus sockets: the system's AF_UNIX sockets (src/Process/UnixSockets.hpp), carried for both buses by one kernel thread ("dbus").
//
//   system bus   /run/dbus/system_bus_socket   (DBUS_SYSTEM_BUS_ADDRESS's default; on the system's disk /Linux/run/dbus/system_bus_socket)
//   session bus  $XDG_RUNTIME_DIR/bus          (/run/user/0/bus; DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/0/bus is in the Linux programs' environment)
// Each is also bound as the abstract name of the same text, which Windows programs reach through ws2_32's AF_UNIX too (filesystem::layout has the paths).
//
// Bytes and descriptors (SCM_RIGHTS) go to the bus as they come; the bus's output goes out as the socket takes it, each message's descriptors with its
// first byte (a copy with a use of its own for each recipient). The bus is handed more output only while less than kTakeBelow bytes wait here, so a
// connection that does not read fills its queue in the bus (whose limit then holds) rather than here. Credentials are the socket's: the peer's process
// id, and uid 0 (there are no other users yet).
//
// Locking: sockets are moved under the system's lock (ISystemSync), the buses are called with it let go; descriptors are released with it let go too.
#pragma once
#include <stdint.h>
#include "DBus/Bus.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Thread.hpp"
#include "Process/UnixSockets.hpp"
#include "Utility/UniquePointer.hpp"

namespace dbus {

class SocketServer {
 public:
  static constexpr uint32_t kMaxListeners = 8;
  static constexpr uint64_t kReadChunk = 64 * 1024;
  static constexpr uint64_t kMaxReadPerRound = 1024 * 1024;
  static constexpr uint64_t kTakeBelow = 256 * 1024;
  static constexpr uint64_t kCompactAt = 64 * 1024;      // sent bytes dropped from the front of a connection's output

  SocketServer(process::UnixSocketManager& sockets, allocators::IAllocator& allocator);
  ~SocketServer();
  SocketServer(const SocketServer&) = delete;
  SocketServer& operator=(const SocketServer&) = delete;

  // Listens for `bus` at `linux_path` (as Linux programs see it): as the abstract name and, when `files` is given, as the socket file `host_path` of the
  // system's file system (its folders are made). False when neither could be bound.
  bool listen(Bus& bus, StringView linux_path, StringView host_path, filesystem::IFileSystem* files);
  // One round: new connections, bytes in, the buses' ticks (`now_us`), bytes out. True when anything moved.
  bool pump(uint64_t now_us);
  void wait(uint64_t timeout_ns);
  bool start();
  void stop();
  uint32_t connections() const { return uint32_t(connections_.count()); }

 private:
  struct Listener {
    uint32_t socket;
    Bus* bus;
  };
  struct Connection {
    explicit Connection(allocators::IAllocator& allocator) : inbox(allocator), received(allocator), pending(allocator), batches(allocator) {}
    uint32_t socket = 0, client = 0;
    Bus* bus = nullptr;
    ByteBuffer inbox;
    Queue<process::UnixInFlight> received;      // descriptors that came with `inbox`
    ByteBuffer pending;
    uint64_t sent = 0;
    struct Batch {      // descriptors (each with a use of its own) to go with byte `at` of `pending`
      explicit Batch(allocators::IAllocator& allocator) : rights(allocator) {}
      uint64_t at = 0;
      Queue<process::UnixInFlight> rights;
    };
    List<UniquePointer<Batch>> batches;
    bool dead = false, closing = false;
  };
  struct Accepted {
    uint32_t socket;
    uint64_t pid;
    Bus* bus;
  };
  static void thread_main(void* self);
  bool ready_locked();
  bool bind_one(process::UnixName::Kind kind, StringView name, StringView host_path, Bus& bus);
  void send_owed_locked(Connection& c, bool* moved);
  void queue_fds(Connection& c, uint64_t base, const List<Bus::OutFd>& fds);
  void release_entries(Queue<process::UnixInFlight>& entries);
  void drop_rights(Connection& c);

  process::UnixSocketManager& sockets_;
  allocators::IAllocator& allocator_;
  Listener listeners_[kMaxListeners] = {};
  uint32_t listener_count_ = 0;
  Bus* buses_[kMaxListeners] = {};
  uint32_t bus_count_ = 0;
  List<UniquePointer<Connection>> connections_;
  ByteBuffer chunk_;
  volatile bool stopping_ = false;
  kernel::Thread* thread_ = nullptr;
};

// Listens for `system` at /run/dbus/system_bus_socket and `session` at $XDG_RUNTIME_DIR/bus (the abstract names and, with `files`, the socket files under
// filesystem::layout::kLinuxRun). False when either could not be bound at all.
bool listen_standard_sockets(SocketServer& server, Bus& system, Bus& session, filesystem::IFileSystem* files, allocators::IAllocator& allocator);

}  // namespace dbus
