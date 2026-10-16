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
#include "Hardware/ImageSignalProcessor/IImageSignalProcessor.hpp"

namespace hardware {

// A CPU implementation of the ISP pipeline, usable on any machine: bilinear
// demosaicing, black-level subtraction, per-channel white-balance gain, then a
// gamma curve through a lookup table. Hardware ISP classes use it until they
// have native drivers.
class SoftwareImageSignalProcessor : public IImageSignalProcessor {
 public:
  static constexpr uint32_t kGammaEntries = 1024;

  SoftwareImageSignalProcessor();

  const char* name() const override { return "software ISP"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  uint32_t max_dimension() const override { return 16384; }

  bool set_settings(const IspSettings& settings) override;
  const IspSettings& settings() const override { return settings_; }

  bool process(const RawFrame& in, ImageBuffer* out) override;
  bool analyze(const RawFrame& in, IspStatistics* out) override;

 private:
  IspSettings settings_;
  uint8_t gamma_[kGammaEntries];
};

}  // namespace hardware
