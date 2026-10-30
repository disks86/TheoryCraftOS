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

#include "Graphics/RenderNode.hpp"

namespace graphics {

RenderNode::RenderNode(hardware::IGraphicsProcessingUnit* gpu, hardware::VirtioGpu* virtio, hardware::VmwareSvgaGpu* vmware, allocators::IAllocator& allocator,
                       bool allow_acceleration, hardware::IntelGpu* intel, bool allow_intel_render, hardware::AmdVegaM* vega)
    : gpu_(gpu), software_(allocator, gpu) {
  active_ = &software_;
  build_accelerated(virtio, vmware, intel, vega, allocator, allow_acceleration, allow_intel_render);
}

RenderNode::~RenderNode() { destroy_accelerated(); }

}  // namespace graphics
