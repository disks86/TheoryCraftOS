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

#include "hardware/Keyboard/Ps2/Ps2Keyboard.hpp"

namespace hardware {

bool Ps2Keyboard::initialize() {
  extended_ = false;
  skip_ = 0;
  return controller_.send_keyboard(0xF4);  // enable scanning
}

void Ps2Keyboard::fetch() {
  uint8_t byte;
  while (controller_.read_keyboard(&byte)) {
    if (skip_) { --skip_; continue; }
    if (byte == 0xE0) { extended_ = true; continue; }
    if (byte == 0xE1) { skip_ = 5; continue; }              // Pause/Break: ignore the sequence
    if (byte == 0xFA || byte == 0xFE || byte == 0x00 || byte == 0xFF) continue;

    const bool released = byte & 0x80;
    const uint8_t code = byte & 0x7F;
    const uint8_t usage = usage_from_scan_code_set1(code, extended_);
    extended_ = false;
    if (usage) push(usage, !released);
  }
}

}  // namespace hardware
