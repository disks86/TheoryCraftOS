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
#include "Allocators/IAllocator.hpp"
#include "Desktop/AppMan/SetupHost.hpp"
#include "FileSystem/IFileSystem.hpp"

namespace desktop::appman {

// The App Manager's way into the Windows world of this kernel: a setup program runs as any Windows program does (Program/ProgramRunner.hpp), on the
// calling thread, with nothing to read from its console; the Uninstall entries come from the kernel's registry (Process/WindowsRegistry.hpp).
class KernelSetupHost final : public ISetupHost {
 public:
  // `files` is the OS file system (it holds /Windows) and must outlive this.
  KernelSetupHost(allocators::IAllocator& allocator, filesystem::IFileSystem& files) : allocator_(allocator), files_(files) {}

  [[nodiscard]] bool run(StringView program, ArrayView<const StringView> arguments, const bool* cancel, uint32_t* exit_code, String* message) override;
  [[nodiscard]] bool uninstall_entries(List<UniquePointer<UninstallEntry>>* entries) override;

 private:
  allocators::IAllocator& allocator_;
  filesystem::IFileSystem& files_;
};

}  // namespace desktop::appman
