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
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_av1 {

// The entropy contexts the Intel AV1 engine (AVP) starts a frame from. The engine reads the cumulative distribution functions of every syntax element from a buffer of
// 236 cache lines (15104 bytes) laid out as the reference driver lays it out: each element's tables start on a line of their own, one table (or part of one) per line.
// A frame with no primary reference starts from the default tables, of which there are four, picked by the frame's base quantizer index; a frame with one starts from the
// tables the engine saved at the end of the primary reference frame.
constexpr uint32_t kCdfContextBytes = 236 * 64;
constexpr uint32_t kDefaultContexts = 4;

// Which of the four default contexts a frame with this base quantizer index starts from.
constexpr uint32_t default_context_for(uint32_t base_qindex) { return base_qindex <= 20 ? 0 : base_qindex <= 60 ? 1 : base_qindex <= 120 ? 2 : 3; }

// Writes default context `index` (0-3) into `out` (kCdfContextBytes). False for a bad index or size.
bool build_default_cdf_context(uint32_t index, ByteView out);

}  // namespace intel_av1
}  // namespace hardware
