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

#include "Hardware/Monitor/MonitorDisplay.hpp"

namespace hardware {

MonitorDisplay::~MonitorDisplay() { release(); }

void MonitorDisplay::release() {
  if (maps_) allocator_.deallocate(maps_);
  if (row_) allocator_.deallocate(row_);
  maps_ = row_ = nullptr;
  row_capacity_ = 0;
  count_ = 0;
}

bool MonitorDisplay::update(const MonitorLayout& layout, const ScanoutPlacement* physical, uint32_t count) {
  Entry fresh[MonitorLayout::kMaxMonitors];
  uint32_t n = 0;
  uint64_t map_words = 0;
  uint32_t widest = 0;
  bool scaling = false;
  int32_t right = 0, bottom = 0;
  for (uint32_t i = 0; i < layout.count(); ++i) {
    const MonitorSlot& s = layout.at(i);
    if (!s.enabled) continue;
    const ScanoutPlacement* place = nullptr;
    for (uint32_t k = 0; k < count; ++k)
      if (physical[k].connector == s.id) place = &physical[k];
    if (!place || n >= MonitorLayout::kMaxMonitors || place->width == 0 || place->height == 0) return false;
    Entry& e = fresh[n++];
    e.lx = s.rect.x;
    e.ly = s.rect.y;
    e.lw = s.rect.width;
    e.lh = s.rect.height;
    e.px = place->x;
    e.py = place->y;
    e.pw = place->width;
    e.ph = place->height;
    e.scale = s.scale_percent ? s.scale_percent : 100;
    if (e.scale != 100 || e.lx != static_cast<int32_t>(e.px) || e.ly != static_cast<int32_t>(e.py) || e.lw != e.pw || e.lh != e.ph) scaling = true;
    if (e.scale != 100) map_words += uint64_t{e.pw} + e.ph;
    if (e.pw > widest) widest = e.pw;
    if (s.rect.right() > right) right = s.rect.right();
    if (s.rect.bottom() > bottom) bottom = s.rect.bottom();
  }
  if (n == 0) return false;
  uint32_t* maps = map_words ? static_cast<uint32_t*>(allocator_.allocate(map_words * sizeof(uint32_t))) : nullptr;
  uint32_t* row = static_cast<uint32_t*>(allocator_.allocate(uint64_t{widest} * sizeof(uint32_t)));
  if ((map_words && !maps) || !row) {
    if (maps) allocator_.deallocate(maps);
    if (row) allocator_.deallocate(row);
    return false;
  }
  uint32_t* next = maps;
  for (uint32_t i = 0; i < n; ++i) {
    Entry& e = fresh[i];
    if (e.scale == 100) continue;
    e.xmap = next;
    next += e.pw;
    e.ymap = next;
    next += e.ph;
    for (uint32_t x = 0; x < e.pw; ++x) e.xmap[x] = static_cast<uint32_t>(uint64_t{x} * 100 / e.scale);   // (the column the pixel shows)
    for (uint32_t y = 0; y < e.ph; ++y) e.ymap[y] = static_cast<uint32_t>(uint64_t{y} * 100 / e.scale);
  }
  release();
  maps_ = maps;
  row_ = row;
  row_capacity_ = widest;
  count_ = n;
  for (uint32_t i = 0; i < n; ++i) entries_[i] = fresh[i];
  width_ = static_cast<uint32_t>(right);
  height_ = static_cast<uint32_t>(bottom);
  scaling_ = scaling;
  return true;
}

void MonitorDisplay::blit(uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh, const uint32_t* pixels, uint32_t pitch) {
  const int64_t x0 = bx, y0 = by, x1 = int64_t{bx} + bw, y1 = int64_t{by} + bh;
  for (uint32_t i = 0; i < count_; ++i) {
    const Entry& e = entries_[i];
    // The part of the rectangle that is on this monitor (desktop coordinates).
    const int64_t ix0 = x0 > e.lx ? x0 : e.lx, iy0 = y0 > e.ly ? y0 : e.ly;
    const int64_t ix1 = x1 < int64_t{e.lx} + e.lw ? x1 : int64_t{e.lx} + e.lw, iy1 = y1 < int64_t{e.ly} + e.lh ? y1 : int64_t{e.ly} + e.lh;
    if (ix1 <= ix0 || iy1 <= iy0) continue;
    if (e.scale == 100) {   // one copy
      gpu_.blit(e.px + static_cast<uint32_t>(ix0 - e.lx), e.py + static_cast<uint32_t>(iy0 - e.ly), static_cast<uint32_t>(ix1 - ix0), static_cast<uint32_t>(iy1 - iy0),
                pixels + (iy0 - y0) * pitch + (ix0 - x0), pitch);
      continue;
    }
    // Enlarged: the monitor's pixel columns / rows that show the desktop columns / rows of the rectangle (see update(): pixel p shows floor(p * 100 / scale)).
    const int64_t rx0 = ix0 - e.lx, rx1 = ix1 - e.lx, ry0 = iy0 - e.ly, ry1 = iy1 - e.ly;
    int64_t pxa = (rx0 * e.scale + 99) / 100, pxb = (rx1 * e.scale + 99) / 100;
    int64_t pya = (ry0 * e.scale + 99) / 100, pyb = (ry1 * e.scale + 99) / 100;
    if (pxb > e.pw) pxb = e.pw;
    if (pyb > e.ph) pyb = e.ph;
    if (pxb <= pxa || pyb <= pya) continue;
    const uint32_t columns = static_cast<uint32_t>(pxb - pxa);
    for (int64_t py = pya; py < pyb; ++py) {
      const uint32_t* source = pixels + (e.ly + static_cast<int64_t>(e.ymap[py]) - y0) * pitch + (e.lx - x0);
      for (int64_t px = pxa; px < pxb; ++px) row_[px - pxa] = source[e.xmap[px]];
      gpu_.blit(e.px + static_cast<uint32_t>(pxa), e.py + static_cast<uint32_t>(py), columns, 1, row_, columns);
    }
  }
}

}  // namespace hardware
