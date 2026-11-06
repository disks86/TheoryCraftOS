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

using network::IpAddress;
using network::Ipv4Address;
using network::Ipv6Address;
using network::IpFamilies;

namespace {
SocketManager* g_sockets = nullptr;
constexpr uint32_t kSuccess = 0, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kConnectionRefused = 0xC0000236u,
                   kAddressAlreadyInUse = 0xC000020Au, kConnectionReset = 0xC0000050u, kNoSuchDevice = 0xC000000Eu, kInvalidConnection = 0xC0000140u,
                   kTooManyAddresses = 0xC0000209u, kInvalidAddress = 0xC0000141u;

// ::ffff:a.b.c.d (RFC 4291 2.5.5.2)
bool is_v4_mapped(const Ipv6Address& a) {
  for (int i = 0; i < 10; ++i)
    if (a.bytes[i] != 0) return false;
  return a.bytes[10] == 0xFF && a.bytes[11] == 0xFF;
}
}  // namespace

SocketManager* system_sockets() { return g_sockets; }
void set_system_sockets(SocketManager* manager) { g_sockets = manager; }

bool SocketManager::to_network(const Socket& s, const IpAddress& program, IpAddress* out) {
  if (!s.ipv6) {
    if (program.v6) return false;
    *out = program;
    return true;
  }
  if (!program.v6) return false;  // (an IPv6 socket is given sockaddr_in6 only; ws2_32 checks the family first)
  if (is_v4_mapped(program.ipv6)) {
    if (s.v6_only) return false;
    *out = IpAddress(Ipv4Address::make(program.ipv6.bytes[12], program.ipv6.bytes[13], program.ipv6.bytes[14], program.ipv6.bytes[15]));
    return true;
  }
  *out = program;
  return true;
}

IpAddress SocketManager::to_program(const Socket& s, const IpAddress& address) {
  if (!s.ipv6 || address.v6) return address;
  const uint32_t v = address.ipv4.value();
  return IpAddress(Ipv6Address::make(0, 0, 0, 0, 0, 0xFFFF, uint16_t(v >> 16), uint16_t(v)));
}

IpFamilies SocketManager::families_of(const Socket& s) {
  if (!s.ipv6) return IpFamilies::V4;
  return s.v6_only ? IpFamilies::V6 : IpFamilies::Both;
}

SocketManager::Created SocketManager::create(SocketProto proto, bool ipv6) {
  Created result;
  for (uint32_t i = 0; i < kCapacity; ++i) {
    Socket& s = sockets_[i];
    if (s.used) continue;
    s = Socket();
    s.used = true;
    s.references = 1;
    s.proto = proto;
    s.ipv6 = ipv6;
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

network::INetworkStack* SocketManager::stack_for(Socket& s, const IpAddress& destination) {
  if (s.stack) return s.stack;
  // The first interface with an address of the socket's kind is the default; one that is on the destination's own link or subnet wins over it.
  uint32_t index = 0;
  bool found = false;
  for (uint32_t i = 0; i < stacks_.count(); ++i) {
    network::INetworkStack* stack = stacks_.at(i);
    if (!stack) continue;
    const network::IpConfiguration c = stack->configuration();
    const network::Ipv6Configuration c6 = stack->configuration6();
    bool usable, near = false;
    if (destination.v6) {
      const Ipv6Address& d = destination.ipv6;
      if (d.is_link_local() || d.is_multicast()) usable = c6.link_local_ready;
      else usable = c6.global_ready && !c6.router.is_unspecified();
      near = usable && c6.global_ready && c6.prefix_length != 0 && d.in_prefix(c6.prefix, c6.prefix_length);
    } else if (!destination.is_unspecified()) {
      usable = !c.address.is_unspecified();
      near = usable && destination.ipv4.in_subnet(c.address, c.netmask);
    } else {
      // A socket that listens or sends without having said where yet: any interface with an address it can use.
      usable = (families_of(s) != IpFamilies::V6 && !c.address.is_unspecified()) || (s.ipv6 && c6.link_local_ready);
    }
    if (!usable) continue;
    if (!found) { index = i; found = true; }
    if (near) { index = i; break; }
  }
  if (found) s.stack = stacks_.at(index);
  // The loopback (127.0.0.0/8, ::1) needs no address of an interface: with none configured yet any stack carries it, as does a listener waiting for it.
  if (!s.stack && (destination.is_loopback() || destination.is_unspecified()))
    for (uint32_t i = 0; i < stacks_.count() && !s.stack; ++i) s.stack = stacks_.at(i);
  return s.stack;
}

uint32_t SocketManager::open_udp(Socket& s, network::INetworkStack& stack, uint16_t port) {
  s.udp = stack.udp_open_families(port, families_of(s));
  return s.udp ? kSuccess : kAddressAlreadyInUse;
}

uint32_t SocketManager::bind(uint32_t id, const IpAddress& address, uint16_t port) {
  // Binding to one of several local addresses is not offered: every interface's own address is used as whatever it is. The address must still be one the
  // socket's family can carry.
  Socket* s = at(id);
  if (!s) return kInvalidHandle;
  IpAddress local;
  if (!to_network(*s, address, &local)) return kInvalidAddress;
  if (s->bound) return kAddressAlreadyInUse;
  network::INetworkStack* stack = stack_for(*s, IpAddress());
  if (!stack) return kNoSuchDevice;
  if (s->proto == SocketProto::Udp) {
    if (const uint32_t status = open_udp(*s, *stack, port); status != kSuccess) return status;
    s->bound_port = s->udp->local_port();
  } else {
    s->bound_port = port;  // a TCP bind is only realised by the later listen() (this network stack has no separate "reserve a port" step)
  }
  s->bound = true;
  return kSuccess;
}

uint32_t SocketManager::connect(uint32_t id, const IpAddress& address, uint16_t port, bool* would_block) {
  *would_block = false;
  Socket* s = at(id);
  if (!s) return kInvalidHandle;
  IpAddress destination;
  if (!to_network(*s, address, &destination)) return kInvalidAddress;
  if (s->proto == SocketProto::Udp) {
    network::INetworkStack* stack = stack_for(*s, destination);
    if (!stack) return kNoSuchDevice;
    if (!s->udp)
      if (const uint32_t status = open_udp(*s, *stack, 0); status != kSuccess) return status;
    s->udp_connected = true;
    s->peer_address = destination;
    s->peer_port = port;
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
  if (!s->tcp || !s->tcp->connect_ip(destination, port)) { s->tcp.reset(); return kTooManyAddresses; }
  s->peer_address = destination;
  s->peer_port = port;
  *would_block = true;  // the handshake is not done yet; the caller polls is_connected()/failed() itself (there is no separate "wait for connect" service)
  return kSuccess;
}

uint32_t SocketManager::listen(uint32_t id, uint32_t backlog) {
  (void)backlog;  // the stack keeps up to 8 pending connections regardless of what is asked for
  Socket* s = at(id);
  if (!s || s->proto != SocketProto::Tcp) return s ? kInvalidParameter : kInvalidHandle;
  if (!s->tcp) {
    network::INetworkStack* stack = stack_for(*s, IpAddress());
    if (!stack) return kNoSuchDevice;
    s->tcp = stack->tcp_open();
    if (!s->tcp) return kAddressAlreadyInUse;
  }
  if (!s->tcp->listen_families(s->bound_port, families_of(*s))) return kAddressAlreadyInUse;
  s->listening = true;
  return kSuccess;
}

uint32_t SocketManager::accept(uint32_t id, uint32_t* out_id, IpAddress* peer_address, uint16_t* peer_port, bool* would_block) {
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
    child.ipv6 = s->ipv6;
    child.v6_only = s->v6_only;
    child.bound = true;
    child.stack = s->stack;
    child.tcp = util::move(accepted);
    child.peer_address = child.tcp->remote_ip();
    child.peer_port = child.tcp->remote_port();
    *out_id = i + 1;
    *peer_address = to_program(child, child.peer_address);
    *peer_port = child.peer_port;
    return kSuccess;
  }
  return 0xC000009Au;  // STATUS_INSUFFICIENT_RESOURCES: accepted on the network, but there is no descriptor slot free for it
}

uint32_t SocketManager::send(uint32_t id, ConstByteView data, const IpAddress* address, const uint16_t* port, uint32_t* sent, bool* would_block) {
  *sent = 0;
  *would_block = false;
  Socket* s = at(id);
  if (!s) return kInvalidHandle;
  if (s->proto == SocketProto::Udp) {
    IpAddress to = s->peer_address;
    uint16_t to_port = s->peer_port;
    if (address) {
      if (!to_network(*s, *address, &to)) return kInvalidAddress;
      to_port = *port;
    } else if (!s->udp_connected) {
      return kInvalidConnection;
    }
    network::INetworkStack* stack = stack_for(*s, to);
    if (!stack) return kNoSuchDevice;
    if (!s->udp)
      if (const uint32_t status = open_udp(*s, *stack, 0); status != kSuccess) return status;
    if (data.size() > (to.v6 ? 1452u : 1472u)) return kInvalidParameter;  // what fits one Ethernet frame without fragmenting (WSAEMSGSIZE is not told apart)
    if (!s->udp->send_to_ip(to, to_port, data)) { *would_block = true; return kSuccess; }  // the stack's own send queue is momentarily full: try again shortly
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

uint32_t SocketManager::receive(uint32_t id, ByteView buffer, uint32_t* got, IpAddress* from_address, uint16_t* from_port, bool* would_block, bool* peer_closed) {
  *got = 0;
  *would_block = false;
  if (peer_closed) *peer_closed = false;
  Socket* s = at(id);
  if (!s) return kInvalidHandle;
  if (s->proto == SocketProto::Udp) {
    if (!s->udp) { *would_block = true; return kSuccess; }  // nothing bound or connected yet: nothing can have arrived
    IpAddress sender;
    uint16_t sender_port = 0;
    uint32_t length = 0;
    if (!s->udp->receive_from_ip(buffer, &sender, &sender_port, &length)) { *would_block = true; return kSuccess; }
    *got = length < buffer.size() ? length : uint32_t(buffer.size());
    if (from_address) *from_address = to_program(*s, sender);
    if (from_port) *from_port = sender_port;
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
  if (from_address) *from_address = to_program(*s, s->peer_address);
  if (from_port) *from_port = s->peer_port;
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

uint32_t SocketManager::set_v6_only(uint32_t id, bool on) {
  Socket* s = at(id);
  if (!s) return kInvalidHandle;
  if (!s->ipv6) return kInvalidParameter;  // (WSAEINVAL, as for IPPROTO_IPV6 options on an AF_INET socket)
  if (s->bound || s->tcp || s->udp) return kInvalidParameter;  // too late: the versions it takes are fixed in the network stack already
  s->v6_only = on;
  return kSuccess;
}

bool SocketManager::v6_only(uint32_t id) const {
  const Socket* s = at(id);
  return s && s->ipv6 && s->v6_only;
}

bool SocketManager::is_ipv6(uint32_t id) const {
  const Socket* s = at(id);
  return s && s->ipv6;
}

bool SocketManager::local_name(uint32_t id, IpAddress* address, uint16_t* port) {
  const Socket* s = at(id);
  if (!s) return false;
  const bool has_peer = s->proto == SocketProto::Udp ? s->udp_connected : !!s->tcp && !s->listening;
  IpAddress local = s->ipv6 && (!has_peer || s->peer_address.v6) ? IpAddress(network::kIpv6Any) : IpAddress(network::kAnyAddress);
  if (s->stack && has_peer) {
    // The address this socket's packets leave from: the loopback for a loopback peer, else the interface's own of the peer's kind.
    const IpAddress& peer = s->peer_address;
    if (peer.is_loopback()) {
      local = peer;
    } else if (peer.v6) {
      const network::Ipv6Configuration c6 = s->stack->configuration6();
      local = IpAddress(peer.ipv6.is_link_local() || !c6.global_ready ? c6.link_local : c6.global);
    } else {
      local = IpAddress(s->stack->configuration().address);
    }
  } else if (s->stack && !s->ipv6) {
    local = IpAddress(s->stack->configuration().address);
  }
  *address = to_program(*s, local);
  const uint16_t host_port = s->proto == SocketProto::Udp ? (s->udp ? s->udp->local_port() : s->bound_port) : (s->tcp ? s->tcp->local_port() : s->bound_port);
  *port = host_port;
  return true;
}

bool SocketManager::interface_configuration(uint32_t index, network::IpConfiguration* out) const {
  if (index >= stacks_.count()) return false;
  network::INetworkStack* stack = stacks_.at(index);
  *out = stack ? stack->configuration() : network::IpConfiguration();
  return true;
}

bool SocketManager::interface_configuration6(uint32_t index, network::Ipv6Configuration* out) const {
  if (index >= stacks_.count()) return false;
  network::INetworkStack* stack = stacks_.at(index);
  *out = stack ? stack->configuration6() : network::Ipv6Configuration();
  return true;
}

bool SocketManager::has_ipv6_route() const {
  for (uint32_t i = 0; i < stacks_.count(); ++i) {
    const network::INetworkStack* stack = stacks_.at(i);
    if (!stack) continue;
    const network::Ipv6Configuration c6 = stack->configuration6();
    if (c6.global_ready && !c6.router.is_unspecified()) return true;
  }
  return false;
}

bool SocketManager::start_lookup(StringView name, bool ipv6, NameLookup* lookup) const {
  *lookup = NameLookup();
  for (uint32_t i = 0; i < stacks_.count() && lookup->count < NameLookup::kMaxQueries; ++i) {
    network::INetworkStack* stack = stacks_.at(i);
    if (!stack) continue;
    const network::IpConfiguration c = stack->configuration();
    const network::Ipv6Configuration c6 = stack->configuration6();
    const bool has_server = (c.configured() && !c.dns.is_unspecified()) || (c6.configured() && !c6.dns.is_unspecified());
    if (!has_server) continue;
    SharedPointer<network::IDnsQuery> query = ipv6 ? stack->dns().query_aaaa(name) : stack->dns().query(name);
    if (!query) return false;  // not a valid name (the same for every interface) or out of memory
    lookup->queries[lookup->count++] = util::move(query);
  }
  return lookup->count != 0;
}

SocketManager::LookupResult SocketManager::lookup_result(const NameLookup& lookup, IpAddress* address) const {
  bool pending = false;
  for (uint32_t i = 0; i < lookup.count; ++i) {
    const network::DnsStatus status = lookup.queries[i]->status();
    if (status == network::DnsStatus::Resolved) {
      *address = lookup.queries[i]->ip_address();
      return LookupResult::Found;
    }
    pending |= status == network::DnsStatus::Pending;
  }
  return pending ? LookupResult::Pending : LookupResult::NotFound;
}

bool SocketManager::peer_name(uint32_t id, IpAddress* address, uint16_t* port) {
  const Socket* s = at(id);
  if (!s) return false;
  if (s->proto == SocketProto::Udp ? !s->udp_connected : (!s->tcp || s->listening)) return false;
  *address = to_program(*s, s->peer_address);
  *port = s->peer_port;
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
