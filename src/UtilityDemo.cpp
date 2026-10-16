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

#include "UtilityDemo.hpp"
#include "Utility/Optional.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"
#include "Utility/Variant.hpp"
#include "console.hpp"

namespace {
void report(const char* what, bool ok) {
  console::puts("  ");
  console::puts(what);
  console::puts(ok ? ": ok\n" : ": FAILED\n");
}

// A base with no virtual destructor, like the project's interfaces.
struct Shape {
  int sides = 0;
};
struct Tile {
  int unused[3] = {1, 2, 3};
};
int g_destroyed = 0;
struct Square : Tile, Shape {  // Shape sits at a non-zero offset inside Square
  Square() { sides = 4; }
  ~Square() { ++g_destroyed; }
};

struct Counter {
  uint32_t value = 0;
};

// What each task gets: the pointer to copy, and how many copies to make.
struct SharedJob {
  const SharedPointer<Counter>* shared;
  uint32_t repeats;
};

void shared_task(void* context) {
  auto* job = static_cast<SharedJob*>(context);
  for (uint32_t i = 0; i < job->repeats; ++i) {
    SharedPointer<Counter> mine = *job->shared;  // atomic increment ...
    __atomic_fetch_add(&mine->value, 1, __ATOMIC_RELAXED);
  }                                              // ... and decrement
}

struct Name {
  const char* text;
};
}  // namespace

bool run_utility_demo(allocators::Allocator& allocator, scheduling::ICPUScheduler& scheduler) {
  const uint64_t free_before = allocator.free_bytes();
  bool all_ok = true;

  {
    UniquePointer<Shape> shape = UniquePointer<Square>::make(allocator);  // ownership as a base
    bool ok = shape && shape->sides == 4;
    UniquePointer<Shape> moved = util::move(shape);
    ok &= !shape && moved && g_destroyed == 0;
    moved.reset();  // runs Square's destructor although Shape has none
    ok &= g_destroyed == 1 && !moved;
    report("unique pointer (derived object owned as base)", ok);
    all_ok &= ok;
  }
  {
    SharedPointer<Counter> counter = SharedPointer<Counter>::make(allocator);
    constexpr uint32_t kTasks = 16, kRepeats = 2000;
    SharedJob jobs[kTasks];
    bool ok = counter && counter.use_count() == 1;
    for (SharedJob& job : jobs) {
      job = {&counter, kRepeats};
      ok &= scheduler.submit(shared_task, &job);
    }
    scheduler.run();  // tasks run on every core and copy the pointer concurrently
    ok &= counter->value == kTasks * kRepeats && counter.use_count() == 1;
    report("shared pointer (16 tasks copying it across cores)", ok);
    all_ok &= ok;
  }
  {
    Optional<Name> name;
    bool ok = !name && name.value_or(Name{"anonymous"}).text[0] == 'a';
    name = Name{"core0"};
    ok &= name && name->text[4] == '0';
    name.reset();
    ok &= !name.has_value();

    Variant<int, float, Name> value(7);
    ok &= value.holds<int>() && *value.get<int>() == 7 && !value.get<float>();
    value = 2.5f;
    ok &= value.index() == 1;
    value.emplace<Name>(Name{"boot"});
    const int length = value.visit([](auto& x) -> int {
      if constexpr (util::kSame<util::Decay<decltype(x)>, Name>) return 4;
      else return -1;
    });
    ok &= length == 4;
    report("optional and variant", ok);
    all_ok &= ok;
  }

  const bool freed = allocator.free_bytes() == free_before;
  report("no memory left allocated", freed);
  return all_ok && freed;
}
