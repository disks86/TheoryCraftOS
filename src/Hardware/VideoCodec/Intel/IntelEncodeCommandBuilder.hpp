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
#include "Hardware/VideoCodec/Intel/IntelMfxCommands.hpp"

namespace hardware {
namespace intel_enc {
namespace build {

// A bit field of a command: the dword it is in and its bit range (the generated IntelEncodeCommandImages*.inc files name these).
struct EncodeField {
  uint8_t dw, lo, hi;
};

// A command being filled in: the media driver's defaults, then fields by name.
template <uint32_t N>
class Command {
 public:
  explicit Command(const uint32_t (&defaults)[N]) {
    for (uint32_t i = 0; i < N; ++i) d_[i] = defaults[i];
  }
  void set(EncodeField f, uint32_t value) {
    const uint32_t width = f.hi - f.lo + 1u;
    const uint32_t mask = (width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u)) << f.lo;
    d_[f.dw] = (d_[f.dw] & ~mask) | ((value << f.lo) & mask);
  }
  void raw(uint32_t dw, uint32_t value) { d_[dw] = value; }
  uint32_t get(uint32_t dw) const { return d_[dw]; }
  // A buffer's address (low dword; the high dword stays 0 below 4 GiB) and its memory attributes dword.
  void buffer(uint32_t dw, uint32_t address, uint32_t attributes) {
    d_[dw] = address;
    d_[dw + 1] = 0;
    d_[dw + 2] = (d_[dw + 2] & ~0x7Fu) | attributes;
  }
  void emit(intel_mfx::DwordWriter& w) const {
    for (uint32_t i = 0; i < N; ++i) w.emit(d_[i]);
  }

 private:
  uint32_t d_[N];
};

constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

}  // namespace build
}  // namespace intel_enc
}  // namespace hardware
