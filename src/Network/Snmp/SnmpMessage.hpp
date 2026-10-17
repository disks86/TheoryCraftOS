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
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace network {

// An object identifier: up to 20 arcs.
struct Oid {
  static constexpr uint32_t kMaxArcs = 20;
  uint32_t arc[kMaxArcs] = {};
  uint8_t length = 0;

  // "1.3.6.1.2.1.1.1.0" (a leading dot is accepted). False for anything else.
  static bool parse(const char* text, Oid* out);
  Oid child(uint32_t sub) const {
    Oid o = *this;
    if (o.length < kMaxArcs) o.arc[o.length++] = sub;
    return o;
  }
  bool operator==(const Oid& o) const {
    if (length != o.length) return false;
    for (uint32_t i = 0; i < length; ++i)
      if (arc[i] != o.arc[i]) return false;
    return true;
  }
  bool operator!=(const Oid& o) const { return !(*this == o); }
  // True if `prefix` is this identifier or one of its ancestors.
  bool starts_with(const Oid& prefix) const {
    if (length < prefix.length) return false;
    for (uint32_t i = 0; i < prefix.length; ++i)
      if (arc[i] != prefix.arc[i]) return false;
    return true;
  }
};

enum class SnmpType : uint8_t { Null, Integer, OctetString, ObjectId, IpAddress, Counter32, Gauge32, TimeTicks, Counter64, NoSuchObject, NoSuchInstance, EndOfMibView };

struct SnmpValue {
  static constexpr uint32_t kMaxBytes = 96;
  SnmpType type = SnmpType::Null;
  int64_t integer = 0;       // Integer (signed) and the unsigned types (Counter64 above 2^63 is clamped)
  uint8_t bytes[kMaxBytes] = {};  // OctetString and IpAddress (longer strings are cut)
  uint8_t length = 0;
  Oid oid;                   // ObjectId
  bool is_exception() const { return type == SnmpType::NoSuchObject || type == SnmpType::NoSuchInstance || type == SnmpType::EndOfMibView; }
  bool is_number() const { return type == SnmpType::Integer || (type >= SnmpType::Counter32 && type <= SnmpType::Counter64); }
};

struct SnmpVarBind {
  Oid oid;
  SnmpValue value;
};

enum class SnmpPdu : uint8_t { Get = 0xA0, GetNext = 0xA1, Response = 0xA2, Set = 0xA3, TrapV1 = 0xA4, GetBulk = 0xA5, Inform = 0xA6, TrapV2 = 0xA7, Report = 0xA8 };

// The fields of a version 1 Trap PDU (RFC 1157), which has its own layout.
struct SnmpTrapV1 {
  Oid enterprise;
  uint8_t agent_address[4] = {};
  int32_t generic = 0;   // 0 coldStart ... 6 enterpriseSpecific
  int32_t specific = 0;
  uint32_t timestamp = 0;  // sysUpTime in hundredths of a second
};

// The version 3 parts of a message (RFC 3412 header and RFC 3414 user-based security parameters, plus the scoped PDU's context).
struct SnmpV3 {
  int32_t msg_id = 0;
  int32_t max_size = 1400;
  uint8_t flags = 0;  // bit 0 authenticated, bit 1 encrypted, bit 2 a report may be returned
  uint8_t engine_id[32] = {};
  uint8_t engine_id_length = 0;
  uint32_t boots = 0, time = 0;
  char user[32] = {};
  uint8_t auth_parameters[24] = {};
  uint8_t auth_length = 0;
  uint8_t priv_parameters[8] = {};
  uint8_t priv_length = 0;
  uint8_t context_engine_id[32] = {};
  uint8_t context_engine_id_length = 0;
  char context_name[32] = {};
};

// An SNMP message (RFC 1157 / 3416 / 3412): version 1, 2c or 3; community (v1/v2c) or `v3`; one PDU with up to 12 variable bindings.
struct SnmpMessage {
  static constexpr uint32_t kMaxBinds = 12;
  uint8_t version = 1;  // 0 = v1, 1 = v2c, 3 = v3
  char community[32] = "public";
  SnmpPdu pdu = SnmpPdu::Get;
  int32_t request_id = 0;
  int32_t error_status = 0;  // (GetBulk: non-repeaters)
  int32_t error_index = 0;   // (GetBulk: max-repetitions)
  SnmpVarBind binds[kMaxBinds];
  uint32_t count = 0;
  SnmpTrapV1 trap1;  // TrapV1 PDUs
  SnmpV3 v3;         // version 3
};

// Writes a version 1 or 2c message in BER into `out` (cleared first; `allocator` is for the scratch buffers). Values are written as given - a request's are Null.
bool encode_snmp(const SnmpMessage& message, allocators::IAllocator& allocator, ByteBuffer* out);
// Just the PDU (what is inside a v1/v2c message after the community, or inside a v3 scoped PDU): its tag byte, length and body.
bool encode_snmp_pdu(const SnmpMessage& message, allocators::IAllocator& allocator, ByteBuffer* out);
// Fills pdu, request id, error fields, bindings (or the v1 trap fields) from a PDU whose tag byte is `tag` and body `body`.
bool decode_snmp_pdu(uint8_t tag, ConstByteView body, SnmpMessage* out);
// Reads a version 1 or 2c message from untrusted bytes; false if anything is malformed, truncated or unsupported. Strings longer than
// SnmpValue::kMaxBytes are cut, bindings beyond kMaxBinds make the message invalid.
bool decode_snmp(ConstByteView data, SnmpMessage* out);

}  // namespace network
