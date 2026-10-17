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

#include "Network/Mdns/MdnsMessage.hpp"

namespace network::mdns {

namespace {
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
uint32_t length_of(const char* s) {
  uint32_t n = 0;
  while (s && s[n]) ++n;
  return n;
}
bool put16(ByteBuffer& b, uint32_t v) {
  const uint8_t x[2] = {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
  return b.append(ConstByteView(x, 2));
}
bool put32(ByteBuffer& b, uint32_t v) {
  const uint8_t x[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
  return b.append(ConstByteView(x, 4));
}

// Writes dotted text as labels. A name made of an empty string is the root.
bool write_name(ByteBuffer& out, const char* name) {
  uint32_t i = 0;
  const uint32_t total = length_of(name);
  if (total >= kMaxName) return false;
  while (i < total) {
    uint32_t j = i;
    while (j < total && name[j] != '.') ++j;
    const uint32_t label = j - i;
    if (label == 0 || label > 63) return false;
    if (!out.put8(static_cast<uint8_t>(label)) || !out.append(ConstByteView(reinterpret_cast<const uint8_t*>(name + i), label))) return false;
    i = j + 1;
  }
  return out.put8(0);
}

// Reads a (possibly compressed) name at `offset` into dotted text; *next is the offset after the name as it stands at `offset`.
bool read_name(ConstByteView m, uint64_t offset, char* out, uint64_t* next) {
  const uint8_t* p = m.data();
  const uint64_t size = m.size();
  uint32_t n = 0;
  bool jumped = false;
  uint32_t hops = 0;
  uint64_t pos = offset;
  while (true) {
    if (pos >= size) return false;
    const uint8_t len = p[pos];
    if (len == 0) {
      if (!jumped) *next = pos + 1;
      out[n] = 0;
      return true;
    }
    if ((len & 0xc0) == 0xc0) {
      if (pos + 1 >= size) return false;
      const uint64_t target = static_cast<uint64_t>(len & 0x3f) << 8 | p[pos + 1];
      if (!jumped) *next = pos + 2;
      jumped = true;
      if (++hops > 16 || target >= size) return false;
      pos = target;
      continue;
    }
    if (len > 63 || pos + 1 + len > size) return false;
    if (n != 0) {
      if (n + 1 >= kMaxName) return false;
      out[n++] = '.';
    }
    if (n + len >= kMaxName) return false;
    for (uint32_t i = 0; i < len; ++i) out[n++] = static_cast<char>(p[pos + 1 + i]);
    pos += 1 + len;
  }
}
}  // namespace

bool names_equal(const char* a, const char* b) {
  uint32_t la = length_of(a), lb = length_of(b);
  if (la && a[la - 1] == '.') --la;
  if (lb && b[lb - 1] == '.') --lb;
  if (la != lb) return false;
  for (uint32_t i = 0; i < la; ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

bool build_query(const Question* questions, uint32_t count, ByteBuffer& out) {
  if (!put16(out, 0) || !put16(out, 0) || !put16(out, count) || !put16(out, 0) || !put16(out, 0) || !put16(out, 0)) return false;
  for (uint32_t i = 0; i < count; ++i)
    if (!write_name(out, questions[i].name) || !put16(out, questions[i].type) || !put16(out, kClassIn | (questions[i].unicast_response ? 0x8000 : 0))) return false;
  return true;
}

ResponseBuilder::ResponseBuilder(ByteBuffer& out) : out_(out) {
  ok_ = put16(out_, 0) && put16(out_, 0x8400) && put16(out_, 0) && put16(out_, 0) && put16(out_, 0) && put16(out_, 0);  // response, authoritative
}
bool ResponseBuilder::put_name(const char* name) { return write_name(out_, name); }
bool ResponseBuilder::header_of(const char* name, uint16_t type, uint32_t ttl, uint32_t* length_offset) {
  if (!ok_) return false;
  ok_ = put_name(name) && put16(out_, type) && put16(out_, kClassIn | 0x8000) && put32(out_, ttl) && put16(out_, 0);
  *length_offset = static_cast<uint32_t>(out_.size()) - 2;
  ++records_;
  return ok_;
}
bool ResponseBuilder::end_record(uint32_t length_offset) {
  const uint32_t length = static_cast<uint32_t>(out_.size()) - length_offset - 2;
  // patch the two length bytes
  return ok_ = ok_ && out_.patch8(length_offset, static_cast<uint8_t>(length >> 8)) && out_.patch8(length_offset + 1, static_cast<uint8_t>(length));
}
bool ResponseBuilder::add_ptr(const char* name, const char* target, uint32_t ttl) {
  uint32_t at;
  return header_of(name, kTypePtr, ttl, &at) && put_name(target) && end_record(at);
}
bool ResponseBuilder::add_srv(const char* name, const char* target, uint16_t port, uint32_t ttl) {
  uint32_t at;
  return header_of(name, kTypeSrv, ttl, &at) && put16(out_, 0) && put16(out_, 0) && put16(out_, port) && put_name(target) && end_record(at);
}
bool ResponseBuilder::add_txt(const char* name, const char* const* strings, uint32_t count, uint32_t ttl) {
  uint32_t at;
  if (!header_of(name, kTypeTxt, ttl, &at)) return false;
  if (count == 0) ok_ = out_.put8(0);
  for (uint32_t i = 0; ok_ && i < count; ++i) {
    const uint32_t n = length_of(strings[i]);
    ok_ = n <= 255 && out_.put8(static_cast<uint8_t>(n)) && out_.append(ConstByteView(reinterpret_cast<const uint8_t*>(strings[i]), n));
  }
  return ok_ && end_record(at);
}
bool ResponseBuilder::add_a(const char* name, Ipv4Address address, uint32_t ttl) {
  uint32_t at;
  return header_of(name, kTypeA, ttl, &at) && put32(out_, address.value()) && end_record(at);
}
bool ResponseBuilder::finish() {
  if (!ok_) return false;
  // the answer count lives at offset 6..7
  return out_.patch8(6, static_cast<uint8_t>(records_ >> 8)) && out_.patch8(7, static_cast<uint8_t>(records_));
}

bool decode_message(ConstByteView data, Message* out) {
  const uint8_t* p = data.data();
  const uint64_t size = data.size();
  if (size < 12) return false;
  out->id = 0;
  out->question_count = 0;
  out->record_count = 0;
  out->id = static_cast<uint16_t>(p[0] << 8 | p[1]);
  out->response = (p[2] & 0x80) != 0;
  const uint32_t qd = static_cast<uint32_t>(p[4] << 8 | p[5]), an = static_cast<uint32_t>(p[6] << 8 | p[7]);
  const uint32_t ns = static_cast<uint32_t>(p[8] << 8 | p[9]), ar = static_cast<uint32_t>(p[10] << 8 | p[11]);
  uint64_t pos = 12;
  for (uint32_t i = 0; i < qd; ++i) {
    char name[kMaxName];
    if (!read_name(data, pos, name, &pos) || pos + 4 > size) return false;
    const uint16_t type = static_cast<uint16_t>(p[pos] << 8 | p[pos + 1]);
    const uint16_t cls = static_cast<uint16_t>(p[pos + 2] << 8 | p[pos + 3]);
    pos += 4;
    if (out->question_count < Message::kMaxQuestions) {
      Question& q = out->questions[out->question_count++];
      for (uint32_t k = 0; name[k]; ++k) q.name[k] = name[k];
      q.type = type;
      q.unicast_response = (cls & 0x8000) != 0;
    }
  }
  const uint32_t total = an + ns + ar;
  if (total > 1000) return false;
  for (uint32_t i = 0; i < total; ++i) {
    char name[kMaxName];
    if (!read_name(data, pos, name, &pos) || pos + 10 > size) return false;
    const uint16_t type = static_cast<uint16_t>(p[pos] << 8 | p[pos + 1]);
    const uint16_t cls = static_cast<uint16_t>(p[pos + 2] << 8 | p[pos + 3]);
    const uint32_t ttl = static_cast<uint32_t>(p[pos + 4]) << 24 | static_cast<uint32_t>(p[pos + 5]) << 16 | static_cast<uint32_t>(p[pos + 6]) << 8 | p[pos + 7];
    const uint32_t rdlength = static_cast<uint32_t>(p[pos + 8] << 8 | p[pos + 9]);
    pos += 10;
    if (pos + rdlength > size) return false;
    const uint64_t rdata = pos;
    pos += rdlength;
    if (type != kTypeA && type != kTypePtr && type != kTypeSrv && type != kTypeTxt) continue;
    if (out->record_count >= Message::kMaxRecords) continue;
    Record r;
    for (uint32_t k = 0; name[k]; ++k) r.name[k] = name[k];
    r.type = type;
    r.ttl = ttl;
    r.cache_flush = (cls & 0x8000) != 0;
    uint64_t unused;
    switch (type) {
      case kTypeA:
        if (rdlength != 4) return false;
        r.address = Ipv4Address::make(p[rdata], p[rdata + 1], p[rdata + 2], p[rdata + 3]);
        break;
      case kTypePtr:
        if (!read_name(data, rdata, r.target, &unused)) return false;
        break;
      case kTypeSrv:
        if (rdlength < 7) return false;
        r.priority = static_cast<uint16_t>(p[rdata] << 8 | p[rdata + 1]);
        r.weight = static_cast<uint16_t>(p[rdata + 2] << 8 | p[rdata + 3]);
        r.port = static_cast<uint16_t>(p[rdata + 4] << 8 | p[rdata + 5]);
        if (!read_name(data, rdata + 6, r.target, &unused)) return false;
        break;
      case kTypeTxt: {
        // validate the string lengths, keep what fits
        uint32_t at = 0;
        while (at < rdlength) {
          const uint32_t len = p[rdata + at];
          if (at + 1 + len > rdlength) return false;
          at += 1 + len;
        }
        r.txt_length = rdlength < kMaxTxt ? rdlength : 0;  // an oversized TXT is left empty rather than cut in the middle of a string
        for (uint32_t k = 0; k < r.txt_length; ++k) r.txt[k] = p[rdata + k];
        break;
      }
    }
    out->records[out->record_count++] = r;
  }
  return true;
}

bool txt_value(const uint8_t* txt, uint32_t length, const char* key, char* value, uint32_t capacity) {
  const uint32_t key_length = length_of(key);
  uint32_t at = 0;
  while (at < length) {
    const uint32_t len = txt[at];
    if (at + 1 + len > length) return false;
    const uint8_t* s = txt + at + 1;
    if (len >= key_length) {
      bool match = true;
      for (uint32_t i = 0; i < key_length && match; ++i) match = lower(static_cast<char>(s[i])) == lower(key[i]);
      if (match && (len == key_length || s[key_length] == '=')) {
        uint32_t n = 0;
        for (uint32_t i = key_length + 1; i < len && n + 1 < capacity; ++i) value[n++] = static_cast<char>(s[i]);
        if (capacity) value[n] = 0;
        return true;
      }
    }
    at += 1 + len;
  }
  return false;
}

}  // namespace network::mdns
