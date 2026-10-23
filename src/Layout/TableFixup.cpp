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

// Tables in the box tree: what a browser's own style sheet and the old presentational attributes say about <table>, <tr>, <td> ..., the anonymous
// boxes of CSS 2.1 section 17.2.1 (a cell outside a row gets a row, a row outside a table gets a table, loose content in a table gets a row and a
// cell), the resolution of collapsed borders (17.6.2: the winner of each edge is worked out here and written into the styles of the cells, so layout
// and painting only see ordinary borders: each cell holds its half of an edge, the table the outer half), and the wrapper box that keeps a table's
// captions outside its border.
#include <initializer_list>
#include "Layout/Table.hpp"
#include "Browser/Css.hpp"
#include "Dom/Dom.hpp"

namespace layout {

using browser::BorderStyle;
using browser::Display;
using browser::Float;
using browser::Layout;
using browser::Length;
using browser::Style;

namespace {

enum class Role : uint8_t { None, Table, Group, Row, Cell, Caption };

Role role_of(const dom::Node* n) {
  if (n->is_tag("table")) return Role::Table;
  if (n->is_tag("thead") || n->is_tag("tbody") || n->is_tag("tfoot")) return Role::Group;
  if (n->is_tag("tr")) return Role::Row;
  if (n->is_tag("td") || n->is_tag("th")) return Role::Cell;
  if (n->is_tag("caption")) return Role::Caption;
  return Role::None;
}

// A number attribute: digits, then perhaps a % (width="50%"). Anything else is not one.
struct Number {
  bool ok = false, percent = false;
  int64_t value = 0;
};

Number number_attribute(const dom::Node* n, const char* name) {
  Number out;
  StringView v;
  if (!dom::get_attribute(n, name, &v)) return out;
  v = v.trimmed();
  uint64_t i = 0;
  int64_t value = 0;
  for (; i < v.size() && v[i] >= '0' && v[i] <= '9'; ++i) value = value * 10 + (v[i] - '0') > 1000000 ? 1000000 : value * 10 + (v[i] - '0');
  if (i == 0) return out;
  out.ok = true;
  out.value = value;
  if (i < v.size() && v[i] == '%') out.percent = true;
  return out;
}

Length percent_length(int64_t percent) {
  Length l = Length::px(0);
  l.percent100 = static_cast<int32_t>(percent * 100);
  return l;
}

const dom::Node* table_of(const dom::Node* n) {
  const dom::Node* p = n->parent;
  for (int hops = 0; p && hops < 8; ++hops, p = p->parent)
    if (p->is_tag("table")) return p;
  return nullptr;
}

constexpr int32_t kUndeclared = -100;       // what the probe style holds where the page's style sheet said nothing
constexpr uint8_t kUndeclaredAlign = 200;
constexpr uint32_t kGray = 0xFF808080u;

bool valign_attribute(const dom::Node* n, uint8_t* out) {
  StringView v;
  if (!dom::get_attribute(n, "valign", &v)) return false;
  v = v.trimmed();
  if (v.equals_ignore_case("top")) *out = 1;
  else if (v.equals_ignore_case("middle") || v.equals_ignore_case("center")) *out = 2;
  else if (v.equals_ignore_case("bottom")) *out = 3;
  else if (v.equals_ignore_case("baseline")) *out = 0;
  else return false;
  return true;
}

}  // namespace

void apply_table_presentation(const dom::Node* n, const Style& parent, const browser::StyleSheet& sheet, Style* s) {
  const Role role = role_of(n);
  if (role == Role::None) return;
  // What did the page's own style sheet say? The same cascade on a style where every value this cares about holds a mark.
  Style probe = parent.inherit();
  for (int i = 0; i < 4; ++i) {
    probe.padding[i] = Length::px(kUndeclared);
    probe.margin[i] = Length::px(kUndeclared);
    probe.border_px100[i] = kUndeclared;
  }
  probe.vertical_align = kUndeclaredAlign;
  probe.border_spacing = Length::px(kUndeclared);
  StringView inline_style;
  dom::get_attribute(n, "style", &inline_style);
  sheet.apply(n, inline_style, parent, &probe);
  auto declared_padding = [&](int i) { return !(probe.padding[i].is_px() && probe.padding[i].px100 == kUndeclared); };
  auto declared_margin = [&](int i) { return !(probe.margin[i].is_px() && probe.margin[i].px100 == kUndeclared); };
  auto declared_border = [&](int i) { return probe.border_px100[i] != kUndeclared; };
  Layout natural = Layout::Flow;
  switch (role) {
    case Role::Table: natural = Layout::Table; break;
    case Role::Group: natural = Layout::TableRowGroup; break;
    case Role::Row: natural = Layout::TableRow; break;
    case Role::Cell: natural = Layout::TableCell; break;
    case Role::Caption: natural = Layout::TableCaption; break;
    case Role::None: break;
  }
  const bool unset = probe.display == Display::Inline && probe.layout == Layout::Flow && s->layout == Layout::Flow;   // (the page said nothing about display)
  if (!unset && s->layout != natural) return;   // the page says this is another kind of box
  s->layout = natural;
  if (!s->inline_box) s->display = Display::Block;
  if (role == Role::Table) {
    s->sizing = browser::BoxSizing::BorderBox;   // (a table's width is that of its border box)
    for (int i : {0, 2})
      if (!declared_margin(i)) s->margin[i] = Length::px(0);
    if (probe.border_spacing.is_px() && probe.border_spacing.px100 == kUndeclared) {
      const Number spacing = number_attribute(n, "cellspacing");
      s->border_spacing = Length::px(spacing.ok && !spacing.percent ? static_cast<int32_t>(spacing.value * 100) : 200);
    }
    const Number border = number_attribute(n, "border");
    StringView raw;
    const bool border_present = dom::get_attribute(n, "border", &raw);
    const int64_t border_width = border.ok ? border.value : (border_present ? 1 : 0);
    if (border_width > 0)
      for (int i = 0; i < 4; ++i)
        if (!declared_border(i)) {
          s->border_px100[i] = static_cast<int32_t>(border_width * 100);
          s->border_style[i] = BorderStyle::Outset;
          s->border_color[i] = kGray;
        }
    StringView align;
    if (!dom::get_attribute(n, "align", &align) && !declared_margin(1) && !declared_margin(3)) {
      // <center> and align=center on a block (text-align: -webkit-center in browsers) centre the tables inside them too
      for (const dom::Node* p = n->parent; p && !p->is_tag("td") && !p->is_tag("th") && !p->is_tag("table"); p = p->parent) {
        StringView a;
        if (p->is_tag("center") || (dom::get_attribute(p, "align", &a) && a.trimmed().equals_ignore_case("center"))) {
          s->margin[1] = s->margin[3] = Length();
          break;
        }
        if (p->is_tag("body") || p->is_tag("html")) break;
      }
    }
    if (dom::get_attribute(n, "align", &align)) {
      s->align = parent.align;   // (the attribute is about the table, not the text in it)
      if (s->float_side == Float::None) {
        if (align.equals_ignore_case("left")) s->float_side = Float::Left;
        else if (align.equals_ignore_case("right")) s->float_side = Float::Right;
        else if (align.equals_ignore_case("center") && !declared_margin(1) && !declared_margin(3)) s->margin[1] = s->margin[3] = Length();
      }
    }
  }
  if (role == Role::Table || role == Role::Cell) {
    const Number width = number_attribute(n, "width"), height = number_attribute(n, "height");
    if (width.ok && s->width.is_auto()) s->width = width.percent ? percent_length(width.value) : Length::px(static_cast<int32_t>(width.value * 100));
    if (height.ok && s->height.is_auto()) s->height = height.percent ? percent_length(height.value) : Length::px(static_cast<int32_t>(height.value * 100));
  }
  if (role == Role::Group || role == Role::Row) {
    uint8_t valign = kUndeclaredAlign;
    if (probe.vertical_align != kUndeclaredAlign) valign = probe.vertical_align;
    else if (!valign_attribute(n, &valign)) valign = parent.layout == Layout::TableRowGroup ? parent.vertical_align : 2;
    s->vertical_align = valign;
  }
  if (role == Role::Cell) {
    for (int i : {0, 1, 2, 3}) s->margin[i] = Length::px(0);   // (margins do not apply to cells)
    const dom::Node* table = table_of(n);
    const Number padding = table ? number_attribute(table, "cellpadding") : Number();
    for (int i = 0; i < 4; ++i)
      if (!declared_padding(i)) s->padding[i] = Length::px(padding.ok && !padding.percent ? static_cast<int32_t>(padding.value * 100) : 100);
    const Number border = table ? number_attribute(table, "border") : Number();
    StringView raw;
    if (table && ((border.ok && border.value > 0) || (!border.ok && dom::get_attribute(table, "border", &raw))))
      for (int i = 0; i < 4; ++i)
        if (!declared_border(i)) {
          s->border_px100[i] = 100;
          s->border_style[i] = BorderStyle::Inset;
          s->border_color[i] = kGray;
        }
    if (probe.vertical_align != kUndeclaredAlign) s->vertical_align = probe.vertical_align;
    else if (!valign_attribute(n, &s->vertical_align)) s->vertical_align = parent.layout == Layout::TableRow ? parent.vertical_align : 2;
    if (dom::has_attribute(n, "nowrap")) s->white_space = 1;
  }
}

void read_table_attributes(const dom::Node* n, Box* box) {
  const Role role = role_of(n);
  if (role == Role::Cell) {
    const Number cs = number_attribute(n, "colspan"), rs = number_attribute(n, "rowspan");
    box->colspan = cs.ok && cs.value >= 1 ? static_cast<uint16_t>(cs.value > 1000 ? 1000 : cs.value) : 1;
    box->rowspan = rs.ok ? static_cast<uint16_t>(rs.value > 65000 ? 65000 : rs.value) : 1;
  } else if (role == Role::Group) {
    box->group_role = n->is_tag("thead") ? 1 : n->is_tag("tfoot") ? 2 : 0;
  }
  if (role == Role::Cell || role == Role::Table) box->establishes_bfc = true;
}

namespace {

enum class Part : uint8_t { Other, Cell, Row, Group, Caption };

Part part_of(const Box& b) {
  if (b.kind == BoxKind::Text) return Part::Other;
  switch (b.style.layout) {
    case Layout::TableCell: return Part::Cell;
    case Layout::TableRow: return Part::Row;
    case Layout::TableRowGroup: return Part::Group;
    case Layout::TableCaption: return Part::Caption;
    default: return Part::Other;
  }
}

bool blank_text(const Box& b) {
  if (b.kind != BoxKind::Text || b.style.pre || b.style.white_space == 2 || b.style.white_space == 3 || b.style.white_space == 5) return false;
  for (uint64_t i = 0; i < b.text.size(); ++i) {
    const char c = b.text[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
  }
  return true;
}

struct Fixer {
  BoxTree& tree;
  List<BoxId>* cells;
  bool ok = true;

  BoxId make(const Box& like, Layout layout) {
    const BoxId anon = tree.create(BoxKind::Anonymous, 0, like.style.inherit());
    if (anon == kNoBox) { ok = false; return kNoBox; }
    Box& b = tree.box(anon);
    b.style.display = Display::Block;
    b.style.layout = layout;
    b.style.vertical_align = (like.style.layout == Layout::TableRow || like.style.layout == Layout::TableRowGroup) ? like.style.vertical_align : 2;
    if (layout == Layout::TableCell) b.establishes_bfc = true;
    if (layout == Layout::Table) b.establishes_bfc = true;
    return anon;
  }

  void move_into(BoxId into, const List<BoxId>& run) {
    for (uint64_t i = 0; i < run.count(); ++i) ok &= tree.append(into, run[i]);
  }

  // Loose content (blank text between boxes is dropped): a row holding a cell holding it.
  BoxId wrap_content(const Box& like, const List<BoxId>& run, bool with_row) {
    const BoxId cell = make(like, Layout::TableCell);
    if (cell == kNoBox) return kNoBox;
    move_into(cell, run);
    ok &= cells->push_back(cell);
    if (!with_row) return cell;
    const BoxId row = make(like, Layout::TableRow);
    if (row == kNoBox) return kNoBox;
    ok &= tree.append(row, cell);
    return row;
  }

  BoxId wrap_cells(const Box& like, const List<BoxId>& run) {
    const BoxId row = make(like, Layout::TableRow);
    if (row == kNoBox) return kNoBox;
    move_into(row, run);
    return row;
  }

  void run(BoxId id) {
    Box& p = tree.box(id);
    const Layout pl = p.style.layout;
    const bool in_table = pl == Layout::Table, in_group = pl == Layout::TableRowGroup, in_row = pl == Layout::TableRow;
    if (p.children.count() == 0) return;
    if (!in_table && !in_group && !in_row) {
      bool any = false;
      for (uint64_t i = 0; i < p.children.count() && !any; ++i) {
        const Part part = part_of(tree.box(p.children[i]));
        any = part == Part::Cell || part == Part::Row || part == Part::Group;
      }
      if (!any) return;
    }
    List<BoxId> result(tree.allocator()), group(tree.allocator()), pending(tree.allocator());
    enum class Run : uint8_t { None, Cells, Loose, Parts } kind = Run::None;
    auto flush = [&]() {
      if (kind == Run::None) return;
      BoxId made = kNoBox;
      if (kind == Run::Cells) made = wrap_cells(p, group);
      else if (kind == Run::Loose) made = wrap_content(p, group, !in_row);
      else {   // table parts outside a table: an anonymous table around them
        made = make(p, Layout::Table);
        if (made != kNoBox) {
          move_into(made, group);
          run(made);
          tree.box(made).inline_content = false;
        }
      }
      if (made != kNoBox) ok &= result.push_back(made);
      group.clear();
      kind = Run::None;
    };
    for (uint64_t i = 0; i < p.children.count() && ok; ++i) {
      const BoxId cid = p.children[i];
      const Box& c = tree.box(cid);
      const Part part = part_of(c);
      if (c.out_of_flow) { ok &= result.push_back(cid); continue; }
      if (!in_table && !in_group && !in_row) {
        const bool table_part = part == Part::Cell || part == Part::Row || part == Part::Group;
        if (table_part) {
          if (kind != Run::Parts) flush();
          kind = Run::Parts;
          for (uint64_t k = 0; k < pending.count(); ++k) ok &= group.push_back(pending[k]);
          pending.clear();
          ok &= group.push_back(cid);
        } else if (blank_text(c) && kind == Run::Parts) {
          ok &= pending.push_back(cid);
        } else {
          flush();
          for (uint64_t k = 0; k < pending.count(); ++k) ok &= result.push_back(pending[k]);
          pending.clear();
          ok &= result.push_back(cid);
        }
        continue;
      }
      const bool allowed = in_table ? (part == Part::Row || part == Part::Group || part == Part::Caption) : in_group ? part == Part::Row : part == Part::Cell;
      if (allowed) {
        flush();
        pending.clear();   // (blank text between table parts makes no box)
        ok &= result.push_back(cid);
      } else if (blank_text(c)) {
        if (kind != Run::None) ok &= pending.push_back(cid);
      } else {
        const Run want = (part == Part::Cell && !in_row) ? Run::Cells : Run::Loose;
        if (kind != want) flush();
        kind = want;
        for (uint64_t k = 0; k < pending.count(); ++k) ok &= group.push_back(pending[k]);
        pending.clear();
        ok &= group.push_back(cid);
      }
    }
    flush();
    Box& again = tree.box(id);
    again.children.clear();
    for (uint64_t i = 0; i < result.count(); ++i) {
      ok &= again.children.push_back(result[i]);
      tree.box(result[i]).parent = id;
    }
  }
};

}  // namespace

bool fixup_table_parts(BoxTree& tree, BoxId id, List<BoxId>* cells) {
  Fixer fixer{tree, cells};
  fixer.run(id);
  return fixer.ok;
}

namespace {

// --- collapsed borders

struct Edge {
  int32_t width = 0;
  BorderStyle style = BorderStyle::None;
  uint32_t color = 0;
  uint8_t rank = 0;      // who it is from: the table 1, a row group 2, a row 3, a cell 4 (the closer one wins a tie)
  bool set = false;
};

int style_order(BorderStyle s) {
  switch (s) {
    case BorderStyle::Double: return 8;
    case BorderStyle::Solid: return 7;
    case BorderStyle::Dashed: return 6;
    case BorderStyle::Dotted: return 5;
    case BorderStyle::Ridge: return 4;
    case BorderStyle::Outset: return 3;
    case BorderStyle::Groove: return 2;
    case BorderStyle::Inset: return 1;
    default: return 0;
  }
}

// CSS 2.1 section 17.6.2.1: hidden wins over everything, then the wider border, then the style, then the closer element.
void offer(Edge* edge, const Style& s, int side, uint8_t rank) {
  if (s.border_style[side] == BorderStyle::None) return;
  Edge cand;
  cand.set = true;
  cand.style = s.border_style[side];
  cand.width = cand.style == BorderStyle::Hidden ? 0 : (s.border_px100[side] > 0 ? s.border_px100[side] : 0);
  cand.color = s.border_color[side];
  cand.rank = rank;
  bool wins = false;
  if (!edge->set) wins = true;
  else if (edge->style == BorderStyle::Hidden) wins = false;
  else if (cand.style == BorderStyle::Hidden) wins = true;
  else if (cand.width != edge->width) wins = cand.width > edge->width;
  else if (style_order(cand.style) != style_order(edge->style)) wins = style_order(cand.style) > style_order(edge->style);
  else wins = cand.rank > edge->rank;
  if (wins) *edge = cand;
}

// An edge's width is shared by the two boxes at it: the one before it takes the bigger half when it is a whole number of pixels and odd.
int32_t half_before(int32_t w) { return w % 100 == 0 ? ((w / 100 + 1) / 2) * 100 : w - w / 2; }
int32_t half_after(int32_t w) { return w - half_before(w); }

void give(Style* s, int side, const Edge& e, int32_t width) {
  if (!e.set || e.style == BorderStyle::Hidden || width <= 0) {
    s->border_px100[side] = 0;
    s->border_style[side] = BorderStyle::None;
    return;
  }
  s->border_px100[side] = width;
  s->border_style[side] = e.style;
  s->border_color[side] = e.color;
}

// The widest of `count` edges `step` apart.
const Edge& widest(const List<Edge>& edges, uint64_t first, uint64_t count, uint64_t step) {
  uint64_t best = first;
  for (uint64_t i = 1; i < count; ++i)
    if (edges[first + i * step].width > edges[best].width) best = first + i * step;
  return edges[best];
}

bool resolve_collapsed_borders(BoxTree& tree, BoxId table_id) {
  TableGrid grid(tree.allocator());
  if (!build_table_grid(tree, table_id, &grid)) return false;
  const uint64_t R = grid.rows.count(), C = grid.columns;
  if (R == 0 || C == 0 || R * C > 100000) return true;
  List<Edge> h(tree.allocator()), v(tree.allocator());   // h: (R+1) x C edges above rows; v: R x (C+1) edges left of columns
  for (uint64_t i = 0; i < (R + 1) * C; ++i)
    if (!h.push_back(Edge())) return false;
  for (uint64_t i = 0; i < R * (C + 1); ++i)
    if (!v.push_back(Edge())) return false;
  Box& table = tree.box(table_id);
  const Style table_style = table.style;
  for (uint64_t c = 0; c < C; ++c) {
    offer(&h[c], table_style, 0, 1);
    offer(&h[R * C + c], table_style, 2, 1);
  }
  for (uint64_t r = 0; r < R; ++r) {
    offer(&v[r * (C + 1)], table_style, 3, 1);
    offer(&v[r * (C + 1) + C], table_style, 1, 1);
  }
  for (uint64_t r = 0; r < R; ++r) {
    const Style& rs = tree.box(grid.rows[r]).style;
    for (uint64_t c = 0; c < C; ++c) {
      offer(&h[r * C + c], rs, 0, 3);
      offer(&h[(r + 1) * C + c], rs, 2, 3);
    }
    offer(&v[r * (C + 1)], rs, 3, 3);
    offer(&v[r * (C + 1) + C], rs, 1, 3);
    const BoxId g = grid.row_groups[r];
    if (g == kNoBox) continue;
    const Style& gs = tree.box(g).style;
    offer(&v[r * (C + 1)], gs, 3, 2);
    offer(&v[r * (C + 1) + C], gs, 1, 2);
    if (r == 0 || grid.row_groups[r - 1] != g)
      for (uint64_t c = 0; c < C; ++c) offer(&h[r * C + c], gs, 0, 2);
    if (r + 1 == R || grid.row_groups[r + 1] != g)
      for (uint64_t c = 0; c < C; ++c) offer(&h[(r + 1) * C + c], gs, 2, 2);
  }
  for (uint64_t i = 0; i < grid.cells.count(); ++i) {
    const TableCell& cell = grid.cells[i];
    const Style& cs = tree.box(cell.box).style;
    for (uint32_t c = 0; c < cell.colspan; ++c) {
      offer(&h[static_cast<uint64_t>(cell.row) * C + cell.column + c], cs, 0, 4);
      offer(&h[static_cast<uint64_t>(cell.row + cell.rowspan) * C + cell.column + c], cs, 2, 4);
    }
    for (uint32_t r = 0; r < cell.rowspan; ++r) {
      offer(&v[static_cast<uint64_t>(cell.row + r) * (C + 1) + cell.column], cs, 3, 4);
      offer(&v[static_cast<uint64_t>(cell.row + r) * (C + 1) + cell.column + cell.colspan], cs, 1, 4);
    }
  }
  // each cell takes its half of the edges around it
  for (uint64_t i = 0; i < grid.cells.count(); ++i) {
    const TableCell& cell = grid.cells[i];
    Style& cs = tree.box(cell.box).style;
    const uint64_t top = static_cast<uint64_t>(cell.row) * C + cell.column, bottom = static_cast<uint64_t>(cell.row + cell.rowspan) * C + cell.column;
    const uint64_t left = static_cast<uint64_t>(cell.row) * (C + 1) + cell.column, right = left + cell.colspan;
    const Edge& et = widest(h, top, cell.colspan, 1);
    const Edge& eb = widest(h, bottom, cell.colspan, 1);
    const Edge& el = widest(v, left, cell.rowspan, C + 1);
    const Edge& er = widest(v, right, cell.rowspan, C + 1);
    give(&cs, 0, et, half_after(et.width));
    give(&cs, 2, eb, half_before(eb.width));
    give(&cs, 3, el, half_after(el.width));
    give(&cs, 1, er, half_before(er.width));
  }
  // the table keeps the outer halves; rows and groups have no borders of their own any more
  Style& ts = table.style;
  const Edge& tt = widest(h, 0, C, 1);
  const Edge& tb = widest(h, R * C, C, 1);
  const Edge& tl = widest(v, 0, R, C + 1);
  const Edge& tr = widest(v, C, R, C + 1);
  give(&ts, 0, tt, half_before(tt.width));
  give(&ts, 2, tb, half_after(tb.width));
  give(&ts, 3, tl, half_before(tl.width));
  give(&ts, 1, tr, half_after(tr.width));
  for (int i = 0; i < 4; ++i) ts.padding[i] = Length::px(0);
  for (uint64_t r = 0; r < R; ++r) {
    Style& rs = tree.box(grid.rows[r]).style;
    for (int i = 0; i < 4; ++i) { rs.border_style[i] = BorderStyle::None; rs.border_px100[i] = 0; }
    if (grid.row_groups[r] != kNoBox) {
      Style& gs = tree.box(grid.row_groups[r]).style;
      for (int i = 0; i < 4; ++i) { gs.border_style[i] = BorderStyle::None; gs.border_px100[i] = 0; }
    }
  }
  return true;
}

// The table gets a wrapper that holds its captions (above and below it) when it has some.
bool wrap_captions(BoxTree& tree, BoxId table_id) {
  Box& table = tree.box(table_id);
  const BoxId parent_id = table.parent;
  bool any = false;
  for (uint64_t i = 0; i < table.children.count(); ++i) any |= part_of(tree.box(table.children[i])) == Part::Caption;
  if (!any || parent_id == kNoBox) return true;
  uint64_t at = 0;
  Box& parent = tree.box(parent_id);
  while (at < parent.children.count() && parent.children[at] != table_id) ++at;
  if (at == parent.children.count()) return true;

  Style ws = table.style;
  ws.layout = Layout::Flow;
  ws.border_collapse = false;
  ws.border_spacing = Length();
  ws.table_fixed = false;
  ws.own_background = false;
  ws.own_background_color = 0;
  ws.background_argb = 0;
  ws.background_image = ws.background_size = ws.background_position = ws.background_repeat = ws.background_clip = ws.background_origin = ws.background_attachment = browser::TextRef();
  ws.box_shadow = browser::TextRef();
  ws.outline_style = BorderStyle::None;
  for (browser::CornerRadius& r : ws.radius) r = browser::CornerRadius();
  for (int i = 0; i < 4; ++i) {
    ws.border_px100[i] = 0;
    ws.border_style[i] = BorderStyle::None;
    ws.padding[i] = Length::px(0);
  }
  ws.height = Length();
  ws.min_height = Length();
  ws.max_height = Length::of(Length::None);
  ws.sizing = browser::BoxSizing::ContentBox;
  const bool inline_table = table.kind == BoxKind::InlineBlock;
  if (ws.width.is_auto()) {
    ws.width = Length::of(Length::FitContent);
  } else if (table.style.sizing == browser::BoxSizing::ContentBox && ws.width.is_px() && !ws.width.has_percent()) {
    ws.width.px100 += table.style.border_px100[1] + table.style.border_px100[3] + table.style.padding[1].px100 + table.style.padding[3].px100;
  }
  const BoxId wrapper_id = tree.create(inline_table ? BoxKind::InlineBlock : BoxKind::Anonymous, 0, ws);
  if (wrapper_id == kNoBox) return false;
  Box& wrapper = tree.box(wrapper_id);
  wrapper.out_of_flow = table.out_of_flow;
  wrapper.floated = table.floated;
  wrapper.establishes_bfc = true;
  wrapper.inline_content = false;
  if (!wrapper.style.inline_box) wrapper.style.display = Display::Block;
  // the table keeps what is drawn on it; where it is and how it is shown are the wrapper's
  Style& ts = table.style;
  const bool had_width = !table.style.width.is_auto();
  for (int i = 0; i < 4; ++i) {
    ts.margin[i] = Length::px(0);
    ts.inset[i] = Length();
  }
  ts.position = browser::Position::Static;
  ts.float_side = Float::None;
  ts.clear = browser::Clear::None;
  ts.z_index_auto = true;
  ts.z_index = 0;
  ts.opacity_x1000 = 1000;
  ts.transform_value = ts.translate = ts.rotate = ts.scale = browser::TextRef();
  ts.min_width = Length();
  ts.max_width = Length::of(Length::None);
  ts.width = had_width ? percent_length(100) : Length();
  ts.sizing = browser::BoxSizing::BorderBox;
  ts.display = Display::Block;
  ts.inline_box = false;
  table.kind = BoxKind::Block;
  table.out_of_flow = false;
  table.floated = false;
  // children of the wrapper: captions above, the table, captions below
  List<BoxId> above(tree.allocator()), below(tree.allocator()), keep(tree.allocator());
  for (uint64_t i = 0; i < table.children.count(); ++i) {
    const BoxId cid = table.children[i];
    const Box& c = tree.box(cid);
    if (part_of(c) != Part::Caption) {
      if (!keep.push_back(cid)) return false;
    } else if (c.style.caption_bottom || ts.caption_bottom) {
      if (!below.push_back(cid)) return false;
    } else {
      if (!above.push_back(cid)) return false;
    }
  }
  table.children.clear();
  for (uint64_t i = 0; i < keep.count(); ++i)
    if (!table.children.push_back(keep[i])) return false;
  for (uint64_t i = 0; i < above.count(); ++i)
    if (!tree.append(wrapper_id, above[i])) return false;
  if (!tree.append(wrapper_id, table_id)) return false;
  for (uint64_t i = 0; i < below.count(); ++i)
    if (!tree.append(wrapper_id, below[i])) return false;
  Box& p = tree.box(parent_id);
  p.children[at] = wrapper_id;
  tree.box(wrapper_id).parent = parent_id;
  return true;
}

}  // namespace

bool finish_table(BoxTree& tree, BoxId table_id) {
  if (tree.box(table_id).style.layout != Layout::Table) return true;
  bool ok = true;
  if (tree.box(table_id).style.border_collapse) ok &= resolve_collapsed_borders(tree, table_id);
  ok &= wrap_captions(tree, table_id);
  return ok;
}

}  // namespace layout
