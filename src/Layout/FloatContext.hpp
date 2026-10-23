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

// The floats of one block formatting context (CSS 2.1 section 9.5): where they are, how much room is left beside them, and where the next
// one goes. All rectangles are margin boxes in the coordinates of the formatting context's root (its border box's top left is 0, 0).
#pragma once
#include "Layout/LayoutUnit.hpp"
#include "ObjectContainer/List.hpp"

namespace layout {

struct FloatRect {
  LayoutUnit x, y, width, height;
  bool left = true;
  LayoutUnit right_edge() const { return x + width; }
  LayoutUnit bottom() const { return y + height; }
};

class FloatContext {
 public:
  explicit FloatContext(allocators::IAllocator& allocator) : floats_(allocator) {}
  FloatContext(const FloatContext&) = delete;
  FloatContext& operator=(const FloatContext&) = delete;

  bool empty() const { return floats_.count() == 0; }

  // The free horizontal space for a box that spans [y, y + height) inside [cb_left, cb_right]: the floats that reach into that range push the
  // left limit right and the right limit left. A height of 0 still looks at the line at y.
  void band(LayoutUnit y, LayoutUnit height, LayoutUnit cb_left, LayoutUnit cb_right, LayoutUnit* left, LayoutUnit* right) const {
    const LayoutUnit end = y + max(height, LayoutUnit::from_raw(1));
    LayoutUnit l = cb_left, r = cb_right;
    for (uint64_t i = 0; i < floats_.count(); ++i) {
      const FloatRect& f = floats_[i];
      if (f.y >= end || f.bottom() <= y) continue;
      if (f.left) l = max(l, f.right_edge());
      else r = min(r, f.x);
    }
    *left = l;
    *right = max(r, l);
  }

  // The smallest bottom edge of a float below `y` (the next place a line or box that does not fit could go), or max() if there is none.
  LayoutUnit next_edge_below(LayoutUnit y) const {
    LayoutUnit best = LayoutUnit::max();
    for (uint64_t i = 0; i < floats_.count(); ++i) {
      const LayoutUnit b = floats_[i].bottom();
      if (b > y && b < best) best = b;
    }
    return best;
  }

  // The lowest bottom edge of the floats on the sides a `clear` names (LayoutUnit::min() when there is none).
  LayoutUnit clear_y(bool left, bool right) const {
    LayoutUnit y = LayoutUnit::min();
    for (uint64_t i = 0; i < floats_.count(); ++i)
      if ((floats_[i].left && left) || (!floats_[i].left && right)) y = max(y, floats_[i].bottom());
    return y;
  }

  LayoutUnit lowest_bottom() const { return clear_y(true, true); }

  // Puts a float of `width` x `height` (margin box) as high as the rules allow (CSS 2.1 9.5.1): not above `min_y`, not above any earlier float,
  // as far left (or right) as possible inside [cb_left, cb_right], and lower when it does not fit beside the floats that are there. A float
  // wider than the whole space goes at the edge and overflows. Returns false if memory ran out.
  bool place(LayoutUnit width, LayoutUnit height, bool left, LayoutUnit cb_left, LayoutUnit cb_right, LayoutUnit min_y, FloatRect* out) {
    LayoutUnit y = max(min_y, last_top_);
    for (uint64_t attempt = 0; attempt <= floats_.count() + 1; ++attempt) {
      LayoutUnit l, r;
      band(y, height, cb_left, cb_right, &l, &r);
      if (r - l >= width) break;
      const LayoutUnit next = next_edge_below(y);
      if (next.is_max()) break;   // nothing to wait for: it goes here and sticks out
      y = next;
    }
    LayoutUnit l, r;
    band(y, height, cb_left, cb_right, &l, &r);
    FloatRect f;
    f.left = left;
    f.width = width;
    f.height = height;
    f.y = y;
    f.x = left ? l : r - width;
    if (!left && f.x < cb_left && r - l < width) f.x = r - width;
    last_top_ = y;
    *out = f;
    return floats_.push_back(f);
  }

 private:
  List<FloatRect> floats_;
  LayoutUnit last_top_ = LayoutUnit::min();
};

}  // namespace layout
