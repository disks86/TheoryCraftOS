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
      CHECK(store.get(first, &e) && e.title.view().equals("Dentist") && e.minutes == 45 && e.remind_before == 10 && !e.notified);
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
      List<uint32_t> due(a);
      store.take_due(local_minutes(kDay, 14, 19), &due);   // the early one is over: marked, not announced; the dentist is 11 minutes away
      CHECK(due.count() == 0);
      store.take_due(local_minutes(kDay, 14, 20), &due);
      CHECK(due.count() == 1 && due[0] == id);
      due.clear();
      store.take_due(local_minutes(kDay, 14, 21), &due);
      CHECK(due.count() == 0);   // once
      CalendarStore::Event e(a);
      CHECK(store.get(id, &e) && e.notified);

      // Changing it announces it again.
      CHECK(store.update(id, local_minutes(kDay, 15, 0), 30, 5, "Dentist (moved)"));
      CHECK(store.get(id, &e) && !e.notified && e.start == local_minutes(kDay, 15, 0) && e.title.view().equals("Dentist (moved)"));
      CHECK(!store.update(999, 0, 0, 0, "x") && !store.update(id, 0, 0, 0, ""));
      store.take_due(local_minutes(kDay, 14, 54), &due);
      CHECK(due.count() == 0);
      store.take_due(local_minutes(kDay, 14, 55), &due);
      CHECK(due.count() == 1 && due[0] == id);   // 15:00 minus 5 minutes
    }
    {
      CalendarStore store(a, &files);   // the restart
      CHECK(store.count() == 2);
      CalendarStore::Event e(a);
      CHECK(store.get(id, &e) && e.title.view().equals("Dentist (moved)") && e.start == local_minutes(kDay, 15, 0) && e.minutes == 30 && e.remind_before == 5);
      CHECK(store.get(ended, &e) && e.notified);
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
  CHECK(a.live == 0);
  if (g_failed) {
    printf("CalendarStoreTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("CalendarStoreTest: all passed\n");
  return 0;
}
