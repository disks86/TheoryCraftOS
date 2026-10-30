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
  kBlit = 16,
  kBindSamplerStates = 18,
  kBeginQuery = 19,
  kEndQuery = 20,
  kGetQueryResult = 21,
  kBindShader = 31,
  kSetShaderBuffers = 34,
  kMemoryBarrier = 36,
  kLaunchGrid = 37,
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
constexpr uint32_t kFormatR16G16B16A16Unorm = 51;   // (VIRGL_FORMAT_*, the numbering of Gallium's pipe_format)
constexpr uint32_t kFormatB8G8R8A8Srgb = 100;
constexpr uint32_t kFormatR32G32B32A32Uint = 196;
constexpr uint32_t kFormatR32G32B32A32Sint = 200;
constexpr uint32_t kFormatR16G16B16A16Uint = 188;
constexpr uint32_t kFormatR16G16B16A16Sint = 192;
constexpr uint32_t kFormatR8G8B8A8Uint = 180;
constexpr uint32_t kFormatR16G16B16A16Snorm = 59;
constexpr uint32_t kFormatR10G10B10A2Unorm = 8;

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
constexpr uint32_t kQueryOcclusionPredicate = 1;   // PIPE_QUERY_OCCLUSION_PREDICATE
constexpr uint32_t kQueryPipelineStatistics = 10;  // PIPE_QUERY_PIPELINE_STATISTICS (the enum of virglrenderer's p_defines.h: occlusion counter 0, predicate 1, timestamp 2, disjoint 3, time elapsed 4, primitives generated 5, emitted 6, SO statistics 7, SO overflow 8, GPU finished 9, pipeline statistics 10): the statistic is the query index, in the upper half of the word (PIPE_STAT_QUERY_*: IA_VERTICES 0, IA_PRIMITIVES 1, VS_INVOCATIONS 2, PS_INVOCATIONS 7)
constexpr uint32_t kQueryStateDone = 1;         // virgl_host_query_state::query_state once the host wrote the result (state, size, 64-bit result: 16 bytes)
constexpr uint32_t kTargetBuffer = 0;
constexpr uint32_t kTargetTexture2d = 2, kTargetTexture3d = 3, kTargetTextureCube = 4, kTargetTexture2dArray = 7;   // (PIPE_TEXTURE_*)
constexpr uint32_t kShaderTessControl = 3, kShaderTessEval = 4;   // (PIPE_SHADER_TESS_CTRL, TESS_EVAL)
constexpr uint32_t kPrimPatches = 14;   // (PIPE_PRIM_PATCHES; the draw command then has two more words, the control points of a patch and the draw id)
constexpr uint32_t kShaderGeometry = 2;   // (PIPE_SHADER_GEOMETRY)
constexpr uint32_t kShaderCompute = 5;   // (PIPE_SHADER_COMPUTE)
constexpr uint32_t kBindShaderBuffer = 1u << 14;   // a buffer compute shaders read and write
constexpr uint32_t kShaderVertex = 0;
constexpr uint32_t kShaderFragment = 1;
constexpr uint32_t kPrimPoints = 0, kPrimLines = 1, kPrimTriangles = 4, kPrimLinesAdjacency = 10, kPrimTrianglesAdjacency = 12;
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
