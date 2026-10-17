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

#include "Network/Snmp/SnmpClient.hpp"

namespace network {

bool SnmpClient::start(SnmpPdu pdu, const Oid* oids, uint32_t count, int32_t a, int32_t b) {
  if (state_ == SnmpState::Pending) return false;
  error_ = SnmpError::None;
  response_ = SnmpMessage();
  if (count == 0 || count > SnmpMessage::kMaxBinds) {
    state_ = SnmpState::Failed;
    error_ = SnmpError::Invalid;
    return false;
  }
  if (!socket_) socket_ = stack_.udp_open(0);
  SnmpMessage m;
  m.version = version_;
  for (uint32_t i = 0; community_[i]; ++i) m.community[i] = community_[i];
  m.pdu = pdu;
  m.request_id = request_id_ = next_id_;
  next_id_ = (next_id_ + 1) & 0x3FFFFFFF;
  m.error_status = a;
  m.error_index = b;
  m.count = count;
  for (uint32_t i = 0; i < count; ++i) m.binds[i].oid = oids[i];
  if (!socket_ || !encode_snmp(m, allocator_, &packet_)) {
    state_ = SnmpState::Failed;
    error_ = socket_ ? SnmpError::Invalid : SnmpError::Send;
    return false;
  }
  attempts_ = 0;
  state_ = SnmpState::Pending;
  if (!transmit()) {
    // (the next poll() tries again; an agent that cannot be reached at all times out)
  }
  return true;
}

bool SnmpClient::transmit() {
  sent_at_ms_ = stack_.now_ms();
  ++attempts_;
  return socket_->send_to(agent_, port_, packet_.view());
}

void SnmpClient::poll() {
  if (state_ != SnmpState::Pending) return;
  uint8_t buffer[1500];
  Ipv4Address from;
  uint16_t from_port;
  uint32_t length;
  while (socket_->receive_from(ByteView(buffer), &from, &from_port, &length)) {
    if (length > sizeof buffer || !(from == agent_)) continue;
    SnmpMessage m;
    if (!decode_snmp(ConstByteView(buffer, length), &m) || m.pdu != SnmpPdu::Response || m.request_id != request_id_) continue;
    response_ = m;
    if (m.error_status != 0) {
      state_ = SnmpState::Failed;
      error_ = SnmpError::Agent;
    } else {
      state_ = SnmpState::Done;
    }
    return;
  }
  if (stack_.now_ms() - sent_at_ms_ >= timeout_ms_) {
    if (attempts_ > retries_) {
      state_ = SnmpState::Failed;
      error_ = SnmpError::Timeout;
    } else {
      transmit();
    }
  }
}

}  // namespace network
