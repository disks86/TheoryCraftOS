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

#include "Gui/Controls/DatePicker.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {
const char* const kMonths[12] = {"January", "February", "March",     "April",   "May",      "June",
                                 "July",    "August",   "September", "October", "November", "December"};
const char* const kWeekdayNames[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};

// Month and year as one line, e.g. "March 2026".
void month_title(const Date& d, char* out) {
  const char* name = kMonths[d.month - 1];
  int n = 0;
  while (*name) out[n++] = *name++;
  out[n++] = ' ';
  out[n++] = static_cast<char>('0' + d.year / 1000 % 10);
  out[n++] = static_cast<char>('0' + d.year / 100 % 10);
  out[n++] = static_cast<char>('0' + d.year / 10 % 10);
  out[n++] = static_cast<char>('0' + d.year % 10);
  out[n] = 0;
}
}  // namespace

// --- DatePickerPopup -----------------------------------------------------------

void DatePickerPopup::start(const Date& date) {
  cursor_ = date;
  show_month_of(date);
}

// The date in the top left cell: the Sunday on or before the first of the month on show.
Date DatePickerPopup::first_cell() const { return add_days(view_, -weekday(view_)); }

Rect2D DatePickerPopup::cell_of(const Date& date) const {
  const int64_t index = days_from_date(date) - days_from_date(first_cell());
  const int32_t col = static_cast<int32_t>(index % 7), row = static_cast<int32_t>(index / 7);
  return {kPad + col * kCellWidth, kGridTop + row * kRow, static_cast<uint32_t>(kCellWidth), static_cast<uint32_t>(kRow)};
}

bool DatePickerPopup::date_at(int32_t x, int32_t y, Date* out) const {
  if (x < kPad || y < kGridTop) return false;
  const int32_t col = (x - kPad) / kCellWidth, row = (y - kGridTop) / kRow;
  if (col >= 7 || row >= kRows) return false;
  *out = add_days(first_cell(), row * 7 + col);
  return true;
}

void DatePickerPopup::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.face);
  painter.bevel(all, false);

  // Header: previous / next month buttons around the month's name.
  const Rect2D prev{kPad, kPad, static_cast<uint32_t>(kHeader), static_cast<uint32_t>(kHeader)};
  const Rect2D next{kWidth - kPad - kHeader, kPad, static_cast<uint32_t>(kHeader), static_cast<uint32_t>(kHeader)};
  painter.fill(prev, t.face);
  painter.bevel(prev, false);
  painter.fill(next, t.face);
  painter.bevel(next, false);
  painter.text_centered(prev, "<", t.text);
  painter.text_centered(next, ">", t.text);
  char title[24];
  month_title(view_, title);
  painter.text_centered({kPad + kHeader, kPad, static_cast<uint32_t>(kWidth - 2 * kPad - 2 * kHeader), static_cast<uint32_t>(kHeader)}, title, t.text);

  // Weekday names, then the grid on a white field.
  for (int32_t c = 0; c < 7; ++c)
    painter.text_centered({kPad + c * kCellWidth, kPad + kHeader, static_cast<uint32_t>(kCellWidth), static_cast<uint32_t>(kWeekdays)}, kWeekdayNames[c], t.shadow);
  painter.fill({kPad, kGridTop, static_cast<uint32_t>(7 * kCellWidth), static_cast<uint32_t>(kRows * kRow)}, t.field);
  painter.outline({kPad - 1, kGridTop - 1, static_cast<uint32_t>(7 * kCellWidth + 2), static_cast<uint32_t>(kRows * kRow + 2)}, t.shadow);
  const Date first = first_cell();
  for (int32_t i = 0; i < 7 * kRows; ++i) {
    const Date d = add_days(first, i);
    const Rect2D cell{kPad + (i % 7) * kCellWidth, kGridTop + (i / 7) * kRow, static_cast<uint32_t>(kCellWidth), static_cast<uint32_t>(kRow)};
    const bool chosen = d == owner_.date();
    const bool this_month = d.month == view_.month && d.year == view_.year;
    if (chosen) painter.fill({cell.x + 1, cell.y + 1, cell.width - 2, cell.height - 2}, t.selection);
    char digits[3] = {static_cast<char>(d.day >= 10 ? '0' + d.day / 10 : ' '), static_cast<char>('0' + d.day % 10), 0};
    painter.text_centered(cell, StringView(digits, 2), chosen ? t.selection_text : this_month ? t.text : t.disabled_text);
    if (d == cursor_) painter.focus_frame({cell.x + 1, cell.y + 1, cell.width - 2, cell.height - 2});
  }
}

void DatePickerPopup::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  if (contains({kPad, kPad, static_cast<uint32_t>(kHeader), static_cast<uint32_t>(kHeader)}, args.x, args.y)) {
    view_ = add_months(view_, -1);
    invalidate();
  } else if (contains({kWidth - kPad - kHeader, kPad, static_cast<uint32_t>(kHeader), static_cast<uint32_t>(kHeader)}, args.x, args.y)) {
    view_ = add_months(view_, +1);
    invalidate();
  }
}

void DatePickerPopup::on_mouse_up(const MouseArgs& args) {
  Date d;
  if (args.button == MouseButton::Left && date_at(args.x, args.y, &d)) {
    owner_.set_date(d);
    owner_.close_popup();
  }
}

bool DatePickerPopup::on_key(const KeyArgs& args) {
  using namespace hardware;
  Date moved = cursor_;
  switch (args.usage) {
    case key::kLeft: moved = add_days(cursor_, -1); break;
    case key::kRight: moved = add_days(cursor_, +1); break;
    case key::kUp: moved = add_days(cursor_, -7); break;
    case key::kDown: moved = add_days(cursor_, +7); break;
    case 0x4B: moved = add_months(cursor_, -1); break;  // Page Up
    case 0x4E: moved = add_months(cursor_, +1); break;  // Page Down
    case key::kEnter:
    case key::kSpace:
      owner_.set_date(cursor_);
      owner_.close_popup();
      return true;
    case key::kEscape:
      owner_.close_popup();
      return true;
    default:
      return true;  // a popup swallows other keys
  }
  cursor_ = moved;
  show_month_of(moved);
  invalidate();
  return true;
}

// --- DatePicker ----------------------------------------------------------------

void DatePicker::set_date(const Date& date) {
  UiGuard lock(this);
  if (!is_valid(date) || date == date_) return;
  date_ = date;
  invalidate();
  fire(changed_);
}

void DatePicker::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const uint32_t text_color = enabled() ? t.text : t.disabled_text;
  char text[11];
  format_date(date_, text);
  const Rect2D label{3, 3, static_cast<uint32_t>(w > 24 ? w - 24 : 0), static_cast<uint32_t>(h > 6 ? h - 6 : 0)};
  if (focused() && !open_) painter.fill(label, t.selection);
  painter.text(label.x + 2, (h - Painter::kGlyphHeight) / 2, StringView(text, 10), focused() && !open_ ? t.selection_text : text_color);
  const Rect2D button{w - 19, 2, 17, static_cast<uint32_t>(h > 4 ? h - 4 : 0)};
  painter.fill(button, t.face);
  painter.bevel(button, open_);
  const int32_t cx = button.x + 8 + (open_ ? 1 : 0), cy = h / 2 - 1 + (open_ ? 1 : 0);
  for (int32_t i = 0; i < 4; ++i) painter.line(cx - 3 + i, cy + i - 1, cx + 3 - i, cy + i - 1, text_color);
}

void DatePicker::open_popup() {
  Window* w = window();
  if (!w) return;
  const Point o = window_origin();
  popup_.start(date_);
  if (w->open_popup(&popup_, this, o.x, o.y + static_cast<int32_t>(bounds_.height))) open_ = true;
  invalidate();
}

void DatePicker::close_popup() {
  UiGuard lock(this);
  if (open_ && window()) window()->close_popup();
  open_ = false;
}

void DatePicker::on_mouse_down(const MouseArgs& args) {
  if (args.button == MouseButton::Left) open_popup();  // a click while open is consumed by the window
}

bool DatePicker::on_key(const KeyArgs& args) {
  using namespace hardware;
  switch (args.usage) {
    case key::kLeft: set_date(add_days(date_, -1)); return true;
    case key::kRight: set_date(add_days(date_, +1)); return true;
    case key::kUp: set_date(add_days(date_, -7)); return true;
    case key::kDown:
      if (args.modifiers & (modifier::kLeftAlt | modifier::kRightAlt)) open_popup();
      else set_date(add_days(date_, +7));
      return true;
    case 0x4B: set_date(add_months(date_, -1)); return true;  // Page Up
    case 0x4E: set_date(add_months(date_, +1)); return true;  // Page Down
    case key::kSpace:
    case key::kEnter:
      open_popup();
      return true;
    default:
      return false;
  }
}

}  // namespace gui
