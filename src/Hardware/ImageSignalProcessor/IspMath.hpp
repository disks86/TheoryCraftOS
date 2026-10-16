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

// Floating-point helpers for building lookup tables. The image has no libm.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

// base^exponent for base in (0, inf), via exp(exponent * ln(base)); accurate to
// about 1e-9 relative, far finer than an 8-bit gamma table needs.
double isp_pow(double base, double exponent);

// Fills table[0..entries-1] with round(255 * (i / (entries-1)) ^ (100 / gamma_x100)),
// the encoding curve for a display gamma of gamma_x100 / 100.
void build_gamma_table(ByteView table, uint16_t gamma_x100);

}  // namespace hardware
