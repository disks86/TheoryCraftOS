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

// Intrinsic sizes (CSS Sizing): the width a box wants when nothing wraps (max-content: the longest line of its content) and the narrowest it
// can be without overflowing (min-content: the longest word or unbreakable stretch). Floats, inline-blocks, table cells and flex items are
// sized from them.
#include "Layout/LayoutInternal.hpp"

namespace layout {

using browser::BoxSizing;

void LayoutContext::intrinsic_widths(BoxId id, LayoutUnit* min_content, LayoutUnit* max_content) {
  Box& b = tree_.box(id);
  if (b.intrinsic_known) {
    *min_content = b.min_content;
    *max_content = b.max_content;
    return;
  }
  resolve_edges(b, LayoutUnit(), false);
  LayoutUnit lo, hi;
  if (b.kind == BoxKind::Replaced) {
    LayoutUnit w, h;
    size_replaced(b, &w, &h);
    lo = hi = w;
  } else if (b.inline_content) {
    const bool wraps = style_wraps(b.style);
    List<Token> wide(tree_.allocator()), narrow(tree_.allocator());
    if (!collect_tokens(id, LayoutUnit::max(), TokenMode::MeasureMax, &wide) || !collect_tokens(id, LayoutUnit::max(), TokenMode::MeasureMin, &narrow)) {
      failed_ = true;
    } else {
      // max-content: the longest line when only forced breaks end lines (spaces at the start and end of a line do not count)
      LayoutUnit line, trailing;
      bool started = false;
      for (uint64_t i = 0; i < wide.count(); ++i) {
        const Token& t = wide[i];
        if (t.type == Token::Type::Break) {
          hi = max(hi, line - trailing);
          line = trailing = LayoutUnit();
          started = false;
          continue;
        }
        if (t.type == Token::Type::Space) {
          if (!started) continue;
          trailing += t.width;
        } else if (t.type == Token::Type::Word || t.type == Token::Type::Atomic) {
          started = true;
          trailing = LayoutUnit();
        }
        line += t.width;
      }
      hi = max(hi, line - trailing);
      // min-content: the widest stretch between two places a line may end
      LayoutUnit chunk;
      for (uint64_t i = 0; i < narrow.count(); ++i) {
        const Token& t = narrow[i];
        if (t.type == Token::Type::Break) {
          lo = max(lo, chunk);
          chunk = LayoutUnit();
          continue;
        }
        if (t.type == Token::Type::Space && !t.no_break) {
          lo = max(lo, chunk);   // (the space itself hangs at the end of the line and takes no room)
          chunk = LayoutUnit();
          continue;
        }
        chunk += t.width;
        if (break_after_token(narrow, i, wraps)) {
          lo = max(lo, chunk);
          chunk = LayoutUnit();
        }
      }
      lo = max(lo, chunk);
    }
  } else {
    for (uint64_t i = 0; i < b.children.count(); ++i) {
      const BoxId cid = b.children[i];
      Box& c = tree_.box(cid);
      if (c.kind == BoxKind::Marker || c.out_of_flow) continue;
      resolve_edges(c, LayoutUnit(), false);
      LayoutUnit cmin, cmax;
      intrinsic_widths(cid, &cmin, &cmax);
      const LayoutUnit bp = c.border.horizontal() + c.padding.horizontal();
      LayoutUnit specified;
      if (resolve_in(c.style.width, c, false, &specified)) cmin = cmax = c.style.sizing == BoxSizing::BorderBox ? max(specified - bp, LayoutUnit()) : specified;
      cmin = clamp_content_width(c, cmin);
      cmax = clamp_content_width(c, cmax);
      const LayoutUnit extra = bp + c.margin.horizontal();
      lo = max(lo, cmin + extra);
      hi = max(hi, cmax + extra);
    }
  }
  if (hi < lo) hi = lo;
  b.min_content = lo;
  b.max_content = hi;
  b.intrinsic_known = true;
  *min_content = lo;
  *max_content = hi;
}

LayoutUnit LayoutContext::atomic_outer_width(BoxId id, bool max_content) {
  Box& b = tree_.box(id);
  resolve_edges(b, LayoutUnit(), false);
  const LayoutUnit bp = b.border.horizontal() + b.padding.horizontal();
  LayoutUnit w;
  if (b.kind == BoxKind::Replaced) {
    LayoutUnit h;
    size_replaced(b, &w, &h);
  } else if (LayoutUnit specified; resolve_in(b.style.width, b, false, &specified)) {
    w = clamp_content_width(b, b.style.sizing == BoxSizing::BorderBox ? max(specified - bp, LayoutUnit()) : specified);
  } else {
    LayoutUnit mn, mx;
    intrinsic_widths(id, &mn, &mx);
    w = clamp_content_width(b, max_content ? mx : mn);
  }
  return b.margin.horizontal() + bp + w;
}

}  // namespace layout
