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

// The PipeWire sockets: the system's AF_UNIX sockets (src/Process/UnixSockets.hpp), carried by one kernel thread ("pipewire") for every protocol.
//
// Each protocol listens under a path of the Linux programs' tree and under the same abstract name (which Windows programs reach through ws2_32 too):
//   $XDG_RUNTIME_DIR/pipewire-0     PipeWire's native protocol (NativeServer)  -  /run/user/0/pipewire-0, on the system's disk /Linux/run/user/0/pipewire-0
//   $XDG_RUNTIME_DIR/pulse/native   PulseAudio's protocol (PulseServer)         -  /run/user/0/pulse/native
// (see filesystem::layout::kLinuxRuntimeFolder). Bytes go to the protocol as they come and its output as the socket has room; a connection whose
// protocol lets kMaxPending bytes pile up unsent is dropped. Each round also ticks the protocols (graph changes, flow control), at least every
// few milliseconds.
//
// Locking: as the X server's transport: sockets are moved under the system's lock, the protocols are called with it let go.
#pragma once
#include <stdint.h>
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Thread.hpp"
#include "PipeWire/ProtocolServer.hpp"
#include "Process/UnixSockets.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

class SocketServer {
 public:
  static constexpr uint32_t kMaxListeners = 8;
  static constexpr uint64_t kReadChunk = 64 * 1024;
  static constexpr uint64_t kMaxReadPerRound = 1024 * 1024;
  static constexpr uint64_t kMaxPending = 16u << 20;

  SocketServer(process::UnixSocketManager& sockets, allocators::IAllocator& allocator);
  ~SocketServer();
  SocketServer(const SocketServer&) = delete;
  SocketServer& operator=(const SocketServer&) = delete;

  // Listens for `protocol` at `linux_path` (a path as Linux programs see it, under the runtime folder): as the abstract name and, when `files` is
  // given, as the socket file `host_path` of the system's file system (its folders are made). False when neither could be bound.
  bool listen(IProtocolServer& protocol, StringView linux_path, StringView host_path, filesystem::IFileSystem* files);
  // One round: new connections, bytes in, protocol ticks, bytes out. True when anything moved.
  bool pump();
  void wait(uint64_t timeout_ns);
  bool start();
  void stop();
  uint32_t connections() const { return static_cast<uint32_t>(connections_.count()); }

 private:
  struct Listener {
    uint32_t socket;
    IProtocolServer* protocol;
  };
  struct Connection {
    explicit Connection(allocators::IAllocator& allocator) : inbox(allocator), pending(allocator) {}
    uint32_t socket = 0, client = 0;
    IProtocolServer* protocol = nullptr;
    ByteBuffer inbox;
    ByteBuffer pending;
    uint64_t sent = 0;
    bool dead = false;
    bool closing = false;
  };
  struct Accepted {
    uint32_t socket;
    uint64_t pid;
    IProtocolServer* protocol;
  };
  static void thread_main(void* self);
  bool ready_locked();
  bool bind_one(process::UnixName::Kind kind, StringView name, StringView host_path, IProtocolServer& protocol);

  process::UnixSocketManager& sockets_;
  allocators::IAllocator& allocator_;
  Listener listeners_[kMaxListeners] = {};
  uint32_t listener_count_ = 0;
  IProtocolServer* protocols_[kMaxListeners] = {};
  uint32_t protocol_count_ = 0;
  List<UniquePointer<Connection>> connections_;
  ByteBuffer chunk_;
  volatile bool stopping_ = false;
  kernel::Thread* thread_ = nullptr;
};

// Listens for `native` at $XDG_RUNTIME_DIR/pipewire-0 and `pulse` at $XDG_RUNTIME_DIR/pulse/native (the abstract names and, with `files`, the socket files
// under filesystem::layout::kRuntimeFolder). False when either could not be bound at all.
bool listen_standard_sockets(SocketServer& server, IProtocolServer& native, IProtocolServer& pulse, filesystem::IFileSystem* files, allocators::IAllocator& allocator);

}  // namespace pipewire
