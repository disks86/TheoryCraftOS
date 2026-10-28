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

// The appointments of the calendar application, kept in the application's own storage: one database file, /AppStore/Calendar/calendar.db
// (filesystem::layout::kCalendarDatabase), so they are still there after a restart.
//
//   calendar_events (event_id INTEGER PRIMARY KEY, starts_at, minutes, remind_before, notified INTEGER, title TEXT)
//
// Times are local times counted in minutes since 1970-01-01 00:00 (`local_minutes()`); the calendar has no time zones. An appointment
// is `minutes` long (0: no end given) and wants to be announced `remind_before` minutes before it starts. take_due() hands out the
// announcements that are due, once each: the flag is saved, so a restart does not announce an appointment again. Every change is on disk when
// the call returns. Safe to use from any thread.
#pragma once
#include "Database/Local/LocalDatabase.hpp"
#include "Gui/Date.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace desktop {

constexpr int64_t kMinutesPerDay = 24 * 60;

// The minute of the local calendar `date` at `hour`:`minute`.
constexpr int64_t local_minutes(const gui::Date& date, int32_t hour, int32_t minute) { return gui::days_from_date(date) * kMinutesPerDay + hour * 60 + minute; }
// Which day (days since 1970-01-01) a minute count falls on, and the minute within that day.
constexpr int64_t day_of(int64_t minutes) { return minutes >= 0 ? minutes / kMinutesPerDay : -((-minutes + kMinutesPerDay - 1) / kMinutesPerDay); }
constexpr int32_t minute_of_day(int64_t minutes) { return static_cast<int32_t>(minutes - day_of(minutes) * kMinutesPerDay); }

class CalendarStore {
 public:
  static constexpr uint32_t kMaxEvents = 5000, kMaxTitleBytes = 100, kMaxMinutes = 7 * 24 * 60;

  struct Event {
    explicit Event(allocators::IAllocator& allocator) : title(allocator) {}
    uint32_t id = 0;
    int64_t start = 0;          // local minutes
    uint32_t minutes = 0;       // length
    uint32_t remind_before = 0;
    bool notified = false;
    String title;
  };

  // `files` is the filesystem the database lives on (nullptr: the appointments are only kept in memory). It must outlive the store.
  CalendarStore(allocators::IAllocator& allocator, filesystem::IFileSystem* files);

  // True while the appointments are kept in the file.
  bool persistent() const { return database_.persistent(); }
  uint32_t count() const;
  // Counts changes (for a window that shows them).
  uint64_t version() const;

  // Adds an appointment; returns its number, 0 if it was refused (empty title, too many appointments, a value out of range, or the database could
  // not be written). The title is cut at kMaxTitleBytes.
  uint32_t add(int64_t start, uint32_t minutes, uint32_t remind_before, StringView title);
  // Changes an appointment (it will be announced again). False if there is none with that number, or on failure (nothing changes then).
  bool update(uint32_t id, int64_t start, uint32_t minutes, uint32_t remind_before, StringView title);
  bool remove(uint32_t id);
  bool get(uint32_t id, Event* out) const;

  // The numbers of the appointments that start on `day` (days since 1970-01-01), the earliest first.
  void ids_on(int64_t day, List<uint32_t>* out) const;
  bool has_events_on(int64_t day) const;
  // The appointments to announce at `now` (local minutes): the reminder time has come and they have not ended. Each is handed out once.
  // Appointments that ended unannounced (the machine was off) are marked as done without being handed out.
  void take_due(int64_t now, List<uint32_t>* out);

 private:
  static bool valid(int64_t start, uint32_t minutes, uint32_t remind_before, StringView title);
  bool run(StringView sql, ArrayView<const database::DbParam> params);
  bool ensure_tables();
  void load();
  Event* find(uint32_t id);
  const Event* find(uint32_t id) const;

  allocators::IAllocator& allocator_;
  database::local::LocalDatabase database_;
  database::local::LocalConnection connection_;
  mutable kernel::Mutex mutex_;
  List<Event> events_;
  uint32_t next_id_ = 1;
  uint64_t version_ = 0;
};

}  // namespace desktop
