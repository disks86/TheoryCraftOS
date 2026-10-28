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

CalendarStore::CalendarStore(allocators::IAllocator& allocator, filesystem::IFileSystem* files)
    : allocator_(allocator), database_(allocator, files, filesystem::layout::kCalendarDatabase), connection_(allocator, database_), events_(allocator) {
  database_.open();   // (memory only if the file cannot be used: the calendar still works for this session)
  if (ensure_tables()) load();
}

bool CalendarStore::run(StringView sql, ArrayView<const DbParam> params) { return connection_.execute(sql, params, nullptr) == DbStatus::Ok; }

bool CalendarStore::ensure_tables() {
  return run("CREATE TABLE IF NOT EXISTS calendar_events (event_id INTEGER PRIMARY KEY, starts_at INTEGER NOT NULL, minutes INTEGER, remind_before INTEGER, notified INTEGER, "
             "title TEXT NOT NULL)", ArrayView<const DbParam>());
}

bool CalendarStore::valid(int64_t start, uint32_t minutes, uint32_t remind_before, StringView title) {
  constexpr int64_t kLimit = gui::days_from_date({9999, 12, 31}) * kMinutesPerDay;
  return !title.empty() && start >= 0 && start < kLimit && minutes <= kMaxMinutes && remind_before <= kMaxMinutes;
}

void CalendarStore::load() {
  UniquePointer<database::IResultSet> rows;
  if (connection_.execute("SELECT event_id, starts_at, minutes, remind_before, notified, title FROM calendar_events ORDER BY event_id", &rows) != DbStatus::Ok || !rows) return;
  for (uint64_t r = 0; r < rows->row_count() && events_.count() < kMaxEvents; ++r) {
    int64_t id = 0, start = 0, minutes = 0, remind = 0, notified = 0;
    if (!rows->get_int(r, 0, &id) || !rows->get_int(r, 1, &start) || id <= 0 || id > 0x7FFFFFFF) continue;
    rows->get_int(r, 2, &minutes);
    rows->get_int(r, 3, &remind);
    rows->get_int(r, 4, &notified);
    StringView title = rows->text(r, 5);
    if (title.size() > kMaxTitleBytes) title = title.first(kMaxTitleBytes);
    if (minutes < 0 || minutes > kMaxMinutes) minutes = 0;
    if (remind < 0 || remind > kMaxMinutes) remind = 0;
    if (!valid(start, static_cast<uint32_t>(minutes), static_cast<uint32_t>(remind), title)) continue;   // (a damaged row)
    Event e(allocator_);
    e.id = static_cast<uint32_t>(id);
    e.start = start;
    e.minutes = static_cast<uint32_t>(minutes);
    e.remind_before = static_cast<uint32_t>(remind);
    e.notified = notified != 0;
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

uint32_t CalendarStore::add(int64_t start, uint32_t minutes, uint32_t remind_before, StringView title) {
  if (title.size() > kMaxTitleBytes) title = title.first(kMaxTitleBytes);
  kernel::ScopedMutex lock(mutex_);
  if (!valid(start, minutes, remind_before, title) || events_.count() >= kMaxEvents || next_id_ == 0x7FFFFFFF) return 0;
  Event e(allocator_);
  e.id = next_id_;
  e.start = start;
  e.minutes = minutes;
  e.remind_before = remind_before;
  if (!e.title.assign(title)) return 0;
  const DbParam params[] = {DbParam::from_int(e.id), DbParam::from_int(start), DbParam::from_int(minutes), DbParam::from_int(remind_before), DbParam::from_int(0), DbParam::from_text(title)};
  if (!run("INSERT INTO calendar_events VALUES (?, ?, ?, ?, ?, ?)", ArrayView<const DbParam>(params, 6))) return 0;
  if (!events_.push_back(static_cast<Event&&>(e))) {
    const DbParam gone[] = {DbParam::from_int(next_id_)};
    run("DELETE FROM calendar_events WHERE event_id = ?", ArrayView<const DbParam>(gone, 1));
    return 0;
  }
  ++version_;
  return next_id_++;
}

bool CalendarStore::update(uint32_t id, int64_t start, uint32_t minutes, uint32_t remind_before, StringView title) {
  if (title.size() > kMaxTitleBytes) title = title.first(kMaxTitleBytes);
  kernel::ScopedMutex lock(mutex_);
  Event* e = find(id);
  if (!e || !valid(start, minutes, remind_before, title)) return false;
  String copy(allocator_);
  if (!copy.assign(title)) return false;
  const DbParam params[] = {DbParam::from_int(start), DbParam::from_int(minutes), DbParam::from_int(remind_before), DbParam::from_text(title), DbParam::from_int(id)};
  if (!run("UPDATE calendar_events SET starts_at = ?, minutes = ?, remind_before = ?, notified = 0, title = ? WHERE event_id = ?", ArrayView<const DbParam>(params, 5))) return false;
  e->start = start;
  e->minutes = minutes;
  e->remind_before = remind_before;
  e->notified = false;
  e->title.assign(copy.view());
  ++version_;
  return true;
}

bool CalendarStore::remove(uint32_t id) {
  kernel::ScopedMutex lock(mutex_);
  for (uint64_t i = 0; i < events_.count(); ++i) {
    if (events_[i].id != id) continue;
    const DbParam params[] = {DbParam::from_int(id)};
    if (!run("DELETE FROM calendar_events WHERE event_id = ?", ArrayView<const DbParam>(params, 1))) return false;
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
  out->notified = e->notified;
  return out->title.assign(e->title.view());
}

void CalendarStore::ids_on(int64_t day, List<uint32_t>* out) const {
  kernel::ScopedMutex lock(mutex_);
  for (const Event& e : events_) {
    if (day_of(e.start) != day) continue;
    uint64_t at = out->count();   // (insertion sort by start, then by number)
    while (at > 0) {
      const Event* before = find((*out)[at - 1]);
      if (before && (before->start < e.start || (before->start == e.start && before->id < e.id))) break;
      --at;
    }
    out->insert(at, e.id);
  }
}

bool CalendarStore::has_events_on(int64_t day) const {
  kernel::ScopedMutex lock(mutex_);
  for (const Event& e : events_)
    if (day_of(e.start) == day) return true;
  return false;
}

void CalendarStore::take_due(int64_t now, List<uint32_t>* out) {
  kernel::ScopedMutex lock(mutex_);
  for (Event& e : events_) {
    if (e.notified) continue;
    const int64_t end = e.start + (e.minutes ? e.minutes : 1);
    if (now < e.start - static_cast<int64_t>(e.remind_before)) continue;
    const bool over = now >= end;
    const DbParam params[] = {DbParam::from_int(e.id)};
    if (!run("UPDATE calendar_events SET notified = 1 WHERE event_id = ?", ArrayView<const DbParam>(params, 1))) continue;   // (tried again at the next minute)
    e.notified = true;
    ++version_;
    if (!over) out->push_back(e.id);
  }
}

}  // namespace desktop
