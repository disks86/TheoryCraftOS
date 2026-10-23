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

// Positioned boxes (CSS 2.1 section 9.3 and 10.3.7, 10.6.4). A relatively positioned box is laid out where it falls in the flow and then shifted;
// the flow does not notice. An absolutely positioned box leaves the flow: it is remembered with the place it would have had, and when the nearest
// positioned ancestor (its containing block; the viewport if there is none, and for fixed boxes always) has its size, the box is sized and placed
// against that block's padding box from its top/right/bottom/left, falling back to the remembered place for what is auto.
#include "Layout/LayoutInternal.hpp"

namespace layout {

using browser::BoxSizing;
using browser::Position;

void LayoutContext::apply_relative(Box& b) {
  if (b.style.position != Position::Relative && b.style.position != Position::Sticky) return;
  if (b.style.position == Position::Sticky) return;   // (a sticky box sits where the flow puts it until scrolling is involved)
  const browser::Style& s = b.style;
  LayoutUnit v, dx, dy;
  if (resolve_in(s.inset[3], b, false, &v)) dx = v;
  else if (resolve_in(s.inset[1], b, false, &v)) dx = -v;
  if (resolve_in(s.inset[0], b, true, &v)) dy = v;
  else if (resolve_in(s.inset[2], b, true, &v)) dy = -v;
  b.x += dx;
  b.y += dy;
}

void LayoutContext::register_absolute(BoxId id, BoxId container, LayoutUnit static_x, LayoutUnit static_y) {
  PendingAbsolute p;
  p.box = id;
  p.container = container;
  p.static_x = static_x;
  p.static_y = static_y;
  p.containing_block = tree_.root();
  if (tree_.box(id).style.position != Position::Fixed) {
    for (BoxId cur = container; cur != kNoBox; cur = tree_.box(cur).parent) {
      if (tree_.box(cur).style.position != Position::Static && tree_.box(cur).kind != BoxKind::Inline) {
        p.containing_block = cur;
        break;
      }
    }
  }
  failed_ |= !pending_abs_.push_back(p);
}

// Where a box's border box is relative to the border box of an ancestor (boxes without a place of their own, inline boxes, add nothing).
LayoutUnit LayoutContext::offset_in(BoxId from, BoxId ancestor, bool horizontal) const {
  LayoutUnit sum;
  for (BoxId cur = from; cur != ancestor && cur != kNoBox; cur = tree_.box(cur).parent) {
    const Box& b = tree_.box(cur);
    if (b.kind == BoxKind::Inline) continue;
    sum += horizontal ? b.x : b.y;
  }
  return sum;
}

void LayoutContext::layout_absolutes(BoxId containing_block) {
  List<PendingAbsolute> mine(tree_.allocator());
  List<PendingAbsolute> rest(tree_.allocator());
  for (uint64_t i = 0; i < pending_abs_.count(); ++i) {
    if (pending_abs_[i].containing_block == containing_block) failed_ |= !mine.push_back(pending_abs_[i]);
    else failed_ |= !rest.push_back(pending_abs_[i]);
  }
  if (mine.count() == 0) return;
  pending_abs_.clear();
  for (uint64_t i = 0; i < rest.count(); ++i) failed_ |= !pending_abs_.push_back(rest[i]);
  for (uint64_t i = 0; i < mine.count() && !failed_; ++i) layout_absolute(mine[i]);
}

void LayoutContext::layout_absolute(const PendingAbsolute& pending) {
  Box& p = tree_.box(pending.box);
  const Box& cb = tree_.box(pending.containing_block);
  const browser::Style& s = p.style;
  const bool replaced = p.kind == BoxKind::Replaced;

  // the containing block: the padding box of a positioned ancestor, the viewport for the root
  LayoutUnit cb_left, cb_top, cb_w, cb_h;
  if (cb.kind == BoxKind::Root) {
    cb_w = options_.viewport_width;
    cb_h = options_.viewport_height;
  } else {
    cb_left = cb.border.left;
    cb_top = cb.border.top;
    cb_w = max(cb.width - cb.border.horizontal(), LayoutUnit());
    cb_h = max(cb.height - cb.border.vertical(), LayoutUnit());
  }
  const LayoutUnit static_x = pending.static_x + offset_in(pending.container, pending.containing_block, true);
  const LayoutUnit static_y = pending.static_y + offset_in(pending.container, pending.containing_block, false);

  // (insets are percentages of the containing block's padding box)
  LayoutUnit left, right, top, bottom;
  const bool left_auto = !resolve_with(s.inset[3], s, cb_w, true, &left), right_auto = !resolve_with(s.inset[1], s, cb_w, true, &right);
  const bool top_auto = !resolve_with(s.inset[0], s, cb_h, true, &top), bottom_auto = !resolve_with(s.inset[2], s, cb_h, true, &bottom);

  p.cb_height = cb_h;
  p.cb_height_known = true;
  AutoMargins autos = resolve_edges(p, cb_w);
  const LayoutUnit bp_h = p.border.horizontal() + p.padding.horizontal();
  const LayoutUnit bp_v = p.border.vertical() + p.padding.vertical();
  const LayoutUnit fixed_ml = autos.left ? LayoutUnit() : p.margin.left, fixed_mr = autos.right ? LayoutUnit() : p.margin.right;
  const LayoutUnit fixed_mt = autos.top ? LayoutUnit() : p.margin.top, fixed_mb = autos.bottom ? LayoutUnit() : p.margin.bottom;
  LayoutUnit specified_width, specified_height;
  const bool width_auto = !resolve_with(s.width, s, cb_w, true, &specified_width) && !replaced;

  // the width (CSS 2.1 section 10.3.7)
  WidthRequest request;
  request.forced = true;
  if (replaced) {
    LayoutUnit h;
    size_replaced(p, &request.content_width, &h);
  } else if (!width_auto) {
    request.content_width = clamp_content_width(p, s.sizing == BoxSizing::BorderBox ? max(specified_width - bp_h, LayoutUnit()) : specified_width);
  } else if (!left_auto && !right_auto) {
    request.content_width = clamp_content_width(p, max(cb_w - left - right - fixed_ml - fixed_mr - bp_h, LayoutUnit()));
  } else {
    LayoutUnit mn, mx;
    intrinsic_widths(pending.box, &mn, &mx);
    const LayoutUnit available = cb_w - (left_auto ? LayoutUnit() : left) - (right_auto ? LayoutUnit() : right) - fixed_ml - fixed_mr - bp_h;
    request.content_width = clamp_content_width(p, min(max(mn, available), mx));
  }
  // the height: both top and bottom given and the height auto makes the box as high as the room between them
  const bool height_auto = !resolve_with(s.height, s, cb_h, true, &specified_height) && !replaced;
  if (height_auto && !top_auto && !bottom_auto) {
    request.has_forced_height = true;
    request.forced_height = max(cb_h - top - bottom - fixed_mt - fixed_mb - bp_v, LayoutUnit());
  }
  request.cb_height_known = true;
  request.cb_height = cb_h;
  layout_block(pending.box, cb_w, request);
  autos = resolve_edges(p, cb_w);   // (layout_block's own pass zeroed the auto margins; they are decided here)

  // horizontal place: left, else right, else where it would have been; auto margins share what is left when both sides are given
  LayoutUnit ml = fixed_ml, mr = fixed_mr;
  if (!left_auto && !right_auto && (!width_auto || replaced)) {
    const LayoutUnit spare = cb_w - left - right - p.width - fixed_ml - fixed_mr;
    if (autos.left && autos.right) {
      ml = max(spare, LayoutUnit()) / 2;
      mr = max(spare, LayoutUnit()) - ml;
    } else if (autos.left) {
      ml = max(spare, LayoutUnit());
    } else if (autos.right) {
      mr = max(spare, LayoutUnit());
    }
  }
  LayoutUnit x;
  if (!left_auto) x = cb_left + left + ml;
  else if (!right_auto) x = cb_left + cb_w - right - mr - p.width;
  else x = static_x + ml;
  LayoutUnit y;
  if (!top_auto) y = cb_top + top + fixed_mt;
  else if (!bottom_auto) y = cb_top + cb_h - bottom - fixed_mb - p.height;
  else y = static_y + fixed_mt;
  p.margin.left = ml;
  p.margin.right = mr;
  p.margin.top = fixed_mt;
  p.margin.bottom = fixed_mb;
  // relative to the box it sits in (the block that owns the line, or the block itself)
  p.x = x - offset_in(pending.container, pending.containing_block, true);
  p.y = y - offset_in(pending.container, pending.containing_block, false);
}

}  // namespace layout
