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

#include "Graphics/SystemDxg.hpp"

namespace graphics {

uint32_t register_dxg_adapters(dxgk::Kernel& kernel, GraphicsPool& pool) {
  uint32_t added = 0;
  for (uint32_t i = 0; i < pool.count(); ++i) {
    RenderNode* node = pool.render_node(i);
    if (!node) continue;
    dxgk::AdapterConfig config;
    config.display = node->gpu();
    uint64_t total = 0, used = 0;
    // The budget is the GPU's own memory when it says how much it has; the integrated and virtual GPUs use system memory, for which the default budget stands.
    if (config.display && config.display->video_memory(&total, &used) && total) config.local_memory_budget = total;
    if (kernel.add_adapter(node->device(), config) != dxgk::kInvalidIndex) ++added;
  }
  return added;
}

}  // namespace graphics
