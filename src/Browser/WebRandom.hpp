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

// Where web pages get unpredictable bytes (crypto.getRandomValues(), key generation, ECDSA nonces): one source for all pages, safe to use from any thread.
#pragma once
#include "Crypto/Random.hpp"

namespace browser {

// Gives the pages a random source (it must outlive them); nullptr goes back to the CPU's own generator.
void set_system_random(crypto::IRandom* random);
// Fills `out`; false if there is no source (a CPU without RDRAND and none given).
bool system_random(ByteView out);

}  // namespace browser
