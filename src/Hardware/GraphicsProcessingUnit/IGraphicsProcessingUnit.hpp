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
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/IHardware.hpp"

namespace hardware {

class IMonitorSource;

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

  // Copies a w x h block of 0xAARRGGBB pixels (rows `pitch_pixels` apart) to (x, y); alpha is ignored.
  virtual void blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels,
                    uint32_t pitch_pixels) {
    for (uint32_t j = 0; j < h; ++j)
      for (uint32_t i = 0; i < w; ++i) put_pixel(x + i, y + j, pixels[uint64_t{j} * pitch_pixels + i]);
  }
  // Makes everything drawn so far visible. Direct framebuffers show drawing at once; a GPU that
  // keeps its screen in separate memory (virtio-gpu) copies the changes over here.
  virtual void present() {}

  // The connectors and monitors of this device (see Hardware/Monitor); nullptr for a device that tells nothing about them. The monitor layer
  // (SystemMonitors) then treats the whole screen as one display.
  virtual IMonitorSource* monitor_source() { return nullptr; }

  // --- Telemetry. Each is for the computer manager and says "unknown" unless the device can tell.
  // The video memory the device has and how much of it is in use; false if unknown.
  virtual bool video_memory(uint64_t* total_bytes, uint64_t* used_bytes) const { (void)total_bytes; (void)used_bytes; return false; }
  // Which PCI function this device is, false for one that is not a PCI device (or when it is not known). Lets what a probe of the same function found out
  // (the size of its video memory) be matched to the device.
  virtual bool pci_location(PciAddress* out) const { (void)out; return false; }
  // Tells the device how much video memory it has, when something other than the device found out (the video engine probe read it off the card). Ignored
  // by a device that already knows.
  virtual void set_video_memory_total(uint64_t bytes) { (void)bytes; }
  // The display buffer the firmware set up: its address in the CPU's physical address space (the OS runs identity mapped) and the size of the whole allocation,
  // not only the visible mode. False for a device that has no such buffer. Lets a driver keep clear of memory the firmware is still showing.
  virtual bool framebuffer_range(uint64_t* base, uint64_t* bytes) const { (void)base; (void)bytes; return false; }
  // Share of the time the graphics engine is busy, 0-100, or -1 if unknown.
  virtual int32_t utilization_percent() { return -1; }
  // The chip's temperature in thousandths of a degree Celsius, or -1 if unknown.
  virtual int32_t temperature_millicelsius() { return -1; }

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
