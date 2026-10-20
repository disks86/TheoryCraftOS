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

#include "Browser/PageLayout.hpp"

namespace browser {

namespace {

class Builder {
 public:
  Builder(PageLayout& layout, List<Piece>& pieces, List<uint32_t>& lines, uint32_t columns) : pieces_(pieces), lines_(lines), columns_(columns) { (void)layout; }

  bool failed() const { return failed_; }

  void end_line() {
    open_ = false;
    space_ = false;
  }

  // A line to add pieces to, starting at `indent` if it is a new one.
  void ensure_line(uint32_t indent) {
    if (open_) return;
    failed_ |= !lines_.push_back(static_cast<uint32_t>(pieces_.count()));
    column_ = indent < columns_ - 1 ? indent : columns_ - 1;
    open_ = true;
    has_ = false;
  }

  // A blank line, unless the page starts here or the last line is blank already.
  void blank_line() {
    end_line();
    if (lines_.count() == 0) return;
    const uint64_t last = lines_.count() - 1;
    const uint64_t end = last + 1 < lines_.count() ? lines_[last + 1] : pieces_.count();
    if (lines_[last] == end) return;   // (no pieces: it is blank)
    failed_ |= !lines_.push_back(static_cast<uint32_t>(pieces_.count()));
  }

  bool has_content() const { return open_ && has_; }

  // Adds [from, from + length) of a run at the current column (after a space if one is pending), breaking the text over lines if it is longer
  // than the room there is.
  void place(uint32_t run, uint32_t from, uint32_t length, uint32_t indent) {
    while (length > 0 && !failed_) {
      ensure_line(indent);
      const uint32_t gap = has_ && space_ ? 1 : 0;
      uint32_t room = columns_ > column_ + gap ? columns_ - column_ - gap : 0;
      if (room < length && has_) {   // does not fit after what is there: a line of its own
        end_line();
        continue;
      }
      if (room == 0) room = 1;
      const uint32_t part = length < room ? length : room;
      append(run, from, part, column_ + gap, gap);
      column_ += gap + part;
      has_ = true;
      space_ = false;
      from += part;
      length -= part;
      if (length > 0) end_line();
    }
  }

  void note_space() {
    if (has_) space_ = true;
  }

  void rule(uint32_t run) {
    end_line();
    ensure_line(0);
    append(run, 0, 0, 0, 0);
    has_ = true;
    end_line();
  }

 private:
  void append(uint32_t run, uint32_t from, uint32_t length, uint32_t column, uint32_t gap) {
    if (pieces_.count() > 0 && lines_.count() > 0 && lines_[lines_.count() - 1] < pieces_.count()) {   // the line has a piece: perhaps this continues it
      Piece& last = pieces_[pieces_.count() - 1];
      if (last.run == run && gap == 1 && last.from + last.length + 1 == from && last.column + last.length + 1 == column) {
        last.length += 1 + length;
        return;
      }
      if (last.run == run && gap == 0 && last.from + last.length == from && last.column + last.length == column) {
        last.length += length;
        return;
      }
    }
    Piece piece;
    piece.run = run;
    piece.from = from;
    piece.length = length;
    piece.column = column;
    failed_ |= !pieces_.push_back(piece);
  }

  List<Piece>& pieces_;
  List<uint32_t>& lines_;
  uint32_t columns_;
  uint32_t column_ = 0;
  bool open_ = false, has_ = false, space_ = false, failed_ = false;
};

}  // namespace

bool PageLayout::build(const HtmlDocument& doc, uint32_t columns) {
  clear();
  columns_ = columns < 8 ? 8 : columns;
  Builder b(*this, pieces_, lines_, columns_);
  bool any = false;
  for (uint64_t r = 0; r < doc.run_count() && !b.failed() && lines_.count() < kMaxLines; ++r) {
    const Run& run = doc.run(r);
    const uint32_t index = static_cast<uint32_t>(r);
    const uint32_t indent = run.indent;
    const uint8_t breaks = any ? run.breaks : 0;
    if (breaks >= 1) b.end_line();
    if (breaks >= 2) b.blank_line();
    if (run.flags & kRunRule) {
      b.rule(index);
      any = true;
      continue;
    }
    const StringView text = doc.run_text(r);
    if (text.empty()) continue;
    any = true;
    if (run.flags & kRunPre) {
      uint64_t i = 0;
      bool first = true;
      while (i <= text.size() && !b.failed() && lines_.count() < kMaxLines) {
        uint64_t end = text.find('\n', i);
        if (end == StringView::npos) end = text.size();
        if (!first) b.end_line();
        b.ensure_line(indent);
        if (end > i) b.place(index, static_cast<uint32_t>(i), static_cast<uint32_t>(end - i), indent);
        first = false;
        i = end + 1;
      }
      continue;
    }
    uint64_t i = 0;
    while (i < text.size() && !b.failed() && lines_.count() < kMaxLines) {
      if (text[i] == ' ') {
        b.note_space();
        ++i;
        continue;
      }
      uint64_t end = i;
      while (end < text.size() && text[end] != ' ') ++end;
      b.place(index, static_cast<uint32_t>(i), static_cast<uint32_t>(end - i), indent);
      i = end;
    }
  }
  if (b.failed()) {
    clear();
    return false;
  }
  return true;
}

int64_t PageLayout::piece_at(uint32_t line, uint32_t column) const {
  if (line >= lines_.count()) return -1;
  for (uint32_t p = first_piece(line); p < end_piece(line); ++p) {
    const Piece& piece = pieces_[p];
    if (column >= piece.column && column < piece.column + piece.length) return p;
  }
  return -1;
}

int32_t PageLayout::link_at(const HtmlDocument& doc, uint32_t line, uint32_t column) const {
  const int64_t p = piece_at(line, column);
  return p < 0 ? -1 : doc.run(pieces_[static_cast<uint64_t>(p)].run).link;
}

}  // namespace browser
