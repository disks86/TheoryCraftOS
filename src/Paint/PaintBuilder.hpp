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

// Turns a laid out box tree into a display list, in the order CSS 2.1 appendix E paints things. Every stacking context (the root, positioned boxes,
// boxes with opacity below 1) paints, in this order: its own background and border; the contexts with a negative z-index; the backgrounds and
// borders of its in-flow blocks, tree order; its floats, each as a unit; its inline content (text, inline boxes' backgrounds, inline-blocks and
// pictures as units); the positioned boxes with z-index 0 or auto in tree order; the contexts with a positive z-index. overflow other than
// visible clips what is inside a box to its padding box (rounded with border-radius); opacity below 1 draws a box and what is in it as one group.
//
// Beyond that: box-shadow (blurred, spread, inset) and text-shadow, transform (2D; 3D is flattened) and filter as groups of operations, clip-path
// basic shapes, conic / repeating gradients with colour hints, url() background layers (the painter asks the object painter), list-style-image,
// outlines with offsets and every border style. Not painted: backdrop-filter, mask, background-clip: text, clip-path path() and url(), filter url(),
// background-attachment, selection and the caret.
#pragma once
#include "Dom/Dom.hpp"
#include "Layout/Box.hpp"
#include "Layout/FontMeasurer.hpp"
#include "Paint/DisplayList.hpp"

namespace paint {

struct PaintOptions {
  layout::FontMeasurer* measurer = nullptr;       // required: the fonts of the text
  const browser::StyleSheet* sheet = nullptr;     // gradients, shadows (without it only colours and borders are drawn)
  const dom::Document* doc = nullptr;             // finds the links (their text is blue when the page does not say) and form controls
};

// Builds the list for `tree` (replacing what `out` had). The page's extent is that of the root box. False if memory ran out or no measurer was given.
bool build_display_list(const layout::BoxTree& tree, const PaintOptions& options, DisplayList* out);

}  // namespace paint
