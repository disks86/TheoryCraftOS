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

#include "Network/ServerNetwork.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/Move.hpp"

namespace network {

using filesystem::Status;

namespace {

// Opens the socket on `stack` and waits for the handshake.
Status open_socket(INetworkStack& stack, Ipv4Address address, uint16_t port, uint32_t timeout_ms, SharedPointer<ITcpSocket>* out) {
  SharedPointer<ITcpSocket> socket = stack.tcp_open();
  if (!socket || !socket->connect(address, port)) return Status::Network;
  const uint64_t deadline = stack.now_ms() + timeout_ms;
  while (!socket->is_connected()) {
    if (socket->failed() || stack.now_ms() > deadline) {
      socket->abort();
      return Status::Network;
    }
    stack.poll();
    kernel::sleep_us(100);
  }
  *out = util::move(socket);
  return Status::Ok;
}

}  // namespace

Status SingleStackNetwork::connect(StringView host, uint16_t port, uint32_t timeout_ms, SharedPointer<ITcpSocket>* socket, INetworkStack** stack) {
  Ipv4Address address;
  if (!parse_ipv4(host, &address) && !stack_.resolve(host, timeout_ms, &address)) return Status::Network;
  *stack = &stack_;
  return open_socket(stack_, address, port, timeout_ms, socket);
}

Status StackSetNetwork::connect(StringView host, uint16_t port, uint32_t timeout_ms, SharedPointer<ITcpSocket>* socket, INetworkStack** stack) {
  Ipv4Address address;
  uint32_t index = 0;
  if (parse_ipv4(host, &address)) {
    // The interface whose network holds the address, else the first one that is configured.
    bool found = false;
    for (uint32_t i = 0; i < stacks_.count(); ++i) {
      const IpConfiguration c = stacks_.at(i)->configuration();
      if (c.address.is_unspecified()) continue;
      if (!found) {
        index = i;
        found = true;
      }
      if ((c.address.value() & c.netmask.value()) == (address.value() & c.netmask.value())) {
        index = i;
        break;
      }
    }
    if (!found) return Status::Network;
  } else if (!stacks_.resolve(host, timeout_ms, &address, &index)) {
    return Status::Network;
  }
  *stack = stacks_.at(index);
  return open_socket(*stacks_.at(index), address, port, timeout_ms, socket);
}

}  // namespace network
