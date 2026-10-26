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

#include "Process/Sockets.hpp"
#include "Utility/Move.hpp"

namespace process {

using network::Ipv4Address;

namespace {
SocketManager* g_sockets = nullptr;
constexpr uint32_t kSuccess = 0, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kNotImplemented = 0xC0000002u, kConnectionRefused = 0xC0000236u,
                   kHostUnreachable = 0xC000023Du, kAddressAlreadyInUse = 0xC000020Au, kConnectionReset = 0xC0000050u, kNoSuchDevice = 0xC000000Eu,
                   kInvalidConnection = 0xC0000140u, kTooManyAddresses = 0xC0000209u;
uint16_t swap16(uint16_t v) { return uint16_t((v << 8) | (v >> 8)); }
}  // namespace

SocketManager* system_sockets() { return g_sockets; }
void set_system_sockets(SocketManager* manager) { g_sockets = manager; }

SocketManager::Created SocketManager::create(SocketProto proto) {
  Created result;
  for (uint32_t i = 0; i < kCapacity; ++i) {
    Socket& s = sockets_[i];
    if (s.used) continue;
    s = Socket();
    s.used = true;
    s.references = 1;
    s.proto = proto;
    result.id = i + 1;
    return result;
  }
  result.status = 0xC000009Au;  // STATUS_INSUFFICIENT_RESOURCES
  return result;
}

void SocketManager::add_ref(uint32_t id) {
  if (Socket* s = at(id)) ++s->references;
}

void SocketManager::release(uint32_t id) {
  Socket* s = at(id);
  if (!s || s->references == 0) return;
  if (--s->references == 0) {
    if (s->tcp) s->tcp->close();
    if (s->udp) s->udp->close();
    if (s->pending_accept) s->pending_accept->close();  // accepted off the network but never given to the program: let it go too
    *s = Socket();
  }
}

network::INetworkStack* SocketManager::stack_for(Socket& s, Ipv4Address destination) {
  if (s.stack) return s.stack;
  uint32_t index = 0;
  bool found = false;
  for (uint32_t i = 0; i < stacks_.count(); ++i) {
    network::INetworkStack* stack = stacks_.at(i);
    if (!stack) continue;
    const network::IpConfiguration c = stack->configuration();
    if (c.address.is_unspecified()) continue;
    if (!found) { index = i; found = true; }
    if (!destination.is_unspecified() && destination.in_subnet(c.address, c.netmask)) { index = i; break; }
  }
  if (found) s.stack = stacks_.at(index);
  return s.stack;
}

uint32_t SocketManager::bind(uint32_t id, uint32_t address, uint16_t port) {
  (void)address;  // binding to one of several local addresses is not offered; every interface's own address is used as whatever it is
  Socket* s = at(id);
  if (!s) return kInvalidHandle;
  if (s->bound) return kAddressAlreadyInUse;
  network::INetworkStack* stack = stack_for(*s, Ipv4Address());
  if (!stack) return kNoSuchDevice;
  const uint16_t host_port = swap16(port);
  if (s->proto == SocketProto::Udp) {
    s->udp = stack->udp_open(host_port);
    if (!s->udp) return kAddressAlreadyInUse;
    s->bound_port = s->udp->local_port();
  } else {
    s->bound_port = host_port;  // a TCP bind is only realised by the later listen() (this network stack has no separate "reserve a port" step)
  }
  s->bound = true;
  return kSuccess;
}

uint32_t SocketManager::connect(uint32_t id, uint32_t address, uint16_t port, bool* would_block) {
  *would_block = false;
  Socket* s = at(id);
  if (!s) return kInvalidHandle;
  const Ipv4Address destination = Ipv4Address::from_value(address);
  const uint16_t host_port = swap16(port);
  if (s->proto == SocketProto::Udp) {
    network::INetworkStack* stack = stack_for(*s, destination);
    if (!stack) return kNoSuchDevice;
    if (!s->udp) {
      s->udp = stack->udp_open(0);
      if (!s->udp) return kAddressAlreadyInUse;
    }
    s->udp_connected = true;
    s->peer_address = destination;
    s->peer_port = host_port;
    return kSuccess;
  }
  if (s->tcp) {
    // A connect already under way (this is socket_wait's retry loop calling back in, not just a program calling connect() twice): report how it is doing
    // instead of starting a second one, which would abandon the first mid-handshake every time this is called again.
    if (s->tcp->failed()) return kConnectionRefused;
    if (!s->tcp->is_connected()) *would_block = true;
    return kSuccess;
  }
  network::INetworkStack* stack = stack_for(*s, destination);
  if (!stack) return kNoSuchDevice;
  s->tcp = stack->tcp_open();
  if (!s->tcp || !s->tcp->connect(destination, host_port)) { s->tcp.reset(); return kTooManyAddresses; }
  s->peer_address = destination;
  s->peer_port = host_port;
  *would_block = true;  // the handshake is not done yet; the caller polls is_connected()/failed() itself (there is no separate "wait for connect" service)
  return kSuccess;
}

uint32_t SocketManager::listen(uint32_t id, uint32_t backlog) {
  (void)backlog;  // the stack keeps up to 8 pending connections regardless of what is asked for
  Socket* s = at(id);
  if (!s || s->proto != SocketProto::Tcp) return s ? kInvalidParameter : kInvalidHandle;
  if (!s->tcp) {
    network::INetworkStack* stack = stack_for(*s, Ipv4Address());
    if (!stack) return kNoSuchDevice;
    s->tcp = stack->tcp_open();
    if (!s->tcp) return kAddressAlreadyInUse;
  }
  if (!s->tcp->listen(s->bound_port)) return kAddressAlreadyInUse;
  s->listening = true;
  return kSuccess;
}

uint32_t SocketManager::accept(uint32_t id, uint32_t* out_id, uint32_t* peer_address, uint16_t* peer_port, bool* would_block) {
  *would_block = false;
  Socket* s = at(id);
  if (!s || !s->listening) return s ? kInvalidParameter : kInvalidHandle;
  SharedPointer<network::ITcpSocket> accepted = s->pending_accept ? util::move(s->pending_accept) : s->tcp->accept();
  if (!accepted) {
    *would_block = true;  // the caller (an NT service) loops calling this again, unless the socket is non-blocking: see socket_wait in WindowsSockets.cpp
    return kSuccess;
  }
  for (uint32_t i = 0; i < kCapacity; ++i) {
    Socket& child = sockets_[i];
    if (child.used) continue;
    child = Socket();
    child.used = true;
    child.references = 1;
    child.proto = SocketProto::Tcp;
    child.bound = true;
    child.stack = s->stack;
    child.tcp = util::move(accepted);
    child.peer_address = child.tcp->remote_address();
    child.peer_port = child.tcp->remote_port();
    *out_id = i + 1;
    *peer_address = child.peer_address.value();
    *peer_port = swap16(child.peer_port);
    return kSuccess;
  }
  return 0xC000009Au;  // STATUS_INSUFFICIENT_RESOURCES: accepted on the network, but there is no descriptor slot free for it
}

uint32_t SocketManager::send(uint32_t id, ConstByteView data, const uint32_t* address, const uint16_t* port, uint32_t* sent, bool* would_block) {
  *sent = 0;
  *would_block = false;
  Socket* s = at(id);
  if (!s) return kInvalidHandle;
  if (s->proto == SocketProto::Udp) {
    Ipv4Address to = s->peer_address;
    uint16_t to_port = s->peer_port;
    if (address) { to = Ipv4Address::from_value(*address); to_port = swap16(*port); }
    else if (!s->udp_connected) return kInvalidConnection;
    network::INetworkStack* stack = stack_for(*s, to);
    if (!stack) return kNoSuchDevice;
    if (!s->udp) {
      s->udp = stack->udp_open(0);
      if (!s->udp) return kAddressAlreadyInUse;
    }
    if (data.size() > 1472) return kInvalidParameter;
    if (!s->udp->send_to(to, to_port, data)) { *would_block = true; return kSuccess; }      // the stack's own send queue is momentarily full: try again shortly
    *sent = uint32_t(data.size());
    return kSuccess;
  }
  if (!s->tcp) return kInvalidConnection;
  if (s->tcp->failed()) return kConnectionReset;
  const uint32_t n = s->tcp->send(data);
  if (n == 0 && !data.empty()) { *would_block = true; return kSuccess; }  // the send buffer is full; the caller waits (unless non-blocking) and tries again
  *sent = n;
  return kSuccess;
}

uint32_t SocketManager::receive(uint32_t id, ByteView buffer, uint32_t* got, uint32_t* from_address, uint16_t* from_port, bool* would_block, bool* peer_closed) {
  *got = 0;
  *would_block = false;
  if (peer_closed) *peer_closed = false;
  Socket* s = at(id);
  if (!s) return kInvalidHandle;
  if (s->proto == SocketProto::Udp) {
    if (!s->udp) { *would_block = true; return kSuccess; }  // nothing bound or connected yet: nothing can have arrived
    Ipv4Address sender;
    uint16_t sender_port = 0;
    uint32_t length = 0;
    if (!s->udp->receive_from(buffer, &sender, &sender_port, &length)) { *would_block = true; return kSuccess; }
    *got = length < buffer.size() ? length : uint32_t(buffer.size());
    if (from_address) *from_address = sender.value();
    if (from_port) *from_port = swap16(sender_port);
    return kSuccess;
  }
  if (!s->tcp) return kInvalidConnection;
  if (s->tcp->failed()) return kConnectionReset;
  const uint32_t n = s->tcp->receive(buffer);
  if (n == 0) {
    if (s->tcp->peer_closed()) { if (peer_closed) *peer_closed = true; return kSuccess; }  // orderly close: recv() returns 0, like the real one
    *would_block = true;
    return kSuccess;
  }
  *got = n;
  if (from_address) *from_address = s->peer_address.value();
  if (from_port) *from_port = swap16(s->peer_port);
  return kSuccess;
}

uint32_t SocketManager::shutdown(uint32_t id, uint32_t how) {
  Socket* s = at(id);
  if (!s) return kInvalidHandle;
  if (s->proto != SocketProto::Tcp || !s->tcp) return kInvalidConnection;
  if (how == 1 || how == 2) s->tcp->close();  // SD_SEND / SD_BOTH: there is no separate "stop reading", so SD_RECEIVE alone does nothing extra
  return kSuccess;
}

bool SocketManager::set_non_blocking(uint32_t id, bool on) {
  Socket* s = at(id);
  if (!s) return false;
  s->non_blocking = on;
  return true;
}

bool SocketManager::is_non_blocking(uint32_t id) const {
  const Socket* s = at(id);
  return s && s->non_blocking;
}

bool SocketManager::local_name(uint32_t id, uint32_t* address, uint16_t* port) {
  const Socket* s = at(id);
  if (!s) return false;
  const network::IpConfiguration c = s->stack ? s->stack->configuration() : network::IpConfiguration();
  *address = c.address.value();
  const uint16_t host_port = s->proto == SocketProto::Udp ? (s->udp ? s->udp->local_port() : s->bound_port) : (s->tcp ? s->tcp->local_port() : s->bound_port);
  *port = swap16(host_port);
  return true;
}

bool SocketManager::peer_name(uint32_t id, uint32_t* address, uint16_t* port) {
  const Socket* s = at(id);
  if (!s) return false;
  if (s->proto == SocketProto::Udp ? !s->udp_connected : !s->tcp) return false;
  *address = s->peer_address.value();
  *port = swap16(s->peer_port);
  return true;
}

uint32_t SocketManager::readable(uint32_t id) const {
  const Socket* s = at(id);
  if (!s) return 0;
  if (s->proto == SocketProto::Udp) return s->udp ? s->udp->pending() : 0;
  return s->tcp ? s->tcp->available() : 0;
}

bool SocketManager::is_tcp(uint32_t id) const {
  const Socket* s = at(id);
  return s && s->proto == SocketProto::Tcp;
}

bool SocketManager::ready_to_read(uint32_t id) const {
  const Socket* s = at(id);
  if (!s) return false;
  if (s->proto == SocketProto::Udp) return s->udp && s->udp->pending() > 0;
  if (!s->tcp) return false;
  if (s->listening) {
    if (!s->pending_accept) s->pending_accept = s->tcp->accept();  // cached (see the Socket struct): never drops a connection that arrived
    return bool(s->pending_accept);
  }
  return s->tcp->available() > 0 || s->tcp->peer_closed() || s->tcp->failed();
}

bool SocketManager::ready_to_write(uint32_t id) const {
  const Socket* s = at(id);
  if (!s) return false;
  if (s->proto == SocketProto::Udp) return true;
  if (!s->tcp) return true;  // not connected yet: select() for a connecting socket resolves on failure or success either way
  return s->tcp->is_connected() || s->tcp->failed();
}

bool SocketManager::has_error(uint32_t id) const {
  const Socket* s = at(id);
  return s && s->proto == SocketProto::Tcp && s->tcp && s->tcp->failed();
}

}  // namespace process
