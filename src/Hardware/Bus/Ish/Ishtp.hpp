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

// ISHTP, the bus on top of the ISH IPC (intel-ish-hid/ishtp of Linux): the host bus messages (HBM, to the bus address 0: start with the protocol version, enumerate the firmware clients,
// read each one's properties, connect, flow control, disconnect) and the messages of the clients. A message of a client is cut into pieces of at most 124 bytes, each with the header of
// the client pair; the last has msg_complete. The firmware sends a message to the host only for a flow control credit the host gave it (one per message), and the host sends to a client
// only when the firmware gave one. All traffic goes over the IPC (no DMA transfers). Polled: poll() reads what the firmware has sent; calls that wait for an answer poll themselves.
#pragma once
#include "Hardware/Bus/Ish/IshIpc.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

struct IshtpClientProperties {
  uint8_t address = 0;                 // the firmware address
  uint8_t guid[16] = {};               // as the firmware sends it (the first three fields little endian)
  uint8_t version = 0;
  uint8_t max_connections = 0;
  uint32_t max_message = 0;
};

// One connection of the host to a firmware client.
struct IshtpConnection {
  uint8_t fw_address = 0, host_address = 0;
  bool connected = false;
  uint32_t credits = 0;                // messages the firmware allows us to send
  bool granted = false;                // we gave the firmware a credit that it has not used
  uint32_t max_message = 0;
  explicit IshtpConnection(allocators::IAllocator& a) : assembly(a), messages(a) {}
  ByteBuffer assembly;                 // the message being put together
  List<UniquePointer<ByteBuffer>> messages;   // complete messages not taken yet
};

class IshtpBus {
 public:
  static constexpr uint32_t kMtu = ish_reg::kMaxMessage - 4;
  static constexpr uint32_t kMaxClients = 64, kMaxConnections = 4, kMaxQueued = 32;
  IshtpBus(allocators::IAllocator& allocator, IshIpc& ipc) : allocator_(allocator), ipc_(ipc), connections_(allocator) {}

  // Starts the bus: the version, the clients and their properties. False if the firmware does not answer or does not support the version.
  bool start();
  uint32_t client_count() const { return client_count_; }
  const IshtpClientProperties& client(uint32_t i) const { return clients_[i < kMaxClients ? i : 0]; }
  const IshtpClientProperties* find_client(const uint8_t guid[16]) const;
  // Connects to a firmware client and gives it the first receive credit. nullptr on a refusal or no answer.
  IshtpConnection* connect(const IshtpClientProperties& client);
  // Sends one message (at most the client's maximum), waiting for a credit if there is none. False when no credit comes or the link is down.
  bool send(IshtpConnection& connection, ConstByteView message);
  // Reads everything the firmware has sent (credits, message pieces, connection news).
  void poll();
  // The next complete message of the connection, if any.
  bool take(IshtpConnection& connection, ByteBuffer* out);
  // Whether the firmware reset since the last call (connections are gone and the bus has to be started again).
  bool take_reset();

 private:
  bool wait_flag(const bool* flag, uint32_t polls);
  bool send_hbm(ConstByteView body);
  void handle(const IshMessage& message);
  void handle_hbm(ConstByteView body);
  void handle_client_piece(uint8_t fw, uint8_t host, bool complete, ConstByteView payload);
  bool grant(IshtpConnection& connection);
  IshtpConnection* connection_for(uint8_t fw, uint8_t host);

  allocators::IAllocator& allocator_;
  IshIpc& ipc_;
  List<UniquePointer<IshtpConnection>> connections_;
  IshtpClientProperties clients_[kMaxClients];
  uint32_t client_count_ = 0;
  uint8_t map_[32] = {};
  // answers of the bus messages
  bool started_ = false, start_supported_ = false, enumerated_ = false, props_answered_ = false, props_ok_ = false;
  uint8_t props_for_ = 0;
  IshtpClientProperties props_;
  IshtpConnection* connecting_ = nullptr;
  bool connect_answered_ = false;
  uint8_t connect_status_ = 0;
  bool reset_ = false;
};

}  // namespace hardware
