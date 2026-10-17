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

#include "Network/Ipp/IppMessage.hpp"

namespace network::ipp {

namespace {
uint32_t length_of(const char* s) {
  uint32_t n = 0;
  while (s && s[n]) ++n;
  return n;
}
bool same(const char* a, const char* b) {
  uint32_t i = 0;
  for (; a[i] && a[i] == b[i]; ++i) {}
  return a[i] == b[i];
}
}  // namespace

RequestBuilder::RequestBuilder(ByteBuffer& out, uint16_t operation, uint32_t request_id) : out_(out) {
  const uint8_t head[8] = {1, 1, static_cast<uint8_t>(operation >> 8), static_cast<uint8_t>(operation), static_cast<uint8_t>(request_id >> 24),
                           static_cast<uint8_t>(request_id >> 16), static_cast<uint8_t>(request_id >> 8), static_cast<uint8_t>(request_id)};
  ok_ = out_.append(ConstByteView(head, 8));
}

bool RequestBuilder::group(uint8_t tag) { return ok_ = ok_ && out_.put8(tag); }

bool RequestBuilder::put_attribute(uint8_t tag, const char* name, const uint8_t* value, uint32_t length) {
  const uint32_t name_length = length_of(name);
  if (!ok_ || name_length > 0xffff || length > 0xffff) return ok_ = false;
  const uint8_t head[3] = {tag, static_cast<uint8_t>(name_length >> 8), static_cast<uint8_t>(name_length)};
  const uint8_t value_length[2] = {static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
  ok_ = out_.append(ConstByteView(head, 3)) && (name_length == 0 || out_.append(ConstByteView(reinterpret_cast<const uint8_t*>(name), name_length))) &&
        out_.append(ConstByteView(value_length, 2)) && (length == 0 || out_.append(ConstByteView(value, length)));
  return ok_;
}

bool RequestBuilder::add_integer(uint8_t tag, const char* name, int32_t value) {
  const uint32_t v = static_cast<uint32_t>(value);
  const uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
  return put_attribute(tag, name, b, 4);
}
bool RequestBuilder::add_boolean(const char* name, bool value) {
  const uint8_t b = value ? 1 : 0;
  return put_attribute(kTagBoolean, name, &b, 1);
}
bool RequestBuilder::add_string(uint8_t tag, const char* name, const char* value) {
  return put_attribute(tag, name, reinterpret_cast<const uint8_t*>(value), length_of(value));
}
bool RequestBuilder::add_more_string(uint8_t tag, const char* value) { return add_string(tag, "", value); }

bool RequestBuilder::add_more_integer(uint8_t tag, int32_t value) { return add_integer(tag, "", value); }

bool RequestBuilder::standard_operation_attributes() {
  return group(kGroupOperation) && add_string(kTagCharset, "attributes-charset", "utf-8") &&
         add_string(kTagLanguage, "attributes-natural-language", "en");
}
bool RequestBuilder::finish() { return group(kGroupEnd); }

const Attribute* Response::find(const char* name, uint8_t group) const {
  for (uint32_t i = 0; i < count; ++i)
    if (attributes[i].index == 0 && (group == 0 || attributes[i].group == group) && same(attributes[i].name, name)) return &attributes[i];
  return nullptr;
}
const Attribute* Response::find(const char* name, uint32_t index, uint8_t group) const {
  for (uint32_t i = 0; i < count; ++i)
    if (attributes[i].index == index && (group == 0 || attributes[i].group == group) && same(attributes[i].name, name)) return &attributes[i];
  return nullptr;
}
uint32_t Response::value_count(const char* name) const {
  uint32_t n = 0;
  for (uint32_t i = 0; i < count; ++i)
    if (same(attributes[i].name, name)) ++n;
  return n;
}

bool decode_response(ConstByteView data, Response* out, uint32_t* consumed) {
  const uint8_t* p = data.data();
  const uint64_t size = data.size();
  if (size < 9) return false;
  *out = Response();
  out->version_major = p[0];
  out->version_minor = p[1];
  if (p[0] != 1 && p[0] != 2) return false;
  out->status = static_cast<uint16_t>(p[2] << 8 | p[3]);
  out->request_id = static_cast<uint32_t>(p[4]) << 24 | static_cast<uint32_t>(p[5]) << 16 | static_cast<uint32_t>(p[6]) << 8 | p[7];
  uint64_t pos = 8;
  uint8_t group = 0;
  int depth = 0;                      // inside a collection: its members are read and left out
  char last_name[Attribute::kNameMax] = {};
  uint16_t last_index = 0;
  bool have_last = false;
  while (true) {
    if (pos >= size) return false;    // no end-of-attributes tag
    const uint8_t tag = p[pos++];
    if (tag == kGroupEnd) break;
    if (tag < 0x10) {                 // a delimiter: starts a group
      group = tag;
      have_last = false;
      continue;
    }
    if (pos + 2 > size) return false;
    const uint32_t name_length = static_cast<uint32_t>(p[pos] << 8 | p[pos + 1]);
    pos += 2;
    if (pos + name_length + 2 > size) return false;
    const uint8_t* name = p + pos;
    pos += name_length;
    const uint32_t value_length = static_cast<uint32_t>(p[pos] << 8 | p[pos + 1]);
    pos += 2;
    if (pos + value_length > size) return false;
    const uint8_t* value = p + pos;
    pos += value_length;

    if (tag == kTagBegCollection) { ++depth; continue; }
    if (tag == kTagEndCollection) { if (depth > 0) --depth; continue; }
    if (depth > 0) continue;
    if (group == 0) return false;     // an attribute outside every group

    if (out->count >= Response::kMaxAttributes) { out->overflow = true; continue; }
    Attribute& a = out->attributes[out->count];
    a.group = group;
    a.tag = tag;
    if (name_length > 0) {
      uint32_t n = name_length < Attribute::kNameMax - 1 ? name_length : Attribute::kNameMax - 1;
      for (uint32_t i = 0; i < n; ++i) a.name[i] = static_cast<char>(name[i]);
      a.name[n] = 0;
      for (uint32_t i = 0; i <= n; ++i) last_name[i] = a.name[i];
      last_index = 0;
      have_last = true;
    } else {
      if (!have_last) return false;   // an additional value with nothing to add to
      for (uint32_t i = 0; i < Attribute::kNameMax; ++i) a.name[i] = last_name[i];
      a.index = ++last_index;
    }
    if (tag == kTagInteger || tag == kTagEnum) {
      if (value_length != 4) return false;
      a.integer = static_cast<int32_t>(static_cast<uint32_t>(value[0]) << 24 | static_cast<uint32_t>(value[1]) << 16 | static_cast<uint32_t>(value[2]) << 8 | value[3]);
    } else if (tag == kTagBoolean) {
      if (value_length != 1) return false;
      a.integer = value[0] ? 1 : 0;
    } else if (tag == kTagResolution) {
      if (value_length != 9) return false;
      const int32_t x = static_cast<int32_t>(static_cast<uint32_t>(value[0]) << 24 | static_cast<uint32_t>(value[1]) << 16 | static_cast<uint32_t>(value[2]) << 8 | value[3]);
      const int32_t y = static_cast<int32_t>(static_cast<uint32_t>(value[4]) << 24 | static_cast<uint32_t>(value[5]) << 16 | static_cast<uint32_t>(value[6]) << 8 | value[7]);
      a.integer = x;
      // "<x>x<y>dpi" (unit 3) or dpcm (4)
      char t[40];
      uint32_t n = 0;
      auto put_number = [&](int32_t v) {
        char digits[12];
        uint32_t k = 0;
        uint32_t u = v < 0 ? 0 : static_cast<uint32_t>(v);
        do { digits[k++] = static_cast<char>('0' + u % 10); u /= 10; } while (u && k < 11);
        while (k) t[n++] = digits[--k];
      };
      put_number(x);
      t[n++] = 'x';
      put_number(y);
      const char* unit = value[8] == 4 ? "dpcm" : "dpi";
      for (; *unit; ++unit) t[n++] = *unit;
      for (uint32_t i = 0; i < n; ++i) a.text[i] = t[i];
      a.text[n] = 0;
    } else if (tag == kTagRange) {
      if (value_length != 8) return false;
      a.integer = static_cast<int32_t>(static_cast<uint32_t>(value[0]) << 24 | static_cast<uint32_t>(value[1]) << 16 | static_cast<uint32_t>(value[2]) << 8 | value[3]);
    } else if (tag >= 0x40 || tag == kTagOctetString) {
      uint32_t n = value_length < Attribute::kValueMax - 1 ? value_length : Attribute::kValueMax - 1;
      for (uint32_t i = 0; i < n; ++i) a.text[i] = static_cast<char>(value[i]);
      a.text[n] = 0;
      a.truncated = n < value_length;
    }
    ++out->count;
  }
  if (consumed) *consumed = static_cast<uint32_t>(pos);
  return true;
}

}  // namespace network::ipp
