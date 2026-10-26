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

#include "Hardware/GraphicsProcessingUnit/Multi/GpuPool.hpp"

namespace hardware {


GpuPool::~GpuPool() {
  for (uint32_t i = 0; i < manager_.count(); ++i) {
    GpuNode& n = nodes_[i];
    if (n.destroy) n.destroy(n, allocator_);
    if (n.leased_for_driver) manager_.release(i);
  }
}

void GpuPool::initialize() {
  manager_.initialize();
  for (uint32_t i = 0; i < GpuTopology::kMaxAdapters; ++i) nodes_[i] = GpuNode{};
  for (uint32_t i = 0; i < manager_.count(); ++i) nodes_[i].adapter = manager_.topology().adapter(i);
}

bool GpuPool::adopt(uint32_t index, IGraphicsProcessingUnit* gpu, VirtioGpu* virtio, VmwareSvgaGpu* vmware) {
  if (index >= manager_.count() || !gpu || nodes_[index].gpu) return false;
  nodes_[index].gpu = gpu;
  nodes_[index].virtio = virtio;
  nodes_[index].vmware = vmware;
  return true;
}

uint32_t GpuPool::driver_count() const {
  uint32_t n = 0;
  for (uint32_t i = 0; i < manager_.count(); ++i)
    if (nodes_[i].gpu) ++n;
  return n;
}

}  // namespace hardware
