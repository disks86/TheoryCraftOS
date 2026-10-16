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

#include "Network/Dns/DnsMessage.hpp"

namespace network::dns {

namespace {
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Reads a (possibly compressed) name at `offset` into dotted text. Sets *next to
// the offset just past the name as it appears at `offset` (compression pointers
// are not followed for that). False on malformed data.
bool read_name(ConstByteView m, uint64_t offset, ArrayView<char> out, uint64_t* next) {
  uint64_t written = 0, jumps = 0;
  bool jumped = false;
  for (;;) {
    uint8_t b;
    if (!m.try_get(offset, &b)) return false;
    if ((b & 0xC0) == 0xC0) {  // compression pointer: the name continues elsewhere
      uint8_t low;
      if (!m.try_get(offset + 1, &low)) return false;
      if (!jumped) *next = offset + 2;
      jumped = true;
      offset = (uint64_t{static_cast<uint8_t>(b & 0x3F)} << 8) | low;
      if (++jumps > 64) return false;  // a pointer loop
    } else if (b == 0) {
      if (!jumped) *next = offset + 1;
      if (written >= out.size()) return false;
      out[written] = '\0';
      return true;
    } else if (b & 0xC0) {
      return false;  // reserved label types
    } else {
      ConstByteView label;
      if (!m.try_subview(offset + 1, b, &label)) return false;  // the label runs past the end
      if (written + b + 2 > out.size()) return false;
    if (written) out[written++] = '.';
      for (uint8_t c : label) out[written++] = static_cast<char>(c);
      offset += 1 + b;
    }
  }
}

// Copies a name into a text buffer of known size (with a NUL); false if it does not fit.
bool copy_name(ArrayView<char> to, StringView from) { return from.copy_to(to); }
}  // namespace

bool names_equal(StringView a, StringView b) {
  if (a.ends_with('.')) a = a.without_suffix(1);  // ignore one trailing dot
  if (b.ends_with('.')) b = b.without_suffix(1);
  return a.equals_ignore_case(b);
}

uint32_t encode_name(StringView name, ByteView out) {
  if (name.ends_with('.')) name = name.without_suffix(1);  // an absolute name: drop the trailing dot
  if (name.empty() || name.size() > kMaxTextName) return 0;

  ByteWriter w(out);
  StringView rest = name, label;
  while (true) {  // labels are what lies between the dots, last one included
    const uint64_t dot = rest.find('.');
    label = dot == StringView::npos ? rest : rest.first(dot);
    if (label.empty() || label.size() > 63) return 0;  // empty label ("a..b") or too long
    w.u8(static_cast<uint8_t>(label.size()));
    w.bytes(label.bytes());
    if (dot == StringView::npos) break;
    rest = rest.without_prefix(dot + 1);
  }
  w.u8(0);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t build_query(uint16_t id, StringView name, ByteView out) {
  ByteWriter w(out);
  w.be16(id);
  w.be16(0x0100);  // standard query, recursion desired
  w.be16(1);       // one question
  w.be16(0);
  w.be16(0);
  w.be16(0);
  if (!w.ok()) return 0;
  // Room for the name plus the four bytes (type, class) after it.
  const uint64_t room = out.size() > kHeaderSize + 4 ? out.size() - kHeaderSize - 4 : 0;
  const uint32_t n = encode_name(name, out.subview(kHeaderSize, room));
  if (!n) return 0;
  w.reserve(n);  // the name is already in place
  w.be16(kTypeA);
  w.be16(kClassIn);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

ParseResult parse_response(ConstByteView m, uint16_t id, StringView name, Answer* answer) {
  ByteReader in(m);
  const uint16_t wire_id = in.be16();
  const uint16_t flags = in.be16();
  const uint16_t questions = in.be16();
  const uint16_t answers = in.be16();
  in.skip(4);  // authority and additional record counts
  if (!in.ok() || wire_id != id) return ParseResult::Invalid;
  if (!(flags & 0x8000) || (flags & 0x7800)) return ParseResult::Invalid;  // not a response to a standard query
  if (questions != 1) return ParseResult::Invalid;

  // The question must be the one we asked.
  char qname_storage[kMaxName + 1];
  const ArrayView<char> qname(qname_storage);
  uint64_t o = kHeaderSize;
  if (!read_name(m, o, qname, &o)) return ParseResult::Invalid;
  in.seek(o);
  const uint16_t qtype = in.be16(), qclass = in.be16();
  if (!in.ok()) return ParseResult::Invalid;
  if (!names_equal(StringView(qname.data()), name) || qtype != kTypeA || qclass != kClassIn) return ParseResult::Invalid;
  o = in.position();

  if (flags & 0x0200) return ParseResult::Truncated;
  const uint8_t rcode = flags & 0x0F;
  if (rcode == 3) return ParseResult::NoSuchName;
  if (rcode != 0) return ParseResult::ServerError;

  // Follow the CNAME chain from the name asked for to an A record.
  char target_storage[kMaxName + 1];
  const ArrayView<char> target(target_storage);
  if (!copy_name(target, StringView(qname.data()))) return ParseResult::Invalid;
  uint32_t min_ttl = 0xFFFFFFFF;
  for (uint16_t i = 0; i < answers; ++i) {
    char owner_storage[kMaxName + 1];
    if (!read_name(m, o, ArrayView<char>(owner_storage), &o)) return ParseResult::Invalid;
    in.seek(o);
    const uint16_t type = in.be16(), klass = in.be16();
    const uint32_t ttl = in.be32();
    const uint16_t rdlength = in.be16();
    ConstByteView rdata = in.take(rdlength);  // the record data must lie inside the message
    if (!in.ok()) return ParseResult::Invalid;
    if (klass == kClassIn && names_equal(StringView(owner_storage), StringView(target.data()))) {
      if (type == kTypeA && rdlength == 4) {
        ByteReader address(rdata);
        answer->address = Ipv4Address::read(address);
        answer->ttl = ttl < min_ttl ? ttl : min_ttl;
        return ParseResult::Answer;
      }
      if (type == kTypeCname) {  // the name is an alias: from here on look for the target's records
        uint64_t unused;
        char next_storage[kMaxName + 1];
        // The target name may use compression pointers into the whole message, so it is read from there.
        if (!read_name(m, in.position() - rdlength, ArrayView<char>(next_storage), &unused)) return ParseResult::Invalid;
        if (!copy_name(target, StringView(next_storage))) return ParseResult::Invalid;
        if (ttl < min_ttl) min_ttl = ttl;
      }
    }
    o = in.position();
  }
  return ParseResult::NoAddress;
}

}  // namespace network::dns
