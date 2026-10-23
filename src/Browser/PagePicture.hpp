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

// A page as pixels: the box tree laid out for a viewport and the display list painted from it. Page::render_picture() (Page.hpp) makes one; the page view
// draws it, scrolls it and finds what is under the pointer in it.
#pragma once
#include "Dom/Dom.hpp"
#include "Layout/Box.hpp"
#include "Paint/DisplayList.hpp"

namespace browser {

class PagePicture {
 public:
  explicit PagePicture(allocators::IAllocator& allocator) : tree(allocator), list(allocator) {}
  PagePicture(const PagePicture&) = delete;
  PagePicture& operator=(const PagePicture&) = delete;

  layout::BoxTree tree;
  paint::DisplayList list;
  const dom::Document* document = nullptr;   // the page's tree (finds the element a text belongs to, and links); valid while the page is
  int32_t viewport_width = 0;                // the width the page was laid out for, in pixels

  int32_t width() const { return list.width(); }
  int32_t height() const { return list.height(); }
};

}  // namespace browser
