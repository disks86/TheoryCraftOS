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

// Fits a document's runs into lines of a given number of columns (the screen font has fixed-width characters, so a column is a character). Words
// wrap at the edge - a word longer than a line is broken - and lines that follow a run start at its indent; preformatted runs keep their line
// breaks (long lines wrap too). A line is a list of pieces, each part of one run, with the column it starts at; the layout also finds the link
// at a position.
#pragma once
#include "Browser/HtmlDocument.hpp"

namespace browser {

struct Piece {
  uint32_t run = 0;       // which run of the document
  uint32_t from = 0;      // where in the run's text the piece starts
  uint32_t length = 0;
  uint32_t column = 0;
};

class PageLayout {
 public:
  static constexpr uint32_t kMaxLines = 400000;

  explicit PageLayout(allocators::IAllocator& allocator) : pieces_(allocator), lines_(allocator) {}

  // Lays out `doc` in `columns` columns (at least 8). False if memory ran out (the layout is then empty).
  bool build(const HtmlDocument& doc, uint32_t columns);
  void clear() {
    pieces_.clear();
    lines_.clear();
  }

  uint32_t columns() const { return columns_; }
  uint32_t line_count() const { return static_cast<uint32_t>(lines_.count()); }
  // The pieces of a line are piece(first_piece(line)) up to, not including, piece(end_piece(line)).
  uint32_t first_piece(uint32_t line) const { return lines_[line]; }
  uint32_t end_piece(uint32_t line) const { return line + 1 < lines_.count() ? lines_[line + 1] : static_cast<uint32_t>(pieces_.count()); }
  const Piece& piece(uint32_t index) const { return pieces_[index]; }
  // The piece at a column of a line, or -1.
  int64_t piece_at(uint32_t line, uint32_t column) const;
  // The link shown at a column of a line, or -1.
  int32_t link_at(const HtmlDocument& doc, uint32_t line, uint32_t column) const;

 private:
  List<Piece> pieces_;
  List<uint32_t> lines_;   // for each line, the index of its first piece
  uint32_t columns_ = 0;
};

}  // namespace browser
