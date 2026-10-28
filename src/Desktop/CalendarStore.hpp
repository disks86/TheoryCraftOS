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
//   calendar_items (event_id INTEGER PRIMARY KEY, starts_at, minutes, remind_before, last_notified, repeat_kind, repeat_every, repeat_until INTEGER, title TEXT)
//
// Times are local times counted in minutes since 1970-01-01 00:00 (`local_minutes()`); the calendar has no time zones. An appointment
// is `minutes` long (0: no end given) and wants to be announced `remind_before` minutes before it starts. An appointment that lasts
// the whole day (all_day(): it starts at midnight and is 24 hours long) is how birthdays and holidays are kept.
//
// An appointment may repeat (Recurrence): every `every` days, weeks, months or years, from its first day until an optional last day. A monthly or
// yearly appointment on a day the month does not have (the 31st, February 29th) falls on the last day of that month. take_due() hands out the
// announcements that are due, once for each occurrence: the start of the occurrence last announced is saved, so a restart does not announce it again.
// Every change is on disk when the call returns. Safe to use from any thread.
//
// (Databases of the first version had a table calendar_events without the recurrence; it is read once, copied and dropped.)
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

enum class Repeat : uint8_t { None, Daily, Weekly, Monthly, Yearly };

struct Recurrence {
  static constexpr uint32_t kMaxEvery = 999;
  Repeat kind = Repeat::None;
  uint32_t every = 1;      // 1: every day/week/month/year, 2: every other one, ...
  int64_t until_day = -1;  // the last day an occurrence may fall on (days since 1970-01-01); -1: no end
};

// The first day on or after `day` that an appointment whose first day is `first_day` occurs on; -1 if there is none (it does not repeat and is over, or the
// recurrence has ended).
int64_t next_occurrence_day(int64_t first_day, const Recurrence& recurrence, int64_t day);

class CalendarStore {
 public:
  static constexpr uint32_t kMaxEvents = 5000, kMaxTitleBytes = 100, kMaxMinutes = 7 * 24 * 60;

  struct Event {
    explicit Event(allocators::IAllocator& allocator) : title(allocator) {}
    uint32_t id = 0;
    int64_t start = 0;          // local minutes
    uint32_t minutes = 0;       // length
    uint32_t remind_before = 0;
    int64_t last_notified = -1;   // the start of the occurrence that was announced last (-1: none yet)
    Recurrence recurrence;
    String title;

    int64_t first_day() const { return day_of(start); }
    int32_t time_of_day() const { return minute_of_day(start); }
    bool repeats() const { return recurrence.kind != Repeat::None; }
    // Lasts the whole day: it starts at midnight and is 24 hours long.
    bool all_day() const { return time_of_day() == 0 && minutes == kMinutesPerDay; }
    bool occurs_on(int64_t day) const { return next_occurrence_day(first_day(), recurrence, day) == day; }
  };

  // An announcement: the appointment and the start of the occurrence it is about.
  struct Due {
    uint32_t id = 0;
    int64_t start = 0;
  };

  // `files` is the filesystem the database lives on (nullptr: the appointments are only kept in memory). It must outlive the store.
  CalendarStore(allocators::IAllocator& allocator, filesystem::IFileSystem* files);

  // True while the appointments are kept in the file.
  bool persistent() const { return database_.persistent(); }
  uint32_t count() const;
  // Counts changes (for a window that shows them).
  uint64_t version() const;

  // Adds an appointment; returns its number, 0 if it was refused (empty title, too many appointments, a value out of range, a recurrence that is not valid
  // or ends before the first day, or the database could not be written). The title is cut at kMaxTitleBytes.
  uint32_t add(int64_t start, uint32_t minutes, uint32_t remind_before, StringView title, const Recurrence& recurrence = Recurrence());
  // Changes an appointment (it will be announced again). False if there is none with that number, or on failure (nothing changes then).
  bool update(uint32_t id, int64_t start, uint32_t minutes, uint32_t remind_before, StringView title, const Recurrence& recurrence = Recurrence());
  bool remove(uint32_t id);
  bool get(uint32_t id, Event* out) const;

  // The numbers of the appointments that occur on `day` (days since 1970-01-01), the earliest first.
  void ids_on(int64_t day, List<uint32_t>* out) const;
  bool has_events_on(int64_t day) const;
  // The announcements to make at `now` (local minutes): for each appointment its current or next occurrence, when the reminder time of it has come and it has
  // not ended. Each occurrence is handed out once; one that ended unannounced (the machine was off) is never handed out.
  void take_due(int64_t now, List<Due>* out);

 private:
  static bool valid(int64_t start, uint32_t minutes, uint32_t remind_before, StringView title, const Recurrence& recurrence);
  bool run(StringView sql, ArrayView<const database::DbParam> params);
  bool ensure_tables();
  void load();
  void migrate_first_version();
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
