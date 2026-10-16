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
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"

// A scrolling text console drawn through an IGraphicsProcessingUnit. It only
// uses the GPU interface (glyphs, fill and copy), so it works with every GPU
// class, including the UEFI framebuffer fallback.
class GraphicsConsole {
 public:
  // Starts (or restarts after a mode change) on `gpu` with a cleared screen.
  void attach(hardware::IGraphicsProcessingUnit* gpu);
  bool attached() const { return gpu_ != nullptr; }

  void put(char c);
  // Text color for what is put next (0x00RRGGBB); 0 restores the default.
  void set_foreground(uint32_t color) { foreground_ = color ? color : kForeground; }
  // Shows what has been put so far on GPUs that need telling.
  void flush() {
    if (gpu_) gpu_->present();
  }

 private:
  void new_line();

  static constexpr uint32_t kForeground = 0xE0E0E0;
  static constexpr uint32_t kBackground = 0x101018;

  hardware::IGraphicsProcessingUnit* gpu_ = nullptr;
  uint32_t foreground_ = kForeground;
  uint32_t cols_ = 0, rows_ = 0, col_ = 0, row_ = 0;
};
