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

namespace emulation {
namespace rvcrypto {

// Executes the scalar crypto and bit-manipulation instructions of Zbkb, Zbkx, Zknd, Zkne, Zknh, Zksed and Zksh; `a` and `b` are the values of rs1 and rs2. False when the word is not one of them.
bool scalar_crypto(uint32_t insn, uint64_t a, uint64_t b, uint64_t* out);

}  // namespace rvcrypto
}  // namespace emulation
