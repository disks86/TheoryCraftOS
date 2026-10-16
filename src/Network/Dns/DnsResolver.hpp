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
#include "Allocators/IAllocator.hpp"
#include "Network/Dns/IDnsResolver.hpp"
#include "Network/INetworkStack.hpp"
#include "ObjectContainer/List.hpp"

namespace network {

// The default IDnsResolver. Uses the stack's UDP and TCP sockets to reach the
// DNS server in the stack's current configuration.
class DnsResolver final : public IDnsResolver {
 public:
  static constexpr uint32_t kMaxCacheEntries = 32;
  static constexpr uint32_t kUdpAttempts = 3;        // tried after 0 s, 1 s and 3 s
  static constexpr uint32_t kTcpTimeoutMs = 5000;
  static constexpr uint32_t kNegativeTtlSeconds = 30;
  static constexpr uint32_t kMaxTtlSeconds = 3600;

  DnsResolver(INetworkStack& stack, allocators::IAllocator& allocator)
      : stack_(stack), allocator_(allocator), queries_(allocator), cache_(allocator) {}

  SharedPointer<IDnsQuery> query(const char* name) override;
  void clear_cache() override { cache_.clear(); }
  uint32_t cache_size() const override { return static_cast<uint32_t>(cache_.count()); }

  // Sends, retries and receives for the queries in flight. Called by the stack's poll().
  void poll();

 private:
  struct Query;
  struct CacheEntry {
    char name[64];  // names longer than this are simply not cached
    bool found;     // false: a cached "no such name"
    Ipv4Address address;
    uint64_t expires_ms;
  };

  void step(Query& query);
  void finish(Query& query, DnsStatus status);
  void remember(const char* name, bool found, Ipv4Address address, uint32_t ttl_seconds);
  const CacheEntry* lookup(const char* name);
  uint32_t random();
  SharedPointer<IUdpSocket> open_random_socket();

  INetworkStack& stack_;
  allocators::IAllocator& allocator_;
  List<SharedPointer<Query>> queries_;
  List<CacheEntry> cache_;
  uint32_t random_state_ = 0x2545F491;
  uint32_t counter_ = 0;
};

}  // namespace network
