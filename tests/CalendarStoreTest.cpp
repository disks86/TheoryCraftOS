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

// Host-side test of the calendar's storage: appointments are added, changed and deleted, found by day, announced once when their reminder time comes,
// and survive a restart (a new store on the same filesystem). Damaged files do not crash it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Desktop/CalendarStore.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Database/Local/LocalDatabase.hpp"

using namespace desktop;
using gui::Date;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static const Date kDay{2026, 10, 27};

static bool read_all(filesystem::IFileSystem& fs, const char* path, ByteBuffer* data) {
  UniquePointer<filesystem::IFile> file;
  if (fs.open_file(path, filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return false;
  data->clear();
  uint8_t chunk[4096];
  for (uint64_t offset = 0;;) {
    uint64_t got = 0;
    if (file->read(offset, ByteView(chunk, sizeof chunk), &got) != filesystem::Status::Ok) return false;
    if (got == 0) return true;
    data->append(ConstByteView(chunk, got));
    offset += got;
  }
}

static bool write_all(filesystem::IFileSystem& fs, const char* path, ConstByteView data) {
  UniquePointer<filesystem::IFile> file;
  uint64_t put = 0;
  return fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok && file->write(0, data, &put) == filesystem::Status::Ok && put == data.size();
}

int main() {
  TestAllocator a;
  {
    // The time arithmetic: days and minutes of the day, also before 1970.
    CHECK(local_minutes({1970, 1, 1}, 0, 0) == 0);
    CHECK(local_minutes({1970, 1, 2}, 1, 30) == 1440 + 90);
    CHECK(day_of(local_minutes(kDay, 23, 59)) == gui::days_from_date(kDay) && minute_of_day(local_minutes(kDay, 23, 59)) == 23 * 60 + 59);
    CHECK(day_of(-1) == -1 && minute_of_day(-1) == 1439);
  }
  {
    filesystem::RamFileSystem files(a);
    const int64_t day = gui::days_from_date(kDay);
    uint32_t first = 0, second = 0;
    {
      CalendarStore store(a, &files);
      CHECK(store.persistent() && store.count() == 0);
      first = store.add(local_minutes(kDay, 14, 30), 45, 10, "Dentist");
      second = store.add(local_minutes(kDay, 9, 0), 0, 0, "Stand-up");
      const uint32_t third = store.add(local_minutes(gui::add_days(kDay, 1), 8, 0), 30, 5, "Next day");
      CHECK(first && second && third && first != second && second != third && store.count() == 3);
      CHECK(store.add(local_minutes(kDay, 8, 0), 0, 0, "") == 0);                       // no title
      CHECK(store.add(-5, 0, 0, "Before 1970") == 0);                                    // out of range
      CHECK(store.add(local_minutes(kDay, 8, 0), CalendarStore::kMaxMinutes + 1, 0, "Too long") == 0);
      CHECK(store.count() == 3);

      List<uint32_t> ids(a);
      store.ids_on(day, &ids);
      CHECK(ids.count() == 2 && ids[0] == second && ids[1] == first);   // the earliest first
      CHECK(store.has_events_on(day) && store.has_events_on(day + 1) && !store.has_events_on(day + 2));

      CalendarStore::Event e(a);
      CHECK(store.get(first, &e) && e.title.view().equals("Dentist") && e.minutes == 45 && e.remind_before == 10 && e.last_notified < 0);
      CHECK(!store.get(999, &e));

      store.remove(second);
      store.ids_on(day, &ids);
      CHECK(ids.count() == 3 && store.count() == 2);   // (ids still holds the earlier two; the list only grows)
    }
  }
  {
    // A fresh store: reminders and the restart.
    filesystem::RamFileSystem files(a);
    uint32_t id = 0, ended = 0;
    {
      CalendarStore store(a, &files);
      id = store.add(local_minutes(kDay, 14, 30), 45, 10, "Dentist");
      ended = store.add(local_minutes(kDay, 8, 0), 15, 0, "Early");
      List<CalendarStore::Due> due(a);
      store.take_due(local_minutes(kDay, 14, 19), &due);   // the early one is over: marked, not announced; the dentist is 11 minutes away
      CHECK(due.count() == 0);
      store.take_due(local_minutes(kDay, 14, 20), &due);
      CHECK(due.count() == 1 && due[0].id == id);
      due.clear();
      store.take_due(local_minutes(kDay, 14, 21), &due);
      CHECK(due.count() == 0);   // once
      CalendarStore::Event e(a);
      CHECK(store.get(id, &e) && e.last_notified >= 0);

      // Changing it announces it again.
      CHECK(store.update(id, local_minutes(kDay, 15, 0), 30, 5, "Dentist (moved)"));
      CHECK(store.get(id, &e) && e.last_notified < 0 && e.start == local_minutes(kDay, 15, 0) && e.title.view().equals("Dentist (moved)"));
      CHECK(!store.update(999, 0, 0, 0, "x") && !store.update(id, 0, 0, 0, ""));
      store.take_due(local_minutes(kDay, 14, 54), &due);
      CHECK(due.count() == 0);
      store.take_due(local_minutes(kDay, 14, 55), &due);
      CHECK(due.count() == 1 && due[0].id == id);   // 15:00 minus 5 minutes
    }
    {
      CalendarStore store(a, &files);   // the restart
      CHECK(store.count() == 2);
      CalendarStore::Event e(a);
      CHECK(store.get(id, &e) && e.title.view().equals("Dentist (moved)") && e.start == local_minutes(kDay, 15, 0) && e.minutes == 30 && e.remind_before == 5);
      CHECK(store.get(ended, &e) && e.last_notified < 0);   // (it ended unannounced: it simply is never announced)
      const uint32_t fresh = store.add(local_minutes(kDay, 16, 0), 0, 0, "After restart");
      CHECK(fresh > id && fresh > ended);   // numbers are not reused
      CHECK(store.remove(id) && !store.remove(id) && store.count() == 2);
    }
    {
      CalendarStore store(a, &files);
      CHECK(store.count() == 2);
      CalendarStore::Event e(a);
      CHECK(!store.get(id, &e));
    }
    filesystem::DirectoryEntry entry;
    CHECK(files.stat(filesystem::layout::kCalendarDatabase, &entry) == filesystem::Status::Ok && entry.size > 0);
  }
  {
    // Without a filesystem the calendar still works, in memory.
    CalendarStore store(a, nullptr);
    CHECK(!store.persistent());
    CHECK(store.add(local_minutes(kDay, 10, 0), 0, 0, "Only in memory") != 0 && store.count() == 1);
  }
  {
    // A damaged file is set aside and the calendar starts empty; one with garbage in a row skips the row.
    filesystem::RamFileSystem files(a);
    files.create_directory("/AppStore");
    files.create_directory("/AppStore/Calendar");
    {
      CalendarStore store(a, &files);
      store.add(local_minutes(kDay, 10, 0), 0, 0, "Kept");
    }
    ByteBuffer data(a);
    CHECK(read_all(files, filesystem::layout::kCalendarDatabase, &data) && data.size() > 16);
    const uint64_t cuts[] = {3, data.size() / 2, data.size() - 1};
    for (uint64_t cut : cuts) {   // truncated
      ByteBuffer copy(a);
      copy.append(data.view().first(cut));
      write_all(files, filesystem::layout::kCalendarDatabase, copy.view());
      CalendarStore store(a, &files);
      CHECK(store.count() <= 1);
      CHECK(store.add(local_minutes(kDay, 11, 0), 0, 0, "Works after damage") != 0);
    }
    for (uint64_t bit = 0; bit < 64; ++bit) {   // bit flips
      ByteBuffer copy(a);
      copy.append(data.view());
      copy.writable_view()[(bit * 7) % copy.size()] ^= static_cast<uint8_t>(1u << (bit % 8));
      write_all(files, filesystem::layout::kCalendarDatabase, copy.view());
      CalendarStore store(a, &files);
      CHECK(store.count() <= 2);
    }
  }
  {
    // Recurrence: which days an appointment occurs on.
    auto day = [](int32_t y, int32_t m, int32_t d) { return gui::days_from_date({y, m, d}); };
    Recurrence daily{Repeat::Daily, 3, -1}, weekly{Repeat::Weekly, 1, -1}, monthly{Repeat::Monthly, 1, -1}, yearly{Repeat::Yearly, 1, -1}, none;
    CHECK(next_occurrence_day(day(2026, 1, 1), none, day(2025, 12, 1)) == day(2026, 1, 1));
    CHECK(next_occurrence_day(day(2026, 1, 1), none, day(2026, 1, 2)) == -1);
    CHECK(next_occurrence_day(day(2026, 1, 1), daily, day(2026, 1, 1)) == day(2026, 1, 1));
    CHECK(next_occurrence_day(day(2026, 1, 1), daily, day(2026, 1, 2)) == day(2026, 1, 4));   // every third day
    CHECK(next_occurrence_day(day(2026, 1, 1), daily, day(2026, 1, 4)) == day(2026, 1, 4));
    CHECK(next_occurrence_day(day(2026, 10, 27), weekly, day(2026, 11, 4)) == day(2026, 11, 10));   // Tuesdays
    // Birthday on February 29th: February 28th in years without one.
    CHECK(next_occurrence_day(day(2024, 2, 29), yearly, day(2025, 1, 1)) == day(2025, 2, 28));
    CHECK(next_occurrence_day(day(2024, 2, 29), yearly, day(2025, 3, 1)) == day(2026, 2, 28));
    CHECK(next_occurrence_day(day(2024, 2, 29), yearly, day(2028, 1, 1)) == day(2028, 2, 29));
    CHECK(next_occurrence_day(day(2026, 12, 25), yearly, day(2026, 12, 26)) == day(2027, 12, 25));
    // The 31st falls on the last day of shorter months.
    CHECK(next_occurrence_day(day(2026, 1, 31), monthly, day(2026, 2, 1)) == day(2026, 2, 28));
    CHECK(next_occurrence_day(day(2026, 1, 31), monthly, day(2026, 2, 29 - 1 + 1)) == day(2026, 3, 31));
    CHECK(next_occurrence_day(day(2026, 1, 31), monthly, day(2026, 4, 1)) == day(2026, 4, 30));
    Recurrence every_other{Repeat::Monthly, 2, -1};
    CHECK(next_occurrence_day(day(2026, 1, 15), every_other, day(2026, 2, 1)) == day(2026, 3, 15));
    CHECK(next_occurrence_day(day(2026, 1, 15), every_other, day(2026, 3, 16)) == day(2026, 5, 15));
    // An end date.
    Recurrence ends{Repeat::Yearly, 1, day(2028, 12, 31)};
    CHECK(next_occurrence_day(day(2026, 6, 1), ends, day(2028, 1, 1)) == day(2028, 6, 1));
    CHECK(next_occurrence_day(day(2026, 6, 1), ends, day(2028, 6, 2)) == -1);
    // The far end of the calendar does not wrap.
    CHECK(next_occurrence_day(day(9999, 12, 30), yearly, day(9999, 12, 31)) == -1);
    CHECK(next_occurrence_day(day(1, 1, 1), Recurrence{Repeat::Daily, Recurrence::kMaxEvery, -1}, day(9999, 12, 31)) >= day(9999, 12, 31));
  }
  {
    // Birthdays and holidays in the store: all-day, yearly; found on their days, announced once a year, kept over a restart.
    filesystem::RamFileSystem files(a);
    const Date birthday{1990, 3, 14};
    uint32_t id = 0;
    {
      CalendarStore store(a, &files);
      id = store.add(local_minutes(birthday, 0, 0), 24 * 60, 0, "Ada's birthday", Recurrence{Repeat::Yearly, 1, -1});
      const uint32_t standup = store.add(local_minutes({2026, 3, 14}, 9, 0), 15, 0, "Stand-up");
      CHECK(id && standup);
      CalendarStore::Event e(a);
      CHECK(store.get(id, &e) && e.all_day() && e.repeats() && e.recurrence.kind == Repeat::Yearly);
      List<uint32_t> ids(a);
      store.ids_on(gui::days_from_date({2026, 3, 14}), &ids);
      CHECK(ids.count() == 2 && ids[0] == id && ids[1] == standup);   // (all day sorts first)
      CHECK(store.has_events_on(gui::days_from_date({2031, 3, 14})) && !store.has_events_on(gui::days_from_date({2031, 3, 15})) && !store.has_events_on(gui::days_from_date({1989, 3, 14})));
      CHECK(!store.add(local_minutes(birthday, 0, 0), 0, 0, "x", Recurrence{Repeat::Yearly, 0, -1}));                                 // every 0
      CHECK(!store.add(local_minutes(birthday, 0, 0), 0, 0, "x", Recurrence{Repeat::Yearly, 1, gui::days_from_date(birthday) - 1}));   // ends before it starts
      CHECK(!store.add(local_minutes(birthday, 0, 0), 0, 0, "x", Recurrence{static_cast<Repeat>(9), 1, -1}));

      List<CalendarStore::Due> due(a);
      store.take_due(local_minutes({2026, 3, 13}, 23, 59), &due);
      CHECK(due.count() == 0);   // a reminder of 0 minutes: at midnight
      store.take_due(local_minutes({2026, 3, 14}, 0, 0), &due);
      CHECK(due.count() == 1 && due[0].id == id && due[0].start == local_minutes({2026, 3, 14}, 0, 0));
      due.clear();
      store.take_due(local_minutes({2026, 3, 14}, 12, 0), &due);
      CHECK(due.count() == 0);   // once for this year
    }
    {
      CalendarStore store(a, &files);   // restart: this year's is not announced again, next year's is
      CalendarStore::Event e(a);
      CHECK(store.get(id, &e) && e.recurrence.kind == Repeat::Yearly && e.recurrence.every == 1 && e.recurrence.until_day == -1 && e.all_day());
      List<CalendarStore::Due> due(a);
      store.take_due(local_minutes({2026, 3, 14}, 13, 0), &due);
      CHECK(due.count() == 0);
      store.take_due(local_minutes({2027, 3, 14}, 0, 5), &due);
      CHECK(due.count() == 1 && due[0].start == local_minutes({2027, 3, 14}, 0, 0));
      // Weeks later it is the 2028 one that is next, and a machine that was off over a whole one does not announce what is past.
      due.clear();
      store.take_due(local_minutes({2028, 3, 15}, 8, 0), &due);
      CHECK(due.count() == 0);
      // Changing the recurrence takes effect, and the appointment is announced again.
      CHECK(store.update(id, local_minutes(birthday, 0, 0), 24 * 60, 0, "Ada's birthday", Recurrence{Repeat::Yearly, 1, gui::days_from_date({2030, 3, 14})}));
      store.take_due(local_minutes({2029, 3, 14}, 0, 0), &due);
      CHECK(due.count() == 1);
      due.clear();
      store.take_due(local_minutes({2031, 3, 14}, 0, 0), &due);
      CHECK(due.count() == 0);   // after the last day
    }
  }
  {
    // A calendar of the first version (calendar_events, no recurrence) is taken over.
    filesystem::RamFileSystem files(a);
    {
      database::local::LocalDatabase db(a, &files, filesystem::layout::kCalendarDatabase);
      db.open();
      database::local::LocalConnection connection(a, db);
      CHECK(connection.execute("CREATE TABLE calendar_events (event_id INTEGER PRIMARY KEY, starts_at INTEGER NOT NULL, minutes INTEGER, remind_before INTEGER, notified INTEGER, title TEXT NOT NULL)", nullptr) == database::DbStatus::Ok);
      const database::DbParam one[] = {database::DbParam::from_int(7), database::DbParam::from_int(local_minutes(kDay, 10, 0)), database::DbParam::from_int(30), database::DbParam::from_int(5),
                                       database::DbParam::from_int(1), database::DbParam::from_text("Old appointment")};
      CHECK(connection.execute("INSERT INTO calendar_events VALUES (?, ?, ?, ?, ?, ?)", ArrayView<const database::DbParam>(one, 6), nullptr) == database::DbStatus::Ok);
    }
    for (int round = 0; round < 2; ++round) {   // (the second start reads the new table only)
      CalendarStore store(a, &files);
      CalendarStore::Event e(a);
      CHECK(store.count() == 1 + round && store.get(7, &e) && e.title.view().equals("Old appointment") && e.minutes == 30 && e.remind_before == 5 && e.last_notified >= 0 && !e.repeats());
      if (round == 0) CHECK(store.add(local_minutes(kDay, 11, 0), 0, 0, "New") == 8);   // (numbers go on after the old ones)
    }
  }
  {
    // Holidays that move: the 4th Thursday of November, the last Monday of May, the 1st Monday of every month.
    auto day = [](int32_t y, int32_t m, int32_t d) { return gui::days_from_date({y, m, d}); };
    Recurrence thanksgiving, memorial, first_monday, fifth_friday;
    thanksgiving.kind = Repeat::Yearly;
    thanksgiving.on = On::NthWeekday;
    memorial.kind = Repeat::Yearly;
    memorial.on = On::LastWeekday;
    first_monday.kind = Repeat::Monthly;
    first_monday.on = On::NthWeekday;
    fifth_friday = first_monday;
    CHECK(recurrence_fits(day(2026, 11, 26), thanksgiving));
    CHECK(recurrence_fits(day(2026, 5, 25), memorial) && !recurrence_fits(day(2026, 5, 18), memorial));                    // not the last Monday
    CHECK(!recurrence_fits(day(2026, 5, 25), Recurrence{Repeat::Weekly, 1, -1, On::LastWeekday}));                         // only months and years
    int found = 0;
    for (int32_t year = 2027; year <= 2090; ++year) {
      const int64_t t = next_occurrence_day(day(2026, 11, 26), thanksgiving, day(year, 1, 1));
      const gui::Date d = gui::date_from_days(t);
      CHECK(d.year == year && d.month == 11 && gui::weekday(d) == 4 && (d.day - 1) / 7 == 3);   // Thursday, the 4th
      const gui::Date m = gui::date_from_days(next_occurrence_day(day(2026, 5, 25), memorial, day(year, 1, 1)));
      CHECK(m.year == year && m.month == 5 && gui::weekday(m) == 1 && m.day + 7 > gui::days_in_month(year, 5));   // Monday, the last
      found += d.year == year && m.year == year;
    }
    CHECK(found == 64);
    CHECK(gui::date_from_days(next_occurrence_day(day(2026, 11, 26), thanksgiving, day(2027, 1, 1))) == Date({2027, 11, 25}));
    CHECK(gui::date_from_days(next_occurrence_day(day(2026, 11, 26), thanksgiving, day(2028, 1, 1))) == Date({2028, 11, 23}));
    CHECK(gui::date_from_days(next_occurrence_day(day(2026, 5, 25), memorial, day(2027, 1, 1))) == Date({2027, 5, 31}));
    // Every month's first Monday (2026-10-05 is one); the days between are skipped.
    CHECK(gui::date_from_days(next_occurrence_day(day(2026, 10, 5), first_monday, day(2026, 10, 6))) == Date({2026, 11, 2}));
    CHECK(gui::date_from_days(next_occurrence_day(day(2026, 10, 5), first_monday, day(2026, 12, 1))) == Date({2026, 12, 7}));
    // A 5th Friday is the last one: July 31st 2026 -> August 28th.
    CHECK(recurrence_fits(day(2026, 7, 31), fifth_friday));
    CHECK(gui::date_from_days(next_occurrence_day(day(2026, 7, 31), fifth_friday, day(2026, 8, 1))) == Date({2026, 8, 28}));

    filesystem::RamFileSystem files(a);
    uint32_t id = 0;
    {
      CalendarStore store(a, &files);
      CHECK(store.add(local_minutes({2026, 5, 18}, 0, 0), 1440, 0, "Not the last Monday", memorial) == 0);   // refused
      id = store.add(local_minutes({2026, 11, 26}, 0, 0), 1440, 0, "Thanksgiving", thanksgiving);
      CHECK(id != 0 && store.has_events_on(day(2027, 11, 25)) && !store.has_events_on(day(2027, 11, 26)));
    }
    {
      CalendarStore store(a, &files);   // (kept as it was saved)
      CalendarStore::Event e(a);
      CHECK(store.get(id, &e) && e.recurrence.kind == Repeat::Yearly && e.recurrence.on == On::NthWeekday && e.all_day());
      CHECK(store.has_events_on(day(2028, 11, 23)));
      List<CalendarStore::Due> due(a);
      store.take_due(local_minutes({2027, 11, 25}, 0, 0), &due);
      CHECK(due.count() == 1 && due[0].start == local_minutes({2027, 11, 25}, 0, 0) && due[0].all_day() && due[0].title.view().equals("Thanksgiving"));
    }
  }
  {
    // One occurrence at a time: moved, retitled, cancelled, brought back.
    auto day = [](int32_t y, int32_t m, int32_t d) { return gui::days_from_date({y, m, d}); };
    filesystem::RamFileSystem files(a);
    uint32_t id = 0;
    const Recurrence weekly{Repeat::Weekly, 1, -1};
    {
      CalendarStore store(a, &files);
      id = store.add(local_minutes({2026, 10, 27}, 18, 0), 60, 10, "Class", weekly);
      CHECK(id && store.exception_count() == 0);
      CHECK(!store.change_occurrence(id, day(2026, 10, 28), local_minutes({2026, 10, 28}, 9, 0), 30, "x"));   // not a day of the rule
      CHECK(!store.change_occurrence(999, day(2026, 11, 3), local_minutes({2026, 11, 3}, 9, 0), 30, "x"));
      CHECK(!store.change_occurrence(id, day(2026, 11, 3), local_minutes({2026, 11, 3}, 9, 0), 30, ""));
      CHECK(store.change_occurrence(id, day(2026, 11, 3), local_minutes({2026, 11, 4}, 19, 0), 90, "Class (moved)"));
      CHECK(store.exception_count() == 1);

      List<CalendarStore::Occurrence> list(a);
      store.occurrences_on(day(2026, 11, 3), &list);
      CHECK(list.count() == 0 && !store.has_events_on(day(2026, 11, 3)));   // moved away
      store.occurrences_on(day(2026, 11, 4), &list);
      CHECK(list.count() == 1 && list[0].id == id && list[0].changed && list[0].original_day == day(2026, 11, 3) && list[0].start == local_minutes({2026, 11, 4}, 19, 0) &&
            list[0].minutes == 90 && list[0].title.view().equals("Class (moved)") && store.has_events_on(day(2026, 11, 4)));
      list.clear();
      store.occurrences_on(day(2026, 11, 10), &list);
      CHECK(list.count() == 1 && !list[0].changed && list[0].title.view().equals("Class") && list[0].start == local_minutes({2026, 11, 10}, 18, 0));

      CHECK(store.cancel_occurrence(id, day(2026, 11, 10)) && !store.has_events_on(day(2026, 11, 10)) && store.has_events_on(day(2026, 11, 17)));
      CHECK(!store.cancel_occurrence(id, day(2026, 11, 11)));
      CHECK(store.exception_count() == 2);
      CHECK(store.change_occurrence(id, day(2026, 11, 3), local_minutes({2026, 11, 3}, 20, 0), 90, "Class (again)"));   // a change can be changed
      CHECK(store.exception_count() == 2);
    }
    {
      CalendarStore store(a, &files);   // the restart keeps them
      CHECK(store.exception_count() == 2 && !store.has_events_on(day(2026, 11, 10)) && store.has_events_on(day(2026, 11, 3)));
      List<CalendarStore::Occurrence> list(a);
      store.occurrences_on(day(2026, 11, 3), &list);
      CHECK(list.count() == 1 && list[0].title.view().equals("Class (again)") && list[0].start == local_minutes({2026, 11, 3}, 20, 0));

      // Reminders: the series skips what has an exception; a changed occurrence has its own.
      List<CalendarStore::Due> due(a);
      store.take_due(local_minutes({2026, 11, 3}, 17, 50), &due);
      CHECK(due.count() == 0);   // 18:00 was moved to 20:00
      store.take_due(local_minutes({2026, 11, 3}, 19, 50), &due);
      CHECK(due.count() == 1 && due[0].start == local_minutes({2026, 11, 3}, 20, 0) && due[0].minutes == 90 && due[0].title.view().equals("Class (again)"));
      due.clear();
      store.take_due(local_minutes({2026, 11, 3}, 19, 55), &due);
      CHECK(due.count() == 0);   // once
      store.take_due(local_minutes({2026, 11, 10}, 17, 50), &due);
      CHECK(due.count() == 0);   // cancelled
      store.take_due(local_minutes({2026, 11, 17}, 17, 50), &due);
      CHECK(due.count() == 1 && due[0].start == local_minutes({2026, 11, 17}, 18, 0) && due[0].title.view().equals("Class"));
    }
    {
      CalendarStore store(a, &files);
      CHECK(store.exception_count() == 2);
      List<CalendarStore::Due> due(a);
      store.take_due(local_minutes({2026, 11, 3}, 19, 58), &due);
      CHECK(due.count() == 0);   // the restart did not announce the changed one again
      CHECK(store.restore_occurrence(id, day(2026, 11, 10)) && store.has_events_on(day(2026, 11, 10)) && !store.restore_occurrence(id, day(2026, 11, 10)));
      CHECK(store.exception_count() == 1);

      // Changing only the title or time of the series keeps the exceptions; changing the days drops them.
      CHECK(store.update(id, local_minutes({2026, 10, 27}, 17, 0), 60, 10, "Course", weekly) && store.exception_count() == 1);
      CHECK(store.update(id, local_minutes({2026, 10, 28}, 17, 0), 60, 10, "Course", weekly) && store.exception_count() == 0 && store.has_events_on(day(2026, 11, 4)));
      CHECK(store.change_occurrence(id, day(2026, 11, 4), local_minutes({2026, 11, 4}, 12, 0), 30, "Lunch"));
      CHECK(store.remove(id) && store.exception_count() == 0 && !store.has_events_on(day(2026, 11, 4)));
    }
    {
      CalendarStore store(a, &files);
      CHECK(store.count() == 0 && store.exception_count() == 0);
    }
  }
  CHECK(a.live == 0);
  if (g_failed) {
    printf("CalendarStoreTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("CalendarStoreTest: all passed\n");
  return 0;
}
