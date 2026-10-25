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

// An owned list of strings for item lists (drop-down entries, menu items).
#pragma once
#include <new>
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace gui {

class StringList {
 public:
  explicit StringList(allocators::IAllocator& allocator) : allocator_(allocator), items_(allocator) {}
  ~StringList() { clear(); }
  StringList(const StringList&) = delete;
  StringList& operator=(const StringList&) = delete;

  // Appends a copy; returns its index or -1 if memory ran out.
  int32_t add(StringView text) {
    void* memory = allocator_.allocate(sizeof(String));
    if (!memory) return -1;
    String* s = new (memory) String(allocator_);
    if (!s->assign(text) || !items_.push_back(s)) {
      s->~String();
      allocator_.deallocate(memory);
      return -1;
    }
    return static_cast<int32_t>(items_.count() - 1);
  }
  // Replaces the text at `index`; false if the index is out of range or memory ran out (the old text is kept then).
  bool set(int32_t index, StringView text) {
    if (index < 0 || static_cast<uint64_t>(index) >= items_.count()) return false;
    return items_[static_cast<uint64_t>(index)]->assign(text);
  }
  void clear() {
    for (uint64_t i = 0; i < items_.count(); ++i) {
      items_[i]->~String();
      allocator_.deallocate(items_[i]);
    }
    items_.clear();
  }
  int32_t count() const { return static_cast<int32_t>(items_.count()); }
  StringView at(int32_t index) const {
    return index >= 0 && static_cast<uint64_t>(index) < items_.count() ? items_[static_cast<uint64_t>(index)]->view()
                                                                         : StringView();
  }

 private:
  allocators::IAllocator& allocator_;
  List<String*> items_;
};

}  // namespace gui
