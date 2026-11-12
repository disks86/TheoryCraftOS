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

// Connects the kernel's OpenGL to Windows programs (NtTcosOpenGl) and Linux programs (/dev/dxg's TCOS_DXG_IOCTL_GL), one request layout for both
// (src/Process/WindowsOpenGl.hpp).
#pragma once
#include "OpenGl/GlService.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

namespace opengl {

uint32_t windows_gl_service(process::Process& p, const process::SyscallRequest& r);
// Runs the request at `request` for the process: memory handed to a Linux program comes from its mmap area, and a Linux window is never shown by the kernel.
uint32_t gl_request(process::Process& p, uint64_t request, bool linux_process);
// Makes `service` the system's OpenGL and the one Windows programs reach.
void install_windows_gl(GlService& service);
// Makes `service` the system's OpenGL and the one Linux programs reach.
void install_linux_gl(GlService& service);

}  // namespace opengl
