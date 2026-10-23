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

// Makes the box tree of a document (Box.hpp): styles every element (the cascade and the defaults of its kind), decides what kind of box
// it makes, and arranges the boxes the way CSS 2.1 section 9.2.1.1 asks - a block container holds either only block-level or only
// inline-level boxes, so inline content beside blocks is wrapped in anonymous blocks, white space between blocks makes no box, and an inline
// element that has blocks inside becomes a block itself (the "a wraps a div" case; the real rule splits the inline around the blocks).
//
// Not done here yet: ::before and ::after content and counters, form controls drawn as controls (they come out as boxes of a typical
// size), and a shadow tree's slots beyond what dom::slot_assigned_nodes gives.
#pragma once
#include "Browser/Css.hpp"
#include "Browser/ObjectTypes.hpp"
#include "Dom/Dom.hpp"
#include "Layout/Box.hpp"

namespace layout {

struct BuildOptions {
  browser::IObjectSource* objects = nullptr;   // pictures, videos and canvases (nullptr: they are boxes without a natural size)
  uint32_t max_depth = 200;                    // boxes nested deeper than this are not made (their text is dropped): layout recurses with the tree
  uint64_t max_boxes = 400000;
};

// Builds the tree of `doc` into `out` (replacing what it had). False if memory ran out or the page has no root element (the tree is then
// empty or partial and should not be laid out).
bool build_box_tree(dom::Document& doc, const browser::StyleSheet& sheet, const BuildOptions& options, BoxTree* out);

}  // namespace layout
