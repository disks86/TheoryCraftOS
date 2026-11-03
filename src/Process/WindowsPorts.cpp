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
#include "Process/WindowsPorts.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsObjects.hpp"
#include "Process/WindowsWait.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kTimeout = 0x102, kBufferOverflow = 0x80000005u, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du,
                   kInvalidInfoClass = 0xC0000003u, kInfoLengthMismatch = 0xC0000004u, kObjectTypeMismatch = 0xC0000024u, kNoMemory = 0xC0000017u, kNotImplemented = 0xC0000002u,
                   kInsufficientResources = 0xC000009Au, kPortDisconnected = 0xC0000037u, kPortConnectionRefused = 0xC0000041u, kPortMessageTooLong = 0xC000002Fu,
                   kInvalidPortHandle = 0xC0000042u, kInvalidCid = 0xC000000Bu, kObjectNameNotFound = 0xC0000034u, kInvalidParameter2 = 0xC00000F0u;
constexpr uint32_t kMaxPorts = 128, kMaxSlots = 64, kSlotBytes = 2048, kHeader = 0x28, kQueue = 32;
constexpr uint16_t kRequest = 1, kReply = 2, kDatagram = 3, kPortClosed = 5, kConnectionRequest = 10;
constexpr uint32_t kAlpcReplyMessage = 0x1, kAlpcSyncRequest = 0x20000;
constexpr uint32_t kDefaultMessage = 0x118;

enum Role : uint8_t { kConnection = 0, kServerCommunication = 1, kClientCommunication = 2 };
enum SlotState : uint8_t { kQueued = 0, kDelivered = 1, kAnswered = 2, kAccepted = 3, kCompleted = 4, kRefused = 5, kCancelled = 6 };

struct PortEntry {
  bool used = false, alpc = false, disconnected = false;
  uint8_t role = kConnection;
  uint32_t object = 0, max_message = kDefaultMessage;
  int16_t peer = -1, parent = -1, connection_slot = -1;
  uint64_t context = 0;
  int16_t queue[kQueue] = {};
  uint8_t head = 0, count = 0;
};
struct Slot {
  bool used = false, delivered = false, expects_reply = false;
  uint8_t state = kQueued;
  uint16_t length = 0;
  uint32_t message_id = 0;
  int16_t port = -1;                  // the port whose queue it is in (or was)
  int16_t connection_port = -1;       // a connection request: the port it asks
  uint32_t client_object = 0;         // a connection that was accepted: the client's communication port
  uint32_t server_object = 0;         // and the server's, until the server's handle is made
  uint64_t pid = 0, tid = 0;
  uint8_t data[kSlotBytes];
};
// All of it is kept with the system lock held (the lock of the object manager).
PortEntry g_ports[kMaxPorts];
Slot g_slots[kMaxSlots];
uint32_t g_next_message = 1;

struct SystemLock {
  ISystemSync* sync;
  SystemLock() : sync(system_objects() ? &system_objects()->sync() : nullptr) { if (sync) sync->lock(); }
  ~SystemLock() { if (sync) sync->unlock(); }
  SystemLock(const SystemLock&) = delete;
  SystemLock& operator=(const SystemLock&) = delete;
};

uint64_t me_thread(Process& p) { return p.runtime() ? p.runtime()->current_thread_id() : p.id(); }

uint64_t timeout_of(Process& p, uint64_t pointer, bool* fault) {
  constexpr uint64_t kUnixToNt = 11644473600ull;
  *fault = false;
  if (pointer == 0) return ~uint64_t{0};
  uint64_t raw = 0;
  if (!p.read_user_u64(pointer, &raw)) { *fault = true; return 0; }
  if (int64_t(raw) < 0) { const uint64_t units = uint64_t(0) - raw; return units > ~uint64_t{0} / 100 ? ~uint64_t{0} : units * 100; }
  const uint64_t now = p.host().realtime_ns() / 100 + kUnixToNt * 10000000ull;
  return raw > now ? (raw - now) * 100 : 0;
}

bool alive_locked(uint32_t object) {
  ObjectManager::ObjectState state;
  return system_objects()->object_state_locked(object, 0, &state) && state.kind == ObjectKind::Port;
}
// The entry of a port object (made when the port was); -1 if it is gone.
int find_port_locked(uint32_t object) {
  for (uint32_t i = 0; i < kMaxPorts; ++i) if (g_ports[i].used && g_ports[i].object == object) return alive_locked(object) ? int(i) : -1;
  return -1;
}
int new_port_locked(uint32_t object, Role role) {
  for (uint32_t i = 0; i < kMaxPorts; ++i) {
    if (g_ports[i].used && !alive_locked(g_ports[i].object)) g_ports[i] = PortEntry();      // (the ports whose handles are all closed make room)
    if (g_ports[i].used) continue;
    g_ports[i] = PortEntry();
    g_ports[i].used = true;
    g_ports[i].object = object;
    g_ports[i].role = role;
    return int(i);
  }
  return -1;
}
int new_slot_locked() {
  for (uint32_t i = 0; i < kMaxSlots; ++i) if (!g_slots[i].used) { g_slots[i] = Slot(); g_slots[i].used = true; return int(i); }
  return -1;
}
void free_slot_locked(int slot) { if (slot >= 0) g_slots[slot].used = false; }
bool queue_push_locked(int port, int slot) {
  PortEntry& e = g_ports[port];
  if (e.count >= kQueue) return false;
  e.queue[(e.head + e.count) % kQueue] = int16_t(slot);
  ++e.count;
  g_slots[slot].port = int16_t(port);
  return true;
}
int queue_pop_locked(int port) {
  PortEntry& e = g_ports[port];
  if (e.count == 0) return -1;
  const int slot = e.queue[e.head];
  e.head = uint8_t((e.head + 1) % kQueue);
  --e.count;
  return slot;
}
// The next message for a receiver of the port: its own queue, and for a connection port also those of the communication ports it made.
int next_message_locked(int port) {
  int slot = queue_pop_locked(port);
  if (slot >= 0 || g_ports[port].role != kConnection) return slot;
  for (uint32_t i = 0; i < kMaxPorts; ++i) if (g_ports[i].used && g_ports[i].parent == port && g_ports[i].role == kServerCommunication && (slot = queue_pop_locked(int(i))) >= 0) return slot;
  return -1;
}
// The port at the other end of a communication port: -1 when it has gone (its handles are closed, or it disconnected).
int peer_locked(int port) {
  const PortEntry& e = g_ports[port];
  if (e.peer < 0 || !g_ports[e.peer].used || !alive_locked(g_ports[e.peer].object) || g_ports[e.peer].disconnected) return -1;
  return e.peer;
}

// PORT_MESSAGE: DataLength (2), TotalLength (2), Type (2), DataInfoOffset (2), ClientId (16), MessageId (4), (pad 4), ClientViewSize / CallbackId (8).
uint16_t get16(const uint8_t* d, uint32_t at) { return uint16_t(d[at] | d[at + 1] << 8); }
void put16(uint8_t* d, uint32_t at, uint16_t v) { d[at] = uint8_t(v); d[at + 1] = uint8_t(v >> 8); }
void put32m(uint8_t* d, uint32_t at, uint32_t v) { for (int i = 0; i < 4; ++i) d[at + i] = uint8_t(v >> (8 * i)); }
void put64m(uint8_t* d, uint32_t at, uint64_t v) { for (int i = 0; i < 8; ++i) d[at + i] = uint8_t(v >> (8 * i)); }
uint32_t get32m(const uint8_t* d, uint32_t at) { return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24; }

// Reads the message at `address`: header and data (the data length in the header says how much); `limit` is the longest the port allows.
uint32_t read_message(Process& p, uint64_t address, uint32_t limit, uint8_t* out, uint16_t* length) {
  if (address == 0 || !p.read_user(address, ByteView(out, kHeader))) return kAccessViolation;
  const uint32_t data = get16(out, 0);
  if (kHeader + data > limit || kHeader + data > kSlotBytes) return kPortMessageTooLong;
  if (data != 0 && !p.read_user(address + kHeader, ByteView(out + kHeader, data))) return kAccessViolation;
  put16(out, 2, uint16_t(kHeader + data));
  *length = uint16_t(kHeader + data);
  return kSuccess;
}
uint32_t write_message(Process& p, uint64_t address, const uint8_t* data, uint16_t length) {
  if (address == 0) return kAccessViolation;
  return p.write_user(address, ConstByteView(data, length)) ? kSuccess : kAccessViolation;
}
// A message of the caller: stamps the type, the sender and the id on the copy.
void stamp(Slot& s, Process& p, uint16_t type, uint32_t id) {
  s.pid = p.id();
  s.tid = me_thread(p);
  s.message_id = id;
  put16(s.data, 4, type);
  put64m(s.data, 8, s.pid);
  put64m(s.data, 16, s.tid);
  put32m(s.data, 24, id);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- creating

namespace {

uint32_t make_connection_port(Process& p, uint64_t handle_at, uint64_t attributes, uint32_t max_message, bool alpc) {
  uint32_t object = 0;
  if (const uint32_t status = port_create_object(p, handle_at, attributes, &object)) return status;
  SystemLock lock;
  const int port = new_port_locked(object, kConnection);
  if (port < 0) return kInsufficientResources;
  g_ports[port].alpc = alpc;
  g_ports[port].max_message = max_message < kHeader + 8 ? kDefaultMessage : (max_message > kSlotBytes ? kSlotBytes : max_message);
  return kSuccess;
}

}  // namespace

uint32_t nt_create_port(Process& p, const SyscallRequest& r) {
  uint64_t max_message = 0;
  if (!argument(p, r, 3, &max_message)) return kAccessViolation;
  return make_connection_port(p, r.args[0], r.args[1], uint32_t(max_message), false);
}

uint32_t nt_alpc_create_port(Process& p, const SyscallRequest& r) {
  uint32_t max_message = 0;
  if (r.args[2] != 0) {      // ALPC_PORT_ATTRIBUTES: Flags, SecurityQos (12 bytes), then MaxMessageLength at 0x10
    uint8_t raw[8];
    if (!p.read_user(r.args[2] + 0x10, ByteView(raw, 8))) return kAccessViolation;
    max_message = get32m(raw, 0);
  }
  return make_connection_port(p, r.args[0], r.args[1], max_message ? max_message : kSlotBytes, true);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- connecting

namespace {

// The client side of a connection: a request for the server port to see, and the wait for its answer. `info` is the connection information (header and data) to the server;
// what the server answers with comes back in `reply_info`.
uint32_t connect(Process& p, uint32_t server_object, uint64_t handle_out, uint8_t* info, uint16_t info_length, uint8_t* reply, uint16_t* reply_length, uint64_t timeout, uint32_t* max_message) {
  ObjectManager* objects = system_objects();
  int slot = -1;
  {
    SystemLock lock;
    const int server = find_port_locked(server_object);
    if (server < 0 || g_ports[server].role != kConnection) return kPortConnectionRefused;
    slot = new_slot_locked();
    if (slot < 0) return kInsufficientResources;
    Slot& s = g_slots[slot];
    for (uint32_t i = 0; i < info_length; ++i) s.data[i] = info[i];
    s.length = info_length;
    s.connection_port = int16_t(server);
    stamp(s, p, kConnectionRequest, g_next_message++);
    if (max_message) *max_message = g_ports[server].max_message;
    if (!queue_push_locked(server, slot)) { free_slot_locked(slot); return kInsufficientResources; }
    objects->sync().notify_all();
  }
  uint8_t outcome = 0;
  uint32_t client_object = 0;
  uint32_t status = wait_until(p, timeout, [&]() -> uint64_t {
    Slot& s = g_slots[slot];
    if (s.state == kCompleted || s.state == kRefused) {
      outcome = s.state;
      client_object = s.client_object;
      const uint16_t n = s.length;
      for (uint32_t i = 0; i < n && i < kSlotBytes; ++i) reply[i] = s.data[i];
      *reply_length = n;
      return kSuccess;
    }
    const int server = s.connection_port;
    if (server < 0 || !g_ports[server].used || !alive_locked(g_ports[server].object)) return kPortConnectionRefused;      // the server port is gone
    return kWaitPending;
  });
  {
    SystemLock lock;
    if (status != kSuccess) {      // given up: a request nobody has taken is taken back
      Slot& s = g_slots[slot];
      if (s.state == kCompleted || s.state == kRefused) { outcome = s.state; client_object = s.client_object; status = kSuccess; }
      else {
        if (s.port >= 0 && !s.delivered) {      // still in a queue
          PortEntry& q = g_ports[s.port];
          for (uint32_t k = 0, n = q.count; k < n; ++k) {
            const int at = (q.head + k) % kQueue;
            if (q.queue[at] == slot) { for (uint32_t j = k; j + 1 < n; ++j) q.queue[(q.head + j) % kQueue] = q.queue[(q.head + j + 1) % kQueue]; --q.count; break; }
          }
        }
        s.state = kCancelled;
      }
    }
    free_slot_locked(slot);
  }
  if (status != kSuccess) return status == kTimeout ? kTimeout : status;
  if (outcome == kRefused) { if (client_object) objects->release(client_object); return kPortConnectionRefused; }
  const uint32_t given = port_give_handle(p, client_object, handle_out);
  return given;
}

}  // namespace

uint32_t nt_connect_port(Process& p, const SyscallRequest& r, bool secure) {
  // NtConnectPort(PHANDLE, PUNICODE_STRING, QoS, ClientView, ServerView, PULONG MaxMessageLength, PVOID Info, PULONG InfoLength); the secure form has the SID before ServerView.
  const uint32_t base = secure ? 5 : 4;
  uint64_t server_view = 0, max_length_at = 0, info_at = 0, info_length_at = 0;
  if (!argument(p, r, base, &server_view) || !argument(p, r, base + 1, &max_length_at) || !argument(p, r, base + 2, &info_at) || !argument(p, r, base + 3, &info_length_at)) return kAccessViolation;
  (void)server_view;
  if (r.args[0] == 0 || r.args[1] == 0) return kAccessViolation;
  if (!p.write_user_u64(r.args[0], 0)) return kAccessViolation;
  uint8_t info[kSlotBytes];
  for (uint32_t i = 0; i < kHeader; ++i) info[i] = 0;
  uint32_t info_length = 0;
  if (info_length_at != 0) {
    uint8_t raw[4];
    if (!p.read_user(info_length_at, ByteView(raw, 4))) return kAccessViolation;
    info_length = get32m(raw, 0);
    if (info_at == 0) info_length = 0;
    if (info_length > 0x400 || kHeader + info_length > kSlotBytes) return kPortMessageTooLong;
    if (info_length != 0 && !p.read_user(info_at, ByteView(info + kHeader, info_length))) return kAccessViolation;
  }
  put16(info, 0, uint16_t(info_length));
  put16(info, 2, uint16_t(kHeader + info_length));
  uint32_t server = 0;
  if (const uint32_t status = port_find_by_name(p, r.args[1], &server)) return status == kObjectNameNotFound ? status : status;
  uint8_t reply[kSlotBytes];
  uint16_t reply_length = 0;
  uint32_t max_message = 0;
  const uint32_t status = connect(p, server, r.args[0], info, uint16_t(kHeader + info_length), reply, &reply_length, ~uint64_t{0}, &max_message);
  system_objects()->release(server);
  if (info_length_at != 0 && reply_length >= kHeader) {      // what the server answered with replaces the connection information
    const uint32_t data = reply_length - kHeader;
    const uint32_t room = info_length;
    const uint32_t n = data < room ? data : room;
    if (n != 0 && info_at != 0) p.write_user(info_at, ConstByteView(reply + kHeader, n));
    p.write_user_u32(info_length_at, n);
  }
  if (status == kSuccess && max_length_at != 0) p.write_user_u32(max_length_at, max_message);
  return status;
}

uint32_t nt_alpc_connect_port(Process& p, const SyscallRequest& r) {
  uint64_t flags = 0, sid = 0, message_at = 0, length_at = 0, out_attr = 0, in_attr = 0, timeout_at = 0;
  if (!argument(p, r, 4, &flags) || !argument(p, r, 5, &sid) || !argument(p, r, 6, &message_at) || !argument(p, r, 7, &length_at) || !argument(p, r, 8, &out_attr) ||
      !argument(p, r, 9, &in_attr) || !argument(p, r, 10, &timeout_at)) return kAccessViolation;
  (void)flags; (void)sid; (void)out_attr; (void)in_attr;
  if (r.args[0] == 0 || r.args[1] == 0) return kAccessViolation;
  if (!p.write_user_u64(r.args[0], 0)) return kAccessViolation;
  bool fault = false;
  const uint64_t timeout = timeout_of(p, timeout_at, &fault);
  if (fault) return kAccessViolation;
  uint8_t info[kSlotBytes];
  uint16_t info_length = kHeader;
  for (uint32_t i = 0; i < kHeader; ++i) info[i] = 0;
  uint32_t buffer_length = 0;
  if (message_at != 0) {
    if (length_at != 0) { uint8_t raw[4]; if (!p.read_user(length_at, ByteView(raw, 4))) return kAccessViolation; buffer_length = get32m(raw, 0); }
    if (const uint32_t status = read_message(p, message_at, kSlotBytes, info, &info_length)) return status;
  } else {
    put16(info, 2, kHeader);
  }
  uint32_t server = 0;
  if (const uint32_t status = port_find_by_name(p, r.args[1], &server)) return status;
  uint8_t reply[kSlotBytes];
  uint16_t reply_length = 0;
  const uint32_t status = connect(p, server, r.args[0], info, info_length, reply, &reply_length, timeout, nullptr);
  system_objects()->release(server);
  if (message_at != 0 && reply_length >= kHeader && (status == kSuccess || status == kPortConnectionRefused)) {
    if (buffer_length == 0 || reply_length <= buffer_length) write_message(p, message_at, reply, reply_length);
    if (length_at != 0) p.write_user_u32(length_at, reply_length);
  }
  return status;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- listening, accepting

namespace {

// Takes the next message of the port out of its queue, waiting for one (the time given). Copies it into `out`; the connection requests stay known until they are answered.
uint32_t receive(Process& p, uint32_t port_object, uint64_t timeout, uint8_t* out, uint16_t* length, uint64_t* context, bool connection_requests_only) {
  return wait_until(p, timeout, [&]() -> uint64_t {
    const int port = find_port_locked(port_object);
    if (port < 0) return kInvalidPortHandle;
    for (;;) {
      int slot = -1;
      if (connection_requests_only) {      // (NtListenPort: only connection requests count; others stay in the queue)
        PortEntry& e = g_ports[port];
        for (uint32_t k = 0; k < e.count && slot < 0; ++k) {
          const int at = (e.head + k) % kQueue;
          if (g_slots[e.queue[at]].data[4] == kConnectionRequest) {
            slot = e.queue[at];
            for (uint32_t j = k; j + 1 < e.count; ++j) e.queue[(e.head + j) % kQueue] = e.queue[(e.head + j + 1) % kQueue];
            --e.count;
          }
        }
      } else slot = next_message_locked(port);
      if (slot < 0) break;
      Slot& s = g_slots[slot];
      if (s.state == kCancelled) { free_slot_locked(slot); continue; }
      for (uint32_t i = 0; i < s.length; ++i) out[i] = s.data[i];
      *length = s.length;
      *context = s.port >= 0 ? g_ports[s.port].context : 0;
      s.delivered = true;
      const uint16_t type = get16(s.data, 4);
      if (type == kConnectionRequest) return kSuccess;      // the slot waits for Accept
      if (type == kRequest) { s.state = kDelivered; return kSuccess; }      // and the request for its reply
      free_slot_locked(slot);      // a datagram is over once it is taken
      return kSuccess;
    }
    if (g_ports[port].role != kConnection) {      // a communication port with nobody at the other end gets nothing more
      const PortEntry& e = g_ports[port];
      if (e.disconnected || peer_locked(port) < 0) return kPortDisconnected;
    }
    return kWaitPending;
  });
}

}  // namespace

uint32_t nt_listen_port(Process& p, const SyscallRequest& r) {
  uint32_t port = 0;
  if (const uint32_t status = port_object_of(p, r.args[0], &port)) return status;
  uint8_t message[kSlotBytes];
  uint16_t length = 0;
  uint64_t context = 0;
  const uint32_t status = receive(p, port, ~uint64_t{0}, message, &length, &context, true);
  if (status != kSuccess) return status;
  return write_message(p, r.args[1], message, length);
}

namespace {

// Finds the connection request the server means (its message id) among those it was given.
int find_request_locked(uint32_t message_id) {
  for (uint32_t i = 0; i < kMaxSlots; ++i) if (g_slots[i].used && g_slots[i].delivered && get16(g_slots[i].data, 4) == kConnectionRequest && g_slots[i].message_id == message_id && g_slots[i].state == kQueued) return int(i);
  return -1;
}

// Accepts or refuses the connection request that has this id: the answer goes back to the client as connection information (header and data in `answer`). When accepted,
// the two communication ports are made and the handle of the server's is written to handle_at; `complete` says whether the client is let go at once (ALPC) or by NtCompleteConnectPort.
uint32_t accept(Process& p, uint32_t message_id, const uint8_t* answer, uint16_t answer_length, bool accepting, uint64_t context, uint64_t handle_at, bool complete, bool alpc, uint32_t max_message) {
  ObjectManager* objects = system_objects();
  uint32_t server_comm = 0, client_comm = 0;
  if (accepting) {      // (made before the lock is taken: making an object takes it)
    if (const uint32_t status = port_make_anonymous(&server_comm)) return status;
    if (const uint32_t status = port_make_anonymous(&client_comm)) { objects->release(server_comm); return status; }
  }
  uint32_t status = kSuccess;
  {
    SystemLock lock;
    const int request = find_request_locked(message_id);
    if (request < 0) status = kInvalidCid;
    else {
      Slot& s = g_slots[request];
      for (uint32_t i = 0; i < answer_length; ++i) s.data[i] = answer[i];
      s.length = answer_length;
      if (!accepting) s.state = kRefused;
      else {
        const int server = new_port_locked(server_comm, kServerCommunication);
        const int client = new_port_locked(client_comm, kClientCommunication);
        if (server < 0 || client < 0) { if (server >= 0) g_ports[server] = PortEntry(); if (client >= 0) g_ports[client] = PortEntry(); status = kInsufficientResources; }
        else {
          const int parent = s.connection_port;
          g_ports[server].peer = int16_t(client); g_ports[client].peer = int16_t(server);
          g_ports[server].parent = int16_t(parent); g_ports[server].context = context; g_ports[server].alpc = alpc;
          g_ports[server].max_message = g_ports[client].max_message = parent >= 0 ? g_ports[parent].max_message : max_message;
          g_ports[client].alpc = alpc;
          g_ports[server].connection_slot = int16_t(request);
          s.client_object = client_comm;
          s.state = complete ? kCompleted : kAccepted;
        }
      }
      objects->sync().notify_all();
    }
  }
  if (status != kSuccess || !accepting) {
    if (accepting) { objects->release(server_comm); objects->release(client_comm); }
    return status;
  }
  const uint32_t given = port_give_handle(p, server_comm, handle_at);      // (the server's handle holds the reference the object was made with)
  return given;
}

}  // namespace

uint32_t nt_accept_connect_port(Process& p, const SyscallRequest& r) {
  uint64_t accepting = 0, server_view = 0, client_view = 0;
  if (!argument(p, r, 3, &accepting) || !argument(p, r, 4, &server_view) || !argument(p, r, 5, &client_view)) return kAccessViolation;
  (void)server_view; (void)client_view;
  if (r.args[0] == 0) return kAccessViolation;
  if (!p.write_user_u64(r.args[0], 0)) return kAccessViolation;
  uint8_t message[kSlotBytes];
  uint16_t length = 0;
  if (const uint32_t status = read_message(p, r.args[2], kSlotBytes, message, &length)) return status;
  return accept(p, get32m(message, 24), message, length, (accepting & 0xFF) != 0, r.args[1], r.args[0], false, false, kDefaultMessage);
}

uint32_t nt_alpc_accept_connect_port(Process& p, const SyscallRequest& r) {
  uint64_t port_context = 0, request_at = 0, attributes = 0, accepting = 0;
  if (!argument(p, r, 5, &port_context) || !argument(p, r, 6, &request_at) || !argument(p, r, 7, &attributes) || !argument(p, r, 8, &accepting)) return kAccessViolation;
  (void)attributes;
  if (r.args[0] == 0) return kAccessViolation;
  if (!p.write_user_u64(r.args[0], 0)) return kAccessViolation;
  uint32_t connection = 0;
  if (const uint32_t status = port_object_of(p, r.args[1], &connection)) return status;
  uint8_t message[kSlotBytes];
  uint16_t length = 0;
  if (const uint32_t status = read_message(p, request_at, kSlotBytes, message, &length)) return status;
  return accept(p, get32m(message, 24), message, length, (accepting & 0xFF) != 0, port_context, r.args[0], true, true, kSlotBytes);
}

uint32_t nt_complete_connect_port(Process& p, const SyscallRequest& r) {
  uint32_t object = 0;
  if (const uint32_t status = port_object_of(p, r.args[0], &object)) return status;
  SystemLock lock;
  const int port = find_port_locked(object);
  if (port < 0 || g_ports[port].role != kServerCommunication || g_ports[port].connection_slot < 0) return kInvalidPortHandle;
  Slot& s = g_slots[g_ports[port].connection_slot];
  if (s.used && s.state == kAccepted) { s.state = kCompleted; system_objects()->sync().notify_all(); }
  g_ports[port].connection_slot = -1;
  return kSuccess;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- messages

namespace {

// A message to the other end of the port: a request that waits for its reply (`wait`), or one that does not.
uint32_t send(Process& p, uint32_t object, uint64_t message_at, uint16_t type, bool wait, uint64_t timeout, uint8_t* reply, uint16_t* reply_length, bool reply_to_peer_request = false) {
  uint8_t message[kSlotBytes];
  uint16_t length = 0;
  uint32_t limit = kSlotBytes;
  {
    SystemLock lock;
    const int port = find_port_locked(object);
    if (port < 0) return kInvalidPortHandle;
    if (g_ports[port].role == kConnection) return kInvalidPortHandle;
    limit = g_ports[port].max_message;
  }
  if (const uint32_t status = read_message(p, message_at, limit, message, &length)) return status;
  (void)reply_to_peer_request;
  int slot = -1;
  uint32_t id = 0;
  {
    SystemLock lock;
    const int port = find_port_locked(object);
    if (port < 0) return kInvalidPortHandle;
    const int peer = peer_locked(port);
    if (peer < 0) return kPortDisconnected;
    slot = new_slot_locked();
    if (slot < 0) return kInsufficientResources;
    Slot& s = g_slots[slot];
    for (uint32_t i = 0; i < length; ++i) s.data[i] = message[i];
    s.length = length;
    id = g_next_message++;
    stamp(s, p, type, id);
    s.expects_reply = wait;
    // a server's reply to a client goes with the same id; requests of a client go to the server's communication port
    if (!queue_push_locked(peer, slot)) { free_slot_locked(slot); return kInsufficientResources; }
    system_objects()->sync().notify_all();
  }
  if (!wait) return kSuccess;
  uint32_t status = wait_until(p, timeout, [&]() -> uint64_t {
    Slot& s = g_slots[slot];
    if (s.state == kAnswered) {
      for (uint32_t i = 0; i < s.length; ++i) reply[i] = s.data[i];
      *reply_length = s.length;
      return kSuccess;
    }
    const int port = find_port_locked(object);
    if (port < 0) return kInvalidPortHandle;
    if (peer_locked(port) < 0 && s.state != kAnswered) return kPortDisconnected;
    return kWaitPending;
  });
  SystemLock lock;
  if (status != kSuccess && g_slots[slot].state == kAnswered) status = kSuccess;
  if (status != kSuccess) {      // given up: the request is taken back if the server has not got it
    Slot& s = g_slots[slot];
    if (!s.delivered && s.port >= 0) {
      PortEntry& q = g_ports[s.port];
      for (uint32_t k = 0, n = q.count; k < n; ++k) {
        if (q.queue[(q.head + k) % kQueue] == slot) { for (uint32_t j = k; j + 1 < n; ++j) q.queue[(q.head + j) % kQueue] = q.queue[(q.head + j + 1) % kQueue]; --q.count; break; }
      }
    } else s.state = kCancelled;
  }
  free_slot_locked(slot);
  return status;
}

// A reply to the request with this id (the id is in the message), from the port whose server got it.
uint32_t reply_to(Process& p, uint32_t object, const uint8_t* message, uint16_t length) {
  SystemLock lock;
  const int port = find_port_locked(object);
  if (port < 0) return kInvalidPortHandle;
  const uint32_t id = get32m(message, 24);
  for (uint32_t i = 0; i < kMaxSlots; ++i) {
    Slot& s = g_slots[i];
    if (!s.used || !s.delivered || s.state != kDelivered || s.message_id != id || !s.expects_reply) continue;
    for (uint32_t k = 0; k < length; ++k) s.data[k] = message[k];
    s.length = length;
    s.state = kAnswered;
    put16(s.data, 4, kReply);
    put64m(s.data, 8, p.id());
    put64m(s.data, 16, me_thread(p));
    system_objects()->sync().notify_all();
    return kSuccess;
  }
  (void)port;
  return kInvalidCid;      // nobody waits for it
}

}  // namespace

uint32_t nt_request_port(Process& p, const SyscallRequest& r) {
  uint32_t object = 0;
  if (const uint32_t status = port_object_of(p, r.args[0], &object)) return status;
  uint8_t unused[kSlotBytes];
  uint16_t unused_length = 0;
  return send(p, object, r.args[1], kDatagram, false, 0, unused, &unused_length);
}

uint32_t nt_request_wait_reply_port(Process& p, const SyscallRequest& r) {
  uint32_t object = 0;
  if (const uint32_t status = port_object_of(p, r.args[0], &object)) return status;
  uint8_t reply[kSlotBytes];
  uint16_t length = 0;
  const uint32_t status = send(p, object, r.args[1], kRequest, true, ~uint64_t{0}, reply, &length);
  if (status != kSuccess) return status;
  return write_message(p, r.args[2], reply, length);
}

uint32_t nt_reply_port(Process& p, const SyscallRequest& r) {
  uint32_t object = 0;
  if (const uint32_t status = port_object_of(p, r.args[0], &object)) return status;
  uint8_t message[kSlotBytes];
  uint16_t length = 0;
  if (const uint32_t status = read_message(p, r.args[1], kSlotBytes, message, &length)) return status;
  return reply_to(p, object, message, length);
}

uint32_t nt_reply_wait_receive_port(Process& p, const SyscallRequest& r, bool with_timeout) {
  uint64_t timeout_at = 0;
  if (with_timeout && !argument(p, r, 4, &timeout_at)) return kAccessViolation;
  uint32_t object = 0;
  if (const uint32_t status = port_object_of(p, r.args[0], &object)) return status;
  bool fault = false;
  const uint64_t timeout = timeout_of(p, timeout_at, &fault);
  if (fault) return kAccessViolation;
  if (r.args[2] != 0) {
    uint8_t message[kSlotBytes];
    uint16_t length = 0;
    if (const uint32_t status = read_message(p, r.args[2], kSlotBytes, message, &length)) return status;
    const uint32_t status = reply_to(p, object, message, length);
    if (status != kSuccess) return status;
  }
  uint8_t message[kSlotBytes];
  uint16_t length = 0;
  uint64_t context = 0;
  const uint32_t status = receive(p, object, timeout, message, &length, &context, false);
  if (status != kSuccess) return status;
  if (r.args[1] != 0 && !p.write_user_u64(r.args[1], context)) return kAccessViolation;
  return write_message(p, r.args[3], message, length);
}

uint32_t nt_reply_wait_reply_port(Process& p, const SyscallRequest& r) {
  uint32_t object = 0;
  if (const uint32_t status = port_object_of(p, r.args[0], &object)) return status;
  uint8_t message[kSlotBytes];
  uint16_t length = 0;
  if (const uint32_t status = read_message(p, r.args[1], kSlotBytes, message, &length)) return status;
  if (const uint32_t status = reply_to(p, object, message, length)) return status;
  uint64_t context = 0;
  const uint32_t status = receive(p, object, ~uint64_t{0}, message, &length, &context, false);
  if (status != kSuccess) return status;
  return write_message(p, r.args[1], message, length);
}

uint32_t nt_query_information_port(Process& p, const SyscallRequest& r) {
  uint64_t result_at = 0;
  if (!argument(p, r, 4, &result_at)) return kAccessViolation;
  uint32_t object = 0;
  if (const uint32_t status = port_object_of(p, r.args[0], &object)) return status;
  if (uint32_t(r.args[1]) != 0) return kInvalidInfoClass;      // PortBasicInformation (the only class)
  const uint32_t length = uint32_t(r.args[3]);
  if (length > 0x100) return kInfoLengthMismatch;
  uint8_t zero[0x100] = {};
  if (length != 0 && !p.write_user(r.args[2], ConstByteView(zero, length))) return kAccessViolation;
  if (result_at != 0 && !p.write_user_u32(result_at, length)) return kAccessViolation;
  return kSuccess;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- ALPC messages

uint32_t nt_alpc_send_wait_receive_port(Process& p, const SyscallRequest& r) {
  uint64_t receive_at = 0, length_at = 0, receive_attr = 0, timeout_at = 0;
  if (!argument(p, r, 4, &receive_at) || !argument(p, r, 5, &length_at) || !argument(p, r, 6, &receive_attr) || !argument(p, r, 7, &timeout_at)) return kAccessViolation;
  (void)receive_attr;
  uint32_t object = 0;
  if (const uint32_t status = port_object_of(p, r.args[0], &object)) return status;
  const uint32_t flags = uint32_t(r.args[1]);
  bool fault = false;
  const uint64_t timeout = timeout_of(p, timeout_at, &fault);
  if (fault) return kAccessViolation;
  uint32_t buffer_length = 0;
  if (length_at != 0) { uint8_t raw[4]; if (!p.read_user(length_at, ByteView(raw, 4))) return kAccessViolation; buffer_length = get32m(raw, 0); }
  uint8_t message[kSlotBytes];
  uint16_t length = 0;
  uint32_t status = kSuccess;
  bool received = false;
  if (r.args[2] != 0) {
    if (flags & kAlpcReplyMessage) {
      if (status = read_message(p, r.args[2], kSlotBytes, message, &length)) return status;
      if (status = reply_to(p, object, message, length)) return status;
    } else if ((flags & kAlpcSyncRequest) != 0 && receive_at != 0) {
      status = send(p, object, r.args[2], kRequest, true, timeout, message, &length);
      received = status == kSuccess;
      if (!received) return status;
    } else {
      uint8_t unused[kSlotBytes];
      uint16_t unused_length = 0;
      status = send(p, object, r.args[2], kRequest, false, 0, unused, &unused_length);
      if (status != kSuccess) return status;
    }
  }
  if (receive_at != 0 && !received) {
    uint64_t context = 0;
    status = receive(p, object, timeout, message, &length, &context, false);
    if (status != kSuccess) return status;
    received = true;
  }
  if (received) {
    if (buffer_length != 0 && length > buffer_length) {
      if (length_at != 0) p.write_user_u32(length_at, length);
      return kBufferOverflow;
    }
    if (const uint32_t w = write_message(p, receive_at, message, length)) return w;
    if (length_at != 0) p.write_user_u32(length_at, length);
  }
  return kSuccess;
}

uint32_t nt_alpc_disconnect_port(Process& p, const SyscallRequest& r) {
  uint32_t object = 0;
  if (const uint32_t status = port_object_of(p, r.args[0], &object)) return status;
  if (uint32_t(r.args[1]) != 0) return kInvalidParameter2;
  SystemLock lock;
  const int port = find_port_locked(object);
  if (port < 0) return kInvalidPortHandle;
  g_ports[port].disconnected = true;
  system_objects()->sync().notify_all();
  return kSuccess;
}

// ALPC_BASIC_INFORMATION (class 0): Flags, SequenceNo, PortContext.
uint32_t nt_alpc_query_information(Process& p, const SyscallRequest& r) {
  uint64_t result_at = 0;
  if (!argument(p, r, 4, &result_at)) return kAccessViolation;
  uint32_t object = 0;
  if (const uint32_t status = port_object_of(p, r.args[0], &object)) return status;
  if (uint32_t(r.args[1]) != 0) return kInvalidInfoClass;
  if (uint32_t(r.args[3]) != 16) return kInfoLengthMismatch;
  uint8_t out[16] = {};
  {
    SystemLock lock;
    const int port = find_port_locked(object);
    if (port < 0) return kInvalidPortHandle;
    put32m(out, 0, g_ports[port].role == kConnection ? 0x20000u : 0u);
    put32m(out, 4, uint32_t(port));
    put64m(out, 8, g_ports[port].context);
  }
  if (!p.write_user(r.args[2], ConstByteView(out, 16))) return kAccessViolation;
  if (result_at != 0 && !p.write_user_u32(result_at, 16)) return kAccessViolation;
  return kSuccess;
}

uint32_t nt_alpc_set_information(Process& p, const SyscallRequest& r) {
  uint32_t object = 0;
  if (const uint32_t status = port_object_of(p, r.args[0], &object)) return status;
  if (uint32_t(r.args[1]) > 3) return kInvalidInfoClass;
  return kSuccess;
}

uint32_t nt_alpc_validate_port(Process& p, const SyscallRequest& r) {
  uint32_t object = 0;
  return port_object_of(p, r.args[0], &object);
}

}  // namespace process::windowsabi
