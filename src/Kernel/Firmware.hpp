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

namespace kernel {

// UEFI boot services are not safe to call from several cores, nor from a thread
// that may move between cores. Every firmware call made after kernel::start() goes
// through here: it runs on the boot core, one call at a time.
//
//   kernel::firmware([&] { status = snp->transmit(...); });
//
// A thread pinned to the boot core (the main thread) calls directly under the
// firmware lock; any other thread hands the call to a service thread pinned there
// and waits for it. Before kernel::start() the call is simply made. Must not be
// used from interrupt context or with a spinlock held.

void firmware_call(void (*function)(void* context), void* context);

template <class F>
void firmware(F&& function) {
  firmware_call([](void* context) { (*static_cast<F*>(context))(); }, &function);
}

// True if the calling context may call firmware services directly right now.
bool may_call_firmware_directly();

// Starts the service thread (called by kernel::start()).
bool firmware_service_start();

}  // namespace kernel
