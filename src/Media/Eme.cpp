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

#include "Media/Eme.hpp"
#include "Utility/Move.hpp"

namespace media {

namespace {
bool same(const uint8_t* a, const uint8_t* b) {
  uint8_t difference = 0;   // (no early exit: the time does not say how much of a key identifier matched)
  for (int i = 0; i < 16; ++i) difference |= static_cast<uint8_t>(a[i] ^ b[i]);
  return difference == 0;
}
}  // namespace

bool ClearKeyStore::add_key(uint32_t session, const uint8_t kid[16], const uint8_t key[16]) {
  for (uint64_t i = 0; i < entries_.count(); ++i) {
    Entry& e = entries_[i];
    if (e.session != session || !same(e.kid, kid)) continue;
    for (int k = 0; k < 16; ++k) e.key[k] = key[k];
    ++epoch_;
    return true;
  }
  if (entries_.count() >= kMaxKeys) return false;
  Entry e;
  e.session = session;
  for (int k = 0; k < 16; ++k) {
    e.kid[k] = kid[k];
    e.key[k] = key[k];
  }
  if (!entries_.push_back(e)) return false;
  ++epoch_;
  return true;
}

void ClearKeyStore::remove_session(uint32_t session) {
  bool removed = false;
  for (uint64_t i = entries_.count(); i > 0; --i) {
    Entry& e = entries_[i - 1];
    if (e.session != session) continue;
    for (uint8_t& b : e.key) b = 0;   // (gone from memory, not only from the table)
    entries_.remove_at(i - 1);
    removed = true;
  }
  if (removed) ++epoch_;
}

bool ClearKeyStore::has_key(const uint8_t kid[16]) const {
  for (const Entry& e : entries_.view())
    if (same(e.kid, kid)) return true;
  return false;
}

bool ClearKeyStore::key_for(const uint8_t kid[16], uint8_t key[16]) const {
  for (const Entry& e : entries_.view()) {
    if (!same(e.kid, kid)) continue;
    for (int k = 0; k < 16; ++k) key[k] = e.key[k];
    return true;
  }
  return false;
}

void ClearKeyStore::wipe() {
  for (uint64_t i = 0; i < entries_.count(); ++i)
    for (uint8_t& b : entries_[i].key) b = 0;
}

uint32_t EmeHub::create() {
  if (stores_.count() >= kMaxStores) return 0;
  Slot slot;
  slot.id = next_;
  slot.store = UniquePointer<ClearKeyStore>::make(allocator_, allocator_);
  if (!slot.store || !stores_.push_back(util::move(slot))) return 0;
  return next_++;
}

ClearKeyStore* EmeHub::find(uint32_t id) {
  for (uint64_t i = 0; i < stores_.count(); ++i)
    if (stores_[i].id == id) return stores_[i].store.get();
  return nullptr;
}

}  // namespace media
