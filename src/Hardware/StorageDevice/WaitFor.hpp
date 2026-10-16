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
#include "Kernel/Delay.hpp"
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/ByteStream.hpp"
#include "uefi.hpp"

namespace hardware {

// Polls `done()` until it returns true or `timeout_ms` passes (firmware timer).
template <class F>
bool wait_for(efi::SystemTable* st, uint32_t timeout_ms, F&& done) {
  // Poll fast at first (most devices answer within microseconds), then back off
  // to sleeping so a slow device does not keep a core busy.
  uint64_t waited = 0;
  while (waited < uint64_t{timeout_ms} * 1000) {
    if (done()) return true;
    const uint64_t step = waited < 2000 ? 10 : 200;
    delay_us(step);
    waited += step;
  }
  return done();
}

// Copies a fixed-width, blank-padded device string (ATA, SCSI inquiry) into a
// NUL-terminated buffer with trailing blanks removed. `swapped` undoes ATA's
// byte-swapped 16-bit words (the input length must then be even).
inline void copy_device_string(ArrayView<char> out, ConstByteView in, bool swapped) {
  if (out.empty()) return;
  uint64_t n = in.size() < out.size() - 1 ? in.size() : out.size() - 1;
  if (swapped) n &= ~uint64_t{1};  // words
  for (uint64_t i = 0; i < n; ++i) out[i] = static_cast<char>(swapped ? in[i ^ 1] : in[i]);
  while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0)) --n;
  out[n] = 0;
  for (uint64_t i = 0; i < n; ++i)
    if (static_cast<uint8_t>(out[i]) < 32 || static_cast<uint8_t>(out[i]) > 126) out[i] = '?';
}

}  // namespace hardware
