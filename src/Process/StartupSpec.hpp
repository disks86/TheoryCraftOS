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
#include "Utility/StringView.hpp"

namespace process {

// What CreateProcess hands over in STARTUPINFO (and the console it starts with): it ends up in the child's RTL_USER_PROCESS_PARAMETERS.
struct StartupSpec {
  uint32_t flags = 0;                          // STARTF_*
  uint32_t x = 0, y = 0, x_size = 0, y_size = 0, x_chars = 0, y_chars = 0, fill_attribute = 0, show_window = 0;
  uint32_t console_flags = 0;
  uint64_t console_handle = 0;                 // the console the child belongs to (0: none)
  StringView title, desktop;                   // UTF-8
};

}  // namespace process
