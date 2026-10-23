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

#include "Layout/Layout.hpp"
#include "Layout/LayoutInternal.hpp"

namespace layout {

using browser::BorderStyle;
using browser::BoxSizing;
using browser::Length;
using browser::Overflow;
using browser::Style;

bool layout_tree(BoxTree& tree, const LayoutOptions& options) {
  if (!options.measurer || tree.empty()) return false;
  LayoutContext context(tree, options);
  return context.run();
}

bool LayoutContext::run() {
  for (uint64_t i = 0; i < tree_.count(); ++i) {   // (a box laid out before starts over)
    Box& b = tree_.box(static_cast<BoxId>(i));
    b.lines.clear();
    b.fragments.clear();
    b.intrinsic_known = false;
  }
  const BoxId root = tree_.root();
  WidthRequest request;
  request.forced = true;
  request.content_width = options_.viewport_width;
  layout_block(root, options_.viewport_width, request);
  Box& r = tree_.box(root);
  r.x = LayoutUnit();
  r.y = LayoutUnit();
  return !failed_;
}

LayoutContext::AutoMargins LayoutContext::resolve_edges(Box& b, LayoutUnit cb_width) {
  (void)cb_width;   // (percentages of the containing block's width come with the Length work)
  const Style& s = b.style;
  AutoMargins autos;
  auto border = [&](int i) {
    const bool shown = s.border_style[i] != BorderStyle::None && s.border_style[i] != BorderStyle::Hidden;
    return shown ? max(LayoutUnit::from_px100(s.border_px100[i]), LayoutUnit()) : LayoutUnit();
  };
  b.border.top = border(0);
  b.border.right = border(1);
  b.border.bottom = border(2);
  b.border.left = border(3);
  b.padding.top = max(length_or(s.padding[0], LayoutUnit()), LayoutUnit());
  b.padding.right = max(length_or(s.padding[1], LayoutUnit()), LayoutUnit());
  b.padding.bottom = max(length_or(s.padding[2], LayoutUnit()), LayoutUnit());
  b.padding.left = max(length_or(s.padding[3], LayoutUnit()), LayoutUnit());
  autos.top = s.margin[0].is_auto();
  autos.right = s.margin[1].is_auto();
  autos.bottom = s.margin[2].is_auto();
  autos.left = s.margin[3].is_auto();
  b.margin.top = length_or(s.margin[0], LayoutUnit());
  b.margin.right = length_or(s.margin[1], LayoutUnit());
  b.margin.bottom = length_or(s.margin[2], LayoutUnit());
  b.margin.left = length_or(s.margin[3], LayoutUnit());
  return autos;
}

LayoutUnit LayoutContext::clamp_content_width(const Box& b, LayoutUnit width) const {
  const Style& s = b.style;
  const LayoutUnit edges = s.sizing == BoxSizing::BorderBox ? b.border.horizontal() + b.padding.horizontal() : LayoutUnit();
  if (s.max_width.is_px()) width = min(width, max(LayoutUnit::from_px100(s.max_width.px100) - edges, LayoutUnit()));
  if (s.min_width.is_px()) width = max(width, max(LayoutUnit::from_px100(s.min_width.px100) - edges, LayoutUnit()));
  return max(width, LayoutUnit());
}

LayoutUnit LayoutContext::clamp_content_height(const Box& b, LayoutUnit height) const {
  const Style& s = b.style;
  const LayoutUnit edges = s.sizing == BoxSizing::BorderBox ? b.border.vertical() + b.padding.vertical() : LayoutUnit();
  if (s.max_height.is_px()) height = min(height, max(LayoutUnit::from_px100(s.max_height.px100) - edges, LayoutUnit()));
  if (s.min_height.is_px()) height = max(height, max(LayoutUnit::from_px100(s.min_height.px100) - edges, LayoutUnit()));
  return max(height, LayoutUnit());
}

// The content size of a replaced element (CSS 2.1 section 10.3.2 and 10.6.2): what the style says, else what follows from the natural
// size and its shape, else the natural size; a picture that has no natural size and is given none is empty.
void LayoutContext::size_replaced(const Box& b, LayoutUnit* width, LayoutUnit* height) const {
  const Style& s = b.style;
  const LayoutUnit h_edges = s.sizing == BoxSizing::BorderBox ? b.border.horizontal() + b.padding.horizontal() : LayoutUnit();
  const LayoutUnit v_edges = s.sizing == BoxSizing::BorderBox ? b.border.vertical() + b.padding.vertical() : LayoutUnit();
  const bool has_w = s.width.is_px(), has_h = s.height.is_px();
  const bool ratio = b.has_natural_width && b.has_natural_height && b.natural_width > LayoutUnit() && b.natural_height > LayoutUnit();
  LayoutUnit w = has_w ? max(LayoutUnit::from_px100(s.width.px100) - h_edges, LayoutUnit()) : LayoutUnit();
  LayoutUnit h = has_h ? max(LayoutUnit::from_px100(s.height.px100) - v_edges, LayoutUnit()) : LayoutUnit();
  if (has_w && has_h) {
  } else if (has_w) {
    h = ratio ? w.scaled(b.natural_height.raw(), b.natural_width.raw()) : b.has_natural_height ? b.natural_height : LayoutUnit();
  } else if (has_h) {
    w = ratio ? h.scaled(b.natural_width.raw(), b.natural_height.raw()) : b.has_natural_width ? b.natural_width : LayoutUnit();
  } else {
    w = b.has_natural_width ? b.natural_width : LayoutUnit();
    h = b.has_natural_height ? b.natural_height : LayoutUnit();
  }
  *width = clamp_content_width(b, w);
  *height = clamp_content_height(b, h);
}

BlockOutcome LayoutContext::layout_block(BoxId id, LayoutUnit cb_width, const WidthRequest& request) {
  BlockOutcome outcome;
  Box& b = tree_.box(id);
  const Style& s = b.style;
  const AutoMargins autos = resolve_edges(b, cb_width);
  const LayoutUnit bp_h = b.border.horizontal() + b.padding.horizontal();
  const LayoutUnit bp_v = b.border.vertical() + b.padding.vertical();
  const bool replaced = b.kind == BoxKind::Replaced;

  // --- the width (CSS 2.1 sections 10.3.3 and 10.3.5)
  LayoutUnit content_width;
  LayoutUnit replaced_height;
  const bool keyword_width = s.width.kind == Length::MinContent || s.width.kind == Length::MaxContent || s.width.kind == Length::FitContent;
  if (replaced) {
    size_replaced(b, &content_width, &replaced_height);
    if (request.forced) content_width = request.content_width;
  } else if (request.forced) {
    content_width = request.content_width;
  } else if (s.width.is_px()) {
    const LayoutUnit specified = LayoutUnit::from_px100(s.width.px100);
    content_width = clamp_content_width(b, s.sizing == BoxSizing::BorderBox ? max(specified - bp_h, LayoutUnit()) : specified);
  } else if (request.shrink || keyword_width) {
    LayoutUnit mn, mx;
    intrinsic_widths(id, &mn, &mx);
    const LayoutUnit available = (request.shrink ? request.available : cb_width) - b.margin.horizontal() - bp_h;
    LayoutUnit w = min(max(mn, available), mx);
    if (s.width.kind == Length::MinContent) w = mn;
    else if (s.width.kind == Length::MaxContent) w = mx;
    content_width = clamp_content_width(b, w);
  } else {
    const LayoutUnit ml = autos.left ? LayoutUnit() : b.margin.left, mr = autos.right ? LayoutUnit() : b.margin.right;
    content_width = clamp_content_width(b, max(cb_width - ml - mr - bp_h, LayoutUnit()));
  }
  // the horizontal margins that were auto take what is left over (they are 0 for boxes that sit in a line or are sized to fit)
  const bool in_flow_block = !request.forced && !request.shrink && b.is_block_level() && !b.floated && !b.out_of_flow;
  if (in_flow_block) {
    const LayoutUnit used = content_width + bp_h;
    if (autos.left && autos.right) {
      const LayoutUnit spare = max(cb_width - used, LayoutUnit());
      b.margin.left = spare / 2;
      b.margin.right = spare - b.margin.left;
    } else if (autos.left) {
      b.margin.left = max(cb_width - used - b.margin.right, LayoutUnit());
    } else if (autos.right) {
      b.margin.right = max(cb_width - used - b.margin.left, LayoutUnit());
    } else {
      b.margin.right = cb_width - used - b.margin.left;   // (over-constrained: the right margin gives way)
    }
  } else {
    if (autos.left) b.margin.left = LayoutUnit();
    if (autos.right) b.margin.right = LayoutUnit();
  }
  b.width = content_width + bp_h;

  // --- the formatting context: a box that starts one keeps its own floats; any other box shares its parent's
  FloatContext local_floats(tree_.allocator());
  FloatContext* const saved_floats = floats_;
  const LayoutUnit saved_x = origin_x_, saved_y = origin_y_;
  if (b.establishes_bfc || !floats_) {
    floats_ = &local_floats;
    origin_x_ = LayoutUnit();
    origin_y_ = LayoutUnit();
  } else {
    origin_x_ = request.bfc_x + b.margin.left;
    origin_y_ = request.bfc_y;
  }

  // --- the content
  const bool height_auto = !s.height.is_px();
  const LayoutUnit min_height = s.min_height.is_px() ? LayoutUnit::from_px100(s.min_height.px100) : LayoutUnit();
  const bool can_collapse_top = !b.establishes_bfc && !replaced && b.border.top.is_zero() && b.padding.top.is_zero();
  const bool can_collapse_bottom = !b.establishes_bfc && !replaced && b.border.bottom.is_zero() && b.padding.bottom.is_zero() && height_auto && min_height.is_zero();
  LayoutUnit content_height;
  ChildrenResult children;
  b.baseline = LayoutUnit();
  b.has_baseline = false;
  if (replaced) {
    content_height = replaced_height;
  } else if (b.inline_content) {
    content_height = layout_inline_content(id, content_width);
    children.empty = b.lines.count() == 0;
    if (b.lines.count() > 0 && s.overflow_x == Overflow::Visible && s.overflow_y == Overflow::Visible) {
      const LineInfo& last = b.lines[b.lines.count() - 1];
      b.baseline = b.content_top() + last.y + last.baseline;
      b.has_baseline = true;
    }
  } else {
    children = layout_block_children(id, content_width, can_collapse_top, can_collapse_bottom);
    content_height = children.height;
    // the baseline of the last in-flow block that has one
    for (uint64_t i = b.children.count(); i > 0 && s.overflow_x == Overflow::Visible && s.overflow_y == Overflow::Visible; --i) {
      const Box& c = tree_.box(b.children[i - 1]);
      if (c.kind != BoxKind::Marker && c.has_baseline) {
        b.baseline = c.y + c.baseline;
        b.has_baseline = true;
        break;
      }
    }
  }
  // a box that starts a formatting context contains its floats: it is as high as the lowest of them
  if (b.establishes_bfc && !replaced && height_auto && !local_floats.empty()) content_height = max(content_height, local_floats.lowest_bottom() - b.content_top());
  floats_ = saved_floats;
  origin_x_ = saved_x;
  origin_y_ = saved_y;
  if (!replaced) {
    if (!height_auto) {
      const LayoutUnit specified = LayoutUnit::from_px100(s.height.px100);
      content_height = s.sizing == BoxSizing::BorderBox ? max(specified - bp_v, LayoutUnit()) : specified;
    }
    if (request.has_forced_height && height_auto) content_height = request.forced_height;
    content_height = clamp_content_height(b, content_height);
  }
  b.height = content_height + bp_v;

  // --- its margins as its parent will collapse them (CSS 2.1 section 8.3.1)
  outcome.top.add(b.margin.top);
  outcome.bottom.add(b.margin.bottom);
  if (can_collapse_top) outcome.top.add(children.top_escape);
  if (can_collapse_bottom) outcome.bottom.add(children.bottom_escape);
  outcome.self_collapsing = !replaced && can_collapse_top && can_collapse_bottom && children.empty && content_height.is_zero() && b.kind != BoxKind::Root;
  if (b.kind == BoxKind::Block && b.style.display == browser::Display::ListItem) place_marker(id);
  // the boxes positioned against this one can be placed now that its size is known
  if (b.style.position != browser::Position::Static || b.kind == BoxKind::Root) layout_absolutes(id);
  return outcome;
}

MarginPair LayoutContext::estimate_top_margin(BoxId id) {
  MarginPair m;
  const Box& b = tree_.box(id);
  const Style& s = b.style;
  m.add(length_or(s.margin[0], LayoutUnit()));
  if (b.establishes_bfc || b.kind == BoxKind::Replaced || b.inline_content) return m;
  const bool border = s.border_style[0] != BorderStyle::None && s.border_style[0] != BorderStyle::Hidden && s.border_px100[0] > 0;
  if (border || length_or(s.padding[0], LayoutUnit()) != LayoutUnit()) return m;
  for (uint64_t i = 0; i < b.children.count(); ++i) {
    const Box& c = tree_.box(b.children[i]);
    if (c.kind == BoxKind::Marker || c.floated || c.out_of_flow) continue;
    if (c.is_block_level()) m.add(estimate_top_margin(b.children[i]));
    break;
  }
  return m;
}

void LayoutContext::place_float(BoxId float_id, BoxId container, LayoutUnit y) {
  const LayoutUnit width = tree_.box(container).content_width();
  WidthRequest request;
  request.shrink = true;
  request.available = width;
  layout_block(float_id, width, request);
  Box& f = tree_.box(float_id);
  const Box& cont = tree_.box(container);
  const LayoutUnit outer_w = f.margin.left + f.width + f.margin.right, outer_h = f.margin.top + f.height + f.margin.bottom;
  const LayoutUnit cb_left = origin_x_ + cont.content_left();
  LayoutUnit min_y = y;
  if (f.style.clear != browser::Clear::None) {
    const bool l = f.style.clear == browser::Clear::Left || f.style.clear == browser::Clear::Both;
    const bool r = f.style.clear == browser::Clear::Right || f.style.clear == browser::Clear::Both;
    min_y = max(min_y, floats_->clear_y(l, r));
  }
  FloatRect placed;
  failed_ |= !floats_->place(outer_w, outer_h, f.style.float_side == browser::Float::Left, cb_left, cb_left + width, min_y, &placed);
  f.x = placed.x + f.margin.left - origin_x_;
  f.y = placed.y + f.margin.top - origin_y_;
  apply_relative(f);
}

ChildrenResult LayoutContext::layout_block_children(BoxId id, LayoutUnit content_width, bool collapse_top, bool collapse_bottom) {
  ChildrenResult result;
  LayoutUnit cursor;
  MarginPair pending;
  bool first = true;
  const uint64_t count = tree_.box(id).children.count();
  for (uint64_t i = 0; i < count && !failed_; ++i) {
    const BoxId child_id = tree_.box(id).children[i];
    Box& child = tree_.box(child_id);
    if (child.kind == BoxKind::Marker) continue;
    const LayoutUnit content_left = origin_x_ + tree_.box(id).content_left();
    const LayoutUnit content_top = origin_y_ + tree_.box(id).content_top();
    if (child.floated) {   // a float goes where the next block would, beside what is already there
      place_float(child_id, id, content_top + cursor + ((first && collapse_top) ? LayoutUnit() : pending.sum()));
      continue;
    }
    if (child.out_of_flow) {   // it leaves the flow: placed against its containing block later; this is where it would have been
      register_absolute(child_id, id, tree_.box(id).content_left(), tree_.box(id).content_top() + cursor + ((first && collapse_top) ? LayoutUnit() : pending.sum()));
      continue;
    }
    // where the box will be, as far as can be told before it is laid out: its top edge after the margins that collapse into it
    MarginPair margin = pending;
    margin.add(estimate_top_margin(child_id));
    LayoutUnit y_local = (first && collapse_top) ? cursor : cursor + margin.sum();
    bool cleared = false;
    if (child.style.clear != browser::Clear::None) {   // clearance: the top edge goes below the floats it clears
      const bool l = child.style.clear == browser::Clear::Left || child.style.clear == browser::Clear::Both;
      const bool r = child.style.clear == browser::Clear::Right || child.style.clear == browser::Clear::Both;
      const LayoutUnit below = floats_->clear_y(l, r);
      if (below > content_top + y_local) {
        cursor = below - content_top;
        y_local = cursor;
        pending = MarginPair();
        cleared = true;
      }
    }
    // a box with a formatting context of its own does not overlap the floats beside it: it takes the room between them, or goes lower
    LayoutUnit width_for_child = content_width, shift = LayoutUnit();
    if (child.establishes_bfc && !floats_->empty()) {
      for (uint64_t attempt = 0; attempt <= 64; ++attempt) {
        LayoutUnit l, r;
        floats_->band(content_top + y_local, LayoutUnit::from_raw(1), content_left, content_left + content_width, &l, &r);
        if (r - l >= content_width) break;
        if (!child.style.width.is_px() || atomic_outer_width(child_id, true) <= r - l) {
          width_for_child = r - l;
          shift = l - content_left;
          break;
        }
        const LayoutUnit next = floats_->next_edge_below(content_top + y_local);
        if (next.is_max()) break;
        cursor = next - content_top;
        y_local = cursor;
        pending = MarginPair();
        cleared = true;
      }
    }
    WidthRequest request;
    request.bfc_x = content_left + shift;
    request.bfc_y = content_top + y_local;
    const BlockOutcome o = layout_block(child_id, width_for_child, request);
    Box& parent = tree_.box(id);
    Box& c = tree_.box(child_id);
    c.x = parent.content_left() + shift + c.margin.left;
    c.y = parent.content_top() + cursor;   // (relative offsets below are applied once the place in the flow is known)
    if (o.self_collapsing) {
      MarginPair m = o.top;
      m.add(o.bottom);
      if (first && collapse_top) {
        result.top_escape.add(m);
        c.y = parent.content_top() + cursor;
      } else {
        pending.add(m);
        c.y = parent.content_top() + cursor + pending.sum();
      }
      continue;
    }
    if (cleared) {
      // (the clearance took the place of the margin above)
    } else if (first && collapse_top) {
      result.top_escape.add(o.top);
    } else {
      pending.add(o.top);
      cursor += pending.sum();
      pending = MarginPair();
    }
    first = false;
    result.empty = false;
    c.y = parent.content_top() + cursor;
    cursor += c.height;
    pending = o.bottom;
    apply_relative(c);
  }
  if (collapse_bottom) result.bottom_escape = pending;
  else cursor += pending.sum();
  result.height = cursor;
  return result;
}

// The marker of a list item sits outside its first line, to the left of the content (inside markers are ordinary text and not here).
void LayoutContext::place_marker(BoxId item) {
  Box& li = tree_.box(item);
  for (uint64_t i = 0; i < li.children.count(); ++i) {
    Box& m = tree_.box(li.children[i]);
    if (m.kind != BoxKind::Marker) continue;
    const TextMetrics metrics = measurer_.metrics(m.style);
    m.width = measurer_.width(m.style, m.text.view());
    m.height = line_height_of(m.style, metrics);
    m.x = li.content_left() - m.width;
    m.y = li.content_top() + (li.lines.count() > 0 ? li.lines[0].y : LayoutUnit());
    return;
  }
}

}  // namespace layout
