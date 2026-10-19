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

#include "Hardware/Monitor/MonitorLayout.hpp"

namespace hardware {

namespace {
uint64_t distance2(const MonitorRect& r, int32_t x, int32_t y) {
  const int32_t cx = x < r.x ? r.x : x >= r.right() ? r.right() - 1 : x;
  const int32_t cy = y < r.y ? r.y : y >= r.bottom() ? r.bottom() - 1 : y;
  const int64_t dx = int64_t{x} - cx, dy = int64_t{y} - cy;
  return static_cast<uint64_t>(dx * dx + dy * dy);
}
}  // namespace

int32_t MonitorLayout::find(uint32_t id) const {
  for (uint64_t i = 0; i < slots_.count(); ++i)
    if (slots_[i].id == id) return static_cast<int32_t>(i);
  return -1;
}

uint32_t MonitorLayout::enabled_count() const {
  uint32_t n = 0;
  for (uint64_t i = 0; i < slots_.count(); ++i) n += slots_[i].enabled;
  return n;
}

bool MonitorLayout::add(uint32_t id, uint32_t width, uint32_t height) {
  if (slots_.count() >= kMaxMonitors || find(id) >= 0 || width == 0 || height == 0) return false;
  MonitorSlot slot;
  slot.id = id;
  slot.rect = {slots_.count() ? bounds().right() : 0, 0, width, height};   // (to the right of the others)
  slot.physical_width = width;
  slot.physical_height = height;
  slot.primary = slots_.count() == 0;
  if (!slots_.push_back(slot)) return false;
  pack_row();
  return true;
}

bool MonitorLayout::remove(uint32_t id) {
  const int32_t i = find(id);
  if (i < 0) return false;
  const bool was_primary = slots_[static_cast<uint64_t>(i)].primary;
  slots_.remove_at(static_cast<uint64_t>(i));
  if (was_primary && slots_.count()) slots_[0].primary = true;
  return true;
}

const MonitorSlot* MonitorLayout::primary() const {
  const MonitorSlot* first_enabled = nullptr;
  for (uint64_t i = 0; i < slots_.count(); ++i) {
    if (slots_[i].primary && slots_[i].enabled) return &slots_[i];
    if (slots_[i].enabled && !first_enabled) first_enabled = &slots_[i];
  }
  if (first_enabled) return first_enabled;
  return slots_.count() ? &slots_[0] : nullptr;
}

bool MonitorLayout::set_primary(uint32_t id) {
  const int32_t i = find(id);
  if (i < 0) return false;
  for (uint64_t k = 0; k < slots_.count(); ++k) slots_[k].primary = static_cast<int32_t>(k) == i;
  return true;
}

bool MonitorLayout::set_enabled(uint32_t id, bool enabled) {
  const int32_t i = find(id);
  if (i < 0) return false;
  MonitorSlot& slot = slots_[static_cast<uint64_t>(i)];
  const bool was = slot.enabled;
  slot.enabled = enabled;
  if (enabled && !was && enabled_count() > 1) {   // back: its place may have been taken, or the others moved while it was away
    bool over = false;
    for (uint64_t k = 0; k < slots_.count(); ++k)
      if (static_cast<int32_t>(k) != i && slots_[k].enabled && slots_[k].rect.overlap(slot.rect) != 0) over = true;
    if (over || !connected()) {
      const int32_t x = slot.rect.x, y = slot.rect.y;
      if (!place_adjacent(id, x, y)) pack_row();
    }
  }
  return true;
}

bool MonitorLayout::resize(uint32_t id, uint32_t width, uint32_t height) {
  const int32_t i = find(id);
  if (i < 0 || width == 0 || height == 0) return false;
  MonitorSlot& slot = slots_[static_cast<uint64_t>(i)];
  slot.rect.width = width;
  slot.rect.height = height;
  slot.physical_width = width;   // (a plain resize is a mode of that size at 100 %)
  slot.physical_height = height;
  slot.scale_percent = 100;
  pack_row();
  return true;
}

// Orders the slots by their x position (ties: as they are), then lines them up.
void MonitorLayout::pack_row() {
  for (uint64_t i = 1; i < slots_.count(); ++i)   // (a stable insertion sort: layouts are tiny)
    for (uint64_t k = i; k > 0 && slots_[k].rect.x < slots_[k - 1].rect.x; --k) {
      const MonitorSlot t = slots_[k];
      slots_[k] = slots_[k - 1];
      slots_[k - 1] = t;
    }
  int32_t x = 0;
  for (uint64_t i = 0; i < slots_.count(); ++i) {
    slots_[i].rect.x = x;
    slots_[i].rect.y = 0;
    x += static_cast<int32_t>(slots_[i].rect.width);
  }
}

bool MonitorLayout::move_to_position(uint32_t id, uint32_t position) {
  const int32_t from = find(id);
  if (from < 0) return false;
  if (position >= slots_.count()) position = static_cast<uint32_t>(slots_.count() - 1);
  const MonitorSlot moving = slots_[static_cast<uint64_t>(from)];
  slots_.remove_at(static_cast<uint64_t>(from));
  if (!slots_.insert(position, moving)) return false;
  int32_t x = 0;
  for (uint64_t i = 0; i < slots_.count(); ++i) {   // (explicit order: repack without sorting by the old positions)
    slots_[i].rect.x = x;
    slots_[i].rect.y = 0;
    x += static_cast<int32_t>(slots_[i].rect.width);
  }
  return true;
}

bool MonitorLayout::set_position(uint32_t id, int32_t x, int32_t y) {
  const int32_t i = find(id);
  if (i < 0) return false;
  slots_[static_cast<uint64_t>(i)].rect.x = x;
  slots_[static_cast<uint64_t>(i)].rect.y = y;
  return true;
}

MonitorRect MonitorLayout::bounds() const {
  if (slots_.count() == 0) return MonitorRect();
  int32_t l = slots_[0].rect.x, t = slots_[0].rect.y, r = slots_[0].rect.right(), b = slots_[0].rect.bottom();
  for (uint64_t i = 1; i < slots_.count(); ++i) {
    const MonitorRect& m = slots_[i].rect;
    if (m.x < l) l = m.x;
    if (m.y < t) t = m.y;
    if (m.right() > r) r = m.right();
    if (m.bottom() > b) b = m.bottom();
  }
  return {l, t, static_cast<uint32_t>(r - l), static_cast<uint32_t>(b - t)};
}

void MonitorLayout::normalize() {
  const MonitorRect box = bounds();
  for (uint64_t i = 0; i < slots_.count(); ++i) {
    slots_[i].rect.x -= box.x;
    slots_[i].rect.y -= box.y;
  }
}

int32_t MonitorLayout::monitor_at(int32_t x, int32_t y) const {
  for (uint64_t i = 0; i < slots_.count(); ++i)
    if (slots_[i].enabled && slots_[i].rect.contains(x, y)) return static_cast<int32_t>(i);
  return -1;
}

int32_t MonitorLayout::nearest(int32_t x, int32_t y) const {
  int32_t best = -1;
  uint64_t best_distance = 0;
  for (uint64_t i = 0; i < slots_.count(); ++i) {
    if (!slots_[i].enabled) continue;
    const uint64_t d = distance2(slots_[i].rect, x, y);
    if (best < 0 || d < best_distance) {
      best = static_cast<int32_t>(i);
      best_distance = d;
    }
  }
  return best;
}

int32_t MonitorLayout::best_for(const MonitorRect& rect) const {
  int32_t best = -1;
  uint64_t best_overlap = 0;
  for (uint64_t i = 0; i < slots_.count(); ++i) {
    if (!slots_[i].enabled) continue;
    const uint64_t o = slots_[i].rect.overlap(rect);
    if (o > best_overlap) {
      best = static_cast<int32_t>(i);
      best_overlap = o;
    }
  }
  if (best >= 0) return best;
  return nearest(rect.x + static_cast<int32_t>(rect.width / 2), rect.y + static_cast<int32_t>(rect.height / 2));
}

bool MonitorLayout::clamp(int32_t* x, int32_t* y) const {
  const int32_t i = nearest(*x, *y);
  if (i < 0) return false;
  const MonitorRect& r = slots_[static_cast<uint64_t>(i)].rect;
  if (*x < r.x) *x = r.x;
  if (*x >= r.right()) *x = r.right() - 1;
  if (*y < r.y) *y = r.y;
  if (*y >= r.bottom()) *y = r.bottom() - 1;
  return true;
}

uint32_t MonitorLayout::logical_size(uint32_t pixels, uint32_t scale_percent) {
  const uint64_t size = uint64_t{pixels} * 100 / (scale_percent ? scale_percent : 100);
  return size ? static_cast<uint32_t>(size) : 1;
}

namespace {

bool rects_overlap(const MonitorRect& a, const MonitorRect& b) { return a.overlap(b) != 0; }

// Two rectangles that do not overlap touch if they share a piece of an edge.
bool rects_touch(const MonitorRect& a, const MonitorRect& b) {
  const bool beside = a.right() == b.x || b.right() == a.x, stacked = a.bottom() == b.y || b.bottom() == a.y;
  const int32_t v0 = a.y > b.y ? a.y : b.y, v1 = a.bottom() < b.bottom() ? a.bottom() : b.bottom();
  const int32_t h0 = a.x > b.x ? a.x : b.x, h1 = a.right() < b.right() ? a.right() : b.right();
  return (beside && v1 > v0) || (stacked && h1 > h0);
}

}  // namespace

bool MonitorLayout::set_mode(uint32_t id, uint32_t width, uint32_t height, uint32_t scale_percent) {
  const int32_t i = find(id);
  if (i < 0 || width == 0 || height == 0 || scale_percent < 100 || scale_percent > 400) return false;
  MonitorSlot& slot = slots_[static_cast<uint64_t>(i)];
  slot.physical_width = width;
  slot.physical_height = height;
  slot.scale_percent = scale_percent;
  slot.rect.width = logical_size(width, scale_percent);
  slot.rect.height = logical_size(height, scale_percent);
  // Grown over a neighbour: it goes to the nearest free place beside the others (which may be where it is not).
  bool over = false;
  for (uint64_t k = 0; k < slots_.count(); ++k)
    if (static_cast<int32_t>(k) != i && slots_[k].enabled && rects_overlap(slot.rect, slots_[k].rect)) over = true;
  if (over && enabled_count() > 1) {
    const int32_t x = slot.rect.x, y = slot.rect.y;
    if (!place_adjacent(id, x, y)) pack_row();
  }
  normalize();
  return true;
}

bool MonitorLayout::place_adjacent(uint32_t id, int32_t x, int32_t y) {
  const int32_t index = find(id);
  if (index < 0) return false;
  MonitorSlot& moving = slots_[static_cast<uint64_t>(index)];
  const int32_t w = static_cast<int32_t>(moving.rect.width), h = static_cast<int32_t>(moving.rect.height);
  uint32_t others = 0;
  for (uint64_t k = 0; k < slots_.count(); ++k) others += static_cast<int32_t>(k) != index && slots_[k].enabled;
  if (others == 0) {
    moving.rect.x = x;
    moving.rect.y = y;
    normalize();
    return true;
  }
  constexpr int32_t kSnap = 24;
  struct Candidate {
    int32_t x, y;
  };
  bool found = false;
  Candidate best{0, 0};
  int64_t best_cost = 0;
  for (uint64_t k = 0; k < slots_.count(); ++k) {
    if (static_cast<int32_t>(k) == index || !slots_[k].enabled) continue;
    const MonitorRect b = slots_[k].rect;
    for (int32_t side = 0; side < 4; ++side) {   // 0: left of it, 1: right of it, 2: above, 3: below
      Candidate c{0, 0};
      if (side < 2) {   // beside it: x is fixed, y slides along the edge (keeping at least a pixel of contact)
        c.x = side == 0 ? b.x - w : b.right();
        c.y = y < b.y - h + 1 ? b.y - h + 1 : y > b.bottom() - 1 ? b.bottom() - 1 : y;
      } else {          // above or below it: y is fixed, x slides
        c.y = side == 2 ? b.y - h : b.bottom();
        c.x = x < b.x - w + 1 ? b.x - w + 1 : x > b.right() - 1 ? b.right() - 1 : x;
      }
      // Lined up with the edges of the neighbour when close to them.
      Candidate snapped = c;
      if (side < 2) {
        if (c.y - b.y >= -kSnap && c.y - b.y <= kSnap) snapped.y = b.y;
        else if (c.y + h - b.bottom() >= -kSnap && c.y + h - b.bottom() <= kSnap) snapped.y = b.bottom() - h;
      } else {
        if (c.x - b.x >= -kSnap && c.x - b.x <= kSnap) snapped.x = b.x;
        else if (c.x + w - b.right() >= -kSnap && c.x + w - b.right() <= kSnap) snapped.x = b.right() - w;
      }
      for (int pass = 0; pass < 2; ++pass) {
        const Candidate& cand = pass == 0 ? snapped : c;
        const MonitorRect r{cand.x, cand.y, moving.rect.width, moving.rect.height};
        bool ok = true;
        for (uint64_t m = 0; m < slots_.count() && ok; ++m)
          if (static_cast<int32_t>(m) != index && slots_[m].enabled && rects_overlap(r, slots_[m].rect)) ok = false;
        if (!ok) continue;
        // All the monitors must still be one piece: walk from the first enabled one through touching rectangles.
        bool seen[kMaxMonitors] = {};
        uint32_t reached = 0, stack[kMaxMonitors], top = 0;
        for (uint64_t m = 0; m < slots_.count() && top == 0; ++m)
          if (slots_[m].enabled) {
            seen[m] = true;
            stack[top++] = static_cast<uint32_t>(m);
            ++reached;
          }
        while (top) {
          const uint32_t cur = stack[--top];
          const MonitorRect cr = static_cast<int32_t>(cur) == index ? r : slots_[cur].rect;
          for (uint64_t m = 0; m < slots_.count(); ++m) {
            if (seen[m] || !slots_[m].enabled) continue;
            const MonitorRect mr = static_cast<int32_t>(m) == index ? r : slots_[m].rect;
            if (!rects_touch(cr, mr)) continue;
            seen[m] = true;
            stack[top++] = static_cast<uint32_t>(m);
            ++reached;
          }
        }
        if (reached != enabled_count()) continue;
        const int64_t dx = int64_t{cand.x} - x, dy = int64_t{cand.y} - y;
        const int64_t cost = dx * dx + dy * dy;
        if (!found || cost < best_cost) {
          found = true;
          best = cand;
          best_cost = cost;
        }
        break;   // (the snapped one is preferred when it is possible)
      }
    }
  }
  if (!found) return false;
  moving.rect.x = best.x;
  moving.rect.y = best.y;
  normalize();
  return true;
}

bool MonitorLayout::assign(const MonitorSlot* slots, uint32_t count) {
  if (count > kMaxMonitors) return false;
  slots_.clear();
  for (uint32_t i = 0; i < count; ++i)
    if (!slots_.push_back(slots[i])) return false;
  return true;
}

bool MonitorLayout::connected() const {
  bool seen[kMaxMonitors] = {};
  uint32_t reached = 0, stack[kMaxMonitors], top = 0;
  for (uint64_t m = 0; m < slots_.count() && top == 0; ++m)
    if (slots_[m].enabled) {
      seen[m] = true;
      stack[top++] = static_cast<uint32_t>(m);
      ++reached;
    }
  while (top) {
    const uint32_t cur = stack[--top];
    for (uint64_t m = 0; m < slots_.count(); ++m) {
      if (seen[m] || !slots_[m].enabled || !rects_touch(slots_[cur].rect, slots_[m].rect)) continue;
      seen[m] = true;
      stack[top++] = static_cast<uint32_t>(m);
      ++reached;
    }
  }
  return reached == enabled_count();
}

// Puts the monitors that are enabled in a row (the others keep their numbers but their places mean nothing until they come back).
void MonitorLayout::repair() {
  if (connected()) return;
  int32_t x = 0;
  for (uint64_t i = 0; i < slots_.count(); ++i) {
    if (!slots_[i].enabled) continue;
    slots_[i].rect.x = x;
    slots_[i].rect.y = 0;
    x += static_cast<int32_t>(slots_[i].rect.width);
  }
  normalize();
}

}  // namespace hardware
