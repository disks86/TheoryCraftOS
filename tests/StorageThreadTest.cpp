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

// Host-side concurrency tests for the browser's shared storage: a StorageArea, a StorageHub and a PageHub used at the same time by the
// pages of a tab, which run on threads of their own. What must not happen (checked here with real threads, under the sanitizers): a
// thread reading the storage that another thread is changing, a duplicated origin when two pages ask for the first time at once, an
// event delivered twice (or not at all) while the pages post to the same hub, and heap corruption anywhere.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>
#include "Browser/PageHub.hpp"
#include "Kernel/Sync.hpp"

using namespace browser;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// malloc is thread-safe: every thread of the test allocates from it.
struct MallocAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return malloc(size); }
  void deallocate(void* p) override { free(p); }
};

namespace {

// --- Several pages hammering the storage area of one origin at the same time --------------------------------------

struct AreaHammer {
  StorageArea* area = nullptr;
  uint32_t self = 0;
  int rounds = 0;
};

void area_worker(void* argument) {
  AreaHammer& h = *static_cast<AreaHammer*>(argument);
  MallocAllocator a;
  for (int r = 0; r < h.rounds; ++r) {
    String own_key(a), own_value(a), got(a);
    own_key.assign("k");
    own_key.append_uint(h.self);
    own_value.assign("t");
    own_value.append_uint(h.self);
    own_value.append("-r");
    if (!h.area->set(own_key.view(), own_value.view())) { ++g_failed; continue; }
    if (!h.area->get(own_key.view(), &got) || !got.equals(own_value.view())) ++g_failed;   // (a read of what we just wrote must agree)
    String shared(a), shared_got(a);
    shared.assign("shared-");
    shared.append_uint(r);
    if (!h.area->set(shared.view(), "v")) ++g_failed;
    if (!h.area->get(shared.view(), &shared_got) || !shared_got.equals("v")) ++g_failed;
    // (enumerating while the other threads write: every key seen must read back - the enumeration need not be a snapshot)
    for (uint32_t i = 0; i < h.area->count(); ++i) {
      String k = h.area->key(i);
      if (k.empty()) { ++g_failed; break; }
      String again(a);
      if (h.area->get(k.view(), &again) && again.empty()) ++g_failed;
    }
  }
}

// --- Two (or more) pages asking for the first time at the same time -----------------------------------------------

struct HubContend {
  StorageHub* hub = nullptr;
  uint32_t self = 0;
};

void hub_worker(void* argument) {
  HubContend& h = *static_cast<HubContend*>(argument);
  MallocAllocator a;
  String origin(a);
  origin.assign("https://o");
  origin.append_uint(h.self % 4);   // (fewer origins than threads: the same origin is asked for by several at once)
  for (int r = 0; r < 20; ++r) h.hub->area(origin.view()).set("k", "v");
}

// --- The pages' events, posted and read on different threads ----------------------------------------------------------

std::vector<std::string> g_received;   // (appended by one reader thread only; read by main after the join)

struct EventWriter {
  PageHub* hub = nullptr;
  uint32_t page = 0;
  int own = 0;
  int cross = 0;
  volatile int done = 0;
};

void writer_worker(void* argument) {
  EventWriter& w = *static_cast<EventWriter*>(argument);
  MallocAllocator a;
  String origin(a), value(a);
  origin.assign("https://x");
  for (int i = 0; i < w.own; ++i) {
    HubEvent e(a);
    e.kind = HubEvent::Kind::Broadcast;
    e.source = w.page;
    e.origin.assign(origin.view());
    e.name.assign("channel");
    value.assign("n");
    value.append_uint(i);
    e.first.assign(value.view());
    w.hub->post(util::move(e));
  }
  for (int i = 0; i < w.cross; ++i) {   // (same channel, another origin: this reader must never hear it)
    HubEvent e(a);
    e.kind = HubEvent::Kind::Broadcast;
    e.source = w.page;
    e.origin.assign("https://other");
    e.name.assign("channel");
    e.first.assign("y");
    w.hub->post(util::move(e));
  }
  w.done = 1;
}

struct EventReader {
  PageHub* hub = nullptr;
  uint32_t page = 0;
  EventWriter* writer = nullptr;
};

void reader_worker(void* argument) {
  EventReader& r = *static_cast<EventReader*>(argument);
  MallocAllocator a;
  String origin(a);
  origin.assign("https://x");
  uint64_t seen = r.hub->serial();
  HubEvent my_own(a);
  my_own.kind = HubEvent::Kind::Broadcast;
  my_own.source = r.page;
  my_own.origin.assign("https://x");
  my_own.name.assign("channel");
  my_own.first.assign("self");
  r.hub->post(util::move(my_own));   // (what a page posts for itself: no page hears its own event)
  for (;;) {
    HubEvent got(a);
    if (r.hub->next(r.page, origin.view(), &seen, &got)) {
      if (got.first.view().equals("self")) ++g_failed;   // (it is its own: next() must not hand it back)
      else g_received.push_back(std::string(got.first.view().data(), got.first.view().size()));
      continue;
    }
    if (r.writer->done && !r.hub->pending(r.page, origin.view(), seen)) return;
    kernel::sleep_us(50);
  }
}

}  // namespace

int main(int, char**) {
  {   // the shared area of one origin, hammered by four pages
    MallocAllocator a;
    StorageHub hub(a);
    StorageArea& area = hub.area("https://x");
    const int kThreads = 4, kRounds = 30;
    AreaHammer h[kThreads];
    kernel::Thread* t[kThreads] = {};
    for (int i = 0; i < kThreads; ++i) {
      h[i].area = &area;
      h[i].self = static_cast<uint32_t>(i);
      h[i].rounds = kRounds;
      t[i] = kernel::spawn(area_worker, &h[i], "storage");
    }
    for (int i = 0; i < kThreads; ++i) kernel::join(t[i]);
    CHECK(hub.origin_count() == 1);
    CHECK(area.count() == kThreads + kRounds);   // (none was lost: the threads' keys and the shared ones)
    for (int i = 0; i < kThreads; ++i) {
      String key(a), got(a);
      key.assign("k");
      key.append_uint(i);
      const std::string prefix = "t" + std::to_string(i);
      CHECK(area.get(key.view(), &got) && got.view().size() >= prefix.size() && memcmp(got.view().data(), prefix.data(), prefix.size()) == 0);
    }
    for (int r = 0; r < kRounds; ++r) {
      String key(a), got(a);
      key.assign("shared-");
      key.append_uint(r);
      CHECK(area.get(key.view(), &got) && got.equals("v"));
    }
  }
  {   // the same origin made by several pages at the same time
    MallocAllocator a;
    StorageHub hub(a);
    const int kThreads = 8;
    HubContend h[kThreads];
    kernel::Thread* t[kThreads] = {};
    for (int i = 0; i < kThreads; ++i) {
      h[i].hub = &hub;
      h[i].self = static_cast<uint32_t>(i);
      t[i] = kernel::spawn(hub_worker, &h[i], "storage");
    }
    for (int i = 0; i < kThreads; ++i) kernel::join(t[i]);
    CHECK(hub.origin_count() == 4);   // (not 8: a concurrent first ask made one origin, not several)
    for (uint32_t i = 0; i < hub.origin_count(); ++i) {
      String value(a);
      CHECK(hub.area_at(i).count() == 1 && hub.area_at(i).get("k", &value) && value.equals("v"));
    }
  }
  {   // the events of the hub: posted by one page, read by the other, in order, once each
    MallocAllocator a;
    PageHub hub(a);
    const uint32_t writer_page = hub.add_page(), reader_page = hub.add_page();
    const uint64_t before = hub.serial();
    EventWriter w;
    w.hub = &hub;
    w.page = writer_page;
    w.own = 50;
    w.cross = 30;
    EventReader r;
    r.hub = &hub;
    r.page = reader_page;
    r.writer = &w;
    kernel::Thread* tw = kernel::spawn(writer_worker, &w, "storage");
    kernel::Thread* tr = kernel::spawn(reader_worker, &r, "storage");
    kernel::join(tr);
    kernel::join(tw);
    CHECK(hub.serial() >= before + 50 + 30 + 1);
    std::vector<std::string> expected;
    for (int i = 0; i < 50; ++i) expected.push_back("n" + std::to_string(i));
    std::sort(expected.begin(), expected.end());
    auto got = g_received;
    std::sort(got.begin(), got.end());
    got.erase(std::unique(got.begin(), got.end()), got.end());
    CHECK(got == expected);   // (all of them, once each, and nothing of the other origin or of its own)
  }
  if (g_failed) {
    printf("storage-thread tests FAILED (%d)\n", g_failed);
    return 1;
  }
  printf("storage-thread tests passed\n");
  return 0;
}
