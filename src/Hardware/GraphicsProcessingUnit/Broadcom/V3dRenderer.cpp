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

#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dRenderer.hpp"

namespace hardware {

bool V3dRenderer::memory_needed(const V3dFrameDesc& frame, const V3dRenderLayout& layout, uint64_t* bytes) {
  V3dTileGeometry g;
  if (!V3dFrameBuilder::geometry(frame.width, frame.height, &g) || !valid_layout(layout)) return false;
  const uint64_t state = (V3dFrameBuilder::tile_state_bytes(g) + 4095) & ~uint64_t{4095};
  *bytes = uint64_t{layout.binner_bytes} + 2 * uint64_t{kListBytes} + layout.indirect_bytes + state + V3dFrameBuilder::tile_alloc_bytes(g);
  return true;
}

V3dJobResult V3dRenderer::render(V3dGpu& gpu, const V3dFrameDesc& frame, IV3dJobMemory& memory, uint32_t attempts, ArrayView<const V3dMemoryBlock> overflow,
                                 const V3dRenderLayout& layout, IV3dBinningContent* content) {
  V3dTileGeometry g;
  uint64_t needed = 0;
  ArrayView<uint8_t> bytes = memory.bytes();
  const uint32_t base = memory.gpu_address();
  if (!V3dFrameBuilder::geometry(frame.width, frame.height, &g) || !memory_needed(frame, layout, &needed) || bytes.size() < needed || (base & 4095) || needed > UINT32_MAX - base)
    return V3dJobResult::Error;

  const uint32_t binner_at = 0, renderer_at = layout.binner_bytes, tile_list_at = renderer_at + kListBytes, indirect_at = tile_list_at + kListBytes;
  const uint32_t state_at = indirect_at + layout.indirect_bytes;
  const uint32_t state_bytes = static_cast<uint32_t>((V3dFrameBuilder::tile_state_bytes(g) + 4095) & ~uint64_t{4095});
  const uint32_t alloc_at = state_at + state_bytes;
  V3dControlList bcl(bytes.subview(binner_at, layout.binner_bytes), base + binner_at);
  V3dControlList rcl(bytes.subview(renderer_at, kListBytes), base + renderer_at);
  V3dControlList generic(bytes.subview(tile_list_at, kListBytes), base + tile_list_at);
  V3dControlList indirect(bytes.subview(indirect_at, layout.indirect_bytes), base + indirect_at);

  V3dFrameBuilder::begin_binning(bcl, frame);
  if (content && !content->emit(bcl, indirect)) return V3dJobResult::Error;
  V3dFrameBuilder::end_binning(bcl);
  if (!bcl.ok() || !indirect.ok() || !V3dFrameBuilder::build_render_list(rcl, generic, frame, g, base + alloc_at)) return V3dJobResult::Error;
  memory.clean();

  V3dBinJob bin;
  bin.start = bcl.gpu_address();
  bin.end = bcl.gpu_end();
  bin.tile_alloc = {base + alloc_at, static_cast<uint32_t>(V3dFrameBuilder::tile_alloc_bytes(g))};
  bin.tile_state = base + state_at;
  V3dJobResult result = gpu.run_binning(bin, attempts, overflow);
  if (result != V3dJobResult::Done) return result;
  result = gpu.run_rendering(rcl.gpu_address(), rcl.gpu_end(), attempts);
  memory.invalidate();
  return result;
}

}  // namespace hardware
