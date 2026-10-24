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
    explicit Entry(allocators::IAllocator& a) : address(a), state(a), nav_state(a), title(a) {}
    String address;
    String state;             // JSON text; meaningful when has_state
    bool has_state = false;
    uint32_t document = 0;    // which document made this entry (0: unknown)
    int32_t scroll = 0;       // the pixel row the page was scrolled to when it was left
    bool scroll_saved = false;   // (false: the entry was never left, there is nothing to scroll back to)
    uint64_t key = 0;         // the Navigation API's key: stays with the entry when it is replaced
    uint64_t id = 0;          // and its id: a replaced entry gets a new one
    String nav_state;         // navigation.navigate({ state }) / updateCurrentEntry(): JSON text of the Navigation API's state of the entry
    bool has_nav_state = false;
    String title;             // the title the page had when it was left (the list under Back and Forward)
    bool manual_scroll = false;   // history.scrollRestoration = 'manual' when the entry was left: the browser does not scroll it back
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
  // The current entry is replaced by one for `address` (a reload, or navigating to the address that is already shown): same key, new id, no state.
  void replace_navigation(StringView address, uint32_t document);
  // Moves to the entry at `index`. False if there is none.
  bool go_to(uint32_t index) { return index < entries_.count() && go(static_cast<int32_t>(static_cast<int64_t>(index) - index_)); }
  // The entry with this Navigation API key (-1: none).
  int64_t find_key(uint64_t key) const;
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
  void stamp(Entry& e, bool keep_key) {
    ++next_id_;
    if (!keep_key) e.key = next_id_;
    e.id = next_id_;
  }
  allocators::IAllocator& allocator_;
  List<Entry> entries_;
  int64_t index_ = -1;
  uint64_t next_id_ = 0;
};

}  // namespace browser
