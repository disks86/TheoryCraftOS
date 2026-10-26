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

#include "Graphics/SystemGraphics.hpp"

namespace graphics {

SystemGraphics::SystemGraphics(hardware::SystemGpu& gpu, allocators::IAllocator& allocator, bool allow_acceleration, bool allow_intel_render)
    : display_(gpu.gpu()), node_(gpu.gpu(), gpu.virtio_gpu(), gpu.vmware_gpu(), allocator, allow_acceleration, gpu.intel_gpu(), allow_intel_render), renderer_(allocator) {}

}  // namespace graphics
