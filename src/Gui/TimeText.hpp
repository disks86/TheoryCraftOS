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

namespace gui {

// "MM:SS", or "H:MM:SS" from an hour on, for a time in seconds. \`out\` needs room for 8 characters and a NUL. Returns the length.
inline uint32_t format_clock(uint64_t seconds, char* out) {
  const uint64_t hours = seconds / 3600, minutes = seconds / 60 % 60, secs = seconds % 60;
  uint32_t n = 0;
  if (hours > 0) {
    out[n++] = static_cast<char>('0' + hours % 10);
    out[n++] = ':';
  }
  out[n++] = static_cast<char>('0' + minutes / 10);
  out[n++] = static_cast<char>('0' + minutes % 10);
  out[n++] = ':';
  out[n++] = static_cast<char>('0' + secs / 10);
  out[n++] = static_cast<char>('0' + secs % 10);
  out[n] = 0;
  return n;
}

}  // namespace gui
