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
#include "hardware/IHardware.hpp"
#include "hardware/ImageSignalProcessor/ImageTypes.hpp"

namespace hardware {

// An image signal processor: turns raw Bayer sensor frames into displayable
// RGB images (demosaicing, black level, white balance, gamma) and measures
// frames for the 3A loops. Implementations live in subfolders: a CPU-based
// one that works anywhere, and per-vendor classes for hardware ISPs.
class IImageSignalProcessor : public IHardware {
 public:
  // Largest frame the processor accepts (either dimension).
  virtual uint32_t max_dimension() const = 0;

  virtual bool set_settings(const IspSettings& settings) = 0;
  virtual const IspSettings& settings() const = 0;

  // Converts `in` to RGB in `out`, which must be at least as large as the frame.
  // Returns false for unusable input (null/too small buffers, bad bit depth).
  virtual bool process(const RawFrame& in, ImageBuffer* out) = 0;
  // Measures a frame without producing an image.
  virtual bool analyze(const RawFrame& in, IspStatistics* out) = 0;

 protected:
  constexpr IImageSignalProcessor() = default;
  ~IImageSignalProcessor() = default;
};

// Gray-world auto white balance: gains that make the three channel means equal,
// keeping green at 1.0. Other settings are copied from `base`.
IspSettings auto_white_balance(const IspStatistics& statistics, const IspSettings& base);

}  // namespace hardware
