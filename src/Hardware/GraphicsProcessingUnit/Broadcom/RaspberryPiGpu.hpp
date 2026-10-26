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

#pragma once
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Hardware/Platform/RaspberryPi/VideoCoreFirmware.hpp"

namespace hardware {

// The display of a Raspberry Pi 4 or Pi 400: the screen is the framebuffer the firmware set up (the display pipeline, the HVS, is the VideoCore's), drawn
// into and mode-set by `screen` (the UEFI framebuffer device); what this class adds is what only the VideoCore knows - how much memory the GPU has, how hot
// the chip is - and the V3D 3D core (V3dGpu) for rendering. Every drawing and mode call goes to `screen`.
class RaspberryPiGpu final : public IGraphicsProcessingUnit {
 public:
  RaspberryPiGpu(IGraphicsProcessingUnit& screen, VideoCoreFirmware* firmware, V3dGpu* v3d) : screen_(screen), firmware_(firmware), v3d_(v3d) {}

  const char* name() const override { return "Raspberry Pi VideoCore VI (V3D 4.2)"; }
  bool initialize() override { return screen_.initialize(); }
  void shutdown() override { screen_.shutdown(); }
  // The 3D core, if it was found and is running; nullptr otherwise.
  V3dGpu* v3d() const { return v3d_ && v3d_->initialized() ? v3d_ : nullptr; }

  uint32_t mode_count() const override { return screen_.mode_count(); }
  bool mode_info(uint32_t index, DisplayMode* out) const override { return screen_.mode_info(index, out); }
  uint32_t current_mode() const override { return screen_.current_mode(); }
  bool set_mode(uint32_t index) override { return screen_.set_mode(index); }
  uint32_t width() const override { return screen_.width(); }
  uint32_t height() const override { return screen_.height(); }
  void put_pixel(uint32_t x, uint32_t y, uint32_t color) override { screen_.put_pixel(x, y, color); }
  void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) override { screen_.fill_rect(x, y, w, h, color); }
  void copy_rect(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) override { screen_.copy_rect(sx, sy, dx, dy, w, h); }
  void clear(uint32_t color) override { screen_.clear(color); }
  void blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels, uint32_t pitch_pixels) override { screen_.blit(x, y, w, h, pixels, pitch_pixels); }
  void present() override { screen_.present(); }
  IMonitorSource* monitor_source() override { return screen_.monitor_source(); }
  bool framebuffer_range(uint64_t* base, uint64_t* bytes) const override { return screen_.framebuffer_range(base, bytes); }
  uint32_t glyph_width() const override { return screen_.glyph_width(); }
  uint32_t glyph_height() const override { return screen_.glyph_height(); }
  void draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) override { screen_.draw_char(x, y, c, fg, bg); }
  void draw_text(uint32_t x, uint32_t y, const char* text, uint32_t fg, uint32_t bg) override { screen_.draw_text(x, y, text, fg, bg); }

  // The memory the firmware keeps for the GPU. Its use is not known, so `used` is the framebuffer.
  bool video_memory(uint64_t* total_bytes, uint64_t* used_bytes) const override;
  int32_t temperature_millicelsius() override;

 private:
  IGraphicsProcessingUnit& screen_;
  VideoCoreFirmware* firmware_;
  V3dGpu* v3d_;
};

}  // namespace hardware
