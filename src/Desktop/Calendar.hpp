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

// The calendar application: a month on the left (click a day, or use the arrow keys; Page Up/Down change the month; the days that have appointments carry
// a mark), and on the right the appointments of the chosen day with a form to add one (a title, the time, the length and how many minutes before it starts
// it is announced), to change the one chosen in the list, or to delete it. The appointments are kept in the CalendarStore (the application's storage under
// /AppStore/Calendar); the desktop announces them as toasts when their time comes, whether this window is open or not. Like every program window it can be
// a tab of a tabbed window.
//
// An appointment can last the whole day ("All day": the way to keep a birthday or a holiday) and can repeat: every day, week, month or year (or every
// second, third, ... one), for ever or until a last date. The list shows the appointments that occur on the chosen day, repeating ones included,
// and the marks in the month show them too. Monthly and yearly ones can keep to a weekday instead of a date (the 4th Thursday of November, the last Monday of
// May). For a repeating appointment the "Change" box says whether Save and Delete work on all of its occurrences or only on the one chosen in the list (an
// occurrence can be given another day, time or title, or be left out; Reset gives it back to the series).
#pragma once
#include "Desktop/CalendarStore.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/CheckBox.hpp"
#include "Gui/Controls/DatePicker.hpp"
#include "Gui/Controls/DropDown.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/ListView.hpp"
#include "Gui/Controls/SpinBox.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

// The month on show: a header with the month's name and the buttons for the previous and next month, the weekdays, and six weeks of days.
class MonthGrid final : public gui::Control {
 public:
  static constexpr int32_t kCellWidth = 40, kCellHeight = 30, kHeader = 24, kWeekdays = 18, kRows = 6;
  static constexpr int32_t kWidth = 7 * kCellWidth, kHeight = kHeader + kWeekdays + kRows * kCellHeight;

  explicit MonthGrid(allocators::IAllocator& allocator) : Control(allocator) {}

  // The appointments whose days are marked (nullptr: none are).
  void set_store(const CalendarStore* store) { gui::UiGuard lock(this); store_ = store; invalidate(); }
  // The day that is drawn as today (it is outlined).
  void set_today(const gui::Date& today) { gui::UiGuard lock(this); today_ = today; invalidate(); }
  const gui::Date& selected() const { return selected_; }
  // Chooses a day (and shows its month); the handler fires when the day changes.
  void select(const gui::Date& date);
  // Shows the month of `date` without choosing a day.
  void show_month(const gui::Date& date);
  gui::Date month() const { return {view_.year, view_.month, 1}; }
  void on_selected(gui::Handler handler) { gui::UiGuard lock(this); selected_handler_ = handler; }
  // Where a day of the month on show is drawn, in this control's coordinates (an empty rectangle if it is not on show).
  gui::Rect2D cell_of(const gui::Date& date) const;

  bool focusable() const override { return true; }
  void paint(gui::Painter& painter) override;
  void on_mouse_down(const gui::MouseArgs& args) override;
  bool on_key(const gui::KeyArgs& args) override;

 protected:
  gui::Size default_size() const override { return {kWidth, kHeight}; }

 private:
  static constexpr int32_t kGridTop = kHeader + kWeekdays;
  gui::Date first_cell() const { return gui::add_days(month(), -gui::weekday(month())); }

  const CalendarStore* store_ = nullptr;
  gui::Date today_{2000, 1, 1}, selected_{2000, 1, 1}, view_{2000, 1, 1};
  gui::Handler selected_handler_;
};

class Calendar : public gui::Frame {
 public:
  // The local time as minutes since 1970-01-01 00:00 (-1: not known).
  using TimeSource = int64_t (*)(void* context);
  // What Save and Delete do to a repeating appointment: every occurrence of it, or only the one chosen in the list.
  enum Scope : int32_t { kAllOccurrences, kThisOccurrence };

  explicit Calendar(allocators::IAllocator& allocator);

  // The appointments (nullptr: none; the window then shows nothing). The store must outlive the window.
  void set_store(CalendarStore* store);
  // Where "today" comes from. The window goes to today's date.
  void set_time_source(TimeSource source, void* context);

  // The commands behind the buttons. add_event() and update_event() take the form as it is; they return false (and say why in status()) when the title
  // is empty, no appointment is chosen (update), the repeat settings do not fit the date or the store refuses. For a repeating appointment, update_event()
  // and delete_event() work on the whole series or on the occurrence chosen, as the "Change" box says; reset_event() gives a changed occurrence back to the
  // series.
  bool add_event();
  bool update_event();
  bool delete_event();
  bool reset_event();
  void go_to_today();
  // Chooses a day (the list shows its appointments).
  void select_date(const gui::Date& date);
  // Chooses the appointment at `index` in the list (-1: none) and puts it in the form.
  void select_event(int32_t index);
  // Shows the appointments of the chosen day again (after the store changed).
  void refresh();

  gui::Date selected_date() const { return grid_.selected(); }
  uint32_t event_count() const { return static_cast<uint32_t>(occurrences_.count()); }
  // The number of the appointment chosen in the list, 0 if none; and the occurrence (nullptr if none).
  uint32_t selected_event() const;
  const CalendarStore::Occurrence* selected_occurrence() const;
  StringView status() const { return status_.text(); }
  MonthGrid& grid() { return grid_; }
  gui::ListView& list() { return list_; }
  gui::TextBox& title_box() { return title_; }
  gui::DatePicker& date_box() { return date_; }
  gui::CheckBox& all_day_box() { return all_day_; }
  gui::DropDown& repeat_box() { return repeat_; }
  gui::DropDown& by_box() { return by_; }
  gui::DropDown& scope_box() { return scope_; }
  gui::SpinBox& every_box() { return every_; }
  gui::CheckBox& until_check() { return until_check_; }
  gui::DatePicker& until_box() { return until_; }
  gui::SpinBox& hour_box() { return hour_; }
  gui::SpinBox& minute_box() { return minute_; }
  gui::SpinBox& length_box() { return length_; }
  gui::SpinBox& remind_box() { return remind_; }
  gui::Button& add_button() { return add_; }
  gui::Button& update_button() { return update_; }
  gui::Button& delete_button() { return delete_; }
  gui::Button& reset_button() { return reset_; }
  gui::Button& today_button() { return today_button_; }

  void layout() override;

 private:
  static void on_day(void* self, gui::Control&);
  static void on_pick(void* self, gui::Control&);
  static void on_add(void* self, gui::Control&);
  static void on_update(void* self, gui::Control&);
  static void on_delete(void* self, gui::Control&);
  static void on_reset(void* self, gui::Control&);
  static void on_today(void* self, gui::Control&);
  static void on_form_changed(void* self, gui::Control&);
  void say(StringView text) { status_.set_text(text); }
  // The form's start (on the date of the form), and the same on the first day of the chosen appointment (for a change of all of its occurrences).
  int64_t form_start() const;
  int64_t series_start(const CalendarStore::Event& event) const;
  // The form's repeat settings, for an appointment whose first day is `first_day`; false (and the status says why) if the end date is before it or the
  // weekday settings do not fit it.
  bool form_recurrence(int64_t first_day, Recurrence* out);
  uint32_t form_minutes() const;
  bool repeating_selection() const;
  void update_form();   // enables what applies to the form as it is (no time for an all-day appointment, no "every" for one that does not repeat, ...)

  gui::Label day_label_, title_label_, time_label_, colon_label_, length_label_, remind_label_, repeat_label_, every_label_, by_label_, scope_label_, status_;
  MonthGrid grid_;
  gui::ListView list_;
  gui::TextBox title_;
  gui::DatePicker date_, until_;
  gui::SpinBox hour_, minute_, length_, remind_, every_;
  gui::CheckBox all_day_, until_check_;
  gui::DropDown repeat_, by_, scope_;
  gui::Button add_, update_, delete_, reset_, today_button_;
  CalendarStore* store_ = nullptr;
  TimeSource time_ = nullptr;
  void* time_context_ = nullptr;
  List<CalendarStore::Occurrence> occurrences_;   // the occurrences on the chosen day, in list order
};

}  // namespace desktop
