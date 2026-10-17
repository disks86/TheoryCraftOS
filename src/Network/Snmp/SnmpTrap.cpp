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

#include "Network/Snmp/SnmpTrap.hpp"

namespace network {

namespace {
bool same_text(const char* a, const char* b) {
  while (*a && *a == *b) ++a, ++b;
  return *a == *b;
}
Oid make(const char* text) {
  Oid o;
  Oid::parse(text, &o);
  return o;
}
}  // namespace

bool SnmpTrapReceiver::start() {
  if (socket_) return true;
  socket_ = stack_.udp_open(port_);
  return static_cast<bool>(socket_);
}

bool SnmpTrapReceiver::accept_community(const char* community) {
  if (community_count_ >= 4 || !community) return false;
  uint32_t n = 0;
  for (; community[n] && n < 31; ++n) communities_[community_count_][n] = community[n];
  if (community[n]) return false;
  communities_[community_count_][n] = 0;
  ++community_count_;
  return true;
}

bool SnmpTrapReceiver::add_v3_user(const char* user, UsmAuth auth, const char* auth_password, UsmPriv priv, const char* priv_password) {
  if (user_count_ >= 4 || !user || (priv != UsmPriv::None && auth == UsmAuth::None)) return false;
  V3User& u = users_[user_count_];
  auto copy = [](char* dst, uint32_t capacity, const char* src) {
    uint32_t n = 0;
    for (; src && src[n] && n + 1 < capacity; ++n) dst[n] = src[n];
    dst[n] = 0;
    return !src || !src[n];
  };
  if (!copy(u.name, sizeof u.name, user) || !copy(u.auth_password, sizeof u.auth_password, auth_password) || !copy(u.priv_password, sizeof u.priv_password, priv_password)) return false;
  u.auth = auth;
  u.priv = priv;
  ++user_count_;
  return true;
}

const UsmKeys* SnmpTrapReceiver::keys_for(uint32_t user, const uint8_t* engine, uint32_t engine_length) {
  for (CachedKeys& c : cache_)
    if (c.used && c.user_index == user && c.engine_length == engine_length) {
      bool same = true;
      for (uint32_t i = 0; i < engine_length; ++i) same = same && c.engine[i] == engine[i];
      if (same) return &c.keys;
    }
  CachedKeys& slot = cache_[cache_next_++ % 4];
  const V3User& u = users_[user];
  slot.used = false;
  if (!usm_make_keys(u.auth, u.auth_password, u.priv, u.priv_password, engine, engine_length, &slot.keys)) return nullptr;
  slot.used = true;
  slot.user_index = static_cast<uint8_t>(user);
  slot.engine_length = static_cast<uint8_t>(engine_length);
  for (uint32_t i = 0; i < engine_length; ++i) slot.engine[i] = engine[i];
  return &slot.keys;
}

void SnmpTrapReceiver::enqueue(const SnmpNotification& n) {
  if (count_ == kQueue) {  // full: the oldest goes
    head_ = (head_ + 1) % kQueue;
    --count_;
    ++dropped_;
  }
  queue_[(head_ + count_) % kQueue] = n;
  ++count_;
}

bool SnmpTrapReceiver::next(SnmpNotification* out) {
  if (count_ == 0) return false;
  *out = queue_[head_];
  head_ = (head_ + 1) % kQueue;
  --count_;
  return true;
}

void SnmpTrapReceiver::poll() {
  if (!socket_) return;
  uint8_t buffer[1500];
  Ipv4Address from;
  uint16_t port;
  uint32_t length;
  for (int guard = 0; guard < 32 && socket_->receive_from(ByteView(buffer), &from, &port, &length); ++guard) {
    if (length <= sizeof buffer) handle(from, port, ConstByteView(buffer, length));
  }
}

void SnmpTrapReceiver::handle(Ipv4Address from, uint16_t port, ConstByteView data) {
  static const Oid sys_uptime = make("1.3.6.1.2.1.1.3.0"), trap_oid = make("1.3.6.1.6.3.1.1.4.1.0"), traps = make("1.3.6.1.6.3.1.1.5");
  SnmpMessage m;
  SnmpNotification n;
  n.source = from;
  // Version first: 30 <length> 02 01 <version>
  size_t at = 2 + (data.size() > 1 && (data[1] & 0x80) ? (data[1] & 0x7F) : 0);
  const bool v3 = data.size() > at + 2 && data[0] == 0x30 && data[at] == 0x02 && data[at + 1] == 0x01 && data[at + 2] == 3;
  if (v3) {
    UsmStatus st = decode_snmp_v3(data, nullptr, &m);   // reads the header even when the message is private
    if ((st != UsmStatus::Ok && st != UsmStatus::WrongLevel) || m.v3.engine_id_length == 0) return void(++rejected_);
    uint32_t user = user_count_;
    for (uint32_t i = 0; i < user_count_; ++i)
      if (same_text(users_[i].name, m.v3.user)) user = i;
    if (user == user_count_) return void(++rejected_);
    const UsmKeys* keys = keys_for(user, m.v3.engine_id, m.v3.engine_id_length);
    if (!keys) return void(++rejected_);
    if (decode_snmp_v3(data, keys->auth == UsmAuth::None ? nullptr : keys, &m) != UsmStatus::Ok) return void(++rejected_);
    if (m.pdu != SnmpPdu::TrapV2) return void(++rejected_);   // (informs and reports are not handled)
    n.version = 3;
    for (uint32_t i = 0; i < sizeof n.user - 1 && m.v3.user[i]; ++i) n.user[i] = m.v3.user[i];
  } else {
    if (!decode_snmp(data, &m)) return void(++rejected_);
    if (community_count_ > 0) {
      bool ok = false;
      for (uint32_t i = 0; i < community_count_; ++i) ok = ok || same_text(communities_[i], m.community);
      if (!ok) return void(++rejected_);
    }
    for (uint32_t i = 0; i < sizeof n.community - 1 && m.community[i]; ++i) n.community[i] = m.community[i];
    if (m.pdu == SnmpPdu::TrapV1) {
      n.version = 0;
      n.enterprise = m.trap1.enterprise;
      n.agent_address = Ipv4Address::make(m.trap1.agent_address[0], m.trap1.agent_address[1], m.trap1.agent_address[2], m.trap1.agent_address[3]);
      n.uptime = m.trap1.timestamp;
      // RFC 3584: generic traps map to snmpTraps.N+1, enterprise-specific ones to enterprise.0.specific
      if (m.trap1.generic >= 0 && m.trap1.generic <= 5) n.trap_oid = traps.child(static_cast<uint32_t>(m.trap1.generic) + 1);
      else n.trap_oid = m.trap1.enterprise.child(0).child(static_cast<uint32_t>(m.trap1.specific));
    } else if (m.pdu == SnmpPdu::TrapV2 || m.pdu == SnmpPdu::Inform) {
      n.version = m.version;
      n.inform = m.pdu == SnmpPdu::Inform;
    } else {
      return void(++rejected_);
    }
  }
  // v2 / v3: the first two bindings are sysUpTime.0 and snmpTrapOID.0.
  for (uint32_t i = 0; i < m.count; ++i) {
    const SnmpVarBind& b = m.binds[i];
    if (m.pdu != SnmpPdu::TrapV1 && b.oid == sys_uptime && b.value.is_number()) {
      n.uptime = static_cast<uint32_t>(b.value.integer);
    } else if (m.pdu != SnmpPdu::TrapV1 && b.oid == trap_oid && b.value.type == SnmpType::ObjectId) {
      n.trap_oid = b.value.oid;
    } else {
      n.binds[n.count++] = b;
    }
  }
  // An inform is confirmed by the same bindings in a Response.
  if (n.inform && socket_) {
    SnmpMessage r = m;
    r.pdu = SnmpPdu::Response;
    r.error_status = 0;
    r.error_index = 0;
    ByteBuffer out(allocator_);
    if (encode_snmp(r, allocator_, &out)) socket_->send_to(from, port, out.view());
  }
  ++received_;
  enqueue(n);
}

}  // namespace network
