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
#include "Utility/ByteBuffer.hpp"

namespace network {

// IPP/1.1 (RFC 8010 encoding, RFC 8011 operations) messages: a builder for requests and a bounds-checked decoder for responses.
namespace ipp {

enum Tag : uint8_t {
  kGroupOperation = 0x01, kGroupJob = 0x02, kGroupEnd = 0x03, kGroupPrinter = 0x04, kGroupUnsupported = 0x05,
  kTagUnsupported = 0x10, kTagUnknown = 0x12, kTagNoValue = 0x13,
  kTagInteger = 0x21, kTagBoolean = 0x22, kTagEnum = 0x23,
  kTagOctetString = 0x30, kTagDateTime = 0x31, kTagResolution = 0x32, kTagRange = 0x33, kTagBegCollection = 0x34,
  kTagTextLang = 0x35, kTagNameLang = 0x36, kTagEndCollection = 0x37,
  kTagText = 0x41, kTagName = 0x42, kTagKeyword = 0x44, kTagUri = 0x45, kTagUriScheme = 0x46, kTagCharset = 0x47,
  kTagLanguage = 0x48, kTagMimeType = 0x49, kTagMemberName = 0x4a,
};

enum Operation : uint16_t {
  kPrintJob = 0x0002, kValidateJob = 0x0004, kCancelJob = 0x0008, kGetJobAttributes = 0x0009, kGetJobs = 0x000a, kGetPrinterAttributes = 0x000b,
};

// A status code below 0x0100 is a success (successful-ok, -ok-ignored-or-substituted-attributes, -ok-conflicting-attributes).
constexpr bool status_ok(uint16_t status) { return status < 0x0100; }

// Builds a request. Every call returns false when memory ran out. Attribute names and string values are plain C strings.
class RequestBuilder {
 public:
  RequestBuilder(ByteBuffer& out, uint16_t operation, uint32_t request_id);
  bool group(uint8_t tag);
  bool add_integer(uint8_t tag, const char* name, int32_t value);  // kTagInteger or kTagEnum
  bool add_boolean(const char* name, bool value);
  bool add_string(uint8_t tag, const char* name, const char* value);
  // A further value of the attribute added last (a 1setOf).
  bool add_more_string(uint8_t tag, const char* value);
  bool add_more_integer(uint8_t tag, int32_t value);
  // The attributes-charset and attributes-natural-language every request starts with (they open the operation group).
  bool standard_operation_attributes();
  // Ends the attributes; whatever follows in the same message (the document of a Print-Job) is appended by the caller.
  bool finish();

 private:
  bool put_attribute(uint8_t tag, const char* name, const uint8_t* value, uint32_t length);
  ByteBuffer& out_;
  bool ok_ = true;
};

struct Attribute {
  static constexpr uint32_t kNameMax = 40, kValueMax = 120;
  uint8_t group = 0;
  uint8_t tag = 0;
  char name[kNameMax] = {};
  uint16_t index = 0;           // 0 for the first value of the attribute, then 1, 2, ... (a 1setOf appears as several attributes)
  int32_t integer = 0;          // for integer, enum and boolean; the first number of a range, the width of a resolution
  char text[kValueMax] = {};    // for the string types (cut short if longer), a resolution as "600x600dpi"
  bool truncated = false;
  bool is_string() const { return tag >= 0x40 || tag == kTagOctetString; }
  bool is_number() const { return tag == kTagInteger || tag == kTagEnum || tag == kTagBoolean; }
};

struct Response {
  static constexpr uint32_t kMaxAttributes = 96;
  uint8_t version_major = 0, version_minor = 0;
  uint16_t status = 0;
  uint32_t request_id = 0;
  Attribute attributes[kMaxAttributes];
  uint32_t count = 0;
  bool overflow = false;  // more attributes than fit: the rest were left out
  // The first value of `name` (in the group `group`, or any group when 0), or nullptr.
  const Attribute* find(const char* name, uint8_t group = 0) const;
  // The attribute `name` with value index `index`.
  const Attribute* find(const char* name, uint32_t index, uint8_t group) const;
  uint32_t value_count(const char* name) const;
};

// Parses an IPP message (the HTTP body). `consumed` receives the length of the message proper, so what follows (a document) can be
// found. False if the data is not a well-formed message.
bool decode_response(ConstByteView data, Response* out, uint32_t* consumed = nullptr);

}  // namespace ipp
}  // namespace network
