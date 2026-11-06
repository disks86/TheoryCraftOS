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
#include "Process/SocketAddress.hpp"
#include "Process/Sockets.hpp"
#include "Process/WindowsFiles.hpp"
#include "Utility/ByteText.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kNotImplemented = 0xC0000002u,
                   kNoResources = 0xC000009Au, kInfoLengthMismatch = 0xC0000004u, kInvalidInfoClass = 0xC0000003u;
constexpr uint32_t kStatusWouldBlock = 0xC0000103u;      // "not a directory" everywhere else; here, always on a Socket handle, it means WSAEWOULDBLOCK
constexpr uint32_t kSockStream = 1, kSockDgram = 2, kNotFound = 0xC0000225u, kBufferTooSmall = 0xC0000023u;

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
uint32_t socket_wait(Process& p, uint64_t id, Attempt&& attempt, bool always_block = false) {
  SocketManager* sockets = system_sockets();
  bool would_block = false;
  uint32_t status = attempt(&would_block);
  if (status != kSuccess || !would_block) return status;
  if (!always_block && sockets->is_non_blocking(id)) return kStatusWouldBlock;
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

// Reads the program's socket address (`length` bytes at `at`, a SOCKADDR_IN or SOCKADDR_IN6). NTSTATUS.
uint32_t read_address(Process& p, uint64_t at, uint64_t length, network::IpAddress* address, uint16_t* port) {
  if (at == 0) return kAccessViolation;
  if (length < kSockaddrInSize) return kInvalidParameter;
  uint8_t bytes[kSockaddrIn6Size] = {};
  const uint32_t n = length < kSockaddrIn6Size ? uint32_t(length) : kSockaddrIn6Size;
  if (!p.read_user(at, ByteView(bytes, n))) return kAccessViolation;
  return parse_socket_address(ConstByteView(bytes, n), address, port) ? kSuccess : kInvalidParameter;
}

// Writes a socket address for the program: into `capacity` bytes at `at`, its size to *length_at when that is given. NTSTATUS.
uint32_t write_address(Process& p, uint64_t at, uint32_t capacity, uint64_t length_at, const network::IpAddress& address, uint16_t port) {
  uint8_t bytes[kSockaddrIn6Size];
  const uint32_t n = write_socket_address(ByteView(bytes), address, port);
  if (n > capacity) return kBufferTooSmall;
  if (!p.write_user(at, ConstByteView(bytes, n))) return kAccessViolation;
  if (length_at != 0 && !p.write_user_u32(length_at, n)) return kAccessViolation;
  return kSuccess;
}

// A little-endian ULONG of the program's.
bool read_user_u32(Process& p, uint64_t at, uint32_t* out) {
  uint8_t in[4] = {};
  if (!p.read_user(at, ByteView(in, 4))) return false;
  *out = in[0] | (uint32_t{in[1]} << 8) | (uint32_t{in[2]} << 16) | (uint32_t{in[3]} << 24);
  return true;
}
// An operation's ULONG input (InLength must be 4). NTSTATUS.
uint32_t read_u32_in(Process& p, uint64_t at, uint32_t length, uint32_t* out) {
  if (length != 4) return kInfoLengthMismatch;
  return read_user_u32(p, at, out) ? kSuccess : kAccessViolation;
}

}  // namespace

void socket_token_add(uint64_t token) { if (SocketManager* sockets = system_sockets()) sockets->add_ref(uint32_t(token)); }
void socket_token_release(uint64_t token) { if (SocketManager* sockets = system_sockets()) sockets->release(uint32_t(token)); }

uint32_t nt_create_socket(Process& p, const SyscallRequest& r) {
  SocketManager* sockets = system_sockets();
  if (!sockets) return kNotImplemented;
  const uint32_t family = uint32_t(r.args[0]);
  if (family != 0 && family != kWinAfInet && family != kWinAfInet6) return kInvalidParameter;
  const uint32_t type = uint32_t(r.args[1]);
  if (type != kSockStream && type != kSockDgram) return kInvalidParameter;
  if (r.args[3] == 0) return kAccessViolation;
  const SocketManager::Created c = sockets->create(type == kSockStream ? SocketProto::Tcp : SocketProto::Udp, family == kWinAfInet6);
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
  network::IpAddress address;
  uint16_t port = 0;
  if (const uint32_t status = read_address(p, r.args[1], r.args[2], &address, &port); status != kSuccess) return status;
  return sockets->bind(uint32_t(id), address, port);
}

uint32_t nt_connect_socket(Process& p, const SyscallRequest& r) {
  uint64_t id = 0;
  SocketManager* sockets = require(p, r.args[0], &id);
  if (!sockets) return kInvalidHandle;
  network::IpAddress address;
  uint16_t port = 0;
  if (const uint32_t status = read_address(p, r.args[1], r.args[2], &address, &port); status != kSuccess) return status;
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
  // The peer's address goes to PeerAddress (capacity *PeerAddressLength, which becomes its size); checked before anything is accepted, so a too-small buffer
  // loses no connection.
  uint32_t peer_capacity = 0;
  if (r.args[2] != 0 && (r.args[3] == 0 || !read_user_u32(p, r.args[3], &peer_capacity))) return kAccessViolation;
  if (r.args[2] != 0 && peer_capacity < (sockets->is_ipv6(uint32_t(id)) ? kSockaddrIn6Size : kSockaddrInSize)) return kBufferTooSmall;
  uint32_t out_id = 0;
  network::IpAddress peer_address;
  uint16_t peer_port = 0;
  const uint32_t status = socket_wait(p, uint32_t(id), [&](bool* would_block) {
    return sockets->accept(uint32_t(id), &out_id, &peer_address, &peer_port, would_block);
  });
  if (status != kSuccess) return status;
  uint64_t handle = 0;
  const uint32_t given = give_socket_handle(p, out_id, &handle);
  if (given != kSuccess) return given;
  if (!p.write_user_u64(r.args[1], handle)) { p.close_descriptor(handle / 4 - 1); return kAccessViolation; }
  if (r.args[2] != 0) return write_address(p, r.args[2], peer_capacity, r.args[3], peer_address, peer_port);
  return kSuccess;
}

uint32_t nt_send_socket(Process& p, const SyscallRequest& r) {
  uint64_t to_at = 0, to_length = 0;
  if (!argument(p, r, 4, &to_at) || !argument(p, r, 5, &to_length)) return kAccessViolation;
  uint64_t id = 0;
  SocketManager* sockets = require(p, r.args[0], &id);
  if (!sockets) return kInvalidHandle;
  const uint32_t length = uint32_t(r.args[2]) < kMaxSocketIo_ ? uint32_t(r.args[2]) : kMaxSocketIo_;
  if (length != 0 && r.args[1] == 0) return kAccessViolation;
  uint8_t stack_buffer[kMaxSocketIo_];
  if (length != 0 && !p.read_user(r.args[1], ByteView(stack_buffer, length))) return kAccessViolation;
  network::IpAddress address;
  uint16_t port = 0;
  const bool has_address = to_at != 0;
  if (has_address)
    if (const uint32_t status = read_address(p, to_at, to_length, &address, &port); status != kSuccess) return status;
  uint32_t sent = 0;
  const uint32_t status = socket_wait(p, uint32_t(id), [&](bool* would_block) {
    return sockets->send(uint32_t(id), ConstByteView(stack_buffer, length), has_address ? &address : nullptr, has_address ? &port : nullptr, &sent, would_block);
  });
  if (status != kSuccess) return status;
  if (r.args[3] != 0 && !p.write_user_u32(r.args[3], sent)) return kAccessViolation;
  return kSuccess;
}

uint32_t nt_receive_socket(Process& p, const SyscallRequest& r) {
  uint64_t from_at = 0, from_length_at = 0, peer_closed_at = 0;
  if (!argument(p, r, 4, &from_at) || !argument(p, r, 5, &from_length_at) || !argument(p, r, 6, &peer_closed_at)) return kAccessViolation;
  uint64_t id = 0;
  SocketManager* sockets = require(p, r.args[0], &id);
  if (!sockets) return kInvalidHandle;
  const uint32_t capacity = uint32_t(r.args[2]) < kMaxSocketIo_ ? uint32_t(r.args[2]) : kMaxSocketIo_;
  if (capacity != 0 && r.args[1] == 0) return kAccessViolation;
  uint32_t from_capacity = 0;
  if (from_at != 0 && (from_length_at == 0 || !read_user_u32(p, from_length_at, &from_capacity))) return kAccessViolation;
  if (from_at != 0 && from_capacity < (sockets->is_ipv6(uint32_t(id)) ? kSockaddrIn6Size : kSockaddrInSize)) return kBufferTooSmall;  // (before a datagram is taken)
  uint8_t stack_buffer[kMaxSocketIo_];
  uint32_t got = 0;
  network::IpAddress from_address;
  uint16_t from_port = 0;
  bool peer_closed = false;
  const uint32_t status = socket_wait(p, uint32_t(id), [&](bool* would_block) {
    got = 0;
    return sockets->receive(uint32_t(id), ByteView(stack_buffer, capacity), &got, &from_address, &from_port, would_block, &peer_closed);
  });
  if (status != kSuccess) return status;
  if (got != 0 && !p.write_user(r.args[1], ConstByteView(stack_buffer, got))) return kAccessViolation;
  if (r.args[3] != 0 && !p.write_user_u32(r.args[3], got)) return kAccessViolation;
  if (from_at != 0 && !peer_closed)
    if (const uint32_t written = write_address(p, from_at, from_capacity, from_length_at, from_address, from_port); written != kSuccess) return written;
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
      uint32_t how = 0;
      if (const uint32_t status = read_u32_in(p, r.args[2], in_length, &how); status != kSuccess) return status;
      return sockets->shutdown(uint32_t(id), how);
    }
    case 1:        // SetNonBlocking
    case 6: {      // SetV6Only
      uint8_t in = 0;
      if (in_length != 1 || !p.read_user(r.args[2], ByteView(&in, 1))) return in_length != 1 ? kInfoLengthMismatch : kAccessViolation;
      if (operation == 6) return sockets->set_v6_only(uint32_t(id), in != 0);
      return sockets->set_non_blocking(uint32_t(id), in != 0) ? kSuccess : kInvalidHandle;
    }
    case 2:      // GetSockName
    case 3: {    // GetPeerName
      network::IpAddress address;
      uint16_t port = 0;
      const bool ok = operation == 2 ? sockets->local_name(uint32_t(id), &address, &port) : sockets->peer_name(uint32_t(id), &address, &port);
      if (!ok) return 0xC0000238u;      // STATUS_NOT_CONNECTED (GetPeerName of an unconnected socket; GetSockName only fails on a bad handle, already ruled out)
      const uint32_t written = write_address(p, out_at, out_length, 0, address, port);
      return written == kBufferTooSmall ? kInfoLengthMismatch : written;
    }
    case 4: {    // BytesReadable (FIONREAD)
      if (out_length < 4) return kInfoLengthMismatch;
      return p.write_user_u32(out_at, sockets->readable(uint32_t(id))) ? kSuccess : kAccessViolation;
    }
    case 5: {    // InterfaceConfiguration (In: ULONG index; Out: address, netmask, gateway, DNS server, each as a sockaddr_in carries it)
      uint32_t index = 0;
      if (const uint32_t status = read_u32_in(p, r.args[2], in_length, &index); status != kSuccess) return status;
      if (out_length < 16) return kInfoLengthMismatch;
      network::IpConfiguration c;
      if (!sockets->interface_configuration(index, &c)) return 0x8000001Au;      // STATUS_NO_MORE_ENTRIES
      const uint32_t values[4] = {c.address.value(), c.netmask.value(), c.gateway.value(), c.dns.value()};
      uint8_t out[16];
      ByteWriter w{ByteView(out)};
      for (uint32_t v : values) w.be32(v);
      return p.write_user(out_at, ConstByteView(out, 16)) ? kSuccess : kAccessViolation;
    }
    case 7: {    // GetSocketInfo (Out: ULONG family, ULONG type, BOOLEAN v6only, 3 bytes padding)
      if (out_length < 12) return kInfoLengthMismatch;
      uint8_t out[12] = {};
      ByteWriter w{ByteView(out)};
      w.le32(sockets->is_ipv6(uint32_t(id)) ? kWinAfInet6 : kWinAfInet);
      w.le32(sockets->is_tcp(uint32_t(id)) ? kSockStream : kSockDgram);
      w.u8(sockets->v6_only(uint32_t(id)) ? 1 : 0);
      return p.write_user(out_at, ConstByteView(out, 12)) ? kSuccess : kAccessViolation;
    }
    case 8: {    // InterfaceConfiguration6 (In: ULONG index; Out: link-local, global, router, DNS server (16 bytes each), UCHAR prefix length, UCHAR flags, 2 pad, ULONG MTU)
      uint32_t index = 0;
      if (const uint32_t status = read_u32_in(p, r.args[2], in_length, &index); status != kSuccess) return status;
      if (out_length < 72) return kInfoLengthMismatch;
      network::Ipv6Configuration c;
      if (!sockets->interface_configuration6(index, &c)) return 0x8000001Au;
      uint8_t out[72] = {};
      ByteWriter w{ByteView(out)};
      w.bytes(ConstByteView(c.link_local.bytes));
      w.bytes(ConstByteView(c.global.bytes));
      w.bytes(ConstByteView(c.router.bytes));
      w.bytes(ConstByteView(c.dns.bytes));
      w.u8(c.prefix_length);
      w.u8(uint8_t((c.link_local_ready ? 1 : 0) | (c.global_ready ? 2 : 0)));      // flags: 1 link-local ready, 2 global ready
      w.zeros(2);
      w.le32(c.mtu);
      return p.write_user(out_at, ConstByteView(out, 72)) ? kSuccess : kAccessViolation;
    }
    case 9: {    // ResolveName (In: ULONG family (AF_INET for A, AF_INET6 for AAAA), then the name's bytes; Out: the 4- or 16-byte address). Always waits for the answer.
      constexpr uint32_t kMaxName = 253;
      if (in_length < 5 || in_length > 4 + kMaxName) return kInfoLengthMismatch;
      uint8_t in[4 + kMaxName];
      if (!p.read_user(r.args[2], ByteView(in, in_length))) return kAccessViolation;
      ByteReader reader{ConstByteView(in, in_length)};
      const uint32_t family = reader.le32();
      if (family != kWinAfInet && family != kWinAfInet6) return kInvalidParameter;
      const ConstByteView name_bytes = reader.rest();
      const StringView name = bytes_as_text(name_bytes);      // (the resolver checks it is a valid name)
      const bool ipv6 = family == kWinAfInet6;
      if (out_length < (ipv6 ? 16u : 4u)) return kInfoLengthMismatch;
      SocketManager::NameLookup lookup;
      if (!sockets->start_lookup(name, ipv6, &lookup)) return kNotFound;
      network::IpAddress found;
      const uint64_t started = p.host().monotonic_ns();
      constexpr uint64_t kLookupLimitNs = 15000000000ull;      // (the resolver gives up by itself sooner: this is only a backstop)
      bool missing = false;
      const uint32_t status = socket_wait(p, id, [&](bool* would_block) {
        const SocketManager::LookupResult result = sockets->lookup_result(lookup, &found);
        missing = result == SocketManager::LookupResult::NotFound || (result == SocketManager::LookupResult::Pending && p.host().monotonic_ns() - started > kLookupLimitNs);
        *would_block = result == SocketManager::LookupResult::Pending && !missing;
        return kSuccess;
      }, true);
      if (status != kSuccess) return status;
      if (missing || found.v6 != ipv6) return kNotFound;
      if (ipv6) return p.write_user(out_at, ConstByteView(found.ipv6.bytes)) ? kSuccess : kAccessViolation;
      uint8_t v4[4];
      ByteWriter w{ByteView(v4)};
      w.be32(found.ipv4.value());
      return p.write_user(out_at, ConstByteView(v4)) ? kSuccess : kAccessViolation;
    }
    case 10: {   // HasIpv6Route (Out: BOOLEAN: some interface has a global IPv6 address and a default router)
      if (out_length < 1) return kInfoLengthMismatch;
      const uint8_t routed = sockets->has_ipv6_route() ? 1 : 0;
      return p.write_user(out_at, ConstByteView(&routed, 1)) ? kSuccess : kAccessViolation;
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
