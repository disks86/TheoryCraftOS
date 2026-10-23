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

// Gives every box of a tree (Box.hpp) its place and size in CSS pixels for a viewport of a given width. Block layout follows CSS 2.1
// chapter 10 and section 8.3.1 (widths and auto margins, min/max sizes, box-sizing, margin collapsing between siblings, with parents and
// through empty blocks); inline layout follows section 9.4.2 (line boxes, white-space collapsing, greedy line breaking at spaces, after
// hyphens and around atomic inlines, vertical-align baseline/middle/top/bottom, text-align left/center/right/justify, text-indent);
// inline-blocks, floats and replaced elements are sized shrink-to-fit or from their natural size. Flex, grid, tables, floats' exclusion
// of lines, positioning and multi-column come in the next steps of the layout engine; until then their boxes are laid out as blocks.
#pragma once
#include "Layout/Box.hpp"
#include "Layout/TextMeasure.hpp"

namespace layout {

struct LayoutOptions {
  LayoutUnit viewport_width = LayoutUnit::from_px(800);
  LayoutUnit viewport_height = LayoutUnit::from_px(600);
  ITextMeasurer* measurer = nullptr;   // required
  const browser::StyleSheet* sheet = nullptr;   // for lengths that are expressions (min(50%, 400px)); without it their absolute part is used
};

// Lays out the whole tree. The root box is as wide as the viewport and as high as the page. False if memory ran out or no measurer was given.
bool layout_tree(BoxTree& tree, const LayoutOptions& options);

}  // namespace layout
