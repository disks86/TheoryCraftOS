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

#include "hardware/ImageSignalProcessor/IspMath.hpp"
#include "Math/FloatMath.hpp"

namespace hardware {

double isp_pow(double base, double exponent) { return fmath::pow(base, exponent); }

void build_gamma_table(uint8_t* table, uint32_t entries, uint16_t gamma_x100) {
  const double inverse_gamma = gamma_x100 ? 100.0 / gamma_x100 : 1.0;
  for (uint32_t i = 0; i < entries; ++i) {
    const double v = entries > 1 ? static_cast<double>(i) / (entries - 1) : 1.0;
    const double encoded = 255.0 * fmath::pow(v, inverse_gamma) + 0.5;
    table[i] = static_cast<uint8_t>(encoded > 255.0 ? 255.0 : encoded);
  }
}

}  // namespace hardware
