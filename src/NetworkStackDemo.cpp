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

#include "NetworkStackDemo.hpp"
#include "console.hpp"

using namespace network;

namespace {
void put_ip(Ipv4Address a) {
  for (int i = 0; i < 4; ++i) {
    if (i) console::puts(".");
    console::put_dec(a.octets[i]);
  }
}

void line(const char* text) { console::puts(text); }


constexpr uint32_t kHostTimeoutMs = 1500;
}  // namespace

bool NetworkStackDemo::start() {
  // Every interface asks for its address at the same moment; a silent network
  // does not hold up the others.
  stacks_.start_dhcp();
  const uint32_t configured = stacks_.wait_for_addresses(8000);
  line("  ");
  console::put_dec(configured);
  line(" of ");
  console::put_dec(stacks_.count());
  line(" interfaces got an address\n");

  bool ok = configured == stacks_.count();
  for (uint32_t i = 0; i < stacks_.count(); ++i)
    if (stacks_.at(i)->configuration().configured()) ok &= check_interface(i);
    else line("  (an interface has no DHCP server)\n");

  // One lookup across all networks: whichever answers first wins.
  Ipv4Address address;
  uint32_t via = 0;
  if (stacks_.count() > 1 && stacks_.resolve("localhost", 4000, &address, &via)) {
    line("  DNS localhost on all networks: ");
    put_ip(address);
    line(" (answered by interface ");
    console::put_dec(via);
    line(")\n");
  }
  return ok;
}

bool NetworkStackDemo::check_interface(uint32_t index) {
  NetworkStack& stack = *stacks_.at(index);
  const IpConfiguration c = stack.configuration();
  bool ok = true;

  line("  [");
  console::put_dec(index);
  line("] ");
  line(stack.interface().name());
  line(": address ");
  put_ip(c.address);
  line("/");
  put_ip(c.netmask);
  line(", gateway ");
  put_ip(c.gateway);
  line(", dns ");
  put_ip(c.dns);
  line(", lease ");
  console::put_dec(stack.dhcp().lease_seconds());
  line(" s\n");

  uint32_t rtt = 0;
  if (stack.ping(c.gateway, 2000, &rtt)) {
    line("      ping ");
    put_ip(c.gateway);
    line(": reply in ");
    console::put_dec(rtt);
    line(" ms\n");
  } else {
    line("      ping: no reply\n");
    ok = false;
  }

  // DNS through this interface's own resolver: a name the server answers itself, and one that does not exist.
  static const char* const kNames[] = {"localhost", "no-such-host.invalid"};
  for (const char* name : kNames) {
    SharedPointer<IDnsQuery> query = stack.dns().query(name);
    const uint64_t deadline = stack.now_ms() + 4000;
    while (query && query->status() == DnsStatus::Pending && stack.now_ms() < deadline) stacks_.poll();
    line("      DNS ");
    line(name);
    line(": ");
    if (query && query->status() == DnsStatus::Resolved) {
      put_ip(query->address());
      line(" (ttl ");
      console::put_dec(query->ttl_seconds());
      line(" s)");
    } else {
      line(query ? dns_status_name(query->status()) : "invalid name");
    }
    line("\n");
  }

  // UDP round trip to an echo server on the host (QEMU maps the gateway address to the host).
  {
    SharedPointer<IUdpSocket> udp = stack.udp_open(0);
    static const char kMessage[] = "hello over UDP from TheoryCraftOS";
    uint8_t reply[128];
    uint32_t length = 0;
    bool answered = false;
    if (udp && udp->send_to(c.gateway, kHostUdpEchoPort, text_bytes(kMessage))) {
      const uint64_t deadline = stack.now_ms() + kHostTimeoutMs;
      while (!answered && stack.now_ms() < deadline) {
        stacks_.poll();
        answered = udp->receive_from(ByteView(reply), nullptr, nullptr, &length);
      }
    }
    if (answered && length <= sizeof(reply) && ConstByteView(reply).first(length).equals(text_bytes(kMessage))) line("      UDP echo from host: ok\n");
    else line("      UDP echo from host: no answer (no echo server on the host:7778 - optional)\n");
  }

  // TCP round trip to an echo server on the host.
  {
    SharedPointer<ITcpSocket> tcp = stack.tcp_open();
    static const char kMessage[] = "hello over TCP from TheoryCraftOS";
    bool done = false, sent = false;
    const bool tried = tcp && tcp->connect(c.gateway, kHostTcpEchoPort);
    uint8_t reply[128];
    uint32_t got = 0;
    const uint64_t deadline = stack.now_ms() + kHostTimeoutMs;
    while (tried && !done && stack.now_ms() < deadline) {
      stacks_.poll();
      if (tcp->state() == TcpState::Closed) break;  // refused
      if (tcp->is_connected() && !sent) sent = tcp->send(text_bytes(kMessage)) > 0;
      got += tcp->receive(ByteView(reply).dropped(got));
      done = got == sizeof(kMessage) - 1;
    }
    if (done && ConstByteView(reply).first(got).equals(text_bytes(kMessage))) line("      TCP echo from host: ok\n");
    else line("      TCP echo from host: no answer (no echo server on the host:7777 - optional)\n");
    if (tcp) tcp->close();
  }

  // A web server on this interface.
  UniquePointer<Endpoint> endpoint = UniquePointer<Endpoint>::make(allocator_, allocator_);
  if (endpoint) {
    endpoint->listener = stack.tcp_open();
    if (endpoint->listener && endpoint->listener->listen(kServerPort) && endpoints_.push_back(util::move(endpoint))) {
      line("      TCP server listening on port ");
      console::put_dec(kServerPort);
      line("\n");
      return ok;
    }
  }
  line("      TCP server: could not listen\n");
  return false;
}

void NetworkStackDemo::service() {
  if (!stacks_.service_running()) stacks_.poll();
  for (UniquePointer<Endpoint>& endpoint : endpoints_) serve(*endpoint);
}

void NetworkStackDemo::serve(Endpoint& endpoint) {
  while (SharedPointer<ITcpSocket> connection = endpoint.listener->accept()) endpoint.connections.push_back(connection);

  static const char kResponse[] =
      "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nHello from TheoryCraftOS!\n";
  for (uint64_t i = endpoint.connections.count(); i > 0; --i) {
    SharedPointer<ITcpSocket>& c = endpoint.connections[i - 1];
    uint8_t request[512];
    const uint32_t n = c->receive(ByteView(request));  // we only need to know that a request arrived
    if (n) {
      c->send(text_bytes(kResponse));
      c->close();  // the FIN follows the response
    } else if (c->state() == TcpState::CloseWait && c->unsent_or_unacknowledged() == 0) {
      c->close();  // the client left without asking for anything
    }
    // Once our side is shut the stack finishes the goodbye on its own; let go of it.
    if (c->state() == TcpState::Closed || c->state() == TcpState::TimeWait) endpoint.connections.remove_at(i - 1);
  }
}
