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

#include "Desktop/Calendar.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

using namespace gui;

namespace {
const char* const kMonths[12] = {"January", "February", "March",     "April",   "May",      "June",
                                 "July",    "August",   "September", "October", "November", "December"};
const char* const kWeekdayNames[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
constexpr int32_t kGap = 6, kRowHeight = 24, kLabelHeight = 22, kRightWidth = 330;

Rect2D rect(int32_t x, int32_t y, int32_t width, int32_t height) {
  return {x, y, static_cast<uint32_t>(width > 0 ? width : 0), static_cast<uint32_t>(height > 0 ? height : 0)};
}
}  // namespace

// --- MonthGrid -----------------------------------------------------------------

void MonthGrid::select(const Date& date) {
  UiGuard lock(this);
  if (!is_valid(date)) return;
  const bool changed = date != selected_;
  selected_ = date;
  view_ = {date.year, date.month, 1};
  invalidate();
  if (changed) fire(selected_handler_);
}

void MonthGrid::show_month(const Date& date) {
  UiGuard lock(this);
  if (!is_valid(date)) return;
  view_ = {date.year, date.month, 1};
  invalidate();
}

Rect2D MonthGrid::cell_of(const Date& date) const {
  const int64_t index = days_from_date(date) - days_from_date(first_cell());
  if (index < 0 || index >= 7 * kRows) return Rect2D{0, 0, 0, 0};
  return rect(static_cast<int32_t>(index % 7) * kCellWidth, kGridTop + static_cast<int32_t>(index / 7) * kCellHeight, kCellWidth, kCellHeight);
}

void MonthGrid::paint(Painter& painter) {
  const Theme& t = painter.theme();
  painter.fill(rect(0, 0, kWidth, kHeader), t.face);
  for (int32_t side = 0; side < 2; ++side) {
    const Rect2D button = rect(side == 0 ? 0 : kWidth - kHeader, 0, kHeader, kHeader);
    painter.fill(button, t.face);
    painter.bevel(button, false);
    painter.text_centered(button, side == 0 ? "<" : ">", t.text);
  }
  String title(allocator());
  title.assign(kMonths[view_.month - 1]);
  title.push_back(' ');
  title.append_uint(static_cast<uint64_t>(view_.year));
  painter.text_centered(rect(kHeader, 0, kWidth - 2 * kHeader, kHeader), title.view(), t.text);
  for (int32_t c = 0; c < 7; ++c) painter.text_centered(rect(c * kCellWidth, kHeader, kCellWidth, kWeekdays), kWeekdayNames[c], t.shadow);
  painter.fill(rect(0, kGridTop, kWidth, kRows * kCellHeight), t.field);
  const Date first = first_cell();
  for (int32_t i = 0; i < 7 * kRows; ++i) {
    const Date d = add_days(first, i);
    const Rect2D cell = rect((i % 7) * kCellWidth, kGridTop + (i / 7) * kCellHeight, kCellWidth, kCellHeight);
    const bool chosen = d == selected_, this_month = d.month == view_.month && d.year == view_.year;
    if (chosen) painter.fill(rect(cell.x + 1, cell.y + 1, kCellWidth - 2, kCellHeight - 2), t.selection);
    if (d == today_) painter.outline(rect(cell.x + 1, cell.y + 1, kCellWidth - 2, kCellHeight - 2), t.shadow);
    const char digits[3] = {static_cast<char>(d.day >= 10 ? '0' + d.day / 10 : ' '), static_cast<char>('0' + d.day % 10), 0};
    painter.text_centered(rect(cell.x, cell.y, kCellWidth, kCellHeight - 6), StringView(digits, 2), chosen ? t.selection_text : this_month ? t.text : t.disabled_text);
    if (store_ && store_->has_events_on(days_from_date(d)))   // (a small bar under the number)
      painter.fill(rect(cell.x + kCellWidth / 2 - 8, cell.y + kCellHeight - 7, 16, 3), chosen ? t.selection_text : 0xFF2060C0);
    if (chosen && focused()) painter.focus_frame(rect(cell.x + 1, cell.y + 1, kCellWidth - 2, kCellHeight - 2));
  }
  painter.outline(rect(0, kGridTop, kWidth, kRows * kCellHeight), t.shadow);
}

void MonthGrid::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  if (args.y >= 0 && args.y < kHeader) {
    if (args.x >= 0 && args.x < kHeader) show_month(add_months(month(), -1));
    else if (args.x >= kWidth - kHeader && args.x < kWidth) show_month(add_months(month(), +1));
    return;
  }
  if (args.x < 0 || args.x >= kWidth || args.y < kGridTop || args.y >= kGridTop + kRows * kCellHeight) return;
  select(add_days(first_cell(), (args.y - kGridTop) / kCellHeight * 7 + args.x / kCellWidth));
}

bool MonthGrid::on_key(const KeyArgs& args) {
  using namespace hardware;
  Date moved = selected_;
  switch (args.usage) {
    case key::kLeft: moved = add_days(selected_, -1); break;
    case key::kRight: moved = add_days(selected_, +1); break;
    case key::kUp: moved = add_days(selected_, -7); break;
    case key::kDown: moved = add_days(selected_, +7); break;
    case 0x4B: moved = add_months(selected_, -1); break;   // Page Up
    case 0x4E: moved = add_months(selected_, +1); break;   // Page Down
    default: return false;
  }
  select(moved);
  return true;
}

// --- Calendar ------------------------------------------------------------------

Calendar::Calendar(allocators::IAllocator& allocator)
    : Frame(allocator), day_label_(allocator), title_label_(allocator), time_label_(allocator), colon_label_(allocator), length_label_(allocator), remind_label_(allocator),
      repeat_label_(allocator), every_label_(allocator), by_label_(allocator), scope_label_(allocator), status_(allocator), grid_(allocator), list_(allocator), title_(allocator),
      date_(allocator), until_(allocator), hour_(allocator), minute_(allocator), length_(allocator), remind_(allocator), every_(allocator), all_day_(allocator),
      until_check_(allocator), repeat_(allocator), by_(allocator), scope_(allocator), add_(allocator), update_(allocator), delete_(allocator), reset_(allocator),
      today_button_(allocator), occurrences_(allocator) {
  set_title("Calendar");
  set_bounds({100, 20, 670, 600});
  set_padding(Insets::all(6));
  set_minimize_enabled(true);

  title_label_.set_text("Title");
  time_label_.set_text("On");
  colon_label_.set_text(":");
  length_label_.set_text("Length (minutes)");
  remind_label_.set_text("Remind (minutes before)");
  repeat_label_.set_text("Repeat");
  every_label_.set_text("every");
  by_label_.set_text("Keep to");
  scope_label_.set_text("Change");
  all_day_.set_text("All day");
  until_check_.set_text("Until");
  static const char* const kRepeatNames[] = {"Does not repeat", "Daily", "Weekly", "Monthly", "Yearly"};
  for (const char* name : kRepeatNames) repeat_.add_item(name);
  repeat_.set_selected_index(0);
  static const char* const kByNames[] = {"The date", "The weekday", "The last weekday"};
  for (const char* name : kByNames) by_.add_item(name);
  by_.set_selected_index(0);
  scope_.add_item("All occurrences");
  scope_.add_item("This occurrence only");
  scope_.set_selected_index(kAllOccurrences);
  every_.set_range(1, static_cast<int32_t>(Recurrence::kMaxEvery));
  add_.set_text("Add");
  update_.set_text("Save");
  delete_.set_text("Delete");
  reset_.set_text("Reset");
  today_button_.set_text("Today");
  title_.set_max_length(CalendarStore::kMaxTitleBytes);
  hour_.set_range(0, 23);
  minute_.set_range(0, 59);
  minute_.set_step(5);
  length_.set_range(0, static_cast<int32_t>(CalendarStore::kMaxMinutes));
  length_.set_step(15);
  remind_.set_range(0, static_cast<int32_t>(CalendarStore::kMaxMinutes));
  remind_.set_step(5);
  hour_.set_value(9);
  length_.set_value(60);
  remind_.set_value(10);
  list_.add_column("Time", 64);
  list_.add_column("Title", 150);
  list_.add_column("Repeats", 80);
  list_.set_view_mode(ViewMode::Details);
  list_.set_copy_enabled(false);

  grid_.on_selected({&Calendar::on_day, this});
  list_.on_selection_changed({&Calendar::on_pick, this});
  add_.on_click({&Calendar::on_add, this});
  update_.on_click({&Calendar::on_update, this});
  delete_.on_click({&Calendar::on_delete, this});
  reset_.on_click({&Calendar::on_reset, this});
  today_button_.on_click({&Calendar::on_today, this});
  all_day_.on_changed({&Calendar::on_form_changed, this});
  repeat_.on_selection_changed({&Calendar::on_form_changed, this});
  scope_.on_selection_changed({&Calendar::on_form_changed, this});
  until_check_.on_changed({&Calendar::on_form_changed, this});

  Control* const parts[] = {&grid_,   &today_button_, &day_label_, &list_,     &title_label_, &title_,    &time_label_, &date_,         &hour_,   &colon_label_, &minute_, &all_day_,
                            &length_label_, &length_, &remind_label_, &remind_, &repeat_label_, &repeat_, &every_label_, &every_, &by_label_, &by_, &until_check_, &until_,
                            &scope_label_,  &scope_,  &add_,          &update_, &delete_,       &reset_,  &status_};
  for (Control* part : parts) add(part);
  update_form();
  select_date(grid_.selected());
  date_.set_date(grid_.selected());
}

const CalendarStore::Occurrence* Calendar::selected_occurrence() const {
  const int32_t index = list_.selected_item();
  return index >= 0 && static_cast<uint64_t>(index) < occurrences_.count() ? &occurrences_[static_cast<uint64_t>(index)] : nullptr;
}

uint32_t Calendar::selected_event() const {
  const CalendarStore::Occurrence* o = selected_occurrence();
  return o ? o->id : 0;
}

bool Calendar::repeating_selection() const {
  CalendarStore::Event e(allocator());
  return store_ && selected_event() && store_->get(selected_event(), &e) && e.repeats();
}

void Calendar::update_form() {
  const bool all_day = all_day_.checked(), repeating = repeating_selection();
  const bool one = repeating && scope_.selected_index() == kThisOccurrence;   // (only this occurrence: the series settings are not part of the change)
  const bool kind_repeats = repeat_.selected_index() > 0;
  const bool by_choice = repeat_.selected_index() == static_cast<int32_t>(Repeat::Monthly) || repeat_.selected_index() == static_cast<int32_t>(Repeat::Yearly);
  hour_.set_enabled(!all_day);
  minute_.set_enabled(!all_day);
  length_.set_enabled(!all_day);
  date_.set_enabled(!repeating || one);
  scope_.set_enabled(repeating);
  remind_.set_enabled(!one);
  repeat_.set_enabled(!one);
  every_.set_enabled(kind_repeats && !one);
  by_.set_enabled(by_choice && !one);
  until_check_.set_enabled(kind_repeats && !one);
  until_.set_enabled(kind_repeats && !one && until_check_.checked());
  const CalendarStore::Occurrence* o = selected_occurrence();
  reset_.set_enabled(o && o->changed);
}

void Calendar::on_form_changed(void* self, Control&) { static_cast<Calendar*>(self)->update_form(); }

uint32_t Calendar::form_minutes() const { return all_day_.checked() ? static_cast<uint32_t>(kMinutesPerDay) : static_cast<uint32_t>(length_.value()); }

int64_t Calendar::form_start() const {
  return all_day_.checked() ? local_minutes(date_.date(), 0, 0) : local_minutes(date_.date(), hour_.value(), minute_.value());
}

// For all of the occurrences of a repeating appointment the first day stays: only its time of day comes from the form.
int64_t Calendar::series_start(const CalendarStore::Event& event) const {
  return event.first_day() * kMinutesPerDay + (all_day_.checked() ? 0 : hour_.value() * 60 + minute_.value());
}

bool Calendar::form_recurrence(int64_t first_day, Recurrence* out) {
  const int32_t kind = repeat_.selected_index();
  out->kind = kind > 0 && kind <= static_cast<int32_t>(Repeat::Yearly) ? static_cast<Repeat>(kind) : Repeat::None;
  out->every = out->kind == Repeat::None ? 1 : static_cast<uint32_t>(every_.value());
  out->until_day = -1;
  out->on = On::Date;
  if (out->kind == Repeat::Monthly || out->kind == Repeat::Yearly) {
    const int32_t by = by_.selected_index();
    out->on = by == 1 ? On::NthWeekday : by == 2 ? On::LastWeekday : On::Date;
  }
  if (!recurrence_fits(first_day, *out)) {
    say("That day is not the last of its weekday in the month.");
    return false;
  }
  if (out->kind != Repeat::None && until_check_.checked()) {
    out->until_day = days_from_date(until_.date());
    if (out->until_day < first_day) {
      say("The last day is before the appointment.");
      return false;
    }
  }
  return true;
}

void Calendar::set_store(CalendarStore* store) {
  UiGuard lock(this);
  store_ = store;
  grid_.set_store(store);
  refresh();
}

void Calendar::set_time_source(TimeSource source, void* context) {
  UiGuard lock(this);
  time_ = source;
  time_context_ = context;
  go_to_today();
}

void Calendar::go_to_today() {
  UiGuard lock(this);
  const int64_t now = time_ ? time_(time_context_) : -1;
  if (now < 0) return;
  const Date today = date_from_days(day_of(now));
  grid_.set_today(today);
  grid_.select(today);
  date_.set_date(today);
  refresh();
}

void Calendar::select_date(const Date& date) {
  UiGuard lock(this);
  if (!is_valid(date)) return;
  if (date == grid_.selected()) refresh();
  else grid_.select(date);   // (its handler refreshes)
}

void Calendar::refresh() {
  UiGuard lock(this);
  const uint32_t chosen = selected_event();
  const int64_t chosen_day = selected_occurrence() ? selected_occurrence()->original_day : 0;
  occurrences_.clear();
  list_.clear_items();
  String day(allocator());
  char date[11];
  format_date(grid_.selected(), date);
  day.assign("Appointments on ");
  day.append(StringView(date, 10));
  day_label_.set_text(day.view());
  if (store_) {
    store_->occurrences_on(days_from_date(grid_.selected()), &occurrences_);
    for (const CalendarStore::Occurrence& o : occurrences_) {
      const int32_t m = o.time_of_day();
      const char time[6] = {static_cast<char>('0' + m / 600), static_cast<char>('0' + m / 60 % 10), ':', static_cast<char>('0' + m % 60 / 10), static_cast<char>('0' + m % 10), 0};
      static const char* const kRepeats[] = {"", "Daily", "Weekly", "Monthly", "Yearly"};
      CalendarStore::Event e(allocator());
      const char* repeats = o.changed ? "Changed" : store_->get(o.id, &e) ? kRepeats[static_cast<uint8_t>(e.recurrence.kind)] : "";
      const StringView cells[3] = {o.all_day() ? StringView("All day") : StringView(time, 5), o.title.view(), StringView(repeats)};
      list_.add_item(cells, 3, ItemIcon::File);
    }
  }
  int32_t again = -1;
  for (uint64_t i = 0; i < occurrences_.count(); ++i)
    if (occurrences_[i].id == chosen && occurrences_[i].original_day == chosen_day) again = static_cast<int32_t>(i);
  list_.select_item(again);
  update_form();
  grid_.invalidate();
}

void Calendar::select_event(int32_t index) {
  UiGuard lock(this);
  list_.select_item(index);
  const CalendarStore::Occurrence* o = selected_occurrence();
  CalendarStore::Event e(allocator());
  if (!store_ || !o || !store_->get(o->id, &e)) {
    update_form();
    return;
  }
  title_.set_text(o->title.view());
  date_.set_date(date_from_days(day_of(o->start)));
  const int32_t m = o->time_of_day();
  hour_.set_value(m / 60);
  minute_.set_value(m % 60);
  length_.set_value(o->all_day() ? 60 : static_cast<int32_t>(o->minutes));
  remind_.set_value(static_cast<int32_t>(e.remind_before));
  all_day_.set_checked(o->all_day());
  repeat_.set_selected_index(static_cast<int32_t>(e.recurrence.kind));
  by_.set_selected_index(static_cast<int32_t>(e.recurrence.on));
  every_.set_value(static_cast<int32_t>(e.recurrence.every));
  until_check_.set_checked(e.recurrence.until_day >= 0);
  if (e.recurrence.until_day >= 0) until_.set_date(date_from_days(e.recurrence.until_day));
  scope_.set_selected_index(o->changed ? kThisOccurrence : kAllOccurrences);
  update_form();
}

bool Calendar::add_event() {
  UiGuard lock(this);
  if (!store_ || title_.text().empty()) {
    say(store_ ? "Give the appointment a title." : "There is no storage for appointments.");
    return false;
  }
  Recurrence recurrence;
  if (!form_recurrence(day_of(form_start()), &recurrence)) return false;
  if (!store_->add(form_start(), form_minutes(), static_cast<uint32_t>(remind_.value()), title_.text(), recurrence)) {
    say("The appointment could not be saved.");
    return false;
  }
  title_.set_text("");
  grid_.select(date_.date());   // (shows the day it was put on)
  refresh();
  say(store_->persistent() ? "Added." : "Added (the storage cannot be written: it is kept until shutdown).");
  return true;
}

bool Calendar::update_event() {
  UiGuard lock(this);
  const CalendarStore::Occurrence* o = selected_occurrence();
  CalendarStore::Event e(allocator());
  if (!store_ || !o || title_.text().empty() || !store_->get(o->id, &e)) {
    say(!o ? "Choose an appointment first." : "Give the appointment a title.");
    return false;
  }
  const uint32_t id = o->id;
  const int64_t original_day = o->original_day;
  if (e.repeats() && scope_.selected_index() == kThisOccurrence) {
    if (!store_->change_occurrence(id, original_day, form_start(), form_minutes(), title_.text())) {
      say("The occurrence could not be saved.");
      return false;
    }
    grid_.select(date_.date());
    refresh();
    say("Saved this occurrence.");
    return true;
  }
  const int64_t start = e.repeats() ? series_start(e) : form_start();
  Recurrence recurrence;
  if (!form_recurrence(day_of(start), &recurrence)) return false;
  if (!store_->update(id, start, form_minutes(), static_cast<uint32_t>(remind_.value()), title_.text(), recurrence)) {
    say("The appointment could not be saved.");
    return false;
  }
  if (!e.repeats()) grid_.select(date_.date());
  refresh();
  say(e.repeats() ? "Saved all occurrences." : "Saved.");
  return true;
}

bool Calendar::delete_event() {
  UiGuard lock(this);
  const CalendarStore::Occurrence* o = selected_occurrence();
  CalendarStore::Event e(allocator());
  if (!store_ || !o || !store_->get(o->id, &e)) {
    say("Choose an appointment first.");
    return false;
  }
  const bool one = e.repeats() && scope_.selected_index() == kThisOccurrence;
  if (!(one ? store_->cancel_occurrence(o->id, o->original_day) : store_->remove(o->id))) {
    say("The appointment could not be deleted.");
    return false;
  }
  refresh();
  say(one ? "Left out this occurrence." : "Deleted.");
  return true;
}

bool Calendar::reset_event() {
  UiGuard lock(this);
  const CalendarStore::Occurrence* o = selected_occurrence();
  if (!store_ || !o || !o->changed) {
    say("Choose a changed occurrence first.");
    return false;
  }
  if (!store_->restore_occurrence(o->id, o->original_day)) {
    say("The occurrence could not be reset.");
    return false;
  }
  grid_.select(date_from_days(o->original_day));
  refresh();
  say("The occurrence is as the series has it again.");
  return true;
}

void Calendar::on_day(void* self, Control&) {
  auto* c = static_cast<Calendar*>(self);
  c->date_.set_date(c->grid_.selected());
  c->refresh();
}
void Calendar::on_pick(void* self, Control&) {
  auto* c = static_cast<Calendar*>(self);
  c->select_event(c->list_.selected_item());
}
void Calendar::on_add(void* self, Control&) { static_cast<Calendar*>(self)->add_event(); }
void Calendar::on_update(void* self, Control&) { static_cast<Calendar*>(self)->update_event(); }
void Calendar::on_delete(void* self, Control&) { static_cast<Calendar*>(self)->delete_event(); }
void Calendar::on_reset(void* self, Control&) { static_cast<Calendar*>(self)->reset_event(); }
void Calendar::on_today(void* self, Control&) { static_cast<Calendar*>(self)->go_to_today(); }

void Calendar::layout() {
  Frame::layout();
  const Rect2D area = client_rect();
  const int32_t left = area.x, top = area.y, width = static_cast<int32_t>(area.width), height = static_cast<int32_t>(area.height);
  place(grid_, rect(left, top, MonthGrid::kWidth, MonthGrid::kHeight));
  place(today_button_, rect(left, top + MonthGrid::kHeight + kGap, 80, 26));
  place(status_, rect(left, top + height - kLabelHeight, MonthGrid::kWidth, kLabelHeight));

  const int32_t x = left + MonthGrid::kWidth + 2 * kGap;
  const int32_t w = width - MonthGrid::kWidth - 2 * kGap > kRightWidth ? width - MonthGrid::kWidth - 2 * kGap : kRightWidth;
  int32_t y = top;
  place(day_label_, rect(x, y, w, kLabelHeight));
  y += kLabelHeight + 2;
  const int32_t form_height = 9 * (kRowHeight + 4) + 30 + kGap;
  const int32_t list_height = height - (y - top) - form_height;
  place(list_, rect(x, y, w, list_height > 60 ? list_height : 60));
  y += (list_height > 60 ? list_height : 60) + kGap;
  place(title_label_, rect(x, y, 48, kRowHeight));
  place(title_, rect(x + 48, y, w - 48, kRowHeight));
  y += kRowHeight + 4;
  place(time_label_, rect(x, y, 48, kRowHeight));
  place(date_, rect(x + 48, y, 130, kRowHeight));
  place(hour_, rect(x + 186, y, 64, kRowHeight));
  place(colon_label_, rect(x + 250, y, 12, kRowHeight));
  place(minute_, rect(x + 262, y, 64, kRowHeight));
  y += kRowHeight + 4;
  place(all_day_, rect(x + 48, y, 100, kRowHeight));
  y += kRowHeight + 4;
  place(length_label_, rect(x, y, 16 * 8 + 8, kRowHeight));
  place(length_, rect(x + 16 * 8 + 8, y, 76, kRowHeight));
  y += kRowHeight + 4;
  place(remind_label_, rect(x, y, 23 * 8 + 8, kRowHeight));
  place(remind_, rect(x + 23 * 8 + 8, y, 76, kRowHeight));
  y += kRowHeight + 4;
  place(repeat_label_, rect(x, y, 56, kRowHeight));
  place(repeat_, rect(x + 56, y, 140, kRowHeight));
  place(every_label_, rect(x + 204, y, 48, kRowHeight));
  place(every_, rect(x + 252, y, 64, kRowHeight));
  y += kRowHeight + 4;
  place(by_label_, rect(x, y, 56, kRowHeight));
  place(by_, rect(x + 56, y, 140, kRowHeight));
  place(until_check_, rect(x + 204, y, 64, kRowHeight));
  y += kRowHeight + 4;
  place(until_, rect(x + 204, y, 120, kRowHeight));
  y += kRowHeight + 4;
  place(scope_label_, rect(x, y, 56, kRowHeight));
  place(scope_, rect(x + 56, y, 180, kRowHeight));
  y += kRowHeight + 6;
  place(add_, rect(x, y, 70, 26));
  place(update_, rect(x + 76, y, 70, 26));
  place(delete_, rect(x + 152, y, 70, 26));
  place(reset_, rect(x + 228, y, 70, 26));
}

}  // namespace desktop
