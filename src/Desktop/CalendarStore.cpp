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
      const int32_t month = static_cast<int32_t>(index % 12) + 1, last = gui::days_in_month(static_cast<int32_t>(year), month);
      const int64_t candidate = gui::days_from_date({static_cast<int32_t>(year), month, first.day < last ? first.day : last});
      if (candidate >= day) {
        result = candidate;
        break;
      }
    }
  }
  return r.until_day >= 0 && result > r.until_day ? -1 : result;
}

CalendarStore::CalendarStore(allocators::IAllocator& allocator, filesystem::IFileSystem* files)
    : allocator_(allocator), database_(allocator, files, filesystem::layout::kCalendarDatabase), connection_(allocator, database_), events_(allocator) {
  database_.open();   // (memory only if the file cannot be used: the calendar still works for this session)
  if (ensure_tables()) {
    migrate_first_version();
    load();
  }
}

bool CalendarStore::run(StringView sql, ArrayView<const DbParam> params) { return connection_.execute(sql, params, nullptr) == DbStatus::Ok; }

bool CalendarStore::ensure_tables() {
  return run("CREATE TABLE IF NOT EXISTS calendar_items (event_id INTEGER PRIMARY KEY, starts_at INTEGER NOT NULL, minutes INTEGER, remind_before INTEGER, last_notified INTEGER, "
             "repeat_kind INTEGER, repeat_every INTEGER, repeat_until INTEGER, title TEXT NOT NULL)", ArrayView<const DbParam>());
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
  if (static_cast<uint8_t>(recurrence.kind) > static_cast<uint8_t>(Repeat::Yearly) || recurrence.every < 1 || recurrence.every > Recurrence::kMaxEvery) return false;
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
    recurrence.kind = kind >= 0 && kind <= static_cast<int64_t>(Repeat::Yearly) ? static_cast<Repeat>(kind) : Repeat::None;
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
                            DbParam::from_int(-1),          DbParam::from_int(static_cast<int64_t>(recurrence.kind)),
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
  const DbParam params[] = {DbParam::from_int(start), DbParam::from_int(minutes), DbParam::from_int(remind_before), DbParam::from_int(static_cast<int64_t>(recurrence.kind)),
                            DbParam::from_int(recurrence.every), DbParam::from_int(recurrence.until_day), DbParam::from_text(title), DbParam::from_int(id)};
  if (!run("UPDATE calendar_items SET starts_at = ?, minutes = ?, remind_before = ?, last_notified = -1, repeat_kind = ?, repeat_every = ?, repeat_until = ?, title = ? WHERE event_id = ?",
           ArrayView<const DbParam>(params, 8)))
    return false;
  e->start = start;
  e->minutes = minutes;
  e->remind_before = remind_before;
  e->last_notified = -1;
  e->recurrence = recurrence;
  e->title.assign(copy.view());
  ++version_;
  return true;
}

bool CalendarStore::remove(uint32_t id) {
  kernel::ScopedMutex lock(mutex_);
  for (uint64_t i = 0; i < events_.count(); ++i) {
    if (events_[i].id != id) continue;
    const DbParam params[] = {DbParam::from_int(id)};
    if (!run("DELETE FROM calendar_items WHERE event_id = ?", ArrayView<const DbParam>(params, 1))) return false;
    events_.remove_at(i);
    ++version_;
    return true;
  }
  return false;
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

void CalendarStore::ids_on(int64_t day, List<uint32_t>* out) const {
  kernel::ScopedMutex lock(mutex_);
  for (const Event& e : events_) {
    if (!e.occurs_on(day)) continue;
    uint64_t at = out->count();   // (insertion sort by time of day, then by number)
    while (at > 0) {
      const Event* before = find((*out)[at - 1]);
      if (before && (before->time_of_day() < e.time_of_day() || (before->time_of_day() == e.time_of_day() && before->id < e.id))) break;
      --at;
    }
    out->insert(at, e.id);
  }
}

bool CalendarStore::has_events_on(int64_t day) const {
  kernel::ScopedMutex lock(mutex_);
  for (const Event& e : events_)
    if (e.occurs_on(day)) return true;
  return false;
}

void CalendarStore::take_due(int64_t now, List<Due>* out) {
  kernel::ScopedMutex lock(mutex_);
  for (Event& e : events_) {
    // The occurrence that matters is the first one that has not ended: it starts after now - length, so on a day after (now - length - time of day) / 1 day.
    const int64_t length = e.minutes ? e.minutes : 1;
    const int64_t day = next_occurrence_day(e.first_day(), e.recurrence, day_of(now - length - e.time_of_day()) + 1);
    if (day < 0) continue;
    const int64_t start = day * kMinutesPerDay + e.time_of_day();
    if (e.last_notified >= start || now < start - static_cast<int64_t>(e.remind_before)) continue;
    const DbParam params[] = {DbParam::from_int(start), DbParam::from_int(e.id)};
    if (!run("UPDATE calendar_items SET last_notified = ? WHERE event_id = ?", ArrayView<const DbParam>(params, 2))) continue;   // (tried again at the next minute)
    e.last_notified = start;
    ++version_;
    Due due;
    due.id = e.id;
    due.start = start;
    out->push_back(due);
  }
}

}  // namespace desktop
