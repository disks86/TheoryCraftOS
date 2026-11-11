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

// The compositor's GPU images over the kernel's Direct3D layer: a dmabuf a client passed (an export of /dev/dxg, Process/DxgBufferFile.hpp) becomes a
// wayland::GpuImage the desktop draws. Kept apart from the rest of src/Wayland so that the compositor builds without the Direct3D code (the host tests and the
// interop harness do); the kernel gives make_dxg_image to the transport (SocketServer::set_gpu_images).
//
// Drawing: the device's own image is lent (Kernel::acquire_export_image) and drawn in place when the desktop's renderer draws on that device - no copy at all;
// otherwise the picture is drawn from the CPU's view of it (Kernel::read_export: one copy, as a wl_shm buffer costs). A renderer that refused the device's image
// once is given the CPU's view after that, without asking again.
//
// Locking: the compositor calls in under its mutex (UI lock -> compositor -> the Direct3D layer's lock: the layer never calls out while holding its own).
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Process/ProcessFiles.hpp"
#include "Wayland/WaylandServer.hpp"

namespace dxgk {
class Kernel;
}

namespace wayland {

// A GpuImage for `object` when it is a GPU image of `kernel` (a dxgk::Kernel); null otherwise. (A SocketServer::MakeGpuImage.)
SharedPointer<GpuImage> make_dxg_image(void* kernel, allocators::IAllocator& allocator, const SharedPointer<process::OpenObject>& object);

}  // namespace wayland
