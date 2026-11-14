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

// Monitors of different scales, each drawn at its own (Graphics2D/MultiScaleRenderer2D.hpp): for every enabled monitor of the layout, a picture of
// its pixels drawn by a software renderer, under a ScaledRenderer2D at the monitor's scale whose origin is the monitor's place in the layout. The
// software renderer presents into the monitor's place on the display device (ScanoutPlacement: the monitors' pixels side by side), and the
// renderer() everything goes through presents the device once after them. Used instead of MonitorDisplay (one picture at the largest scale,
// resampled for the other monitors) when the enabled monitors' scales differ.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/Graphics2D/MultiScaleRenderer2D.hpp"
#include "Graphics/Graphics2D/ScaledRenderer2D.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Hardware/Monitor/IMonitorSource.hpp"
#include "Hardware/Monitor/MonitorLayout.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class MonitorRenderers {
 public:
  MonitorRenderers(IGraphicsProcessingUnit& gpu, allocators::IAllocator& allocator) : gpu_(gpu), allocator_(allocator), outputs_(allocator), multi_(allocator, &gpu) {}
  ~MonitorRenderers() = default;
  MonitorRenderers(const MonitorRenderers&) = delete;
  MonitorRenderers& operator=(const MonitorRenderers&) = delete;

  // True when the enabled monitors of `layout` are not all at one scale (this class is then what draws them).
  static bool mixed_scales(const MonitorLayout& layout);
  // Builds a view for every enabled monitor (matched to `physical` by id). False, with the old views kept, when a monitor has no place on the
  // device, a place runs off it, or memory ran out. `text`: the face the views draw text with at a scale (CellTextFace; null: the 8x16 font enlarged).
  bool update(const MonitorLayout& layout, const ScanoutPlacement* physical, uint32_t count, graphics::ICellTextFace* text = nullptr);
  // What the desktop draws on: the layout's units, every monitor at its own scale. Valid while this object lives (rebuilt by update, kept by address).
  graphics::IRenderer2D& renderer() { return multi_; }
  uint32_t view_count() const { return multi_.view_count(); }

 private:
  // Presents a monitor's picture at its place on the device (the device's present comes once, from the multi renderer).
  class Placed final : public IGraphicsProcessingUnit {
   public:
    Placed(IGraphicsProcessingUnit& gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h) : gpu_(gpu), x_(x), y_(y), w_(w), h_(h) {}
    const char* name() const override { return "monitor"; }
    bool initialize() override { return true; }
    void shutdown() override {}
    uint32_t mode_count() const override { return 0; }
    bool mode_info(uint32_t, DisplayMode*) const override { return false; }
    uint32_t current_mode() const override { return 0; }
    bool set_mode(uint32_t) override { return false; }
    uint32_t width() const override { return w_; }
    uint32_t height() const override { return h_; }
    void put_pixel(uint32_t x, uint32_t y, uint32_t color) override { if (x < w_ && y < h_) gpu_.put_pixel(x_ + x, y_ + y, color); }
    void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) override;
    void copy_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void clear(uint32_t color) override { fill_rect(0, 0, w_, h_, color); }
    void blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels, uint32_t pitch_pixels) override;
    void present() override {}
    uint32_t glyph_width() const override { return gpu_.glyph_width(); }
    uint32_t glyph_height() const override { return gpu_.glyph_height(); }
    void draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t) override {}
    void draw_text(uint32_t, uint32_t, const char*, uint32_t, uint32_t) override {}

   private:
    IGraphicsProcessingUnit& gpu_;
    uint32_t x_, y_, w_, h_;
  };
  struct Output {
    explicit Output(allocators::IAllocator& a) : pixels(a) {}
    List<uint32_t> pixels;
    UniquePointer<Placed> placed;
    UniquePointer<graphics::SoftwareRenderer2D> software;
    UniquePointer<graphics::ScaledRenderer2D> scaled;
  };

  IGraphicsProcessingUnit& gpu_;
  allocators::IAllocator& allocator_;
  List<UniquePointer<Output>> outputs_;
  graphics::MultiScaleRenderer2D multi_;
};

}  // namespace hardware
