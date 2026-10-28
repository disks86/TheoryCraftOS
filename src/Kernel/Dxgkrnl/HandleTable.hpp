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

// A process's table of handles to one kind of kernel object. A handle is the slot number plus a generation plus the kind of object: the low 16 bits are slot + 1, the next
// 12 the number of times the slot has been used (never 0), the top 4 the table's kind. So a handle that was closed is not taken for a later object in the same slot, and a
// handle of one kind (a device) is never valid in the table of another (an allocation), as all of a process's handles share one namespace in D3DKMT. The table holds a
// reference to each object.
#pragma once
#include "Kernel/Dxgkrnl/DxgTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"

namespace dxgk {

template <class T>
class HandleTable {
 public:
  static constexpr uint32_t kMaxHandles = 0xFFFE;
  static constexpr uint16_t kMaxGeneration = 0xFFF;

  // `kind` is 1..15, different for each table of a process.
  HandleTable(allocators::IAllocator& allocator, uint32_t kind) : slots_(allocator), free_(allocator), kind_(kind & 0xF) {}

  uint32_t count() const { return live_; }

  // The handle of the new entry; 0 if the table is full or memory ran out.
  Handle insert(const SharedPointer<T>& object) {
    if (!object) return kNullHandle;
    uint32_t slot;
    if (free_.count()) {
      slot = free_[free_.count() - 1];
      free_.remove_at(free_.count() - 1);
    } else {
      if (slots_.count() >= kMaxHandles) return kNullHandle;
      if (!slots_.push_back(Slot{})) return kNullHandle;
      slot = static_cast<uint32_t>(slots_.count() - 1);
    }
    Slot& s = slots_[slot];
    s.object = object;
    ++live_;
    return make_handle(s.generation, slot);
  }

  // The object a handle names, or a null pointer for 0, a stale handle or one out of range.
  SharedPointer<T> get(Handle handle) const {
    const Slot* s = find(handle);
    return s ? s->object : SharedPointer<T>();
  }
  T* raw(Handle handle) const {
    const Slot* s = find(handle);
    return s ? s->object.get() : nullptr;
  }

  // Drops the table's reference; false if the handle is not valid.
  bool remove(Handle handle) {
    const uint32_t slot = (handle & 0xFFFF) - 1;
    Slot* s = find(handle);
    if (!s) return false;
    s->object = SharedPointer<T>();
    s->generation = s->generation >= kMaxGeneration ? 1 : static_cast<uint16_t>(s->generation + 1);   // (0 is skipped on wrap-around)
    --live_;
    free_.push_back(slot);   // (if this fails the slot is simply never reused)
    return true;
  }

  // The handle and object of every live entry, in slot order. The callback may not insert or remove.
  template <class F>
  void for_each(F&& callback) const {
    for (uint64_t i = 0; i < slots_.count(); ++i) {
      const Slot& s = slots_[i];
      if (s.object) callback(make_handle(s.generation, static_cast<uint32_t>(i)), s.object);
    }
  }

 private:
  struct Slot {
    SharedPointer<T> object;
    uint16_t generation = 1;
  };
  Handle make_handle(uint32_t generation, uint32_t slot) const { return (kind_ << 28) | (generation << 16) | (slot + 1); }
  const Slot* find(Handle handle) const {
    const uint32_t index = handle & 0xFFFF;
    if (index == 0 || index > slots_.count() || (handle >> 28) != kind_) return nullptr;
    const Slot& s = slots_[index - 1];
    if (!s.object || s.generation != ((handle >> 16) & 0xFFF)) return nullptr;
    return &s;
  }
  Slot* find(Handle handle) {
    const uint32_t index = handle & 0xFFFF;
    if (index == 0 || index > slots_.count() || (handle >> 28) != kind_) return nullptr;
    Slot& s = slots_[index - 1];
    if (!s.object || s.generation != ((handle >> 16) & 0xFFF)) return nullptr;
    return &s;
  }

  List<Slot> slots_;
  List<uint32_t> free_;
  uint32_t live_ = 0;
  uint32_t kind_;
};

}  // namespace dxgk
