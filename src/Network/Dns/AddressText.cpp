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

// IP addresses as text (declared in IDnsResolver.hpp): what the resolver, URLs and the address text of programs all read and write. Kept apart
// from the resolver so that code that only parses addresses does not need it.
#include "Network/Dns/IDnsResolver.hpp"

namespace network {

bool parse_ipv4(StringView text, Ipv4Address* address) {
  StringView rest = text, part;
  uint32_t value = 0, octets = 0;
  while (rest.next_token('.', &part)) {
    uint64_t octet;
    if (part.size() > 3 || !part.parse_uint(&octet) || octet > 255) return false;  // empty, too long, not digits, or > 255
    value = (value << 8) | static_cast<uint32_t>(octet);
    if (++octets > 4) return false;
  }
  if (octets != 4 || text.ends_with('.')) return false;  // next_token drops a trailing empty label: refuse it here
  if (address) *address = Ipv4Address::from_value(value);
  return true;
}

bool parse_ipv6(StringView text, Ipv6Address* address) {
  if (text.size() < 2 || text.size() > 45) return false;  // ("::" to the longest form with an IPv4 tail)
  uint16_t groups[8] = {};
  uint32_t count = 0;
  int gap = -1;  // where "::" stands (the number of groups before it)
  uint64_t i = 0;
  if (text[0] == ':') {
    if (text[1] != ':') return false;
    gap = 0;
    i = 2;
  }
  while (i < text.size()) {
    // An IPv4 address in the last 32 bits: the rest of the text has a dot.
    bool dotted = false;
    for (uint64_t j = i; j < text.size() && text[j] != ':'; ++j) dotted = dotted || text[j] == '.';
    if (dotted) {
      Ipv4Address v4;
      if (count > 6 || !parse_ipv4(text.substr(i, text.size() - i), &v4)) return false;
      groups[count++] = static_cast<uint16_t>((v4.octets[0] << 8) | v4.octets[1]);
      groups[count++] = static_cast<uint16_t>((v4.octets[2] << 8) | v4.octets[3]);
      i = text.size();
      break;
    }
    uint32_t value = 0, digits = 0;
    for (; i < text.size() && text[i] != ':'; ++i, ++digits) {
      const char c = text[i];
      const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (d < 0 || digits >= 4) return false;
      value = (value << 4) | static_cast<uint32_t>(d);
    }
    if (digits == 0 || count >= 8) return false;
    groups[count++] = static_cast<uint16_t>(value);
    if (i == text.size()) break;
    ++i;  // the ':'
    if (i < text.size() && text[i] == ':') {
      if (gap >= 0) return false;  // only one "::"
      gap = static_cast<int>(count);
      ++i;
    } else if (i == text.size()) {
      return false;  // a trailing single ':'
    }
  }
  if (gap < 0 ? count != 8 : count > 7) return false;
  Ipv6Address a{};
  const uint32_t zeros = 8 - count;
  for (uint32_t g = 0, out = 0; g < count; ++g, ++out) {
    if (gap >= 0 && g == static_cast<uint32_t>(gap)) out += zeros;
    a.bytes[2 * out] = static_cast<uint8_t>(groups[g] >> 8);
    a.bytes[2 * out + 1] = static_cast<uint8_t>(groups[g]);
  }
  if (address) *address = a;
  return true;
}

uint32_t format_ipv6(const Ipv6Address& address, ArrayView<char> out) {
  if (out.size() < 40) return 0;
  // The longest run of zero groups (at least two; the first of equal ones).
  int best = -1, best_length = 1;
  for (int g = 0; g < 8;) {
    if (address.group(g) != 0) {
      ++g;
      continue;
    }
    int end = g;
    while (end < 8 && address.group(end) == 0) ++end;
    if (end - g > best_length) {
      best = g;
      best_length = end - g;
    }
    g = end;
  }
  uint32_t n = 0;
  const char* hex = "0123456789abcdef";
  for (int g = 0; g < 8; ++g) {
    if (g == best) {
      out[n++] = ':';
      if (g == 0) out[n++] = ':';
      g += best_length - 1;
      continue;
    }
    const uint16_t v = address.group(g);
    bool started = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
      const uint32_t digit = (v >> shift) & 0xF;
      if (digit || started || shift == 0) {
        out[n++] = hex[digit];
        started = true;
      }
    }
    if (g < 7) out[n++] = ':';
  }
  out[n] = 0;
  return n;
}

}  // namespace network
