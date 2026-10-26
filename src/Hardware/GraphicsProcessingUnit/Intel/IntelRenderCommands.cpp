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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelRenderCommands.hpp"

namespace hardware {
namespace intel_gt {
namespace gen9_3d {

namespace {
// The headers (bits 31:16) of every command a 3D batch may hold, apart from the single-dword ones.
constexpr uint32_t kAllowed[] = {
    kStateBaseAddress, kStateSip, kPipeControl,
    kPushConstantAllocVs, kPushConstantAllocHs, kPushConstantAllocDs, kPushConstantAllocGs, kPushConstantAllocPs,
    kViewportStatePointersSfClip, kViewportStatePointersCc, kUrbVs, kUrbHs, kUrbDs, kUrbGs, kBlendStatePointers, kCcStatePointers, kMultisample, kSampleMask, kWmHzOp,
    kConstantVs, kConstantGs, kConstantPs, kConstantHs, kConstantDs, kVs, kGs, kHs, kTe, kDs, kPs,
    kBindingTablePointersVs, kBindingTablePointersHs, kBindingTablePointersDs, kBindingTablePointersGs, kBindingTablePointersPs,
    kSamplerStatePointersVs, kSamplerStatePointersHs, kSamplerStatePointersDs, kSamplerStatePointersGs, kSamplerStatePointersPs,
    kStreamout, kClip, kSf, kWm, kSbe, kSbeSwiz, kRaster, kPsBlend, kWmDepthStencil, kPsExtra, kScissorStatePointers,
    kDepthBuffer, kStencilBuffer, kHierDepthBuffer, kClearParams, kDrawingRectangle,
    kVertexBuffers, kVertexElements, kVf, kVfTopology, kVfInstancing, k3dPrimitive,
};
constexpr uint32_t kMaxLength = 256;   // the longest command of the set is 3DSTATE_VERTEX_ELEMENTS (up to 34 elements) and SBE_SWIZ: far less
}  // namespace

uint32_t render_command_length(ArrayView<const uint32_t> commands) {
  if (commands.empty()) return 0;
  const uint32_t h = commands[0];
  if (h == kMiNoop || h == kMiBatchBufferEnd) return 1;
  if ((h & 0xFFFF0000u) == kPipelineSelect) return h == kPipelineSelect3d ? 1 : 0;
  const uint32_t top = h & 0xFFFF0000u;
  bool known = false;
  for (const uint32_t allowed : kAllowed)
    if (top == allowed) known = true;
  if (!known) return 0;
  const uint32_t length = (h & 0xFFu) + 2;
  if (length > kMaxLength || length > commands.size()) return 0;
  if (top == kPipeControl) {
    // Six dwords, no register write, and the address in the batch's own address space.
    if (length != 6 || (commands[1] & (kPcLriPostSync | kPcGlobalGtt))) return 0;
  }
  if (top == kStateBaseAddress && length != 18 && length != 19) return 0;
  return length;
}

bool render_batch_is_acceptable(ArrayView<const uint32_t> batch, uint32_t* commands_end) {
  uint64_t i = 0;
  while (i < batch.size()) {
    const uint32_t length = render_command_length(batch.subview(i, batch.size() - i));
    if (length == 0) return false;
    i += length;
    if (batch[i - 1] == kMiBatchBufferEnd && length == 1) {
      if (commands_end) *commands_end = static_cast<uint32_t>(i);
      return true;
    }
  }
  return false;  // no end marker
}

}  // namespace gen9_3d
}  // namespace intel_gt
}  // namespace hardware
