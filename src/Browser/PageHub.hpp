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

// What the pages of a browser share. StorageHub keeps the Web Storage of each origin (one hub for localStorage, shared by every window; one for the
// sessionStorage of each tab). PageHub carries events from one page to the others: a change of storage ("storage" events reach the other pages of the same
// origin, and for sessionStorage of the same tab) and BroadcastChannel messages (same origin). Pages take the events addressed to them when they run.
#pragma once
#include "Browser/DomBindings.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace browser {

class StorageHub {
 public:
  explicit StorageHub(allocators::IAllocator& allocator) : allocator_(allocator), origins_(allocator), fallback_(allocator) {}
  // The storage of an origin ("https://example.com"; "null" for pages without one), made when first asked for.
  StorageArea& area(StringView origin);
  uint32_t origin_count() const { return static_cast<uint32_t>(origins_.count()); }
  void clear() {
    origins_.clear();
    ++clears_;
  }
  // Empties every origin's storage (the areas stay: pages hold references to them).
  void clear_all() {
    for (uint64_t i = 0; i < origins_.count(); ++i) origins_[i]->area.clear();
    ++clears_;
  }
  // For saving: goes up whenever anything in any origin changed; and the origins with their storage.
  uint64_t version() const;
  StringView origin_name(uint32_t i) const { return i < origins_.count() ? origins_[i]->origin.view() : StringView(); }
  StorageArea& area_at(uint32_t i) { return origins_[i]->area; }
  uint64_t total_bytes() const {
    uint64_t sum = 0;
    for (uint64_t i = 0; i < origins_.count(); ++i) sum += origins_[i]->area.bytes();
    return sum;
  }

 private:
  struct Origin {
    Origin(allocators::IAllocator& a, StringView name) : origin(a), area(a) { origin.assign(name); }
    String origin;
    StorageArea area;
  };
  allocators::IAllocator& allocator_;
  List<UniquePointer<Origin>> origins_;
  StorageArea fallback_;
  uint64_t clears_ = 0;
};

struct HubEvent {
  explicit HubEvent(allocators::IAllocator& a) : origin(a), name(a), first(a), second(a), url(a) {}
  enum class Kind : uint8_t { Storage, Broadcast };
  Kind kind = Kind::Storage;
  uint64_t serial = 0;
  uint32_t source = 0;       // the page that made it
  uint64_t group = 0;        // storage: which storage hub (0: localStorage; otherwise the tab's sessionStorage)
  String origin;
  String name;               // storage: the key ("" and key_null for clear()); broadcast: the channel name
  String first, second;      // storage: the old and the new value; broadcast: the message (serialized) and unused
  String url;                // storage: the address of the page that changed it
  bool key_null = false, old_null = false, new_null = false;
};

class PageHub {
 public:
  static constexpr uint32_t kBacklog = 256;
  explicit PageHub(allocators::IAllocator& allocator) : allocator_(allocator), events_(allocator) {}

  // Pages register to get an id (it tells whose events are their own).
  uint32_t add_page() { return ++next_page_; }
  void post(HubEvent&& event);
  uint64_t serial() const { return serial_; }
  // The next event after *seen that was not made by `page`, in its origin and storage group (group 0: any for broadcasts). False when there is none.
  bool next(uint32_t page, StringView origin, uint64_t* seen, HubEvent* out);
  // Are there events a page that has seen up to `seen` has not looked at?
  bool pending(uint32_t page, StringView origin, uint64_t seen) const;

 private:
  allocators::IAllocator& allocator_;
  List<HubEvent> events_;
  uint64_t serial_ = 0;
  uint32_t next_page_ = 0;
};

}  // namespace browser
