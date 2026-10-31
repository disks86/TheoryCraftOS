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
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

// The NT services that describe the machine (WindowsSyscalls.cpp maps service numbers to these). Each returns an NTSTATUS.
namespace process::windowsabi {

// NtQuerySystemInformation(Class, Buffer, Length, ReturnLength): SystemBasicInformation (0), SystemProcessorInformation (1), SystemPerformanceInformation (2: the memory
// figures), SystemTimeOfDayInformation (3).
uint32_t nt_query_system_information(Process& p, const SyscallRequest& r);
// NtQuerySystemInformationEx(Class, Input, InputLength, Buffer, Length, ReturnLength): the logical processors (107) and the CPU sets (175), else as NtQuerySystemInformation.
uint32_t nt_query_system_information_ex(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
