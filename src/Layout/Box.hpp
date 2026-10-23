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

// The box tree: what a page's elements and text become before they are placed. build_box_tree() (BoxBuilder.hpp) makes it from the DOM
// and the style sheet - one box per element that has one (display:none makes none, display:contents none of its own), one per run of
// text, anonymous blocks around inline content that sits beside blocks - and layout_tree() (Layout.hpp) gives every box its place and
// size in CSS pixels. Painting, hit testing and the geometry the scripts ask for read the result.
//
// Boxes are owned by the tree (a List of UniquePointers, so a Box& stays valid while the tree grows) and refer to each other by BoxId
// (an index into it). A box's position (x, y) is that of its border box relative to its parent box's border box; its size (width,
// height) is the border box's. Margins, borders and padding are kept resolved, in pixels. A block container whose content is inline
// keeps the text it laid out as lines of fragments (see LineInfo and Fragment); the text itself stays in the Text boxes. Inline-level boxes
// that sit in a line (Text, Inline, atomic InlineBlock and Replaced) have no place relative to their parent: an atomic one has its (x, y)
// relative to the border box of the block container that owns the line, and Inline and Text boxes are only their fragments.
#pragma once
#include "Browser/Css.hpp"
#include "Layout/LayoutUnit.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace layout {

using BoxId = uint32_t;
constexpr BoxId kNoBox = 0xFFFFFFFFu;

enum class BoxKind : uint8_t {
  Root,          // the initial containing block: the viewport-sized box everything sits in
  Block,         // a block-level box (flow layout inside, unless the style says flex or grid)
  Inline,        // an inline box: its text and inline children flow in the lines of the block container
  InlineBlock,   // an atomic inline-level box (inline-block, inline-flex, inline-grid, inline-table): laid out like a block, placed like a word
  Text,          // a run of text (the text of a DOM text node)
  LineBreak,     // <br>
  Replaced,      // a picture, video, canvas, svg, iframe or form control: it has a natural size and no children
  Anonymous,     // a block made to hold inline content that sits beside block-level siblings
  Marker,        // the bullet or number of a list item
};

struct Edges {
  LayoutUnit top, right, bottom, left;
  LayoutUnit horizontal() const { return left + right; }
  LayoutUnit vertical() const { return top + bottom; }
};

// What an inline box, a text box or an atomic inline contributed to one line.
struct Fragment {
  BoxId box = kNoBox;
  uint32_t text_offset = 0, text_length = 0;   // Text boxes: the part of the box's text on this line (already white-space collapsed: see Box::text)
  LayoutUnit x, y, width, height;              // relative to the border box of the block container that owns the line
  bool starts_box = false, ends_box = false;   // Inline boxes: this fragment has the box's start (left edge) / end (right edge)
};

// One line box of a block container.
struct LineInfo {
  LayoutUnit x, y, width, height;   // the line box (relative to the container's border box); `x` is where its content starts after alignment
  LayoutUnit baseline;              // distance from the top of the line box to the baseline
  uint32_t first_fragment = 0, fragment_count = 0;
};

struct Box {
  explicit Box(allocators::IAllocator& allocator) : children(allocator), text(allocator), lines(allocator), fragments(allocator) {}
  Box(const Box&) = delete;
  Box& operator=(const Box&) = delete;

  BoxKind kind = BoxKind::Block;
  uint32_t node = 0;                 // the serial of the element or text node it came from (0: anonymous)
  BoxId parent = kNoBox;
  List<BoxId> children;
  browser::Style style;
  // Text boxes: the text as the DOM has it. After layout, fragments point into `text` collapsed by white-space (spaces merged, newlines
  // turned into spaces) - that is kept here instead of the original, since nothing needs the original again. Markers: the marker text.
  String text;

  // --- what the builder found out
  bool inline_content = false;       // a block container whose children are all inline-level: it lays them out in lines
  bool out_of_flow = false;          // position absolute or fixed
  bool floated = false;
  bool establishes_bfc = false;      // starts its own block formatting context (floats and overflow do not escape; margins do not collapse through)
  LayoutUnit natural_width, natural_height;   // Replaced: the size it has by itself (0 when it has none)
  bool has_natural_width = false, has_natural_height = false;
  uint8_t object_kind = 0;           // Replaced: a browser::ObjectKind (0: not a picture, video or canvas: an iframe or a form control)

  // --- tables (TableFixup.cpp sets them from the DOM; Table.cpp lays the table out)
  uint16_t colspan = 1, rowspan = 1;   // cells: the colspan and rowspan attributes (rowspan 0: down to the end of the row group)
  uint8_t group_role = 0;              // row groups: 1 thead (laid out first), 2 tfoot (last)

  // --- the containing block as layout last saw it: what percentages of the box's lengths are of (not known while only intrinsic sizes are worked out)
  LayoutUnit cb_width, cb_height;
  bool cb_width_known = false, cb_height_known = false;

  // --- the result of layout
  LayoutUnit x, y, width, height;    // the border box, relative to the parent's border box
  Edges margin, border, padding;
  List<LineInfo> lines;
  List<Fragment> fragments;
  // Distance from the top of the border box to the baseline the box sits on in a line (inline-blocks: that of their last line; boxes
  // without one have has_baseline false and sit on their bottom margin edge).
  LayoutUnit baseline;
  bool has_baseline = false;
  // The widths the box takes when its content is not wrapped at all (max) and wrapped at every chance (min), margins, borders and padding
  // not included; computed on demand (see Intrinsic.cpp) and kept until the next layout.
  LayoutUnit min_content, max_content;
  bool intrinsic_known = false;

  LayoutUnit content_left() const { return border.left + padding.left; }
  LayoutUnit content_top() const { return border.top + padding.top; }
  LayoutUnit content_width() const { return max(width - border.horizontal() - padding.horizontal(), LayoutUnit()); }
  LayoutUnit content_height() const { return max(height - border.vertical() - padding.vertical(), LayoutUnit()); }
  // A replaced element is block-level when its display says block (img { display: block }); everything else follows its kind.
  bool is_block_level() const {
    return kind == BoxKind::Root || kind == BoxKind::Block || kind == BoxKind::Anonymous || (kind == BoxKind::Replaced && style.display == browser::Display::Block);
  }
  bool is_inline_level() const { return !is_block_level() && kind != BoxKind::Marker; }
};

class BoxTree {
 public:
  explicit BoxTree(allocators::IAllocator& allocator) : allocator_(allocator), boxes_(allocator) {}
  BoxTree(const BoxTree&) = delete;
  BoxTree& operator=(const BoxTree&) = delete;

  allocators::IAllocator& allocator() const { return allocator_; }
  uint64_t count() const { return boxes_.count(); }
  bool empty() const { return boxes_.count() == 0; }
  BoxId root() const { return boxes_.count() ? 0 : kNoBox; }
  // The box with this id (an id that does not exist stops the OS like any bounds violation: ids come from the tree).
  Box& box(BoxId id) { return *boxes_[id]; }
  const Box& box(BoxId id) const { return *boxes_[id]; }

  // Makes a box that is not anyone's child yet; kNoBox if memory ran out.
  BoxId create(BoxKind kind, uint32_t node, const browser::Style& style) {
    UniquePointer<Box> made = UniquePointer<Box>::make(allocator_, allocator_);
    if (!made) return kNoBox;
    made->kind = kind;
    made->node = node;
    made->style = style;
    if (!boxes_.push_back(util::move(made))) return kNoBox;
    return static_cast<BoxId>(boxes_.count() - 1);
  }
  // Puts `child` last among the children of `parent`. False if memory ran out.
  bool append(BoxId parent, BoxId child) {
    if (!box(parent).children.push_back(child)) return false;
    box(child).parent = parent;
    return true;
  }
  void clear() { boxes_.clear(); }

 private:
  allocators::IAllocator& allocator_;
  List<UniquePointer<Box>> boxes_;
};

}  // namespace layout
