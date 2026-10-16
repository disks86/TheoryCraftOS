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

#include "Hardware/TextConsole.hpp"
#include "Kernel/Firmware.hpp"

namespace hardware {
void TextConsole::put(char c) const {
  // Firmware output only from a context that may call the firmware (the console
  // lock keeps the call exclusive); other threads reach the debug console only.
  if (!st_ || !st_->con_out || !kernel::may_call_firmware_directly()) return;
  efi::Char16 buf[3] = {};
  if (c == '\n') { buf[0] = u'\r'; buf[1] = u'\n'; }
  else           { buf[0] = static_cast<efi::Char16>(c); }
  st_->con_out->output_string(st_->con_out, buf);
}
}  // namespace hardware
