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
};

constexpr uint32_t header(uint32_t command, uint32_t object, uint32_t length_dwords) {
  return command | (object << 8) | (length_dwords << 16);
}

// Formats (VIRGL_FORMAT_*).
constexpr uint32_t kFormatB8G8R8A8Unorm = 1;
constexpr uint32_t kFormatZ32Float = 18;
constexpr uint32_t kFormatR8Unorm = 64;
constexpr uint32_t kFormatR32Float = 28;
constexpr uint32_t kFormatR32G32Float = 29;
constexpr uint32_t kFormatR32G32B32Float = 30;
constexpr uint32_t kFormatR32G32B32A32Float = 31;

// Resource bind flags (VIRGL_BIND_*).
constexpr uint32_t kBindDepthStencil = 1u << 0;
constexpr uint32_t kBindRenderTarget = 1u << 1;
constexpr uint32_t kBindSamplerView = 1u << 3;
constexpr uint32_t kBindVertexBuffer = 1u << 4;
constexpr uint32_t kBindIndexBuffer = 1u << 5;
constexpr uint32_t kBindScanout = 1u << 18;

// Gallium enums.
constexpr uint32_t kTargetBuffer = 0;
constexpr uint32_t kTargetTexture2d = 2;
constexpr uint32_t kShaderVertex = 0;
constexpr uint32_t kShaderFragment = 1;
constexpr uint32_t kPrimTriangles = 4;
constexpr uint32_t kFuncLess = 1, kFuncLequal = 3, kFuncAlways = 7;
constexpr uint32_t kBlendAdd = 0;
constexpr uint32_t kBlendFactorOne = 1, kBlendFactorSrcAlpha = 3, kBlendFactorInvSrcAlpha = 0x12;
constexpr uint32_t kClearDepth = 1u << 0, kClearColor0 = 1u << 2;
constexpr uint32_t kFaceNone = 0, kFaceFront = 1, kFaceBack = 2;

}  // namespace graphics::virgl
