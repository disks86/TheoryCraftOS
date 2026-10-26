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

#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_gt {
namespace gen9_3d {

// The commands of the Gen9 (Skylake to Comet Lake) render command streamer that a 3D batch of this OS uses, and a walker that checks a batch against them. Opcodes
// and field positions follow Mesa's genxml (gen8.xml, gen9.xml) and the intel-gpu-tools Gen9 render-copy test, which runs on hardware.

// A 3D command header: command type 3 (bits 31:29), pipeline / sub-type (28:27), opcode (26:24), sub-opcode (23:16). The low byte of most of them is the number of
// dwords that follow the first, less one.
constexpr uint32_t cmd(uint32_t pipeline, uint32_t opcode, uint32_t sub_opcode) { return (3u << 29) | (pipeline << 27) | (opcode << 24) | (sub_opcode << 16); }

constexpr uint32_t kMiNoop = 0;
constexpr uint32_t kMiBatchBufferEnd = 0x0Au << 23;

constexpr uint32_t kPipelineSelect = cmd(1, 1, 4);       // a single dword
constexpr uint32_t kPipelineSelect3d = kPipelineSelect | (3u << 8) | 0;   // mask 3, select the 3D pipeline
constexpr uint32_t kStateBaseAddress = cmd(0, 1, 1);
constexpr uint32_t kStateSip = cmd(0, 1, 2);
constexpr uint32_t kPipeControl = cmd(3, 2, 0);

constexpr uint32_t kPushConstantAllocVs = cmd(3, 1, 0x12), kPushConstantAllocHs = cmd(3, 1, 0x13), kPushConstantAllocDs = cmd(3, 1, 0x14),
                   kPushConstantAllocGs = cmd(3, 1, 0x15), kPushConstantAllocPs = cmd(3, 1, 0x16);
constexpr uint32_t kViewportStatePointersSfClip = cmd(3, 0, 0x21), kViewportStatePointersCc = cmd(3, 0, 0x23);
constexpr uint32_t kUrbVs = cmd(3, 0, 0x30), kUrbHs = cmd(3, 0, 0x31), kUrbDs = cmd(3, 0, 0x32), kUrbGs = cmd(3, 0, 0x33);
constexpr uint32_t kBlendStatePointers = cmd(3, 0, 0x24), kCcStatePointers = cmd(3, 0, 0x0E);
constexpr uint32_t kMultisample = cmd(3, 0, 0x0D), kSampleMask = cmd(3, 0, 0x18);
constexpr uint32_t kWmHzOp = cmd(3, 0, 0x52);
constexpr uint32_t kConstantVs = cmd(3, 0, 0x15), kConstantGs = cmd(3, 0, 0x16), kConstantPs = cmd(3, 0, 0x17), kConstantHs = cmd(3, 0, 0x19), kConstantDs = cmd(3, 0, 0x1A);
constexpr uint32_t kVs = cmd(3, 0, 0x10), kGs = cmd(3, 0, 0x11), kHs = cmd(3, 0, 0x1B), kTe = cmd(3, 0, 0x1C), kDs = cmd(3, 0, 0x1D), kPs = cmd(3, 0, 0x20);
constexpr uint32_t kBindingTablePointersVs = cmd(3, 0, 0x26), kBindingTablePointersHs = cmd(3, 0, 0x27), kBindingTablePointersDs = cmd(3, 0, 0x28),
                   kBindingTablePointersGs = cmd(3, 0, 0x29), kBindingTablePointersPs = cmd(3, 0, 0x2A);
constexpr uint32_t kSamplerStatePointersVs = cmd(3, 0, 0x2B), kSamplerStatePointersHs = cmd(3, 0, 0x2C), kSamplerStatePointersDs = cmd(3, 0, 0x2D),
                   kSamplerStatePointersGs = cmd(3, 0, 0x2E), kSamplerStatePointersPs = cmd(3, 0, 0x2F);
constexpr uint32_t kStreamout = cmd(3, 0, 0x1E), kClip = cmd(3, 0, 0x12), kSf = cmd(3, 0, 0x13), kWm = cmd(3, 0, 0x14), kSbe = cmd(3, 0, 0x1F), kSbeSwiz = cmd(3, 0, 0x51),
                   kRaster = cmd(3, 0, 0x50), kPsBlend = cmd(3, 0, 0x4D), kWmDepthStencil = cmd(3, 0, 0x4E), kPsExtra = cmd(3, 0, 0x4F);
constexpr uint32_t kScissorStatePointers = cmd(3, 0, 0x0F);
constexpr uint32_t kDepthBuffer = cmd(3, 0, 0x05), kStencilBuffer = cmd(3, 0, 0x06), kHierDepthBuffer = cmd(3, 0, 0x07), kClearParams = cmd(3, 0, 0x04);
constexpr uint32_t kDrawingRectangle = cmd(3, 1, 0);
constexpr uint32_t kVertexBuffers = cmd(3, 0, 8), kVertexElements = cmd(3, 0, 9), kVf = cmd(3, 0, 0x0C), kVfTopology = cmd(3, 0, 0x4B), kVfInstancing = cmd(3, 0, 0x49);
constexpr uint32_t k3dPrimitive = cmd(3, 3, 0);

// Primitive topologies of 3DSTATE_VF_TOPOLOGY.
constexpr uint32_t kTopologyTriangleList = 0x04;
constexpr uint32_t kTopologyRectList = 0x0F;

// PIPE_CONTROL dword 1 flags.
constexpr uint32_t kPcDepthCacheFlush = 1u << 0, kPcStateCacheInvalidate = 1u << 2, kPcConstCacheInvalidate = 1u << 3, kPcVfCacheInvalidate = 1u << 4,
                   kPcDcFlush = 1u << 5, kPcPipeControlFlush = 1u << 7, kPcTextureCacheInvalidate = 1u << 10, kPcInstructionCacheInvalidate = 1u << 11,
                   kPcRenderTargetCacheFlush = 1u << 12, kPcPostSyncWriteImmediate = 1u << 14, kPcTlbInvalidate = 1u << 18, kPcCsStall = 1u << 20,
                   kPcStoreDataIndex = 1u << 21, kPcLriPostSync = 1u << 23, kPcGlobalGtt = 1u << 24;
constexpr uint32_t kPcPostSyncMask = 3u << 14;

// How many dwords the command at the start of `commands` takes, or 0 when it is not one of the commands above (or is longer than what is left, or is a PIPE_CONTROL that
// writes a register or names the global GTT).
uint32_t render_command_length(ArrayView<const uint32_t> commands);

// A batch is a run of the commands above ending in MI_BATCH_BUFFER_END; what follows the end marker is data (state the commands point to) and is not looked at. True if
// every command up to the marker is allowed. `commands_end` gets the number of dwords up to and including the marker.
bool render_batch_is_acceptable(ArrayView<const uint32_t> batch, uint32_t* commands_end = nullptr);

}  // namespace gen9_3d
}  // namespace intel_gt
}  // namespace hardware
