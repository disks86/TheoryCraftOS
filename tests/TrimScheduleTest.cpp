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

// The schedule of the automatic fstrim (FileSystem/TrimSchedule): not in the first minutes after start-up, then weekly by the wall clock with
// the last run kept across starts in its state file, and once per start when there is no clock (or it went backwards); the state file's
// format, and damaged or hostile contents read as "never".
#include <stdio.h>
#include <stdlib.h>
#include "FileSystem/TrimSchedule.hpp"

void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  fprintf(stderr, "BOUNDS VIOLATION: %s (index %llu, size %llu)\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

struct Allocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return size ? malloc(size) : nullptr; }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { free(p); }
};

using filesystem::TrimSchedule;
constexpr uint64_t kDay = 24 * 3600, kStart = 1700000000;

}  // namespace

int main() {
  Allocator a;
  {  // a clock: weekly, remembered across starts
    TrimSchedule s;
    CHECK(!s.due(kStart, 0) && !s.due(kStart, TrimSchedule::kFirstDelaySeconds - 1));
    CHECK(s.due(kStart, TrimSchedule::kFirstDelaySeconds));   // never trimmed: once the boot is over
    s.done(kStart);
    CHECK(!s.due(kStart + kDay, 7200) && !s.due(kStart + 6 * kDay, 7200));
    CHECK(s.due(kStart + 7 * kDay, 7200));
    String text(a);
    CHECK(s.save(&text) && text.view().equals("last 1700000000\n"));
    TrimSchedule next;   // the next start reads it
    next.load(text.view());
    CHECK(next.last_unix() == kStart);
    CHECK(!next.due(kStart + 3 * kDay, 3600));   // started again after three days: not yet
    CHECK(next.due(kStart + 8 * kDay, 3600));
    next.done(kStart + 8 * kDay);
    CHECK(!next.due(kStart + 8 * kDay + 60, 4000));
  }
  {  // no clock: once per start
    TrimSchedule s;
    CHECK(s.due(0, 700));
    s.done(0);
    CHECK(!s.due(0, 700) && !s.due(0, 100000) && s.last_unix() == 0);
  }
  {  // a clock that went back (a battery replaced): once per start, the remembered run kept
    TrimSchedule s;
    s.load("last 1800000000\n");
    CHECK(s.due(kStart, 700));
    s.done(kStart);
    CHECK(!s.due(kStart, 800) && s.last_unix() == kStart);
  }
  {  // contents that are not a state file: never trimmed
    const char* bad[] = {"", "last", "last ", "last x\n", "last 12x\n", "first 123\n", "last 99999999999999999999999\n", "last -5\n"};
    for (const char* text : bad) {
      TrimSchedule s;
      s.load("last 1700000000\n");
      s.load(text);
      CHECK(s.last_unix() == 0);
    }
    TrimSchedule s;
    s.load("last 42");   // (no newline at the end: fine)
    CHECK(s.last_unix() == 42);
  }
  if (g_failed) {
    printf("TrimScheduleTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("TrimScheduleTest: all passed\n");
  return 0;
}
