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
#include "Kernel/Dxgkrnl/SelfTest.hpp"
#include "Utility/AllocatedView.hpp"

namespace graphics {

bool register_dxg_node(dxgk::Kernel& kernel, RenderNode& node, allocators::IAllocator& allocator, bool software_fallback, bool* fell_back, DxgFallback* why, uint64_t shared_memory) {
  if (fell_back) *fell_back = false;
  dxgk::AdapterConfig config;
  config.display = node.gpu();
  uint64_t total = 0, used = 0;
  // The budget is the GPU's own memory when it says how much it has; the integrated and virtual GPUs use system memory: their share of it (`shared_memory`), or the
  // default budget. (A virtual GPU's objects are the host's; what the guest keeps of them is their backing, counted by system_memory_limit.)
  if (config.display && config.display->video_memory(&total, &used) && total) {
    config.local_memory_budget = total;
  } else if (shared_memory > config.local_memory_budget) {
    config.local_memory_budget = shared_memory;
    if (shared_memory > config.system_memory_limit) config.system_memory_limit = shared_memory;
  }
  IGraphicsDevice* device = &node.device();
  if (node.accelerated()) {
    // The device draws a Direct3D triangle on a kernel of its own first (nothing of it is left behind); if it cannot, the CPU renderer is this adapter's device.
    dxgk::Kernel probe(allocator);
    bool passed = probe.add_adapter(*device, config) != dxgk::kInvalidIndex;
    if (passed) {
      const uint64_t count = uint64_t{dxgk::kSelfTestSize} * dxgk::kSelfTestSize;
      ArrayView<uint32_t> pixels = allocate_view<uint32_t>(allocator, count);
      DxgFallback result;
      passed = !pixels.empty() &&
               (result.status = dxgk::self_test_render(allocator, probe, 0, pixels, &result.step)) == dxgk::status::kSuccess;
      if (passed && !dxgk::self_test_picture_ok(pixels)) passed = false, result.picture_wrong = true;
      if (!pixels.empty()) release_view(allocator, pixels);
      if (!passed && why) *why = result;
    }
    if (!passed) {
      if (fell_back) *fell_back = true;
      if (!software_fallback) return false;
      device = &node.software_device();
    }
  }
  // The CPU renderer keeps its images in system memory: a framebuffer-only card's memory size (a few megabytes of scanout on the standard VGA) is no
  // budget for it - with 3 MB, one 640 x 480 depth buffer and two targets in a list left no room and every pass after that was refused.
  if (device == &node.software_device()) config.local_memory_budget = dxgk::AdapterConfig().local_memory_budget;
  return kernel.add_adapter(*device, config) != dxgk::kInvalidIndex;
}

uint32_t register_dxg_adapters(dxgk::Kernel& kernel, GraphicsPool& pool, allocators::IAllocator& allocator, uint32_t* fallbacks, DxgFallback* why, uint64_t shared_memory) {
  uint32_t added = 0;
  if (fallbacks) *fallbacks = 0;
  for (uint32_t i = 0; i < pool.count(); ++i) {
    RenderNode* node = pool.render_node(i);
    if (!node) continue;
    bool fell_back = false;
    if (register_dxg_node(kernel, *node, allocator, true, &fell_back, why, shared_memory)) ++added;
    if (fell_back && fallbacks) ++*fallbacks;
  }
  return added;
}

}  // namespace graphics
