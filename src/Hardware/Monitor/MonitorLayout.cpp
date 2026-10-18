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
  slots_[static_cast<uint64_t>(i)].enabled = enabled;
  return true;
}

bool MonitorLayout::resize(uint32_t id, uint32_t width, uint32_t height) {
  const int32_t i = find(id);
  if (i < 0 || width == 0 || height == 0) return false;
  slots_[static_cast<uint64_t>(i)].rect.width = width;
  slots_[static_cast<uint64_t>(i)].rect.height = height;
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

}  // namespace hardware
