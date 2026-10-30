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
#include "Emulation/SoftFloat.hpp"
#include <stdint.h>

// FEAT_FP8: the two 8-bit floating-point formats and their conversions. E5M2 is the top byte of a binary16; E4M3 has no infinities and a single NaN pattern per sign.
namespace emulation {
namespace fp8 {

enum class Fmt : uint8_t { E5M2 = 0, E4M3 = 1 };

// The value of an 8-bit float scaled by 2^-scale, rounded to nearest even into binary16, BFloat16 or binary32 (bits in the low part of the result). A NaN is the default NaN.
// `inexact` is set when the rounding lost bits.
uint32_t widen(Fmt f, uint8_t v, softfloat::Format to, uint32_t scale, bool* inexact);
// A binary16, BFloat16 or binary32 value times 2^scale to an 8-bit float, rounded to nearest even. On overflow (and for an infinity) the result is the infinity of E5M2 or the NaN of E4M3, or the largest
// finite value when `saturate` is set. `flags` receives the softfloat exception bits.
uint8_t narrow(Fmt f, softfloat::Format from, uint64_t bits, int32_t scale, bool saturate, uint32_t* flags);
// acc + a * b * 2^-scale for two 8-bit floats, rounded once into binary16 or binary32 (the product is exact). The result of a NaN or infinity follows the usual rules; flags are added to `env`.
uint32_t multiply_add(Fmt fa, uint8_t a, Fmt fb, uint8_t b, uint32_t scale, softfloat::Format dst, uint32_t acc, bool saturate, softfloat::Env& env);
// acc + sum of a[i] * b[i] * 2^-scale (n terms) with a single rounding into binary16 or binary32. With `saturate` an overflow gives the largest finite value.
uint32_t dot_add(Fmt fa, const uint8_t* a, Fmt fb, const uint8_t* b, uint32_t n, uint32_t scale, softfloat::Format dst, uint32_t acc, bool saturate);
// True when the value is a signaling NaN (every NaN of E4M3 is).
bool is_signaling(Fmt f, uint8_t v);
// True when the value is a NaN of the format.
bool is_nan(Fmt f, uint8_t v);

}  // namespace fp8
}  // namespace emulation
