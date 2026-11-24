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

// NtTcosVirtGpu(PULONG64 arguments): this OS's own service (514), the virtio-gpu render node for Windows programs (the Venus backend of vulkan-1.dll). The
// operations and their layout are in src/Process/VirtGpuDevice.h (TCOS_VIRTGPU_OP_*); the sessions are graphics::virtgpu::Session, the same object a Linux
// program's /dev/dri/renderD128 descriptor holds. A handle belongs to the process that opened it; its sessions go when the process ends.
#pragma once
#include <stdint.h>
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

namespace process::windowsabi {

// Set up once at start-up with the kernel's shared allocator (the sessions and their buffers come from it). Until then OPEN is STATUS_NO_SUCH_DEVICE.
bool install_virtgpu_service(allocators::IAllocator& allocator);
uint32_t nt_tcos_virtgpu(Process& p, const SyscallRequest& r);
// A process ended: its sessions go (the mappings went with its address space).
void virtgpu_process_exited(uint64_t pid);
// How many sessions are open (tests).
uint32_t virtgpu_session_count();

}  // namespace process::windowsabi
