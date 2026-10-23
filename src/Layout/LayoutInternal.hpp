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

// The layout engine's working parts, shared by Layout.cpp (blocks, replaced boxes, sizes), Inline.cpp (lines) and Intrinsic.cpp (the
// widths a box wants). Not for use outside src/Layout.
#pragma once
#include "Layout/FloatContext.hpp"
#include "Layout/Layout.hpp"

namespace layout {

// The margin a box contributes to a collapse: the largest positive margin and the most negative one. Adjoining margins collapse to
// their sum (a 20px and a -8px margin make 12px; 20px and 30px make 30px).
struct MarginPair {
  LayoutUnit positive, negative;
  void add(LayoutUnit m) {
    if (m >= LayoutUnit()) positive = max(positive, m);
    else negative = min(negative, m);
  }
  void add(const MarginPair& o) {
    positive = max(positive, o.positive);
    negative = min(negative, o.negative);
  }
  LayoutUnit sum() const { return positive + negative; }
};

// One piece of inline content in the order the lines are made from. Text is cut into words and spaces; the edges of inline boxes and the
// atomic inlines (inline-blocks, pictures) are tokens too, so that lines can be broken between them.
struct Token {
  enum class Type : uint8_t { Word, Space, Open, Close, Atomic, Break, Float, Abs };
  Type type = Type::Word;
  BoxId box = kNoBox;
  uint32_t offset = 0, length = 0;   // Word and Space: the part of the box's (collapsed) text
  LayoutUnit width;                  // Word and Space: its advance; Open and Close: the left or right edge of the inline box; Atomic: the margin box (Float: 0)
  bool break_after = false;          // Word: a line may end after it (after a hyphen, in text that may be broken anywhere)
  bool hidden = false;               // a space that collapsed away at the start or end of a line
  bool no_break = false;             // white-space nowrap or pre: a line may not end after it
  uint8_t vertical_align = 0;        // the vertical-align of the nearest inline box that sets one (0 baseline, 1 top, 2 middle, 3 bottom)
};

struct ChildrenResult {
  LayoutUnit height;
  MarginPair top_escape, bottom_escape;   // margins of the first and last child that collapse with the parent's
  bool empty = true;                      // no child took any room (every child was a block whose margins collapsed through)
};

struct BlockOutcome {
  MarginPair top, bottom;      // the box's margins as the parent collapses them (its own, joined with its first/last child's when they adjoin)
  bool self_collapsing = false;   // nothing in it: its top and bottom margins collapse together and with the neighbours'
};

enum class TokenMode : uint8_t { Layout, MeasureMax, MeasureMin };

// An absolutely positioned box waiting for its containing block to be finished (its size is needed to place it), with the place it would have
// had in the flow (relative to the border box of `container`, the block whose content it was in).
struct PendingAbsolute {
  BoxId box = kNoBox, container = kNoBox, containing_block = kNoBox;
  LayoutUnit static_x, static_y;
};

// Does the style let lines break at spaces (white-space normal, pre-wrap, pre-line, break-spaces)?
inline bool style_wraps(const browser::Style& style) {
  const uint8_t ws = style.pre && style.white_space == 0 ? 2 : style.white_space;
  return ws == 0 || ws == 3 || ws == 4 || ws == 5;
}

// Is there a place to end a line after token `i`? (After a space, around atomic inlines, after a hyphen; never in text that does not wrap.)
inline bool break_after_token(const List<Token>& tokens, uint64_t i, bool container_wraps) {
  const Token& t = tokens[i];
  if (t.no_break) return false;
  if (t.type == Token::Type::Space) return true;
  if (t.type == Token::Type::Atomic) return container_wraps;
  if (t.type == Token::Type::Word && t.break_after) return true;
  if (container_wraps && i + 1 < tokens.count() && tokens[i + 1].type == Token::Type::Atomic) return true;
  return false;
}

class LayoutContext {
 public:
  LayoutContext(BoxTree& tree, const LayoutOptions& options) : tree_(tree), options_(options), measurer_(*options.measurer), pending_abs_(tree.allocator()) {}
  bool run();

  // Intrinsic.cpp: the widths the content of a box takes when it is not wrapped (max) and wrapped at every chance (min). Cached in the box.
  void intrinsic_widths(BoxId id, LayoutUnit* min_content, LayoutUnit* max_content);

 private:
  struct WidthRequest {
    bool forced = false;          // the content width is given (content_width)
    bool shrink = false;          // shrink-to-fit: between the min-content and max-content width, as much as fits in `available`
    LayoutUnit content_width, available;
    // Where the box is in its block formatting context (the coordinates floats are kept in): the left edge of its parent's content box and its
    // own top border edge. A box that starts a formatting context of its own ignores them.
    LayoutUnit bfc_x, bfc_y;
    // The content height is given (an absolutely positioned box with top and bottom and an auto height).
    bool has_forced_height = false;
    LayoutUnit forced_height;
    // The height of the containing block when it is definite (what a percentage height is of).
    bool cb_height_known = false;
    LayoutUnit cb_height;
  };
  struct AutoMargins {
    bool top = false, right = false, bottom = false, left = false;
  };

  // --- Layout.cpp
  BlockOutcome layout_block(BoxId id, LayoutUnit cb_width, const WidthRequest& request);
  ChildrenResult layout_block_children(BoxId id, LayoutUnit content_width, bool collapse_top, bool collapse_bottom);
  AutoMargins resolve_edges(Box& box, LayoutUnit cb_width, bool cb_known = true);
  void size_replaced(const Box& box, LayoutUnit* width, LayoutUnit* height) const;
  void place_marker(BoxId item);
  // Lays out a floated box shrink-to-fit inside the container `container` and puts it as high as the float rules allow, not above `y` (in the
  // coordinates of the block formatting context). Its (x, y) become relative to the container's border box.
  void place_float(BoxId float_id, BoxId container, LayoutUnit y);
  // The margin the box's top edge will have once it is collapsed with its first child's and so on down (a cheap guess made before the box is laid
  // out, so that the floats inside it can be placed at the right height).
  MarginPair estimate_top_margin(BoxId id, LayoutUnit cb_width);
  // Positioning (CSS 2.1 section 9.3): relative boxes move after they are placed; absolute and fixed boxes are laid out when their containing block
  // (the nearest positioned ancestor, or the viewport) is done.
  void apply_relative(Box& box);
  void register_absolute(BoxId box, BoxId container, LayoutUnit static_x, LayoutUnit static_y);
  void layout_absolutes(BoxId containing_block);
  void layout_absolute(const PendingAbsolute& pending);
  LayoutUnit offset_in(BoxId from, BoxId ancestor, bool horizontal) const;
  // A length worked out against what its percentages are of (`base`, if `known`). False for auto, the keywords, and a percentage of something not
  // known (it then acts as auto).
  bool resolve_with(const browser::Length& length, const browser::Style& style, LayoutUnit base, bool known, LayoutUnit* out) const;
  // The same for a box's own lengths: widths, margins, paddings and left/right of the box's containing block width; heights and top/bottom of its height.
  bool resolve_in(const browser::Length& length, const Box& box, bool vertical, LayoutUnit* out) const {
    return resolve_with(length, box.style, vertical ? box.cb_height : box.cb_width, vertical ? box.cb_height_known : box.cb_width_known, out);
  }
  LayoutUnit resolve_or(const browser::Length& length, const Box& box, bool vertical, LayoutUnit fallback) const {
    LayoutUnit v;
    return resolve_in(length, box, vertical, &v) ? v : fallback;
  }
  LayoutUnit clamp_content_width(const Box& box, LayoutUnit width) const;
  LayoutUnit clamp_content_height(const Box& box, LayoutUnit height) const;

  // --- Flex.cpp, Grid.cpp, Table.cpp, Multicol.cpp: how a box lays out its children when it is not an ordinary block. Each is called from layout_block with the
  // box's content width (its borders, padding and own width worked out) and, when the height is given, that height (height_known); it positions the
  // children (their x, y relative to the box's border box, as layout_block_children does), sets the baseline of the box if it has one, and returns the
  // height of the content. The box starts a formatting context of its own, so no margins collapse through it. Until they are written they lay the
  // children out as blocks.
  ChildrenResult layout_flex(BoxId id, LayoutUnit content_width, bool height_known, LayoutUnit height);
  ChildrenResult layout_grid(BoxId id, LayoutUnit content_width, bool height_known, LayoutUnit height);
  ChildrenResult layout_table(BoxId id, LayoutUnit content_width, bool height_known, LayoutUnit height);
  ChildrenResult layout_columns(BoxId id, LayoutUnit content_width, bool height_known, LayoutUnit height);

  // --- Inline.cpp
  // Lays out the inline content of a block container into lines; returns the height of the lines.
  LayoutUnit layout_inline_content(BoxId id, LayoutUnit content_width);
  // Lays out an atomic inline (inline-block, replaced) shrink-to-fit in `available` and returns its margin box width.
  LayoutUnit layout_atomic(BoxId id, LayoutUnit available);
  // Makes the tokens of a container's inline content (Layout: atomic inlines are laid out now; Measure*: their intrinsic widths are used).
  bool collect_tokens(BoxId container, LayoutUnit available, TokenMode mode, List<Token>* tokens);
  LayoutUnit line_height_of(const browser::Style& style, const TextMetrics& metrics) const;
  // The width an atomic inline takes in a line without being laid out: its margin box at its max-content (or min-content) width (Intrinsic.cpp).
  LayoutUnit atomic_outer_width(BoxId id, bool max_content);
  LayoutUnit space_width(const browser::Style& style) { return measurer_.width(style, StringView(" ")); }

  BoxTree& tree_;
  const LayoutOptions& options_;
  ITextMeasurer& measurer_;
  bool failed_ = false;
  List<PendingAbsolute> pending_abs_;
  bool child_cb_height_known_ = false;       // the height of the box being laid out, when it is definite: what its children's percentage heights are of
  LayoutUnit child_cb_height_;
  FloatContext* floats_ = nullptr;           // the floats of the block formatting context being laid out
  LayoutUnit origin_x_, origin_y_;           // the border box of the box being laid out, in that context's coordinates

  friend struct InlineCollector;
};


}  // namespace layout
