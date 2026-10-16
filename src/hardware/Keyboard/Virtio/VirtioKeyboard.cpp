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

#include "hardware/Keyboard/Virtio/VirtioKeyboard.hpp"

namespace hardware {

void VirtioKeyboard::fetch() {
  VirtioInputEvent e;
  while (device_.next_event(&e)) {
    if (e.type != kEvKey || e.value == 2) continue;  // ignore sync markers and auto-repeat
    const uint8_t usage = usage_from_linux_key(e.code);
    if (usage) push(usage, e.value == 1);
  }
}

}  // namespace hardware
