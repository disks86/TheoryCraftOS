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
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/StringView.hpp"

namespace pipewire {

// A descriptor a protocol hands to a client: shared memory the server reads and writes in place (a memfd the client maps), or an eventfd the two wake each
// other with. The transport makes them (IDescriptorMaker) and passes them with the message they belong to (SCM_RIGHTS).
class Descriptor {
 public:
  // The memory's bytes (empty for an eventfd). For memory a client handed over (its memfd), the bytes the file had when it came: a client that grows
  // the file later is not seen growing.
  virtual ByteView memory() = 0;
  // An eventfd: adds one to its counter (wakes whoever waits on it).
  virtual bool signal() = 0;
  // An eventfd: the counter, read and reset (0 when nothing was signalled).
  virtual uint64_t take_signals() = 0;
  // A GPU image a client handed over (a dmabuf: on this system an export of /dev/dxg): memory() is the server's copy of its pixels, which
  // sync_from_gpu() fills from the image (false when the GPU still works on it, or it cannot be read).
  virtual bool gpu() const { return false; }
  virtual bool sync_from_gpu() { return false; }

 protected:
  Descriptor() = default;
  ~Descriptor() = default;      // (made with SharedPointer::make_as, which destroys the real type)
};
using Fd = SharedPointer<Descriptor>;

class IDescriptorMaker {
 public:
  // Shared memory of `size` bytes (zeroed), mapped for the server; empty when it cannot be made.
  virtual Fd make_memory(uint64_t size, StringView name) = 0;
  // An eventfd (non-blocking, counter 0).
  virtual Fd make_event() = 0;

 protected:
  IDescriptorMaker() = default;
  ~IDescriptorMaker() = default;
};

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
  // For protocols that pass descriptors: what take_output() gives, and the descriptors its messages carry, in order (the transport sends them with the
  // first of those bytes, so each arrives no later than its message).
  virtual bool take_output_with_fds(uint32_t client, ByteBuffer* out, List<Fd>* fds) {
    (void)fds;
    return take_output(client, out);
  }
  // For protocols that take descriptors: bytes that came from the connection and the descriptors that came with them (in the order they came; each
  // arrives no later than the bytes of its message). A protocol that takes none sees only the bytes.
  virtual void receive_with_fds(uint32_t client, ConstByteView bytes, List<Fd>& fds) {
    fds.clear();
    receive(client, bytes);
  }
  // The transport's maker of descriptors (none: the protocol offers nothing that needs them).
  virtual void set_descriptor_maker(IDescriptorMaker* maker) { (void)maker; }

 protected:
  IProtocolServer() = default;
  ~IProtocolServer() = default;
};

}  // namespace pipewire
