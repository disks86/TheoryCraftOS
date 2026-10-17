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

#include "Desktop/PrintPreview.hpp"

namespace desktop {

using namespace gui;
using namespace hardware;

void PrintPreview::set_sheet(PaperSize paper, bool landscape, uint32_t margin_mm10) {
  UiGuard lock(this);
  paper_ = paper;
  landscape_ = landscape;
  margin_mm10_ = margin_mm10;
  cache_valid_ = false;
  invalidate();
}

void PrintPreview::show_text(const TextLayout* layout) {
  UiGuard lock(this);
  text_ = layout;
  picture_ = nullptr;
  if (page_ >= page_count()) page_ = page_count() ? page_count() - 1 : 0;
  invalidate();
}

void PrintPreview::show_picture(const graphics::Surface* picture, Fit fit, Halftone method) {
  UiGuard lock(this);
  text_ = nullptr;
  picture_ = picture;
  fit_ = fit;
  method_ = method;
  page_ = 0;
  cache_valid_ = false;
  invalidate();
}

void PrintPreview::show_nothing() {
  UiGuard lock(this);
  text_ = nullptr;
  picture_ = nullptr;
  page_ = 0;
  invalidate();
}

uint32_t PrintPreview::page_count() const {
  if (text_) return text_->page_count();
  return picture_ ? 1 : 0;
}

void PrintPreview::set_page(uint32_t page) {
  UiGuard lock(this);
  const uint32_t count = page_count();
  page_ = count == 0 ? 0 : page >= count ? count - 1 : page;
  invalidate();
}

void PrintPreview::layout() {
  cache_valid_ = false;
  Control::layout();
}

// The sheet is as large as fits in the control (with a border for its shadow), in the proportions of the paper.
Rect2D PrintPreview::sheet_rect() const {
  const PaperInfo& p = paper_info(paper_);
  const uint32_t pw = landscape_ ? p.height_mm10 : p.width_mm10, ph = landscape_ ? p.width_mm10 : p.height_mm10;
  const int32_t room_w = static_cast<int32_t>(bounds_.width) - 2 * kBorder, room_h = static_cast<int32_t>(bounds_.height) - 2 * kBorder;
  if (room_w <= 0 || room_h <= 0 || pw == 0 || ph == 0) return {0, 0, 0, 0};
  int32_t w = room_w, h = static_cast<int32_t>(static_cast<int64_t>(room_w) * ph / pw);
  if (h > room_h) {
    h = room_h;
    w = static_cast<int32_t>(static_cast<int64_t>(room_h) * pw / ph);
  }
  return {(static_cast<int32_t>(bounds_.width) - w) / 2, (static_cast<int32_t>(bounds_.height) - h) / 2, static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

Rect2D PrintPreview::printable_rect() const {
  const Rect2D sheet = sheet_rect();
  const PaperInfo& p = paper_info(paper_);
  const uint32_t pw = landscape_ ? p.height_mm10 : p.width_mm10, ph = landscape_ ? p.width_mm10 : p.height_mm10;
  if (sheet.width == 0 || pw == 0 || ph == 0 || pw <= 2 * margin_mm10_ || ph <= 2 * margin_mm10_) return sheet;
  const int32_t mx = static_cast<int32_t>(static_cast<uint64_t>(margin_mm10_) * sheet.width / pw);
  const int32_t my = static_cast<int32_t>(static_cast<uint64_t>(margin_mm10_) * sheet.height / ph);
  return {sheet.x + mx, sheet.y + my, sheet.width - 2 * mx, sheet.height - 2 * my};
}

uint32_t PrintPreview::drawn_lines(uint32_t page) const {
  if (!text_) return 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < text_->line_count(page); ++i) {
    const ConstByteView line = text_->line(page, i);
    for (uint64_t k = 0; k < line.size(); ++k)
      if (line[k] != ' ') {
        ++n;
        break;
      }
  }
  return n;
}

// The picture as the printer will print it, at the preview's size: placed on the printable area and halftoned.
void PrintPreview::rebuild_picture(const Rect2D& area) {
  if (area.width != cache_w_ || area.height != cache_h_) {
    release_view(allocator(), cache_);
    cache_ = allocate_view<uint32_t>(allocator(), uint64_t{area.width} * area.height);
    cache_w_ = cache_.empty() ? 0 : area.width;
    cache_h_ = cache_.empty() ? 0 : area.height;
  }
  cache_valid_ = true;
  if (cache_.empty() || !picture_) return;
  PageBitmap page(allocator());
  if (!page.create(area.width, area.height) || !render_surface(*picture_, &page, fit_, method_, allocator())) {
    for (uint32_t& p : cache_) p = 0xFFFFFFFF;
    return;
  }
  for (uint32_t y = 0; y < area.height; ++y)
    for (uint32_t x = 0; x < area.width; ++x) cache_[uint64_t{y} * area.width + x] = page.get(x, y) ? 0xFF202020 : 0xFFFFFFFF;
}

void PrintPreview::paint(Painter& painter) {
  const Theme& t = painter.theme();
  painter.fill({0, 0, bounds_.width, bounds_.height}, t.shadow);
  painter.bevel({0, 0, bounds_.width, bounds_.height}, true);
  const Rect2D sheet = sheet_rect();
  if (sheet.width == 0) return;
  painter.fill({sheet.x + 3, sheet.y + 3, sheet.width, sheet.height}, t.dark);   // its shadow
  painter.fill(sheet, 0xFFFFFFFF);
  painter.outline(sheet, 0xFF000000);
  const Rect2D area = printable_rect();
  if (area.width == 0 || area.height == 0) return;
  if (text_ && page_ < text_->page_count()) {
    const uint32_t columns = text_->columns(), lines = text_->lines_per_page();
    for (uint32_t i = 0; i < text_->line_count(page_); ++i) {
      const ConstByteView line = text_->line(page_, i);
      const int32_t y0 = area.y + static_cast<int32_t>(static_cast<uint64_t>(i) * area.height / lines);
      const int32_t y1 = area.y + static_cast<int32_t>(static_cast<uint64_t>(i + 1) * area.height / lines);
      const int32_t bar_h = (y1 - y0) * 3 / 5 > 1 ? (y1 - y0) * 3 / 5 : 1, bar_y = y0 + ((y1 - y0) - bar_h) / 2;
      uint64_t k = 0;
      while (k < line.size()) {   // a bar for every run of characters that are not blank
        if (line[k] == ' ') {
          ++k;
          continue;
        }
        uint64_t end = k;
        while (end < line.size() && line[end] != ' ') ++end;
        const int32_t x0 = area.x + static_cast<int32_t>(k * area.width / columns);
        const int32_t x1 = area.x + static_cast<int32_t>(end * area.width / columns);
        painter.fill({x0, bar_y, static_cast<uint32_t>(x1 > x0 ? x1 - x0 : 1), static_cast<uint32_t>(bar_h)}, 0xFF303030);
        k = end;
      }
    }
  } else if (picture_) {
    if (!cache_valid_) rebuild_picture(area);
    if (!cache_.empty()) {
      graphics::Surface s;
      s.pixels = cache_;
      s.width = s.pitch = cache_w_;
      s.height = cache_h_;
      painter.image(s, {0, 0, cache_w_, cache_h_}, area.x, area.y);
    }
  }
}

}  // namespace desktop
