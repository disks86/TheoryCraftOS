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

// Layout of flexbox (CSS Flexbox Level 1, section 9): the items are collected and ordered, broken into lines, their flexible lengths resolved
// (the freeze loop, with min and max clamping and the automatic minimum size), laid out at the sizes that came out so that their own content
// fits them, and aligned on the cross axis (align-content, align-items / align-self incl. baseline and stretch, auto margins) and the main
// axis (justify-content, auto margins, gaps). Positions are worked out along the main and cross axis from their start (logical) and turned
// into x and y at the end, where reversed directions (row-reverse, wrap-reverse, rtl rows) flip them.
//
// Not supported: aspect-ratio on items, `flex-basis: content` (acts as auto), safe/unsafe keywords, the static position of absolutely
// positioned children is the start of the content box, baseline alignment in a column (acts as flex-start), writing modes other than
// horizontal-tb, fragmentation.
#include "Layout/LayoutInternal.hpp"

namespace layout {

using browser::BoxSizing;
using browser::FlexDirection;
using browser::FlexWrap;
using browser::Length;
using browser::Overflow;
using browser::Place;
using browser::Style;

namespace {

struct Item {
  BoxId id = kNoBox;
  int32_t order = 0;
  // sizes along the main axis (content box unless noted) and the edges around it; margins are "logical": ms is the one at the main-start side
  LayoutUnit main_bp, cross_bp;   // border + padding
  LayoutUnit ms, me, cs, ce;      // margins at the main start / end and the cross start / end (auto ones are 0 until the free space is shared out)
  bool auto_ms = false, auto_me = false, auto_cs = false, auto_ce = false;
  LayoutUnit base, hyp, target;   // flex base size, hypothetical main size (clamped), resolved main size
  LayoutUnit min_main, max_main = LayoutUnit::max();
  int32_t grow = 0, shrink = 0;   // x100
  bool frozen = false;
  bool cross_auto = false;        // the cross size is auto, so stretch applies
  LayoutUnit cross;               // column: the content width it is laid out with
  LayoutUnit nat_main;            // column: its content height at that width, when `measured`
  bool measured = false;
  Place align = Place::FlexStart;
  LayoutUnit outer_cross;         // its margin box in the cross axis
  LayoutUnit ascent;              // baseline alignment: the baseline's distance from the cross-start margin edge
  LayoutUnit main_off, cross_off; // the margin box's place from the line's main start and the container's cross start
};

struct Line {
  uint32_t first = 0, count = 0;   // in the ordered item list
  LayoutUnit hyp_sum;              // the hypothetical outer main sizes and the gaps
  LayoutUnit cross, cross_pos;
  LayoutUnit max_ascent;
};

// amount * weight / total where the weights can be wider than 32 bits (the image has no 128-bit division): both are scaled down together.
LayoutUnit share(LayoutUnit amount, int64_t weight, int64_t total) {
  while (total > INT32_MAX) {
    total >>= 1;
    weight >>= 1;
  }
  if (total <= 0) return LayoutUnit();
  return amount.scaled(static_cast<int32_t>(weight), static_cast<int32_t>(total));
}

int32_t px100_of(LayoutUnit v) {
  const int64_t px100 = v.to_px100();
  return px100 > 2000000000 ? 2000000000 : px100 < -2000000000 ? -2000000000 : static_cast<int32_t>(px100);
}

}  // namespace

struct FlexLayout {
  LayoutContext& c;
  BoxTree& tree;
  BoxId cid;
  bool row = true, rev_main = false, rev_cross = false, wrap = false;
  LayoutUnit cw, height;   // the container's content width and (when def_h) height
  bool def_h = false;
  LayoutUnit main_gap, cross_gap;
  List<Item> items;
  List<uint32_t> seq;      // the items in the order they are laid out
  List<Line> lines;

  FlexLayout(LayoutContext& ctx, BoxId id) : c(ctx), tree(ctx.tree_), cid(id), items(ctx.tree_.allocator()), seq(ctx.tree_.allocator()), lines(ctx.tree_.allocator()) {}

  // --- helpers
  LayoutUnit gap_of(const Length& l, LayoutUnit base, bool known) const {
    LayoutUnit v;
    return c.resolve_with(l, tree.box(cid).style, base, known, &v) ? max(v, LayoutUnit()) : LayoutUnit();
  }
  // A content-box size from a length of the box (box-sizing taken off); false when it is not a definite size.
  bool content_size(const Box& b, const Length& l, bool vertical, LayoutUnit* out) const {
    LayoutUnit v;
    if (!c.resolve_in(l, b, vertical, &v)) return false;
    const LayoutUnit edges = b.style.sizing == BoxSizing::BorderBox ? (vertical ? b.border.vertical() + b.padding.vertical() : b.border.horizontal() + b.padding.horizontal()) : LayoutUnit();
    *out = max(v - edges, LayoutUnit());
    return true;
  }
  LayoutUnit clamp_main(const Item& it, LayoutUnit v) const { return max(min(v, it.max_main), it.min_main); }
  LayoutUnit main_border(const Item& it) const { const Box& b = tree.box(it.id); return row ? b.width : b.height; }
  LayoutUnit cross_border(const Item& it) const { const Box& b = tree.box(it.id); return row ? b.height : b.width; }
  LayoutUnit outer_main(const Item& it) const { return it.ms + main_border(it) + it.me; }
  Place align_of(const Style& cs) const {
    const Style& s = tree.box(cid).style;
    const Place p = cs.align_self != Place::Auto ? cs.align_self : s.align_items;
    switch (p) {
      case Place::Center: return Place::Center;
      case Place::End: case Place::FlexEnd: case Place::Right: return Place::FlexEnd;
      case Place::Baseline: return row ? Place::Baseline : Place::FlexStart;
      case Place::Start: case Place::FlexStart: case Place::Left: case Place::SpaceBetween: case Place::SpaceAround: case Place::SpaceEvenly: return Place::FlexStart;
      default: return Place::Stretch;
    }
  }

  // Lays the item out with the content width (and height, if forced): its own content is placed at the size the flexing gave it.
  void layout_item(const Item& it, LayoutUnit content_w, bool force_h, LayoutUnit content_h, bool auto_h = false) {
    Box& b = tree.box(it.id);
    const Length saved = b.style.height;
    if (auto_h) {   // (measuring what the content takes, whatever height the style gives)
      b.style.height = Length();
    } else if (force_h) {
      const LayoutUnit edges = b.style.sizing == BoxSizing::BorderBox ? b.border.vertical() + b.padding.vertical() : LayoutUnit();
      b.style.height = Length::px(px100_of(content_h + edges));
    }
    LayoutContext::WidthRequest r;
    r.forced = true;
    r.content_width = content_w;
    r.cb_height_known = def_h;
    r.cb_height = height;
    c.layout_block(it.id, cw, r);
    tree.box(it.id).style.height = saved;
  }

  void set_margins(Item& it) {
    Box& b = tree.box(it.id);
    // (logical start margins sit at the physical end when that axis is reversed)
    if (row) {
      b.margin.left = rev_main ? it.me : it.ms;
      b.margin.right = rev_main ? it.ms : it.me;
      b.margin.top = rev_cross ? it.ce : it.cs;
      b.margin.bottom = rev_cross ? it.cs : it.ce;
    } else {
      b.margin.top = rev_main ? it.me : it.ms;
      b.margin.bottom = rev_main ? it.ms : it.me;
      b.margin.left = rev_cross ? it.ce : it.cs;
      b.margin.right = rev_cross ? it.cs : it.ce;
    }
  }

  // --- steps
  void collect() {
    const Box& box = tree.box(cid);
    for (uint64_t i = 0; i < box.children.count() && !c.failed_; ++i) {
      const BoxId child = box.children[i];
      const Box& b = tree.box(child);
      if (b.kind == BoxKind::Marker || b.kind == BoxKind::Text || b.kind == BoxKind::LineBreak) continue;
      if (b.out_of_flow) {
        c.register_absolute(child, cid, box.content_left(), box.content_top());
        continue;
      }
      Item it;
      it.id = child;
      it.order = b.style.order;
      c.failed_ |= !items.push_back(it);
    }
    // (a stable insertion sort by `order`)
    for (uint64_t i = 0; i < items.count() && !c.failed_; ++i) {
      uint64_t at = seq.count();
      while (at > 0 && items[seq[at - 1]].order > items[i].order) --at;
      c.failed_ |= !seq.insert(at, static_cast<uint32_t>(i));
    }
  }

  // The sizes of one item that do not depend on the others: edges, min and max, flex base size and hypothetical main size.
  void prepare(Item& it) {
    Box& b = tree.box(it.id);
    const Style& s = b.style;
    LayoutUnit mn, mx;
    c.intrinsic_widths(it.id, &mn, &mx);   // (before the edges: it resolves them against an unknown containing block)
    b.cb_height = height;
    b.cb_height_known = def_h;
    const LayoutContext::AutoMargins autos = c.resolve_edges(b, cw);
    const LayoutUnit bp_w = b.border.horizontal() + b.padding.horizontal(), bp_h = b.border.vertical() + b.padding.vertical();
    it.main_bp = row ? bp_w : bp_h;
    it.cross_bp = row ? bp_h : bp_w;
    auto margin = [&](LayoutUnit start, LayoutUnit end, bool auto_start, bool auto_end, bool rev, LayoutUnit* ls, LayoutUnit* le, bool* als, bool* ale) {
      *ls = rev ? end : start;
      *le = rev ? start : end;
      *als = rev ? auto_end : auto_start;
      *ale = rev ? auto_start : auto_end;
    };
    if (row) {
      margin(b.margin.left, b.margin.right, autos.left, autos.right, rev_main, &it.ms, &it.me, &it.auto_ms, &it.auto_me);
      margin(b.margin.top, b.margin.bottom, autos.top, autos.bottom, rev_cross, &it.cs, &it.ce, &it.auto_cs, &it.auto_ce);
    } else {
      margin(b.margin.top, b.margin.bottom, autos.top, autos.bottom, rev_main, &it.ms, &it.me, &it.auto_ms, &it.auto_me);
      margin(b.margin.left, b.margin.right, autos.left, autos.right, rev_cross, &it.cs, &it.ce, &it.auto_cs, &it.auto_ce);
    }
    it.grow = s.flex_grow_x100;
    it.shrink = s.flex_shrink_x100;
    it.align = align_of(s);
    LayoutUnit v;
    const Length& cross_len = row ? s.height : s.width;
    it.cross_auto = !content_size(b, cross_len, row, &v) && !(!row && (s.width.kind == Length::MinContent || s.width.kind == Length::MaxContent));

    // min and max along the main axis
    const Length& max_len = row ? s.max_width : s.max_height;
    const Length& min_len = row ? s.min_width : s.min_height;
    if (content_size(b, max_len, !row, &v)) it.max_main = v;
    const bool auto_min = min_len.is_auto();
    if (!auto_min && content_size(b, min_len, !row, &v)) it.min_main = v;

    // column: the width it will have (its cross size), then the height its content takes at that width
    if (!row) {
      LayoutUnit avail = max(cw - it.cs - it.ce - it.cross_bp, LayoutUnit());
      LayoutUnit w;
      if (content_size(b, s.width, false, &w)) it.cross = w;
      else if (s.width.kind == Length::MinContent) it.cross = mn;
      else if (s.width.kind == Length::MaxContent) it.cross = mx;
      else if (it.align == Place::Stretch && !it.auto_cs && !it.auto_ce) it.cross = avail;
      else it.cross = min(max(mn, avail), mx);
      it.cross = c.clamp_content_width(b, it.cross);
    }
    const bool scroll = s.overflow_x != Overflow::Visible || s.overflow_y != Overflow::Visible;
    LayoutUnit spec;   // the size the style gives along the main axis
    bool has_spec = content_size(b, row ? s.width : s.height, !row, &spec);
    if (row && !has_spec) {
      if (s.width.kind == Length::MinContent) { spec = mn; has_spec = true; }
      else if (s.width.kind == Length::MaxContent) { spec = mx; has_spec = true; }
    }

    // the flex base size
    bool have_base = false;
    const Length& basis = s.flex_basis;
    if (basis.is_px() || basis.has_percent()) {
      if (c.resolve_with(basis, s, row ? cw : height, row || def_h, &v)) {
        it.base = max(v - (s.sizing == BoxSizing::BorderBox ? it.main_bp : LayoutUnit()), LayoutUnit());
        have_base = true;
      }
    } else if (row && basis.kind == Length::MinContent) {
      it.base = mn;
      have_base = true;
    } else if (row && (basis.kind == Length::MaxContent || basis.kind == Length::FitContent)) {
      it.base = mx;
      have_base = true;
    }
    if (!have_base && has_spec) {
      it.base = spec;
      have_base = true;
    }
    const bool needs_measure = !row && (!have_base || (auto_min && !scroll));
    if (needs_measure) {
      layout_item(it, it.cross, false, LayoutUnit(), true);
      it.nat_main = tree.box(it.id).content_height();
      it.measured = true;
    }
    if (!have_base) it.base = row ? mx : it.nat_main;

    // the automatic minimum size: the smaller of what the content wants and the size the style gives, never beyond the max
    if (auto_min) {
      if (scroll) {
        it.min_main = LayoutUnit();
      } else {
        LayoutUnit content = row ? mn : it.nat_main;
        if (has_spec) content = min(content, spec);
        it.min_main = min(content, it.max_main);
      }
    }
    it.hyp = clamp_main(it, it.base);
    it.target = it.hyp;
  }

  void make_lines(LayoutUnit avail) {
    Line cur;
    LayoutUnit sum;
    for (uint64_t k = 0; k < seq.count() && !c.failed_; ++k) {
      const Item& it = items[seq[k]];
      const LayoutUnit outer = it.hyp + it.main_bp + it.ms + it.me;
      if (wrap && cur.count > 0 && !avail.is_max() && sum + main_gap + outer > avail) {
        cur.hyp_sum = sum;
        c.failed_ |= !lines.push_back(cur);
        cur = Line();
        cur.first = static_cast<uint32_t>(k);
        sum = LayoutUnit();
      }
      sum = cur.count > 0 ? sum + main_gap + outer : outer;
      ++cur.count;
    }
    if (cur.count > 0) {
      cur.hyp_sum = sum;
      c.failed_ |= !lines.push_back(cur);
    }
  }

  // Resolving the flexible lengths of a line (section 9.7): the freeze loop.
  void flex_line(const Line& ln, LayoutUnit inner) {
    const LayoutUnit gaps = main_gap * static_cast<int32_t>(ln.count - 1);
    const bool growing = ln.hyp_sum < inner;
    auto overhead = [&](const Item& it) { return it.main_bp + it.ms + it.me; };
    for (uint32_t k = 0; k < ln.count; ++k) {
      Item& it = items[seq[ln.first + k]];
      const int32_t factor = growing ? it.grow : it.shrink;
      it.frozen = factor == 0 || (growing ? it.base > it.hyp : it.base < it.hyp);
      if (it.frozen) it.target = it.hyp;
    }
    auto used = [&]() {   // frozen items at their target, the others at their base size
      LayoutUnit sum = gaps;
      for (uint32_t k = 0; k < ln.count; ++k) {
        const Item& it = items[seq[ln.first + k]];
        sum += overhead(it) + (it.frozen ? it.target : it.base);
      }
      return sum;
    };
    const LayoutUnit initial_free = inner - used();
    for (uint32_t round = 0; round <= ln.count; ++round) {
      int64_t factors = 0, weights = 0;
      uint32_t open = 0;
      for (uint32_t k = 0; k < ln.count; ++k) {
        const Item& it = items[seq[ln.first + k]];
        if (it.frozen) continue;
        ++open;
        factors += growing ? it.grow : it.shrink;
        weights += growing ? it.grow : static_cast<int64_t>(it.shrink) * min(it.base, LayoutUnit::from_px(1 << 18)).raw();
      }
      if (open == 0) break;
      LayoutUnit remaining = inner - used();
      if (factors < 100) {   // (flex factors that add up to less than one take only that part of the free space)
        const LayoutUnit part = initial_free.scaled(static_cast<int32_t>(factors), 100);
        if (part.abs() < remaining.abs()) remaining = part;
      }
      if (!remaining.is_zero() && weights > 0) {
        for (uint32_t k = 0; k < ln.count; ++k) {
          Item& it = items[seq[ln.first + k]];
          if (it.frozen) continue;
          const int64_t w = growing ? it.grow : static_cast<int64_t>(it.shrink) * min(it.base, LayoutUnit::from_px(1 << 18)).raw();
          it.target = max(it.base + share(remaining, w, weights), LayoutUnit());
        }
      } else {
        for (uint32_t k = 0; k < ln.count; ++k) {
          Item& it = items[seq[ln.first + k]];
          if (!it.frozen) it.target = it.base;
        }
      }
      // min / max violations: freeze the items that were clamped the way the total violation goes
      LayoutUnit total;
      for (uint32_t k = 0; k < ln.count; ++k) {
        Item& it = items[seq[ln.first + k]];
        if (!it.frozen) total += clamp_main(it, it.target) - it.target;
      }
      for (uint32_t k = 0; k < ln.count; ++k) {
        Item& it = items[seq[ln.first + k]];
        if (it.frozen) continue;
        const LayoutUnit clamped = clamp_main(it, it.target);
        const bool freeze = total.is_zero() || (total > LayoutUnit() ? clamped > it.target : clamped < it.target);
        it.target = clamped;
        if (freeze) it.frozen = true;
      }
    }
    for (uint32_t k = 0; k < ln.count; ++k) items[seq[ln.first + k]].frozen = true;
  }

  static bool is_end(Place p) { return p == Place::FlexEnd || p == Place::End; }

  ChildrenResult run() {
    ChildrenResult result;
    Box& box = tree.box(cid);
    const Style& s = box.style;
    row = s.flex_direction == FlexDirection::Row || s.flex_direction == FlexDirection::RowReverse;
    rev_main = (s.flex_direction == FlexDirection::RowReverse || s.flex_direction == FlexDirection::ColumnReverse) != (row && s.direction == browser::Direction::Rtl);
    wrap = s.flex_wrap != FlexWrap::NoWrap;
    rev_cross = s.flex_wrap == FlexWrap::WrapReverse;
    def_h = c.child_cb_height_known_;
    height = c.child_cb_height_;
    main_gap = row ? gap_of(s.column_gap, cw, true) : gap_of(s.row_gap, height, def_h);
    cross_gap = row ? gap_of(s.row_gap, height, def_h) : gap_of(s.column_gap, cw, true);

    collect();
    if (c.failed_ || seq.count() == 0) return result;
    result.empty = false;
    for (uint64_t k = 0; k < seq.count() && !c.failed_; ++k) prepare(items[seq[k]]);
    if (c.failed_) return result;

    // --- lines and the main size of the container
    const bool main_definite = row || def_h;
    make_lines(row ? cw : main_definite ? height : LayoutUnit::max());
    if (c.failed_ || lines.count() == 0) return result;
    LayoutUnit main_inner = row ? cw : height;
    if (!main_definite) main_inner = c.clamp_content_height(box, lines[0].hyp_sum);   // (a column of unknown height is as high as its items, within min and max)
    for (uint64_t l = 0; l < lines.count(); ++l) flex_line(lines[l], main_inner);

    // --- cross sizes: the items at their resolved main sizes
    const bool cross_definite = !row || def_h;
    const LayoutUnit cross_known = row ? height : cw;
    for (uint64_t l = 0; l < lines.count() && !c.failed_; ++l) {
      for (uint32_t k = 0; k < lines[l].count; ++k) {
        Item& it = items[seq[lines[l].first + k]];
        Box& b = tree.box(it.id);
        if (row) {
          // (one line of known height: the items that stretch can be laid out at their final height straight away)
          const bool stretch_now = !wrap && def_h && it.align == Place::Stretch && it.cross_auto && !it.auto_cs && !it.auto_ce;
          const LayoutUnit ch = c.clamp_content_height(b, max(height - it.cs - it.ce - it.cross_bp, LayoutUnit()));
          layout_item(it, it.target, stretch_now, ch);
        } else if (!it.measured || it.nat_main != it.target) {
          layout_item(it, it.cross, true, it.target);
        }
        it.outer_cross = it.cs + cross_border(it) + it.ce;
      }
    }
    if (c.failed_) return result;
    LayoutUnit lines_cross;
    for (uint64_t l = 0; l < lines.count(); ++l) {
      Line& ln = lines[l];
      LayoutUnit max_asc, max_desc, other;
      for (uint32_t k = 0; k < ln.count; ++k) {
        Item& it = items[seq[ln.first + k]];
        if (row && it.align == Place::Baseline && !it.auto_cs && !it.auto_ce) {
          const Box& b = tree.box(it.id);
          it.ascent = it.cs + (b.has_baseline ? b.baseline : b.height);
          max_asc = max(max_asc, it.ascent);
          max_desc = max(max_desc, it.outer_cross - it.ascent);
        } else {
          other = max(other, it.outer_cross);
        }
      }
      ln.max_ascent = max_asc;
      ln.cross = max(other, max_asc + max_desc);
    }
    if (!wrap) {   // the only line is as high as the container when that is known, else as its items, within min and max
      lines[0].cross = cross_definite ? cross_known : (row ? c.clamp_content_height(box, lines[0].cross) : lines[0].cross);
    }
    for (uint64_t l = 0; l < lines.count(); ++l) lines_cross += lines[l].cross + (l > 0 ? cross_gap : LayoutUnit());
    const LayoutUnit cross_inner = cross_definite ? cross_known : (row ? c.clamp_content_height(box, lines_cross) : lines_cross);

    // --- align-content: the lines of a wrapping container share the room left in the cross axis
    if (wrap) {
      const LayoutUnit free = cross_inner - lines_cross;
      const int32_t n = static_cast<int32_t>(lines.count());
      LayoutUnit lead, between = cross_gap;
      switch (s.align_content) {
        case Place::FlexEnd: case Place::End: lead = free; break;
        case Place::Center: lead = free / 2; break;
        case Place::SpaceBetween: if (free > LayoutUnit() && n > 1) between += free / (n - 1); break;
        case Place::SpaceAround: if (free > LayoutUnit()) { between += free / n; lead = free / n / 2; } break;
        case Place::SpaceEvenly: if (free > LayoutUnit()) { between += free / (n + 1); lead = free / (n + 1); } break;
        case Place::FlexStart: case Place::Start: case Place::Baseline: case Place::Left: case Place::Right: break;
        default:   // normal, stretch: every line gets an equal part
          if (free > LayoutUnit()) {
            for (uint64_t l = 0; l < lines.count(); ++l) lines[l].cross += (l + 1 == lines.count()) ? free - (free / n) * (n - 1) : free / n;
          }
          break;
      }
      LayoutUnit at = lead;
      for (uint64_t l = 0; l < lines.count(); ++l) {
        lines[l].cross_pos = at;
        at += lines[l].cross + between;
      }
    }

    // --- aligning the items in their lines (cross axis), then along the main axis
    for (uint64_t l = 0; l < lines.count() && !c.failed_; ++l) {
      Line& ln = lines[l];
      for (uint32_t k = 0; k < ln.count; ++k) {
        Item& it = items[seq[ln.first + k]];
        Box& b = tree.box(it.id);
        LayoutUnit off;
        if (it.auto_cs || it.auto_ce) {
          const LayoutUnit free = ln.cross - (it.cs + cross_border(it) + it.ce);
          if (free > LayoutUnit()) {
            if (it.auto_cs && it.auto_ce) {
              it.cs += free / 2;
              it.ce += free - free / 2;
            } else if (it.auto_cs) {
              it.cs += free;
            } else {
              it.ce += free;
            }
          }
        } else if (it.align == Place::Stretch && it.cross_auto) {
          const LayoutUnit want = max(ln.cross - it.cs - it.ce - it.cross_bp, LayoutUnit());
          if (row) {
            const LayoutUnit content = c.clamp_content_height(b, want);
            if (content + it.cross_bp != b.height) layout_item(it, it.target, true, content);
          } else {
            const LayoutUnit content = c.clamp_content_width(b, want);
            if (content != it.cross) {
              it.cross = content;
              layout_item(it, content, true, it.target);
            }
          }
        } else if (it.align == Place::Baseline) {
          off = ln.max_ascent - it.ascent;
        } else if (it.align == Place::Center) {
          off = (ln.cross - (it.cs + cross_border(it) + it.ce)) / 2;
        } else if (it.align == Place::FlexEnd) {
          off = ln.cross - (it.cs + cross_border(it) + it.ce);
        }
        it.outer_cross = it.cs + cross_border(it) + it.ce;
        it.cross_off = ln.cross_pos + off;
      }
      // main axis
      LayoutUnit used = main_gap * static_cast<int32_t>(ln.count - 1);
      int32_t autos = 0;
      for (uint32_t k = 0; k < ln.count; ++k) {
        const Item& it = items[seq[ln.first + k]];
        used += outer_main(it);
        autos += (it.auto_ms ? 1 : 0) + (it.auto_me ? 1 : 0);
      }
      LayoutUnit free = main_inner - used;
      LayoutUnit lead, between = main_gap;
      if (autos > 0 && free > LayoutUnit()) {
        LayoutUnit left = free;
        int32_t remaining = autos;
        for (uint32_t k = 0; k < ln.count; ++k) {
          Item& it = items[seq[ln.first + k]];
          if (it.auto_ms) { const LayoutUnit part = left / remaining; it.ms += part; left -= part; --remaining; }
          if (it.auto_me) { const LayoutUnit part = left / remaining; it.me += part; left -= part; --remaining; }
        }
      } else {
        Place jc = s.justify_content;
        if (row && (jc == Place::Left || jc == Place::Right)) jc = (jc == Place::Left) != rev_main ? Place::FlexStart : Place::FlexEnd;
        const int32_t n = static_cast<int32_t>(ln.count);
        switch (jc) {
          case Place::FlexEnd: case Place::End: lead = free; break;
          case Place::Center: lead = free / 2; break;
          case Place::SpaceBetween: if (free > LayoutUnit() && n > 1) between += free / (n - 1); break;
          case Place::SpaceAround: if (free > LayoutUnit()) { between += free / n; lead = free / n / 2; } break;
          case Place::SpaceEvenly: if (free > LayoutUnit()) { between += free / (n + 1); lead = free / (n + 1); } break;
          default: break;
        }
      }
      LayoutUnit at = lead;
      for (uint32_t k = 0; k < ln.count; ++k) {
        Item& it = items[seq[ln.first + k]];
        it.main_off = at;
        at += outer_main(it) + between;
      }
    }

    // --- the places
    for (uint64_t l = 0; l < lines.count(); ++l) {
      const Line& ln = lines[l];
      for (uint32_t k = 0; k < ln.count; ++k) {
        Item& it = items[seq[ln.first + k]];
        Box& b = tree.box(it.id);
        const LayoutUnit om = outer_main(it), oc = it.cs + cross_border(it) + it.ce;
        const LayoutUnit pm = rev_main ? main_inner - it.main_off - om + it.me : it.main_off + it.ms;
        const LayoutUnit pc = rev_cross ? cross_inner - it.cross_off - oc + it.ce : it.cross_off + it.cs;
        b.x = box.content_left() + (row ? pm : pc);
        b.y = box.content_top() + (row ? pc : pm);
        set_margins(it);
        c.apply_relative(b);
      }
    }

    // --- the baseline of the container: that of the items sharing a baseline in the first line, else of its first item
    if (s.overflow_x == Overflow::Visible && s.overflow_y == Overflow::Visible) {
      const Line& first = lines[0];
      const Item* pick = &items[seq[first.first]];
      if (row) {
        for (uint32_t k = 0; k < first.count; ++k) {
          const Item& it = items[seq[first.first + k]];
          if (it.align == Place::Baseline && !it.auto_cs && !it.auto_ce) { pick = &it; break; }
        }
      }
      const Box& b = tree.box(pick->id);
      box.baseline = b.y + (b.has_baseline ? b.baseline : b.height);
      box.has_baseline = true;
    }
    result.height = row ? cross_inner : main_inner;
    return result;
  }
};

ChildrenResult LayoutContext::layout_flex(BoxId id, LayoutUnit content_width, bool height_known, LayoutUnit height) {
  (void)height_known;
  (void)height;
  FlexLayout flex(*this, id);
  flex.cw = content_width;
  return flex.run();
}

void LayoutContext::flex_intrinsic_widths(BoxId id, LayoutUnit* min_content, LayoutUnit* max_content) {
  Box& b = tree_.box(id);
  const Style& s = b.style;
  const bool row = s.flex_direction == FlexDirection::Row || s.flex_direction == FlexDirection::RowReverse;
  const bool wrap = s.flex_wrap != FlexWrap::NoWrap;
  LayoutUnit sum_min, sum_max, big_min, big_max;
  uint32_t n = 0;
  for (uint64_t i = 0; i < b.children.count(); ++i) {
    const BoxId cid = b.children[i];
    Box& c = tree_.box(cid);
    if (c.kind == BoxKind::Marker || c.kind == BoxKind::Text || c.kind == BoxKind::LineBreak || c.out_of_flow) continue;
    LayoutUnit cmin, cmax;
    intrinsic_widths(cid, &cmin, &cmax);
    resolve_edges(c, LayoutUnit(), false);
    const LayoutUnit bp = c.border.horizontal() + c.padding.horizontal();
    LayoutUnit specified;
    const bool has_width = resolve_in(c.style.width, c, false, &specified);
    LayoutUnit lo = cmin, hi = cmax;
    if (has_width) lo = hi = c.style.sizing == BoxSizing::BorderBox ? max(specified - bp, LayoutUnit()) : specified;
    if (row && c.style.flex_basis.is_px()) {   // (the flex basis stands for the width an item is laid out with)
      hi = max(c.style.flex_basis.px100 > 0 ? LayoutUnit::from_px100(c.style.flex_basis.px100) : LayoutUnit(), LayoutUnit());
      if (c.style.sizing == BoxSizing::BorderBox) hi = max(hi - bp, LayoutUnit());
      if (!has_width) lo = min(lo, hi);
    }
    lo = clamp_content_width(c, lo);
    hi = clamp_content_width(c, hi);
    const LayoutUnit extra = bp + c.margin.horizontal();
    sum_min += lo + extra;
    sum_max += hi + extra;
    big_min = max(big_min, lo + extra);
    big_max = max(big_max, hi + extra);
    ++n;
  }
  if (!row) {
    *min_content = big_min;
    *max_content = big_max;
    return;
  }
  LayoutUnit gap;
  if (n > 1 && s.column_gap.is_px() && s.column_gap.px100 > 0) gap = LayoutUnit::from_px100(s.column_gap.px100) * static_cast<int32_t>(n - 1);
  *max_content = sum_max + gap;
  *min_content = wrap ? big_min : sum_min + gap;
}

}  // namespace layout
