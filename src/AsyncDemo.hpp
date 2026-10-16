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
#include "Hardware/Clock/IClock.hpp"

// Exercises async/await: tasks that await each other, delays that overlap (three
// 20 ms waits run together in about 20 ms, not 60), when_all, an AsyncMutex held
// across an await, an AsyncEvent, and streams used with co_await (memory to memory
// with copy_to_async, and a cancelled read). Uses `clock` for time and takes the
// coroutine frames from `allocator` (which must be thread safe). Returns false if
// any check failed.
bool run_async_demo(hardware::IClock& clock, allocators::IAllocator& allocator);
