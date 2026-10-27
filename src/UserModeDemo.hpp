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

// Runs two tiny Linux programs in ring 3 (see UserModeDemo.cpp): one that writes to the console, maps memory and exits with a code, one that touches an
// unmapped address. Checks the output, the exit codes and that the kernel survives the fault. Returns false if any check failed.
bool run_usermode_demo(allocators::IAllocator& allocator);

// Runs one Windows program (TCOS_WINE_TEST_PROGRAM), the way the shell starts it, and prints its output and exit code. Unlike run_usermode_demo(), the
// caller decides when: running a PE program corrupts firmware's own state in a way that later crashes a Boot Services call (see the project's
// "usermode-windows" memory for the investigation), so this must run after all hardware bring-up - in particular after every detach_firmware_drivers()
// caller (network and storage device init) - not before it, the way the full demo does.
bool run_wine_test_program(allocators::IAllocator& allocator);
