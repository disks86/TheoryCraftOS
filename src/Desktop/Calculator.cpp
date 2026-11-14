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

#include "Desktop/Calculator.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Kernel/Clipboard.hpp"
#include "Spreadsheet/Sheet.hpp"

namespace desktop {

using namespace gui;

namespace {
// The keypad: label, key it stands for, row, column and how many columns it spans.
struct Key {
  const char* label;
  char key;   // digits, '.', '+', '-', '*', '/', '=', 'C' (clear), 'B' (backspace), 'N' (negate)
  uint32_t row, column, span;
};
const Key kKeypad[] = {
    {"C", 'C', 0, 0, 1},  {"Back", 'B', 0, 1, 1}, {"+/-", 'N', 0, 2, 1}, {"/", '/', 0, 3, 1},
    {"7", '7', 1, 0, 1},  {"8", '8', 1, 1, 1},    {"9", '9', 1, 2, 1},   {"*", '*', 1, 3, 1},
    {"4", '4', 2, 0, 1},  {"5", '5', 2, 1, 1},    {"6", '6', 2, 2, 1},   {"-", '-', 2, 3, 1},
    {"1", '1', 3, 0, 1},  {"2", '2', 3, 1, 1},    {"3", '3', 3, 2, 1},   {"+", '+', 3, 3, 1},
    {"0", '0', 4, 0, 2},  {".", '.', 4, 2, 1},    {"=", '=', 4, 3, 1},
};
constexpr uint32_t kRows = 5, kColumns = 4, kDisplayHeight = 44, kGap = 4;
constexpr uint32_t kMaxDigits = 15;
// What the display says after a division by zero (the next digit, C or Escape starts over).
constexpr const char* kDivideByZero = "Cannot divide by zero";
}  // namespace

// The number in a sunken field, right-aligned like a pocket calculator's; the focus frame goes around the inside of the field.
void CalculatorDisplay::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  const int32_t width = Painter::text_width(text_.view());
  const int32_t x = static_cast<int32_t>(bounds_.width) - 8 - width, y = (static_cast<int32_t>(bounds_.height) - Painter::kGlyphHeight) / 2;
  painter.text(x > 4 ? x : 4, y, text_.view(), t.text);
  if (focused() && bounds_.width > 8 && bounds_.height > 8) painter.focus_frame({3, 3, bounds_.width - 6, bounds_.height - 6});
}

bool KeypadButton::on_key(const KeyArgs& args) {
  if (args.usage != hardware::key::kSpace) return false;   // (Enter goes on to the calculator: it is "=")
  click();
  return true;
}

Calculator::Calculator(allocators::IAllocator& allocator)
    : Frame(allocator), menu_(allocator), display_(allocator),
      buttons_{KeypadButton(allocator), KeypadButton(allocator), KeypadButton(allocator), KeypadButton(allocator), KeypadButton(allocator), KeypadButton(allocator),
               KeypadButton(allocator), KeypadButton(allocator), KeypadButton(allocator), KeypadButton(allocator), KeypadButton(allocator), KeypadButton(allocator),
               KeypadButton(allocator), KeypadButton(allocator), KeypadButton(allocator), KeypadButton(allocator), KeypadButton(allocator), KeypadButton(allocator),
               KeypadButton(allocator)},
      display_menu_(allocator), about_(allocator), entry_(allocator) {
  static_assert(sizeof(kKeypad) / sizeof(kKeypad[0]) == kKeys, "one button per key");
  set_title("Calculator");
  set_bounds({160, 100, 260, 362});
  set_padding(Insets::all(6));
  set_minimize_enabled(true);

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  if (Menu* edit = menu_.add_menu("Edit")) {
    menu_copy_item_ = edit->add_item("Copy", {&Calculator::on_copy, this}, "Ctrl+C");
    menu_paste_item_ = edit->add_item("Paste", {&Calculator::on_paste, this}, "Ctrl+V");
  }
  menu_.on_opening(Handler::immediate(&Calculator::on_menu_opening, this));
  if (Menu* help = menu_.add_menu("Help")) help->add_item("About Calculator", {&Calculator::on_about, this});
  add(&menu_);

  // The display comes first in the tab order, so the focus starts there.
  display_.set_dock(DockStyle::Top);
  display_.set_size(0, static_cast<int32_t>(kDisplayHeight));
  copy_item_ = display_menu_.add_item("Copy", {&Calculator::on_copy, this});
  paste_item_ = display_menu_.add_item("Paste", {&Calculator::on_paste, this});
  display_menu_.on_opening(Handler::immediate(&Calculator::on_menu_opening, this));
  display_.set_context_menu(&display_menu_);
  add(&display_);
  for (uint32_t i = 0; i < kKeys; ++i) {
    buttons_[i].set_text(kKeypad[i].label);
    buttons_[i].on_click({&Calculator::on_button, this});
    add(&buttons_[i]);
  }
  entry_.assign("0");
  refresh();
}

gui::Button* Calculator::key_button(char key) {
  for (uint32_t i = 0; i < kKeys; ++i)
    if (kKeypad[i].key == key) return &buttons_[i];
  return nullptr;
}

void Calculator::layout() {
  Frame::layout();   // (the menu bar and the display dock to the top; the rest of the client area is the keypad)
  const Rect2D area = client_rect();
  const int32_t top = display_.bounds().y + static_cast<int32_t>(display_.bounds().height) + static_cast<int32_t>(kGap);
  const int32_t bottom = area.y + static_cast<int32_t>(area.height);
  const int32_t width = static_cast<int32_t>(area.width), height = bottom > top ? bottom - top : 0;
  const int32_t cell_w = width / static_cast<int32_t>(kColumns), cell_h = height / static_cast<int32_t>(kRows);
  for (uint32_t i = 0; i < kKeys; ++i) {
    const Key& k = kKeypad[i];
    const int32_t x = area.x + static_cast<int32_t>(k.column) * cell_w, y = top + static_cast<int32_t>(k.row) * cell_h;
    const int32_t w = cell_w * static_cast<int32_t>(k.span) - static_cast<int32_t>(kGap), h = cell_h - static_cast<int32_t>(kGap);
    place(buttons_[i], {x, y, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(h > 0 ? h : 0)});
  }
}

void Calculator::on_button(void* self, Control& sender) {
  auto* c = static_cast<Calculator*>(self);
  for (uint32_t i = 0; i < kKeys; ++i)
    if (&sender == &c->buttons_[i]) return c->press(kKeypad[i].key);
}

void Calculator::press(char key) {
  if (key >= '0' && key <= '9') press_digit(key);
  else if (key == '.') press_point();
  else if (key == '+' || key == '-' || key == '*' || key == '/') press_operator(key);
  else if (key == '=') press_equals();
  else if (key == 'C') press_clear();
  else if (key == 'B') press_backspace();
  else if (key == 'N') press_negate();
}

// The display follows the entry; Copy applies while a number is shown (not the message after a division by zero).
void Calculator::refresh() {
  display_.set_text(entry_.view());
  if (copy_item_) copy_item_->set_enabled(!error_);
  if (menu_copy_item_) menu_copy_item_->set_enabled(!error_);
}

void Calculator::on_copy(void* self, Control&) { static_cast<Calculator*>(self)->copy_display(); }
void Calculator::on_paste(void* self, Control&) { static_cast<Calculator*>(self)->paste_number(); }
void Calculator::on_about(void* self, Control&) {
  auto* c = static_cast<Calculator*>(self);
  if (WindowManager* m = c->manager()) c->about_.show(*m, NoticeKind::Information, "Calculator is part of TheoryCraftOS.", "About Calculator");
}

// Paste applies while the clipboard holds text (whether that text is a number is only known when it is pasted).
void Calculator::on_menu_opening(void* self, Control&) {
  auto* c = static_cast<Calculator*>(self);
  const kernel::Clipboard* board = c->clipboard();
  const bool text = board && board->has_text();
  if (c->paste_item_) c->paste_item_->set_enabled(text);
  if (c->menu_paste_item_) c->menu_paste_item_->set_enabled(text);
}

bool Calculator::copy_display() {
  UiGuard lock(this);
  kernel::Clipboard* board = clipboard();
  return board && !error_ && board->set_text(entry_.view());
}

// A number on the clipboard (blanks and line ends around it do not count) replaces the one shown, as if it had been typed. Anything else is left alone.
bool Calculator::paste_number() {
  UiGuard lock(this);
  kernel::Clipboard* board = clipboard();
  String text(allocator());
  if (!board || !board->get_text(&text)) return false;
  StringView number = text.view();
  auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (!number.empty() && blank(number[0])) number = number.substr(1);
  while (!number.empty() && blank(number[number.size() - 1])) number = number.substr(0, number.size() - 1);
  double value = 0;
  if (!spreadsheet::Sheet::parse_number(number, &value)) return false;
  if (error_) press_clear();
  show_value(value);
  fresh_ = false;   // (it is the number being entered: an operator typed next applies to it)
  refresh();
  return true;
}

void Calculator::press_digit(char digit) {
  if (error_) press_clear();
  if (fresh_) {
    entry_.assign("");
    fresh_ = false;
  }
  if (entry_.view().equals("0")) entry_.assign("");
  uint32_t digits = 0;
  for (char ch : entry_.view()) digits += ch >= '0' && ch <= '9';
  if (digits < kMaxDigits) entry_.push_back(digit);
  if (entry_.empty()) entry_.assign("0");
  refresh();
}

void Calculator::press_point() {
  if (error_) press_clear();
  if (fresh_) {
    entry_.assign("0");
    fresh_ = false;
  }
  if (entry_.view().find('.') == StringView::npos) entry_.push_back('.');
  refresh();
}

void Calculator::apply_pending() {
  double rhs = 0;
  if (!spreadsheet::Sheet::parse_number(entry_.view(), &rhs)) rhs = 0;
  switch (pending_) {
    case '+': accumulator_ += rhs; break;
    case '-': accumulator_ -= rhs; break;
    case '*': accumulator_ *= rhs; break;
    case '/':
      if (rhs == 0) {
        error_ = true;
        entry_.assign(kDivideByZero);
        return;
      }
      accumulator_ /= rhs;
      break;
    default: accumulator_ = rhs; break;
  }
  show_value(accumulator_);
}

void Calculator::show_value(double value) {
  entry_.assign("");
  spreadsheet::Sheet::format_number(value, &entry_);
}

void Calculator::press_operator(char op) {
  if (error_) return;
  if (!fresh_ || pending_ == 0) apply_pending();   // (an operator typed twice only replaces the first)
  if (error_) return refresh();
  pending_ = op;
  fresh_ = true;
  refresh();
}

void Calculator::press_equals() {
  if (error_) return;
  if (pending_ != 0) apply_pending();
  pending_ = 0;
  fresh_ = true;
  refresh();
}

void Calculator::press_clear() {
  accumulator_ = 0;
  pending_ = 0;
  fresh_ = true;
  error_ = false;
  entry_.assign("0");
  refresh();
}

void Calculator::press_backspace() {
  if (error_) return press_clear();
  if (fresh_) return;   // (a result is not edited)
  entry_.erase(entry_.size() - 1, 1);
  if (entry_.empty() || entry_.view().equals("-")) entry_.assign("0");
  refresh();
}

void Calculator::press_negate() {
  if (error_ || entry_.view().equals("0")) return;
  if (entry_.view().starts_with('-')) entry_.erase(0, 1);
  else entry_.insert(0, "-");
  refresh();
}

bool Calculator::on_key(const KeyArgs& args) {
  namespace key = hardware::key;
  const bool ctrl = (args.modifiers & hardware::modifier::kControl) != 0;
  const bool alt = (args.modifiers & (hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt)) != 0;
  if (ctrl && !alt) {
    if (args.usage == key::kA + 2) { copy_display(); return true; }    // Ctrl+C
    if (args.usage == key::kA + 21) { paste_number(); return true; }   // Ctrl+V
  }
  if (!ctrl && !alt) {
    if (args.usage == key::kEnter || args.usage == key::kKeypadEnter) { press_equals(); return true; }
    if (args.usage == key::kBackspace) { press_backspace(); return true; }
    if (args.usage == key::kEscape || args.usage == key::kDelete) { press_clear(); return true; }
    if (args.usage == key::kF1 + 8) { press_negate(); return true; }   // F9 changes the sign
    const char c = args.character;
    if ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' || c == '*' || c == '/' || c == '=') {
      press(c);
      return true;
    }
  }
  return Frame::on_key(args);
}

}  // namespace desktop
