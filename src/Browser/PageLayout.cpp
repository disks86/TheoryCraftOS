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

  // Blank lines so that there are `count` of them (but none at the start of the page) between what came before and what comes next.
  void blank_lines(uint32_t count) {
    end_line();
    if (lines_.count() == 0) return;
    uint32_t have = 0;
    for (uint64_t l = lines_.count(); l > 0; --l) {
      const uint64_t end = l < lines_.count() ? lines_[l] : pieces_.count();
      if (lines_[l - 1] != end) break;   // it has pieces: not blank
      ++have;
    }
    for (; have < count && !failed_; ++have) failed_ |= !lines_.push_back(static_cast<uint32_t>(pieces_.count()));
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

  // An object of w x h pixels (shrunk to the line if it is wider; `pct` is its width in percent of the line, 0 for w): a word when it is one line high, else
  // lines of its own.
  void object(uint32_t run, uint32_t w, uint32_t h, uint32_t pct, uint32_t indent) {
    const uint32_t start = indent < columns_ - 1 ? indent : columns_ - 1;
    const uint64_t avail_px = uint64_t{columns_ - start} * 8;
    uint64_t pw = w, ph = h;
    if (pct) {
      pw = avail_px * pct / 100;
      if (pw == 0) pw = 1;
      ph = uint64_t{h} * pw / (w ? w : 1);
    }
    if (pw > avail_px) {
      ph = ph * avail_px / pw;
      pw = avail_px;
    }
    if (pw == 0) pw = 1;
    if (ph == 0) ph = 1;
    if (ph > 65000) ph = 65000;
    const uint32_t cols = static_cast<uint32_t>((pw + 7) / 8), rows = static_cast<uint32_t>((ph + 15) / 16);
    if (rows == 1) {
      ensure_line(indent);
      uint32_t gap = has_ && space_ ? 1 : 0;
      if (has_ && column_ + gap + cols > columns_) {
        end_line();
        ensure_line(indent);
        gap = 0;
      }
      push_object(run, 0, cols, column_ + gap, pw, ph);
      column_ += gap + cols;
      has_ = true;
      space_ = false;
      return;
    }
    end_line();
    for (uint32_t row = 0; row < rows && !failed_ && lines_.count() < PageLayout::kMaxLines; ++row) {
      ensure_line(indent);
      push_object(run, row, cols, column_, pw, ph);
      has_ = true;
      end_line();
    }
  }

  void rule(uint32_t run) {
    end_line();
    ensure_line(0);
    append(run, 0, 0, 0, 0);
    has_ = true;
    end_line();
  }

 private:
  void push_object(uint32_t run, uint32_t row, uint32_t cols, uint32_t column, uint64_t pw, uint64_t ph) {
    Piece piece;
    piece.run = run;
    piece.from = row;
    piece.length = cols;
    piece.column = column;
    piece.px_w = static_cast<uint16_t>(pw > 65535 ? 65535 : pw);
    piece.px_h = static_cast<uint16_t>(ph > 65535 ? 65535 : ph);
    failed_ |= !pieces_.push_back(piece);
  }

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
    if (breaks >= 2) b.blank_lines(breaks - 1u);
    if (run.flags & kRunRule) {
      b.rule(index);
      any = true;
      continue;
    }
    if (run.object) {
      any = true;
      b.object(index, run.object_w, run.object_h, run.object_pct, indent);
      continue;
    }
    const StringView text = doc.run_text(r);
    if (text.empty()) continue;
    any = true;
    if ((run.flags & kRunPre) || run.exact) {
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
  // Lines of centred and right aligned text move over (a line is aligned the way its first piece says).
  for (uint32_t line = 0; line < lines_.count(); ++line) {
    const uint32_t first = first_piece(line), end = end_piece(line);
    if (first == end) continue;
    const uint8_t align = doc.run(pieces_[first].run).align;
    if (align == 0) continue;
    uint32_t right = 0;
    for (uint32_t p = first; p < end; ++p) right = pieces_[p].column + pieces_[p].length > right ? pieces_[p].column + pieces_[p].length : right;
    const uint32_t room = columns_ > right ? columns_ - right : 0;
    const uint32_t shift = align == 1 ? room / 2 : room;
    for (uint32_t p = first; p < end; ++p) pieces_[p].column += shift;
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
