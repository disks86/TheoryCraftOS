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

#include "Process/WindowsSockets.hpp"
#include "Process/Sockets.hpp"
#include "Process/WindowsFiles.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kNotImplemented = 0xC0000002u,
                   kNoResources = 0xC000009Au, kInfoLengthMismatch = 0xC0000004u, kInvalidInfoClass = 0xC0000003u;
constexpr uint32_t kStatusWouldBlock = 0xC0000103u;      // "not a directory" everywhere else; here, always on a Socket handle, it means WSAEWOULDBLOCK
constexpr uint32_t kAfInet = 2, kSockStream = 1, kSockDgram = 2;

bool slot_of(uint64_t handle, uint64_t* slot) {
  if (handle == 0 || handle % 4 != 0 || handle / 4 > Process::kMaxDescriptors) return false;
  *slot = handle / 4 - 1;
  return true;
}
constexpr uint32_t kMaxSocketIo_ = 16384;      // a send()/recv() for more than this just moves part of it, like a real socket already may do

SocketManager* require(Process& p, uint64_t handle, uint64_t* id) {
  SocketManager* sockets = system_sockets();
  if (!sockets) return nullptr;
  uint64_t slot = 0;
  if (!slot_of(handle, &slot) || p.files().kind(slot) != DescriptorKind::Socket) return nullptr;
  *id = p.files().token(slot);
  return sockets;
}

uint32_t give_socket_handle(Process& p, uint32_t id, uint64_t* handle) {
  const int64_t slot = p.files().open_token(DescriptorKind::Socket, id);
  if (slot < 0) { system_sockets()->release(id); return kNoResources; }
  p.files().set_handle_flags(uint64_t(slot), false, false);
  *handle = (uint64_t(slot) + 1) * 4;
  return kSuccess;
}

// Retries `attempt` (one of SocketManager's operations) until it is not "would block" any more, a blocking socket's handles stay busy here instead of returning to
// the program; a non-blocking one's returns WSAEWOULDBLOCK at once, like it would from the real AFD driver.
template <typename Attempt>
uint32_t socket_wait(Process& p, uint64_t id, Attempt&& attempt) {
  SocketManager* sockets = system_sockets();
  bool would_block = false;
  uint32_t status = attempt(&would_block);
  if (status != kSuccess || !would_block) return status;
  if (sockets->is_non_blocking(id)) return kStatusWouldBlock;
  IProcessRuntime* runtime = p.runtime();
  for (;;) {
    if (p.exited() || (runtime && runtime->interrupted())) return 0xC0000120u;      // STATUS_CANCELLED
    if (runtime) runtime->unlock();
    p.host().sleep_ns(2000000);      // 2 ms: a short, steady poll - the network stack's own service thread, not this wait, is what makes progress
    if (runtime) runtime->lock();
    would_block = false;
    status = attempt(&would_block);
    if (status != kSuccess || !would_block) return status;
  }
}

}  // namespace

void socket_token_add(uint64_t token) { if (SocketManager* sockets = system_sockets()) sockets->add_ref(uint32_t(token)); }
void socket_token_release(uint64_t token) { if (SocketManager* sockets = system_sockets()) sockets->release(uint32_t(token)); }

uint32_t nt_create_socket(Process& p, const SyscallRequest& r) {
  SocketManager* sockets = system_sockets();
  if (!sockets) return kNotImplemented;
  if (r.args[0] != 0 && uint32_t(r.args[0]) != kAfInet) return kInvalidParameter;
  const uint32_t type = uint32_t(r.args[1]);
  if (type != kSockStream && type != kSockDgram) return kInvalidParameter;
  if (r.args[3] == 0) return kAccessViolation;
  const SocketManager::Created c = sockets->create(type == kSockStream ? SocketProto::Tcp : SocketProto::Udp);
  if (c.id == 0) return c.status;
  uint64_t handle = 0;
  const uint32_t status = give_socket_handle(p, c.id, &handle);
  if (status != kSuccess) return status;
  if (!p.write_user_u64(r.args[3], handle)) { p.close_descriptor(handle / 4 - 1); return kAccessViolation; }
  return kSuccess;
}

uint32_t nt_bind_socket(Process& p, const SyscallRequest& r) {
  uint64_t id = 0;
  SocketManager* sockets = require(p, r.args[0], &id);
  if (!sockets) return kInvalidHandle;
  return sockets->bind(uint32_t(id), uint32_t(r.args[1]), uint16_t(r.args[2]));
}

uint32_t nt_connect_socket(Process& p, const SyscallRequest& r) {
  uint64_t id = 0;
  SocketManager* sockets = require(p, r.args[0], &id);
  if (!sockets) return kInvalidHandle;
  const uint32_t address = uint32_t(r.args[1]);
  const uint16_t port = uint16_t(r.args[2]);
  return socket_wait(p, uint32_t(id), [&](bool* would_block) { return sockets->connect(uint32_t(id), address, port, would_block); });
}

uint32_t nt_listen_socket(Process& p, const SyscallRequest& r) {
  uint64_t id = 0;
  SocketManager* sockets = require(p, r.args[0], &id);
  if (!sockets) return kInvalidHandle;
  return sockets->listen(uint32_t(id), uint32_t(r.args[1]));
}

uint32_t nt_accept_socket(Process& p, const SyscallRequest& r) {
  uint64_t id = 0;
  SocketManager* sockets = require(p, r.args[0], &id);
  if (!sockets) return kInvalidHandle;
  if (r.args[1] == 0) return kAccessViolation;
  uint32_t out_id = 0, peer_address = 0;
  uint16_t peer_port = 0;
  const uint32_t status = socket_wait(p, uint32_t(id), [&](bool* would_block) {
    return sockets->accept(uint32_t(id), &out_id, &peer_address, &peer_port, would_block);
  });
  if (status != kSuccess) return status;
  uint64_t handle = 0;
  const uint32_t given = give_socket_handle(p, out_id, &handle);
  if (given != kSuccess) return given;
  if (!p.write_user_u64(r.args[1], handle)) { p.close_descriptor(handle / 4 - 1); return kAccessViolation; }
  if (r.args[2] != 0 && !p.write_user_u32(r.args[2], peer_address)) return kAccessViolation;
  if (r.args[3] != 0 && !p.write_user_u32(r.args[3], peer_port)) return kAccessViolation;
  return kSuccess;
}

uint32_t nt_send_socket(Process& p, const SyscallRequest& r) {
  uint64_t address_arg = 0, port_arg = 0, has_address_arg = 0;
  if (!argument(p, r, 4, &address_arg) || !argument(p, r, 5, &port_arg) || !argument(p, r, 6, &has_address_arg)) return kAccessViolation;
  uint64_t id = 0;
  SocketManager* sockets = require(p, r.args[0], &id);
  if (!sockets) return kInvalidHandle;
  const uint32_t length = uint32_t(r.args[2]) < kMaxSocketIo_ ? uint32_t(r.args[2]) : kMaxSocketIo_;
  if (length != 0 && r.args[1] == 0) return kAccessViolation;
  uint8_t stack_buffer[kMaxSocketIo_];
  if (length != 0 && !p.read_user(r.args[1], ByteView(stack_buffer, length))) return kAccessViolation;
  const uint32_t address = uint32_t(address_arg);
  const uint16_t port = uint16_t(port_arg);
  const bool has_address = (has_address_arg & 0xFF) != 0;
  uint32_t sent = 0;
  const uint32_t status = socket_wait(p, uint32_t(id), [&](bool* would_block) {
    return sockets->send(uint32_t(id), ConstByteView(stack_buffer, length), has_address ? &address : nullptr, has_address ? &port : nullptr, &sent, would_block);
  });
  if (status != kSuccess) return status;
  if (r.args[3] != 0 && !p.write_user_u32(r.args[3], sent)) return kAccessViolation;
  return kSuccess;
}

uint32_t nt_receive_socket(Process& p, const SyscallRequest& r) {
  uint64_t from_address_at = 0, from_port_at = 0, peer_closed_at = 0;
  if (!argument(p, r, 4, &from_address_at) || !argument(p, r, 5, &from_port_at) || !argument(p, r, 6, &peer_closed_at)) return kAccessViolation;
  uint64_t id = 0;
  SocketManager* sockets = require(p, r.args[0], &id);
  if (!sockets) return kInvalidHandle;
  const uint32_t capacity = uint32_t(r.args[2]) < kMaxSocketIo_ ? uint32_t(r.args[2]) : kMaxSocketIo_;
  if (capacity != 0 && r.args[1] == 0) return kAccessViolation;
  uint8_t stack_buffer[kMaxSocketIo_];
  uint32_t got = 0, from_address = 0;
  uint16_t from_port = 0;
  bool peer_closed = false;
  const uint32_t status = socket_wait(p, uint32_t(id), [&](bool* would_block) {
    got = 0;
    return sockets->receive(uint32_t(id), ByteView(stack_buffer, capacity), &got, &from_address, &from_port, would_block, &peer_closed);
  });
  if (status != kSuccess) return status;
  if (got != 0 && !p.write_user(r.args[1], ConstByteView(stack_buffer, got))) return kAccessViolation;
  if (r.args[3] != 0 && !p.write_user_u32(r.args[3], got)) return kAccessViolation;
  if (from_address_at != 0 && !p.write_user_u32(from_address_at, from_address)) return kAccessViolation;
  if (from_port_at != 0 && !p.write_user_u32(from_port_at, from_port)) return kAccessViolation;
  if (peer_closed_at != 0) {
    const uint8_t flag = peer_closed ? 1 : 0;
    if (!p.write_user(peer_closed_at, ConstByteView(&flag, 1))) return kAccessViolation;
  }
  return kSuccess;
}

uint32_t nt_socket_control(Process& p, const SyscallRequest& r) {
  uint64_t out_at = 0, out_length_arg = 0;
  if (!argument(p, r, 4, &out_at) || !argument(p, r, 5, &out_length_arg)) return kAccessViolation;
  uint64_t id = 0;
  SocketManager* sockets = require(p, r.args[0], &id);
  if (!sockets) return kInvalidHandle;
  const uint32_t operation = uint32_t(r.args[1]), in_length = uint32_t(r.args[3]), out_length = uint32_t(out_length_arg);
  switch (operation) {
    case 0: {      // Shutdown
      uint8_t in[4] = {};
      if (in_length != 4 || !p.read_user(r.args[2], ByteView(in, 4))) return in_length != 4 ? kInfoLengthMismatch : kAccessViolation;
      return sockets->shutdown(uint32_t(id), in[0] | (uint32_t{in[1]} << 8) | (uint32_t{in[2]} << 16) | (uint32_t{in[3]} << 24));
    }
    case 1: {      // SetNonBlocking
      uint8_t in = 0;
      if (in_length != 1 || !p.read_user(r.args[2], ByteView(&in, 1))) return in_length != 1 ? kInfoLengthMismatch : kAccessViolation;
      return sockets->set_non_blocking(uint32_t(id), in != 0) ? kSuccess : kInvalidHandle;
    }
    case 2:      // GetSockName
    case 3: {    // GetPeerName
      if (out_length < 8) return kInfoLengthMismatch;
      uint32_t address = 0;
      uint16_t port = 0;
      const bool ok = operation == 2 ? sockets->local_name(uint32_t(id), &address, &port) : sockets->peer_name(uint32_t(id), &address, &port);
      if (!ok) return 0xC0000238u;      // STATUS_NOT_CONNECTED (GetPeerName of an unconnected socket; GetSockName only fails on a bad handle, already ruled out)
      // address and port are already network-byte-order values (Ipv4Address::value() and SocketManager's swap16()d port): dump them
      // big-endian, matching a raw sockaddr_in's bytes, not the host's little-endian integer layout.
      uint8_t out[8];
      for (int i = 0; i < 4; ++i) out[i] = uint8_t(address >> (8 * (3 - i)));
      out[4] = uint8_t(port >> 8); out[5] = uint8_t(port); out[6] = out[7] = 0;
      return p.write_user(out_at, ConstByteView(out, 8)) ? kSuccess : kAccessViolation;
    }
    case 4: {    // BytesReadable (FIONREAD)
      if (out_length < 4) return kInfoLengthMismatch;
      return p.write_user_u32(out_at, sockets->readable(uint32_t(id))) ? kSuccess : kAccessViolation;
    }
    default: return kInvalidInfoClass;
  }
}

uint32_t nt_select_sockets(Process& p, const SyscallRequest& r) {
  uint64_t except_count_at = 0, except_at = 0, timeout_at = 0, ready_count_at = 0;
  if (!argument(p, r, 4, &except_count_at) || !argument(p, r, 5, &except_at) || !argument(p, r, 6, &timeout_at) || !argument(p, r, 7, &ready_count_at))
    return kAccessViolation;
  SocketManager* sockets = system_sockets();
  if (!sockets) return kNotImplemented;
  constexpr uint32_t kMaxEach = 64;
  // Each count is read/write (like a real fd_set's own fd_count field): the caller's in-value is how many handles it put in, and on return it holds how many of
  // that same array are now ready - the only way the caller learns each array's own post-compaction size, since *ReadyCount is just the grand total of all three.
  const uint64_t read_count_at = r.args[0], write_count_at = r.args[2];
  auto read_count_of = [&](uint64_t at, uint32_t* out) { return at == 0 || p.read_user(at, ByteView(reinterpret_cast<uint8_t*>(out), 4)); };
  uint32_t read_count = 0, write_count = 0, except_count = 0;
  if (!read_count_of(read_count_at, &read_count) || !read_count_of(write_count_at, &write_count) || !read_count_of(except_count_at, &except_count))
    return kAccessViolation;
  if (read_count > kMaxEach || write_count > kMaxEach || except_count > kMaxEach) return kInvalidParameter;
  uint32_t read_handles[kMaxEach], write_handles[kMaxEach], except_handles[kMaxEach];
  if (read_count && (r.args[1] == 0 || !p.read_user(r.args[1], ByteView(reinterpret_cast<uint8_t*>(read_handles), read_count * 4)))) return kAccessViolation;
  if (write_count && (r.args[3] == 0 || !p.read_user(r.args[3], ByteView(reinterpret_cast<uint8_t*>(write_handles), write_count * 4)))) return kAccessViolation;
  if (except_count && (except_at == 0 || !p.read_user(except_at, ByteView(reinterpret_cast<uint8_t*>(except_handles), except_count * 4)))) return kAccessViolation;
  int64_t timeout_100ns = -1;      // forever
  if (timeout_at != 0) {
    uint64_t raw = 0;
    if (!p.read_user_u64(timeout_at, &raw)) return kAccessViolation;
    timeout_100ns = int64_t(raw);
  }
  auto id_of = [&](uint32_t handle, uint64_t* id) -> bool {
    uint64_t slot = 0;
    if (!slot_of(handle, &slot) || p.files().kind(slot) != DescriptorKind::Socket) return false;
    *id = p.files().token(slot);
    return true;
  };
  const uint64_t started = p.host().monotonic_ns();
  IProcessRuntime* runtime = p.runtime();
  for (;;) {
    uint32_t ready = 0, kept_read = 0, kept_write = 0, kept_except = 0;
    for (uint32_t i = 0; i < read_count; ++i) { uint64_t sid; if (id_of(read_handles[i], &sid) && sockets->ready_to_read(uint32_t(sid))) { read_handles[kept_read++] = read_handles[i]; ++ready; } }
    for (uint32_t i = 0; i < write_count; ++i) { uint64_t sid; if (id_of(write_handles[i], &sid) && sockets->ready_to_write(uint32_t(sid))) { write_handles[kept_write++] = write_handles[i]; ++ready; } }
    for (uint32_t i = 0; i < except_count; ++i) { uint64_t sid; if (id_of(except_handles[i], &sid) && sockets->has_error(uint32_t(sid))) { except_handles[kept_except++] = except_handles[i]; ++ready; } }
    if (ready > 0 || timeout_100ns == 0) {
      if (read_count && !p.write_user(r.args[1], ConstByteView(reinterpret_cast<uint8_t*>(read_handles), kept_read * 4))) return kAccessViolation;
      if (write_count && !p.write_user(r.args[3], ConstByteView(reinterpret_cast<uint8_t*>(write_handles), kept_write * 4))) return kAccessViolation;
      if (except_count && !p.write_user(except_at, ConstByteView(reinterpret_cast<uint8_t*>(except_handles), kept_except * 4))) return kAccessViolation;
      if (read_count_at != 0 && !p.write_user_u32(read_count_at, kept_read)) return kAccessViolation;
      if (write_count_at != 0 && !p.write_user_u32(write_count_at, kept_write)) return kAccessViolation;
      if (except_count_at != 0 && !p.write_user_u32(except_count_at, kept_except)) return kAccessViolation;
      return p.write_user_u32(ready_count_at, ready) ? kSuccess : kAccessViolation;
    }
    if (p.exited() || (runtime && runtime->interrupted())) return 0xC0000120u;
    if (timeout_100ns > 0 && uint64_t(timeout_100ns) * 100 <= p.host().monotonic_ns() - started) {
      if (read_count_at != 0 && !p.write_user_u32(read_count_at, 0)) return kAccessViolation;
      if (write_count_at != 0 && !p.write_user_u32(write_count_at, 0)) return kAccessViolation;
      if (except_count_at != 0 && !p.write_user_u32(except_count_at, 0)) return kAccessViolation;
      return p.write_user_u32(ready_count_at, 0) ? kSuccess : kAccessViolation;
    }
    if (runtime) runtime->unlock();
    p.host().sleep_ns(2000000);
    if (runtime) runtime->lock();
  }
}

}  // namespace process::windowsabi
