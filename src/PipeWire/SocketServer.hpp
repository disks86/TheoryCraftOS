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
// Descriptors: the server is the protocols' IDescriptorMaker; descriptors clients send (SCM_RIGHTS) go to the protocol with the bytes they came with
// (receive_with_fds): a memfd as its memory, the bytes the file has when it comes (a client's own buffers for the native protocol's port_buffers). Memory is a file in memory (memfd, Process/MemoryFile.hpp) the kernel reads and writes in
// place while the client maps it; an event is an eventfd (UnixType::Event in the socket manager). What a protocol hands out goes with SCM_RIGHTS on the
// first bytes of the output it came with. Descriptors the protocols let go of are released by this thread (a graveyard), never under another lock.
//
// Locking: as the X server's transport: sockets are moved under the system's lock, the protocols are called with it let go.
// (an event's signal() and take_signals() take the system's lock for a moment: they are called from the protocols' tick, with it let go.)
#pragma once
#include <stdint.h>
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Thread.hpp"
#include "PipeWire/ProtocolServer.hpp"
#include "Process/UnixSockets.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

// Where the transport's GPU images come from (make_gpu_image): on this system exports of /dev/dxg the kernel's Direct3D layer makes (main.cpp), as an open
// file a descriptor can be passed of (its gpu_buffer the export). Called on the transport's thread.
class IGpuImages {
 public:
  // An image of width x height, 4 bytes a pixel (B, G, R, A), as an entry to pass; *stride its bytes from row to row. False when it cannot be made.
  virtual bool make(uint32_t width, uint32_t height, process::UnixInFlight* out, uint32_t* stride) = 0;

 protected:
  ~IGpuImages() = default;
};

class SocketServer final : public IDescriptorMaker {
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
  // --- IDescriptorMaker
  Fd make_memory(uint64_t size, StringView name) override;
  Fd make_event() override;
  Fd make_gpu_image(uint32_t width, uint32_t height, uint32_t* stride) override;
  bool gpu_images() const override { return gpu_images_ != nullptr; }
  // Where GPU images come from (null: none can be made). Set before the thread starts; it must outlive the transport.
  void set_gpu_images(IGpuImages* images) { gpu_images_ = images; }
  struct Graveyard;

 private:
  struct Listener {
    uint32_t socket;
    IProtocolServer* protocol;
  };
  struct Connection {
    explicit Connection(allocators::IAllocator& allocator) : inbox(allocator), pending(allocator), batches(allocator), arrived(allocator) {}
    uint32_t socket = 0, client = 0;
    IProtocolServer* protocol = nullptr;
    ByteBuffer inbox;
    ByteBuffer pending;
    uint64_t sent = 0;
    struct Batch {      // descriptors still to go (each with a use of its own), with the byte `at` of `pending` and after the batches before
      explicit Batch(allocators::IAllocator& allocator) : rights(allocator) {}
      uint64_t at = 0;
      Queue<process::UnixInFlight> rights;
    };
    List<UniquePointer<Batch>> batches;
    Queue<process::UnixInFlight> arrived;      // descriptors the client sent (SCM_RIGHTS) that the protocol has not been given yet
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
  void send_owed_locked(Connection& c, bool* moved);
  void queue_fds(Connection& c, uint64_t at, const List<Fd>& fds);
  void drop_rights(Connection& c);
  void drain_graveyard();
  void release_entries(Queue<process::UnixInFlight>& entries);
  Fd adopt(process::UnixInFlight entry);

  process::UnixSocketManager& sockets_;
  allocators::IAllocator& allocator_;
  IGpuImages* gpu_images_ = nullptr;
  Listener listeners_[kMaxListeners] = {};
  uint32_t listener_count_ = 0;
  IProtocolServer* protocols_[kMaxListeners] = {};
  uint32_t protocol_count_ = 0;
  List<UniquePointer<Connection>> connections_;
  ByteBuffer chunk_;
  SharedPointer<Graveyard> graveyard_;
  volatile bool stopping_ = false;
  kernel::Thread* thread_ = nullptr;
};

// Listens for `native` at $XDG_RUNTIME_DIR/pipewire-0 and `pulse` at $XDG_RUNTIME_DIR/pulse/native (the abstract names and, with `files`, the socket files
// under filesystem::layout::kRuntimeFolder). False when either could not be bound at all.
bool listen_standard_sockets(SocketServer& server, IProtocolServer& native, IProtocolServer& pulse, filesystem::IFileSystem* files, allocators::IAllocator& allocator);

}  // namespace pipewire
