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

// Boxes: elements that have borders, sizes, padding on the right, centred margins, flexbox or grid layout are drawn into a box of their own - lines of character
// cells, a column being 8 pixels wide and a line 16 high - and the box is put on the page. What is inside a box is drawn by a renderer of its own (like the cells
// of a table), fitted into the box's width, and then wrapped in its padding, border and background. Flex and grid containers draw their items the same way and put
// the boxes side by side.
#include "Browser/RendererInternal.hpp"

namespace browser {

namespace {

constexpr uint32_t kWide = 400;   // the width a box is measured at when it may be as wide as it likes

uint32_t clamp_cells(int32_t v, uint32_t cap) { return v <= 0 ? 0 : static_cast<uint32_t>(v) > cap ? cap : static_cast<uint32_t>(v); }

// The columns / lines of a pixel length (0 for anything else).
uint32_t cols_of(const Length& l, uint32_t cap = 400) { return l.kind == Length::Px ? clamp_cells(l.columns(), cap) : 0; }
uint32_t lines_of(const Length& l, uint32_t cap = 400) { return l.kind == Length::Px ? clamp_cells(l.lines(), cap) : 0; }

char border_horizontal(BorderStyle s) {
  switch (s) {
    case BorderStyle::Dotted: return '.';
    case BorderStyle::Double: return '=';
    default: return '-';
  }
}
char border_vertical(BorderStyle s) {
  switch (s) {
    case BorderStyle::Dotted:
    case BorderStyle::Dashed: return ':';
    case BorderStyle::Double: return '#';
    default: return '|';
  }
}

}  // namespace

DocumentRenderer::Edges DocumentRenderer::edges_of(const Style& s) const {
  Edges e;
  auto visible = [&](int i) { return s.border_style[i] != BorderStyle::None && s.border_style[i] != BorderStyle::Hidden && s.border_px100[i] > 0; };
  e.border_top = visible(0) ? 1 : 0;
  e.border_right = visible(1) ? 1 : 0;
  e.border_bottom = visible(2) ? 1 : 0;
  e.border_left = visible(3) ? 1 : 0;
  e.padding_top = lines_of(s.padding[0]);
  e.padding_right = cols_of(s.padding[1]);
  e.padding_bottom = lines_of(s.padding[2]);
  e.padding_left = cols_of(s.padding[3]);
  return e;
}

bool DocumentRenderer::wants_box(const dom::Node* n, const Style& s) const {
  if (s.display == Display::None || s.display == Display::ListItem) return false;
  if (s.display == Display::Inline && !s.inline_box) return false;
  if (tag_in(n, {"html", "body", "table", "thead", "tbody", "tfoot", "tr", "td", "th", "caption", "colgroup", "col", "img", "video", "audio", "canvas", "svg", "input", "textarea",
                 "select", "button", "progress", "meter", "iframe", "br", "hr", "option", "optgroup", "dialog", "details", "summary"}))
    return false;
  const Edges e = edges_of(s);
  const bool border = e.border_top || e.border_right || e.border_bottom || e.border_left;
  const bool sized = !s.width.is_auto() || s.max_width.is_px() || (s.min_width.is_px() && s.min_width.px100 > 0) || s.height.is_px() || (s.min_height.is_px() && s.min_height.px100 > 0) ||
                     s.max_height.is_px();
  if (s.layout == Layout::Flex || s.layout == Layout::Grid || s.layout == Layout::Table) return true;
  if (s.column_count > 0 || s.column_width.is_px()) return true;
  if (s.inline_box) return border || sized || e.padding_left || e.padding_right || e.padding_top || e.padding_bottom || cols_of(s.margin[1]) || cols_of(s.margin[3]);
  if (border || sized) return true;
  if (e.padding_right > 0 || cols_of(s.margin[1]) > 0) return true;
  return false;
}

Style DocumentRenderer::inside_style(const Style& s, uint32_t content_cols) const {
  Style o = s;
  o.display = s.display == Display::Inline ? Display::Block : s.display;
  o.layout = Layout::Flow;
  o.inline_box = false;
  for (int i = 0; i < 4; ++i) {
    o.margin[i] = Length::px(0);
    o.padding[i] = Length::px(0);
    o.border_px100[i] = 0;
    o.border_style[i] = BorderStyle::None;
    o.border_color[i] = 0;
  }
  o.margin_top = o.margin_bottom = o.padding_top = o.padding_bottom = o.margin_left = o.padding_left = 0;
  o.width = o.height = o.min_width = o.min_height = Length();
  o.max_width = o.max_height = Length::of(Length::None);
  o.width_px = o.height_px = 0;
  o.width_pct = 0;
  o.indent = 0;
  o.float_side = Float::None;
  o.position = Position::Static;
  o.text_ellipsis = false;
  o.column_count = 0;
  o.column_width = Length();
  if (s.own_background) {
    o.background = s.own_background_color;
    o.own_background = false;
  }
  o.background_block = false;
  o.content_px = static_cast<int32_t>(content_cols) * 8;
  return o;
}

// Fits a drawn document into lines of `width` columns. Every line is padded to the width with spaces (shaded with the background) unless width is a measure.
bool DocumentRenderer::fit_document(const HtmlDocument& d, uint32_t width, uint32_t background, Raster* out) {
  allocators::IAllocator& a = out_.allocator();
  PageLayout layout(a);
  if (!layout.build(d, width < 2 ? 2 : width)) return false;
  List<int32_t> link_map(a);
  for (uint32_t i = 0; i < d.link_count(); ++i) link_map.push_back(-2);
  out->width = width;
  for (uint32_t line = 0; line < layout.line_count() && !failed_; ++line) {
    uint32_t used = 0, fill = background;
    bool first = true;
    for (uint32_t p = layout.first_piece(line); p < layout.end_piece(line); ++p) {
      const Piece& piece = layout.piece(p);
      const Run& run = d.run(piece.run);
      if (first && (run.flags & kRunBlockBg) && run.background) fill = run.background;
      first = false;
      if (piece.column > used) {
        LinePiece gap(a);
        gap.text.assign(StringView(String(a).view()));
        for (uint32_t k = used; k < piece.column; ++k) gap.text.push_back(' ');
        gap.background = fill;
        failed_ |= !out->pieces.push_back(util::move(gap));
        used = piece.column;
      }
      LinePiece lp(a);
      if (run.flags & kRunRule) {
        for (uint32_t k = 0; k < width; ++k) lp.text.push_back('-');
        lp.color = run.color;
        used = width;
      } else if (run.object) {
        const char* label = run.object == 2 ? "[video]" : run.object == 3 ? "[canvas]" : run.object == 4 ? "[audio]" : "[image]";
        if (piece.from == 0) lp.text.assign(label);
        while (lp.text.size() < piece.length) lp.text.push_back(' ');
        if (lp.text.size() > piece.length) lp.text.truncate(piece.length);
        used = piece.column + static_cast<uint32_t>(lp.text.size());
      } else {
        lp.text.assign(d.run_text(piece.run).substr(piece.from, piece.length));
        lp.flags = static_cast<uint8_t>(run.flags & ~(kRunBlockBg | kRunPre | kRunRule));
        lp.level = run.level;
        lp.color = run.color;
        lp.background = run.background;
        lp.node = run.node;
        if (run.link >= 0 && static_cast<uint32_t>(run.link) < link_map.count()) {
          if (link_map[static_cast<uint32_t>(run.link)] == -2) link_map[static_cast<uint32_t>(run.link)] = out_.add_link(d.link(static_cast<uint32_t>(run.link)));
          lp.link = link_map[static_cast<uint32_t>(run.link)];
        }
        used = piece.column + static_cast<uint32_t>(lp.text.size());
      }
      failed_ |= !out->pieces.push_back(util::move(lp));
    }
    if (width < kWide * 2) {
      if (used < width) {
        LinePiece pad(a);
        for (uint32_t k = used; k < width; ++k) pad.text.push_back(' ');
        pad.background = fill;
        failed_ |= !out->pieces.push_back(util::move(pad));
      }
    }
    failed_ |= !out->ends.push_back(static_cast<uint32_t>(out->pieces.count()));
  }
  if (layout.line_count() == 0 && !failed_) {   // nothing in it: still one line
    LinePiece pad(a);
    for (uint32_t k = 0; k < width && width < kWide * 2; ++k) pad.text.push_back(' ');
    pad.background = background;
    failed_ |= !out->pieces.push_back(util::move(pad));
    failed_ |= !out->ends.push_back(static_cast<uint32_t>(out->pieces.count()));
  }
  return !failed_;
}

bool DocumentRenderer::draw_content(const dom::Node* n, const Style& inside, uint32_t content_cols, Raster* out) {
  allocators::IAllocator& a = out_.allocator();
  UniquePointer<HtmlDocument> doc = UniquePointer<HtmlDocument>::make(a, a);
  if (!doc) return false;
  DocumentRenderer sub(*doc, sheet_, options_);
  sub.root_ = cur().inherit();
  sub.root_.indent = 0;
  sub.root_.content_px = static_cast<int32_t>(content_cols) * 8;
  sub.container_cols_ = content_cols;
  sub.box_root_ = n;
  sub.box_style_ = &inside;
  sub.box_depth_ = box_depth_ + 1;
  sub.table_depth_ = table_depth_;
  sub.modal_pass_ = modal_pass_;
  sub.measuring_ = measuring_;
  sub.walk(n);
  sub.flush();
  if (sub.failed_) return false;
  used_boxes_ = true;
  return fit_document(*doc, content_cols, inside.background_block ? inside.background : (inside.own_background ? inside.own_background_color : 0), out);
}

void DocumentRenderer::decorate(Raster* r, const Style& s, const Edges& e, uint32_t min_outer_lines) {
  allocators::IAllocator& a = out_.allocator();
  const uint32_t inner_width = r->width;
  const uint32_t width = inner_width + e.horizontal();
  const uint32_t bg = s.own_background ? s.own_background_color : 0;
  const uint32_t ink = s.color;
  Raster out(a);
  out.width = width;
  auto spaces = [&](uint32_t n, uint32_t background) {
    LinePiece p(a);
    for (uint32_t i = 0; i < n; ++i) p.text.push_back(' ');
    p.background = background;
    return p;
  };
  auto edge_piece = [&](char c, uint32_t count, int side) {
    LinePiece p(a);
    for (uint32_t i = 0; i < count; ++i) p.text.push_back(c);
    p.color = s.border_color[side] ? s.border_color[side] : ink;
    return p;
  };
  auto close = [&]() { failed_ |= !out.ends.push_back(static_cast<uint32_t>(out.pieces.count())); };
  auto push = [&](LinePiece&& p) {
    if (p.text.empty()) return;
    failed_ |= !out.pieces.push_back(util::move(p));
  };
  // a blank line of the padding (with the sides of the border)
  auto blank_line = [&]() {
    if (e.border_left) push(edge_piece(border_vertical(s.border_style[3]), 1, 3));
    push(spaces(inner_width + e.padding_left + e.padding_right, bg));
    if (e.border_right) push(edge_piece(border_vertical(s.border_style[1]), 1, 1));
    close();
  };
  auto horizontal_line = [&](int side) {   // 0: top, 2: bottom
    const char h = border_horizontal(s.border_style[side]);
    if (e.border_left) push(edge_piece(e.border_left && (side == 0 ? true : true) ? '+' : h, 1, 3));
    push(edge_piece(h, inner_width + e.padding_left + e.padding_right, side));
    if (e.border_right) push(edge_piece('+', 1, 1));
    close();
  };
  if (e.border_top) horizontal_line(0);
  for (uint32_t i = 0; i < e.padding_top; ++i) blank_line();
  const uint32_t content_lines = r->lines();
  const uint32_t min_content = min_outer_lines > e.vertical() ? min_outer_lines - e.vertical() : 0;
  const uint32_t total_content = content_lines > min_content ? content_lines : min_content;
  for (uint32_t line = 0; line < content_lines && !failed_; ++line) {
    if (e.border_left) push(edge_piece(border_vertical(s.border_style[3]), 1, 3));
    if (e.padding_left) push(spaces(e.padding_left, bg));
    for (uint32_t i = line ? r->ends[line - 1] : 0; i < r->ends[line]; ++i) {
      LinePiece& p = r->pieces[i];
      if (!p.background && bg) p.background = bg;
      push(util::move(p));
    }
    if (e.padding_right) push(spaces(e.padding_right, bg));
    if (e.border_right) push(edge_piece(border_vertical(s.border_style[1]), 1, 1));
    close();
  }
  for (uint32_t line = content_lines; line < total_content && !failed_; ++line) {
    // (blank lines of the content area: the borders at the sides, the content width of spaces)
    if (e.border_left) push(edge_piece(border_vertical(s.border_style[3]), 1, 3));
    push(spaces(inner_width + e.padding_left + e.padding_right, bg));
    if (e.border_right) push(edge_piece(border_vertical(s.border_style[1]), 1, 1));
    close();
  }
  for (uint32_t i = 0; i < e.padding_bottom; ++i) blank_line();
  if (e.border_bottom) horizontal_line(2);
  failed_ |= !r->take(out);
}

void DocumentRenderer::emit_raster(Raster& r, uint32_t left, uint32_t break_before) {
  allocators::IAllocator& a = out_.allocator();
  if (break_before > pending_) pending_ = static_cast<uint8_t>(break_before > 250 ? 250 : break_before);
  List<LinePiece> row(a);
  for (uint32_t line = 0; line < r.lines() && !failed_; ++line) {
    if (left > 0) table_spaces(row, left, 0, 0);
    for (uint32_t i = line ? r.ends[line - 1] : 0; i < r.ends[line]; ++i) failed_ |= !row.push_back(util::move(r.pieces[i]));
    // plain spaces at the end go (shaded or coloured ones stay: they are the box)
    while (row.count() > 0) {
      LinePiece& last = row[row.count() - 1];
      if (last.background != 0 || last.color != 0) break;
      bool blank = true;
      for (uint64_t k = 0; k < last.text.size() && blank; ++k) blank = last.text[k] == ' ';
      if (!blank) break;
      row.remove_at(row.count() - 1);
    }
    table_line(row, false);
  }
}

bool DocumentRenderer::build_box(const dom::Node* n, const Style& s, uint32_t avail, uint32_t outer_width, uint32_t min_outer_lines, Raster* out) {
  const Edges e = edges_of(s);
  const uint32_t ml = cols_of(s.margin[3]), mr = cols_of(s.margin[1]);
  const uint32_t room = avail > ml + mr + 1 ? avail - ml - mr : 1;
  uint32_t outer = outer_width;
  const uint32_t horizontal = e.horizontal();
  auto outer_of = [&](const Length& l) -> uint32_t {   // a width as an outer width
    const uint32_t w = cols_of(l, 4000);
    return s.sizing == BoxSizing::BorderBox ? w : w + horizontal;
  };
  if (outer == 0) {
    const bool shrink = s.inline_box || s.float_side != Float::None || s.width.kind == Length::FitContent || s.width.kind == Length::MinContent ||
                        s.width.kind == Length::MaxContent || s.layout == Layout::Table || measuring_;
    if (s.width.is_px()) {
      outer = outer_of(s.width);
    } else if (shrink) {
      uint32_t min_c = 0, max_c = 0;
      if (!measure_box(n, s, room > horizontal ? room - horizontal : 1, &min_c, &max_c)) return false;
      outer = (s.width.kind == Length::MinContent ? min_c : max_c) + horizontal;
    } else {
      outer = room;
    }
    if (s.max_width.is_px() && outer > outer_of(s.max_width)) outer = outer_of(s.max_width);
    if (s.min_width.is_px() && outer < outer_of(s.min_width)) outer = outer_of(s.min_width);
    if (outer > room) outer = room;
  }
  if (outer < horizontal + 1) outer = horizontal + 1;
  const uint32_t content_cols = outer - horizontal;
  const Style inside = inside_style(s, content_cols);
  // the height asked for (content box unless border-box)
  uint32_t min_lines = min_outer_lines, max_lines = 0;
  if (s.height.is_px()) {
    const uint32_t h = lines_of(s.height, 2000);
    const uint32_t outer_h = s.sizing == BoxSizing::BorderBox ? h : h + e.vertical();
    if (outer_h > min_lines) min_lines = outer_h;
    max_lines = outer_h;
  }
  if (s.min_height.is_px()) {
    const uint32_t h = lines_of(s.min_height, 2000);
    const uint32_t outer_h = s.sizing == BoxSizing::BorderBox ? h : h + e.vertical();
    if (outer_h > min_lines) min_lines = outer_h;
  }
  if (s.max_height.is_px() && (max_lines == 0 || lines_of(s.max_height, 2000) + (s.sizing == BoxSizing::BorderBox ? 0 : e.vertical()) < max_lines)) {
    max_lines = lines_of(s.max_height, 2000) + (s.sizing == BoxSizing::BorderBox ? 0 : e.vertical());
    if (min_lines > max_lines) min_lines = max_lines;
  }
  const uint32_t content_lines = min_lines > e.vertical() ? min_lines - e.vertical() : 0;
  allocators::IAllocator& a = out_.allocator();
  Raster content(a);
  bool done = false;
  if (s.layout == Layout::Flex) done = layout_flex(n, s, content_cols, content_lines, &content);
  else if (s.layout == Layout::Grid) done = layout_grid(n, s, content_cols, content_lines, &content);
  else if (s.column_count > 0 || s.column_width.is_px()) done = layout_columns(n, s, content_cols, &content);
  if (!done) {
    if (failed_) return false;
    content.clear();
    if (!draw_content(n, inside, content_cols, &content)) return false;
  }
  content.width = content_cols;
  // overflow: hidden with a height clips; an ellipsis marks lines cut at the right
  const bool clip = s.overflow_y != Overflow::Visible;
  if (max_lines > 0 && clip) {
    const uint32_t keep = max_lines > e.vertical() ? max_lines - e.vertical() : 0;
    while (content.lines() > keep && content.lines() > 0) {
      const uint32_t first = content.lines() > 1 ? content.ends[content.lines() - 2] : 0;
      while (content.pieces.count() > first) content.pieces.remove_at(content.pieces.count() - 1);
      content.ends.remove_at(content.ends.count() - 1);
    }
  }
  decorate(&content, s, e, min_lines);
  failed_ |= !out->take(content);
  return !failed_;
}

bool DocumentRenderer::measure_box(const dom::Node* n, const Style& s, uint32_t avail, uint32_t* min_content, uint32_t* max_content) {
  allocators::IAllocator& a = out_.allocator();
  const bool saved = measuring_;
  measuring_ = true;
  if (s.layout == Layout::Flex || s.layout == Layout::Grid) {   // (a container is measured by laying it out: its items go side by side)
    Raster r(a);
    const uint32_t saved_min = measured_min_, saved_max = measured_max_;
    const bool ok = s.layout == Layout::Flex ? layout_flex(n, s, kWide, 0, &r) : layout_grid(n, s, kWide, 0, &r);
    const uint32_t mn = measured_min_, mx = measured_max_;
    measured_min_ = saved_min;
    measured_max_ = saved_max;
    measuring_ = saved;
    if (!ok) return false;
    *max_content = mx < 1 ? 1 : mx > avail ? avail : mx;
    *min_content = mn < 1 ? 1 : mn > *max_content ? *max_content : mn;
    return true;
  }
  const uint32_t wide = kWide;
  UniquePointer<HtmlDocument> doc = UniquePointer<HtmlDocument>::make(a, a);
  Style inside = inside_style(s, wide);
  bool ok = true;
  uint32_t longest_line = 0, longest_word = 0;
  if (!doc) ok = false;
  else {
    DocumentRenderer sub(*doc, sheet_, options_);
    sub.root_ = cur().inherit();
    sub.root_.indent = 0;
    sub.root_.content_px = static_cast<int32_t>(wide) * 8;
    sub.container_cols_ = wide;
    sub.box_root_ = n;
    sub.box_style_ = &inside;
    sub.box_depth_ = box_depth_ + 1;
    sub.table_depth_ = table_depth_;
    sub.modal_pass_ = modal_pass_;
    sub.measuring_ = true;
    sub.walk(n);
    sub.flush();
    ok = !sub.failed_;
    if (ok) {
      Raster r(a);
      ok = fit_document(*doc, wide * 4, 0, &r);   // (so wide that nothing is padded)
      for (uint32_t line = 0; ok && line < r.lines(); ++line) {
        uint32_t used = 0;
        for (uint32_t i = line ? r.ends[line - 1] : 0; i < r.ends[line]; ++i) used += static_cast<uint32_t>(r.pieces[i].text.size());
        if (used > longest_line) longest_line = used;
      }
      for (uint64_t k = 0; k < doc->run_count(); ++k) {
        const Run& run = doc->run(k);
        const StringView t = doc->run_text(k);
        uint64_t word = 0;
        for (uint64_t j = 0; j <= t.size(); ++j) {
          const bool cut = j == t.size() || t[j] == ' ' || ((run.flags & kRunPre) && t[j] == '\n') || (run.exact && t[j] == '\n');
          if (cut) {
            if (word > longest_word) longest_word = static_cast<uint32_t>(word);
            word = 0;
          } else {
            ++word;
          }
        }
        // (an exact run is a line that does not break: its whole length is its smallest width)
        if (run.exact && t.size() > longest_word) longest_word = static_cast<uint32_t>(t.size());
      }
    }
  }
  measuring_ = saved;
  if (!ok) return false;
  *max_content = longest_line < 1 ? 1 : longest_line > avail ? avail : longest_line;
  *min_content = longest_word < 1 ? 1 : longest_word > *max_content ? *max_content : longest_word;
  return true;
}

bool DocumentRenderer::render_box(const dom::Node* n, const Style& s) {
  flush();
  allocators::IAllocator& a = out_.allocator();
  const Style& parent = cur();
  const uint32_t avail = container_cols_ > parent.indent + 1 ? container_cols_ - parent.indent : 1;
  Raster box(a);
  if (!build_box(n, s, avail, 0, 0, &box)) return false;
  const uint32_t ml = cols_of(s.margin[3]), mr = cols_of(s.margin[1]);
  const bool ml_auto = s.margin[3].is_auto(), mr_auto = s.margin[1].is_auto();
  uint32_t left = ml;
  if (box.width + ml + mr < avail) {
    const uint32_t spare = avail - box.width;
    if (ml_auto && mr_auto) left = spare / 2;
    else if (ml_auto) left = spare > mr ? spare - mr : 0;
  }
  if (left + box.width > avail && avail > box.width) left = avail - box.width;
  // text-align moves a box that is narrower than its container only when it is inline-level (a block box stays where its margins put it)
  if ((s.inline_box || s.float_side != Float::None) && box.width < avail && left == ml) {
    const uint32_t spare = avail - box.width - ml;
    if (parent.align == 1) left += spare / 2;
    else if (parent.align == 2) left += spare;
    if (s.float_side == Float::Right) left = avail - box.width;
  }
  block(static_cast<uint8_t>(1 + lines_of(s.margin[0], 6)));
  emit_raster(box, left, 0);
  block(static_cast<uint8_t>(1 + lines_of(s.margin[2], 6)));
  used_boxes_ = true;
  ended_space_ = true;
  return !failed_;
}

// A bare piece of text that is an item of a flex or grid container: drawn into `width` columns with the container's text style.
bool DocumentRenderer::draw_text_item(const dom::Node* text, const Style& inside, uint32_t width, Raster* out) {
  allocators::IAllocator& a = out_.allocator();
  UniquePointer<HtmlDocument> doc = UniquePointer<HtmlDocument>::make(a, a);
  if (!doc) return false;
  DocumentRenderer sub(*doc, sheet_, options_);
  sub.root_ = inside.inherit();
  sub.root_.indent = 0;
  sub.root_.content_px = static_cast<int32_t>(width) * 8;
  sub.container_cols_ = width;
  sub.walk(text);
  sub.flush();
  if (sub.failed_) return false;
  return fit_document(*doc, width, 0, out);
}

// --- Flexbox ----------------------------------------------------------------------------------------

bool DocumentRenderer::collect_items(const dom::Node* container, const Style& inside, List<UniquePointer<Item>>* items, List<const dom::Node*>* out_of_flow, uint32_t depth) {
  allocators::IAllocator& a = out_.allocator();
  for (const dom::Node* c = container->first_child; c && items->count() < 400; c = c->next) {
    if (c->type == dom::NodeType::Text) {
      bool blank = true;
      for (const char ch : c->data.view()) blank = blank && is_space(ch);
      if (blank) continue;
      auto item = UniquePointer<Item>::make(a, a);
      if (!item) return false;
      item->node = c;
      item->style = inside.inherit();
      item->style.display = Display::Block;
      failed_ |= !items->push_back(util::move(item));
      continue;
    }
    if (!c->is_element()) continue;
    Style cs = derive_style(inside, c, sheet_);
    if (cs.display == Display::None) continue;
    if (tag_in(c, {"script", "style", "template"})) continue;
    if (cs.contents && depth < 4) {   // display: contents: its children are the items
      Style flattened = cs;
      if (!collect_items(c, flattened, items, out_of_flow, depth + 1)) return false;
      continue;
    }
    if (cs.position == Position::Absolute || cs.position == Position::Fixed) {
      if (out_of_flow) out_of_flow->push_back(c);
      continue;
    }
    auto item = UniquePointer<Item>::make(a, a);
    if (!item) return false;
    item->node = c;
    item->style = cs;
    if (cs.own_background) {
      item->style.background = cs.own_background_color;
      item->style.background_block = cs.display != Display::Inline;
    }
    if (item->style.display == Display::Inline) item->style.display = Display::Block;   // (items are blockified)
    failed_ |= !items->push_back(util::move(item));
  }
  return !failed_;
}

// Margins, edges and the content sizes of an item.
bool DocumentRenderer::prepare_item(Item* item, const Style& inside, uint32_t content_cols) {
  const Style& st = item->style;
  item->edges = edges_of(st);
  item->margin_left = cols_of(st.margin[3]);
  item->margin_right = cols_of(st.margin[1]);
  item->margin_top = lines_of(st.margin[0]);
  item->margin_bottom = lines_of(st.margin[2]);
  item->auto_left = st.margin[3].is_auto();
  item->auto_right = st.margin[1].is_auto();
  item->auto_top = st.margin[0].is_auto();
  item->auto_bottom = st.margin[2].is_auto();
  item->grow = st.flex_grow_x100;
  item->shrink = st.flex_shrink_x100;
  item->order = st.order;
  (void)inside;
  if (item->node->type == dom::NodeType::Text) {
    // a bare run of text: measured with the container's own style
    allocators::IAllocator& a = out_.allocator();
    UniquePointer<HtmlDocument> doc = UniquePointer<HtmlDocument>::make(a, a);
    if (!doc) return false;
    DocumentRenderer sub(*doc, sheet_, options_);
    sub.root_ = inside.inherit();
    sub.root_.indent = 0;
    sub.container_cols_ = kWide;
    sub.walk(item->node);
    sub.flush();
    if (sub.failed_) return false;
    uint32_t widest = 0, word = 0;
    for (uint64_t k = 0; k < doc->run_count(); ++k) {
      const StringView t = doc->run_text(k);
      widest += static_cast<uint32_t>(t.size());
      uint64_t w = 0;
      for (uint64_t j = 0; j <= t.size(); ++j) {
        if (j == t.size() || t[j] == ' ') {
          if (w > word) word = static_cast<uint32_t>(w);
          w = 0;
        } else {
          ++w;
        }
      }
    }
    // (the white space at the ends of the text is not part of the item)
    item->max_content = widest > 0 && doc->run_count() ? static_cast<uint32_t>(doc->run_text(0).trimmed().size()) : 0;
    uint32_t total = 0;
    for (uint64_t k = 0; k < doc->run_count(); ++k) total += static_cast<uint32_t>(doc->run_text(k).size());
    item->max_content = total;
    if (item->max_content > 0 && doc->run_count()) {
      const StringView first = doc->run_text(0);
      if (first.starts_with(' ') && doc->run_count() == 1) item->max_content = static_cast<uint32_t>(first.size()) - 1;
      if (doc->run_count() == 1 && doc->run_text(0).ends_with(' ') && item->max_content > 0) --item->max_content;
    }
    item->min_content = word < 1 ? 1 : word;
    if (item->max_content < 1) item->max_content = 1;
    if (item->min_content > item->max_content) item->min_content = item->max_content;
    return true;
  }
  uint32_t min_c = 0, max_c = 0;
  if (!measure_box(item->node, st, kWide, &min_c, &max_c)) return false;
  item->min_content = min_c;
  item->max_content = max_c;
  (void)content_cols;
  return true;
}

bool DocumentRenderer::layout_flex(const dom::Node* n, const Style& s, uint32_t content_cols, uint32_t content_lines, Raster* out) {
  allocators::IAllocator& a = out_.allocator();
  const Style inside = inside_style(s, content_cols);
  List<UniquePointer<Item>> items(a);
  List<const dom::Node*> loose(a);
  if (!collect_items(n, inside, &items, &loose, 0)) return false;
  const bool column = s.flex_direction == FlexDirection::Column || s.flex_direction == FlexDirection::ColumnReverse;
  const bool reversed = s.flex_direction == FlexDirection::RowReverse || s.flex_direction == FlexDirection::ColumnReverse;
  const uint32_t col_gap = cols_of(s.column_gap), row_gap = lines_of(s.row_gap);
  out->clear();
  out->width = content_cols;
  if (items.count() == 0) {
    LinePiece pad(a);
    for (uint32_t k = 0; k < content_cols; ++k) pad.text.push_back(' ');
    failed_ |= !out->pieces.push_back(util::move(pad));
    failed_ |= !out->ends.push_back(1);
    return true;
  }
  for (uint64_t i = 0; i < items.count() && !failed_; ++i)
    if (!prepare_item(items[i].get(), inside, content_cols)) return false;
  // order (stable)
  for (uint64_t i = 1; i < items.count(); ++i) {
    uint64_t k = i;
    while (k > 0 && items[k - 1]->order > items[k]->order) {
      UniquePointer<Item> tmp = util::move(items[k - 1]);
      items[k - 1] = util::move(items[k]);
      items[k] = util::move(tmp);
      --k;
    }
  }
  if (reversed)
    for (uint64_t i = 0, j = items.count() - 1; i < j; ++i, --j) {
      UniquePointer<Item> tmp = util::move(items[i]);
      items[i] = util::move(items[j]);
      items[j] = util::move(tmp);
    }

  // the Place that applies to an item across the container
  auto cross_align = [&](const Item& it) {
    Place p = it.style.align_self == Place::Auto ? s.align_items : it.style.align_self;
    return p;
  };

  if (!column) {
    // --- the base sizes
    auto content_basis = [&](Item& it) -> uint32_t {
      const Style& st = it.style;
      const uint32_t horizontal = it.edges.horizontal();
      uint32_t content;
      if (st.flex_basis.is_px()) {
        content = cols_of(st.flex_basis, 4000);
        if (st.sizing == BoxSizing::BorderBox) content = content > horizontal ? content - horizontal : 0;
      } else if (st.flex_basis.kind == Length::MinContent) content = it.min_content;
      else if (st.flex_basis.is_auto() && st.width.is_px()) {
        content = cols_of(st.width, 4000);
        if (st.sizing == BoxSizing::BorderBox) content = content > horizontal ? content - horizontal : 0;
      } else if (st.flex_basis.is_auto() && st.width.kind == Length::MinContent) content = it.min_content;
      else content = it.max_content;
      return content;
    };
    for (uint64_t i = 0; i < items.count(); ++i) {
      Item& it = *items[i];
      const uint32_t horizontal = it.edges.horizontal();
      it.basis = content_basis(it) + horizontal;
      it.main = it.basis;
      // the smallest an item may get: its minimum content (unless it says otherwise)
      it.min_main = it.style.min_width.is_px() ? cols_of(it.style.min_width, 4000) + (it.style.sizing == BoxSizing::BorderBox ? 0 : horizontal) : it.min_content + horizontal;
      if (it.style.overflow_x != Overflow::Visible && !it.style.min_width.is_px()) it.min_main = horizontal + 1;
      it.max_main = it.style.max_width.is_px() ? cols_of(it.style.max_width, 4000) + (it.style.sizing == BoxSizing::BorderBox ? 0 : horizontal) : 0xFFFFFFFFu;
      if (it.min_main > it.max_main) it.min_main = it.max_main;
    }
    // --- the lines
    uint32_t line_count = 1;
    if (s.flex_wrap != FlexWrap::NoWrap) {
      uint32_t used = 0;
      for (uint64_t i = 0; i < items.count(); ++i) {
        Item& it = *items[i];
        const uint32_t outer = it.main + it.margin_left + it.margin_right;
        if (used > 0 && used + col_gap + outer > content_cols) {
          ++line_count;
          used = 0;
        }
        it.line = static_cast<int>(line_count - 1);
        used += (used > 0 ? col_gap : 0) + outer;
      }
    }
    // --- sizes and boxes, line by line
    List<Raster> unused(a);
    (void)unused;
    struct LineBox {
      uint32_t first, last;   // items [first, last)
      uint32_t height = 0;
      uint32_t free = 0;
    };
    List<LineBox> line_boxes(a);
    for (uint32_t l = 0; l < line_count; ++l) {
      LineBox lb;
      lb.first = lb.last = 0;
      bool started = false;
      for (uint64_t i = 0; i < items.count(); ++i) {
        if (items[i]->line != static_cast<int>(l)) continue;
        if (!started) { lb.first = static_cast<uint32_t>(i); started = true; }
        lb.last = static_cast<uint32_t>(i) + 1;
      }
      if (started) line_boxes.push_back(lb);
    }
    for (uint64_t l = 0; l < line_boxes.count() && !failed_; ++l) {
      LineBox& lb = line_boxes[l];
      const uint32_t count = lb.last - lb.first;
      uint32_t sum = col_gap * (count - 1);
      for (uint32_t i = lb.first; i < lb.last; ++i) sum += items[i]->main + items[i]->margin_left + items[i]->margin_right;
      // grow or shrink
      for (int pass = 0; pass < 3; ++pass) {
        sum = col_gap * (count - 1);
        for (uint32_t i = lb.first; i < lb.last; ++i) sum += items[i]->main + items[i]->margin_left + items[i]->margin_right;
        if (sum < content_cols && !measuring_) {
          uint64_t total_grow = 0;
          for (uint32_t i = lb.first; i < lb.last; ++i)
            if (items[i]->main < items[i]->max_main) total_grow += static_cast<uint32_t>(items[i]->grow > 0 ? items[i]->grow : 0);
          if (total_grow == 0) break;
          uint32_t free = content_cols - sum, given = 0;
          for (uint32_t i = lb.first; i < lb.last; ++i) {
            if (items[i]->grow <= 0 || items[i]->main >= items[i]->max_main) continue;
            uint32_t share = static_cast<uint32_t>(static_cast<uint64_t>(free) * static_cast<uint32_t>(items[i]->grow) / total_grow);
            if (items[i]->main + share > items[i]->max_main) share = items[i]->max_main - items[i]->main;
            items[i]->main += share;
            given += share;
          }
          if (given == 0 && free > 0) {   // (the leftover columns go to the first item that can take them)
            for (uint32_t i = lb.first; i < lb.last && free > 0; ++i)
              if (items[i]->grow > 0 && items[i]->main < items[i]->max_main) {
                ++items[i]->main;
                --free;
                break;
              }
          }
          if (given == free) break;
        } else if (sum > content_cols && !measuring_) {
          uint64_t total_shrink = 0;
          for (uint32_t i = lb.first; i < lb.last; ++i)
            if (items[i]->main > items[i]->min_main) total_shrink += static_cast<uint64_t>(items[i]->shrink > 0 ? items[i]->shrink : 0) * (items[i]->basis > 0 ? items[i]->basis : 1);
          if (total_shrink == 0) break;
          const uint32_t need = sum - content_cols;
          uint32_t taken = 0;
          for (uint32_t i = lb.first; i < lb.last; ++i) {
            if (items[i]->shrink <= 0 || items[i]->main <= items[i]->min_main) continue;
            uint32_t share = static_cast<uint32_t>(static_cast<uint64_t>(need) * static_cast<uint64_t>(items[i]->shrink) * (items[i]->basis > 0 ? items[i]->basis : 1) / total_shrink);
            if (share == 0 && taken < need) share = 1;
            if (share > items[i]->main - items[i]->min_main) share = items[i]->main - items[i]->min_main;
            if (taken + share > need) share = need - taken;
            items[i]->main -= share;
            taken += share;
          }
          if (taken == 0) break;
        } else {
          break;
        }
      }
      sum = col_gap * (count - 1);
      for (uint32_t i = lb.first; i < lb.last; ++i) sum += items[i]->main + items[i]->margin_left + items[i]->margin_right;
      lb.free = sum < content_cols ? content_cols - sum : 0;
      // the boxes at their final widths
      for (uint32_t i = lb.first; i < lb.last && !failed_; ++i) {
        Item& it = *items[i];
        if (it.node->type == dom::NodeType::Text) {
          // text item: drawn as a box without decoration
          it.box.clear();
          if (!draw_text_item(it.node, inside, it.main, &it.box)) return false;
        } else {
          it.box.clear();
          if (!build_box(it.node, it.style, it.main + it.margin_left + it.margin_right, it.main, 0, &it.box)) return false;
        }
        const uint32_t h = it.box.lines() + it.margin_top + it.margin_bottom;
        if (h > lb.height) lb.height = h;
      }
    }
    // a single line takes the container's height when it has one
    if (line_boxes.count() == 1 && content_lines > line_boxes[0].height && s.flex_wrap == FlexWrap::NoWrap) line_boxes[0].height = content_lines;
    // --- put the lines together
    if (s.flex_wrap == FlexWrap::WrapReverse)
      for (uint64_t i = 0, j = line_boxes.count() - 1; i < j; ++i, --j) {
        LineBox tmp = line_boxes[i];
        line_boxes[i] = line_boxes[j];
        line_boxes[j] = tmp;
      }
    uint32_t natural = 0;
    for (uint64_t l = 0; l < line_boxes.count() && !failed_; ++l) {
      LineBox& lb = line_boxes[l];
      const uint32_t count = lb.last - lb.first;
      // stretch the items that do not have a height
      for (uint32_t i = lb.first; i < lb.last && !failed_; ++i) {
        Item& it = *items[i];
        const Place place = cross_align(it);
        const uint32_t outer_h = it.box.lines() + it.margin_top + it.margin_bottom;
        if ((place == Place::Normal || place == Place::Stretch) && it.node->type == dom::NodeType::Element && it.style.height.is_auto() && outer_h < lb.height &&
            !it.auto_top && !it.auto_bottom) {
          const uint32_t target = lb.height - it.margin_top - it.margin_bottom;
          it.box.clear();
          if (!build_box(it.node, it.style, it.main + it.margin_left + it.margin_right, it.main, target, &it.box)) return false;
        }
        const uint32_t h = it.box.lines() + it.margin_top + it.margin_bottom;
        uint32_t free_v = lb.height > h ? lb.height - h : 0;
        uint32_t offset = 0;
        if (it.auto_top && it.auto_bottom) offset = free_v / 2;
        else if (it.auto_top) offset = free_v;
        else if (it.auto_bottom) offset = 0;
        else if (place == Place::Center) offset = free_v / 2;
        else if (place == Place::End || place == Place::FlexEnd) offset = free_v;
        it.cross_offset = it.margin_top + offset;
      }
      // the free room along the line
      uint32_t lead = 0, between = col_gap, trail = 0;
      uint32_t free = lb.free;
      Place jc = s.justify_content;
      const bool grew = [&] {
        for (uint32_t i = lb.first; i < lb.last; ++i)
          if (items[i]->grow > 0) return true;
        return false;
      }();
      if (grew) free = 0;
      int auto_count = 0;
      for (uint32_t i = lb.first; i < lb.last; ++i) auto_count += (items[i]->auto_left ? 1 : 0) + (items[i]->auto_right ? 1 : 0);
      uint32_t auto_each = 0, auto_extra = 0;
      if (auto_count > 0 && free > 0) {
        auto_each = free / auto_count;
        auto_extra = free % auto_count;
        free = 0;
      }
      if (!measuring_) {
        if (jc == Place::End || jc == Place::FlexEnd || jc == Place::Right) lead = free;
        else if (jc == Place::Center) lead = free / 2;
        else if (jc == Place::SpaceBetween && count > 1) between += free / (count - 1);
        else if (jc == Place::SpaceAround) { lead = free / (2 * count); trail = lead; between += free / count; }
        else if (jc == Place::SpaceEvenly) { lead = free / (count + 1); between += lead; }
        if (jc == Place::Start || jc == Place::FlexStart || jc == Place::Left || jc == Place::Normal || jc == Place::Stretch || jc == Place::Baseline) lead = 0;
      }
      (void)trail;
      if (reversed && (jc == Place::Normal || jc == Place::Start || jc == Place::FlexStart) && !measuring_) lead = lb.free;   // (row-reverse starts at the right)
      uint32_t line_width = 0;
      for (uint32_t row = 0; row < lb.height && !failed_; ++row) {
        List<LinePiece> pieces(a);
        uint32_t used = 0;
        auto spaces = [&](uint32_t cnt) {
          if (cnt == 0) return;
          LinePiece p(a);
          for (uint32_t k = 0; k < cnt; ++k) p.text.push_back(' ');
          failed_ |= !pieces.push_back(util::move(p));
          used += cnt;
        };
        spaces(lead);
        uint32_t autos_used = 0;
        for (uint32_t i = lb.first; i < lb.last && !failed_; ++i) {
          Item& it = *items[i];
          if (i > lb.first) spaces(between);
          uint32_t before = it.margin_left, after = it.margin_right;
          if (it.auto_left) { before += auto_each + (autos_used < auto_extra ? 1 : 0); ++autos_used; }
          if (it.auto_right) { after += auto_each + (autos_used < auto_extra ? 1 : 0); ++autos_used; }
          spaces(before);
          if (row >= it.cross_offset && row < it.cross_offset + it.box.lines()) {
            const uint32_t y = row - it.cross_offset;
            for (uint32_t k = y ? it.box.ends[y - 1] : 0; k < it.box.ends[y]; ++k) {
              LinePiece copy(a);
              copy.text.assign(it.box.pieces[k].text.view());
              copy.flags = it.box.pieces[k].flags;
              copy.level = it.box.pieces[k].level;
              copy.color = it.box.pieces[k].color;
              copy.background = it.box.pieces[k].background;
              copy.node = it.box.pieces[k].node;
              copy.link = it.box.pieces[k].link;
              used += static_cast<uint32_t>(copy.text.size());
              failed_ |= !pieces.push_back(util::move(copy));
            }
          } else {
            spaces(it.main);
          }
          spaces(after);
        }
        if (!measuring_ && used < content_cols) spaces(content_cols - used);
        if (used > line_width) line_width = used;
        for (uint64_t k = 0; k < pieces.count(); ++k) failed_ |= !out->pieces.push_back(util::move(pieces[k]));
        failed_ |= !out->ends.push_back(static_cast<uint32_t>(out->pieces.count()));
      }
      if (line_width > natural) natural = line_width;
      if (l + 1 < line_boxes.count()) {
        for (uint32_t g = 0; g < row_gap; ++g) {
          LinePiece pad(a);
          for (uint32_t k = 0; k < (measuring_ ? 0 : content_cols); ++k) pad.text.push_back(' ');
          if (!pad.text.empty()) failed_ |= !out->pieces.push_back(util::move(pad));
          failed_ |= !out->ends.push_back(static_cast<uint32_t>(out->pieces.count()));
        }
      }
    }
    if (measuring_) out->width = natural;
    // measuring: the largest width the container needs (all in one line), and the narrowest (the widest item, when it wraps)
    uint32_t widest_min = 0, sum_min = col_gap * (items.count() - 1), sum_max = col_gap * (items.count() - 1);
    for (uint64_t i = 0; i < items.count(); ++i) {
      const uint32_t mn = items[i]->min_content + items[i]->edges.horizontal() + items[i]->margin_left + items[i]->margin_right;
      const uint32_t mx = items[i]->max_content + items[i]->edges.horizontal() + items[i]->margin_left + items[i]->margin_right;
      if (mn > widest_min) widest_min = mn;
      sum_min += mn;
      sum_max += mx;
    }
    measured_min_ = s.flex_wrap == FlexWrap::NoWrap ? sum_min : widest_min;
    measured_max_ = sum_max;
    return !failed_;
  }

  // --- a column: the items one above the other
  uint32_t total_height = 0;
  const uint32_t n_items = static_cast<uint32_t>(items.count());
  uint32_t widest = 0;
  for (uint32_t i = 0; i < n_items && !failed_; ++i) {
    Item& it = *items[i];
    const Place place = cross_align(it);
    const uint32_t horizontal = it.edges.horizontal();
    uint32_t outer;
    if (it.style.width.is_px()) {
      outer = cols_of(it.style.width, 4000) + (it.style.sizing == BoxSizing::BorderBox ? 0 : horizontal);
    } else if ((place == Place::Normal || place == Place::Stretch) && !measuring_) {
      outer = content_cols > it.margin_left + it.margin_right ? content_cols - it.margin_left - it.margin_right : 1;
    } else {
      outer = it.max_content + horizontal;
    }
    if (it.style.max_width.is_px() && outer > cols_of(it.style.max_width, 4000) + (it.style.sizing == BoxSizing::BorderBox ? 0 : horizontal)) outer = cols_of(it.style.max_width, 4000) + (it.style.sizing == BoxSizing::BorderBox ? 0 : horizontal);
    if (it.style.min_width.is_px() && outer < cols_of(it.style.min_width, 4000) + (it.style.sizing == BoxSizing::BorderBox ? 0 : horizontal)) outer = cols_of(it.style.min_width, 4000) + (it.style.sizing == BoxSizing::BorderBox ? 0 : horizontal);
    if (outer > content_cols) outer = content_cols;
    if (outer < horizontal + 1) outer = horizontal + 1;
    it.main = outer;
    it.box.clear();
    if (it.node->type == dom::NodeType::Text) {
      if (!draw_text_item(it.node, inside, outer, &it.box)) return false;
    } else if (!build_box(it.node, it.style, outer + it.margin_left + it.margin_right, outer, 0, &it.box)) {
      return false;
    }
    total_height += it.box.lines() + it.margin_top + it.margin_bottom;
    if (outer + it.margin_left + it.margin_right > widest) widest = outer + it.margin_left + it.margin_right;
  }
  total_height += row_gap * (n_items > 0 ? n_items - 1 : 0);
  // extra height goes to the items that grow, or is spread by justify-content
  uint32_t lead = 0, between = row_gap;
  if (content_lines > total_height && !measuring_) {
    uint32_t free = content_lines - total_height;
    uint64_t total_grow = 0;
    for (uint32_t i = 0; i < n_items; ++i) total_grow += static_cast<uint32_t>(items[i]->grow > 0 ? items[i]->grow : 0);
    if (total_grow > 0) {
      uint32_t given = 0;
      for (uint32_t i = 0; i < n_items && !failed_; ++i) {
        Item& it = *items[i];
        if (it.grow <= 0) continue;
        uint32_t share = static_cast<uint32_t>(static_cast<uint64_t>(free) * static_cast<uint32_t>(it.grow) / total_grow);
        if (i + 1 == n_items || given + share > free) share = free - given;
        given += share;
        if (it.node->type == dom::NodeType::Element && share > 0) {
          const uint32_t target = it.box.lines() + share;
          it.box.clear();
          if (!build_box(it.node, it.style, it.main + it.margin_left + it.margin_right, it.main, target, &it.box)) return false;
        }
      }
    } else {
      const Place jc = s.justify_content;
      if (jc == Place::End || jc == Place::FlexEnd) lead = free;
      else if (jc == Place::Center) lead = free / 2;
      else if (jc == Place::SpaceBetween && n_items > 1) between += free / (n_items - 1);
      else if (jc == Place::SpaceAround) { lead = free / (2 * n_items); between += free / n_items; }
      else if (jc == Place::SpaceEvenly) { lead = free / (n_items + 1); between += lead; }
    }
  }
  auto blank_row = [&](uint32_t width) {
    LinePiece pad(a);
    for (uint32_t k = 0; k < width; ++k) pad.text.push_back(' ');
    if (!pad.text.empty()) failed_ |= !out->pieces.push_back(util::move(pad));
    failed_ |= !out->ends.push_back(static_cast<uint32_t>(out->pieces.count()));
  };
  const uint32_t row_width = measuring_ ? 0 : content_cols;
  for (uint32_t g = 0; g < lead; ++g) blank_row(row_width);
  for (uint32_t i = 0; i < n_items && !failed_; ++i) {
    Item& it = *items[i];
    if (i > 0) for (uint32_t g = 0; g < between; ++g) blank_row(row_width);
    for (uint32_t g = 0; g < it.margin_top; ++g) blank_row(row_width);
    const Place place = cross_align(it);
    uint32_t left = it.margin_left;
    const uint32_t free_h = content_cols > it.main + it.margin_left + it.margin_right ? content_cols - it.main - it.margin_left - it.margin_right : 0;
    if (it.auto_left && it.auto_right) left += free_h / 2;
    else if (it.auto_left) left += free_h;
    else if (place == Place::Center) left += free_h / 2;
    else if (place == Place::End || place == Place::FlexEnd) left += free_h;
    for (uint32_t y = 0; y < it.box.lines() && !failed_; ++y) {
      uint32_t used = left;
      if (left > 0) {
        LinePiece p(a);
        for (uint32_t k = 0; k < left; ++k) p.text.push_back(' ');
        failed_ |= !out->pieces.push_back(util::move(p));
      }
      for (uint32_t k = y ? it.box.ends[y - 1] : 0; k < it.box.ends[y]; ++k) {
        used += static_cast<uint32_t>(it.box.pieces[k].text.size());
        failed_ |= !out->pieces.push_back(util::move(it.box.pieces[k]));
      }
      if (!measuring_ && used < content_cols) {
        LinePiece p(a);
        for (uint32_t k = used; k < content_cols; ++k) p.text.push_back(' ');
        failed_ |= !out->pieces.push_back(util::move(p));
      }
      failed_ |= !out->ends.push_back(static_cast<uint32_t>(out->pieces.count()));
    }
    for (uint32_t g = 0; g < it.margin_bottom; ++g) blank_row(row_width);
  }
  while (out->lines() < content_lines) blank_row(row_width);
  if (measuring_) out->width = widest;
  uint32_t max_min = 0, max_max = 0;
  for (uint32_t i = 0; i < n_items; ++i) {
    const uint32_t extra = items[i]->edges.horizontal() + items[i]->margin_left + items[i]->margin_right;
    if (items[i]->min_content + extra > max_min) max_min = items[i]->min_content + extra;
    if (items[i]->max_content + extra > max_max) max_max = items[i]->max_content + extra;
  }
  measured_min_ = max_min;
  measured_max_ = max_max;
  (void)lead;
  return !failed_;
}

// --- Grid -------------------------------------------------------------------------------------------

namespace {

// How big a grid track is: a size, a range, or a share of the free room.
struct TrackDef {
  enum Kind : uint8_t { Fixed, Fr, Auto, MinContent, MaxContent };
  Kind min_kind = Auto, max_kind = Auto;
  int32_t min_value = 0, max_value = 0;   // cells (Fixed) or x100 (Fr)
};

struct LineName {
  explicit LineName(allocators::IAllocator& a) : name(a) {}
  String name;
  uint32_t line = 0;   // the number of the line (0 is the first)
};

struct TrackTemplate {
  explicit TrackTemplate(allocators::IAllocator& a) : tracks(a), names(a), repeated(a), repeat_names(a) {}
  List<TrackDef> tracks;
  List<LineName> names;
  List<TrackDef> repeated;       // repeat(auto-fill, ...): the tracks that are repeated
  List<LineName> repeat_names;
  int64_t repeat_at = -1;        // where in `tracks` they go
};

// The whitespace separated parts of a value (brackets, parentheses and quotes keep their insides together).
bool next_word(StringView text, uint64_t* at, StringView* part) {
  uint64_t i = *at;
  while (i < text.size() && is_space(text[i])) ++i;
  if (i >= text.size()) return false;
  const uint64_t start = i;
  int depth = 0;
  char quote = 0;
  while (i < text.size() && (depth > 0 || quote || !is_space(text[i]))) {
    const char c = text[i];
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(' || c == '[') {
      ++depth;
    } else if ((c == ')' || c == ']') && depth > 0) {
      --depth;
    }
    ++i;
  }
  *part = text.substr(start, i - start);
  *at = i;
  return true;
}

bool parse_integer(StringView t, int32_t* out) {
  t = t.trimmed();
  if (t.empty()) return false;
  bool negative = false;
  uint64_t i = 0;
  if (t[0] == '-' || t[0] == '+') negative = t[i++] == '-';
  if (i >= t.size()) return false;
  int64_t v = 0;
  for (; i < t.size(); ++i) {
    if (t[i] < '0' || t[i] > '9') return false;
    v = v * 10 + (t[i] - '0');
    if (v > 100000) v = 100000;
  }
  *out = static_cast<int32_t>(negative ? -v : v);
  return true;
}

// One size of a track: a length or percentage (cells), "1fr", "auto", "min-content", "max-content".
bool track_value(const StyleSheet& sheet, const Style& style, StringView v, int32_t percent_px, bool vertical, TrackDef::Kind* kind, int32_t* value) {
  v = v.trimmed();
  if (v.equals_ignore_case("auto")) { *kind = TrackDef::Auto; return true; }
  if (v.equals_ignore_case("min-content")) { *kind = TrackDef::MinContent; return true; }
  if (v.equals_ignore_case("max-content")) { *kind = TrackDef::MaxContent; return true; }
  if (v.size() > 2 && v.ends_with("fr")) {
    int64_t whole = 0, frac = 0, scale = 1;
    uint64_t i = 0;
    bool any = false;
    while (i < v.size() - 2 && v[i] >= '0' && v[i] <= '9') { whole = whole * 10 + (v[i++] - '0'); any = true; }
    if (i < v.size() - 2 && v[i] == '.') {
      ++i;
      while (i < v.size() - 2 && v[i] >= '0' && v[i] <= '9') { if (scale < 100) { frac = frac * 10 + (v[i] - '0'); scale *= 10; } ++i; any = true; }
    }
    if (!any || i != v.size() - 2) return false;
    *kind = TrackDef::Fr;
    *value = static_cast<int32_t>(whole * 100 + frac * 100 / scale);
    return true;
  }
  Length l;
  if (!sheet.resolve_length(v, style, percent_px, &l) || !l.is_px() || l.px100 < 0) return false;
  *kind = TrackDef::Fixed;
  *value = vertical ? l.lines() : l.columns();
  return true;
}

// Reads a track list ("100px 1fr repeat(2, 50%) minmax(10px, auto) [name] auto"). A list that cannot be read gives nothing.
bool parse_tracks(allocators::IAllocator& a, const StyleSheet& sheet, const Style& style, StringView text, int32_t percent_px, bool vertical, TrackTemplate* out, uint32_t depth, bool in_repeat) {
  List<TrackDef>& tracks = in_repeat ? out->repeated : out->tracks;
  List<LineName>& names = in_repeat ? out->repeat_names : out->names;
  uint64_t at = 0;
  StringView part;
  uint32_t guard = 0;
  while (next_word(text, &at, &part)) {
    if (++guard > 500 || tracks.count() > 400) return false;
    if (part.starts_with('[')) {
      StringView inner = part.substr(1);
      if (inner.ends_with(']')) inner = inner.without_suffix(1);
      uint64_t k = 0;
      StringView word;
      while (next_word(inner, &k, &word)) {
        LineName line(a);
        line.name.assign(word);
        line.line = static_cast<uint32_t>(tracks.count());
        names.push_back(util::move(line));
      }
      continue;
    }
    TrackDef def;
    const uint64_t open = part.find('(');
    const StringView function = open == StringView::npos ? StringView() : part.first(open).trimmed();
    if (open != StringView::npos && part.ends_with(')')) {
      const StringView inner = part.substr(open + 1, part.size() - open - 2);
      if (function.equals_ignore_case("repeat")) {
        // repeat(count, tracks)
        int depth_paren = 0;
        uint64_t comma = StringView::npos;
        for (uint64_t i = 0; i < inner.size() && comma == StringView::npos; ++i) {
          if (inner[i] == '(') ++depth_paren;
          else if (inner[i] == ')') --depth_paren;
          else if (inner[i] == ',' && depth_paren == 0) comma = i;
        }
        if (comma == StringView::npos || depth > 2) return false;
        const StringView count_text = inner.first(comma).trimmed(), body = inner.substr(comma + 1);
        if (count_text.equals_ignore_case("auto-fill") || count_text.equals_ignore_case("auto-fit")) {
          if (in_repeat || out->repeat_at >= 0) return false;
          out->repeat_at = static_cast<int64_t>(out->tracks.count());
          if (!parse_tracks(a, sheet, style, body, percent_px, vertical, out, depth + 1, true)) return false;
          continue;
        }
        int32_t count;
        if (!parse_integer(count_text, &count) || count < 1) return false;
        if (count > 100) count = 100;
        for (int32_t r = 0; r < count; ++r)
          if (!parse_tracks(a, sheet, style, body, percent_px, vertical, out, depth + 1, in_repeat)) return false;
        continue;
      }
      if (function.equals_ignore_case("minmax")) {
        int depth_paren = 0;
        uint64_t comma = StringView::npos;
        for (uint64_t i = 0; i < inner.size() && comma == StringView::npos; ++i) {
          if (inner[i] == '(') ++depth_paren;
          else if (inner[i] == ')') --depth_paren;
          else if (inner[i] == ',' && depth_paren == 0) comma = i;
        }
        if (comma == StringView::npos) return false;
        TrackDef::Kind mk, xk;
        int32_t mv = 0, xv = 0;
        if (!track_value(sheet, style, inner.first(comma), percent_px, vertical, &mk, &mv) || !track_value(sheet, style, inner.substr(comma + 1), percent_px, vertical, &xk, &xv)) return false;
        def.min_kind = mk == TrackDef::Fr ? TrackDef::Auto : mk;
        def.min_value = mv;
        def.max_kind = xk;
        def.max_value = xv;
        tracks.push_back(def);
        continue;
      }
      if (function.equals_ignore_case("fit-content")) {
        TrackDef::Kind xk;
        int32_t xv = 0;
        if (!track_value(sheet, style, inner, percent_px, vertical, &xk, &xv) || xk != TrackDef::Fixed) return false;
        def.min_kind = TrackDef::Auto;
        def.max_kind = TrackDef::Fixed;
        def.max_value = xv;
        tracks.push_back(def);
        continue;
      }
      if (function.equals_ignore_case("calc") || function.equals_ignore_case("min") || function.equals_ignore_case("max") || function.equals_ignore_case("clamp") ||
          function.equals_ignore_case("var")) {
        TrackDef::Kind k;
        int32_t v = 0;
        if (!track_value(sheet, style, part, percent_px, vertical, &k, &v)) return false;
        def.min_kind = def.max_kind = k;
        def.min_value = def.max_value = v;
        tracks.push_back(def);
        continue;
      }
      return false;
    }
    TrackDef::Kind k;
    int32_t v = 0;
    if (!track_value(sheet, style, part, percent_px, vertical, &k, &v)) return false;
    if (k == TrackDef::Fr) {
      def.min_kind = TrackDef::Auto;
      def.max_kind = TrackDef::Fr;
      def.max_value = v;
    } else {
      def.min_kind = def.max_kind = k;
      def.min_value = def.max_value = v;
    }
    tracks.push_back(def);
  }
  return true;
}

// grid-template-areas: "a a b" "a a c" -> names at each cell.
struct AreaGrid {
  explicit AreaGrid(allocators::IAllocator& a) : cells(a), rows(0), columns(0) {}
  List<String> cells;   // rows x columns names ("." and "" for none)
  uint32_t rows, columns;
  bool find(StringView name, uint32_t* r0, uint32_t* c0, uint32_t* r1, uint32_t* c1) const {
    bool found = false;
    for (uint32_t r = 0; r < rows; ++r)
      for (uint32_t c = 0; c < columns; ++c) {
        if (!cells[r * columns + c].view().equals(name)) continue;
        if (!found) { *r0 = r; *c0 = c; *r1 = r + 1; *c1 = c + 1; found = true; }
        else {
          if (r < *r0) *r0 = r;
          if (c < *c0) *c0 = c;
          if (r + 1 > *r1) *r1 = r + 1;
          if (c + 1 > *c1) *c1 = c + 1;
        }
      }
    return found;
  }
};

bool parse_areas(allocators::IAllocator& a, StringView text, AreaGrid* out) {
  // pass 0 finds the widest row, pass 1 fills the cells (short rows are padded with empty names)
  uint32_t columns = 0, rows = 0;
  for (int pass = 0; pass < 2; ++pass) {
    uint64_t i = 0;
    rows = 0;
    while (i < text.size()) {
      while (i < text.size() && text[i] != '"' && text[i] != '\'') ++i;
      if (i >= text.size()) break;
      const char q = text[i++];
      const uint64_t start = i;
      while (i < text.size() && text[i] != q) ++i;
      const StringView row = text.substr(start, i - start);
      if (i < text.size()) ++i;
      if (++rows > 200) return false;
      uint64_t k = 0;
      uint32_t n = 0;
      StringView word;
      while (next_word(row, &k, &word)) {
        ++n;
        if (pass == 1) {
          String name(a);
          name.assign(word.starts_with('.') ? StringView() : word);
          if (!out->cells.push_back(util::move(name))) return false;
        }
      }
      if (pass == 0) {
        if (n > columns) columns = n;
      } else {
        for (; n < columns; ++n) {
          String name(a);
          if (!out->cells.push_back(util::move(name))) return false;
        }
      }
    }
    if (pass == 0 && (rows == 0 || columns == 0)) return false;
  }
  out->rows = rows;
  out->columns = columns;
  return true;
}

// A grid line as an item names it: auto, 3, -1, span 2, area-name, name 2.
struct LineSpec {
  explicit LineSpec(allocators::IAllocator& a) : name(a) {}
  bool line_given = false, span_given = false;
  int32_t line = 0;          // 1-based; negative counts from the end
  int32_t span = 1;
  String name;               // a named line or area ("" for none)
  int32_t name_index = 1;
};

void parse_line_spec(StringView text, LineSpec* out) {
  text = text.trimmed();
  if (text.empty() || text.equals_ignore_case("auto")) return;
  uint64_t at = 0;
  StringView part;
  bool span = false;
  while (next_word(text, &at, &part)) {
    int32_t n;
    if (part.equals_ignore_case("span")) {
      span = true;
      out->span_given = true;
    } else if (parse_integer(part, &n)) {
      if (span) out->span = n < 1 ? 1 : n > 200 ? 200 : n;
      else if (out->name.empty()) { out->line = n; out->line_given = n != 0; }
      else out->name_index = n;
    } else {
      out->name.assign(part);
    }
  }
}

}  // namespace

bool DocumentRenderer::layout_grid(const dom::Node* container, const Style& s, uint32_t content_cols, uint32_t content_lines, Raster* out) {
  allocators::IAllocator& a = out_.allocator();
  const Style inside = inside_style(s, content_cols);
  const bool table_mode = s.layout == Layout::Table;
  List<UniquePointer<Item>> items(a);
  List<const dom::Node*> loose(a);
  uint32_t table_rows = 0;
  if (table_mode) {
    // display: table - the rows (inside row groups too) and their cells, each cell of a row in the next column
    for (const dom::Node* c = container->first_child; c && items.count() < 400; c = c->next) {
      if (!c->is_element()) continue;
      Style cs = derive_style(inside, c, sheet_);
      if (cs.display == Display::None) continue;
      List<const dom::Node*> row_nodes(a);
      if (cs.layout == Layout::TableRowGroup) {
        for (const dom::Node* r = c->first_child; r; r = r->next)
          if (r->is_element()) row_nodes.push_back(r);
      } else {
        row_nodes.push_back(c);
      }
      for (uint64_t ri = 0; ri < row_nodes.count(); ++ri) {
        const dom::Node* rn = row_nodes[ri];
        Style rs = derive_style(cs.layout == Layout::TableRowGroup ? cs : inside, rn, sheet_);
        if (rs.display == Display::None) continue;
        if (rs.layout != Layout::TableRow) {
          // a cell (or anything else) straight in the table: a row of its own
          auto item = UniquePointer<Item>::make(a, a);
          if (!item) return false;
          item->node = rn;
          item->style = rs;
          if (item->style.own_background) { item->style.background = rs.own_background_color; item->style.background_block = true; }
          item->style.display = Display::Block;
          item->row = static_cast<int32_t>(table_rows);
          item->column = 0;
          items.push_back(util::move(item));
          ++table_rows;
          continue;
        }
        uint32_t col = 0;
        for (const dom::Node* cell = rn->first_child; cell && items.count() < 400; cell = cell->next) {
          if (!cell->is_element()) continue;
          Style cellstyle = derive_style(rs, cell, sheet_);
          if (cellstyle.display == Display::None) continue;
          auto item = UniquePointer<Item>::make(a, a);
          if (!item) return false;
          item->node = cell;
          item->style = cellstyle;
          if (cellstyle.own_background) { item->style.background = cellstyle.own_background_color; item->style.background_block = true; }
          item->style.display = Display::Block;
          item->row = static_cast<int32_t>(table_rows);
          item->column = static_cast<int32_t>(col++);
          items.push_back(util::move(item));
        }
        ++table_rows;
      }
    }
  } else if (!collect_items(container, inside, &items, &loose, 0)) {
    return false;
  }
  out->clear();
  out->width = content_cols;
  if (items.count() == 0) {
    LinePiece pad(a);
    for (uint32_t k = 0; k < content_cols && !measuring_; ++k) pad.text.push_back(' ');
    if (!pad.text.empty()) failed_ |= !out->pieces.push_back(util::move(pad));
    failed_ |= !out->ends.push_back(static_cast<uint32_t>(out->pieces.count()));
    return true;
  }
  for (uint64_t i = 0; i < items.count() && !failed_; ++i)
    if (!prepare_item(items[i].get(), inside, content_cols)) return false;

  const uint32_t col_gap = table_mode ? cols_of(s.border_spacing) : cols_of(s.column_gap), row_gap = table_mode ? 0 : lines_of(s.row_gap);
  // --- the templates
  TrackTemplate col_template(a), row_template(a);
  AreaGrid areas(a);
  bool have_areas = false;
  if (!table_mode) {
    const int32_t px_width = static_cast<int32_t>(content_cols) * 8;
    if (!s.grid_template_columns.empty()) parse_tracks(a, sheet_, inside, sheet_.text(s.grid_template_columns), px_width, false, &col_template, 0, false);
    if (!s.grid_template_rows.empty()) parse_tracks(a, sheet_, inside, sheet_.text(s.grid_template_rows), content_lines ? static_cast<int32_t>(content_lines) * 16 : -1, true, &row_template, 0, false);
    if (!s.grid_template_areas.empty()) have_areas = parse_areas(a, sheet_.text(s.grid_template_areas), &areas);
  }
  // auto-fill / auto-fit: how many times the repeated tracks fit
  if (col_template.repeat_at >= 0) {
    uint32_t unit = 0;
    bool fixed = col_template.repeated.count() > 0;
    for (uint64_t i = 0; i < col_template.repeated.count(); ++i) {
      if (col_template.repeated[i].max_kind != TrackDef::Fixed && col_template.repeated[i].min_kind != TrackDef::Fixed) fixed = false;
      unit += static_cast<uint32_t>(col_template.repeated[i].min_kind == TrackDef::Fixed ? col_template.repeated[i].min_value : col_template.repeated[i].max_value);
    }
    uint32_t fixed_total = 0;
    for (uint64_t i = 0; i < col_template.tracks.count(); ++i)
      fixed_total += col_template.tracks[i].min_kind == TrackDef::Fixed ? static_cast<uint32_t>(col_template.tracks[i].min_value) : 0;
    uint32_t repeats = 1;
    const uint32_t per = unit + col_gap * static_cast<uint32_t>(col_template.repeated.count());
    if (fixed && per > 0 && content_cols > fixed_total) repeats = (content_cols - fixed_total + col_gap) / per;
    if (repeats < 1) repeats = 1;
    if (repeats > 100) repeats = 100;
    List<TrackDef> expanded(a);
    List<LineName> names(a);
    for (uint64_t i = 0; i < static_cast<uint64_t>(col_template.repeat_at); ++i) expanded.push_back(col_template.tracks[i]);
    for (uint32_t r = 0; r < repeats; ++r) {
      for (uint64_t i = 0; i < col_template.repeated.count(); ++i) expanded.push_back(col_template.repeated[i]);
    }
    for (uint64_t i = static_cast<uint64_t>(col_template.repeat_at); i < col_template.tracks.count(); ++i) expanded.push_back(col_template.tracks[i]);
    col_template.tracks.clear();
    for (uint64_t i = 0; i < expanded.count(); ++i) col_template.tracks.push_back(expanded[i]);
  }
  // --- the auto sizes for the implicit tracks
  TrackTemplate auto_cols(a), auto_rows(a);
  if (!table_mode) {
    if (!s.grid_auto_columns.empty()) parse_tracks(a, sheet_, inside, sheet_.text(s.grid_auto_columns), static_cast<int32_t>(content_cols) * 8, false, &auto_cols, 0, false);
    if (!s.grid_auto_rows.empty()) parse_tracks(a, sheet_, inside, sheet_.text(s.grid_auto_rows), -1, true, &auto_rows, 0, false);
  }
  // --- placing the items
  uint32_t ncols = static_cast<uint32_t>(col_template.tracks.count());
  uint32_t nrows = static_cast<uint32_t>(row_template.tracks.count());
  if (have_areas) {
    if (areas.columns > ncols) ncols = areas.columns;
    if (areas.rows > nrows) nrows = areas.rows;
  }
  if (table_mode) {
    for (uint64_t i = 0; i < items.count(); ++i) {
      if (static_cast<uint32_t>(items[i]->column) + 1 > ncols) ncols = static_cast<uint32_t>(items[i]->column) + 1;
    }
    nrows = table_rows;
  }
  if (ncols == 0) ncols = 1;
  auto line_named = [&](const TrackTemplate& t, StringView name, int32_t index, bool end, int32_t* line) {
    int32_t seen = 0;
    for (uint64_t i = 0; i < t.names.count(); ++i) {
      String candidate(a);
      (void)candidate;
      if (t.names[i].name.view().equals(name)) {
        if (++seen == index) { *line = static_cast<int32_t>(t.names[i].line); return true; }
      }
    }
    (void)end;
    return false;
  };
  // the explicit position of every item (-1: automatic)
  struct Placed {
    int32_t col = -1, row = -1, col_span = 1, row_span = 1;
  };
  List<Placed> placed(a);
  for (uint64_t i = 0; i < items.count(); ++i) {
    Item& it = *items[i];
    Placed p;
    if (table_mode) {
      p.col = it.column;
      p.row = it.row;
      placed.push_back(p);
      continue;
    }
    LineSpec cs(a), ce(a), rs(a), re(a);
    parse_line_spec(sheet_.text(it.style.grid_column_start), &cs);
    parse_line_spec(sheet_.text(it.style.grid_column_end), &ce);
    parse_line_spec(sheet_.text(it.style.grid_row_start), &rs);
    parse_line_spec(sheet_.text(it.style.grid_row_end), &re);
    // a named area
    bool area_item = false;
    if (have_areas && !cs.name.empty() && !cs.line_given && !cs.span_given && (ce.name.empty() || ce.name.view().equals(cs.name.view())) && rs.name.view().equals(cs.name.view())) {
      uint32_t r0, c0, r1, c1;
      if (areas.find(cs.name.view(), &r0, &c0, &r1, &c1)) {
        p.col = static_cast<int32_t>(c0);
        p.row = static_cast<int32_t>(r0);
        p.col_span = static_cast<int32_t>(c1 - c0);
        p.row_span = static_cast<int32_t>(r1 - r0);
        area_item = true;
      }
    }
    if (!area_item) {
      auto resolve = [&](LineSpec& start, LineSpec& end, const TrackTemplate& tmpl, uint32_t explicit_count, bool columns_axis, int32_t* pos, int32_t* span) {
        auto line_index = [&](LineSpec& l, bool is_end, int32_t* idx) -> bool {
          if (l.line_given && l.name.empty()) {
            *idx = l.line > 0 ? l.line - 1 : static_cast<int32_t>(explicit_count) + l.line + 1 - 1;
            if (l.line < 0) *idx = static_cast<int32_t>(explicit_count) + l.line + 1;
            return *idx >= 0;
          }
          if (!l.name.empty() && !l.span_given) {
            int32_t line;
            if (line_named(tmpl, l.name.view(), l.name_index, is_end, &line)) { *idx = line; return true; }
            if (have_areas) {   // name-start / name-end of an area
              uint32_t r0, c0, r1, c1;
              if (areas.find(l.name.view(), &r0, &c0, &r1, &c1)) { *idx = columns_axis ? static_cast<int32_t>(is_end ? c1 : c0) : static_cast<int32_t>(is_end ? r1 : r0); return true; }
            }
            if (have_areas && l.name.size() > 6 && (l.name.view().ends_with("-start") || l.name.view().ends_with("-end"))) {
              const bool e = l.name.view().ends_with("-end");
              uint32_t r0, c0, r1, c1;
              if (areas.find(l.name.view().first(l.name.size() - (e ? 4 : 6)), &r0, &c0, &r1, &c1)) { *idx = columns_axis ? static_cast<int32_t>(e ? c1 : c0) : static_cast<int32_t>(e ? r1 : r0); return true; }
            }
          }
          return false;
        };
        int32_t si = -1, ei = -1;
        const bool has_start = line_index(start, false, &si), has_end = line_index(end, true, &ei);
        int32_t sp = 1;
        if (start.span_given) sp = start.span;
        else if (end.span_given) sp = end.span;
        if (has_start && has_end) {
          if (ei < si) { const int32_t t = si; si = ei; ei = t; }
          *pos = si;
          *span = ei - si < 1 ? 1 : ei - si;
        } else if (has_start) {
          *pos = si;
          *span = sp;
        } else if (has_end) {
          *pos = ei - sp < 0 ? 0 : ei - sp;
          *span = sp;
        } else {
          *pos = -1;
          *span = sp;
        }
      };
      resolve(cs, ce, col_template, ncols, true, &p.col, &p.col_span);
      resolve(rs, re, row_template, nrows, false, &p.row, &p.row_span);
    }
    if (p.col_span > 200) p.col_span = 200;
    if (p.row_span > 200) p.row_span = 200;
    placed.push_back(p);
  }
  // occupancy and the cursor
  List<uint8_t> occupied(a);
  uint32_t occupied_cols = 0;
  auto ensure = [&](uint32_t rows, uint32_t cols) -> bool {
    if (cols > occupied_cols || rows * occupied_cols > occupied.count()) {
      const uint32_t new_cols = cols > occupied_cols ? cols : occupied_cols;
      List<uint8_t> fresh(a);
      const uint32_t old_rows = occupied_cols ? static_cast<uint32_t>(occupied.count() / occupied_cols) : 0;
      const uint32_t total_rows = rows > old_rows ? rows : old_rows;
      for (uint32_t r = 0; r < total_rows; ++r)
        for (uint32_t c = 0; c < new_cols; ++c) fresh.push_back(r < old_rows && c < occupied_cols ? occupied[r * occupied_cols + c] : 0);
      occupied.clear();
      for (uint64_t i = 0; i < fresh.count(); ++i) occupied.push_back(fresh[i]);
      occupied_cols = new_cols;
    }
    return true;
  };
  auto free_at = [&](uint32_t r, uint32_t c, uint32_t rspan, uint32_t cspan) {
    ensure(r + rspan, c + cspan);
    for (uint32_t y = 0; y < rspan; ++y)
      for (uint32_t x = 0; x < cspan; ++x)
        if (occupied[(r + y) * occupied_cols + c + x]) return false;
    return true;
  };
  auto take = [&](uint32_t r, uint32_t c, uint32_t rspan, uint32_t cspan) {
    ensure(r + rspan, c + cspan);
    for (uint32_t y = 0; y < rspan; ++y)
      for (uint32_t x = 0; x < cspan; ++x) occupied[(r + y) * occupied_cols + c + x] = 1;
  };
  ensure(nrows > 0 ? nrows : 1, ncols);
  if (!table_mode) {
    const bool column_flow = s.grid_column_flow;
    // 1: both definite; 2: one definite; 3: automatic
    for (int stage = 0; stage < 3; ++stage) {
      uint32_t cur_r = 0, cur_c = 0;
      for (uint64_t i = 0; i < items.count() && !failed_; ++i) {
        Placed& p = placed[i];
        const int kind = p.col >= 0 && p.row >= 0 ? 0 : p.row >= 0 ? 1 : 2;
        if (kind != stage) continue;
        uint32_t cspan = static_cast<uint32_t>(p.col_span < 1 ? 1 : p.col_span), rspan = static_cast<uint32_t>(p.row_span < 1 ? 1 : p.row_span);
        if (stage == 0) {
          take(static_cast<uint32_t>(p.row), static_cast<uint32_t>(p.col), rspan, cspan);
          continue;
        }
        if (stage == 1 && p.row >= 0) {   // a definite row: the first column of it with room
          uint32_t c = 0;
          while (!free_at(static_cast<uint32_t>(p.row), c, rspan, cspan)) {
            ++c;
            if (c > 400) break;
          }
          p.col = static_cast<int32_t>(c);
          take(static_cast<uint32_t>(p.row), c, rspan, cspan);
          continue;
        }
        if (p.col >= 0 && !column_flow) {   // a definite column: the next row (from the cursor) with room
          if (s.grid_dense) cur_r = 0;
          else if (cur_c > static_cast<uint32_t>(p.col)) ++cur_r;
          while (!free_at(cur_r, static_cast<uint32_t>(p.col), rspan, cspan)) {
            ++cur_r;
            if (cur_r > 400) break;
          }
          p.row = static_cast<int32_t>(cur_r);
          cur_c = static_cast<uint32_t>(p.col) + cspan;
          take(cur_r, static_cast<uint32_t>(p.col), rspan, cspan);
          continue;
        }
        if (s.grid_dense) cur_r = cur_c = 0;
        // automatic: along the flow from the cursor
        const uint32_t limit = 400;
        if (!column_flow) {
          for (uint32_t guard = 0; guard < limit * 4; ++guard) {
            if (cur_c + cspan > (ncols > cspan ? ncols : cspan)) {
              cur_c = 0;
              ++cur_r;
            }
            if (free_at(cur_r, cur_c, rspan, cspan)) break;
            ++cur_c;
          }
          p.row = static_cast<int32_t>(cur_r);
          p.col = static_cast<int32_t>(cur_c);
          take(cur_r, cur_c, rspan, cspan);
          cur_c += cspan;
        } else {
          const uint32_t rows_fixed = nrows > rspan ? nrows : rspan;
          for (uint32_t guard = 0; guard < limit * 4; ++guard) {
            if (cur_r + rspan > rows_fixed) {
              cur_r = 0;
              ++cur_c;
            }
            if (free_at(cur_r, cur_c, rspan, cspan)) break;
            ++cur_r;
          }
          p.row = static_cast<int32_t>(cur_r);
          p.col = static_cast<int32_t>(cur_c);
          take(cur_r, cur_c, rspan, cspan);
          cur_r += rspan;
        }
      }
    }
  }
  for (uint64_t i = 0; i < items.count(); ++i) {
    Item& it = *items[i];
    it.column = placed[i].col < 0 ? 0 : placed[i].col;
    it.row = placed[i].row < 0 ? 0 : placed[i].row;
    it.column_span = placed[i].col_span < 1 ? 1 : placed[i].col_span;
    it.row_span = placed[i].row_span < 1 ? 1 : placed[i].row_span;
    if (static_cast<uint32_t>(it.column + it.column_span) > ncols) ncols = static_cast<uint32_t>(it.column + it.column_span);
    if (static_cast<uint32_t>(it.row + it.row_span) > nrows) nrows = static_cast<uint32_t>(it.row + it.row_span);
  }
  if (nrows == 0) nrows = 1;
  if (ncols > 200) ncols = 200;
  // --- the track definitions, implicit ones after the explicit ones
  List<TrackDef> col_defs(a), row_defs(a);
  for (uint32_t c = 0; c < ncols; ++c) {
    if (c < col_template.tracks.count()) col_defs.push_back(col_template.tracks[c]);
    else if (table_mode) { TrackDef d; d.min_kind = TrackDef::Auto; d.max_kind = TrackDef::Auto; col_defs.push_back(d); }
    else if (auto_cols.tracks.count() > 0) col_defs.push_back(auto_cols.tracks[(c - col_template.tracks.count()) % auto_cols.tracks.count()]);
    else col_defs.push_back(TrackDef());
  }
  for (uint32_t r = 0; r < nrows; ++r) {
    if (r < row_template.tracks.count()) row_defs.push_back(row_template.tracks[r]);
    else if (auto_rows.tracks.count() > 0) row_defs.push_back(auto_rows.tracks[(r - row_template.tracks.count()) % auto_rows.tracks.count()]);
    else row_defs.push_back(TrackDef());
  }
  // --- the widths of the columns
  List<uint32_t> col_w(a), col_min(a), col_max(a);
  for (uint32_t c = 0; c < ncols; ++c) { col_w.push_back(0); col_min.push_back(0); col_max.push_back(0); }
  auto item_min = [&](const Item& it) { return it.min_content + it.edges.horizontal() + it.margin_left + it.margin_right; };
  auto item_max = [&](const Item& it) {
    uint32_t w = it.max_content + it.edges.horizontal() + it.margin_left + it.margin_right;
    if (it.style.width.is_px()) w = cols_of(it.style.width, 4000) + (it.style.sizing == BoxSizing::BorderBox ? 0 : it.edges.horizontal()) + it.margin_left + it.margin_right;
    return w;
  };
  // single-track items first, then those that span tracks
  for (int pass = 0; pass < 2; ++pass) {
    for (uint64_t i = 0; i < items.count(); ++i) {
      const Item& it = *items[i];
      const bool single = it.column_span == 1;
      if (single != (pass == 0)) continue;
      const uint32_t mn = item_min(it), mx = item_max(it);
      if (single) {
        const uint32_t c = static_cast<uint32_t>(it.column);
        if (mn > col_min[c]) col_min[c] = mn;
        if (mx > col_max[c]) col_max[c] = mx;
      } else {
        // the extra width of a spanning item goes to the tracks it covers
        uint32_t have_min = col_gap * (it.column_span - 1), have_max = col_gap * (it.column_span - 1);
        for (int32_t k = 0; k < it.column_span; ++k) {
          have_min += col_min[static_cast<uint32_t>(it.column + k)];
          have_max += col_max[static_cast<uint32_t>(it.column + k)];
        }
        for (int pass2 = 0; pass2 < 2; ++pass2) {
          const uint32_t want = pass2 == 0 ? mn : mx, have = pass2 == 0 ? have_min : have_max;
          if (want <= have) continue;
          uint32_t extra = want - have, share = extra / static_cast<uint32_t>(it.column_span), rest = extra % static_cast<uint32_t>(it.column_span);
          for (int32_t k = 0; k < it.column_span; ++k) {
            const uint32_t c = static_cast<uint32_t>(it.column + k);
            (pass2 == 0 ? col_min[c] : col_max[c]) += share + (static_cast<uint32_t>(k) < rest ? 1 : 0);
          }
        }
      }
    }
  }
  uint32_t fr_total = 0;
  for (uint32_t c = 0; c < ncols; ++c) {
    const TrackDef& d = col_defs[c];
    uint32_t w;
    if (d.max_kind == TrackDef::Fixed && d.min_kind == TrackDef::Fixed) w = static_cast<uint32_t>(d.min_value);
    else if (d.min_kind == TrackDef::Fixed) w = static_cast<uint32_t>(d.min_value);
    else if (d.max_kind == TrackDef::Fr) { w = col_min[c]; fr_total += static_cast<uint32_t>(d.max_value); }
    else if (d.max_kind == TrackDef::Fixed) w = col_max[c] < static_cast<uint32_t>(d.max_value) ? col_max[c] : static_cast<uint32_t>(d.max_value);
    else if (d.max_kind == TrackDef::MinContent || (d.min_kind == TrackDef::MinContent && d.max_kind != TrackDef::MaxContent)) w = col_min[c];
    else w = col_max[c];
    if (d.min_kind == TrackDef::Fixed && w < static_cast<uint32_t>(d.min_value)) w = static_cast<uint32_t>(d.min_value);
    col_w[c] = w < 1 ? 1 : w;
  }
  uint32_t total = col_gap * (ncols - 1);
  for (uint32_t c = 0; c < ncols; ++c) total += col_w[c];
  const bool table_fixed = table_mode && s.table_fixed;
  if (measuring_) {
    uint32_t min_total = col_gap * (ncols - 1);
    for (uint32_t c = 0; c < ncols; ++c) {
      const TrackDef& d = col_defs[c];
      min_total += d.min_kind == TrackDef::Fixed ? static_cast<uint32_t>(d.min_value) : col_min[c];
    }
    measured_min_ = min_total;
    for (uint32_t c = 0; c < ncols; ++c) {   // (fr tracks measure as their content)
      const TrackDef& d = col_defs[c];
      if (d.max_kind == TrackDef::Fr) col_w[c] = col_max[c] < 1 ? 1 : col_max[c];
    }
    total = col_gap * (ncols - 1);
    for (uint32_t c = 0; c < ncols; ++c) total += col_w[c];
    measured_max_ = total;
  } else {
    if (fr_total > 0 && total < content_cols) {
      uint32_t free = content_cols - total;
      // each fr track has its minimum content already; the free room is shared by the fr
      uint32_t given = 0, fr_seen = 0;
      for (uint32_t c = 0; c < ncols; ++c) {
        const TrackDef& d = col_defs[c];
        if (d.max_kind != TrackDef::Fr) continue;
        fr_seen += static_cast<uint32_t>(d.max_value);
        const uint32_t share = static_cast<uint32_t>(static_cast<uint64_t>(free + [&] { uint32_t base = 0; for (uint32_t k = 0; k < ncols; ++k) if (col_defs[k].max_kind == TrackDef::Fr) base += col_w[k]; return base; }()) * static_cast<uint32_t>(d.max_value) / fr_total);
        // (the fr tracks share the room that is left together with what their minimums took)
        const uint32_t target = share > col_w[c] ? share : col_w[c];
        given += target - col_w[c];
        col_w[c] = target;
      }
      (void)fr_seen;
      (void)given;
      total = col_gap * (ncols - 1);
      for (uint32_t c = 0; c < ncols; ++c) total += col_w[c];
      // rounding or minimums: trim from the widest fr track when the sum went over
      while (total > content_cols) {
        int64_t widest = -1;
        for (uint32_t c = 0; c < ncols; ++c)
          if (col_defs[c].max_kind == TrackDef::Fr && col_w[c] > 1 && (widest < 0 || col_w[c] > col_w[static_cast<uint32_t>(widest)])) widest = c;
        if (widest < 0) break;
        --col_w[static_cast<uint32_t>(widest)];
        --total;
      }
      while (total < content_cols) {   // (the columns lost to rounding go to the last fr track)
        int64_t last = -1;
        for (uint32_t c = 0; c < ncols; ++c)
          if (col_defs[c].max_kind == TrackDef::Fr) last = c;
        if (last < 0) break;
        ++col_w[static_cast<uint32_t>(last)];
        ++total;
      }
    } else if (total < content_cols && fr_total == 0 && (s.justify_content == Place::Normal || s.justify_content == Place::Stretch) && !table_fixed) {
      // auto tracks stretch to fill the container (for a table: when it has a width)
      const bool fills = !table_mode || s.width.is_px() || s.width.is_auto() == false;
      uint32_t autos = 0;
      for (uint32_t c = 0; c < ncols; ++c)
        if (col_defs[c].max_kind == TrackDef::Auto && col_defs[c].min_kind != TrackDef::Fixed) ++autos;
      if (autos > 0 && fills) {
        const uint32_t free = content_cols - total, share = free / autos, rest = free % autos;
        uint32_t seen = 0;
        for (uint32_t c = 0; c < ncols; ++c)
          if (col_defs[c].max_kind == TrackDef::Auto && col_defs[c].min_kind != TrackDef::Fixed) col_w[c] += share + (seen++ < rest ? 1 : 0);
        total = content_cols;
      }
    }
    if (table_fixed) {
      const uint32_t usable = content_cols > col_gap * (ncols - 1) ? content_cols - col_gap * (ncols - 1) : ncols;
      for (uint32_t c = 0; c < ncols; ++c) col_w[c] = usable / ncols + (c < usable % ncols ? 1 : 0);
      total = content_cols;
    }
    if (total > content_cols) {
      // too wide: the tracks that can shrink go down toward their minimum
      for (int guard = 0; guard < 64 && total > content_cols; ++guard) {
        uint32_t room = 0;
        for (uint32_t c = 0; c < ncols; ++c) {
          const uint32_t floor = col_defs[c].min_kind == TrackDef::Fixed ? static_cast<uint32_t>(col_defs[c].min_value) : col_min[c] < 1 ? 1 : col_min[c];
          if (col_w[c] > floor) room += col_w[c] - floor;
        }
        if (room == 0) break;
        const uint32_t need = total - content_cols;
        uint32_t taken = 0;
        for (uint32_t c = 0; c < ncols; ++c) {
          const uint32_t floor = col_defs[c].min_kind == TrackDef::Fixed ? static_cast<uint32_t>(col_defs[c].min_value) : col_min[c] < 1 ? 1 : col_min[c];
          if (col_w[c] <= floor) continue;
          uint32_t cut = static_cast<uint32_t>(static_cast<uint64_t>(need) * (col_w[c] - floor) / room);
          if (cut == 0 && taken < need) cut = 1;
          if (cut > col_w[c] - floor) cut = col_w[c] - floor;
          if (taken + cut > need) cut = need - taken;
          col_w[c] -= cut;
          taken += cut;
        }
        if (taken == 0) break;
        total -= taken;
      }
      // still too wide (words that do not break): every track gets what it can, but the grid is cut to the container
      if (total > content_cols) {
        for (int guard = 0; guard < 400 && total > content_cols; ++guard) {
          int64_t widest = -1;
          for (uint32_t c = 0; c < ncols; ++c)
            if (col_w[c] > 1 && (widest < 0 || col_w[c] > col_w[static_cast<uint32_t>(widest)])) widest = c;
          if (widest < 0) break;
          --col_w[static_cast<uint32_t>(widest)];
          --total;
        }
      }
    }
  }
  // --- the items drawn at their widths, then the heights of the rows
  List<uint32_t> col_x(a);
  {
    uint32_t x = 0;
    for (uint32_t c = 0; c < ncols; ++c) {
      col_x.push_back(x);
      x += col_w[c] + col_gap;
    }
  }
  auto area_width = [&](const Item& it) {
    uint32_t w = col_gap * (static_cast<uint32_t>(it.column_span) - 1);
    for (int32_t k = 0; k < it.column_span; ++k) w += col_w[static_cast<uint32_t>(it.column + k)];
    return w;
  };
  List<uint32_t> row_h(a), row_min(a);
  for (uint32_t r = 0; r < nrows; ++r) { row_h.push_back(0); row_min.push_back(0); }
  for (uint64_t i = 0; i < items.count() && !failed_; ++i) {
    Item& it = *items[i];
    const uint32_t aw = area_width(it);
    Place justify = it.style.justify_self == Place::Auto ? (table_mode ? Place::Stretch : s.justify_items) : it.style.justify_self;
    const uint32_t spare_margins = it.margin_left + it.margin_right;
    uint32_t outer;
    const bool stretch_x = (justify == Place::Normal || justify == Place::Stretch) && it.style.width.is_auto() && !it.auto_left && !it.auto_right;
    if (stretch_x) outer = aw > spare_margins ? aw - spare_margins : 1;
    else if (it.style.width.is_px()) outer = cols_of(it.style.width, 4000) + (it.style.sizing == BoxSizing::BorderBox ? 0 : it.edges.horizontal());
    else outer = it.max_content + it.edges.horizontal();
    if (outer + spare_margins > aw && aw > spare_margins) outer = aw - spare_margins;
    if (outer < it.edges.horizontal() + 1) outer = it.edges.horizontal() + 1;
    it.main = outer;
    it.box.clear();
    if (it.node->type == dom::NodeType::Text) {
      if (!draw_text_item(it.node, inside, outer, &it.box)) return false;
    } else if (!build_box(it.node, it.style, aw, outer, 0, &it.box)) {
      return false;
    }
    const uint32_t h = it.box.lines() + it.margin_top + it.margin_bottom;
    if (it.row_span == 1) {
      if (h > row_min[static_cast<uint32_t>(it.row)]) row_min[static_cast<uint32_t>(it.row)] = h;
    }
  }
  // row sizes
  for (uint32_t r = 0; r < nrows; ++r) {
    const TrackDef& d = row_defs[r];
    uint32_t h;
    if (d.min_kind == TrackDef::Fixed && d.max_kind == TrackDef::Fixed) h = static_cast<uint32_t>(d.min_value);
    else if (d.min_kind == TrackDef::Fixed) h = static_cast<uint32_t>(d.min_value) > row_min[r] ? static_cast<uint32_t>(d.min_value) : row_min[r];
    else if (d.max_kind == TrackDef::Fixed) h = row_min[r] < static_cast<uint32_t>(d.max_value) ? row_min[r] : static_cast<uint32_t>(d.max_value);
    else h = row_min[r];
    row_h[r] = h;
  }
  // items that span rows
  for (uint64_t i = 0; i < items.count(); ++i) {
    const Item& it = *items[i];
    if (it.row_span <= 1) continue;
    uint32_t have = row_gap * (static_cast<uint32_t>(it.row_span) - 1);
    for (int32_t k = 0; k < it.row_span; ++k) have += row_h[static_cast<uint32_t>(it.row + k)];
    const uint32_t need = it.box.lines() + it.margin_top + it.margin_bottom;
    if (need > have) row_h[static_cast<uint32_t>(it.row + it.row_span - 1)] += need - have;
  }
  uint32_t total_h = row_gap * (nrows - 1);
  for (uint32_t r = 0; r < nrows; ++r) total_h += row_h[r];
  if (!measuring_ && content_lines > total_h) {   // a grid with a height shares the rest out to the fr rows, or to the automatic ones
    uint32_t free = content_lines - total_h;
    uint32_t frs = 0;
    for (uint32_t r = 0; r < nrows; ++r) frs += row_defs[r].max_kind == TrackDef::Fr ? static_cast<uint32_t>(row_defs[r].max_value) : 0;
    if (frs > 0) {
      uint32_t given = 0, last = 0;
      for (uint32_t r = 0; r < nrows; ++r) {
        if (row_defs[r].max_kind != TrackDef::Fr) continue;
        const uint32_t share = static_cast<uint32_t>(static_cast<uint64_t>(free) * static_cast<uint32_t>(row_defs[r].max_value) / frs);
        row_h[r] += share;
        given += share;
        last = r;
      }
      row_h[last] += free - given;
    } else if (s.align_content == Place::Normal || s.align_content == Place::Stretch) {
      uint32_t autos = 0;
      for (uint32_t r = 0; r < nrows; ++r)
        if (row_defs[r].max_kind == TrackDef::Auto && row_defs[r].min_kind != TrackDef::Fixed) ++autos;
      if (autos > 0) {
        const uint32_t share = free / autos, rest = free % autos;
        uint32_t seen = 0;
        for (uint32_t r = 0; r < nrows; ++r)
          if (row_defs[r].max_kind == TrackDef::Auto && row_defs[r].min_kind != TrackDef::Fixed) row_h[r] += share + (seen++ < rest ? 1 : 0);
      }
    }
    total_h = row_gap * (nrows - 1);
    for (uint32_t r = 0; r < nrows; ++r) total_h += row_h[r];
  }
  List<uint32_t> row_y(a);
  {
    uint32_t y = 0;
    for (uint32_t r = 0; r < nrows; ++r) {
      row_y.push_back(y);
      y += row_h[r] + row_gap;
    }
  }
  // --- the alignment of each item in its area, stretched ones drawn again at the height they got
  struct Placement {
    uint32_t item, x, y, w, h, ox, oy;
  };
  List<Placement> places(a);
  for (uint64_t i = 0; i < items.count() && !failed_; ++i) {
    Item& it = *items[i];
    Placement pl;
    pl.item = static_cast<uint32_t>(i);
    pl.x = col_x[static_cast<uint32_t>(it.column)];
    pl.y = row_y[static_cast<uint32_t>(it.row)];
    pl.w = area_width(it);
    pl.h = row_gap * (static_cast<uint32_t>(it.row_span) - 1);
    for (int32_t k = 0; k < it.row_span; ++k) pl.h += row_h[static_cast<uint32_t>(it.row + k)];
    const Place align = it.style.align_self == Place::Auto ? (table_mode ? Place::Normal : s.align_items) : it.style.align_self;
    const uint32_t box_h = it.box.lines();
    const uint32_t outer_h = box_h + it.margin_top + it.margin_bottom;
    if (table_mode && it.node->type == dom::NodeType::Element && it.style.vertical_align == 0 && outer_h < pl.h) {   // table cells stretch to the row
      const uint32_t target = pl.h - it.margin_top - it.margin_bottom;
      it.box.clear();
      if (!build_box(it.node, it.style, pl.w, it.main, target, &it.box)) return false;
    } else if (!table_mode && (align == Place::Normal || align == Place::Stretch) && it.node->type == dom::NodeType::Element && it.style.height.is_auto() && outer_h < pl.h &&
               !it.auto_top && !it.auto_bottom) {
      const uint32_t target = pl.h - it.margin_top - it.margin_bottom;
      it.box.clear();
      if (!build_box(it.node, it.style, pl.w, it.main, target, &it.box)) return false;
    }
    const uint32_t final_h = it.box.lines() + it.margin_top + it.margin_bottom;
    const uint32_t free_v = pl.h > final_h ? pl.h - final_h : 0;
    uint32_t oy = it.margin_top;
    if (it.auto_top && it.auto_bottom) oy += free_v / 2;
    else if (it.auto_top) oy += free_v;
    else if (table_mode) oy += it.style.vertical_align == 2 ? free_v / 2 : it.style.vertical_align == 3 ? free_v : 0;
    else if (align == Place::Center) oy += free_v / 2;
    else if (align == Place::End || align == Place::FlexEnd) oy += free_v;
    pl.oy = oy;
    const Place justify = it.style.justify_self == Place::Auto ? (table_mode ? Place::Stretch : s.justify_items) : it.style.justify_self;
    const uint32_t free_h = pl.w > it.box.width + it.margin_left + it.margin_right ? pl.w - it.box.width - it.margin_left - it.margin_right : 0;
    uint32_t ox = it.margin_left;
    if (it.auto_left && it.auto_right) ox += free_h / 2;
    else if (it.auto_left) ox += free_h;
    else if (justify == Place::Center) ox += free_h / 2;
    else if (justify == Place::End || justify == Place::Right) ox += free_h;
    else if (table_mode && s.align == 1 && false) ox += free_h / 2;
    pl.ox = ox;
    places.push_back(pl);
  }
  // --- compose the lines
  uint32_t grid_w = 0;
  for (uint32_t c = 0; c < ncols; ++c) grid_w = col_x[c] + col_w[c];
  const uint32_t out_w = measuring_ ? grid_w : content_cols;
  // grid-level alignment of the whole grid when it is narrower than the container
  uint32_t lead = 0;
  if (!measuring_ && grid_w < content_cols) {
    const uint32_t free = content_cols - grid_w;
    if (s.justify_content == Place::End || s.justify_content == Place::FlexEnd || s.justify_content == Place::Right) lead = free;
    else if (s.justify_content == Place::Center) lead = free / 2;
  }
  const uint32_t lines_total = total_h;
  // places sorted by column position
  for (uint64_t i = 1; i < places.count(); ++i) {
    uint64_t k = i;
    while (k > 0 && places[k - 1].x > places[k].x) {
      Placement tmp = places[k - 1];
      places[k - 1] = places[k];
      places[k] = tmp;
      --k;
    }
  }
  auto spaces = [&](uint32_t n_cols) {
    if (n_cols == 0) return;
    LinePiece p(a);
    for (uint32_t k = 0; k < n_cols; ++k) p.text.push_back(' ');
    failed_ |= !out->pieces.push_back(util::move(p));
  };
  for (uint32_t line = 0; line < lines_total && !failed_; ++line) {
    uint32_t cursor = 0;
    spaces(lead);
    cursor = lead;
    for (uint64_t i = 0; i < places.count() && !failed_; ++i) {
      const Placement& pl = places[i];
      if (line < pl.y || line >= pl.y + pl.h) continue;
      const uint32_t px = pl.x + lead;
      if (px < cursor) continue;   // (overlapping items: the first one wins)
      spaces(px - cursor);
      cursor = px;
      Item& it = *items[pl.item];
      uint32_t used = 0;
      const int64_t y = static_cast<int64_t>(line) - pl.y - pl.oy;
      if (y >= 0 && y < it.box.lines()) {
        spaces(pl.ox);
        used = pl.ox;
        for (uint32_t k = y ? it.box.ends[static_cast<uint32_t>(y) - 1] : 0; k < it.box.ends[static_cast<uint32_t>(y)]; ++k) {
          used += static_cast<uint32_t>(it.box.pieces[k].text.size());
          LinePiece copy(a);
          copy.text.assign(it.box.pieces[k].text.view());
          copy.flags = it.box.pieces[k].flags;
          copy.level = it.box.pieces[k].level;
          copy.color = it.box.pieces[k].color;
          copy.background = it.box.pieces[k].background;
          copy.node = it.box.pieces[k].node;
          copy.link = it.box.pieces[k].link;
          failed_ |= !out->pieces.push_back(util::move(copy));
        }
      }
      if (used < pl.w) spaces(pl.w - used);
      cursor += pl.w;
    }
    if (!measuring_ && cursor < out_w) spaces(out_w - cursor);
    failed_ |= !out->ends.push_back(static_cast<uint32_t>(out->pieces.count()));
  }
  if (lines_total == 0) {
    failed_ |= !out->ends.push_back(static_cast<uint32_t>(out->pieces.count()));
  }
  if (measuring_) out->width = grid_w;
  return !failed_;
}

bool DocumentRenderer::layout_columns(const dom::Node*, const Style&, uint32_t, Raster*) { return false; }

}  // namespace browser
