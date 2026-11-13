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

// What is under a point of the page: the topmost thing in paint order, with the clips of overflow honoured (what a clip hides cannot be clicked), the
// element it belongs to (text counts for the element it is in), the link that element is in, and the cursor it asks for.
#pragma once
#include "Dom/Dom.hpp"
#include "Paint/DisplayList.hpp"
#include "ObjectContainer/List.hpp"

namespace paint {

struct HitResult {
  uint32_t node = 0;        // the serial of the element under the point (0: nothing but the page)
  uint32_t link = 0;        // the serial of the <a href> that element is in (or is); 0: none
  uint8_t cursor = 0;       // a browser::Cursor: what the element asks for (0 auto)
  bool text = false;        // the point is on text
  bool object = false;      // the point is on a picture, video or canvas
  IRect rect;               // where the thing found is, in page pixels (for an object: its rectangle, to tell where in it the click was)
};

// Looks for what is at page position (x, y). `doc` finds the element a text belongs to and the link around it (nullptr: neither is looked for).
// False if there is nothing there.
bool hit_test(const DisplayList& list, const dom::Document* doc, int32_t x, int32_t y, HitResult* out);

// What document.elementFromPoint() looks through: the part of a display list hit testing needs (the clips, the transforms and the ops elements answer for, in
// paint order), kept after the list itself has gone to the window. A few dozen bytes per op instead of the whole list.
class HitMap {
 public:
  explicit HitMap(allocators::IAllocator& allocator) : allocator_(allocator), entries_(allocator), transforms_(allocator), shapes_(allocator) {}
  HitMap(const HitMap&) = delete;
  HitMap& operator=(const HitMap&) = delete;

  // Keeps what `list` says about hits (what was there before is dropped). False if memory ran out: the map is then empty.
  [[nodiscard]] bool build(const DisplayList& list);
  void clear();
  bool empty() const { return entries_.count() == 0; }

  // The serials of the nodes whose ops are under page position (x, y), front to back (each once; a text answers with the serial of its text node, which the
  // caller maps to its element). Clips and transforms are honoured as for hit_test(). False if memory ran out.
  [[nodiscard]] bool nodes_at(int32_t x, int32_t y, List<uint32_t>* out) const;

 private:
  struct Entry {
    OpKind kind = OpKind::Hit;
    IRect rect;
    uint32_t node = 0;
    uint32_t data = 0;   // PushTransform: an index into transforms_; PushClipPath: into shapes_
  };
  friend struct HitMapSource;
  allocators::IAllocator& allocator_;
  List<Entry> entries_;
  List<Affine> transforms_;
  List<ClipShape> shapes_;
};

}  // namespace paint
