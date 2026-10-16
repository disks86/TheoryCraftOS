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
#include "Allocators/Allocator.hpp"
#include "CPUScheduler/ICPUScheduler.hpp"

// Exercises UniquePointer, SharedPointer, Optional and Variant: ownership of a
// derived object through a base without a virtual destructor, a SharedPointer
// copied and dropped concurrently on every core, and value types holding
// nontrivial contents. Logs one line per check and verifies that the allocator
// ends with the memory it started with. Returns true if everything passed.
bool run_utility_demo(allocators::Allocator& allocator, scheduling::ICPUScheduler& scheduler);
