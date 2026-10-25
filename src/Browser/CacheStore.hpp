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

// The Cache API's storage: named caches of request / response pairs for each origin, shared by every page, tab and service worker of the browser (the script side,
// in WebPrelude.cpp, does the matching - Vary, the query options - over the entries this keeps). Thread safe: pages run on threads of their own.
#pragma once
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace browser {

class CacheStore {
 public:
  // The limit for the bytes one origin may keep in its caches (QuotaExceededError beyond it), and for one cache's entries.
  static constexpr uint64_t kOriginQuota = 64u << 20;
  static constexpr uint32_t kMaxEntries = 4096, kMaxCaches = 256;

  struct Entry {
    explicit Entry(allocators::IAllocator& a) : method(a), url(a), request_headers(a), status_text(a), headers(a), body(a), response_url(a), response_type(a) {}
    String method, url, request_headers;   // the request: "Name: value\r\n" lines
    uint32_t status = 200;
    String status_text, headers, body, response_url, response_type;   // the response (the body as bytes)
    bool redirected = false;
    uint64_t bytes() const { return method.size() + url.size() + request_headers.size() + status_text.size() + headers.size() + body.size() + response_url.size() + 64; }
  };

  explicit CacheStore(allocators::IAllocator& allocator) : allocator_(allocator), origins_(allocator) {}

  // --- CacheStorage (names keep the order they were made in)
  bool has(StringView origin, StringView name);
  // Makes the cache if it is new; false if there are too many or memory ran out.
  bool open(StringView origin, StringView name);
  bool remove(StringView origin, StringView name);
  void names(StringView origin, List<String>* out);

  // --- Cache
  uint32_t count(StringView origin, StringView name);
  // The entry at `index` (without or with its body); false if there is none.
  bool read(StringView origin, StringView name, uint32_t index, Entry* out, bool with_body);
  // Replaces the entry at `index` (-1: adds a new one). False if the cache is gone, the quota would be passed or memory ran out; *over_quota says which.
  bool write(StringView origin, StringView name, int64_t index, Entry&& entry, bool* over_quota);
  bool erase(StringView origin, StringView name, uint32_t index);

  // --- Accounting and saving
  uint64_t usage(StringView origin);
  uint64_t total_bytes();
  // Goes up with every change.
  uint64_t version();
  void clear_origin(StringView origin);
  void clear_all();
  // Calls visit(origin, cache name, entry) for everything, in order (the lock is held, so the callback must not call back into the store).
  template <class F>
  void for_each(F&& visit) {
    kernel::ScopedMutex lock(mutex_);
    for (uint64_t o = 0; o < origins_.count(); ++o)
      for (uint64_t c = 0; c < origins_[o]->caches.count(); ++c) {
        Cache& cache = *origins_[o]->caches[c];
        if (cache.entries.count() == 0) visit(origins_[o]->origin.view(), cache.name.view(), static_cast<const Entry*>(nullptr));
        for (uint64_t e = 0; e < cache.entries.count(); ++e) visit(origins_[o]->origin.view(), cache.name.view(), &cache.entries[e]);
      }
  }
  // For loading: adds an entry to a cache (made if needed) without the quota check.
  bool restore(StringView origin, StringView name, Entry&& entry);

 private:
  struct Cache {
    Cache(allocators::IAllocator& a, StringView n) : name(a), entries(a) { name.assign(n); }
    String name;
    List<Entry> entries;
  };
  struct Origin {
    Origin(allocators::IAllocator& a, StringView o) : origin(a), caches(a) { origin.assign(o); }
    String origin;
    List<UniquePointer<Cache>> caches;
  };
  Origin* find_origin(StringView origin, bool create);
  Cache* find_cache(StringView origin, StringView name);
  uint64_t usage_locked(const Origin& origin) const;

  allocators::IAllocator& allocator_;
  kernel::Mutex mutex_;
  List<UniquePointer<Origin>> origins_;
  uint64_t version_ = 0;
};

}  // namespace browser
