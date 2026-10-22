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

// What servers have said about offering HTTP/3 (the Alt-Svc header, RFC 7838): "h3=\":443\"; ma=86400" on an https answer means the same origin also speaks
// HTTP/3 over QUIC on that UDP port for the next `ma` seconds. The HTTP client asks this cache before connecting and teaches it from the answers; a server
// whose QUIC attempt failed is left alone for a while so that the next request does not wait for the same timeout. Only the same host is used (an
// alternative on another host is ignored). One cache can be shared by many clients and threads (it is locked inside).
#pragma once
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace network {

class AltSvcCache {
 public:
  static constexpr uint32_t kMaxEntries = 64;
  static constexpr uint64_t kMaxAgeSeconds = 7 * 24 * 3600, kDefaultAgeSeconds = 24 * 3600, kBrokenMs = 10 * 60 * 1000;

  explicit AltSvcCache(allocators::IAllocator& allocator) : allocator_(allocator), entries_(allocator) {}
  AltSvcCache(const AltSvcCache&) = delete;
  AltSvcCache& operator=(const AltSvcCache&) = delete;

  // Takes in the Alt-Svc header of an answer from https://host:port (`now_ms` is the stack's clock). "clear" forgets the origin.
  void learn(StringView host, uint16_t port, StringView header, uint64_t now_ms);
  // The UDP port HTTP/3 can be tried on for the origin, if it is known, has not expired, and has not failed lately.
  bool find(StringView host, uint16_t port, uint64_t now_ms, uint16_t* h3_port) const;
  // A QUIC attempt failed: no more attempts for a while.
  void mark_broken(StringView host, uint16_t port, uint64_t now_ms);
  bool is_broken(StringView host, uint16_t port, uint64_t now_ms) const;
  void forget(StringView host, uint16_t port);
  uint32_t count() const {
    kernel::ScopedMutex lock(mutex_);
    return static_cast<uint32_t>(entries_.count());
  }

 private:
  struct Entry {
    explicit Entry(allocators::IAllocator& a) : host(a) {}
    String host;
    uint16_t origin_port = 0, h3_port = 0;
    uint64_t expires_ms = 0, broken_until_ms = 0;
  };
  void forget_locked(StringView host, uint16_t port);
  int64_t index_of(StringView host, uint16_t port) const;

  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;
  List<Entry> entries_;
};

}  // namespace network
