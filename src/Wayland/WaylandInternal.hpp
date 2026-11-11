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

// What the parts of the Wayland compositor (WaylandServer.cpp, WaylandSurfaces.cpp, WaylandInput.cpp) share and nobody else needs.
//
// The wire: every message is a 32-bit object id, then a 32-bit word with the message's size in bytes (header included) in the high half and the opcode in
// the low half, then the arguments in 32-bit words of the host's byte order: int, uint, fixed (24.8), object and new_id are one word; a string is its length
// with the NUL, the bytes and the NUL, padded to a word (length 0: null); an array is its length and the bytes, padded. Descriptors travel beside the bytes
// (SCM_RIGHTS), in the order of the arguments that take them.
#pragma once
#include "Utility/ByteStream.hpp"
#include "Wayland/WaylandServer.hpp"

namespace wayland {

inline int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
inline int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }
inline bool contains(const Rect& r, int32_t x, int32_t y) { return x >= r.x && y >= r.y && int64_t{x} - r.x < r.w && int64_t{y} - r.y < r.h; }
inline Rect unite(const Rect& a, const Rect& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  const int32_t x0 = min32(a.x, b.x), y0 = min32(a.y, b.y);
  const int64_t x1 = a.x + int64_t{a.w} > b.x + int64_t{b.w} ? a.x + int64_t{a.w} : b.x + int64_t{b.w};
  const int64_t y1 = a.y + int64_t{a.h} > b.y + int64_t{b.h} ? a.y + int64_t{a.h} : b.y + int64_t{b.h};
  return {x0, y0, static_cast<int32_t>(x1 - x0), static_cast<int32_t>(y1 - y0)};
}
inline Rect intersect(const Rect& a, const Rect& b) {
  const int32_t x0 = max32(a.x, b.x), y0 = max32(a.y, b.y);
  const int64_t x1 = a.x + int64_t{a.w} < b.x + int64_t{b.w} ? a.x + int64_t{a.w} : b.x + int64_t{b.w};
  const int64_t y1 = a.y + int64_t{a.h} < b.y + int64_t{b.h} ? a.y + int64_t{a.h} : b.y + int64_t{b.h};
  if (x1 <= x0 || y1 <= y0) return {x0, y0, 0, 0};
  return {x0, y0, static_cast<int32_t>(x1 - x0), static_cast<int32_t>(y1 - y0)};
}

// wl_shm formats
constexpr uint32_t kFormatArgb8888 = 0, kFormatXrgb8888 = 1, kFormatAbgr8888 = 0x34324241, kFormatXbgr8888 = 0x34324258;
// error codes
constexpr uint32_t kErrorInvalidObject = 0, kErrorInvalidMethod = 1, kErrorNoMemory = 2, kErrorImplementation = 3;      // wl_display
constexpr uint32_t kShmInvalidFormat = 0, kShmInvalidStride = 1, kShmInvalidFd = 2;
constexpr uint32_t kXdgRole = 0, kXdgDefunctSurfaces = 1, kXdgNotTopmostPopup = 2, kXdgInvalidPopupParent = 3, kXdgInvalidSurfaceState = 4,
                   kXdgInvalidPositioner = 5;
constexpr uint32_t kSurfaceInvalidScale = 0, kSurfaceInvalidTransform = 1, kSurfaceInvalidSize = 2, kSurfaceInvalidOffset = 3;
constexpr uint32_t kSubcompositorBadSurface = 0, kSubcompositorBadParent = 1, kSubsurfaceBadSurface = 0;
// xdg_toplevel states and capabilities
constexpr uint32_t kStateMaximized = 1, kStateFullscreen = 2, kStateResizing = 3, kStateActivated = 4;
// the server's wl_display
constexpr uint32_t kDisplayId = 1;
constexpr uint32_t kLastClientId = 0xFEFFFFFFu;

struct Server::Client {
  explicit Client(allocators::IAllocator& allocator) : in(allocator), out(allocator), fds_in(allocator), fds_out(allocator), objects(allocator) {}
  uint8_t number = 0;
  uint64_t pid = 0;
  bool closing = false;
  ByteBuffer in;              // bytes of a message not complete yet
  ByteBuffer out;             // what the client is owed
  Queue<Fd> fds_in;           // descriptors no request took yet
  List<Fd> fds_out;           // descriptors that go with `out`
  Dictionary<uint32_t, Resource> objects;
};

// The arguments of one request. A malformed argument makes it fail (ok() false): the request is then a protocol error.
class Server::Reader {
 public:
  Reader(ConstByteView body, Client& client) : in_(body), client_(client) {}
  bool ok() const { return ok_ && in_.ok(); }
  uint32_t u() { return in_.le32(); }
  int32_t i() { return static_cast<int32_t>(in_.le32()); }
  int32_t fixed_pixels() { return i() / 256; }      // a 24.8 value, in whole pixels (toward 0)
  uint32_t object() { return u(); }
  // A string (without its NUL); empty and *present false for a null one.
  StringView string(bool* present = nullptr) {
    const uint32_t length = u();
    if (present) *present = length != 0;
    if (length == 0) return StringView();
    const uint64_t padded = (uint64_t{length} + 3) & ~uint64_t{3};
    const ConstByteView bytes = in_.take(padded);
    if (!in_.ok() || bytes[length - 1] != 0) {
      ok_ = false;
      return StringView();
    }
    return StringView(bytes.first(length - 1));
  }
  ConstByteView array() {
    const uint32_t length = u();
    const ConstByteView bytes = in_.take((uint64_t{length} + 3) & ~uint64_t{3});
    return in_.ok() ? bytes.first(length) : ConstByteView();
  }
  // The next descriptor the client sent (null: none came, a protocol error).
  Fd fd() {
    Fd out;
    if (!client_.fds_in.dequeue(&out)) ok_ = false;
    return out;
  }

 private:
  ByteReader in_;
  Client& client_;
  bool ok_ = true;
};

// One event, built in place. Too long (only a string can make it so) and it is dropped.
class Server::Event {
 public:
  static constexpr uint32_t kCapacity = 1024;
  Event(uint32_t object, uint16_t opcode) : object_(object), opcode_(opcode) {}
  Event& u(uint32_t v) {
    if (size_ + 4 > kCapacity) { ok_ = false; return *this; }
    for (uint32_t k = 0; k < 4; ++k) bytes_[size_ + k] = static_cast<uint8_t>(v >> (8 * k));      // (the wire is the host's byte order: little-endian on every CPU here)
    size_ += 4;
    return *this;
  }
  Event& i(int32_t v) { return u(static_cast<uint32_t>(v)); }
  Event& fixed(int32_t pixels) { return i(pixels * 256); }
  Event& string(StringView s) {
    const uint64_t padded = (s.size() + 1 + 3) & ~uint64_t{3};
    if (s.size() >= kCapacity || size_ + 4 + padded > kCapacity) { ok_ = false; return *this; }
    u(static_cast<uint32_t>(s.size() + 1));
    ByteView out(bytes_ + size_, padded);
    out.fill(0);
    out.first(s.size()).copy_from(s.bytes());
    size_ += static_cast<uint32_t>(padded);
    return *this;
  }
  Event& array(ConstByteView a) {
    const uint64_t padded = (a.size() + 3) & ~uint64_t{3};
    if (a.size() >= kCapacity || size_ + 4 + padded > kCapacity) { ok_ = false; return *this; }
    u(static_cast<uint32_t>(a.size()));
    ByteView out(bytes_ + size_, padded);
    out.fill(0);
    out.first(a.size()).copy_from(a);
    size_ += static_cast<uint32_t>(padded);
    return *this;
  }
  Event& fd(const Fd& f) {
    fd_ = f;
    return *this;
  }
  bool ok() const { return ok_; }
  uint32_t object() const { return object_; }
  uint16_t opcode() const { return opcode_; }
  ConstByteView arguments() const { return ConstByteView(bytes_, size_); }
  const Fd& descriptor() const { return fd_; }

 private:
  uint32_t object_;
  uint16_t opcode_;
  uint8_t bytes_[kCapacity];
  uint32_t size_ = 0;
  bool ok_ = true;
  Fd fd_;
};

}  // namespace wayland
