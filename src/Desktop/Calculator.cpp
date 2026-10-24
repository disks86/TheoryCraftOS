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
    {"C", 'C', 0, 0, 1},  {"<-", 'B', 0, 1, 1}, {"+/-", 'N', 0, 2, 1}, {"/", '/', 0, 3, 1},
    {"7", '7', 1, 0, 1},  {"8", '8', 1, 1, 1},  {"9", '9', 1, 2, 1},   {"*", '*', 1, 3, 1},
    {"4", '4', 2, 0, 1},  {"5", '5', 2, 1, 1},  {"6", '6', 2, 2, 1},   {"-", '-', 2, 3, 1},
    {"1", '1', 3, 0, 1},  {"2", '2', 3, 1, 1},  {"3", '3', 3, 2, 1},   {"+", '+', 3, 3, 1},
    {"0", '0', 4, 0, 2},  {".", '.', 4, 2, 1},  {"=", '=', 4, 3, 1},
};
constexpr uint32_t kRows = 5, kColumns = 4, kDisplayHeight = 44, kGap = 4;
constexpr uint32_t kMaxDigits = 15;
}  // namespace

Calculator::Calculator(allocators::IAllocator& allocator)
    : Frame(allocator), display_(allocator),
      buttons_{Button(allocator), Button(allocator), Button(allocator), Button(allocator), Button(allocator), Button(allocator), Button(allocator),
               Button(allocator), Button(allocator), Button(allocator), Button(allocator), Button(allocator), Button(allocator), Button(allocator),
               Button(allocator), Button(allocator), Button(allocator), Button(allocator), Button(allocator)},
      entry_(allocator) {
  static_assert(sizeof(kKeypad) / sizeof(kKeypad[0]) == kKeys, "one button per key");
  set_title("Calculator");
  set_bounds({160, 100, 260, 340});
  set_padding(Insets::all(6));
  set_minimize_enabled(true);

  display_.set_dock(DockStyle::Top);
  display_.set_size(0, static_cast<int32_t>(kDisplayHeight));
  add(&display_);
  for (uint32_t i = 0; i < kKeys; ++i) {
    buttons_[i].set_text(kKeypad[i].label);
    buttons_[i].on_click({&Calculator::on_button, this});
    add(&buttons_[i]);
  }
  entry_.assign("0");
  refresh();
}

void Calculator::layout() {
  Frame::layout();   // (the display docks to the top; the rest of the client area is the keypad)
  const Rect2D area = client_rect();
  const int32_t top = area.y + static_cast<int32_t>(kDisplayHeight) + static_cast<int32_t>(kGap);
  const int32_t width = static_cast<int32_t>(area.width), height = area.height > kDisplayHeight + kGap ? static_cast<int32_t>(area.height - kDisplayHeight - kGap) : 0;
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

void Calculator::refresh() { display_.set_text(entry_.view()); }

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
        entry_.assign("Error");
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
  if (!(args.modifiers & hardware::modifier::kControl)) {
    if (args.usage == key::kEnter) { press_equals(); return true; }
    if (args.usage == key::kBackspace) { press_backspace(); return true; }
    if (args.usage == key::kEscape || args.usage == key::kDelete) { press_clear(); return true; }
    const char c = args.character;
    if ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' || c == '*' || c == '/' || c == '=') {
      press(c);
      return true;
    }
  }
  return Frame::on_key(args);
}

}  // namespace desktop
