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
#include "Utility/ByteStream.hpp"
#include "Utility/StringView.hpp"

// The name of an AF_UNIX socket as a program's memory holds it: struct sockaddr_un, which Linux (<sys/un.h>) and Windows (<afunix.h>) lay out the same way -
//   USHORT sun_family = AF_UNIX (1, little-endian), char sun_path[108].
// The length that comes with it decides what the name is (the rules are Linux's, which Windows follows for the forms it has):
//   2 bytes                         unnamed (bind() then picks an abstract name itself: "autobind")
//   sun_path[0] != 0                a path: the bytes up to the first NUL or the end of the length, whichever is first
//   sun_path[0] == 0                an abstract name: every byte after that first NUL up to the length, NULs included (no file behind it)
// The bytes come from the program and are not trusted: every read is bounds-checked.
namespace process {

constexpr uint16_t kAfUnix = 1;      // the same number on Linux and Windows
constexpr uint32_t kUnixPathMax = 108;
constexpr uint32_t kSockaddrUnSize = 2 + kUnixPathMax;

struct UnixName {
  enum class Kind : uint8_t { Unnamed, Path, Abstract };
  Kind kind = Kind::Unnamed;
  uint8_t length = 0;                  // bytes of `bytes` in use: a path without its NUL, an abstract name without the leading NUL
  uint8_t bytes[kUnixPathMax] = {};

  ConstByteView view() const { return ConstByteView(bytes, length); }
  StringView text() const { return StringView(reinterpret_cast<const char*>(bytes), length); }      // (char and uint8_t share their representation)
  bool same_as(const UnixName& other) const {
    if (kind != other.kind || length != other.length) return false;
    for (uint32_t i = 0; i < length; ++i) if (bytes[i] != other.bytes[i]) return false;
    return true;
  }
  static UnixName from(Kind kind, ConstByteView name) {
    UnixName n;
    n.kind = kind;
    n.length = uint8_t(name.size() < kUnixPathMax ? name.size() : kUnixPathMax);
    ByteView(n.bytes, n.length).copy_from(name.first(n.length));
    return n;
  }
};

enum class UnixAddressResult : uint8_t { Ok, Invalid, WrongFamily };

// Reads a sockaddr_un of `bytes.size()` bytes (the program's address length). Invalid: shorter than the family, longer than a sockaddr_un, or a path that is empty
// after its first byte cannot be (a lone NUL is the empty abstract name, which Linux accepts).
inline UnixAddressResult parse_unix_address(ConstByteView bytes, UnixName* out) {
  if (bytes.size() < 2 || bytes.size() > kSockaddrUnSize) return UnixAddressResult::Invalid;
  ByteReader in(bytes);
  if (in.le16() != kAfUnix) return UnixAddressResult::WrongFamily;
  const ConstByteView path = bytes.subview(2, bytes.size() - 2);
  if (path.empty()) { *out = UnixName(); return UnixAddressResult::Ok; }
  if (path[0] == 0) { *out = UnixName::from(UnixName::Kind::Abstract, path.subview(1, path.size() - 1)); return UnixAddressResult::Ok; }
  uint64_t end = 0;
  while (end < path.size() && path[end] != 0) ++end;
  *out = UnixName::from(UnixName::Kind::Path, path.first(end));
  return UnixAddressResult::Ok;
}

// The size of the sockaddr_un for `name` as getsockname()/accept() report it: the family, then the path and its NUL, or the NUL and the abstract name, or nothing.
inline uint32_t unix_address_size(const UnixName& name) {
  return name.kind == UnixName::Kind::Unnamed ? 2u : 2u + 1u + name.length;
}

// Writes the sockaddr_un for `name` into `out`, as much of it as fits (the system calls truncate it like Linux and report the full size, unix_address_size()).
// Returns the bytes written.
inline uint32_t write_unix_address(ByteView out, const UnixName& name) {
  uint8_t full[kSockaddrUnSize + 1] = {};
  ByteWriter w{ByteView(full)};
  w.le16(kAfUnix);
  if (name.kind == UnixName::Kind::Abstract) w.u8(0);
  if (name.kind != UnixName::Kind::Unnamed) w.bytes(name.view());
  if (name.kind == UnixName::Kind::Path) w.u8(0);
  const uint32_t size = unix_address_size(name);
  const uint32_t n = size < out.size() ? size : uint32_t(out.size());
  out.first(n).copy_from(ConstByteView(full, n));
  return n;
}

}  // namespace process
