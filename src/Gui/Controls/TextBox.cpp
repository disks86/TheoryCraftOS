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

#include "Gui/Controls/TextBox.hpp"
#include "Gui/ClipboardText.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/Move.hpp"

namespace gui {

namespace {
constexpr int32_t kPad = 4;  // between the border and the text
}

TextBox::TextBox(allocators::IAllocator& allocator) : Control(allocator), context_(allocator), text_(allocator), fallback_(allocator) {
  context_.on_opening(Handler::immediate(&TextBox::opening, this));
  set_context_menu(&context_);
}

// The context menu: built when first wanted, and its entries enabled for what the box holds.
void TextBox::opening(void* self, Control&) {
  auto* b = static_cast<TextBox*>(self);
  if (!b->cut_item_) {
    b->cut_item_ = b->context_.add_item("Cut", {&TextBox::run_cut, b}, "Ctrl+X");
    b->copy_item_ = b->context_.add_item("Copy", {&TextBox::run_copy, b}, "Ctrl+C");
    b->paste_item_ = b->context_.add_item("Paste", {&TextBox::run_paste, b}, "Ctrl+V");
    b->delete_item_ = b->context_.add_item("Delete", {&TextBox::run_delete, b}, "Del");
    b->context_.add_separator();
    b->select_item_ = b->context_.add_item("Select All", {&TextBox::run_select_all, b}, "Ctrl+A");
  }
  kernel::Clipboard* clipboard = b->clipboard();
  const bool can_paste = clipboard ? clipboard->has_text() : !b->fallback_.empty();
  if (b->cut_item_) b->cut_item_->set_enabled(b->has_selection() && !b->read_only_);
  if (b->copy_item_) b->copy_item_->set_enabled(b->has_selection());
  if (b->paste_item_) b->paste_item_->set_enabled(can_paste && !b->read_only_);
  if (b->delete_item_) b->delete_item_->set_enabled(b->has_selection() && !b->read_only_);
  if (b->select_item_) b->select_item_->set_enabled(!b->text_.empty());
}

void TextBox::run_cut(void* self, Control&) { static_cast<TextBox*>(self)->cut_selection(); }
void TextBox::run_copy(void* self, Control&) { static_cast<TextBox*>(self)->copy_selection(); }
void TextBox::run_paste(void* self, Control&) { static_cast<TextBox*>(self)->paste(); }
void TextBox::run_delete(void* self, Control&) { static_cast<TextBox*>(self)->delete_selection(); }
void TextBox::run_select_all(void* self, Control&) { static_cast<TextBox*>(self)->select_all(); }

// A right click outside the selection moves the caret there first, as a click does.
void TextBox::prepare_context_menu(int32_t x, int32_t) {
  if (x < 0) return;
  const uint32_t at = index_at(x);
  if (!has_selection() || at < selection_start() || at > selection_end()) move_caret(at, false);
}

bool TextBox::copy_selection() {
  UiGuard lock(this);
  return !password_ && has_selection() && clipboard_put_text(clipboard(), &fallback_, selected_text());
}

bool TextBox::cut_selection() {
  UiGuard lock(this);
  if (read_only_ || !copy_selection()) return false;
  return replace_selection(StringView());
}

uint32_t TextBox::visible_chars() const {
  const int32_t room = static_cast<int32_t>(bounds_.width) - 2 * kPad;
  return room > 0 ? static_cast<uint32_t>(room / Painter::kGlyphWidth) : 0;
}

void TextBox::scroll_to_caret() {
  const uint32_t vis = visible_chars();
  if (caret_ < scroll_) scroll_ = caret_;
  if (vis && caret_ > scroll_ + vis) scroll_ = caret_ - vis;
}

void TextBox::changed() {
  scroll_to_caret();
  invalidate();
  fire(changed_);
}

bool TextBox::set_text(StringView text) {
  UiGuard lock(this);
  if (max_length_ && text.size() > max_length_) text = text.substr(0, max_length_);
  if (!text_.assign(text)) return false;
  caret_ = anchor_ = static_cast<uint32_t>(text_.size());
  scroll_ = 0;
  changed();
  return true;
}

void TextBox::select_all() {
  UiGuard lock(this);
  anchor_ = 0;
  caret_ = static_cast<uint32_t>(text_.size());
  scroll_to_caret();
  invalidate();
}

void TextBox::move_caret(uint32_t to, bool extend) {
  const uint32_t length = static_cast<uint32_t>(text_.size());
  caret_ = to > length ? length : to;
  if (!extend) anchor_ = caret_;
  scroll_to_caret();
  invalidate();
}

// Replaces the selection (or inserts at the caret) with `with`, within the maximum length.
bool TextBox::replace_selection(StringView with) {
  if (read_only_) return false;
  const uint32_t from = selection_start(), to = selection_end();
  if (max_length_ && text_.size() - (to - from) + with.size() > max_length_) {
    const uint64_t room = max_length_ > text_.size() - (to - from) ? max_length_ - (text_.size() - (to - from)) : 0;
    with = with.first(room);
  }
  if (from == to && with.empty()) return false;
  String next(allocator());
  if (!next.reserve(text_.size() - (to - from) + with.size()) || !next.append(text_.view().first(from)) || !next.append(with) ||
      !next.append(text_.view().without_prefix(to)))
    return false;
  text_ = util::move(next);
  caret_ = anchor_ = from + static_cast<uint32_t>(with.size());
  changed();
  return true;
}

void TextBox::paste() {
  UiGuard lock(this);
  String text(allocator());
  if (read_only_ || !clipboard_get_text(clipboard(), fallback_, &text)) return;
  for (uint64_t i = 0; i < text.size(); ++i)  // one line only
    if (text[i] == '\n' || text[i] == '\r' || text[i] == '\t') text.set(i, ' ');
  replace_selection(text.view());
}

void TextBox::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  painter.push_frame({2, 2, bounds_.width > 4 ? bounds_.width - 4 : 0, bounds_.height > 4 ? bounds_.height - 4 : 0});
  const int32_t y = (static_cast<int32_t>(bounds_.height) - Painter::kGlyphHeight) / 2 - 2;
  const uint32_t vis = visible_chars();
  const uint64_t from = scroll_ < text_.size() ? scroll_ : text_.size();
  uint64_t count = text_.size() - from;
  if (count > vis + 1) count = vis + 1;  // one partial character may show
  const uint32_t ink = enabled() ? t.text : t.disabled_text;
  String stars(allocator());
  if (password_) stars.assign("");
  auto shown = [&](uint64_t a, uint64_t n) {   // the text in [a, a + n) as it is drawn
    if (!password_) return text_.view().substr(a, n);
    stars.clear();
    for (uint64_t i = 0; i < n; ++i) stars.push_back('*');
    return stars.view();
  };
  painter.text(kPad - 2, y, shown(from, count), ink);
  if (has_selection()) {  // the selected part again, light on dark
    uint64_t a = selection_start(), b = selection_end();
    if (a < from) a = from;
    if (b > from + count) b = from + count;
    if (b > a) {
      const int32_t x = kPad - 2 + static_cast<int32_t>(a - scroll_) * Painter::kGlyphWidth;
      painter.fill({x, y, static_cast<uint32_t>((b - a) * Painter::kGlyphWidth), static_cast<uint32_t>(Painter::kGlyphHeight)}, focused() ? t.selection : t.shadow);
      painter.text(x, y, shown(a, b - a), t.selection_text);
    }
  }
  if (focused() && enabled()) {
    const int32_t cx = kPad - 2 + static_cast<int32_t>(caret_ - scroll_) * Painter::kGlyphWidth;
    painter.line(cx, y, cx, y + Painter::kGlyphHeight - 1, t.text);
  }
  painter.pop_frame();
}

uint32_t TextBox::index_at(int32_t x) const {
  int32_t col = (x - kPad + Painter::kGlyphWidth / 2) / Painter::kGlyphWidth;
  if (x < kPad) col = 0;
  int64_t index = static_cast<int64_t>(scroll_) + col;
  if (index > static_cast<int64_t>(text_.size())) index = static_cast<int64_t>(text_.size());
  return static_cast<uint32_t>(index < 0 ? 0 : index);
}

void TextBox::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  dragging_ = true;
  move_caret(index_at(args.x), false);
}

void TextBox::on_mouse_move(const MouseArgs& args) {
  if (dragging_) move_caret(index_at(args.x), true);
}

bool TextBox::on_key(const KeyArgs& args) {
  using namespace hardware;
  const uint32_t length = static_cast<uint32_t>(text_.size());
  const bool shift = (args.modifiers & modifier::kShift) != 0, ctrl = (args.modifiers & modifier::kControl) != 0;
  if (ctrl) {
    switch (args.usage) {
      case key::kA: select_all(); return true;
      case key::kA + 2: copy_selection(); return true;  // C
      case key::kA + 23: cut_selection(); return true;  // X
      case key::kA + 21: paste(); return true;  // V
      case 0x49: copy_selection(); return true;  // Ctrl+Insert
      default: break;
    }
  }
  switch (args.usage) {
    case key::kLeft:
      if (has_selection() && !shift) move_caret(selection_start(), false);
      else move_caret(caret_ > 0 ? caret_ - 1 : 0, shift);
      return true;
    case key::kRight:
      if (has_selection() && !shift) move_caret(selection_end(), false);
      else move_caret(caret_ < length ? caret_ + 1 : length, shift);
      return true;
    case 0x4A: move_caret(0, shift); return true;       // Home
    case 0x4D: move_caret(length, shift); return true;  // End
    case key::kBackspace:
      if (has_selection()) replace_selection(StringView());
      else if (!read_only_ && caret_ > 0 && text_.erase(caret_ - 1, 1)) {
        --caret_;
        anchor_ = caret_;
        changed();
      }
      return true;
    case 0x4C:  // Delete (with Shift: cut)
      if (shift && has_selection()) {
        cut_selection();
      } else if (has_selection()) {
        replace_selection(StringView());
      } else if (!read_only_ && caret_ < length && text_.erase(caret_, 1)) {
        changed();
      }
      return true;
    case 0x49:  // Insert (with Shift: paste)
      if (shift) paste();
      return true;
    default:
      break;
  }
  const char c = args.character;
  if (c >= 32 && c < 127 && !ctrl) {
    if (!read_only_ && (!max_length_ || length - (selection_end() - selection_start()) < max_length_)) replace_selection(StringView(&c, 1));
    return true;
  }
  return false;  // Tab, Enter, Escape and friends go to the parent
}

}  // namespace gui
