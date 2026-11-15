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
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

// What the App Manager needs of the Windows world to install a package that comes as a setup program (WingetInstaller.hpp): running the program and
// waiting for it, and the list of installed programs that setup programs keep in the registry, which is where a program's uninstaller is found.
namespace desktop::appman {

// An entry of the registry's Uninstall keys (HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\<key>, the same under WOW6432Node, and each user's).
struct UninstallEntry {
  explicit UninstallEntry(allocators::IAllocator& allocator) : key(allocator), name(allocator), version(allocator), command(allocator), quiet_command(allocator) {}
  String key;               // the full name of the registry key: its last part is the product code
  String name;              // DisplayName
  String version;           // DisplayVersion
  String command;           // UninstallString: a command line
  String quiet_command;     // QuietUninstallString: one that asks nothing (may be empty)
};

class ISetupHost {
 public:
  // Runs the Windows program at `program` (a path in the OS file system, under /Windows) with `arguments` and waits for it to end. *cancel (may be null)
  // becoming true stops it. False if it could not be started or was stopped: *message says which. True: *exit_code is the code it ended with.
  [[nodiscard]] virtual bool run(StringView program, ArrayView<const StringView> arguments, const bool* cancel, uint32_t* exit_code, String* message) = 0;
  // The Uninstall entries the registry has now. False if memory ran out.
  [[nodiscard]] virtual bool uninstall_entries(List<UniquePointer<UninstallEntry>>* entries) = 0;

 protected:
  ~ISetupHost() = default;
};

}  // namespace desktop::appman
