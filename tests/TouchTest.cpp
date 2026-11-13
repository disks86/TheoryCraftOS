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

// Touchscreens below the desktop: TouchTracker (frames of contacts into Down / Motion / Up, the first finger, cancelling, a full queue), LinuxMultitouch
// (the multitouch protocol as QEMU's virtio-multitouch sends it, single-touch devices, slots out of range, and random event streams that must never break
// the tracker's rules) and CompositeTouch (several devices, unique ids, pointer emulation reaching every device).
#include <stdio.h>
#include <random>
#include <set>
#include <vector>
#include "Hardware/Touch/CompositeTouch.hpp"
#include "Hardware/Touch/LinuxMultitouch.hpp"
#include "Hardware/Touch/TouchTracker.hpp"

using namespace hardware;

static int g_checks = 0, g_failed = 0;
#define CHECK(...)                                                     \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(__VA_ARGS__)) {                                              \
      printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__);          \
      ++g_failed;                                                      \
    }                                                                  \
  } while (0)

static std::vector<TouchEvent> drain(TouchTracker& t) {
  std::vector<TouchEvent> out;
  TouchEvent e;
  while (t.pop(&e)) out.push_back(e);
  return out;
}

static void frame(TouchTracker& t, std::initializer_list<TouchTracker::Contact> contacts) {
  std::vector<TouchTracker::Contact> v(contacts);
  t.frame(ArrayView<const TouchTracker::Contact>(v.data(), v.size()), 1000, 500);
}

static void test_tracker() {
  TouchTracker t;
  uint32_t x = 0, y = 0;
  CHECK(!t.primary(&x, &y));
  frame(t, {{7, 10, 20}});
  std::vector<TouchEvent> e = drain(t);
  CHECK(e.size() == 1 && e[0].phase == TouchPhase::Down && e[0].id == 7 && e[0].x == 10 && e[0].y == 20 && e[0].max_x == 1000 && e[0].max_y == 500);
  frame(t, {{7, 10, 20}});      // (nothing moved: nothing to say)
  CHECK(drain(t).empty());
  frame(t, {{7, 11, 20}, {9, 300, 400}});
  e = drain(t);
  CHECK(e.size() == 2 && e[0].phase == TouchPhase::Motion && e[0].id == 7 && e[0].x == 11 && e[1].phase == TouchPhase::Down && e[1].id == 9);
  CHECK(t.primary(&x, &y) && x == 11 && y == 20 && t.active_count() == 2);
  // the first lifts: Up with its last position, and the second is the first now
  frame(t, {{9, 301, 400}});
  e = drain(t);
  CHECK(e.size() == 2 && e[0].phase == TouchPhase::Up && e[0].id == 7 && e[0].x == 11 && e[1].phase == TouchPhase::Motion && e[1].id == 9);
  CHECK(t.primary(&x, &y) && x == 301);
  // an id twice in a frame: the first counts
  frame(t, {{9, 301, 400}, {9, 5, 5}});
  CHECK(drain(t).empty());
  // more than ten contacts: the rest are ignored
  std::vector<TouchTracker::Contact> many;
  for (uint32_t i = 0; i < 15; ++i) many.push_back({100 + i, i, i});
  t.frame(ArrayView<const TouchTracker::Contact>(many.data(), many.size()), 1000, 500);
  CHECK(t.active_count() == TouchTracker::kMaxContacts);
  e = drain(t);
  CHECK(e.size() == 1 + TouchTracker::kMaxContacts && e[0].phase == TouchPhase::Up && e[0].id == 9);
  // cancel: each contact
  t.cancel_all();
  e = drain(t);
  CHECK(e.size() == TouchTracker::kMaxContacts && e[0].phase == TouchPhase::Cancel && t.active_count() == 0);
  // a full queue: the oldest kept, the rest counted
  for (uint32_t i = 0; i < 40; ++i) {
    frame(t, {{1, i, 0}, {2, i, 1}});
  }
  CHECK(t.dropped() > 0 && drain(t).size() == TouchTracker::kQueue);
}

// What the QEMU multitouch device sends for a finger: slot, tracking id, position, then SYN_REPORT.
struct Feeder {
  LinuxMultitouch mt;
  TouchTracker t;
  bool ev(uint16_t type, uint16_t code, uint32_t value) { return mt.feed(type, code, value, t, 0x7FFF, 0x7FFF); }
  void syn() { ev(LinuxMultitouch::kEvSyn, LinuxMultitouch::kSynReport, 0); }
  void abs(uint16_t code, uint32_t value) { ev(LinuxMultitouch::kEvAbs, code, value); }
};

static void test_multitouch() {
  Feeder f;
  f.abs(LinuxMultitouch::kAbsMtSlot, 0);
  f.abs(LinuxMultitouch::kAbsMtTrackingId, 5);
  f.abs(LinuxMultitouch::kAbsMtPositionX, 100);
  f.abs(LinuxMultitouch::kAbsMtPositionY, 200);
  CHECK(drain(f.t).empty());      // (not before the frame closes)
  f.syn();
  std::vector<TouchEvent> e = drain(f.t);
  CHECK(e.size() == 1 && e[0].phase == TouchPhase::Down && e[0].id == 5 && e[0].x == 100 && e[0].y == 200 && e[0].max_x == 0x7FFF);
  // a second finger in slot 1, the first moves
  f.abs(LinuxMultitouch::kAbsMtSlot, 1);
  f.abs(LinuxMultitouch::kAbsMtTrackingId, 6);
  f.abs(LinuxMultitouch::kAbsMtPositionX, 900);
  f.abs(LinuxMultitouch::kAbsMtPositionY, 800);
  f.abs(LinuxMultitouch::kAbsMtSlot, 0);
  f.abs(LinuxMultitouch::kAbsMtPositionX, 110);
  f.syn();
  e = drain(f.t);
  CHECK(e.size() == 2 && e[0].phase == TouchPhase::Motion && e[0].id == 5 && e[0].x == 110 && e[1].phase == TouchPhase::Down && e[1].id == 6 && e[1].x == 900);
  // the first lifts (tracking id -1)
  f.abs(LinuxMultitouch::kAbsMtTrackingId, 0xFFFFFFFFu);
  f.syn();
  e = drain(f.t);
  CHECK(e.size() == 1 && e[0].phase == TouchPhase::Up && e[0].id == 5);
  // a slot past ours: ignored, the others go on
  f.abs(LinuxMultitouch::kAbsMtSlot, 50);
  f.abs(LinuxMultitouch::kAbsMtTrackingId, 77);
  f.abs(LinuxMultitouch::kAbsMtPositionX, 1);
  f.syn();
  CHECK(drain(f.t).empty() && f.t.active_count() == 1);
  // a single-touch device: BTN_TOUCH and ABS_X / Y
  Feeder s;
  s.abs(LinuxMultitouch::kAbsX, 30);
  s.abs(LinuxMultitouch::kAbsY, 40);
  s.ev(LinuxMultitouch::kEvKey, LinuxMultitouch::kBtnTouch, 1);
  s.syn();
  e = drain(s.t);
  CHECK(e.size() == 1 && e[0].phase == TouchPhase::Down && e[0].x == 30 && e[0].y == 40);
  s.ev(LinuxMultitouch::kEvKey, LinuxMultitouch::kBtnTouch, 0);
  s.syn();
  e = drain(s.t);
  CHECK(e.size() == 1 && e[0].phase == TouchPhase::Up);
}

// Random event streams (and bit flips of a good one): the events always follow the rules - a Motion or Up only for a contact that is down, never a
// Down for one that is, never more than ten down.
static void test_multitouch_fuzz() {
  std::mt19937 rng(1234);
  const uint16_t codes[] = {LinuxMultitouch::kAbsMtSlot, LinuxMultitouch::kAbsMtTrackingId, LinuxMultitouch::kAbsMtPositionX, LinuxMultitouch::kAbsMtPositionY,
                            LinuxMultitouch::kAbsX, LinuxMultitouch::kAbsY, 0x2E, 0x3A, 0xFFFF};
  bool good = true;
  for (int run = 0; run < 200; ++run) {
    Feeder f;
    std::set<uint32_t> down;
    for (int i = 0; i < 2000; ++i) {
      const uint32_t pick = rng() % 10;
      if (pick < 6) {
        const uint16_t code = codes[rng() % (sizeof codes / sizeof codes[0])];
        const uint32_t value = (rng() % 4 == 0) ? rng() : rng() % 16;
        f.abs(code, value);
      } else if (pick == 6) {
        f.ev(LinuxMultitouch::kEvKey, (rng() % 2) ? LinuxMultitouch::kBtnTouch : static_cast<uint16_t>(rng()), rng() % 2);
      } else if (pick == 7) {
        f.ev(static_cast<uint16_t>(rng() % 8), static_cast<uint16_t>(rng()), rng());
      } else {
        f.syn();
      }
      TouchEvent e;
      while (f.t.pop(&e)) {
        if (e.phase == TouchPhase::Down) good &= down.insert(e.id).second;
        else if (e.phase == TouchPhase::Motion) good &= down.count(e.id) == 1;
        else good &= down.erase(e.id) == 1;      // (Up, Cancel)
      }
      good &= down.size() <= TouchTracker::kMaxContacts && f.t.active_count() == down.size();
    }
  }
  CHECK(good);
}

class FakeTouch final : public ITouch {
 public:
  std::vector<TouchEvent> queue;
  bool emulation = true;
  const char* name() const override { return "fake touch"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  bool poll(TouchEvent* out) override {
    if (queue.empty()) return false;
    *out = queue.front();
    queue.erase(queue.begin());
    return true;
  }
  void set_pointer_emulation(bool on) override { emulation = on; }
};

static void test_composite() {
  CompositeTouch all;
  FakeTouch a, b, c;
  CHECK(all.add(&a) && all.add(&b) && all.count() == 2);
  all.set_pointer_emulation(false);
  CHECK(!a.emulation && !b.emulation);
  CHECK(all.add(&c) && !c.emulation);      // (a device added later is told too)
  TouchEvent e;
  e.id = 3;
  a.queue.push_back(e);
  b.queue.push_back(e);
  std::set<uint32_t> ids;
  TouchEvent got;
  while (all.poll(&got)) ids.insert(got.id);
  CHECK(ids.size() == 2 && ids.count(3) == 1 && ids.count((1u << 24) | 3) == 1);      // (the same device id on two devices: two contacts)
  CHECK(all.remove(&a) && !all.remove(&a) && all.count() == 2);
  b.queue.push_back(e);
  CHECK(all.poll(&got) && got.id == ((1u << 24) | 3));      // (b keeps its slot: its ids do not change)
}

int main() {
  test_tracker();
  test_multitouch();
  test_multitouch_fuzz();
  test_composite();
  printf("touch: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
