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

// The kernel's IServiceLauncher: an activated service's program (a Linux program, Exec's first word a path in the Linux tree) runs with the kernel's
// run_program() (Program/ProgramRunner.hpp) on a kernel thread of its own ("dbus-service"), from / of the Linux tree, with no input; what it prints goes
// to the debug console. The thread lives as long as the program; the activator only looks at how it ended.
#pragma once
#include "DBus/Activation.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"

namespace dbus {

class ProgramActivator final : public IServiceLauncher {
 public:
  ProgramActivator(filesystem::IFileSystem& files, allocators::IAllocator& allocator);
  ProgramActivator(const ProgramActivator&) = delete;
  ProgramActivator& operator=(const ProgramActivator&) = delete;

  uint64_t start(BusKind bus, StringView service_name, ArrayView<const StringView> argv, String* why) override;
  State poll(uint64_t handle, uint32_t* exit_code, String* why) override;
  void forget(uint64_t handle) override;

  struct Run;

 private:
  SharedPointer<Run> find(uint64_t handle);
  filesystem::IFileSystem& files_;
  allocators::IAllocator& allocator_;
  kernel::Mutex lock_;
  List<SharedPointer<Run>> runs_;
  uint64_t next_handle_ = 1;
};

}  // namespace dbus
