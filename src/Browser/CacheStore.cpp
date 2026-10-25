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

#include "Browser/CacheStore.hpp"
#include "Utility/Move.hpp"

namespace browser {

CacheStore::Origin* CacheStore::find_origin(StringView origin, bool create) {
  for (uint64_t i = 0; i < origins_.count(); ++i)
    if (origins_[i]->origin.view().equals(origin)) return origins_[i].get();
  if (!create) return nullptr;
  auto made = UniquePointer<Origin>::make(allocator_, allocator_, origin);
  if (!made || !origins_.push_back(util::move(made))) return nullptr;
  return origins_[origins_.count() - 1].get();
}

CacheStore::Cache* CacheStore::find_cache(StringView origin, StringView name) {
  Origin* o = find_origin(origin, false);
  if (!o) return nullptr;
  for (uint64_t i = 0; i < o->caches.count(); ++i)
    if (o->caches[i]->name.view().equals(name)) return o->caches[i].get();
  return nullptr;
}

uint64_t CacheStore::usage_locked(const Origin& origin) const {
  uint64_t sum = 0;
  for (uint64_t c = 0; c < origin.caches.count(); ++c)
    for (uint64_t e = 0; e < origin.caches[c]->entries.count(); ++e) sum += origin.caches[c]->entries[e].bytes();
  return sum;
}

bool CacheStore::has(StringView origin, StringView name) {
  kernel::ScopedMutex lock(mutex_);
  return find_cache(origin, name) != nullptr;
}

bool CacheStore::open(StringView origin, StringView name) {
  kernel::ScopedMutex lock(mutex_);
  if (find_cache(origin, name)) return true;
  Origin* o = find_origin(origin, true);
  if (!o || o->caches.count() >= kMaxCaches) return false;
  auto made = UniquePointer<Cache>::make(allocator_, allocator_, name);
  if (!made || !o->caches.push_back(util::move(made))) return false;
  ++version_;
  return true;
}

bool CacheStore::remove(StringView origin, StringView name) {
  kernel::ScopedMutex lock(mutex_);
  Origin* o = find_origin(origin, false);
  if (!o) return false;
  for (uint64_t i = 0; i < o->caches.count(); ++i)
    if (o->caches[i]->name.view().equals(name)) {
      o->caches.remove_at(i);
      ++version_;
      return true;
    }
  return false;
}

void CacheStore::names(StringView origin, List<String>* out) {
  kernel::ScopedMutex lock(mutex_);
  Origin* o = find_origin(origin, false);
  if (!o) return;
  for (uint64_t i = 0; i < o->caches.count(); ++i) {
    String name(allocator_);
    name.assign(o->caches[i]->name.view());
    out->push_back(util::move(name));
  }
}

uint32_t CacheStore::count(StringView origin, StringView name) {
  kernel::ScopedMutex lock(mutex_);
  Cache* c = find_cache(origin, name);
  return c ? static_cast<uint32_t>(c->entries.count()) : 0;
}

bool CacheStore::read(StringView origin, StringView name, uint32_t index, Entry* out, bool with_body) {
  kernel::ScopedMutex lock(mutex_);
  Cache* c = find_cache(origin, name);
  if (!c || index >= c->entries.count()) return false;
  const Entry& e = c->entries[index];
  out->method.assign(e.method.view());
  out->url.assign(e.url.view());
  out->request_headers.assign(e.request_headers.view());
  out->status = e.status;
  out->status_text.assign(e.status_text.view());
  out->headers.assign(e.headers.view());
  out->response_url.assign(e.response_url.view());
  out->response_type.assign(e.response_type.view());
  out->redirected = e.redirected;
  if (with_body) out->body.assign(e.body.view());
  else out->body.clear();
  return true;
}

bool CacheStore::write(StringView origin, StringView name, int64_t index, Entry&& entry, bool* over_quota) {
  kernel::ScopedMutex lock(mutex_);
  *over_quota = false;
  Origin* o = find_origin(origin, false);
  Cache* c = find_cache(origin, name);
  if (!o || !c) return false;
  const bool replace = index >= 0 && static_cast<uint64_t>(index) < c->entries.count();
  if (!replace && c->entries.count() >= kMaxEntries) {
    *over_quota = true;
    return false;
  }
  const uint64_t old_bytes = replace ? c->entries[static_cast<uint64_t>(index)].bytes() : 0;
  const uint64_t used = usage_locked(*o);
  if (used - old_bytes + entry.bytes() > kOriginQuota) {
    *over_quota = true;
    return false;
  }
  if (replace) c->entries[static_cast<uint64_t>(index)] = util::move(entry);
  else if (!c->entries.push_back(util::move(entry))) return false;
  ++version_;
  return true;
}

bool CacheStore::erase(StringView origin, StringView name, uint32_t index) {
  kernel::ScopedMutex lock(mutex_);
  Cache* c = find_cache(origin, name);
  if (!c || index >= c->entries.count()) return false;
  c->entries.remove_at(index);
  ++version_;
  return true;
}

uint64_t CacheStore::usage(StringView origin) {
  kernel::ScopedMutex lock(mutex_);
  Origin* o = find_origin(origin, false);
  return o ? usage_locked(*o) : 0;
}

uint64_t CacheStore::total_bytes() {
  kernel::ScopedMutex lock(mutex_);
  uint64_t sum = 0;
  for (uint64_t i = 0; i < origins_.count(); ++i) sum += usage_locked(*origins_[i]);
  return sum;
}

uint64_t CacheStore::version() {
  kernel::ScopedMutex lock(mutex_);
  return version_;
}

void CacheStore::clear_origin(StringView origin) {
  kernel::ScopedMutex lock(mutex_);
  for (uint64_t i = 0; i < origins_.count(); ++i)
    if (origins_[i]->origin.view().equals(origin)) {
      origins_.remove_at(i);
      ++version_;
      return;
    }
}

void CacheStore::clear_all() {
  kernel::ScopedMutex lock(mutex_);
  origins_.clear();
  ++version_;
}

bool CacheStore::restore(StringView origin, StringView name, Entry&& entry) {
  kernel::ScopedMutex lock(mutex_);
  Origin* o = find_origin(origin, true);
  if (!o) return false;
  Cache* c = find_cache(origin, name);
  if (!c) {
    auto made = UniquePointer<Cache>::make(allocator_, allocator_, name);
    if (!made || !o->caches.push_back(util::move(made))) return false;
    c = o->caches[o->caches.count() - 1].get();
  }
  return c->entries.push_back(util::move(entry));
}

}  // namespace browser
