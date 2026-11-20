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
#include "Process/AddressSpace.hpp"
#include "Process/ImageLoader.hpp"
#include "Process/Process.hpp"
#include "Utility/ArrayView.hpp"

namespace process {

// The Linux vDSO: a small shared object every x86-64 Linux process gets (AT_SYSINFO_EHDR), whose clock_gettime, gettimeofday, time and clock_getres read
// the time from the timestamp counter in user mode instead of making a system call (see LinuxVdso/vdso_x86_64.c and docs/LinuxVdso.md). Below its first
// page is a read-only data page (LinuxVdso/vdso_data.h) with the host's UserClock; a host without one leaves its rate 0 and the vDSO makes the system calls.

// The image for programs of `arch` (empty when there is none: only x86-64 has one).
ConstByteView linux_vdso_image(UserArch arch);
// Where the vDSO goes when there is room (the data page is the page before): well below the stack, above the mmap area's usual reach.
constexpr uint64_t kLinuxVdsoHint = 0x00007FFFF7FF0000ull;
// The data page's bytes for `clock` (nullptr: no user clock, the rate is 0). `page` must hold at least TCOS_VDSO_DATA_BYTES; the rest is zeroed.
[[nodiscard]] bool fill_linux_vdso_data(const UserClock* clock, ByteView page);
// Maps the data page and the image of `arch` into `space` and fills the data from `host`. *image_base is the image's address (AT_SYSINFO_EHDR).
// Unsupported when `arch` has no vDSO; on any failure nothing stays mapped.
[[nodiscard]] LoadStatus map_linux_vdso(AddressSpace& space, UserArch arch, IProcessHost& host, uint64_t* image_base);
// Writes the data page of the vDSO at `image_base` again from `host` (a forked child's copy carries its parent's host's clock).
[[nodiscard]] bool refresh_linux_vdso_data(AddressSpace& space, uint64_t image_base, IProcessHost& host);

}  // namespace process
