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

#include "Desktop/CalendarStore.hpp"
#include "FileSystem/SystemLayout.hpp"

namespace desktop {

using database::DbParam;
using database::DbStatus;

namespace {
// The day of the month `year`-`month` that a monthly or yearly recurrence of an appointment whose first day is `first` falls on.
int32_t pick_day(const Recurrence& r, const gui::Date& first, int32_t year, int32_t month) {
  const int32_t last = gui::days_in_month(year, month);
  if (r.on == On::Date) return first.day < last ? first.day : last;
  const int32_t wanted = gui::weekday(first);
  const int32_t place = (first.day - 1) / 7 + 1;   // (the 5th is always the last one)
  if (r.on == On::LastWeekday || place >= 5) return last - (gui::weekday({year, month, last}) - wanted + 7) % 7;
  return 1 + (wanted - gui::weekday({year, month, 1}) + 7) % 7 + 7 * (place - 1);
}
}  // namespace

bool recurrence_fits(int64_t first_day, const Recurrence& r) {
  if (r.on == On::Date) return true;
  if (r.kind != Repeat::Monthly && r.kind != Repeat::Yearly) return false;
  const gui::Date first = gui::date_from_days(first_day);
  return pick_day(r, first, first.year, first.month) == first.day;
}

int64_t next_occurrence_day(int64_t first_day, const Recurrence& r, int64_t day) {
  int64_t result = -1;
  if (day <= first_day) {
    result = first_day;
  } else if (r.kind == Repeat::Daily || r.kind == Repeat::Weekly) {
    const int64_t step = static_cast<int64_t>(r.every ? r.every : 1) * (r.kind == Repeat::Weekly ? 7 : 1);
    result = first_day + (day - first_day + step - 1) / step * step;
  } else if (r.kind == Repeat::Monthly || r.kind == Repeat::Yearly) {
    const gui::Date first = gui::date_from_days(first_day), want = gui::date_from_days(day);
    const int64_t step = static_cast<int64_t>(r.every ? r.every : 1) * (r.kind == Repeat::Yearly ? 12 : 1);
    const int64_t first_index = int64_t{first.year} * 12 + first.month - 1, want_index = int64_t{want.year} * 12 + want.month - 1;
    int64_t k = want_index <= first_index ? 0 : (want_index - first_index + step - 1) / step;
    for (int tries = 0; tries < 2; ++tries, ++k) {   // (the occurrence of the month of `day` may be before it: then the next one)
      const int64_t index = first_index + k * step;
      const int64_t year = index / 12;
      if (year > gui::kMaxYear) break;
      const int32_t month = static_cast<int32_t>(index % 12) + 1;
      const int64_t candidate = gui::days_from_date({static_cast<int32_t>(year), month, pick_day(r, first, static_cast<int32_t>(year), month)});
      if (candidate >= day) {
        result = candidate;
        break;
      }
    }
  }
  return r.until_day >= 0 && result > r.until_day ? -1 : result;
}

CalendarStore::CalendarStore(allocators::IAllocator& allocator, filesystem::IFileSystem* files)
    : allocator_(allocator), database_(allocator, files, filesystem::layout::kCalendarDatabase), connection_(allocator, database_), events_(allocator), exceptions_(allocator) {
  database_.open();   // (memory only if the file cannot be used: the calendar still works for this session)
  if (ensure_tables()) {
    migrate_first_version();
    load();
  }
}

bool CalendarStore::run(StringView sql, ArrayView<const DbParam> params) { return connection_.execute(sql, params, nullptr) == DbStatus::Ok; }

bool CalendarStore::ensure_tables() {
  return run("CREATE TABLE IF NOT EXISTS calendar_items (event_id INTEGER PRIMARY KEY, starts_at INTEGER NOT NULL, minutes INTEGER, remind_before INTEGER, last_notified INTEGER, "
             "repeat_kind INTEGER, repeat_every INTEGER, repeat_until INTEGER, title TEXT NOT NULL)", ArrayView<const DbParam>()) &&
         run("CREATE TABLE IF NOT EXISTS calendar_exceptions (event_id INTEGER NOT NULL, original_day INTEGER NOT NULL, cancelled INTEGER, starts_at INTEGER, minutes INTEGER, "
             "last_notified INTEGER, title TEXT, PRIMARY KEY (event_id, original_day))", ArrayView<const DbParam>());
}

// The first version of the calendar kept its appointments in calendar_events (no recurrence, a flag for "announced"). Without that table the query fails and
// there is nothing to do.
void CalendarStore::migrate_first_version() {
  UniquePointer<database::IResultSet> rows;
  if (connection_.execute("SELECT event_id, starts_at, minutes, remind_before, notified, title FROM calendar_events", &rows) != DbStatus::Ok || !rows) return;
  if (connection_.begin() != DbStatus::Ok) return;
  bool all = true;
  for (uint64_t r = 0; all && r < rows->row_count(); ++r) {
    int64_t id = 0, start = 0, minutes = 0, remind = 0, notified = 0;
    if (!rows->get_int(r, 0, &id) || !rows->get_int(r, 1, &start)) continue;
    rows->get_int(r, 2, &minutes);
    rows->get_int(r, 3, &remind);
    rows->get_int(r, 4, &notified);
    const DbParam params[] = {DbParam::from_int(id), DbParam::from_int(start), DbParam::from_int(minutes), DbParam::from_int(remind), DbParam::from_int(notified ? start : -1),
                              DbParam::from_int(0), DbParam::from_int(1), DbParam::from_int(-1), DbParam::from_text(rows->text(r, 5))};
    all &= run("INSERT INTO calendar_items VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", ArrayView<const DbParam>(params, 9));
  }
  all = all && run("DROP TABLE calendar_events", ArrayView<const DbParam>());
  if (all) connection_.commit();   // (otherwise nothing changes, and the next start tries again)
  else connection_.rollback();
}

bool CalendarStore::valid(int64_t start, uint32_t minutes, uint32_t remind_before, StringView title, const Recurrence& recurrence) {
  constexpr int64_t kLimit = gui::days_from_date({9999, 12, 31}) * kMinutesPerDay;
  if (title.empty() || start < 0 || start >= kLimit || minutes > kMaxMinutes || remind_before > kMaxMinutes) return false;
  if (static_cast<uint8_t>(recurrence.kind) > static_cast<uint8_t>(Repeat::Yearly) || static_cast<uint8_t>(recurrence.on) > static_cast<uint8_t>(On::LastWeekday) ||
      recurrence.every < 1 || recurrence.every > Recurrence::kMaxEvery || !recurrence_fits(day_of(start), recurrence))
    return false;
  return recurrence.until_day == -1 || (recurrence.until_day >= day_of(start) && recurrence.until_day < kLimit / kMinutesPerDay);
}

void CalendarStore::load() {
  UniquePointer<database::IResultSet> rows;
  if (connection_.execute("SELECT event_id, starts_at, minutes, remind_before, last_notified, repeat_kind, repeat_every, repeat_until, title FROM calendar_items ORDER BY event_id", &rows) != DbStatus::Ok || !rows) return;
  for (uint64_t r = 0; r < rows->row_count() && events_.count() < kMaxEvents; ++r) {
    int64_t id = 0, start = 0, minutes = 0, remind = 0, notified = -1, kind = 0, every = 1, until = -1;
    if (!rows->get_int(r, 0, &id) || !rows->get_int(r, 1, &start) || id <= 0 || id > 0x7FFFFFFF) continue;
    rows->get_int(r, 2, &minutes);
    rows->get_int(r, 3, &remind);
    rows->get_int(r, 4, &notified);
    rows->get_int(r, 5, &kind);
    rows->get_int(r, 6, &every);
    rows->get_int(r, 7, &until);
    StringView title = rows->text(r, 8);
    if (title.size() > kMaxTitleBytes) title = title.first(kMaxTitleBytes);
    if (minutes < 0 || minutes > kMaxMinutes) minutes = 0;
    if (remind < 0 || remind > kMaxMinutes) remind = 0;
    Recurrence recurrence;
    const int64_t base = kind % 10, on = kind / 10;   // (saved as kind + 10 * On)
    recurrence.kind = base >= 0 && base <= static_cast<int64_t>(Repeat::Yearly) ? static_cast<Repeat>(base) : Repeat::None;
    recurrence.on = on >= 0 && on <= static_cast<int64_t>(On::LastWeekday) ? static_cast<On>(on) : On::Date;
    recurrence.every = every >= 1 && every <= Recurrence::kMaxEvery ? static_cast<uint32_t>(every) : 1;
    recurrence.until_day = until;
    if (!valid(start, static_cast<uint32_t>(minutes), static_cast<uint32_t>(remind), title, recurrence)) continue;   // (a damaged row)
    Event e(allocator_);
    e.id = static_cast<uint32_t>(id);
    e.start = start;
    e.minutes = static_cast<uint32_t>(minutes);
    e.remind_before = static_cast<uint32_t>(remind);
    e.last_notified = notified;
    e.recurrence = recurrence;
    if (!e.title.assign(title) || !events_.push_back(static_cast<Event&&>(e))) continue;
    if (e.id >= next_id_) next_id_ = e.id + 1;
  }
  UniquePointer<database::IResultSet> extra;   // the exceptions of the occurrences
  if (connection_.execute("SELECT event_id, original_day, cancelled, starts_at, minutes, last_notified, title FROM calendar_exceptions", &extra) != DbStatus::Ok || !extra) return;
  for (uint64_t r = 0; r < extra->row_count() && exceptions_.count() < kMaxExceptions; ++r) {
    int64_t id = 0, day = 0, cancelled = 0, start = 0, minutes = 0, notified = -1;
    if (!extra->get_int(r, 0, &id) || !extra->get_int(r, 1, &day) || !extra->get_int(r, 3, &start) || id <= 0 || id > 0x7FFFFFFF) continue;
    extra->get_int(r, 2, &cancelled);
    extra->get_int(r, 4, &minutes);
    extra->get_int(r, 5, &notified);
    const Event* e = find(static_cast<uint32_t>(id));
    StringView title = extra->text(r, 6);
    if (title.size() > kMaxTitleBytes) title = title.first(kMaxTitleBytes);
    if (!e || !e->occurs_on(day) || find_exception(e->id, day) || minutes < 0 || minutes > kMaxMinutes || !valid(start, static_cast<uint32_t>(minutes), 0, title.empty() ? StringView("-") : title, Recurrence())) continue;   // (a damaged row)
    Exception x(allocator_);
    x.event_id = e->id;
    x.original_day = day;
    x.cancelled = cancelled != 0;
    x.start = start;
    x.minutes = static_cast<uint32_t>(minutes);
    x.last_notified = notified;
    if (x.title.assign(title)) exceptions_.push_back(static_cast<Exception&&>(x));
  }
}

CalendarStore::Event* CalendarStore::find(uint32_t id) {
  for (Event& e : events_)
    if (e.id == id) return &e;
  return nullptr;
}
const CalendarStore::Event* CalendarStore::find(uint32_t id) const {
  for (const Event& e : events_)
    if (e.id == id) return &e;
  return nullptr;
}

uint32_t CalendarStore::count() const {
  kernel::ScopedMutex lock(mutex_);
  return static_cast<uint32_t>(events_.count());
}

uint64_t CalendarStore::version() const {
  kernel::ScopedMutex lock(mutex_);
  return version_;
}

uint32_t CalendarStore::add(int64_t start, uint32_t minutes, uint32_t remind_before, StringView title, const Recurrence& recurrence) {
  if (title.size() > kMaxTitleBytes) title = title.first(kMaxTitleBytes);
  kernel::ScopedMutex lock(mutex_);
  if (!valid(start, minutes, remind_before, title, recurrence) || events_.count() >= kMaxEvents || next_id_ == 0x7FFFFFFF) return 0;
  Event e(allocator_);
  e.id = next_id_;
  e.start = start;
  e.minutes = minutes;
  e.remind_before = remind_before;
  e.recurrence = recurrence;
  if (!e.title.assign(title)) return 0;
  const DbParam params[] = {DbParam::from_int(e.id),        DbParam::from_int(start),
                            DbParam::from_int(minutes),     DbParam::from_int(remind_before),
                            DbParam::from_int(-1),          DbParam::from_int(static_cast<int64_t>(recurrence.kind) + 10 * static_cast<int64_t>(recurrence.on)),
                            DbParam::from_int(recurrence.every), DbParam::from_int(recurrence.until_day),
                            DbParam::from_text(title)};
  if (!run("INSERT INTO calendar_items VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", ArrayView<const DbParam>(params, 9))) return 0;
  if (!events_.push_back(static_cast<Event&&>(e))) {
    const DbParam gone[] = {DbParam::from_int(next_id_)};
    run("DELETE FROM calendar_items WHERE event_id = ?", ArrayView<const DbParam>(gone, 1));
    return 0;
  }
  ++version_;
  return next_id_++;
}

bool CalendarStore::update(uint32_t id, int64_t start, uint32_t minutes, uint32_t remind_before, StringView title, const Recurrence& recurrence) {
  if (title.size() > kMaxTitleBytes) title = title.first(kMaxTitleBytes);
  kernel::ScopedMutex lock(mutex_);
  Event* e = find(id);
  if (!e || !valid(start, minutes, remind_before, title, recurrence)) return false;
  String copy(allocator_);
  if (!copy.assign(title)) return false;
  // The exceptions are for days the rule gave: they stay while the rule gives the same days.
  const bool same_days = day_of(start) == e->first_day() && recurrence.kind == e->recurrence.kind && recurrence.on == e->recurrence.on && recurrence.every == e->recurrence.every;
  if (connection_.begin() != DbStatus::Ok) return false;
  const DbParam params[] = {DbParam::from_int(start), DbParam::from_int(minutes), DbParam::from_int(remind_before), DbParam::from_int(static_cast<int64_t>(recurrence.kind) + 10 * static_cast<int64_t>(recurrence.on)),
                            DbParam::from_int(recurrence.every), DbParam::from_int(recurrence.until_day), DbParam::from_text(title), DbParam::from_int(id)};
  const DbParam which[] = {DbParam::from_int(id)};
  const bool ok = run("UPDATE calendar_items SET starts_at = ?, minutes = ?, remind_before = ?, last_notified = -1, repeat_kind = ?, repeat_every = ?, repeat_until = ?, title = ? WHERE event_id = ?",
                      ArrayView<const DbParam>(params, 8)) &&
                  (same_days || run("DELETE FROM calendar_exceptions WHERE event_id = ?", ArrayView<const DbParam>(which, 1)));
  if (!ok || connection_.commit() != DbStatus::Ok) {
    connection_.rollback();
    return false;
  }
  e->start = start;
  e->minutes = minutes;
  e->remind_before = remind_before;
  e->last_notified = -1;
  e->recurrence = recurrence;
  e->title.assign(copy.view());
  if (!same_days) drop_exceptions(id);
  ++version_;
  return true;
}

bool CalendarStore::remove(uint32_t id) {
  kernel::ScopedMutex lock(mutex_);
  for (uint64_t i = 0; i < events_.count(); ++i) {
    if (events_[i].id != id) continue;
    if (connection_.begin() != DbStatus::Ok) return false;
    const DbParam params[] = {DbParam::from_int(id)};
    if (!run("DELETE FROM calendar_items WHERE event_id = ?", ArrayView<const DbParam>(params, 1)) ||
        !run("DELETE FROM calendar_exceptions WHERE event_id = ?", ArrayView<const DbParam>(params, 1)) || connection_.commit() != DbStatus::Ok) {
      connection_.rollback();
      return false;
    }
    events_.remove_at(i);
    drop_exceptions(id);
    ++version_;
    return true;
  }
  return false;
}

void CalendarStore::drop_exceptions(uint32_t id) {
  for (uint64_t i = exceptions_.count(); i > 0; --i)
    if (exceptions_[i - 1].event_id == id) exceptions_.remove_at(i - 1);
}

CalendarStore::Exception* CalendarStore::find_exception(uint32_t id, int64_t original_day) {
  for (Exception& x : exceptions_)
    if (x.event_id == id && x.original_day == original_day) return &x;
  return nullptr;
}
const CalendarStore::Exception* CalendarStore::find_exception(uint32_t id, int64_t original_day) const {
  for (const Exception& x : exceptions_)
    if (x.event_id == id && x.original_day == original_day) return &x;
  return nullptr;
}

bool CalendarStore::save_exception(const Exception& x) {
  if (connection_.begin() != DbStatus::Ok) return false;
  const DbParam which[] = {DbParam::from_int(x.event_id), DbParam::from_int(x.original_day)};
  const DbParam row[] = {DbParam::from_int(x.event_id), DbParam::from_int(x.original_day), DbParam::from_int(x.cancelled ? 1 : 0), DbParam::from_int(x.start),
                         DbParam::from_int(x.minutes),  DbParam::from_int(x.last_notified), DbParam::from_text(x.title.view())};
  if (!run("DELETE FROM calendar_exceptions WHERE event_id = ? AND original_day = ?", ArrayView<const DbParam>(which, 2)) ||
      !run("INSERT INTO calendar_exceptions VALUES (?, ?, ?, ?, ?, ?, ?)", ArrayView<const DbParam>(row, 7)) || connection_.commit() != DbStatus::Ok) {
    connection_.rollback();
    return false;
  }
  return true;
}

bool CalendarStore::change_occurrence(uint32_t id, int64_t original_day, int64_t start, uint32_t minutes, StringView title) {
  if (title.size() > kMaxTitleBytes) title = title.first(kMaxTitleBytes);
  kernel::ScopedMutex lock(mutex_);
  const Event* e = find(id);
  if (!e || !e->occurs_on(original_day) || !valid(start, minutes, e->remind_before, title, Recurrence())) return false;
  Exception* existing = find_exception(id, original_day);
  if (!existing && exceptions_.count() >= kMaxExceptions) return false;
  Exception x(allocator_);
  x.event_id = id;
  x.original_day = original_day;
  x.start = start;
  x.minutes = minutes;
  if (!x.title.assign(title) || !save_exception(x)) return false;
  if (existing) {
    existing->cancelled = false;
    existing->start = start;
    existing->minutes = minutes;
    existing->last_notified = -1;
    existing->title.assign(title);
  } else if (!exceptions_.push_back(static_cast<Exception&&>(x))) {
    const DbParam which[] = {DbParam::from_int(id), DbParam::from_int(original_day)};
    run("DELETE FROM calendar_exceptions WHERE event_id = ? AND original_day = ?", ArrayView<const DbParam>(which, 2));
    return false;
  }
  ++version_;
  return true;
}

bool CalendarStore::cancel_occurrence(uint32_t id, int64_t original_day) {
  kernel::ScopedMutex lock(mutex_);
  const Event* e = find(id);
  if (!e || !e->occurs_on(original_day)) return false;
  Exception* existing = find_exception(id, original_day);
  if (!existing && exceptions_.count() >= kMaxExceptions) return false;
  Exception x(allocator_);
  x.event_id = id;
  x.original_day = original_day;
  x.cancelled = true;
  x.start = original_day * kMinutesPerDay + e->time_of_day();
  x.minutes = e->minutes;
  if (!x.title.assign(e->title.view()) || !save_exception(x)) return false;
  if (existing) {
    existing->cancelled = true;
  } else if (!exceptions_.push_back(static_cast<Exception&&>(x))) {
    const DbParam which[] = {DbParam::from_int(id), DbParam::from_int(original_day)};
    run("DELETE FROM calendar_exceptions WHERE event_id = ? AND original_day = ?", ArrayView<const DbParam>(which, 2));
    return false;
  }
  ++version_;
  return true;
}

bool CalendarStore::restore_occurrence(uint32_t id, int64_t original_day) {
  kernel::ScopedMutex lock(mutex_);
  for (uint64_t i = 0; i < exceptions_.count(); ++i) {
    if (exceptions_[i].event_id != id || exceptions_[i].original_day != original_day) continue;
    const DbParam which[] = {DbParam::from_int(id), DbParam::from_int(original_day)};
    if (!run("DELETE FROM calendar_exceptions WHERE event_id = ? AND original_day = ?", ArrayView<const DbParam>(which, 2))) return false;
    exceptions_.remove_at(i);
    ++version_;
    return true;
  }
  return false;
}

uint32_t CalendarStore::exception_count() const {
  kernel::ScopedMutex lock(mutex_);
  return static_cast<uint32_t>(exceptions_.count());
}

bool CalendarStore::get(uint32_t id, Event* out) const {
  kernel::ScopedMutex lock(mutex_);
  const Event* e = find(id);
  if (!e) return false;
  out->id = e->id;
  out->start = e->start;
  out->minutes = e->minutes;
  out->remind_before = e->remind_before;
  out->last_notified = e->last_notified;
  out->recurrence = e->recurrence;
  return out->title.assign(e->title.view());
}

void CalendarStore::occurrences_on(int64_t day, List<Occurrence>* out) const {
  kernel::ScopedMutex lock(mutex_);
  auto add = [&](const Occurrence& o) {
    uint64_t at = out->count();   // (insertion sort by time of day, then by number)
    while (at > 0) {
      const Occurrence& before = (*out)[at - 1];
      if (before.time_of_day() < o.time_of_day() || (before.time_of_day() == o.time_of_day() && before.id <= o.id)) break;
      --at;
    }
    Occurrence copy(allocator_);
    copy.id = o.id;
    copy.original_day = o.original_day;
    copy.start = o.start;
    copy.minutes = o.minutes;
    copy.changed = o.changed;
    if (copy.title.assign(o.title.view())) out->insert(at, static_cast<Occurrence&&>(copy));
  };
  for (const Event& e : events_) {
    if (!e.occurs_on(day) || find_exception(e.id, day)) continue;
    Occurrence o(allocator_);
    o.id = e.id;
    o.original_day = day;
    o.start = day * kMinutesPerDay + e.time_of_day();
    o.minutes = e.minutes;
    o.title.assign(e.title.view());
    add(o);
  }
  for (const Exception& x : exceptions_) {   // (changed ones stand where they were moved to)
    if (x.cancelled || day_of(x.start) != day) continue;
    Occurrence o(allocator_);
    o.id = x.event_id;
    o.original_day = x.original_day;
    o.start = x.start;
    o.minutes = x.minutes;
    o.changed = true;
    o.title.assign(x.title.view());
    add(o);
  }
}

void CalendarStore::ids_on(int64_t day, List<uint32_t>* out) const {
  List<Occurrence> list(allocator_);
  occurrences_on(day, &list);
  for (const Occurrence& o : list) out->push_back(o.id);
}

bool CalendarStore::has_events_on(int64_t day) const {
  kernel::ScopedMutex lock(mutex_);
  for (const Event& e : events_)
    if (e.occurs_on(day) && !find_exception(e.id, day)) return true;
  for (const Exception& x : exceptions_)
    if (!x.cancelled && day_of(x.start) == day) return true;
  return false;
}

void CalendarStore::take_due(int64_t now, List<Due>* out) {
  kernel::ScopedMutex lock(mutex_);
  auto announce = [&](uint32_t id, int64_t start, uint32_t minutes, StringView title) {
    Due due(allocator_);
    due.id = id;
    due.start = start;
    due.minutes = minutes;
    if (due.title.assign(title)) out->push_back(static_cast<Due&&>(due));
  };
  for (Event& e : events_) {
    // The occurrence of the rule that matters is the first that has not ended and was not taken out: it starts after now - length, so on a day after
    // (now - length - time of day) / 1 day.
    const int64_t length = e.minutes ? e.minutes : 1;
    int64_t day = day_of(now - length - e.time_of_day()) + 1;
    for (int tries = 0; tries < 64; ++tries) {
      day = next_occurrence_day(e.first_day(), e.recurrence, day);
      if (day < 0) break;
      if (find_exception(e.id, day)) {   // (changed or cancelled: the exception speaks for it)
        ++day;
        continue;
      }
      const int64_t start = day * kMinutesPerDay + e.time_of_day();
      if (e.last_notified >= start || now < start - static_cast<int64_t>(e.remind_before)) break;
      const DbParam params[] = {DbParam::from_int(start), DbParam::from_int(e.id)};
      if (!run("UPDATE calendar_items SET last_notified = ? WHERE event_id = ?", ArrayView<const DbParam>(params, 2))) break;   // (tried again at the next minute)
      e.last_notified = start;
      ++version_;
      announce(e.id, start, e.minutes, e.title.view());
      break;
    }
    for (Exception& x : exceptions_) {   // the changed occurrences of it have reminders of their own
      if (x.event_id != e.id || x.cancelled || x.last_notified >= x.start) continue;
      if (now >= x.start + (x.minutes ? x.minutes : 1) || now < x.start - static_cast<int64_t>(e.remind_before)) continue;
      const DbParam params[] = {DbParam::from_int(x.start), DbParam::from_int(x.event_id), DbParam::from_int(x.original_day)};
      if (!run("UPDATE calendar_exceptions SET last_notified = ? WHERE event_id = ? AND original_day = ?", ArrayView<const DbParam>(params, 3))) continue;
      x.last_notified = x.start;
      ++version_;
      announce(x.event_id, x.start, x.minutes, x.title.view());
    }
  }
}

}  // namespace desktop
