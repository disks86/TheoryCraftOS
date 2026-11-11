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

// A protocol the PipeWire socket server (SocketServer.hpp) carries: bytes in from a connection, bytes out to it, and a tick for work that is not a reply
// (graph changes, flow control). All of it is called from the socket server's one thread, so an implementation needs no lock of its own.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace pipewire {

class IProtocolServer {
 public:
  // A new connection from process `pid` (0 if unknown); returns its handle, 0 to refuse it.
  virtual uint32_t open_client(uint64_t pid) = 0;
  // Bytes that came from the connection.
  virtual void receive(uint32_t client, ConstByteView bytes) = 0;
  // Appends what the connection is owed to `out`. True when the server is done with it (close it once that is sent).
  virtual bool take_output(uint32_t client, ByteBuffer* out) = 0;
  // The connection is gone.
  virtual void close_client(uint32_t client) = 0;
  // Work of its own; called every round and at least every tick_interval_us().
  virtual void tick() = 0;
  virtual uint64_t tick_interval_us() const = 0;

 protected:
  IProtocolServer() = default;
  ~IProtocolServer() = default;
};

}  // namespace pipewire
