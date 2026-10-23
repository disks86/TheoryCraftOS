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

// Layout of tables (CSS 2.1 chapter 17). The box tree has been put in order by TableFixup.cpp: a table holds rows (directly or in row groups), a row
// holds cells, and a table with captions sits in a wrapper box that holds the captions (so a caption is laid out as a block and is not here).
//
// Done: the automatic algorithm (the min-content and max-content width of each cell, percentage and fixed column widths, cells that span columns,
// the extra width shared out the way browsers do), table-layout: fixed, the separate border model with border-spacing and the collapsed one (the
// conflicts are resolved when the box tree is made, see TableFixup.cpp: here the spacing is simply 0), row heights from the cells (rowspan,
// height, vertical-align top / middle / bottom / baseline), a table height that is bigger than its rows, the width and baseline of the table.
// Not done: col / colgroup (they make no box), visibility: collapse on rows, percentage heights on cells and rows, rows that overflow a row group.
#include "Layout/LayoutInternal.hpp"
#include "Layout/Table.hpp"

namespace layout {

using browser::BoxSizing;
using browser::Length;
using browser::Layout;

namespace {

constexpr uint32_t kMaxColumns = 1000;

// A width that is only a percentage (50%, not calc(50% - 4px)): the percentage in hundredths.
bool pure_percent(const Length& l, int32_t* hundredths) {
  if (!l.has_percent() || l.expr != 0 || l.abs100 != 0 || l.percent100 <= 0) return false;
  *hundredths = l.percent100;
  return true;
}

template <typename T>
bool fill(List<T>* list, uint64_t count, T value) {
  list->clear();
  for (uint64_t i = 0; i < count; ++i)
    if (!list->push_back(value)) return false;
  return true;
}

// Adds `extra` to target[first .. first+count) in proportion to weights[first ..] (equally if they are all 0). The last one takes what rounding left.
void spread(List<LayoutUnit>* target, const List<LayoutUnit>& weights, uint32_t first, uint32_t count, LayoutUnit extra) {
  if (count == 0 || extra <= LayoutUnit()) return;
  LayoutUnit total;
  for (uint32_t i = 0; i < count; ++i) total += weights[first + i];
  LayoutUnit given;
  for (uint32_t i = 0; i < count; ++i) {
    LayoutUnit share = extra - given;
    if (i + 1 < count) share = total.is_zero() ? extra / static_cast<int32_t>(count) : extra.scaled(weights[first + i].raw(), total.raw());
    (*target)[first + i] += share;
    given += share;
  }
}

// Moves what is inside a laid out cell down (vertical-align): the lines and everything that sits in them, or the child boxes.
void shift_content(BoxTree& tree, BoxId id, LayoutUnit dy) {
  Box& b = tree.box(id);
  if (b.inline_content) {
    for (uint64_t i = 0; i < b.lines.count(); ++i) b.lines[i].y += dy;
    for (uint64_t i = 0; i < b.fragments.count(); ++i) b.fragments[i].y += dy;
    List<BoxId> work(tree.allocator());
    for (uint64_t i = 0; i < b.children.count(); ++i) work.push_back(b.children[i]);
    while (work.count() > 0) {   // (atomic inlines have their place in the container, however deep the inline boxes around them go)
      const BoxId cid = work[work.count() - 1];
      work.remove_at(work.count() - 1);
      Box& c = tree.box(cid);
      if (c.kind == BoxKind::Inline) {
        for (uint64_t i = 0; i < c.children.count() && work.count() < 100000; ++i) work.push_back(c.children[i]);
      } else if (c.kind == BoxKind::InlineBlock || c.kind == BoxKind::Replaced || c.floated || c.out_of_flow) {
        c.y += dy;
      }
    }
  } else {
    for (uint64_t i = 0; i < b.children.count(); ++i) tree.box(b.children[i]).y += dy;
  }
}

// The baseline of the first line in a box, from its top border edge. False when there is no line.
bool first_baseline(const BoxTree& tree, BoxId id, LayoutUnit* out, int depth) {
  const Box& b = tree.box(id);
  if (b.style.layout == Layout::Table && b.has_baseline) {
    *out = b.baseline;
    return true;
  }
  if (b.inline_content) {
    if (b.lines.count() == 0) return false;
    *out = b.content_top() + b.lines[0].y + b.lines[0].baseline;
    return true;
  }
  if (depth >= 24) return false;
  for (uint64_t i = 0; i < b.children.count(); ++i) {
    const Box& c = tree.box(b.children[i]);
    if (c.kind == BoxKind::Marker || c.out_of_flow || c.floated || !c.is_block_level()) continue;
    LayoutUnit inner;
    if (first_baseline(tree, b.children[i], &inner, depth + 1)) {
      *out = c.y + inner;
      return true;
    }
  }
  return false;
}

}  // namespace

bool build_table_grid(BoxTree& tree, BoxId table, TableGrid* grid) {
  auto add_row = [&](BoxId row, BoxId group) {
    grid->failed |= !grid->rows.push_back(row) || !grid->row_groups.push_back(group) || !grid->group_ends.push_back(0);
  };
  const uint64_t child_count = tree.box(table).children.count();
  for (int pass = 0; pass < 3; ++pass) {   // header groups, then everything else in the order it is written, then footer groups
    for (uint64_t i = 0; i < child_count && !grid->failed; ++i) {
      const BoxId cid = tree.box(table).children[i];
      const Box& c = tree.box(cid);
      if (c.out_of_flow) continue;
      if (c.style.layout == Layout::TableRow) {
        if (pass == 1) add_row(cid, kNoBox);
      } else if (c.style.layout == Layout::TableRowGroup) {
        const int want = c.group_role == 1 ? 0 : c.group_role == 2 ? 2 : 1;
        if (want != pass) continue;
        const uint64_t first = grid->rows.count();
        for (uint64_t k = 0; k < c.children.count(); ++k) {
          const Box& r = tree.box(c.children[k]);
          if (r.style.layout == Layout::TableRow && !r.out_of_flow) add_row(c.children[k], cid);
        }
        for (uint64_t k = first; k < grid->rows.count(); ++k) grid->group_ends[k] = static_cast<uint32_t>(grid->rows.count());
      }
    }
  }
  const uint32_t row_count = static_cast<uint32_t>(grid->rows.count());
  for (uint32_t r = 0; r < row_count; ++r)
    if (grid->row_groups[r] == kNoBox) grid->group_ends[r] = row_count;
  List<uint32_t> busy(tree.allocator());   // per column: the row up to which an earlier cell (rowspan) holds it
  for (uint32_t r = 0; r < row_count && !grid->failed; ++r) {
    const Box& row = tree.box(grid->rows[r]);
    uint32_t col = 0;
    for (uint64_t k = 0; k < row.children.count() && !grid->failed; ++k) {
      const BoxId cid = row.children[k];
      const Box& c = tree.box(cid);
      if (c.style.layout != Layout::TableCell || c.out_of_flow) continue;
      while (col < busy.count() && busy[col] > r) ++col;
      if (col >= kMaxColumns) continue;
      uint32_t cs = c.colspan < 1 ? 1 : c.colspan;
      if (cs > kMaxColumns - col) cs = kMaxColumns - col;
      uint32_t rs = c.rowspan == 0 ? grid->group_ends[r] - r : c.rowspan;
      if (rs > grid->group_ends[r] - r) rs = grid->group_ends[r] - r;
      if (rs < 1) rs = 1;
      while (busy.count() < col + cs) grid->failed |= !busy.push_back(0);
      if (grid->failed) break;
      TableCell cell;
      cell.box = cid;
      cell.row = r;
      cell.column = col;
      cell.rowspan = rs;
      cell.colspan = cs;
      grid->failed |= !grid->cells.push_back(cell);
      for (uint32_t x = col; x < col + cs; ++x) busy[x] = r + rs;
      col += cs;
    }
  }
  grid->columns = static_cast<uint32_t>(busy.count());
  return !grid->failed;
}

// Works out the columns of one table and places everything in it.
struct TableLayouter {
  TableLayouter(LayoutContext& context, BoxId table) : cx(context), tree(context.tree_), id(table), grid(context.tree_.allocator()),
        cell_min(context.tree_.allocator()), cell_pref(context.tree_.allocator()), cell_pct(context.tree_.allocator()), cell_fixed(context.tree_.allocator()),
        col_min(context.tree_.allocator()), col_pref(context.tree_.allocator()), col_pct(context.tree_.allocator()), col_fixed(context.tree_.allocator()),
        col_width(context.tree_.allocator()) {}

  LayoutContext& cx;
  BoxTree& tree;
  BoxId id;
  TableGrid grid;
  LayoutUnit spacing;                       // between cells, and between the cells and the edge of the table
  List<LayoutUnit> cell_min, cell_pref;     // per cell: the narrowest and the preferred border box width
  List<int32_t> cell_pct;                   // per cell: the percentage (hundredths) it asks for
  List<uint8_t> cell_fixed;                 // per cell: it has a width in pixels
  List<LayoutUnit> col_min, col_pref;       // per column
  List<int32_t> col_pct;
  List<uint8_t> col_fixed;
  List<LayoutUnit> col_width;

  bool init() {
    if (!build_table_grid(tree, id, &grid)) return false;
    const Box& t = tree.box(id);
    const bool spaced = !t.style.border_collapse && t.style.border_spacing.is_px() && !t.style.border_spacing.has_percent();
    spacing = spaced ? max(LayoutUnit::from_px100(t.style.border_spacing.px100), LayoutUnit()) : LayoutUnit();
    return true;
  }

  // The widths each cell asks for, and from them those of the columns.
  bool measure() {
    const uint32_t columns = grid.columns;
    if (!fill(&cell_min, grid.cells.count(), LayoutUnit()) || !fill(&cell_pref, grid.cells.count(), LayoutUnit()) || !fill<int32_t>(&cell_pct, grid.cells.count(), 0) ||
        !fill<uint8_t>(&cell_fixed, grid.cells.count(), 0) || !fill(&col_min, columns, LayoutUnit()) || !fill(&col_pref, columns, LayoutUnit()) ||
        !fill<int32_t>(&col_pct, columns, 0) || !fill<uint8_t>(&col_fixed, columns, 0))
      return false;
    uint32_t max_span = 1;
    for (uint64_t i = 0; i < grid.cells.count(); ++i) {
      const TableCell& cell = grid.cells[i];
      Box& b = tree.box(cell.box);
      LayoutUnit mn, mx;
      cx.intrinsic_widths(cell.box, &mn, &mx);
      const LayoutUnit bp = b.border.horizontal() + b.padding.horizontal();
      cell_min[i] = mn + bp;
      cell_pref[i] = mx + bp;
      int32_t pct = 0;
      if (b.style.width.is_px() && !b.style.width.has_percent()) {
        const LayoutUnit w = LayoutUnit::from_px100(b.style.width.px100);
        const LayoutUnit outer = b.style.sizing == BoxSizing::BorderBox ? max(w, bp) : w + bp;
        cell_pref[i] = max(cell_min[i], outer);
        cell_fixed[i] = 1;
      } else if (pure_percent(b.style.width, &pct)) {
        cell_pct[i] = pct;
      }
      if (cell.colspan > max_span) max_span = cell.colspan;
    }
    // cells of one column first, then the ones that span 2, 3 ... (they only add what the columns they span do not already give)
    uint32_t span = 1;
    while (span <= max_span) {
      for (uint64_t i = 0; i < grid.cells.count(); ++i) {
        const TableCell& cell = grid.cells[i];
        if (cell.colspan != span) continue;
        const uint32_t c0 = cell.column;
        if (span == 1) {
          col_min[c0] = max(col_min[c0], cell_min[i]);
          col_pref[c0] = max(col_pref[c0], cell_pref[i]);
          col_pct[c0] = cell_pct[i] > col_pct[c0] ? cell_pct[i] : col_pct[c0];
          if (cell_fixed[i]) col_fixed[c0] = 1;
          continue;
        }
        const LayoutUnit inner = spacing * static_cast<int32_t>(span - 1);
        LayoutUnit have_min, have_pref;
        for (uint32_t c = c0; c < c0 + span; ++c) {
          have_min += col_min[c];
          have_pref += col_pref[c];
        }
        if (cell_min[i] > have_min + inner) spread(&col_min, col_pref, c0, span, cell_min[i] - have_min - inner);
        if (cell_pref[i] > have_pref + inner) spread(&col_pref, col_pref, c0, span, cell_pref[i] - have_pref - inner);
        if (cell_pct[i] > 0)
          for (uint32_t c = c0; c < c0 + span; ++c) {
            const int32_t share = cell_pct[i] / static_cast<int32_t>(span);
            if (share > col_pct[c]) col_pct[c] = share;
          }
      }
      uint32_t next = max_span + 1;   // the next span some cell has
      for (uint64_t i = 0; i < grid.cells.count(); ++i)
        if (grid.cells[i].colspan > span && grid.cells[i].colspan < next) next = grid.cells[i].colspan;
      span = next;
    }
    for (uint32_t c = 0; c < columns; ++c) col_pref[c] = max(col_pref[c], col_min[c]);
    return true;
  }

  int32_t pct_total() const {
    int64_t total = 0;
    for (uint64_t c = 0; c < col_pct.count(); ++c) total += col_pct[c];
    return total > 100000 ? 100000 : static_cast<int32_t>(total);
  }

  // The widest the columns want to be (leaving out the spacing): every column at its preferred width, a column with a percentage at least wide enough
  // for it to be that part of the whole.
  LayoutUnit columns_max() const {
    LayoutUnit plain, wanted;
    int64_t total_pct = 0;
    for (uint64_t c = 0; c < col_pref.count(); ++c) {
      if (col_pct[c] > 0) {
        total_pct += col_pct[c];
        wanted = max(wanted, col_pref[c].scaled(10000, col_pct[c]));
      } else {
        plain += col_pref[c];
      }
    }
    if (total_pct >= 10000) return max(wanted, plain);
    return max(wanted, plain.scaled(10000, static_cast<int32_t>(10000 - total_pct)));
  }

  LayoutUnit columns_min() const {
    LayoutUnit sum;
    for (uint64_t c = 0; c < col_min.count(); ++c) sum += col_min[c];
    return sum;
  }

  LayoutUnit spacing_total() const { return spacing * static_cast<int32_t>(grid.columns + 1); }

  // The widths of the columns for `avail` (without the spacing): automatic layout.
  bool auto_widths(LayoutUnit avail) {
    const uint32_t columns = grid.columns;
    List<LayoutUnit> target(tree.allocator());
    if (!fill(&target, columns, LayoutUnit()) || !fill(&col_width, columns, LayoutUnit())) return false;
    int64_t raw_pct = 0;
    for (uint32_t c = 0; c < columns; ++c) raw_pct += col_pct[c];
    LayoutUnit sum_target, sum_min;
    for (uint32_t c = 0; c < columns; ++c) {
      if (col_pct[c] > 0) {
        const int32_t share = raw_pct > 10000 ? static_cast<int32_t>(static_cast<int64_t>(col_pct[c]) * 10000 / raw_pct) : col_pct[c];
        target[c] = max(col_min[c], avail.scaled(share, 10000));
      } else {
        target[c] = col_pref[c];
      }
      sum_target += target[c];
      sum_min += col_min[c];
    }
    if (avail <= sum_min) {
      for (uint32_t c = 0; c < columns; ++c) col_width[c] = col_min[c];
    } else if (avail < sum_target) {   // between the narrowest and the widest: each column takes its share of the room
      const int32_t num = (avail - sum_min).raw(), den = (sum_target - sum_min).raw();
      LayoutUnit used;
      for (uint32_t c = 0; c < columns; ++c) {
        col_width[c] = col_min[c] + (target[c] - col_min[c]).scaled(num, den);
        used += col_width[c];
      }
      for (uint32_t c = columns; c > 0 && used != avail; --c) {   // (rounding: the last column that can take it gets the rest)
        const LayoutUnit fix = max(avail - used, col_min[c - 1] - col_width[c - 1]);
        col_width[c - 1] += fix;
        used += fix;
      }
    } else {   // wider than the columns want: the extra goes to the columns without a width, else to those with one in pixels, else to the percentages
      for (uint32_t c = 0; c < columns; ++c) col_width[c] = target[c];
      const LayoutUnit extra = avail - sum_target;
      uint32_t first_class = 3;
      for (uint32_t c = 0; c < columns; ++c) {
        const uint32_t k = col_pct[c] > 0 ? 2 : col_fixed[c] ? 1 : 0;
        if (k < first_class) first_class = k;
      }
      List<LayoutUnit> weights(tree.allocator());
      if (!fill(&weights, columns, LayoutUnit())) return false;
      uint32_t members = 0;
      LayoutUnit weight_sum;
      for (uint32_t c = 0; c < columns; ++c) {
        const uint32_t k = col_pct[c] > 0 ? 2 : col_fixed[c] ? 1 : 0;
        if (k == first_class) {
          weights[c] = target[c];
          weight_sum += target[c];
          ++members;
        }
      }
      LayoutUnit given;
      uint32_t seen = 0;
      for (uint32_t c = 0; c < columns && members > 0; ++c) {
        const uint32_t k = col_pct[c] > 0 ? 2 : col_fixed[c] ? 1 : 0;
        if (k != first_class) continue;
        ++seen;
        LayoutUnit share = extra - given;
        if (seen < members) share = weight_sum.is_zero() ? extra / static_cast<int32_t>(members) : extra.scaled(weights[c].raw(), weight_sum.raw());
        col_width[c] += share;
        given += share;
      }
    }
    return true;
  }

  // table-layout: fixed: the widths of the first row's cells say it; columns without one share what is left.
  bool fixed_widths(LayoutUnit avail) {
    const uint32_t columns = grid.columns;
    if (!fill(&col_width, columns, LayoutUnit())) return false;
    List<uint8_t> known(tree.allocator());
    if (!fill<uint8_t>(&known, columns, 0)) return false;
    for (uint64_t i = 0; i < grid.cells.count(); ++i) {
      const TableCell& cell = grid.cells[i];
      if (cell.row != 0) continue;
      const Box& b = tree.box(cell.box);
      LayoutUnit width;
      bool have = false;
      int32_t pct = 0;
      const LayoutUnit bp = b.border.horizontal() + b.padding.horizontal();
      if (b.style.width.is_px() && !b.style.width.has_percent()) {
        const LayoutUnit w = LayoutUnit::from_px100(b.style.width.px100);
        width = b.style.sizing == BoxSizing::BorderBox ? max(w, bp) : w + bp;
        have = true;
      } else if (pure_percent(b.style.width, &pct)) {
        width = max(avail.scaled(pct, 10000), bp);
        have = true;
      }
      if (!have) continue;
      const LayoutUnit each = max(width - spacing * static_cast<int32_t>(cell.colspan - 1), LayoutUnit()) / static_cast<int32_t>(cell.colspan);
      for (uint32_t c = cell.column; c < cell.column + cell.colspan; ++c) {
        col_width[c] = each;
        known[c] = 1;
      }
    }
    LayoutUnit used;
    uint32_t unknown = 0;
    for (uint32_t c = 0; c < columns; ++c) {
      if (known[c]) used += col_width[c];
      else ++unknown;
    }
    if (unknown > 0) {
      const LayoutUnit left = max(avail - used, LayoutUnit());
      LayoutUnit given;
      uint32_t seen = 0;
      for (uint32_t c = 0; c < columns; ++c) {
        if (known[c]) continue;
        ++seen;
        const LayoutUnit share = seen == unknown ? left - given : left / static_cast<int32_t>(unknown);
        col_width[c] = share;
        given += share;
      }
    } else if (used < avail && columns > 0) {   // all of them given and the table wider: they grow in proportion
      List<LayoutUnit> weights(tree.allocator());
      if (!fill(&weights, columns, LayoutUnit())) return false;
      for (uint32_t c = 0; c < columns; ++c) weights[c] = col_width[c];
      spread(&col_width, weights, 0, columns, avail - used);
    }
    return true;
  }

  ChildrenResult run(LayoutUnit content_width, bool height_known, LayoutUnit height);
};

ChildrenResult LayoutContext::layout_table(BoxId id, LayoutUnit content_width, bool height_known, LayoutUnit height) {
  TableLayouter layouter(*this, id);
  return layouter.run(content_width, height_known, height);
}

void LayoutContext::table_intrinsic_widths(BoxId id, LayoutUnit* min_content, LayoutUnit* max_content) {
  TableLayouter layouter(*this, id);
  if (!layouter.init() || !layouter.measure() || layouter.grid.cells.count() == 0) {
    failed_ |= layouter.grid.failed;
    *min_content = *max_content = LayoutUnit();
    return;
  }
  *min_content = layouter.columns_min() + layouter.spacing_total();
  *max_content = layouter.columns_max() + layouter.spacing_total();
}

ChildrenResult TableLayouter::run(LayoutUnit content_width, bool height_known, LayoutUnit height) {
  ChildrenResult result;
  Box& table = tree.box(id);
  for (uint64_t i = 0; i < table.children.count(); ++i) {   // (a positioned box in a table is placed against its containing block like any other)
    const Box& c = tree.box(table.children[i]);
    if (c.out_of_flow) cx.register_absolute(table.children[i], id, table.content_left(), table.content_top());
  }
  if (!init() || !measure()) {
    cx.failed_ = true;
    return result;
  }
  const uint32_t columns = grid.columns, row_count = static_cast<uint32_t>(grid.rows.count());
  if (row_count == 0 || columns == 0) return result;
  result.empty = false;

  // --- the columns
  const LayoutUnit edge = spacing_total();
  const LayoutUnit avail = max(content_width - edge, LayoutUnit());
  LayoutUnit specified;
  const bool width_given = cx.resolve_in(table.style.width, table, false, &specified);
  const bool ok = (table.style.table_fixed && width_given) ? fixed_widths(avail) : auto_widths(avail);
  if (!ok) {
    cx.failed_ = true;
    return result;
  }
  LayoutUnit columns_sum;
  for (uint32_t c = 0; c < columns; ++c) columns_sum += col_width[c];
  const LayoutUnit grid_width = columns_sum + edge;
  if (grid_width > content_width) table.width = grid_width + table.border.horizontal() + table.padding.horizontal();   // (a table is never narrower than its columns)
  List<LayoutUnit> col_x(tree.allocator());
  if (!fill(&col_x, columns, LayoutUnit())) {
    cx.failed_ = true;
    return result;
  }
  LayoutUnit at;
  for (uint32_t c = 0; c < columns; ++c) {
    col_x[c] = at;
    at += col_width[c] + spacing;
  }

  // --- the cells, at the width of the columns they cover
  List<LayoutUnit> natural(tree.allocator()), base(tree.allocator());
  List<LayoutUnit> row_h(tree.allocator()), row_base(tree.allocator()), row_below(tree.allocator());
  if (!fill(&natural, grid.cells.count(), LayoutUnit()) || !fill(&base, grid.cells.count(), LayoutUnit()) || !fill(&row_h, row_count, LayoutUnit()) ||
      !fill(&row_base, row_count, LayoutUnit()) || !fill(&row_below, row_count, LayoutUnit())) {
    cx.failed_ = true;
    return result;
  }
  List<uint8_t> row_has_base(tree.allocator());
  if (!fill<uint8_t>(&row_has_base, row_count, 0)) {
    cx.failed_ = true;
    return result;
  }
  auto span_width = [&](const TableCell& cell) {
    LayoutUnit w = spacing * static_cast<int32_t>(cell.colspan - 1);
    for (uint32_t c = cell.column; c < cell.column + cell.colspan; ++c) w += col_width[c];
    return w;
  };
  for (uint32_t r = 0; r < row_count; ++r) {   // a row's own height says how low it may be
    LayoutUnit rh;
    const Box& row = tree.box(grid.rows[r]);
    if (cx.resolve_with(row.style.height, row.style, LayoutUnit(), false, &rh)) row_h[r] = max(rh, LayoutUnit());
  }
  for (uint64_t i = 0; i < grid.cells.count() && !cx.failed_; ++i) {
    const TableCell& cell = grid.cells[i];
    Box& b = tree.box(cell.box);
    const LayoutUnit width = span_width(cell);
    cx.resolve_edges(b, grid_width, true);
    const LayoutUnit bp_h = b.border.horizontal() + b.padding.horizontal();
    LayoutContext::WidthRequest request;
    request.forced = true;
    request.content_width = max(width - bp_h, LayoutUnit());
    cx.layout_block(cell.box, grid_width, request);
    b.margin = Edges();
    b.width = width;
    natural[i] = b.height;
    LayoutUnit bl;
    base[i] = first_baseline(tree, cell.box, &bl, 0) ? bl : b.border.top + b.padding.top;
    if (cell.rowspan == 1 && b.style.vertical_align == 0) {
      row_base[cell.row] = max(row_base[cell.row], base[i]);
      row_below[cell.row] = max(row_below[cell.row], natural[i] - base[i]);
      row_has_base[cell.row] = 1;
    }
  }
  if (cx.failed_) return result;

  // --- the rows: as high as their cells (a cell that spans rows asks for what the rows it covers do not give)
  for (uint32_t r = 0; r < row_count; ++r)
    if (row_has_base[r]) row_h[r] = max(row_h[r], row_base[r] + row_below[r]);
  for (uint64_t i = 0; i < grid.cells.count(); ++i) {
    const TableCell& cell = grid.cells[i];
    if (cell.rowspan != 1) continue;
    const Box& b = tree.box(cell.box);
    LayoutUnit wanted = natural[i], spec;
    if (cx.resolve_with(b.style.height, b.style, LayoutUnit(), false, &spec)) {
      const LayoutUnit bp_v = b.border.vertical() + b.padding.vertical();
      wanted = max(wanted, b.style.sizing == BoxSizing::BorderBox ? max(spec, bp_v) : spec + bp_v);
    }
    row_h[cell.row] = max(row_h[cell.row], wanted);
  }
  uint32_t max_rows = 1;
  for (uint64_t i = 0; i < grid.cells.count(); ++i)
    if (grid.cells[i].rowspan > max_rows) max_rows = grid.cells[i].rowspan;
  for (uint32_t span = 2; span <= max_rows;) {
    for (uint64_t i = 0; i < grid.cells.count(); ++i) {
      const TableCell& cell = grid.cells[i];
      if (cell.rowspan != span) continue;
      const Box& b = tree.box(cell.box);
      LayoutUnit wanted = natural[i], spec;
      if (cx.resolve_with(b.style.height, b.style, LayoutUnit(), false, &spec)) {
        const LayoutUnit bp_v = b.border.vertical() + b.padding.vertical();
        wanted = max(wanted, b.style.sizing == BoxSizing::BorderBox ? max(spec, bp_v) : spec + bp_v);
      }
      LayoutUnit have = spacing * static_cast<int32_t>(span - 1);
      for (uint32_t r = cell.row; r < cell.row + span; ++r) have += row_h[r];
      if (wanted > have) spread(&row_h, row_h, cell.row, span, wanted - have);
    }
    uint32_t next = max_rows + 1;
    for (uint64_t i = 0; i < grid.cells.count(); ++i)
      if (grid.cells[i].rowspan > span && grid.cells[i].rowspan < next) next = grid.cells[i].rowspan;
    span = next;
  }
  LayoutUnit rows_sum;
  for (uint32_t r = 0; r < row_count; ++r) rows_sum += row_h[r];
  LayoutUnit grid_height = rows_sum + spacing * static_cast<int32_t>(row_count + 1);
  if (height_known && height > grid_height) {   // a table given more height than its rows need gives the rows the rest
    spread(&row_h, row_h, 0, row_count, height - grid_height);
    grid_height = height;
  }
  List<LayoutUnit> row_y(tree.allocator());
  if (!fill(&row_y, row_count, LayoutUnit())) {
    cx.failed_ = true;
    return result;
  }
  at = spacing;
  for (uint32_t r = 0; r < row_count; ++r) {
    row_y[r] = at;
    at += row_h[r] + spacing;
  }

  // --- the places of the row groups, rows and cells
  const LayoutUnit left = table.content_left() + spacing, top = table.content_top();
  const LayoutUnit inner_width = max(grid_width - spacing * 2, LayoutUnit());
  for (uint32_t r = 0; r < row_count; ++r) {
    Box& row = tree.box(grid.rows[r]);
    const BoxId group_id = grid.row_groups[r];
    row.width = inner_width;
    row.height = row_h[r];
    row.margin = row.border = row.padding = Edges();
    if (group_id == kNoBox) {
      row.x = left;
      row.y = top + row_y[r];
    } else {
      uint32_t first = r, last = r;
      while (first > 0 && grid.row_groups[first - 1] == group_id) --first;
      while (last + 1 < row_count && grid.row_groups[last + 1] == group_id) ++last;
      Box& group = tree.box(group_id);
      if (first == r) {
        group.x = left;
        group.y = top + row_y[first];
        group.width = inner_width;
        group.height = row_y[last] + row_h[last] - row_y[first];
        group.margin = group.border = group.padding = Edges();
      }
      row.x = LayoutUnit();
      row.y = row_y[r] - row_y[first];
    }
  }
  for (uint64_t i = 0; i < grid.cells.count(); ++i) {
    const TableCell& cell = grid.cells[i];
    Box& b = tree.box(cell.box);
    LayoutUnit full = spacing * static_cast<int32_t>(cell.rowspan - 1);
    for (uint32_t r = cell.row; r < cell.row + cell.rowspan; ++r) full += row_h[r];
    LayoutUnit delta;
    switch (b.style.vertical_align) {
      case 0: delta = cell.rowspan == 1 ? row_base[cell.row] - base[i] : LayoutUnit(); break;
      case 2: delta = max(full - natural[i], LayoutUnit()) / 2; break;
      case 3: delta = max(full - natural[i], LayoutUnit()); break;
      default: break;
    }
    if (!delta.is_zero()) shift_content(tree, cell.box, delta);
    b.x = col_x[cell.column];
    b.y = LayoutUnit();
    b.height = max(full, natural[i]);
    base[i] += delta;
  }
  // the table sits on the baseline of its first row
  {
    LayoutUnit first_row_y = row_y[0];
    for (uint64_t i = 0; i < grid.cells.count(); ++i) {
      if (grid.cells[i].row != 0) continue;
      table.baseline = top + first_row_y + base[i];
      table.has_baseline = true;
      break;
    }
  }
  result.height = grid_height;
  return result;
}

}  // namespace layout
