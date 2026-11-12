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

// The kernel's D-Bus message bus: what dbus-daemon does, inside the kernel. One Bus is the system bus, another the session bus (BusKind); the transport
// (SocketServer.hpp, or the host test server) carries bytes and descriptors between the bus and its connections.
//
// A connection authenticates (Auth.hpp), says Hello and gets its unique name (":1.N"). Then every message it sends gets its SENDER set and goes
//   - to the bus itself (destination org.freedesktop.DBus): the driver, BusDriver.cpp: names, match rules, credentials, introspection, properties, Peer,
//     Monitoring.BecomeMonitor;
//   - to the primary owner of its destination, and to connections whose match rules eavesdrop and to monitors;
//   - with no destination (a signal): to every connection with a match rule it matches, once each.
// A method return or error is delivered only as the reply to a call the bus delivered and that expects one (on the session bus an unrequested reply goes
// anyway, as dbus-daemon's session policy allows); when the callee goes away without replying the caller gets org.freedesktop.DBus.Error.NoReply.
// Names follow the specification's queueing rules (RequestName flags ALLOW_REPLACEMENT, REPLACE_EXISTING, DO_NOT_QUEUE), with NameOwnerChanged, NameLost
// and NameAcquired. Descriptors (UNIX_FDS) are passed on to a recipient that negotiated them; to one that did not the sender gets NotSupported.
// Nothing is activated: there are no .service files; a name the kernel serves itself is always running (see LocalPeer below).
//
// A connection that breaks the protocol (an invalid message, a message before Hello, a monitor that sends) is closed, as dbus-daemon does.
//
// Limits: kMax* below. A recipient whose unsent output passes kMaxOutgoingBytes gets no more until it reads; the sender of a call it would have got is
// told LimitsExceeded.
//
// Threads: a Bus is used by one thread (the transport's); it has no lock. Local peers are called on that thread too.
#pragma once
#include <stdint.h>
#include "DBus/Auth.hpp"
#include "DBus/Match.hpp"
#include "DBus/Message.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "ObjectContainer/Hive.hpp"
#include "ObjectContainer/List.hpp"
#include "ObjectContainer/Queue.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace dbus {

enum class BusKind : uint8_t { System, Session };

inline constexpr const char* kBusName = "org.freedesktop.DBus";
inline constexpr const char* kBusPath = "/org/freedesktop/DBus";

// RequestName's flags and replies, ReleaseName's replies, StartServiceByName's.
enum : uint32_t { kNameAllowReplacement = 1, kNameReplaceExisting = 2, kNameDoNotQueue = 4 };
enum : uint32_t { kRequestPrimaryOwner = 1, kRequestInQueue = 2, kRequestExists = 3, kRequestAlreadyOwner = 4 };
enum : uint32_t { kReleaseReleased = 1, kReleaseNonExistent = 2, kReleaseNotOwner = 3 };
enum : uint32_t { kStartSuccess = 1, kStartAlreadyRunning = 2 };

// A connection inside the kernel: a service the kernel offers on the bus itself (Notifications.hpp). It has a unique name like any other, owns names with
// Bus::request_name() and sends with Bus::send(); what comes for it is handed to deliver() at the end of the bus's receive(), close_client() or tick() that
// routed it (never from inside send()). Everything is on the bus's thread.
class LocalPeer {
 public:
  virtual void deliver(const Message& message) = 0;
  // Called every tick (now_us: the transport's clock).
  virtual void tick(uint64_t now_us) { (void)now_us; }

 protected:
  LocalPeer() = default;
  ~LocalPeer() = default;
};

class Bus {
 public:
  static constexpr uint32_t kMaxConnections = 2048;
  static constexpr uint32_t kMaxNamesPerConnection = 512;
  static constexpr uint32_t kMaxMatchesPerConnection = 4096;
  static constexpr uint32_t kMaxPendingRepliesPerConnection = 4096;
  static constexpr uint32_t kMaxFdsPerMessage = 253;          // (one sendmsg's worth: Linux's SCM_MAX_FD)
  static constexpr uint32_t kMaxIncomingFds = 1024;           // descriptors received ahead of their messages
  static constexpr uint64_t kMaxOutgoingBytes = 64u << 20;
  static constexpr uint32_t kMaxMonitors = 64;

  // `guid`: 32 hex digits (GetId, the authentication's OK); `machine_id`: 32 hex digits (Peer.GetMachineId).
  Bus(allocators::IAllocator& allocator, BusKind kind, StringView guid, StringView machine_id);
  ~Bus();
  Bus(const Bus&) = delete;
  Bus& operator=(const Bus&) = delete;

  BusKind kind() const { return kind_; }
  StringView guid() const { return StringView(guid_); }

  // --- the transport's side. A connection is a number (0: refused). `uid`: from the transport's credentials (~0 unknown); `can_pass_fds`: the transport
  // carries descriptors for it.
  uint32_t open_client(uint64_t pid, uint32_t uid, bool can_pass_fds);
  // Bytes from the connection, and the descriptors that came with them (taken from `fds`; may be null).
  void receive(uint32_t client, ConstByteView bytes, Queue<FdRef>* fds);
  // A descriptor that goes with the message starting at byte `at` of take_output()'s `out`.
  struct OutFd {
    uint64_t at = 0;
    FdRef fd;
  };
  // Appends what the connection is owed (bytes, and the descriptors that go with them). True when the bus is done with it: close it once that is sent.
  bool take_output(uint32_t client, ByteBuffer* out, List<OutFd>* fds);
  uint64_t output_waiting(uint32_t client) const;
  void close_client(uint32_t client);
  // Work of its own (the local peers' timers); `now_us` is the transport's clock.
  void tick(uint64_t now_us);

  // --- the kernel's own connections
  uint32_t connect_local(LocalPeer& peer);
  void disconnect_local(uint32_t client);
  // RequestName / ReleaseName for a connection (a local one, or for a test).
  uint32_t request_name(uint32_t client, StringView name, uint32_t flags);
  uint32_t release_name(uint32_t client, StringView name);
  // Sends a message from a local connection: SENDER is set, a serial is given when it has none (returned; 0: not sent).
  uint32_t send(uint32_t client, Message& message);

  // --- for the transport, the driver and tests
  StringView unique_name(uint32_t client) const;
  uint32_t connections() const { return uint32_t(connections_.count()); }
  // The primary owner of a name (a unique name's own connection); 0 when none.
  uint32_t owner_of(StringView name) const;
  allocators::IAllocator& allocator() const { return allocator_; }

  struct Connection;
  struct NameEntry;

 private:
  friend class BusDriver;
  friend struct BusDriverCall;
  Connection* find(uint32_t client) const;
  void read_messages(Connection& c);
  void handle(Connection& from, Message& m);
  void route(Connection* from, Message& m);
  void deliver(Connection& to, const Message& m, const ByteBuffer& wire);
  bool wants(const Connection& c, const Message& m, const StringArgs* args, bool eavesdropping) const;
  void broadcast(const Message& m, const Connection* addressed, bool everyone);
  void to_monitors(const Message& m, const ByteBuffer& wire, const Connection* skip);
  void deliver_locals();
  void close(Connection& c);
  void drop(Connection& c);
  void drop_pending(Connection& c);
  void release_all_names(Connection& c, bool tell_loser);
  bool encode_wire(const Message& m);

  // the bus's own messages
  bool from_bus(Message* m, uint8_t type);
  void send_from_bus(Message& m);
  void signal_owner_changed(StringView name, StringView old_owner, StringView new_owner);
  void signal_to(Connection& c, const char* member, StringView name);
  void error_to(Connection& c, const Message& call, StringView error_name, StringView text);

  // names
  NameEntry* name_entry(StringView name) const;
  // Takes the client out of the name's queue (with the signals that go with it); true when nobody is left in it.
  bool remove_from_queue(NameEntry& entry, uint32_t client, bool tell_loser);
  void make_unique_name(Connection& c);

  allocators::IAllocator& allocator_;
  BusKind kind_;
  char guid_[33] = {};
  char machine_id_[33] = {};
  Hive<Connection> connections_;
  Dictionary<uint32_t, Connection*> by_id_;
  Hive<NameEntry> names_;
  Dictionary<const char*, NameEntry*> name_index_;
  Dictionary<uint64_t, uint32_t> pending_;          // (caller << 32 | serial) -> callee
  struct Local {
    uint32_t client;
    UniquePointer<Message> message;
  };
  Queue<Local> local_queue_;
  List<uint32_t> local_peers_;
  uint32_t next_id_ = 1;
  uint32_t serial_ = 0;
  uint32_t monitors_ = 0;
  uint32_t eavesdrop_rules_ = 0;      // match rules with eavesdrop='true', of every connection
  bool delivering_locals_ = false;
  ByteBuffer wire_;
  mutable String scratch_;
};

}  // namespace dbus
