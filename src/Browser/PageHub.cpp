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

#include "Browser/PageHub.hpp"
#include "Utility/Move.hpp"

namespace browser {

// --- StorageArea -------------------------------------------------------------------------------------------------

bool StorageArea::get(StringView key, StringView* value) const {
  for (uint64_t i = 0; i < keys_.count(); ++i) {
    if (keys_[i].view().equals(key)) {
      *value = values_[i].view();
      return true;
    }
  }
  return false;
}

bool StorageArea::set(StringView key, StringView value) {
  uint64_t total = key.size() + value.size();
  int64_t at = -1;
  for (uint64_t i = 0; i < keys_.count(); ++i) {
    if (keys_[i].view().equals(key)) {
      at = static_cast<int64_t>(i);
      continue;
    }
    total += keys_[i].size() + values_[i].size();
  }
  if (total > (5u << 20) || (at < 0 && keys_.count() >= 10000)) return false;   // (the storage limit: about 5 MB)
  if (at >= 0) return values_[static_cast<uint64_t>(at)].assign(value);
  String k(allocator_), v(allocator_);
  if (!k.assign(key) || !v.assign(value)) return false;
  return keys_.push_back(util::move(k)) && values_.push_back(util::move(v));
}

bool StorageArea::remove(StringView key) {
  for (uint64_t i = 0; i < keys_.count(); ++i) {
    if (keys_[i].view().equals(key)) {
      keys_.remove_at(i);
      values_.remove_at(i);
      return true;
    }
  }
  return false;
}


StorageArea& StorageHub::area(StringView origin) {
  for (uint64_t i = 0; i < origins_.count(); ++i)
    if (origins_[i]->origin.equals(origin)) return origins_[i]->area;
  auto fresh = UniquePointer<Origin>::make(allocator_, allocator_, origin);
  if (!fresh || !origins_.push_back(util::move(fresh))) return fallback_;   // (out of memory: writes go to a scratch area)
  return origins_[origins_.count() - 1]->area;
}

void PageHub::post(HubEvent&& event) {
  event.serial = ++serial_;
  if (events_.count() >= kBacklog) events_.remove_at(0);
  events_.push_back(util::move(event));
}

bool PageHub::next(uint32_t page, StringView origin, uint64_t* seen, HubEvent* out) {
  for (uint64_t i = 0; i < events_.count(); ++i) {
    HubEvent& e = events_[i];
    if (e.serial <= *seen) continue;
    *seen = e.serial;
    if (e.source == page || !e.origin.equals(origin)) continue;
    out->kind = e.kind;
    out->serial = e.serial;
    out->source = e.source;
    out->group = e.group;
    out->origin.assign(e.origin.view());
    out->name.assign(e.name.view());
    out->first.assign(e.first.view());
    out->second.assign(e.second.view());
    out->url.assign(e.url.view());
    out->key_null = e.key_null;
    out->old_null = e.old_null;
    out->new_null = e.new_null;
    return true;
  }
  return false;
}

bool PageHub::pending(uint32_t page, StringView origin, uint64_t seen) const {
  for (uint64_t i = 0; i < events_.count(); ++i) {
    const HubEvent& e = events_[i];
    if (e.serial > seen && e.source != page && e.origin.equals(origin)) return true;
  }
  return false;
}

}  // namespace browser
