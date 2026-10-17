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
#include "Network/NetworkTypes.hpp"
#include "Utility/ByteBuffer.hpp"

namespace network::mdns {

// Multicast DNS (RFC 6762) and DNS-SD (RFC 6763) messages: enough to ask for services and read the records that answer.
// Names are handled as dotted text ("My Printer._ipp._tcp.local"); a dot inside an instance name is not escaped, so such names cannot be told apart.
constexpr uint16_t kTypeA = 1, kTypePtr = 12, kTypeTxt = 16, kTypeSrv = 33, kTypeAny = 255;
constexpr uint16_t kClassIn = 1;
constexpr uint32_t kMaxName = 128;
constexpr uint32_t kMaxTxt = 400;
constexpr uint16_t kPort = 5353;
constexpr Ipv4Address kGroup = Ipv4Address::make(224, 0, 0, 251);

struct Question {
  char name[kMaxName] = {};
  uint16_t type = kTypePtr;
  bool unicast_response = false;  // the QU bit: ask for a direct answer
};

struct Record {
  char name[kMaxName] = {};
  uint16_t type = 0;
  uint32_t ttl = 0;
  bool cache_flush = false;
  // PTR: target. SRV: target (host) with priority, weight and port.
  char target[kMaxName] = {};
  uint16_t priority = 0, weight = 0, port = 0;
  // A
  Ipv4Address address;
  // TXT: the raw strings (each length-prefixed), as they came
  uint8_t txt[kMaxTxt] = {};
  uint32_t txt_length = 0;
};

struct Message {
  static constexpr uint32_t kMaxRecords = 48, kMaxQuestions = 8;
  uint16_t id = 0;
  bool response = false;
  Question questions[kMaxQuestions];
  uint32_t question_count = 0;
  Record records[kMaxRecords];  // answers, authority and additional, in order
  uint32_t record_count = 0;
};

// Builds a query for the given questions into `out`. False if a name is too long or memory ran out.
bool build_query(const Question* questions, uint32_t count, ByteBuffer& out);

// Builds a response with the given records as answers (what a responder sends; used by the tests, and handy for announcing).
class ResponseBuilder {
 public:
  explicit ResponseBuilder(ByteBuffer& out);
  bool add_ptr(const char* name, const char* target, uint32_t ttl);
  bool add_srv(const char* name, const char* target, uint16_t port, uint32_t ttl);
  bool add_txt(const char* name, const char* const* strings, uint32_t count, uint32_t ttl);
  bool add_a(const char* name, Ipv4Address address, uint32_t ttl);
  // Patches the record count; the message is complete after this.
  bool finish();

 private:
  bool header_of(const char* name, uint16_t type, uint32_t ttl, uint32_t* length_offset);
  bool put_name(const char* name);
  bool end_record(uint32_t length_offset);
  ByteBuffer& out_;
  uint32_t records_ = 0;
  bool ok_ = true;
};

// Reads a message. Names with compression pointers are followed (with a limit against loops); record types other than A, PTR, SRV and TXT
// are skipped. False if the message is malformed. Records beyond the capacity are dropped.
bool decode_message(ConstByteView data, Message* out);

// Case-insensitive name comparison (DNS names are).
bool names_equal(const char* a, const char* b);

// A TXT record's value for `key` ("rp=ipp/print" gives "ipp/print"), copied into `value`. False if absent. A key with no "=" gives an empty value.
bool txt_value(const uint8_t* txt, uint32_t length, const char* key, char* value, uint32_t capacity);

}  // namespace network::mdns
