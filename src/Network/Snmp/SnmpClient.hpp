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
#include "Network/Snmp/SnmpMessage.hpp"
#include "Network/Snmp/SnmpUsm.hpp"

namespace network {

enum class SnmpState : uint8_t { Idle, Pending, Done, Failed };
enum class SnmpError : uint8_t {
  None,
  Timeout,   // no answer after the retries
  Agent,     // the agent answered with an error status (error_status() says which)
  Send,      // the request could not be sent
  Invalid,   // the request was malformed (too many bindings...)
  Security,  // version 3: unknown user, wrong password (authentication or decryption failed), or a security level the agent refuses
};

// An SNMP manager talking to one agent over UDP (port 161), one request at a time. Polled like the rest of the stack: start a
// request with get() / get_next() / get_bulk(), call poll() while INetworkStack::poll() runs, and read response() when the state is
// Done. A request is sent again after `timeout_ms` (default 1 s), `retries` times (default 2), before it Fails with Timeout.
// Answers from other addresses, with other request ids or that do not decode are ignored.
class SnmpClient {
 public:
  SnmpClient(INetworkStack& stack, allocators::IAllocator& allocator, Ipv4Address agent, const char* community = "public", uint8_t version = 1,
             uint16_t port = 161)
      : stack_(stack), allocator_(allocator), agent_(agent), port_(port), version_(version), packet_(allocator) {
    uint32_t n = 0;
    for (; community && community[n] && n < sizeof community_ - 1; ++n) community_[n] = community[n];
    community_[n] = 0;
    next_id_ = static_cast<int32_t>(stack.now_ms() & 0x3FFFFFFF) | 1;
  }

  // Switches to SNMPv3 with a user (RFC 3414). Passwords need at least 8 characters; with `auth` None the messages go in the clear,
  // with `priv` None they are only authenticated. The first request finds the agent's engine ID and clock itself.
  bool set_v3_user(const char* user, UsmAuth auth, const char* auth_password, UsmPriv priv, const char* priv_password);
  bool v3() const { return version_ == 3; }
  bool engine_known() const { return engine_known_; }
  uint32_t engine_boots() const { return boots_; }

  bool get(const Oid* oids, uint32_t count) { return start(SnmpPdu::Get, oids, count, 0, 0); }
  bool get_next(const Oid* oids, uint32_t count) { return start(SnmpPdu::GetNext, oids, count, 0, 0); }
  // SET (needs a community or user with write access).
  bool set(const Oid* oids, const SnmpValue* values, uint32_t count);
  // Version 2c only: the next `max_repetitions` entries after each of `oids`.
  bool get_bulk(const Oid* oids, uint32_t count, uint32_t max_repetitions) { return version_ == 1 && start(SnmpPdu::GetBulk, oids, count, 0, static_cast<int32_t>(max_repetitions)); }

  void poll();
  SnmpState state() const { return state_; }
  SnmpError error() const { return error_; }
  int32_t error_status() const { return response_.error_status; }  // SNMP error-status of the answer (0 = noError)
  const SnmpMessage& response() const { return response_; }
  void reset() { state_ = SnmpState::Idle; socket_ = SharedPointer<IUdpSocket>(); }
  void set_timing(uint32_t timeout_ms, uint32_t retries) {
    timeout_ms_ = timeout_ms;
    retries_ = retries;
  }

 private:
  bool start(SnmpPdu pdu, const Oid* oids, uint32_t count, int32_t a, int32_t b);
  bool transmit();
  bool send_discovery();
  bool send_v3_request();
  void handle_v3(const SnmpMessage& m);
  void handle_report(const SnmpMessage& m);
  void fail(SnmpError error) {
    state_ = SnmpState::Failed;
    error_ = error;
  }

  INetworkStack& stack_;
  allocators::IAllocator& allocator_;
  Ipv4Address agent_;
  uint16_t port_;
  uint8_t version_;
  char community_[32] = {};
  SharedPointer<IUdpSocket> socket_;
  ByteBuffer packet_;  // the encoded request, kept for resending
  SnmpMessage response_;
  int32_t next_id_ = 1, request_id_ = 0;
  uint32_t timeout_ms_ = 1000, retries_ = 2, attempts_ = 0;
  uint64_t sent_at_ms_ = 0;
  SnmpState state_ = SnmpState::Idle;
  SnmpError error_ = SnmpError::None;
  // version 3
  char user_[32] = {};
  UsmAuth auth_ = UsmAuth::None;
  UsmPriv priv_ = UsmPriv::None;
  char auth_password_[64] = {}, priv_password_[64] = {};
  bool engine_known_ = false, resynced_ = false;
  uint8_t engine_id_[32] = {};
  uint32_t engine_id_length_ = 0;
  uint32_t boots_ = 0, engine_time_ = 0;  // the agent's clock as last seen, at local time time_seen_ms_
  uint64_t time_seen_ms_ = 0;
  UsmKeys keys_;
  uint64_t salt_ = 1;
  bool discovering_ = false;
  SnmpMessage pending_;  // the request to send once the engine is known
  const SnmpValue* set_values_ = nullptr;
};

}  // namespace network
