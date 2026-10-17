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

#pragma once
#include "Network/INetworkStack.hpp"
#include "Network/Snmp/SnmpUsm.hpp"

namespace network {

// A notification an agent sent on its own: a version 1 Trap, a version 2c / 3 Trap (unconfirmed) or a version 2c Inform (which the
// manager must acknowledge - the receiver does that).
struct SnmpNotification {
  Ipv4Address source = kAnyAddress;
  uint8_t version = 0;          // 0 = v1, 1 = v2c, 3 = v3
  bool inform = false;
  char community[32] = {};      // v1 / v2c
  char user[32] = {};           // v3
  uint32_t uptime = 0;          // sysUpTime in hundredths of a second
  // What happened, as a v2 trap OID: snmpTraps.1 .. .6 for the generic traps (coldStart, warmStart, linkDown, linkUp,
  // authenticationFailure, egpNeighborLoss), enterprise.0.N for an enterprise-specific v1 trap, and snmpTrapOID.0's value for v2.
  Oid trap_oid;
  Oid enterprise;               // v1 only
  Ipv4Address agent_address = kAnyAddress;  // v1 only: the address in the PDU (the source may be a proxy)
  SnmpVarBind binds[SnmpMessage::kMaxBinds];  // the variable bindings of interest (sysUpTime.0 and snmpTrapOID.0 of v2 traps are removed)
  uint32_t count = 0;
  const SnmpVarBind* find(const Oid& oid) const {
    for (uint32_t i = 0; i < count; ++i)
      if (binds[i].oid == oid) return &binds[i];
    return nullptr;
  }
  // The first binding whose OID starts with `prefix` (a column of a table, say).
  const SnmpVarBind* find_prefix(const Oid& prefix) const {
    for (uint32_t i = 0; i < count; ++i)
      if (binds[i].oid.starts_with(prefix)) return &binds[i];
    return nullptr;
  }
};

// Listens for notifications on UDP port 162 and queues them (16 deep; the oldest is dropped when it is full). Polled with the
// stack. Informs are answered with a Response at once. By default any community is accepted; accept_community() narrows that to
// the ones named. Version 3 notifications are accepted from the users given to add_v3_user() (the sender's engine ID, which the
// keys depend on, is read from the message; SNMPv3 informs are not acknowledged).
class SnmpTrapReceiver {
 public:
  static constexpr uint32_t kQueue = 16;
  SnmpTrapReceiver(INetworkStack& stack, allocators::IAllocator& allocator, uint16_t port = 162) : stack_(stack), allocator_(allocator), port_(port) {}

  bool start();
  void stop() { socket_ = SharedPointer<IUdpSocket>(); }
  bool running() const { return static_cast<bool>(socket_); }
  void poll();
  // The oldest queued notification. False if there is none.
  bool next(SnmpNotification* out);
  uint32_t pending() const { return count_; }

  bool accept_community(const char* community);   // up to 4; with none given every community passes
  bool add_v3_user(const char* user, UsmAuth auth, const char* auth_password, UsmPriv priv, const char* priv_password);  // up to 4

  uint64_t received() const { return received_; }
  uint64_t rejected() const { return rejected_; }   // wrong community, unknown user, failed authentication, malformed
  uint64_t dropped() const { return dropped_; }     // queue overflows

 private:
  struct V3User {
    char name[32];
    UsmAuth auth;
    UsmPriv priv;
    char auth_password[64], priv_password[64];
  };
  struct CachedKeys {
    bool used = false;
    uint8_t engine[32];
    uint8_t engine_length;
    uint8_t user_index;
    UsmKeys keys;
  };
  const UsmKeys* keys_for(uint32_t user, const uint8_t* engine, uint32_t engine_length);
  void handle(Ipv4Address from, uint16_t port, ConstByteView data);
  void enqueue(const SnmpNotification& n);

  INetworkStack& stack_;
  allocators::IAllocator& allocator_;
  uint16_t port_;
  SharedPointer<IUdpSocket> socket_;
  char communities_[4][32] = {};
  uint32_t community_count_ = 0;
  V3User users_[4];
  uint32_t user_count_ = 0;
  CachedKeys cache_[4];
  uint32_t cache_next_ = 0;
  SnmpNotification queue_[kQueue];
  uint32_t head_ = 0, count_ = 0;
  uint64_t received_ = 0, rejected_ = 0, dropped_ = 0;
};

}  // namespace network
