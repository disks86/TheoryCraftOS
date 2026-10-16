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
#include "Hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"

// Fills the Concurrent* containers from scheduler tasks running on every core at
// once, then checks nothing was lost, duplicated or leaked. `shared` must be the
// thread-safe front of `allocator` that the rest of the system uses (the default
// allocator is not safe to share between cores). Logs one line per check and
// returns true if all of them pass.
bool run_concurrent_demo(allocators::Allocator& allocator, allocators::IAllocator& shared,
                         hardware::ICentralProcessingUnit& cpu, scheduling::ICPUScheduler& scheduler);
