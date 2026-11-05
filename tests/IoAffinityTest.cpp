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

// Device-aware placement (CPUScheduler/IoAffinity): the scores and the choice of a core for a woken thread and for a device's interrupt, the
// learnt I/O core, the cache domain from CPUID, and the device table - single-threaded against expected values, then with threads adding,
// removing, counting and reading devices at once (built with the address sanitizer and, separately, ThreadSanitizer).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <thread>
#include <vector>
#include "CPUScheduler/IoAffinity.hpp"

using namespace scheduling;

void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  fprintf(stderr, "BOUNDS VIOLATION: %s (index %llu, size %llu)\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

namespace {

constexpr int8_t kPerformance = 0, kEfficiency = 1;

// Eight cores: 0-3 Performance sharing cache 0, 4-5 Performance sharing cache 1, 6-7 Efficiency sharing cache 2.
struct Machine {
  CorePlace cores[8];
  Machine() {
    for (uint32_t i = 0; i < 8; ++i) {
      cores[i].online = true;
      cores[i].idle = true;
      cores[i].kind = i < 6 ? kPerformance : kEfficiency;
      cores[i].cache_domain = i < 4 ? 0 : i < 6 ? 1 : 2;
    }
  }
  ArrayView<const CorePlace> view() const { return ArrayView<const CorePlace>(cores, 8); }
};

PlacementHints hints(int32_t io, int32_t last, int8_t kind = -1) {
  PlacementHints h;
  h.io_core = io;
  h.last_core = last;
  h.preferred_kind = kind;
  return h;
}

void test_scores() {
  Machine m;
  CHECK(placement_score(hints(-1, -1), 3, m.view()) == 0);
  CHECK(placement_score(hints(2, -1), 2, m.view()) == kScoreIoCore + kScoreCacheDomain);
  CHECK(placement_score(hints(2, -1), 1, m.view()) == kScoreCacheDomain);
  CHECK(placement_score(hints(2, -1), 4, m.view()) == 0);
  CHECK(placement_score(hints(-1, 5), 5, m.view()) == kScoreLastCore + kScoreCacheDomain);
  CHECK(placement_score(hints(-1, 5), 4, m.view()) == kScoreCacheDomain);
  CHECK(placement_score(hints(2, 5), 5, m.view()) == kScoreLastCore);   // (the domain counts from the device's core when there is one)
  CHECK(placement_score(hints(-1, -1, kEfficiency), 7, m.view()) == kScoreKind);
  CHECK(placement_score(hints(-1, -1, kEfficiency), 0, m.view()) == 0);
  // Out of range: no score, no crash.
  CHECK(placement_score(hints(2, -1), 8, m.view()) == 0);
  CHECK(placement_score(hints(500, 900), 0, m.view()) == 0);
  CHECK(placement_score(hints(2, -1), 2, ArrayView<const CorePlace>()) == 0);
}

void test_wake_core() {
  Machine m;
  const CoreSet all = CoreSet::all();
  // Nothing known: the lowest idle core.
  CHECK(choose_wake_core(hints(-1, -1), all, m.view()) == 0);
  // The device's core when idle.
  CHECK(choose_wake_core(hints(5, -1), all, m.view()) == 5);
  // Busy: the other core of its cache, ahead of lower-numbered cores elsewhere.
  m.cores[5].idle = false;
  CHECK(choose_wake_core(hints(5, -1), all, m.view()) == 4);
  m.cores[4].idle = false;
  CHECK(choose_wake_core(hints(5, -1), all, m.view()) == 0);
  m = Machine();
  // The warm cache with no device.
  CHECK(choose_wake_core(hints(-1, 3), all, m.view()) == 3);
  // The device's core beats the warm cache.
  CHECK(choose_wake_core(hints(6, 3), all, m.view()) == 6);
  // The kind asked for beats the device.
  CHECK(choose_wake_core(hints(6, -1, kPerformance), all, m.view()) == 0);
  CHECK(choose_wake_core(hints(1, -1, kEfficiency), all, m.view()) == 6);
  CHECK(choose_wake_core(hints(7, -1, kEfficiency), all, m.view()) == 7);
  // Only allowed cores.
  CoreSet only_two;
  only_two.add(2);
  CHECK(choose_wake_core(hints(5, 3, kEfficiency), only_two, m.view()) == 2);
  m.cores[2].idle = false;
  CHECK(choose_wake_core(hints(5, 3), only_two, m.view()) == kNoCore);
  // Offline cores are never chosen; nothing idle means no core.
  m = Machine();
  m.cores[5].online = false;
  CHECK(choose_wake_core(hints(5, -1), all, m.view()) == 4);
  for (CorePlace& c : m.cores) c.idle = false;
  CHECK(choose_wake_core(hints(5, -1), all, m.view()) == kNoCore);
  CHECK(choose_wake_core(hints(5, -1), all, ArrayView<const CorePlace>()) == kNoCore);
}

void test_interrupt_core() {
  Machine m;
  const CoreSet all = CoreSet::all();
  uint32_t load[8] = {};
  // Spreads devices over the cores, leaving the avoided one (the boot core) for last.
  for (uint32_t n = 0; n < 7; ++n) {
    const int32_t core = choose_interrupt_core(m.view(), ArrayView<const uint32_t>(load, 8), all, -1, 0);
    CHECK(core == static_cast<int32_t>(n + 1));
    if (core >= 0) ++load[core];
  }
  CHECK(choose_interrupt_core(m.view(), ArrayView<const uint32_t>(load, 8), all, -1, 0) == 1);   // all carry one: the lowest that is not avoided
  // A kind: only its cores, the least loaded.
  uint32_t fresh[8] = {};
  fresh[6] = 2;
  CHECK(choose_interrupt_core(m.view(), ArrayView<const uint32_t>(fresh, 8), all, kEfficiency, 0) == 7);
  // The kind's only core is the avoided one: still that kind.
  m.cores[7].online = false;
  CHECK(choose_interrupt_core(m.view(), ArrayView<const uint32_t>(fresh, 8), all, kEfficiency, 6) == 6);
  // No core of the kind online: any kind.
  m.cores[6].online = false;
  CHECK(choose_interrupt_core(m.view(), ArrayView<const uint32_t>(fresh, 8), all, kEfficiency, 0) == 1);
  // Only the avoided core is online.
  Machine one;
  for (uint32_t i = 1; i < 8; ++i) one.cores[i].online = false;
  CHECK(choose_interrupt_core(one.view(), ArrayView<const uint32_t>(), all, -1, 0) == 0);
  for (CorePlace& c : one.cores) c.online = false;
  CHECK(choose_interrupt_core(one.view(), ArrayView<const uint32_t>(), all, -1, 0) == kNoCore);
  // A short load array counts the rest as unloaded.
  Machine full;
  uint32_t short_load[2] = {5, 5};
  CHECK(choose_interrupt_core(full.view(), ArrayView<const uint32_t>(short_load, 2), all, -1, -1) == 2);
}

void test_learned_core() {
  LearnedIoCore learned;
  CHECK(learned.hint() == kNoCore);
  learned.woken_by_interrupt_on(3);
  CHECK(learned.hint() == 3 && learned.confidence == 1);
  learned.woken_by_interrupt_on(3);
  learned.woken_by_interrupt_on(3);
  learned.woken_by_interrupt_on(3);
  CHECK(learned.confidence == LearnedIoCore::kMaxConfidence);
  // A stray interrupt elsewhere does not move it; a run of them does.
  learned.woken_by_interrupt_on(5);
  CHECK(learned.hint() == 3);
  learned.woken_by_interrupt_on(5);
  CHECK(learned.hint() == 3 && learned.confidence == 1);
  learned.woken_by_interrupt_on(5);
  CHECK(learned.hint() == 5 && learned.confidence == 1);
  learned.forget();
  CHECK(learned.hint() == kNoCore);
}

void test_cache_domain() {
  CHECK(cache_domain_from_apic_id(13, 0) == 13);
  CHECK(cache_domain_from_apic_id(13, 1) == 13);
  CHECK(cache_domain_from_apic_id(13, 2) == 6);
  CHECK(cache_domain_from_apic_id(13, 6) == 1);    // six threads sharing: 3 bits
  CHECK(cache_domain_from_apic_id(13, 16) == 0);
  CHECK(cache_domain_from_apic_id(0xFFFFFFFFu, 0xFFFFFFFFu) == 1);   // (the shift stops at 31)
}

void test_device_table() {
  IoDeviceTable table;
  IoDeviceInfo info;
  CHECK(!table.info(0, &info));
  CHECK(table.interrupt_core(0) == kNoCore);
  CHECK(!table.remove(0) && !table.remove(-1) && !table.remove(64));
  CHECK(!table.set_interrupt_core(-5, 1));
  table.note_interrupt(-1);   // ignored
  table.note_wake(1000);

  const int32_t gpu = table.add("virtio-gpu", 2);
  const int32_t wifi = table.add("a name that is far too long to fit", 0);
  CHECK(gpu == 0 && wifi == 1);
  CHECK(table.interrupt_core(gpu) == 2);
  CHECK(table.info(wifi, &info) && strcmp(info.name, "a name that is far too ") == 0 && strlen(info.name) == 23);
  table.note_interrupt(gpu);
  table.note_interrupt(gpu);
  table.note_wake(gpu);
  CHECK(table.info(gpu, &info) && strcmp(info.name, "virtio-gpu") == 0 && info.interrupt_core == 2 && info.interrupts == 2 && info.wakes == 1);
  CHECK(table.set_interrupt_core(gpu, 5) && table.interrupt_core(gpu) == 5);

  uint32_t per_core[4] = {9, 9, 9, 9};
  table.devices_per_core(ArrayView<uint32_t>(per_core, 4));
  CHECK(per_core[0] == 1 && per_core[1] == 0 && per_core[2] == 0 && per_core[3] == 0);   // (core 5 is past the array)

  CHECK(table.remove(gpu));
  CHECK(!table.remove(gpu));
  CHECK(!table.live(gpu) && table.interrupt_core(gpu) == kNoCore && !table.info(gpu, &info));
  // The freed slot is reused, with fresh counters.
  CHECK(table.add("nvme", kNoCore) == gpu);
  CHECK(table.info(gpu, &info) && strcmp(info.name, "nvme") == 0 && info.interrupts == 0 && info.interrupt_core == kNoCore);
  CHECK(table.add("", 1) == 2 && table.info(2, &info) && info.name[0] == 0);

  // Full.
  IoDeviceTable full;
  for (uint32_t i = 0; i < IoDeviceTable::kCapacity; ++i) CHECK(full.add("d", 0) == static_cast<int32_t>(i));
  CHECK(full.add("one too many", 0) == kNoIoDevice);
  CHECK(full.remove(17) && full.add("again", 3) == 17);
}

// Writers add and remove devices whose name spells their core; readers check every copy they get is one device's, never a mix; counters are
// bumped throughout. ASan/TSan watch the rest.
void test_device_table_threads() {
  IoDeviceTable table;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> consistent{0}, mixed{0};
  std::atomic<uint64_t> noted{0};

  std::vector<std::thread> threads;
  for (uint32_t w = 0; w < 4; ++w) {
    threads.emplace_back([&table, w] {
      std::vector<int32_t> mine;
      for (uint32_t round = 0; round < 20000; ++round) {
        const int32_t core = static_cast<int32_t>((w * 7919 + round) % 100);
        char name[16];
        snprintf(name, sizeof name, "dev%d", core);
        const int32_t id = table.add(name, core);
        if (id >= 0) mine.push_back(id);
        if (mine.size() > 10 || (id < 0 && !mine.empty())) {
          CHECK(table.remove(mine.front()));
          mine.erase(mine.begin());
        }
      }
      for (int32_t id : mine) CHECK(table.remove(id));
    });
  }
  for (uint32_t r = 0; r < 3; ++r) {
    threads.emplace_back([&] {
      uint32_t per_core[128];
      while (!stop.load()) {
        for (int32_t id = 0; id < static_cast<int32_t>(IoDeviceTable::kCapacity); ++id) {
          IoDeviceInfo info;
          if (!table.info(id, &info)) continue;
          int core = -1;
          if (sscanf(info.name, "dev%d", &core) == 1 && core == info.interrupt_core) ++consistent;
          else ++mixed;
          table.note_interrupt(id);
          table.note_wake(id);
          ++noted;
          (void)table.interrupt_core(id);
        }
        table.devices_per_core(ArrayView<uint32_t>(per_core, 128));
      }
    });
  }
  for (uint32_t w = 0; w < 4; ++w) threads[w].join();
  stop.store(true);
  for (uint32_t i = 4; i < threads.size(); ++i) threads[i].join();

  CHECK(mixed.load() == 0);
  CHECK(consistent.load() > 0);
  for (int32_t id = 0; id < static_cast<int32_t>(IoDeviceTable::kCapacity); ++id) CHECK(!table.live(id));
  uint32_t per_core[128];
  table.devices_per_core(ArrayView<uint32_t>(per_core, 128));
  for (uint32_t count : per_core) CHECK(count == 0);
  printf("  %llu consistent reads, %llu counted\n", (unsigned long long)consistent.load(), (unsigned long long)noted.load());
}

}  // namespace

int main() {
  test_scores();
  test_wake_core();
  test_interrupt_core();
  test_learned_core();
  test_cache_domain();
  test_device_table();
  test_device_table_threads();
  if (g_failed) {
    printf("IoAffinityTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("IoAffinityTest: all passed\n");
  return 0;
}
