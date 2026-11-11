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

#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/Queue.hpp"
#include "Process/Objects.hpp"
#include "Process/Process.hpp"
#include "Process/ProcessFiles.hpp"
#include "Process/UnixSocketAddress.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

// AF_UNIX sockets: one system-wide set, shared by Linux and Windows programs. A Linux program reaches them through socket(2) and friends (LinuxSockets.cpp), a
// Windows one through ws2_32.dll's AF_UNIX support and the socket NT services (WindowsSockets.cpp); both hand the same sockets the same names, so a Windows
// server and a Linux client (or the other way round) talk over one connection exactly like two programs of the same kind.
//
// Names. A path name is bound to the file it names in the OS's file system, not to the text: each program's path is turned into the path of the file system
// underneath (ProcessFiles::host_path_of), which is the same file for "C:\tmp\app.sock" in a Windows program and "/mnt/c/tmp/app.sock" in a Linux one (Linux
// programs see the Windows C: drive at /mnt/c, as under WSL). bind() creates that file and it stays after the socket is closed, as on both systems: the next
// bind() of the name fails with "address in use" until it is unlinked. An abstract name (Linux: sun_path starting with a NUL) has no file; the abstract names
// are one namespace for both kinds of program. getsockname()/getpeername() report a name the way the program that bound it spelled it.
//
// Types: SOCK_STREAM (bytes), SOCK_SEQPACKET (connection-oriented messages) and SOCK_DGRAM (messages, connected or not). A connect() of a stream or seqpacket
// socket completes at once when the listener's backlog has room (the listener's accept() then finds the other end), and waits (or says "would block") while it is
// full. Each socket has one receive queue of kBufferBytes; a sender waits for room in its peer's.
//
// Ancillary data: descriptors passed with SCM_RIGHTS (Linux) travel as InFlight entries holding a use of what they refer to; credentials (SO_PEERCRED,
// SCM_CREDENTIALS, Windows' SIO_AF_UNIX_GETPEERPID) are the process ids of the two ends (uid and gid are 0: there are no users yet).
//
// Locking: everything here is guarded by the system's one lock and wake-up (ISystemSync, the one the synchronization objects and pipes use), because a blocked
// call must look at a socket and go to sleep without a wake-up falling in between. The _locked calls need that lock held; the others take it themselves. Every
// change a waiter could be waiting for wakes everybody (notify_all). Nothing here touches the file system or another manager while the lock is held: the system
// calls make and look up the socket files before they take it, and descriptors that a socket's queue held when it went away are released after it is let go
// (release() does that, see take_dropped_locked()).
namespace process {

enum class UnixType : uint8_t { Stream = 1, Datagram = 2, SeqPacket = 5 };      // the Linux SOCK_* numbers (Windows has the same for the first two)

// What an operation came to. Each ABI turns it into its own error (an errno, a WSA error through an NTSTATUS).
enum class UnixStatus : uint8_t {
  Ok,
  WouldBlock,           // not ready: wait (a blocking socket) or report EAGAIN / WSAEWOULDBLOCK
  Invalid,              // EINVAL
  NoMemory,             // ENOMEM / ENOBUFS
  NoSockets,            // ENFILE: the table is full
  BadSocket,            // the id is not a socket (closed meanwhile)
  AddressInUse,         // EADDRINUSE
  NotFound,             // ENOENT: no such socket file / abstract name
  Refused,              // ECONNREFUSED: the name is there but nothing listens on it
  WrongType,            // EPROTOTYPE: the socket at the name is of another type
  AlreadyConnected,     // EISCONN
  NotConnected,         // ENOTCONN
  DestinationRequired,  // EDESTADDRREQ
  Broken,               // EPIPE: the other end is gone or does not take more
  Reset,                // ECONNRESET
  MessageTooBig,        // EMSGSIZE
  NotSupported,         // EOPNOTSUPP
  NotPermitted,         // EPERM (a datagram to a socket connected to someone else)
};

// A descriptor carried by a message (SCM_RIGHTS): what a descriptor table slot holds, plus the use of its token that the message keeps (descriptor_token_add).
struct UnixInFlight {
  DescriptorKind kind = DescriptorKind::Closed;
  SharedPointer<OpenObject> object;
  uint64_t token = 0;
};

// What poll()/select() want to know about one socket.
struct UnixReadiness {
  bool readable = false;       // recv()/accept() would not block (data, a connection, end of file or an error)
  bool writable = false;       // send() would not block
  bool hangup = false;         // POLLHUP: the connection is over in both directions
  bool read_hangup = false;    // POLLRDHUP: the other end will send nothing more
  bool error = false;          // SO_ERROR would report something
};

class UnixSocketManager {
 public:
  static constexpr uint32_t kCapacity = 4096;            // sockets at once, in the whole system
  static constexpr uint32_t kBufferBytes = 128 * 1024;   // one socket's receive queue (SO_RCVBUF; SO_SNDBUF reports the same)
  static constexpr uint32_t kMaxRecords = 256;           // messages (or stream segments with descriptors) one queue keeps
  static constexpr uint32_t kMaxRights = 253;            // descriptors in one message (Linux's SCM_MAX_FD)
  static constexpr uint32_t kMaxBacklog = 4096;          // SOMAXCONN

  UnixSocketManager(ISystemSync& sync, allocators::IAllocator& allocator);
  ~UnixSocketManager();
  UnixSocketManager(const UnixSocketManager&) = delete;
  UnixSocketManager& operator=(const UnixSocketManager&) = delete;

  ISystemSync& sync() { return sync_; }
  allocators::IAllocator& allocator() { return allocator_; }

  // --- lifetime (these take the lock themselves). A new socket has one use, for the descriptor the caller makes for it.
  UnixStatus create(UnixType type, uint64_t owner_pid, uint32_t* id);
  UnixStatus create_pair(UnixType type, uint64_t owner_pid, uint32_t* first, uint32_t* second);      // socketpair(): connected to each other, unnamed
  void add_ref(uint32_t id);
  // The last use gone, the socket goes: its name is freed, its peer sees the end of the connection, and the descriptors its queue still held are released.
  void release(uint32_t id);
  bool exists(uint32_t id);

  // --- operations (the lock held). `pid` is the calling process's id.
  bool exists_locked(uint32_t id) const { return at(id) != nullptr; }
  bool type_locked(uint32_t id, UnixType* type) const;
  // A path name is bound by the file `host_path` (empty for an abstract name or autobind, an unnamed `name`: then an abstract name of five hex digits is made up).
  // The caller made that file and turns AddressInUse into removing nothing: the file was there before (see LinuxSockets/WindowsSockets).
  UnixStatus bind_locked(uint32_t id, const UnixName& name, StringView host_path);
  UnixStatus listen_locked(uint32_t id, uint32_t backlog);
  // Stream/seqpacket: connects to the listener bound at the name (WouldBlock while its backlog is full). Datagram: makes it the default destination and the only
  // sender this socket takes datagrams from (an Unnamed `name` with `disconnect` dissolves that, connect(AF_UNSPEC)).
  UnixStatus connect_locked(uint32_t id, const UnixName& name, StringView host_path, uint64_t pid, bool disconnect = false);
  // Takes the oldest waiting connection: *out_id is a new socket with one use (for the caller's descriptor).
  UnixStatus accept_locked(uint32_t id, uint32_t* out_id);
  // Sends `data` (to `to` for an unconnected datagram socket; null otherwise). A stream socket may move part of it (*sent says how much; WouldBlock only if nothing
  // fits), the message types all of it or nothing. `rights` go with the first byte; on success they belong to the receiver (the caller's vector is emptied),
  // on failure they stay the caller's.
  struct Destination {
    const UnixName* name = nullptr;
    StringView host_path;
  };
  UnixStatus send_locked(uint32_t id, ConstByteView data, const Destination* to, uint64_t pid, Queue<UnixInFlight>* rights, uint32_t* sent);
  // Receives into `buffer`. Stream: what is there up to its size, stopping before a segment that carries descriptors when bytes were already taken; the message
  // types: one message, *message_size its full size (it was cut off if bigger than the buffer). `peek` leaves it queued. *end_of_file: the other end will not
  // send more and nothing is left (then Ok with *got 0). `from` (datagrams), `sender_pid` and `rights` are filled in when given.
  struct Received {
    uint32_t got = 0;
    uint32_t message_size = 0;
    bool end_of_file = false;
    UnixName from;
    uint64_t sender_pid = 0;
  };
  UnixStatus receive_locked(uint32_t id, ByteView buffer, bool peek, Received* out, Queue<UnixInFlight>* rights);
  UnixStatus shutdown_locked(uint32_t id, bool reading, bool writing);

  UnixReadiness readiness_locked(uint32_t id) const;
  uint32_t readable_bytes_locked(uint32_t id) const;      // FIONREAD: bytes waiting (a message socket: the size of the next message)
  bool local_name_locked(uint32_t id, UnixName* out) const;
  // False if not connected (ENOTCONN).
  bool peer_name_locked(uint32_t id, UnixName* out) const;
  bool peer_pid_locked(uint32_t id, uint64_t* pid) const;
  bool is_listening_locked(uint32_t id) const;
  bool is_connected_locked(uint32_t id) const;
  // SO_ERROR: the pending error (Reset after the other end went away with data unread), cleared by asking.
  UnixStatus take_error_locked(uint32_t id);
  bool non_blocking_locked(uint32_t id) const;
  void set_non_blocking_locked(uint32_t id, bool on);
  bool pass_credentials_locked(uint32_t id) const;
  void set_pass_credentials_locked(uint32_t id, bool on);
  // SO_RCVTIMEO / SO_SNDTIMEO in nanoseconds; ~0 is none (wait for ever).
  uint64_t timeout_locked(uint32_t id, bool sending) const;
  void set_timeout_locked(uint32_t id, bool sending, uint64_t ns);
  // Is a socket bound to this file right now? (stat() reports such a file as a socket.)
  bool path_bound_locked(StringView host_path) const;
  bool path_bound(StringView host_path);

  // Descriptors that sockets released under the lock still hold uses of (their queues' InFlight entries): the caller lets go of the lock, then releases them
  // with release_dropped(). release() does both itself.
  bool has_dropped_locked() const { return dropped_.count() != 0; }
  void take_dropped_locked(Queue<UnixInFlight>* out);
  void release_dropped(Queue<UnixInFlight>& dropped);
  // Releases rights that were never sent, or that a receiver had no room for (the lock NOT held).
  void release_rights(Queue<UnixInFlight>& rights) { release_dropped(rights); }

 private:
  enum class State : uint8_t { Unconnected, Listening, Connected };
  struct Record {
    uint32_t bytes = 0;          // how many of the queue's bytes are this message (or stream segment)
    uint32_t rights = 0;         // InFlight entries of the queue that go with its first byte
    uint64_t sender_pid = 0;
    UnixName from;               // the sender's name (datagrams)
  };
  struct Socket {
    explicit Socket(allocators::IAllocator& allocator) : host_path(allocator), data(allocator), records(allocator), rights(allocator), backlog(allocator) {}
    uint32_t id = 0;
    uint32_t references = 0;
    UnixType type = UnixType::Stream;
    State state = State::Unconnected;
    uint64_t owner_pid = 0;
    bool bound = false;
    UnixName name;
    String host_path;                    // the file a path name is bound to (empty: abstract or unnamed)
    uint32_t peer = 0;                   // the other end (stream/seqpacket), the default destination (datagram); 0: none
    uint64_t peer_pid = 0;
    UnixName peer_name;
    bool was_connected = false;          // (a stream socket whose peer went: end of file, not "not connected")
    bool peer_gone = false;
    bool shut_read = false, shut_write = false;
    UnixStatus pending_error = UnixStatus::Ok;
    bool non_blocking = false, pass_credentials = false;
    uint64_t receive_timeout = ~uint64_t{0}, send_timeout = ~uint64_t{0};
    // The receive queue: bytes [data_head, data.size()) of `data`, split into records; the descriptors they carry, in order.
    ByteBuffer data;
    uint64_t data_head = 0;
    Queue<Record> records;
    Queue<UnixInFlight> rights;
    uint32_t backlog_limit = 0;
    Queue<uint32_t> backlog;            // connections not accepted yet: their server-side sockets (each holds the one use the backlog keeps)
    uint64_t queued() const { return data.size() - data_head; }
  };

  Socket* at(uint32_t id) const;
  UnixStatus make_locked(UnixType type, uint64_t owner_pid, uint32_t* id);
  void release_locked(uint32_t id);
  Socket* find_bound_locked(const UnixName& name, StringView host_path) const;
  bool room_for(const Socket& receiver, uint64_t bytes, bool whole) const;
  bool enqueue_locked(Socket& receiver, ConstByteView data, Queue<UnixInFlight>* rights, uint64_t sender_pid, const UnixName& from, bool merge);
  void consume_locked(Socket& s, uint64_t bytes);
  bool reading_ended(const Socket& s) const;

  ISystemSync& sync_;
  allocators::IAllocator& allocator_;
  UniquePointer<Socket> slots_[kCapacity];
  uint16_t generations_[kCapacity] = {};
  uint32_t next_free_ = 0;                // where the search for a free slot starts
  uint32_t autobind_counter_ = 0;
  Queue<UnixInFlight> dropped_;
};

// The system's AF_UNIX sockets: made on first use from the system objects' lock and allocator (null until those exist).
UnixSocketManager* system_unix_sockets();
// Tests: use this one instead (null: back to the made-on-first-use one).
void set_system_unix_sockets(UnixSocketManager* manager);

// Runs `attempt` with the lock held until it says it is done (returns true), sleeping between tries until something changes (the process's own lock is let go
// meanwhile, as windowsabi::wait_until does). 0 when done; 1 when `timeout_ns` (~0 = none) passed; 2 when the process is exiting or was interrupted.
template <typename Attempt>
uint32_t unix_wait(Process& p, UnixSocketManager& sockets, uint64_t timeout_ns, Attempt&& attempt) {
  ISystemSync& sync = sockets.sync();
  IProcessRuntime* runtime = p.runtime();
  const uint64_t started = p.host().monotonic_ns();
  sync.lock();
  for (;;) {
    if (attempt()) { sync.unlock(); return 0; }
    uint64_t left = ~uint64_t{0};
    if (timeout_ns != ~uint64_t{0}) {
      const uint64_t used = p.host().monotonic_ns() - started;
      if (used >= timeout_ns) { sync.unlock(); return 1; }
      left = timeout_ns - used;
    }
    if (p.exited() || (runtime && runtime->interrupted())) { sync.unlock(); return 2; }
    if (runtime && runtime->park_wanted()) {      // (asked to suspend while it waits: it stops here and goes on waiting when resumed)
      sync.unlock();
      const int parked = runtime->park_point();
      sync.lock();
      if (parked == 2) { sync.unlock(); return 2; }
      continue;
    }
    if (runtime) runtime->unlock();
    else p.host().yield();
    sync.wait(left);
    sync.unlock();
    if (runtime) runtime->lock();
    sync.lock();
  }
}

}  // namespace process
