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

// The boot log: one line per piece of hardware or service as it comes up, tagged with how it went,
//   [ ok ] CPU: QEMU Virtual CPU version 2.5+, 4 cores
//   [warn] Input: no mouse found
//   [FAIL] Kernel: could not start threads
// Warnings and failures are counted so the boot can tell whether something deserves the user's
// attention before the desktop opens.
//
//   bootlog::Entry(bootlog::Level::Ok, "RAM").text("pool ").size(bytes);   // printed at the end of the statement
#pragma once
#include <stdint.h>
#include "Utility/StringView.hpp"

namespace bootlog {

enum class Level : uint8_t { Info, Ok, Warning, Fault };

class Entry {
 public:
  Entry(Level level, StringView subsystem);
  ~Entry();  // prints the line
  Entry(const Entry&) = delete;
  Entry& operator=(const Entry&) = delete;

  Entry& text(StringView s);
  Entry& dec(uint64_t v);
  Entry& hex(uint64_t v);
  // Exactly two hex digits (for MAC addresses).
  Entry& hex2(uint8_t v);
  // Bytes as "123 B", "4 KiB", "256 MiB" or "2 GiB".
  Entry& size(uint64_t bytes);

 private:
  void put(char c);

  Level level_;
  char line_[200];
  uint32_t length_ = 0;
};

// Draws the lines logged so far again on the screen (not the debug console): for after the screen
// was cleared (a display mode change) so the log starts at the top again.
void replay();

uint32_t warnings();
uint32_t faults();

}  // namespace bootlog
