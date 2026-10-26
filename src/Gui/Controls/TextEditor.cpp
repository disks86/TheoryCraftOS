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

#include "Gui/Controls/TextEditor.hpp"
#include "Gui/ClipboardText.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Kernel/Clipboard.hpp"
#include "Utility/Move.hpp"

namespace gui {

namespace {
constexpr uint32_t kTabStop = static_cast<uint32_t>(TextEditor::kTabWidth);
}

TextEditor::TextEditor(allocators::IAllocator& allocator)
    : Control(allocator),
      context_(allocator),
      text_(allocator),
      clipboard_(allocator),
      scratch_(allocator),
      starts_(allocator),
      vscroll_(allocator, Orientation::Vertical),
      hscroll_(allocator, Orientation::Horizontal) {
  vscroll_.on_value_changed(Handler::immediate(&TextEditor::vertical_changed, this));
  hscroll_.on_value_changed(Handler::immediate(&TextEditor::horizontal_changed, this));
  hscroll_.set_step(4);
  add(&vscroll_);
  add(&hscroll_);
  vscroll_.set_visible(false);
  hscroll_.set_visible(false);
  starts_.push_back(0);
  context_.on_opening(Handler::immediate(&TextEditor::opening, this));
  set_context_menu(&context_);
}

void TextEditor::opening(void* self, Control&) {
  auto* e = static_cast<TextEditor*>(self);
  if (!e->cut_item_) {
    e->cut_item_ = e->context_.add_item("Cut", {&TextEditor::run_cut, e}, "Ctrl+X");
    e->copy_item_ = e->context_.add_item("Copy", {&TextEditor::run_copy, e}, "Ctrl+C");
    e->paste_item_ = e->context_.add_item("Paste", {&TextEditor::run_paste, e}, "Ctrl+V");
    e->delete_item_ = e->context_.add_item("Delete", {&TextEditor::run_delete, e}, "Del");
    e->context_.add_separator();
    e->select_item_ = e->context_.add_item("Select All", {&TextEditor::run_select_all, e}, "Ctrl+A");
  }
  kernel::Clipboard* clipboard = e->clipboard();
  const bool can_paste = clipboard ? clipboard->has_text() : !e->clipboard_.empty();
  e->cut_item_->set_enabled(e->has_selection() && !e->read_only_);
  e->copy_item_->set_enabled(e->has_selection());
  e->paste_item_->set_enabled(can_paste && !e->read_only_);
  e->delete_item_->set_enabled(e->has_selection() && !e->read_only_);
  e->select_item_->set_enabled(!e->text_.empty());
}

void TextEditor::run_cut(void* self, Control&) { static_cast<TextEditor*>(self)->cut_selection(); }
void TextEditor::run_copy(void* self, Control&) { static_cast<TextEditor*>(self)->copy_selection(); }
void TextEditor::run_paste(void* self, Control&) { static_cast<TextEditor*>(self)->paste(); }
void TextEditor::run_delete(void* self, Control&) { static_cast<TextEditor*>(self)->delete_selection(); }
void TextEditor::run_select_all(void* self, Control&) { static_cast<TextEditor*>(self)->select_all(); }

// A right click outside the selection moves the caret there first, as a click does.
void TextEditor::prepare_context_menu(int32_t x, int32_t y) {
  if (x < 0 || y < 0) return;
  const uint32_t at = offset_at_point(x, y);
  if (!has_selection() || at < selection_start() || at > selection_end()) move_caret(at, false);
}

void TextEditor::vertical_changed(void* self, Control&) { static_cast<TextEditor*>(self)->on_scroll_changed(); }
void TextEditor::horizontal_changed(void* self, Control&) { static_cast<TextEditor*>(self)->on_scroll_changed(); }

void TextEditor::on_scroll_changed() {
  if (syncing_) return;
  top_line_ = static_cast<uint32_t>(vscroll_.value());
  left_column_ = static_cast<uint32_t>(hscroll_.value());
  invalidate();
}

// --- Lines -----------------------------------------------------------------------

void TextEditor::rebuild_lines() {
  starts_.clear();
  starts_.push_back(0);
  max_columns_ = 0;
  uint32_t column = 0;
  const StringView t = text_.view();
  for (uint32_t i = 0; i < t.size(); ++i) {
    if (t[i] == '\n') {
      starts_.push_back(i + 1);
      if (column > max_columns_) max_columns_ = column;
      column = 0;
    } else {
      column += t[i] == '\t' ? kTabStop - column % kTabStop : 1;
    }
  }
  if (column > max_columns_) max_columns_ = column;
}

uint32_t TextEditor::line_length(uint32_t line) const {
  if (line >= starts_.count()) return 0;
  const uint32_t end = line + 1 < starts_.count() ? starts_[line + 1] - 1 : static_cast<uint32_t>(text_.size());
  return end - starts_[line];
}

uint32_t TextEditor::line_of(uint32_t offset) const {
  uint32_t low = 0, high = static_cast<uint32_t>(starts_.count());  // the last start <= offset
  while (high - low > 1) {
    const uint32_t mid = (low + high) / 2;
    if (starts_[mid] <= offset) low = mid;
    else high = mid;
  }
  return low;
}

uint32_t TextEditor::column_at(uint32_t line, uint32_t offset) const {
  const uint32_t start = line_start(line), end = start + line_length(line);
  if (offset > end) offset = end;
  uint32_t column = 0;
  for (uint32_t i = start; i < offset; ++i) column += text_[i] == '\t' ? kTabStop - column % kTabStop : 1;
  return column;
}

uint32_t TextEditor::offset_at(uint32_t line, uint32_t column) const {
  const uint32_t start = line_start(line), end = start + line_length(line);
  uint32_t at = start, col = 0;
  while (at < end && col < column) {
    const uint32_t width = text_[at] == '\t' ? kTabStop - col % kTabStop : 1;
    if (width > 1 && column - col < (width + 1) / 2) break;  // the first half of a tab belongs before it
    col += width;
    ++at;
  }
  return at;
}

void TextEditor::expand_line(uint32_t line, String* out) const {
  out->clear();
  const uint32_t start = line_start(line), end = start + line_length(line);
  for (uint32_t i = start; i < end; ++i) {
    const char c = text_[i];
    if (c == '\t') {
      for (uint32_t n = kTabStop - out->size() % kTabStop; n > 0; --n) out->push_back(' ');
    } else {
      out->push_back(c < 32 || c == 127 ? '.' : c);
    }
  }
}

// --- Editing ----------------------------------------------------------------------

bool TextEditor::set_text(StringView text) {
  UiGuard lock(this);
  if (text.size() > max_length_) return false;
  if (!text_.assign(text)) return false;
  caret_ = anchor_ = 0;
  wanted_column_ = -1;
  top_line_ = left_column_ = 0;
  modified_ = false;
  rebuild_lines();
  update_scroll_bars();
  invalidate();
  fire(changed_);
  fire(moved_);
  return true;
}

bool TextEditor::replace(uint32_t from, uint32_t to, StringView with) {
  if (read_only_ || from > to || to > text_.size()) return false;
  if (text_.size() - (to - from) + with.size() > max_length_) return false;
  String next(allocator());
  if (!next.reserve(text_.size() - (to - from) + with.size()) || !next.append(text_.view().first(from)) || !next.append(with) ||
      !next.append(text_.view().without_prefix(to)))
    return false;
  text_ = util::move(next);
  caret_ = anchor_ = from + static_cast<uint32_t>(with.size());
  wanted_column_ = -1;
  modified_ = true;
  rebuild_lines();
  ensure_caret_visible();
  invalidate();
  fire(changed_);
  fire(moved_);
  return true;
}

bool TextEditor::insert_text(StringView text) {
  UiGuard lock(this);
  return replace(selection_start(), selection_end(), text);
}

bool TextEditor::copy_selection() {
  UiGuard lock(this);
  return has_selection() && clipboard_put_text(clipboard(), &clipboard_, text_.view().substr(selection_start(), selection_end() - selection_start()));
}

bool TextEditor::cut_selection() {
  UiGuard lock(this);
  if (read_only_ || !copy_selection()) return false;
  return replace(selection_start(), selection_end(), StringView());
}

void TextEditor::paste() {
  UiGuard lock(this);
  String text(allocator());
  if (read_only_ || !clipboard_get_text(clipboard(), clipboard_, &text)) return;
  String clean(allocator());  // CR LF and lone CR become the editor's line break
  for (uint64_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') continue;
      clean.push_back('\n');
    } else {
      clean.push_back(text[i]);
    }
  }
  replace(selection_start(), selection_end(), clean.view());
}

void TextEditor::select_all() {
  UiGuard lock(this);
  anchor_ = 0;
  caret_ = static_cast<uint32_t>(text_.size());
  wanted_column_ = -1;
  ensure_caret_visible();
  invalidate();
  fire(moved_);
}

void TextEditor::move_caret(uint32_t offset, bool extend, bool keep_column) {
  if (offset > text_.size()) offset = static_cast<uint32_t>(text_.size());
  caret_ = offset;
  if (!extend) anchor_ = offset;
  if (!keep_column) wanted_column_ = -1;
  ensure_caret_visible();
  invalidate();
  fire(moved_);
}

void TextEditor::set_caret(uint32_t offset, bool extend) {
  UiGuard lock(this);
  move_caret(offset, extend);
}

// --- Scrolling ---------------------------------------------------------------------

uint32_t TextEditor::visible_lines() const { return body_rect().height / kLineHeight; }

uint32_t TextEditor::visible_columns() const {
  const int32_t room = static_cast<int32_t>(body_rect().width) - 2 * kPad;
  return room > 0 ? static_cast<uint32_t>(room / Painter::kGlyphWidth) : 0;
}

Rect2D TextEditor::body_rect() const {
  const int32_t w = static_cast<int32_t>(bounds_.width) - 2 * kBorder - (show_v_ ? ScrollBar::kThickness : 0);
  const int32_t h = static_cast<int32_t>(bounds_.height) - 2 * kBorder - (show_h_ ? ScrollBar::kThickness : 0);
  return {kBorder, kBorder, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(h > 0 ? h : 0)};
}

Point TextEditor::cell_origin(uint32_t line, uint32_t column) const {
  return {kBorder + kPad + (static_cast<int32_t>(column) - static_cast<int32_t>(left_column_)) * Painter::kGlyphWidth,
          kBorder + (static_cast<int32_t>(line) - static_cast<int32_t>(top_line_)) * kLineHeight};
}

void TextEditor::update_scroll_bars() {
  const int32_t full_w = static_cast<int32_t>(bounds_.width) - 2 * kBorder, full_h = static_cast<int32_t>(bounds_.height) - 2 * kBorder;
  const int32_t thick = ScrollBar::kThickness;
  const int32_t lines = static_cast<int32_t>(starts_.count()), columns = static_cast<int32_t>(max_columns_) + 1;
  bool v = false, h = false;
  for (int pass = 0; pass < 3; ++pass) {
    const int32_t visible_rows = (full_h - (h ? thick : 0)) / kLineHeight;
    const int32_t visible_cols = (full_w - (v ? thick : 0) - 2 * kPad) / Painter::kGlyphWidth;
    const bool need_v = lines > visible_rows, need_h = columns > visible_cols;
    if (need_v == v && need_h == h) break;
    v = need_v;
    h = need_h;
  }
  show_v_ = v;
  show_h_ = h;
  const int32_t rows = (full_h - (h ? thick : 0)) / kLineHeight > 0 ? (full_h - (h ? thick : 0)) / kLineHeight : 1;
  const int32_t cols = (full_w - (v ? thick : 0) - 2 * kPad) / Painter::kGlyphWidth > 0 ? (full_w - (v ? thick : 0) - 2 * kPad) / Painter::kGlyphWidth : 1;
  const int32_t max_top = lines > rows ? lines - rows : 0, max_left = columns > cols ? columns - cols : 0;
  if (static_cast<int32_t>(top_line_) > max_top) top_line_ = static_cast<uint32_t>(max_top);
  if (static_cast<int32_t>(left_column_) > max_left) left_column_ = static_cast<uint32_t>(max_left);
  syncing_ = true;
  vscroll_.set_metrics(lines, rows);
  vscroll_.set_value(static_cast<int32_t>(top_line_));
  hscroll_.set_metrics(columns, cols);
  hscroll_.set_value(static_cast<int32_t>(left_column_));
  syncing_ = false;
  vscroll_.set_visible(v);
  hscroll_.set_visible(h);
}

void TextEditor::ensure_caret_visible() {
  update_scroll_bars();
  const uint32_t line = line_of(caret_), column = column_at(line, caret_);
  const uint32_t rows = visible_lines() ? visible_lines() : 1, cols = visible_columns() ? visible_columns() : 1;
  if (line < top_line_) top_line_ = line;
  else if (line >= top_line_ + rows) top_line_ = line + 1 - rows;
  if (column < left_column_) left_column_ = column;
  else if (column >= left_column_ + cols) left_column_ = column + 1 - cols;
  update_scroll_bars();
}

void TextEditor::layout() {
  update_scroll_bars();
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const int32_t thick = ScrollBar::kThickness;
  const int32_t vh = h - 2 * kBorder - (show_h_ ? thick : 0), hw = w - 2 * kBorder - (show_v_ ? thick : 0);
  vscroll_.set_bounds({w - kBorder - thick, kBorder, static_cast<uint32_t>(thick), static_cast<uint32_t>(vh > 0 ? vh : 0)});
  hscroll_.set_bounds({kBorder, h - kBorder - thick, static_cast<uint32_t>(hw > 0 ? hw : 0), static_cast<uint32_t>(thick)});
  Control::layout();
}

// --- Painting -----------------------------------------------------------------------

void TextEditor::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  const uint32_t color = enabled() ? t.text : t.disabled_text;
  const Rect2D body = body_rect();
  painter.push_frame(body);
  const uint32_t rows = (body.height + kLineHeight - 1) / kLineHeight, cols = visible_columns() + 1;
  const uint32_t sel_from = selection_start(), sel_to = selection_end();
  for (uint32_t r = 0; r < rows && top_line_ + r < starts_.count(); ++r) {
    const uint32_t line = top_line_ + r;
    const int32_t y = static_cast<int32_t>(r) * kLineHeight;
    expand_line(line, &scratch_);
    const StringView full = scratch_.view();
    auto x_of = [&](uint32_t column) { return kPad + (static_cast<int32_t>(column) - static_cast<int32_t>(left_column_)) * Painter::kGlyphWidth; };
    auto shown = [&](uint32_t from, uint32_t to) {  // the part of the line between two columns that is on screen
      if (from < left_column_) from = left_column_;
      if (to > left_column_ + cols) to = left_column_ + cols;
      if (to > full.size()) to = static_cast<uint32_t>(full.size());
      return to > from ? full.substr(from, to - from) : StringView();
    };
    painter.text(kPad, y, shown(left_column_, left_column_ + cols), color);
    const uint32_t start = line_start(line), end = start + line_length(line);
    if (sel_to > sel_from && sel_from <= end && sel_to > start) {
      const uint32_t cs = sel_from > start ? column_at(line, sel_from) : 0;
      uint32_t ce = sel_to <= end ? column_at(line, sel_to) : column_at(line, end) + 1;  // the line break shows as a cell
      if (ce < cs) ce = cs;
      const int32_t x0 = x_of(cs) < kPad ? kPad : x_of(cs), x1 = x_of(ce);
      if (x1 > x0) {
        painter.fill({x0, y, static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(kLineHeight)}, focused() ? t.selection : t.shadow);
        const StringView part = shown(cs, ce);
        const uint32_t part_from = cs < left_column_ ? left_column_ : cs;
        if (!part.empty()) painter.text(x_of(part_from), y, part, t.selection_text);
      }
    }
    if (focused() && enabled() && caret_visible() && caret_ >= start && caret_ <= end) {
      const int32_t cx = x_of(column_at(line, caret_));
      if (cx >= kPad - 1) painter.line(cx, y, cx, y + kLineHeight - 1, t.text);
    }
  }
  painter.pop_frame();
  if (show_v_ && show_h_)
    painter.fill({static_cast<int32_t>(bounds_.width) - kBorder - ScrollBar::kThickness, static_cast<int32_t>(bounds_.height) - kBorder - ScrollBar::kThickness,
                  static_cast<uint32_t>(ScrollBar::kThickness), static_cast<uint32_t>(ScrollBar::kThickness)}, t.face);
}

// --- Input ------------------------------------------------------------------------------

uint32_t TextEditor::offset_at_point(int32_t x, int32_t y) const {
  int32_t row = y - kBorder;
  row = row >= 0 ? row / kLineHeight : -((-row + kLineHeight - 1) / kLineHeight);
  int32_t line = static_cast<int32_t>(top_line_) + row;
  if (line < 0) line = 0;
  if (line >= static_cast<int32_t>(starts_.count())) line = static_cast<int32_t>(starts_.count()) - 1;
  int32_t cell = x - kBorder - kPad + Painter::kGlyphWidth / 2;
  cell = cell >= 0 ? cell / Painter::kGlyphWidth : 0;
  return offset_at(static_cast<uint32_t>(line), left_column_ + static_cast<uint32_t>(cell));
}

void TextEditor::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left || !contains(body_rect(), args.x, args.y)) return;
  dragging_ = true;
  move_caret(offset_at_point(args.x, args.y), false);
}

void TextEditor::on_mouse_move(const MouseArgs& args) {
  if (dragging_) move_caret(offset_at_point(args.x, args.y), true);
}

void TextEditor::on_mouse_up(const MouseArgs&) { dragging_ = false; }

void TextEditor::on_mouse_wheel(const MouseArgs& args) {
  const int32_t direction = args.wheel > 0 ? -3 : args.wheel < 0 ? 3 : 0;
  int32_t top = static_cast<int32_t>(top_line_) + direction;
  if (top < 0) top = 0;
  top_line_ = static_cast<uint32_t>(top);
  update_scroll_bars();
  invalidate();
}

bool TextEditor::on_key(const KeyArgs& args) {
  using namespace hardware;
  const bool shift = (args.modifiers & modifier::kShift) != 0, ctrl = (args.modifiers & modifier::kControl) != 0;
  const uint32_t length = static_cast<uint32_t>(text_.size());
  const uint32_t line = line_of(caret_);
  if (ctrl) {
    switch (args.usage) {
      case key::kA: select_all(); return true;
      case key::kA + 2:  // C
        copy_selection();
        return true;
      case key::kA + 23:  // X
        if (!read_only_ && copy_selection()) replace(selection_start(), selection_end(), StringView());
        return true;
      case key::kA + 21:  // V
        paste();
        return true;
      case 0x49:  // Ctrl+Insert
        copy_selection();
        return true;
      default:
        break;
    }
  }
  const uint32_t page = visible_lines() > 1 ? visible_lines() - 1 : 1;
  auto vertical = [&](int32_t lines) {
    const int32_t target = static_cast<int32_t>(line) + lines;
    if (target < 0) {
      move_caret(0, shift);
    } else if (target >= static_cast<int32_t>(starts_.count())) {
      move_caret(length, shift);
    } else {
      const uint32_t wanted = wanted_column_ >= 0 ? static_cast<uint32_t>(wanted_column_) : column_at(line, caret_);
      move_caret(offset_at(static_cast<uint32_t>(target), wanted), shift, true);
      wanted_column_ = static_cast<int32_t>(wanted);
    }
  };
  switch (args.usage) {
    case key::kLeft:
      if (has_selection() && !shift) move_caret(selection_start(), false);
      else if (caret_ > 0) move_caret(caret_ - 1, shift);
      return true;
    case key::kRight:
      if (has_selection() && !shift) move_caret(selection_end(), false);
      else if (caret_ < length) move_caret(caret_ + 1, shift);
      return true;
    case key::kUp: vertical(-1); return true;
    case key::kDown: vertical(1); return true;
    case 0x4B: vertical(-static_cast<int32_t>(page)); return true;  // Page Up
    case 0x4E: vertical(static_cast<int32_t>(page)); return true;   // Page Down
    case 0x4A: move_caret(ctrl ? 0 : line_start(line), shift); return true;                       // Home
    case 0x4D: move_caret(ctrl ? length : line_start(line) + line_length(line), shift); return true;  // End
    case key::kBackspace:
      if (has_selection()) replace(selection_start(), selection_end(), StringView());
      else if (caret_ > 0) replace(caret_ - 1, caret_, StringView());
      return true;
    case 0x49:  // Shift+Insert
      if (shift) paste();
      return true;
    case 0x4C:  // Delete (with Shift: cut)
      if (shift && copy_selection()) replace(selection_start(), selection_end(), StringView());
      else if (has_selection()) replace(selection_start(), selection_end(), StringView());
      else if (caret_ < length) replace(caret_, caret_ + 1, StringView());
      return true;
    case key::kEnter: {
      String text(allocator());  // a line break, then the indentation of the line the caret is on
      text.push_back('\n');
      const uint32_t start = line_start(line);
      for (uint32_t i = start; i < caret_ && (text_[i] == ' ' || text_[i] == '\t'); ++i) text.push_back(text_[i]);
      replace(selection_start(), selection_end(), text.view());
      return true;
    }
    case key::kTab:
      if (!shift) replace(selection_start(), selection_end(), "\t");
      return true;
    default:
      break;
  }
  const char c = args.character;
  if (c >= 32 && c < 127 && !ctrl) {
    replace(selection_start(), selection_end(), StringView(&c, 1));
    return true;
  }
  return false;
}

}  // namespace gui
