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
#include "Process/ModuleLoader.hpp"

namespace windows {

// ntdll.dll and kernel32.dll, built from src/Windows/ with the rest of the OS and carried inside the image (see CMakeLists.txt).
class BuiltinLibraries final : public process::ILibraryProvider {
 public:
  bool find(StringView canonical_name, ConstByteView* image) override;
};

}  // namespace windows
