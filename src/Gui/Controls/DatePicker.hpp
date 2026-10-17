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

#pragma once
#include "Gui/Control.hpp"
#include "Gui/Date.hpp"

namespace gui {

class DatePicker;

// The calendar that drops down from a DatePicker (a popup, drawn above the other controls): a month
// at a time, Sunday first, with the days of the neighbouring months dimmed. The arrows in the header
// (or Page Up/Down) change the month; clicking a day chooses it. Keys: arrows move the day under the
// cursor by one (left/right) or seven (up/down) days, Enter / Space choose, Escape closes.
class DatePickerPopup final : public Control {
 public:
  static constexpr int32_t kPad = 6, kCellWidth = 26, kRow = 20, kHeader = 22, kWeekdays = 18, kRows = 6;
  static constexpr int32_t kWidth = 2 * kPad + 7 * kCellWidth;
  static constexpr int32_t kHeight = kPad + kHeader + kWeekdays + kRows * kRow + kPad;

  DatePickerPopup(allocators::IAllocator& allocator, DatePicker& owner) : Control(allocator), owner_(owner) {}
  // Shows the month of `date` with the cursor on it.
  void start(const Date& date);
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

  // The month on show (its first day) and the cursor, for tests.
  const Date& month() const { return view_; }
  const Date& cursor() const { return cursor_; }
  // Where a day of the month on show is drawn (its cell, in the popup's coordinates); for tests.
  Rect2D cell_of(const Date& date) const;

 protected:
  Size default_size() const override { return {kWidth, kHeight}; }

 private:
  static constexpr int32_t kGridTop = kPad + kHeader + kWeekdays;

  Date first_cell() const;
  bool date_at(int32_t x, int32_t y, Date* out) const;
  void show_month_of(const Date& date) { view_ = {date.year, date.month, 1}; }

  DatePicker& owner_;
  Date view_{2000, 1, 1};
  Date cursor_{2000, 1, 1};
};

// Shows a date as YYYY-MM-DD; clicking it (or Space, Enter, Alt+Down) opens the calendar. While it is
// closed and focused the arrows change the date: left/right by a day, up/down by a week, Page Up/Down by
// a month. Years 1 to 9999.
class DatePicker : public Control {
 public:
  explicit DatePicker(allocators::IAllocator& allocator) : Control(allocator), popup_(allocator, *this) {}

  const Date& date() const { return date_; }
  // Ignored if `date` is not a real date. The handler fires only when the date changes.
  void set_date(const Date& date);
  void on_date_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }
  bool open() const { return open_; }
  void open_popup();
  void close_popup();
  // The calendar popup (for tests).
  DatePickerPopup& popup() { return popup_; }

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;
  void on_popup_closed(Control*) override { open_ = false; invalidate(); }

 protected:
  Size default_size() const override { return {130, 24}; }

 private:
  Date date_;
  Handler changed_;
  DatePickerPopup popup_;
  bool open_ = false;
};

}  // namespace gui
