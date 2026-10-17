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
#include "Network/Snmp/SnmpMessage.hpp"

namespace network {
namespace ber {

// BER building and parsing shared by the SNMP message code (SnmpMessage.cpp, SnmpUsm.cpp, SnmpTrap): lengths up to 65535, definite
// forms only. Readers are bounds-checked: they fail instead of reading past the data.

// ---- encoding

inline bool put_length(ByteBuffer* b, uint32_t n) {
  if (n < 128) return b->put8(static_cast<uint8_t>(n));
  if (n < 256) return b->put8(0x81) && b->put8(static_cast<uint8_t>(n));
  return b->put8(0x82) && b->put8(static_cast<uint8_t>(n >> 8)) && b->put8(static_cast<uint8_t>(n));
}

// Builds one TLV in a scratch buffer and appends it, so lengths need no patching.
inline bool put_tlv(ByteBuffer* b, uint8_t tag, ConstByteView content) {
  return b->put8(tag) && put_length(b, static_cast<uint32_t>(content.size())) && b->append(content);
}

inline bool put_integer(ByteBuffer* b, uint8_t tag, int64_t v) {
  uint8_t raw[9];
  int n = 0;
  // Two's complement, shortest form.
  for (int shift = 56; shift >= 0; shift -= 8) raw[n++] = static_cast<uint8_t>(static_cast<uint64_t>(v) >> shift);
  int start = 0;
  while (start < 7 && ((raw[start] == 0x00 && (raw[start + 1] & 0x80) == 0) || (raw[start] == 0xFF && (raw[start + 1] & 0x80) != 0))) ++start;
  return put_tlv(b, tag, ConstByteView(raw + start, static_cast<uint64_t>(8 - start)));
}

inline bool put_oid(ByteBuffer* b, const Oid& oid) {
  if (oid.length < 2 || oid.arc[0] > 2) return false;
  uint8_t raw[Oid::kMaxArcs * 5 + 1];
  uint32_t n = 0;
  auto sub = [&](uint32_t v) {
    uint8_t tmp[5];
    int k = 0;
    do {
      tmp[k++] = static_cast<uint8_t>(v & 0x7F);
      v >>= 7;
    } while (v);
    while (k) {
      --k;
      raw[n++] = static_cast<uint8_t>(tmp[k] | (k ? 0x80 : 0));
    }
  };
  sub(oid.arc[0] * 40 + oid.arc[1]);
  for (uint32_t i = 2; i < oid.length; ++i) sub(oid.arc[i]);
  return put_tlv(b, 0x06, ConstByteView(raw, n));
}

inline bool put_value(ByteBuffer* b, const SnmpValue& v) {
  switch (v.type) {
    case SnmpType::Null: return b->put8(0x05) && b->put8(0);
    case SnmpType::Integer: return put_integer(b, 0x02, v.integer);
    case SnmpType::OctetString: return put_tlv(b, 0x04, ConstByteView(v.bytes, v.length));
    case SnmpType::ObjectId: return put_oid(b, v.oid);
    case SnmpType::IpAddress: return put_tlv(b, 0x40, ConstByteView(v.bytes, v.length));
    case SnmpType::Counter32: return put_integer(b, 0x41, v.integer);
    case SnmpType::Gauge32: return put_integer(b, 0x42, v.integer);
    case SnmpType::TimeTicks: return put_integer(b, 0x43, v.integer);
    case SnmpType::Counter64: return put_integer(b, 0x46, v.integer);
    case SnmpType::NoSuchObject: return b->put8(0x80) && b->put8(0);
    case SnmpType::NoSuchInstance: return b->put8(0x81) && b->put8(0);
    case SnmpType::EndOfMibView: return b->put8(0x82) && b->put8(0);
  }
  return false;
}

// ---- decoding

struct Reader {
  ConstByteView d;
  uint64_t pos = 0;
  bool ok = true;
  bool tlv(uint8_t* tag, ConstByteView* content) {
    if (!ok || pos + 2 > d.size()) return ok = false;
    *tag = d[pos++];
    uint32_t len = d[pos++];
    if (len & 0x80) {
      const uint32_t bytes = len & 0x7F;
      if (bytes == 0 || bytes > 2 || pos + bytes > d.size()) return ok = false;
      len = 0;
      for (uint32_t i = 0; i < bytes; ++i) len = (len << 8) | d[pos++];
    }
    if (pos + len > d.size()) return ok = false;
    *content = d.subview(pos, len);
    pos += len;
    return true;
  }
  bool done() const { return pos == d.size(); }
};

inline bool read_int(ConstByteView c, int64_t* out, bool is_signed) {
  if (c.empty() || c.size() > 9) return false;
  uint64_t v = (is_signed && (c[0] & 0x80)) ? ~uint64_t{0} : 0;
  uint64_t start = 0;
  if (!is_signed && c[0] == 0 && c.size() > 1) start = 1;  // a leading zero keeps an unsigned number positive
  if (c.size() - start > 8) return false;
  for (uint64_t i = start; i < c.size(); ++i) v = (v << 8) | c[i];
  if (!is_signed && v > 0x7FFFFFFFFFFFFFFFull) v = 0x7FFFFFFFFFFFFFFFull;
  *out = static_cast<int64_t>(v);
  return true;
}

inline bool read_oid(ConstByteView c, Oid* out) {
  *out = Oid();
  if (c.empty()) return false;
  uint64_t i = 0;
  bool first = true;
  while (i < c.size()) {
    uint64_t v = 0;
    uint32_t bytes = 0;
    for (;;) {
      if (i >= c.size() || ++bytes > 5) return false;
      const uint8_t b = c[i++];
      v = (v << 7) | (b & 0x7F);
      if (!(b & 0x80)) break;
    }
    if (v > 0xFFFFFFFFull + 80) return false;
    if (first) {
      first = false;
      const uint32_t a = v < 40 ? 0 : v < 80 ? 1 : 2;
      out->arc[out->length++] = a;
      out->arc[out->length++] = static_cast<uint32_t>(v - a * 40);
    } else {
      if (out->length >= Oid::kMaxArcs || v > 0xFFFFFFFFull) return false;
      out->arc[out->length++] = static_cast<uint32_t>(v);
    }
  }
  return true;
}

inline bool read_value(uint8_t tag, ConstByteView c, SnmpValue* v) {
  *v = SnmpValue();
  switch (tag) {
    case 0x02: v->type = SnmpType::Integer; return read_int(c, &v->integer, true);
    case 0x04:
    case 0x44: {
      v->type = SnmpType::OctetString;
      v->length = static_cast<uint8_t>(c.size() < SnmpValue::kMaxBytes ? c.size() : SnmpValue::kMaxBytes);
      for (uint32_t i = 0; i < v->length; ++i) v->bytes[i] = c[i];
      return true;
    }
    case 0x05: return c.empty();
    case 0x06: v->type = SnmpType::ObjectId; return read_oid(c, &v->oid);
    case 0x40:
      if (c.size() != 4) return false;
      v->type = SnmpType::IpAddress;
      v->length = 4;
      for (uint32_t i = 0; i < 4; ++i) v->bytes[i] = c[i];
      return true;
    case 0x41: v->type = SnmpType::Counter32; return read_int(c, &v->integer, false);
    case 0x42: v->type = SnmpType::Gauge32; return read_int(c, &v->integer, false);
    case 0x43: v->type = SnmpType::TimeTicks; return read_int(c, &v->integer, false);
    case 0x46: v->type = SnmpType::Counter64; return read_int(c, &v->integer, false);
    case 0x80: v->type = SnmpType::NoSuchObject; return c.empty();
    case 0x81: v->type = SnmpType::NoSuchInstance; return c.empty();
    case 0x82: v->type = SnmpType::EndOfMibView; return c.empty();
    default: return false;
  }
}


}  // namespace ber
}  // namespace network
