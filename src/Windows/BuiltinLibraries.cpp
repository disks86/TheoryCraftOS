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
#include "Windows/BuiltinLibraries.hpp"

extern "C" {
extern const uint8_t tcos_ntdll_dll[], tcos_ntdll_dll_end[], tcos_kernel32_dll[], tcos_kernel32_dll_end[];
}

namespace windows {

bool BuiltinLibraries::find(StringView canonical_name, ConstByteView* image) {
  if (canonical_name.equals("ntdll.dll")) {
    *image = ConstByteView(tcos_ntdll_dll, static_cast<uint64_t>(tcos_ntdll_dll_end - tcos_ntdll_dll));
    return true;
  }
  if (canonical_name.equals("kernel32.dll")) {
    *image = ConstByteView(tcos_kernel32_dll, static_cast<uint64_t>(tcos_kernel32_dll_end - tcos_kernel32_dll));
    return true;
  }
  return false;
}

}  // namespace windows
