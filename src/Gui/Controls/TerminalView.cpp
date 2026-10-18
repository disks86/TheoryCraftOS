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

#include "Gui/Controls/TerminalView.hpp"
#include "Gui/ClipboardText.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {
constexpr uint32_t kBackground = 0xFF000000u, kForeground = 0xFFC0C0C0u, kCaretColor = 0xFFC0C0C0u;
constexpr uint32_t kTabStop = 8;
}  // namespace

TerminalView::TerminalView(allocators::IAllocator& allocator)
    : Control(allocator),
      lines_(allocator),
      prompt_(allocator),
      input_(allocator),
      submitted_(allocator),
      saved_input_(allocator),
      scratch_(allocator),
      history_(allocator),
      scroll_bar_(allocator, Orientation::Vertical),
      clipboard_(allocator) {
  lines_.push_back(String(allocator));
  scroll_bar_.on_value_changed(Handler::immediate(&TerminalView::scroll_changed, this));
  add(&scroll_bar_);
}

// --- Output ------------------------------------------------------------------------------------

void TerminalView::write(StringView text) {
  UiGuard lock(this);
  for (uint64_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    String& current = lines_[lines_.count() - 1];
    if (c == '\n') {
      lines_.push_back(String(allocator()));
      column_ = 0;
      if (lines_.count() > kMaxLines) lines_.remove_at(0);
    } else if (c == '\t') {
      do {
        current.push_back(' ');
        ++column_;
      } while (column_ % kTabStop);
    } else if (c == '\b') {
      if (!current.empty()) {
        current.erase(current.size() - 1, 1);
        if (column_) --column_;
      }
    } else if (c >= 0x20 && c < 0x7F) {
      current.push_back(c);
      ++column_;
    } else if (static_cast<unsigned char>(c) >= 0x80) {
      current.push_back('?');   // (a UTF-8 sequence shows as several marks)
      ++column_;
    }
  }
  invalidate();
}

void TerminalView::clear() {
  UiGuard lock(this);
  lines_.clear();
  lines_.push_back(String(allocator()));
  column_ = 0;
  scrolled_ = 0;
  invalidate();
}

bool TerminalView::text(String* out) const {
  out->clear();
  for (uint64_t i = 0; i < lines_.count(); ++i) {
    if (i && !out->push_back('\n')) return false;
    if (!out->append(lines_[i].view())) return false;
  }
  return true;
}

bool TerminalView::copy_all() {
  UiGuard lock(this);
  String all(allocator());
  return text(&all) && clipboard_put_text(clipboard(), &clipboard_, all.view());
}

// --- The line being typed ----------------------------------------------------------------------------

void TerminalView::set_prompt(StringView prompt) {
  UiGuard lock(this);
  prompt_.assign(prompt);
  invalidate();
}

void TerminalView::set_input(StringView text) {
  UiGuard lock(this);
  input_.assign(text.size() > kMaxInput ? text.first(kMaxInput) : text);
  caret_ = static_cast<uint32_t>(input_.size());
  scrolled_ = 0;
  invalidate();
}

bool TerminalView::insert_input(StringView text) {
  UiGuard lock(this);
  String clean(allocator());
  for (uint64_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\n' || c == '\r') break;
    if (c == '\t') clean.push_back(' ');
    else if (c >= 0x20 && c < 0x7F) clean.push_back(c);
  }
  if (clean.empty() || input_.size() + clean.size() > kMaxInput) return false;
  if (!input_.insert(caret_, clean.view())) return false;
  caret_ += static_cast<uint32_t>(clean.size());
  scrolled_ = 0;
  invalidate();
  return true;
}

bool TerminalView::paste() {
  String text(allocator());
  return clipboard_get_text(clipboard(), clipboard_, &text) && insert_input(text.view());
}

void TerminalView::remember(StringView line) {
  if (line.empty()) return;
  if (history_.count() && history_[history_.count() - 1].view().equals(line)) return;
  if (history_.count() >= kMaxHistory) history_.remove_at(0);
  String entry(allocator());
  entry.assign(line);
  history_.push_back(static_cast<String&&>(entry));
}

void TerminalView::recall(int32_t direction) {
  const int32_t count = static_cast<int32_t>(history_.count());
  if (!count) return;
  if (history_at_ < 0) {
    if (direction > 0) return;
    saved_input_.assign(input_.view());
    history_at_ = count - 1;
  } else {
    history_at_ += direction;
    if (history_at_ >= count) history_at_ = -1;
    else if (history_at_ < 0) history_at_ = 0;
  }
  if (history_at_ < 0) set_input(saved_input_.view());
  else set_input(history_[static_cast<uint64_t>(history_at_)].view());
}

void TerminalView::submit() {
  UiGuard lock(this);
  submitted_.assign(input_.view());
  String echo(allocator());
  echo.append(prompt_.view());
  echo.append(input_.view());
  echo.push_back('\n');
  write(echo.view());
  remember(submitted_.view());
  history_at_ = -1;
  input_.clear();
  caret_ = 0;
  scrolled_ = 0;
  invalidate();
  fire(submitted_handler_);
}

// --- Layout and scrolling ---------------------------------------------------------------------------

uint32_t TerminalView::rows_of(uint64_t length, bool live) const {
  const uint32_t cols = columns_ ? columns_ : 1;
  if (live) return static_cast<uint32_t>(length / cols) + 1;   // (room for the caret after the last character)
  return length ? static_cast<uint32_t>((length + cols - 1) / cols) : 1;
}

uint32_t TerminalView::total_rows() const {
  uint32_t total = 0;
  for (uint64_t i = 0; i + 1 < lines_.count(); ++i) total += rows_of(lines_[i].size(), false);
  const uint64_t live = lines_[lines_.count() - 1].size() + prompt_.size() + input_.size();
  return total + rows_of(live, true);
}

void TerminalView::measure() {
  const int32_t w = static_cast<int32_t>(bounds_.width) - ScrollBar::kThickness - 2 * kMargin, h = static_cast<int32_t>(bounds_.height) - 2 * kMargin;
  columns_ = w >= Painter::kGlyphWidth ? static_cast<uint32_t>(w / Painter::kGlyphWidth) : 1;
  rows_ = h >= kLineHeight ? static_cast<uint32_t>(h / kLineHeight) : 1;
}

void TerminalView::sync_scroll_bar() {
  const uint32_t total = total_rows();
  const uint32_t max_back = total > rows_ ? total - rows_ : 0;
  if (scrolled_ > max_back) scrolled_ = max_back;
  syncing_ = true;
  scroll_bar_.set_metrics(static_cast<int32_t>(total), static_cast<int32_t>(rows_));
  scroll_bar_.set_value(static_cast<int32_t>(max_back - scrolled_));
  syncing_ = false;
}

void TerminalView::layout() {
  measure();
  sync_scroll_bar();
  const int32_t thick = ScrollBar::kThickness;
  scroll_bar_.set_bounds({static_cast<int32_t>(bounds_.width) - thick, 0, static_cast<uint32_t>(thick), bounds_.height});
  Control::layout();
}

void TerminalView::scroll_changed(void* self, Control&) {
  auto* t = static_cast<TerminalView*>(self);
  if (t->syncing_) return;
  const uint32_t total = t->total_rows();
  const uint32_t max_back = total > t->rows_ ? total - t->rows_ : 0;
  const uint32_t value = static_cast<uint32_t>(t->scroll_bar_.value());
  t->scrolled_ = value <= max_back ? max_back - value : 0;
  t->invalidate();
}

void TerminalView::scroll_by(int32_t rows) {
  UiGuard lock(this);
  int64_t back = static_cast<int64_t>(scrolled_) + rows;
  if (back < 0) back = 0;
  scrolled_ = static_cast<uint32_t>(back);
  sync_scroll_bar();
  invalidate();
}

void TerminalView::scroll_to_end() {
  UiGuard lock(this);
  scrolled_ = 0;
  sync_scroll_bar();
  invalidate();
}

void TerminalView::on_mouse_wheel(const MouseArgs& args) { scroll_by(args.wheel > 0 ? 3 : args.wheel < 0 ? -3 : 0); }

// --- Painting ---------------------------------------------------------------------------------------

void TerminalView::paint(Painter& painter) {
  const Rect2D all{0, 0, bounds_.width, bounds_.height - 0};
  painter.fill(all, kBackground);
  measure();
  sync_scroll_bar();
  const uint32_t total = total_rows();
  const uint32_t first = total > rows_ + scrolled_ ? total - rows_ - scrolled_ : 0;   // the first visual row on screen
  const uint32_t cols = columns_;
  uint32_t row = 0;   // visual row where the current logical line starts
  String live(allocator());
  const uint64_t count = lines_.count();
  for (uint64_t i = 0; i < count; ++i) {
    const bool is_live = i + 1 == count;
    StringView content = lines_[i].view();
    if (is_live) {
      live.assign(content);
      live.append(prompt_.view());
      live.append(input_.view());
      content = live.view();
    }
    const uint32_t n = rows_of(content.size(), is_live);
    if (row + n > first && row < first + rows_) {
      for (uint32_t r = 0; r < n; ++r) {
        const uint32_t visual = row + r;
        if (visual < first || visual >= first + rows_) continue;
        const uint64_t from = static_cast<uint64_t>(r) * cols;
        if (from < content.size()) {
          const uint64_t len = content.size() - from < cols ? content.size() - from : cols;
          painter.text(kMargin, kMargin + static_cast<int32_t>(visual - first) * kLineHeight, content.substr(from, len), kForeground);
        }
      }
    }
    if (is_live && focused()) {   // the caret: a block on the character it is in front of
      const uint64_t at = lines_[i].size() + prompt_.size() + caret_;
      const uint32_t visual = row + static_cast<uint32_t>(at / cols);
      if (visual >= first && visual < first + rows_) {
        const int32_t x = kMargin + static_cast<int32_t>(at % cols) * Painter::kGlyphWidth;
        const int32_t y = kMargin + static_cast<int32_t>(visual - first) * kLineHeight;
        painter.fill({x, y, static_cast<uint32_t>(Painter::kGlyphWidth), static_cast<uint32_t>(kLineHeight)}, kCaretColor);
        if (at < content.size()) painter.text(x, y, content.substr(at, 1), kBackground);
      }
    }
    row += n;
  }
}

// --- Keys -------------------------------------------------------------------------------------------

bool TerminalView::on_key(const KeyArgs& args) {
  namespace key = hardware::key;
  const bool ctrl = (args.modifiers & hardware::modifier::kControl) != 0;
  if (ctrl) {
    switch (args.usage) {
      case key::kA + 21: paste(); return true;                 // Ctrl+V
      case key::kA + 11: clear(); return true;                 // Ctrl+L
      case key::kA + 2:                                        // Ctrl+Shift+C copies everything, Ctrl+C alone is left to the program
        if (args.modifiers & hardware::modifier::kShift) { copy_all(); return true; }
        return false;
      case key::kA + 20: set_input(StringView()); return true;   // Ctrl+U clears the line
      default: return false;
    }
  }
  UiGuard lock(this);
  const uint32_t length = static_cast<uint32_t>(input_.size());
  switch (args.usage) {
    case key::kEnter: submit(); return true;
    case key::kBackspace:
      if (caret_ > 0) {
        input_.erase(caret_ - 1, 1);
        --caret_;
      }
      break;
    case key::kDelete:
      if (caret_ < length) input_.erase(caret_, 1);
      break;
    case key::kLeft: if (caret_ > 0) --caret_; break;
    case key::kRight: if (caret_ < length) ++caret_; break;
    case 0x4A: caret_ = 0; break;        // Home
    case 0x4D: caret_ = length; break;   // End
    case key::kUp: recall(-1); break;
    case key::kDown: recall(1); break;
    case 0x4B: scroll_by(static_cast<int32_t>(rows_ > 1 ? rows_ - 1 : 1)); return true;    // Page Up
    case 0x4E: scroll_by(-static_cast<int32_t>(rows_ > 1 ? rows_ - 1 : 1)); return true;   // Page Down
    case key::kTab: return true;
    default:
      if (args.character >= 0x20 && args.character < 0x7F) {
        const char c = args.character;
        insert_input(StringView(&c, 1));
        return true;
      }
      return false;
  }
  scrolled_ = 0;
  sync_scroll_bar();
  invalidate();
  return true;
}

}  // namespace gui
