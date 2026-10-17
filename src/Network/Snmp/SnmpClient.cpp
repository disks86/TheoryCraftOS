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

namespace {
// usmStats counters an agent reports (RFC 3414): 1.3.6.1.6.3.15.1.1.N.0
bool is_usm_stat(const Oid& oid, uint32_t n) {
  static const uint32_t prefix[] = {1, 3, 6, 1, 6, 3, 15, 1, 1};
  if (oid.length != 11 || oid.arc[9] != n || oid.arc[10] != 0) return false;
  for (uint32_t i = 0; i < 9; ++i)
    if (oid.arc[i] != prefix[i]) return false;
  return true;
}
}  // namespace

bool SnmpClient::set_v3_user(const char* user, UsmAuth auth, const char* auth_password, UsmPriv priv, const char* priv_password) {
  if (state_ == SnmpState::Pending) return false;
  if (priv != UsmPriv::None && auth == UsmAuth::None) return false;
  auto copy = [](char* dst, uint32_t capacity, const char* src) {
    uint32_t n = 0;
    for (; src && src[n] && n + 1 < capacity; ++n) dst[n] = src[n];
    dst[n] = 0;
    return !src || !src[n];  // false if it did not fit
  };
  if (!copy(user_, sizeof user_, user) || !copy(auth_password_, sizeof auth_password_, auth_password) ||
      !copy(priv_password_, sizeof priv_password_, priv_password))
    return false;
  auto length = [](const char* s) { uint32_t n = 0; while (s[n]) ++n; return n; };
  if ((auth != UsmAuth::None && length(auth_password_) < 8) || (priv != UsmPriv::None && length(priv_password_) < 8)) return false;
  auth_ = auth;
  priv_ = priv;
  version_ = 3;
  engine_known_ = false;
  return true;
}

bool SnmpClient::set(const Oid* oids, const SnmpValue* values, uint32_t count) {
  if (state_ == SnmpState::Pending) return false;
  if (count == 0 || count > SnmpMessage::kMaxBinds) {
    fail(SnmpError::Invalid);
    return false;
  }
  set_values_ = values;
  const bool ok = start(SnmpPdu::Set, oids, count, 0, 0);
  set_values_ = nullptr;
  return ok;
}

bool SnmpClient::start(SnmpPdu pdu, const Oid* oids, uint32_t count, int32_t a, int32_t b) {
  if (state_ == SnmpState::Pending) return false;
  error_ = SnmpError::None;
  response_ = SnmpMessage();
  if (count == 0 || count > SnmpMessage::kMaxBinds) {
    fail(SnmpError::Invalid);
    return false;
  }
  if (!socket_) socket_ = stack_.udp_open(0);
  if (!socket_) {
    fail(SnmpError::Send);
    return false;
  }
  request_id_ = next_id_;
  next_id_ = (next_id_ + 1) & 0x3FFFFFFF;
  SnmpMessage& m = pending_;
  m = SnmpMessage();
  m.version = version_;
  for (uint32_t i = 0; community_[i]; ++i) m.community[i] = community_[i];
  m.pdu = pdu;
  m.request_id = request_id_;
  m.error_status = a;
  m.error_index = b;
  m.count = count;
  for (uint32_t i = 0; i < count; ++i) {
    m.binds[i].oid = oids[i];
    if (set_values_) m.binds[i].value = set_values_[i];
  }
  attempts_ = 0;
  resynced_ = false;
  discovering_ = false;
  bool ok;
  if (version_ == 3) ok = engine_known_ ? send_v3_request() : send_discovery();
  else ok = encode_snmp(m, allocator_, &packet_);
  if (!ok) {
    fail(SnmpError::Invalid);
    return false;
  }
  state_ = SnmpState::Pending;
  transmit();  // (if this fails the next poll() sends again; an agent that cannot be reached at all times out)
  return true;
}

// Engine discovery (RFC 3414 4): an unauthenticated, reportable request with an empty engine ID and user; the agent answers with
// a Report that carries its engine ID, boot count and clock.
bool SnmpClient::send_discovery() {
  SnmpMessage d;
  d.version = 3;
  d.pdu = SnmpPdu::Get;
  d.request_id = request_id_;
  d.v3.msg_id = request_id_;
  d.v3.flags = 4;
  discovering_ = true;
  return encode_snmp_v3(d, nullptr, 0, allocator_, &packet_);
}

bool SnmpClient::send_v3_request() {
  SnmpMessage m = pending_;
  m.version = 3;
  m.request_id = request_id_;
  SnmpV3& v = m.v3;
  v.msg_id = request_id_;
  v.flags = 4;
  v.engine_id_length = static_cast<uint8_t>(engine_id_length_);
  for (uint32_t i = 0; i < engine_id_length_; ++i) v.engine_id[i] = engine_id_[i];
  v.context_engine_id_length = v.engine_id_length;
  for (uint32_t i = 0; i < engine_id_length_; ++i) v.context_engine_id[i] = engine_id_[i];
  v.boots = boots_;
  v.time = engine_time_ + static_cast<uint32_t>((stack_.now_ms() - time_seen_ms_) / 1000);  // the agent's clock now, estimated
  for (uint32_t i = 0; user_[i]; ++i) v.user[i] = user_[i];
  discovering_ = false;
  return encode_snmp_v3(m, auth_ != UsmAuth::None ? &keys_ : nullptr, salt_++, allocator_, &packet_);
}

bool SnmpClient::transmit() {
  sent_at_ms_ = stack_.now_ms();
  ++attempts_;
  return socket_->send_to(agent_, port_, packet_.view());
}

// A Report PDU (version 3): the agent's complaint about the request.
void SnmpClient::handle_report(const SnmpMessage& m) {
  const SnmpV3& v = m.v3;
  const bool has_stat = m.count > 0;
  if (discovering_ || (has_stat && is_usm_stat(m.binds[0].oid, 4))) {  // unknown engine ID: take the one the agent gives
    if (v.engine_id_length == 0) return fail(SnmpError::Security);
    engine_id_length_ = v.engine_id_length;
    for (uint32_t i = 0; i < engine_id_length_; ++i) engine_id_[i] = v.engine_id[i];
    boots_ = v.boots;
    engine_time_ = v.time;
    time_seen_ms_ = stack_.now_ms();
    if (auth_ != UsmAuth::None) {
      if (!usm_make_keys(auth_, auth_password_, priv_, priv_password_, engine_id_, engine_id_length_, &keys_)) return fail(SnmpError::Security);
    } else {
      keys_ = UsmKeys();
    }
    engine_known_ = true;
    if (!send_v3_request()) return fail(SnmpError::Invalid);
    attempts_ = 0;
    transmit();
    return;
  }
  if (has_stat && is_usm_stat(m.binds[0].oid, 2) && !resynced_) {  // not in time window: adopt the agent's clock and send again
    resynced_ = true;
    boots_ = v.boots;
    engine_time_ = v.time;
    time_seen_ms_ = stack_.now_ms();
    if (!send_v3_request()) return fail(SnmpError::Invalid);
    attempts_ = 0;
    transmit();
    return;
  }
  fail(SnmpError::Security);  // unknown user, wrong digest, unsupported level, decryption error, or time still wrong
}

void SnmpClient::handle_v3(const SnmpMessage& m) {
  if (m.pdu == SnmpPdu::Report) {
    handle_report(m);
    return;
  }
  if (m.pdu != SnmpPdu::Response || m.request_id != request_id_) return;
  // The agent's clock moves on with every authentic answer.
  if (auth_ != UsmAuth::None && m.v3.boots >= boots_) {
    boots_ = m.v3.boots;
    engine_time_ = m.v3.time;
    time_seen_ms_ = stack_.now_ms();
  }
  response_ = m;
  if (m.error_status != 0) fail(SnmpError::Agent);
  else state_ = SnmpState::Done;
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
    if (version_ == 3) {
      const UsmStatus status = decode_snmp_v3(ConstByteView(buffer, length), (!discovering_ && auth_ != UsmAuth::None) ? &keys_ : nullptr, &m);
      if (status == UsmStatus::Malformed) continue;
      if (status != UsmStatus::Ok) {
        // A message that failed its check (bad tag, wrong level, undecryptable) is not an answer to us: ignore it, the retry timer ends the wait.
        continue;
      }
      if (m.v3.msg_id != request_id_) continue;
      handle_v3(m);
      if (state_ != SnmpState::Pending) return;
      continue;
    }
    if (!decode_snmp(ConstByteView(buffer, length), &m) || m.pdu != SnmpPdu::Response || m.request_id != request_id_) continue;
    response_ = m;
    if (m.error_status != 0) fail(SnmpError::Agent);
    else state_ = SnmpState::Done;
    return;
  }
  if (stack_.now_ms() - sent_at_ms_ >= timeout_ms_) {
    if (attempts_ > retries_) fail(SnmpError::Timeout);
    else transmit();
  }
}

}  // namespace network
