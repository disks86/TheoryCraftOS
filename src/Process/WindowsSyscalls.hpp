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

namespace process {

// The service numbers of the NT native interface, as this OS's ntdll passes them in EAX. They are ours to define: real Windows renumbers its services in
// almost every build, which is why programs call ntdll instead of SYSCALL.
enum class WindowsSyscallNumber : uint32_t {
  NtReadFile = 0,
  NtWriteFile = 1,
  NtClose = 2,
  NtTerminateProcess = 3,
  NtYieldExecution = 4,
  NtDelayExecution = 5,
  NtQuerySystemTime = 6,
  NtAllocateVirtualMemory = 7,
  NtFreeVirtualMemory = 8,
  NtProtectVirtualMemory = 9,
};

constexpr uint32_t kStatusInvalidSystemService = 0xC000001Cu;      // the answer for a number that is no service

}  // namespace process
