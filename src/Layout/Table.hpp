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

// What the table code shares: the grid a table's cells make (Table.cpp), and the box tree fix-up that gives tables the structure CSS 2.1
// chapter 17 expects (TableFixup.cpp: anonymous boxes, presentational attributes, collapsed borders, the caption wrapper). Not for use outside src/Layout.
#pragma once
#include "Layout/Box.hpp"

namespace dom {
struct Node;
}

namespace layout {

// A cell placed in the grid.
struct TableCell {
  BoxId box = kNoBox;
  uint32_t row = 0, column = 0;      // the first row and column it covers
  uint32_t rowspan = 1, colspan = 1; // how many it covers (rowspan cut to the rows there are)
};

// The rows and cells of a table in the order they are laid out (header groups first, footer groups last).
struct TableGrid {
  explicit TableGrid(allocators::IAllocator& allocator) : rows(allocator), row_groups(allocator), group_ends(allocator), cells(allocator) {}
  List<BoxId> rows;           // the row boxes
  List<BoxId> row_groups;     // per row: its row group box (kNoBox: a row directly in the table)
  List<uint32_t> group_ends;  // per row: the index after the last row of its group (what rowspan 0 reaches)
  List<TableCell> cells;
  uint32_t columns = 0;
  bool failed = false;
};

// Places the cells of `table` (whose rows are its children, or the children of its row groups) in a grid. False if memory ran out.
bool build_table_grid(BoxTree& tree, BoxId table, TableGrid* grid);

// --- TableFixup.cpp
// The style the elements of a table have before and beside the page's style sheet (a browser's own sheet for <table>, <tr>, <td> ... and the
// attributes cellspacing, cellpadding, border, width, height, align, valign, nowrap): changes `style`, which derive_style made, for `node`.
void apply_table_presentation(const dom::Node* node, const browser::Style& parent, const browser::StyleSheet& sheet, browser::Style* style);
// The colspan and rowspan of a cell and the role of a row group, from the attributes.
void read_table_attributes(const dom::Node* node, Box* box);
// Wraps table parts that sit in the wrong place in the anonymous boxes CSS 2.1 section 17.2.1 asks for. The anonymous cells it makes hold content
// that still has to be arranged (blocks and inline content): they are added to `cells`. False if memory ran out.
bool fixup_table_parts(BoxTree& tree, BoxId id, List<BoxId>* cells);
// For a table whose children are all made: resolves its collapsed borders and puts its captions in a wrapper box. False if memory ran out.
bool finish_table(BoxTree& tree, BoxId table);

}  // namespace layout
