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
#include <stdint.h>
#include "Hardware/IHardware.hpp"

namespace hardware {

struct DisplayMode {
  uint32_t width;   // pixels
  uint32_t height;  // pixels
};

// A display-capable graphics device with mode setting, 2D drawing and text
// rendering. Colors are 0x00RRGGBB; coordinates are pixels from the top left.
// Implementations live in vendor subfolders beside this file.
class IGraphicsProcessingUnit : public IHardware {
 public:
  // --- Mode setting. Modes are numbered 0..mode_count()-1.
  virtual uint32_t mode_count() const = 0;
  virtual bool mode_info(uint32_t index, DisplayMode* out) const = 0;
  virtual uint32_t current_mode() const = 0;
  // Switches resolution; the screen contents are lost. False if unsupported.
  virtual bool set_mode(uint32_t index) = 0;

  // Size of the current mode.
  virtual uint32_t width() const = 0;
  virtual uint32_t height() const = 0;

  // --- Drawing. Out-of-range pixels are clipped.
  virtual void put_pixel(uint32_t x, uint32_t y, uint32_t color) = 0;
  virtual void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) = 0;
  // Copies a rectangle within the screen; source and destination may overlap.
  virtual void copy_rect(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y,
                         uint32_t w, uint32_t h) = 0;
  virtual void clear(uint32_t color) = 0;

  // --- Text rendering with a fixed-size font.
  virtual uint32_t glyph_width() const = 0;
  virtual uint32_t glyph_height() const = 0;
  virtual void draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) = 0;
  // Draws left to right without wrapping; '\n' is not interpreted.
  virtual void draw_text(uint32_t x, uint32_t y, const char* text, uint32_t fg, uint32_t bg) = 0;

 protected:
  constexpr IGraphicsProcessingUnit() = default;
  ~IGraphicsProcessingUnit() = default;
};

}  // namespace hardware
