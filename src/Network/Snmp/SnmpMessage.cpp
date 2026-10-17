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

#include "Network/Snmp/SnmpMessage.hpp"
#include "Network/Snmp/SnmpBer.hpp"

namespace network {

bool Oid::parse(const char* text, Oid* out) {
  *out = Oid();
  if (*text == '.') ++text;
  if (!*text) return false;
  for (;;) {
    if (*text < '0' || *text > '9' || out->length >= kMaxArcs) return false;
    uint64_t v = 0;
    while (*text >= '0' && *text <= '9') {
      v = v * 10 + static_cast<uint64_t>(*text++ - '0');
      if (v > 0xFFFFFFFFull) return false;
    }
    out->arc[out->length++] = static_cast<uint32_t>(v);
    if (!*text) break;
    if (*text++ != '.') return false;
  }
  return out->length >= 2;
}

using namespace ber;

bool encode_snmp_pdu(const SnmpMessage& m, allocators::IAllocator& allocator, ByteBuffer* out) {
  out->clear();
  if (m.count > SnmpMessage::kMaxBinds) return false;
  const bool trap1 = m.pdu == SnmpPdu::TrapV1;
  ByteBuffer binds(allocator), pdu(allocator);
  for (uint32_t i = 0; i < m.count; ++i) {
    ByteBuffer one(allocator);
    if (!put_oid(&one, m.binds[i].oid) || !put_value(&one, m.binds[i].value)) return false;
    if (!put_tlv(&binds, 0x30, one.view())) return false;
  }
  if (trap1) {  // enterprise, agent address, generic, specific, time stamp, bindings
    if (!put_oid(&pdu, m.trap1.enterprise) || !put_tlv(&pdu, 0x40, ConstByteView(m.trap1.agent_address, 4)) || !put_integer(&pdu, 0x02, m.trap1.generic) ||
        !put_integer(&pdu, 0x02, m.trap1.specific) || !put_integer(&pdu, 0x43, m.trap1.timestamp) || !put_tlv(&pdu, 0x30, binds.view()))
      return false;
  } else if (!put_integer(&pdu, 0x02, m.request_id) || !put_integer(&pdu, 0x02, m.error_status) || !put_integer(&pdu, 0x02, m.error_index) ||
             !put_tlv(&pdu, 0x30, binds.view())) {
    return false;
  }
  return put_tlv(out, static_cast<uint8_t>(m.pdu), pdu.view());
}

bool encode_snmp(const SnmpMessage& m, allocators::IAllocator& allocator, ByteBuffer* out) {
  out->clear();
  if (m.version > 1) return false;
  ByteBuffer pdu(allocator), body(allocator);
  if (!encode_snmp_pdu(m, allocator, &pdu)) return false;
  uint32_t community_length = 0;
  while (m.community[community_length] && community_length < sizeof m.community - 1) ++community_length;
  if (!put_integer(&body, 0x02, m.version) || !put_tlv(&body, 0x04, ConstByteView(reinterpret_cast<const uint8_t*>(m.community), community_length)) ||
      !body.append(pdu.view()))
    return false;
  return put_tlv(out, 0x30, body.view());
}

bool decode_snmp_pdu(uint8_t tag, ConstByteView body, SnmpMessage* m) {
  if (tag < 0xA0 || tag > 0xA8) return false;
  m->pdu = static_cast<SnmpPdu>(tag);
  Reader pdu{body};
  int64_t v;
  ConstByteView content;
  if (tag == 0xA4) {  // v1 trap
    if (!pdu.tlv(&tag, &content) || tag != 0x06 || !read_oid(content, &m->trap1.enterprise)) return false;
    if (!pdu.tlv(&tag, &content) || tag != 0x40 || content.size() != 4) return false;
    for (int i = 0; i < 4; ++i) m->trap1.agent_address[i] = content[i];
    if (!pdu.tlv(&tag, &content) || tag != 0x02 || !read_int(content, &v, true)) return false;
    m->trap1.generic = static_cast<int32_t>(v);
    if (!pdu.tlv(&tag, &content) || tag != 0x02 || !read_int(content, &v, true)) return false;
    m->trap1.specific = static_cast<int32_t>(v);
    if (!pdu.tlv(&tag, &content) || tag != 0x43 || !read_int(content, &v, false)) return false;
    m->trap1.timestamp = static_cast<uint32_t>(v);
  } else {
    if (!pdu.tlv(&tag, &content) || tag != 0x02 || !read_int(content, &v, true)) return false;
    m->request_id = static_cast<int32_t>(v);
    if (!pdu.tlv(&tag, &content) || tag != 0x02 || !read_int(content, &v, true)) return false;
    m->error_status = static_cast<int32_t>(v);
    if (!pdu.tlv(&tag, &content) || tag != 0x02 || !read_int(content, &v, true)) return false;
    m->error_index = static_cast<int32_t>(v);
  }
  if (!pdu.tlv(&tag, &content) || tag != 0x30 || !pdu.done()) return false;
  Reader list{content};
  m->count = 0;
  while (!list.done()) {
    if (m->count >= SnmpMessage::kMaxBinds) return false;
    if (!list.tlv(&tag, &content) || tag != 0x30) return false;
    Reader bind{content};
    SnmpVarBind& b = m->binds[m->count];
    if (!bind.tlv(&tag, &content) || tag != 0x06 || !read_oid(content, &b.oid)) return false;
    if (!bind.tlv(&tag, &content) || !read_value(tag, content, &b.value) || !bind.done()) return false;
    ++m->count;
  }
  return true;
}

bool decode_snmp(ConstByteView data, SnmpMessage* m) {
  *m = SnmpMessage();
  Reader top{data};
  uint8_t tag;
  ConstByteView content;
  if (!top.tlv(&tag, &content) || tag != 0x30 || !top.done()) return false;
  Reader msg{content};
  int64_t version;
  if (!msg.tlv(&tag, &content) || tag != 0x02 || !read_int(content, &version, true) || (version != 0 && version != 1)) return false;
  m->version = static_cast<uint8_t>(version);
  if (!msg.tlv(&tag, &content) || tag != 0x04 || content.size() >= sizeof m->community) return false;
  for (uint64_t i = 0; i < content.size(); ++i) m->community[i] = static_cast<char>(content[i]);
  m->community[content.size()] = 0;
  if (!msg.tlv(&tag, &content) || !msg.done()) return false;
  return decode_snmp_pdu(tag, content, m);
}

}  // namespace network
