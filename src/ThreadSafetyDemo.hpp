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
#include "Hardware/StorageDevice/SystemStorageDevices.hpp"
#include "Network/NetworkStackSet.hpp"

// Hammers the shared subsystems from several threads at once and checks that
// nothing is corrupted or lost:
//   - allocator: threads allocate, fill, verify and free blocks of random sizes
//   - console: threads print whole lines (a log checker can verify none are torn)
//   - storage: threads read random ranges of every device simultaneously and
//     compare with a reference read; on a scratch device they also write, flush
//     and verify disjoint regions
//   - network (when interfaces exist): concurrent UDP echoes, DNS lookups, pings and
//     TCP echoes through the same stack
// Returns false if any check failed. The echo checks that need a server on the
// host (UDP 7778, TCP 7777 at the gateway) report "no echo server" instead of failing.
bool run_thread_safety_demo(hardware::SystemStorageDevices& storage, network::NetworkStackSet& stacks,
                            allocators::IAllocator& allocator);
