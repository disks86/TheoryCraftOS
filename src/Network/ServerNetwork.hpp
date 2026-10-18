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

// How the network filesystems (SMB, NFS) reach a server: finds the host (by name or address), picks the network interface that can reach it and
// opens the TCP connection, waiting for it to complete.
#pragma once
#include "FileSystem/FileSystemTypes.hpp"
#include "Network/INetworkStack.hpp"
#include "Network/NetworkStackSet.hpp"

namespace network {

class IServerNetwork {
 public:
  // Opens a TCP connection to `host` (a name or a dotted-quad address) on `port`. *stack is the network stack the socket belongs to: the
  // one to poll while using it. Status::Network if the host cannot be found or reached within `timeout_ms`.
  virtual filesystem::Status connect(StringView host, uint16_t port, uint32_t timeout_ms, SharedPointer<ITcpSocket>* socket, INetworkStack** stack) = 0;

 protected:
  IServerNetwork() = default;
  ~IServerNetwork() = default;
};

// The machine's interfaces: a name is looked up on all of them at once, an address goes out of the interface whose network holds it (else
// the first one that has an address, for the gateway to forward).
class StackSetNetwork final : public IServerNetwork {
 public:
  explicit StackSetNetwork(NetworkStackSet& stacks) : stacks_(stacks) {}
  filesystem::Status connect(StringView host, uint16_t port, uint32_t timeout_ms, SharedPointer<ITcpSocket>* socket, INetworkStack** stack) override;

 private:
  NetworkStackSet& stacks_;
};

// One stack (for tests, or a machine with one interface).
class SingleStackNetwork final : public IServerNetwork {
 public:
  explicit SingleStackNetwork(INetworkStack& stack) : stack_(stack) {}
  filesystem::Status connect(StringView host, uint16_t port, uint32_t timeout_ms, SharedPointer<ITcpSocket>* socket, INetworkStack** stack) override;

 private:
  INetworkStack& stack_;
};

}  // namespace network
