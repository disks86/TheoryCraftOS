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

// Draws the desktop on the monitors. The desktop the programs draw on is one picture whose size is the bounds of the monitor layout ("logical":
// every monitor's rectangle is its pixels divided by its scale, and the rectangles are where the user dragged them); the display device holds the
// pixels of the monitors side by side in a row (the "surface"). MonitorDisplay stands in for the display device for whoever presents the desktop
// (the software 2D renderer): width() and height() are those of the desktop, and blit() copies a rectangle of the desktop to the surface - to every
// monitor it touches, to the place of that monitor's pixels, enlarged by the monitor's scale (nearest neighbour). Monitors at 100 % that lie in
// the same arrangement as their pixels cost a single copy each.
//
// Drawing calls other than blit() and present() are not used by the renderer and go straight to the device (in surface coordinates).
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Hardware/Monitor/IMonitorSource.hpp"
#include "Hardware/Monitor/MonitorLayout.hpp"

namespace hardware {

class MonitorDisplay final : public IGraphicsProcessingUnit {
 public:
  MonitorDisplay(IGraphicsProcessingUnit& gpu, allocators::IAllocator& allocator) : gpu_(gpu), allocator_(allocator) {}
  ~MonitorDisplay();
  MonitorDisplay(const MonitorDisplay&) = delete;
  MonitorDisplay& operator=(const MonitorDisplay&) = delete;

  // Sets where everything goes: the enabled monitors of `layout` (rectangle and scale) and where each one's pixels are on the surface
  // (`physical`, `count` entries, matched by the monitors' ids). False (nothing changes) if a monitor has no place on the surface or memory ran out.
  bool update(const MonitorLayout& layout, const ScanoutPlacement* physical, uint32_t count);
  // True when copying is not a plain pass-through (a scaled monitor or monitors not laid out like their pixels).
  bool scaling() const { return scaling_; }

  const char* name() const override { return "monitor compositor"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t mode_count() const override { return 0; }
  bool mode_info(uint32_t, DisplayMode*) const override { return false; }
  uint32_t current_mode() const override { return 0; }
  bool set_mode(uint32_t) override { return false; }
  uint32_t width() const override { return width_; }
  uint32_t height() const override { return height_; }
  void put_pixel(uint32_t x, uint32_t y, uint32_t color) override { gpu_.put_pixel(x, y, color); }
  void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) override { gpu_.fill_rect(x, y, w, h, color); }
  void copy_rect(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) override { gpu_.copy_rect(sx, sy, dx, dy, w, h); }
  void clear(uint32_t color) override { gpu_.clear(color); }
  void blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels, uint32_t pitch_pixels) override;
  void present() override { gpu_.present(); }
  uint32_t glyph_width() const override { return gpu_.glyph_width(); }
  uint32_t glyph_height() const override { return gpu_.glyph_height(); }
  void draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) override { gpu_.draw_char(x, y, c, fg, bg); }
  void draw_text(uint32_t x, uint32_t y, const char* text, uint32_t fg, uint32_t bg) override { gpu_.draw_text(x, y, text, fg, bg); }

 private:
  struct Entry {
    int32_t lx = 0, ly = 0;                    // the monitor's rectangle on the desktop
    uint32_t lw = 0, lh = 0;
    uint32_t px = 0, py = 0, pw = 0, ph = 0;   // its pixels on the surface
    uint32_t scale = 100;
    uint32_t* xmap = nullptr;                  // for each pixel column / row of the monitor, the column / row of the desktop (relative to lx / ly)
    uint32_t* ymap = nullptr;
  };

  void release();

  IGraphicsProcessingUnit& gpu_;
  allocators::IAllocator& allocator_;
  Entry entries_[MonitorLayout::kMaxMonitors];
  uint32_t count_ = 0;
  uint32_t* maps_ = nullptr;
  uint32_t* row_ = nullptr;
  uint32_t row_capacity_ = 0;
  uint32_t width_ = 0, height_ = 0;
  bool scaling_ = false;
};

}  // namespace hardware
