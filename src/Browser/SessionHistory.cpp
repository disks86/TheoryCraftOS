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

#include "Browser/SessionHistory.hpp"
#include "Utility/Move.hpp"

namespace browser {

bool SessionHistory::can_go(int32_t delta) const {
  const int64_t target = index_ + delta;
  return index_ >= 0 && target >= 0 && target < static_cast<int64_t>(entries_.count());
}

void SessionHistory::trim() {
  while (entries_.count() > kMaxEntries) {
    entries_.remove_at(0);
    --index_;
  }
  if (index_ < 0 && !entries_.count()) index_ = -1;
}

bool SessionHistory::navigate(StringView address, uint32_t document) {
  while (static_cast<int64_t>(entries_.count()) > index_ + 1) entries_.remove_at(entries_.count() - 1);
  Entry e(allocator_);
  if (!e.address.assign(address)) return false;
  e.document = document;
  if (!entries_.push_back(util::move(e))) return false;
  ++index_;
  trim();
  return true;
}

void SessionHistory::replace_current(StringView address, uint32_t document) {
  if (index_ < 0) {
    navigate(address, document);
    return;
  }
  Entry& e = entries_[static_cast<uint64_t>(index_)];
  e.address.assign(address);
  e.document = document;
}

bool SessionHistory::push_state(StringView address, StringView state, bool has_state, bool replace) {
  if (index_ < 0) return false;
  if (state.size() > kMaxStateBytes) return false;
  const uint32_t document = entries_[static_cast<uint64_t>(index_)].document;
  if (replace) {
    Entry& e = entries_[static_cast<uint64_t>(index_)];
    e.address.assign(address);
    e.state.assign(state);
    e.has_state = has_state;
    return true;
  }
  while (static_cast<int64_t>(entries_.count()) > index_ + 1) entries_.remove_at(entries_.count() - 1);
  Entry e(allocator_);
  if (!e.address.assign(address) || !e.state.assign(state)) return false;
  e.has_state = has_state;
  e.document = document;
  if (!entries_.push_back(util::move(e))) return false;
  ++index_;
  trim();
  return true;
}

bool SessionHistory::push_fragment(StringView address) { return push_state(address, StringView(), false, false); }

bool SessionHistory::go(int32_t delta) {
  if (!can_go(delta)) return false;
  index_ += delta;
  return true;
}

void SessionHistory::renumber_document(uint32_t old_document, uint32_t new_document) {
  for (uint64_t i = 0; i < entries_.count(); ++i)
    if (entries_[i].document == old_document) entries_[i].document = new_document;
}

}  // namespace browser
