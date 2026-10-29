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

// The slice of the virgl command protocol (virglrenderer's virgl_protocol.h /
// virgl_hw.h, which in turn follow Gallium's state model) that VirglDevice uses.
#pragma once
#include <stdint.h>

namespace graphics::virgl {

enum Command : uint32_t {
  kCreateObject = 1,
  kBindObject,
  kDestroyObject,
  kSetViewportState,
  kSetFramebufferState,
  kSetVertexBuffers,
  kClear,
  kDrawVbo,
  kResourceInlineWrite,
  kSetSamplerViews,
  kSetIndexBuffer,
  kSetConstantBuffer,
  kSetStencilRef,
  kSetBlendColor,
  kSetScissorState,
  kBindSamplerStates = 18,
  kBeginQuery = 19,
  kEndQuery = 20,
  kGetQueryResult = 21,
  kBindShader = 31,
};

enum ObjectType : uint32_t {
  kObjectBlend = 1,
  kObjectRasterizer,
  kObjectDsa,
  kObjectShader,
  kObjectVertexElements,
  kObjectSamplerView,
  kObjectSamplerState,
  kObjectSurface,
  kObjectQuery,
};

constexpr uint32_t header(uint32_t command, uint32_t object, uint32_t length_dwords) {
  return command | (object << 8) | (length_dwords << 16);
}

// Formats (VIRGL_FORMAT_*).
constexpr uint32_t kFormatB8G8R8A8Unorm = 1;
constexpr uint32_t kFormatZ32Float = 18;
constexpr uint32_t kFormatZ32FloatS8X24Uint = 126;   // (a depth image with a stencil plane; 19, the number the name Z24_UNORM_S8_UINT suggests, makes virglrenderer lose the context)
constexpr uint32_t kFormatR8Unorm = 64;
constexpr uint32_t kFormatR32Float = 28;
constexpr uint32_t kFormatR32G32Float = 29;
constexpr uint32_t kFormatR32G32B32Float = 30;
constexpr uint32_t kFormatR32G32B32A32Float = 31;
constexpr uint32_t kFormatR16G16B16A16Float = 94;

// Resource bind flags (VIRGL_BIND_*).
constexpr uint32_t kBindDepthStencil = 1u << 0;
constexpr uint32_t kBindRenderTarget = 1u << 1;
constexpr uint32_t kBindSamplerView = 1u << 3;
constexpr uint32_t kBindVertexBuffer = 1u << 4;
constexpr uint32_t kBindIndexBuffer = 1u << 5;
constexpr uint32_t kBindCustom = 1u << 17;   // a buffer kept in the host's system memory: where query results are written (read back with a transfer)
constexpr uint32_t kBindScanout = 1u << 18;

// Gallium enums.
constexpr uint32_t kQueryOcclusionCounter = 0;   // PIPE_QUERY_OCCLUSION_COUNTER
constexpr uint32_t kQueryStateDone = 1;         // virgl_host_query_state::query_state once the host wrote the result (state, size, 64-bit result: 16 bytes)
constexpr uint32_t kTargetBuffer = 0;
constexpr uint32_t kTargetTexture2d = 2;
constexpr uint32_t kShaderVertex = 0;
constexpr uint32_t kShaderFragment = 1;
constexpr uint32_t kPrimPoints = 0, kPrimLines = 1, kPrimTriangles = 4;
constexpr uint32_t kFuncLess = 1, kFuncLequal = 3, kFuncAlways = 7;
constexpr uint32_t kBlendAdd = 0;   // (PIPE_BLEND_ADD, SUBTRACT, REVERSE_SUBTRACT, MIN, MAX: 0 .. 4, as BlendOp)
constexpr uint32_t kBlendFactors[] = {0x11, 1, 2, 0x12, 3, 0x13, 5, 0x15, 4, 0x14, 6};   // by BlendFactor: ZERO ONE SRC_COLOR INV_SRC_COLOR SRC_ALPHA INV_SRC_ALPHA DST_COLOR INV_DST_COLOR DST_ALPHA INV_DST_ALPHA SRC_ALPHA_SATURATE
constexpr uint32_t kStencilOps[] = {0, 1, 2, 3, 4, 7, 5, 6};   // by StencilOp (Keep Zero Replace IncrementClamp DecrementClamp Invert IncrementWrap DecrementWrap): PIPE_STENCIL_OP_*
constexpr uint32_t kBlendFactorOne = 1, kBlendFactorSrcAlpha = 3, kBlendFactorInvSrcAlpha = 0x13;
constexpr uint32_t kClearDepth = 1u << 0, kClearStencil = 1u << 1, kClearColor0 = 1u << 2;
// Sampler state (the S0 dword of a sampler state object) and sampler view swizzle.
constexpr uint32_t kWrapRepeat = 0, kWrapClampToEdge = 2;
constexpr uint32_t kFilterNearest = 0, kFilterLinear = 1;
constexpr uint32_t kMipFilterNone = 2;
constexpr uint32_t kSwizzleRgba = 0 | (1u << 3) | (2u << 6) | (3u << 9);
constexpr uint32_t kFaceNone = 0, kFaceFront = 1, kFaceBack = 2;

}  // namespace graphics::virgl
