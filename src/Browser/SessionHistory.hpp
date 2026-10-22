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

// The session history of one browser tab: the list of entries its Back and Forward buttons walk. An entry is an address, the state a page gave it with
// history.pushState() (kept as JSON text, because the entry outlives the page), the scroll position, and which document it belongs to: entries made by
// pushState() and by changing the fragment belong to the document that made them, and moving between entries of the same document does not load anything
// (the page gets a `popstate` event instead); moving to an entry of another document loads that address again.
#pragma once
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace browser {

class SessionHistory {
 public:
  static constexpr uint32_t kMaxEntries = 50;
  static constexpr uint64_t kMaxStateBytes = 2u << 20;

  struct Entry {
    explicit Entry(allocators::IAllocator& a) : address(a), state(a) {}
    String address;
    String state;             // JSON text; meaningful when has_state
    bool has_state = false;
    uint32_t document = 0;    // which document made this entry (0: unknown)
    int32_t scroll = 0;       // the line the page was scrolled to when it was left
  };

  explicit SessionHistory(allocators::IAllocator& allocator) : allocator_(allocator), entries_(allocator) {}

  uint32_t count() const { return static_cast<uint32_t>(entries_.count()); }
  int64_t index() const { return index_; }
  bool empty() const { return entries_.count() == 0; }
  const Entry& at(uint32_t i) const { return entries_[i]; }
  Entry& at(uint32_t i) { return entries_[i]; }
  Entry* current() { return index_ >= 0 ? &entries_[static_cast<uint64_t>(index_)] : nullptr; }
  const Entry* current() const { return index_ >= 0 ? &entries_[static_cast<uint64_t>(index_)] : nullptr; }
  bool can_go(int32_t delta) const;

  // A page was opened (typed, a link, a form): a new entry after the current one; the entries ahead of it are forgotten.
  bool navigate(StringView address, uint32_t document);
  // The current entry now shows this address (a reload, a redirect that was followed, the fragment changed) and belongs to this document.
  void replace_current(StringView address, uint32_t document);
  // history.pushState() / replaceState(): the entry belongs to the document that is shown.
  bool push_state(StringView address, StringView state, bool has_state, bool replace);
  // A fragment change made by the page or a link: a new entry of the same document.
  bool push_fragment(StringView address);
  // Moves by `delta` entries. False (nothing changes) if there is no such entry.
  bool go(int32_t delta);
  // After a document was loaded again, its entries answer to its new number.
  void renumber_document(uint32_t old_document, uint32_t new_document);
  void clear() {
    entries_.clear();
    index_ = -1;
  }

 private:
  void trim();
  allocators::IAllocator& allocator_;
  List<Entry> entries_;
  int64_t index_ = -1;
};

}  // namespace browser
