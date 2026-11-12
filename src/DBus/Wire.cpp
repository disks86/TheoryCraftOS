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

#include "DBus/Wire.hpp"

namespace dbus {

namespace {

bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Elements separated by dots, at least two, each non-empty: `first` says which characters may start an element, `rest` which may follow.
template <class First, class Rest>
bool dotted(StringView name, First first, Rest rest) {
  uint32_t elements = 0;
  uint64_t length = 0;
  for (char c : name) {
    if (c == '.') {
      if (length == 0) return false;
      ++elements;
      length = 0;
      continue;
    }
    if (length == 0 ? !first(c) : !rest(c)) return false;
    ++length;
  }
  if (length == 0) return false;
  return elements + 1 >= 2;
}

bool fixed_size(char c, uint32_t* size) {
  switch (c) {
    case 'y': *size = 1; return true;
    case 'n': case 'q': *size = 2; return true;
    case 'i': case 'u': *size = 4; return true;
    case 'x': case 't': case 'd': *size = 8; return true;
    default: return false;
  }
}

bool complete_type_at(StringView sig, uint64_t at, uint64_t* end, uint32_t arrays, uint32_t structs) {
  char c = 0;
  if (!sig.try_get(at, &c)) return false;
  if (is_basic_type(c) || c == 'v') {
    *end = at + 1;
    return true;
  }
  if (c == 'a') {
    if (arrays + 1 > kMaxSignatureDepth) return false;
    char next = 0;
    if (!sig.try_get(at + 1, &next)) return false;
    if (next == '{') {
      if (structs + 1 > kMaxSignatureDepth) return false;
      char key = 0;
      if (!sig.try_get(at + 2, &key) || !is_basic_type(key)) return false;
      uint64_t value_end = 0;
      if (!complete_type_at(sig, at + 3, &value_end, arrays + 1, structs + 1)) return false;
      char close = 0;
      if (!sig.try_get(value_end, &close) || close != '}') return false;
      *end = value_end + 1;
      return true;
    }
    return complete_type_at(sig, at + 1, end, arrays + 1, structs);
  }
  if (c == '(') {
    if (structs + 1 > kMaxSignatureDepth) return false;
    uint64_t p = at + 1;
    uint32_t members = 0;
    for (;;) {
      char m = 0;
      if (!sig.try_get(p, &m)) return false;
      if (m == ')') break;
      if (!complete_type_at(sig, p, &p, arrays, structs + 1)) return false;
      ++members;
    }
    if (members == 0) return false;
    *end = p + 1;
    return true;
  }
  return false;
}

// Checks (and steps over) the one value of the complete type at sig[at]; *end: the position after that type in the signature.
bool validate_value(Reader& r, StringView sig, uint64_t at, uint64_t* end, uint32_t depth, uint32_t fds) {
  char c = 0;
  if (!sig.try_get(at, &c)) return false;
  *end = at + 1;
  switch (c) {
    case 'y': { uint8_t v; return r.u8(&v); }
    case 'b': { bool v; return r.boolean(&v); }
    case 'n': case 'q': { uint16_t v; return r.u16(&v); }
    case 'i': case 'u': { uint32_t v; return r.u32(&v); }
    case 'h': { uint32_t v; return r.u32(&v) && v < fds; }
    case 'x': case 't': case 'd': { uint64_t v; return r.u64(&v); }
    case 's': { StringView v; return r.string(&v); }
    case 'o': { StringView v; return r.object_path(&v); }
    case 'g': { StringView v; return r.signature(&v); }
    case 'v': {
      if (depth + 1 > kMaxValueDepth) return false;
      StringView inner;
      if (!r.signature(&inner) || !single_complete_type(inner)) return false;
      uint64_t inner_end = 0;
      return validate_value(r, inner, 0, &inner_end, depth + 1, fds);
    }
    case 'a': {
      if (depth + 1 > kMaxValueDepth) return false;
      uint64_t array_type_end = 0;      // (the whole array type: a "{...}" element is only a type after its 'a')
      if (!complete_type(sig, at, &array_type_end)) return false;
      *end = array_type_end;
      const char element = sig[at + 1];
      uint64_t array_end = 0;
      if (!r.begin_array(element == '{' ? 8 : alignment_of(element), &array_end)) return false;
      uint32_t size = 0;
      if (fixed_size(element, &size)) {      // (a run of plain numbers: only the length says anything)
        const uint64_t length = array_end - r.position();
        return length % size == 0 && r.skip(length);
      }
      while (r.position() < array_end) {
        uint64_t ignored = 0;
        if (!validate_value(r, sig, at + 1, &ignored, depth + 1, fds)) return false;
      }
      return r.position() == array_end;
    }
    case '(': {
      if (depth + 1 > kMaxValueDepth || !r.align(8)) return false;
      uint64_t p = at + 1;
      while (sig[p] != ')')
        if (!validate_value(r, sig, p, &p, depth + 1, fds)) return false;
      *end = p + 1;
      return true;
    }
    case '{': {
      if (depth + 1 > kMaxValueDepth || !r.align(8)) return false;
      uint64_t p = at + 1;
      if (!validate_value(r, sig, p, &p, depth + 1, fds) || !validate_value(r, sig, p, &p, depth + 1, fds)) return false;
      *end = p + 1;      // (the '}')
      return true;
    }
    default: return false;
  }
}

}  // namespace

bool valid_utf8(ConstByteView bytes) {
  uint64_t i = 0;
  const uint64_t n = bytes.size();
  while (i < n) {
    const uint8_t b = bytes[i];
    if (b < 0x80) { ++i; continue; }
    uint32_t need = 0, cp = 0, min = 0;
    if ((b & 0xE0) == 0xC0) { need = 1; cp = b & 0x1F; min = 0x80; }
    else if ((b & 0xF0) == 0xE0) { need = 2; cp = b & 0x0F; min = 0x800; }
    else if ((b & 0xF8) == 0xF0) { need = 3; cp = b & 0x07; min = 0x10000; }
    else return false;
    if (n - i - 1 < need) return false;
    for (uint32_t k = 1; k <= need; ++k) {
      const uint8_t cont = bytes[i + k];
      if ((cont & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cont & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += need + 1;
  }
  return true;
}

bool valid_object_path(StringView path) {
  if (path.empty() || path[0] != '/') return false;
  if (path.size() == 1) return true;
  if (path.back() == '/') return false;
  char previous = '/';
  for (uint64_t i = 1; i < path.size(); ++i) {
    const char c = path[i];
    if (c == '/') {
      if (previous == '/') return false;
    } else if (!is_alpha(c) && !is_digit(c)) {
      return false;
    }
    previous = c;
  }
  return true;
}

bool valid_interface_name(StringView name) {
  if (name.empty() || name.size() > kMaxNameBytes) return false;
  return dotted(name, [](char c) { return is_alpha(c); }, [](char c) { return is_alpha(c) || is_digit(c); });
}

bool valid_member_name(StringView name) {
  if (name.empty() || name.size() > kMaxNameBytes || is_digit(name[0])) return false;
  for (char c : name)
    if (!is_alpha(c) && !is_digit(c)) return false;
  return true;
}

bool valid_bus_name(StringView name) {
  if (name.empty() || name.size() > kMaxNameBytes) return false;
  auto element = [](char c) { return is_alpha(c) || is_digit(c) || c == '-'; };
  if (name[0] == ':') return dotted(name.without_prefix(1), element, element);
  return dotted(name, [](char c) { return is_alpha(c) || c == '-'; }, element);
}

bool is_basic_type(char c) {
  switch (c) {
    case 'y': case 'b': case 'n': case 'q': case 'i': case 'u': case 'x': case 't': case 'd': case 'h': case 's': case 'o': case 'g': return true;
    default: return false;
  }
}

uint32_t alignment_of(char c) {
  switch (c) {
    case 'n': case 'q': return 2;
    case 'b': case 'i': case 'u': case 'h': case 's': case 'o': case 'a': return 4;
    case 'x': case 't': case 'd': case '(': case '{': return 8;
    default: return 1;      // y, g, v
  }
}

bool complete_type(StringView signature, uint64_t at, uint64_t* end) { return complete_type_at(signature, at, end, 0, 0); }

bool valid_signature(StringView signature) {
  if (signature.size() > kMaxSignatureBytes) return false;
  uint64_t p = 0;
  while (p < signature.size())
    if (!complete_type(signature, p, &p)) return false;
  return true;
}

bool single_complete_type(StringView signature) {
  uint64_t end = 0;
  return signature.size() <= kMaxSignatureBytes && complete_type(signature, 0, &end) && end == signature.size();
}

// --- Reader

bool Reader::take(uint64_t count, ConstByteView* out) {
  if (failed_ || count > data_.size() - position_) return fail();
  *out = data_.subview(position_, count);
  position_ += count;
  return true;
}

bool Reader::skip(uint64_t count) {
  ConstByteView ignored;
  return take(count, &ignored);
}

bool Reader::align(uint32_t alignment) {
  if (failed_) return false;
  const uint64_t padding = (alignment - position_ % alignment) % alignment;
  ConstByteView pad;
  if (!take(padding, &pad)) return false;
  for (uint8_t b : pad)
    if (b != 0) return fail();
  return true;
}

bool Reader::u8(uint8_t* out) {
  ConstByteView b;
  if (!take(1, &b)) return false;
  *out = b[0];
  return true;
}

bool Reader::u16(uint16_t* out) {
  ConstByteView b;
  if (!align(2) || !take(2, &b)) return false;
  *out = big_endian_ ? uint16_t(b[0] << 8 | b[1]) : uint16_t(b[1] << 8 | b[0]);
  return true;
}

bool Reader::u32(uint32_t* out) {
  ConstByteView b;
  if (!align(4) || !take(4, &b)) return false;
  uint32_t v = 0;
  for (uint32_t i = 0; i < 4; ++i) v |= uint32_t(b[big_endian_ ? 3 - i : i]) << (8 * i);
  *out = v;
  return true;
}

bool Reader::u64(uint64_t* out) {
  ConstByteView b;
  if (!align(8) || !take(8, &b)) return false;
  uint64_t v = 0;
  for (uint32_t i = 0; i < 8; ++i) v |= uint64_t(b[big_endian_ ? 7 - i : i]) << (8 * i);
  *out = v;
  return true;
}

bool Reader::boolean(bool* out) {
  uint32_t v = 0;
  if (!u32(&v)) return false;
  if (v > 1) return fail();
  *out = v == 1;
  return true;
}

bool Reader::string(StringView* out) {
  uint32_t length = 0;
  ConstByteView bytes, nul;
  if (!u32(&length) || !take(length, &bytes) || !take(1, &nul)) return false;
  if (nul[0] != 0 || !valid_utf8(bytes)) return fail();
  for (uint8_t b : bytes)
    if (b == 0) return fail();
  *out = StringView(bytes);
  return true;
}

bool Reader::object_path(StringView* out) {
  if (!string(out)) return false;
  if (!valid_object_path(*out)) return fail();
  return true;
}

bool Reader::signature(StringView* out) {
  uint8_t length = 0;
  ConstByteView bytes, nul;
  if (!u8(&length) || !take(length, &bytes) || !take(1, &nul)) return false;
  *out = StringView(bytes);
  if (nul[0] != 0 || !valid_signature(*out)) return fail();
  return true;
}

bool Reader::begin_array(uint32_t element_alignment, uint64_t* end) {
  uint32_t length = 0;
  if (!u32(&length) || !align(element_alignment)) return false;
  if (length > kMaxArrayBytes || length > data_.size() - position_) return fail();
  *end = position_ + length;
  return true;
}

// --- Writer

void Writer::put(uint64_t v, uint32_t bytes) {
  uint8_t b[8] = {};
  for (uint32_t i = 0; i < bytes; ++i) b[big_endian_ ? bytes - 1 - i : i] = uint8_t(v >> (8 * i));
  if (!out_.append(ConstByteView(b, bytes))) ok_ = false;
}

void Writer::pad(uint32_t alignment) {
  const uint64_t padding = (alignment - (out_.size() - base_) % alignment) % alignment;
  if (padding && !out_.put_zeros(padding)) ok_ = false;
}

void Writer::u8(uint8_t v) { put(v, 1); }
void Writer::u16(uint16_t v) { pad(2); put(v, 2); }
void Writer::u32(uint32_t v) { pad(4); put(v, 4); }
void Writer::u64(uint64_t v) { pad(8); put(v, 8); }

void Writer::raw(ConstByteView bytes) {
  if (!out_.append(bytes)) ok_ = false;
}

void Writer::string(StringView s) {
  if (s.size() > kMaxMessageBytes) { ok_ = false; return; }
  u32(uint32_t(s.size()));
  raw(s.bytes());
  u8(0);
}

void Writer::signature(StringView s) {
  if (s.size() > kMaxSignatureBytes) { ok_ = false; return; }
  u8(uint8_t(s.size()));
  raw(s.bytes());
  u8(0);
}

Writer::ArrayMark Writer::begin_array(uint32_t element_alignment) {
  ArrayMark mark;
  pad(4);
  mark.length_at = out_.size();
  put(0, 4);
  pad(element_alignment);
  mark.start = out_.size();
  return mark;
}

void Writer::end_array(const ArrayMark& mark) {
  const uint64_t length = out_.size() - mark.start;
  if (length > kMaxArrayBytes || mark.length_at + 4 > out_.size()) { ok_ = false; return; }
  for (uint32_t i = 0; i < 4; ++i) out_.patch8(mark.length_at + (big_endian_ ? 3 - i : i), uint8_t(length >> (8 * i)));
}

// --- bodies

bool read_value(Reader& r, StringView type, uint32_t unix_fds) {
  uint64_t end = 0;
  return single_complete_type(type) && validate_value(r, type, 0, &end, 0, unix_fds);
}

bool validate_body(ConstByteView body, StringView signature, bool big_endian, uint32_t unix_fds) {
  if (!valid_signature(signature)) return false;
  Reader r(body, big_endian);
  uint64_t p = 0;
  while (p < signature.size())
    if (!validate_value(r, signature, p, &p, 0, unix_fds)) return false;
  return r.at_end() && !r.failed();
}

void string_args(ConstByteView body, StringView signature, bool big_endian, uint32_t wanted, StringArgs* out) {
  *out = StringArgs();
  if (wanted > StringArgs::kMax) wanted = StringArgs::kMax;
  Reader r(body, big_endian);
  uint64_t p = 0;
  while (p < signature.size() && out->count < wanted) {
    const char c = signature[p];
    const uint32_t i = out->count++;
    if (c == 's' || c == 'o') {
      StringView v;
      if (!(c == 's' ? r.string(&v) : r.object_path(&v))) return;
      out->value[i] = v;
      out->kind[i] = c;
      ++p;
    } else if (!validate_value(r, signature, p, &p, 0, UINT32_MAX)) {
      return;
    }
  }
}

}  // namespace dbus
