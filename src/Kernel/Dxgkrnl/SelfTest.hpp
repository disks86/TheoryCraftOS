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

// A Direct3D first triangle through the kernel-mode driver, to find out whether a 3D device does what a Direct3D application needs of it: the same Direct3D 9 shader pair, pipeline and
// draw every device is given (vs_2_0 `m4x4 oPos, v0, c0 ; mov oD0, v1`, ps_2_0 `mov oC0, v0`; position float3, colour float4, an identity matrix). Every device takes it in some form -
// the interpreter's function, TGSI text, the Direct3D 9 tokens, or one of its fixed kernels (Adapter::create_shader hands each every form).
//
// The system runs it on each accelerated GPU before it keeps the GPU's device for Direct3D (SystemDxg.cpp) and falls back to the CPU renderer for a device that fails; the host tests
// run it on every device they can simulate and compare the picture with the software device's (tests/DxgkrnlDeviceSuite.hpp). A new device (the AMD one) is held to the same.
#pragma once
#include "Kernel/Dxgkrnl/DxgKernel.hpp"

namespace dxgk {

constexpr uint32_t kSelfTestSize = 32;           // the picture is kSelfTestSize x kSelfTestSize
constexpr ProcessId kSelfTestProcess = 0x7FFFFFF0;

// Draws the triangle on adapter `adapter_index` of `kernel` and reads the picture (B8G8R8A8 words, rows top to bottom; kSelfTestSize squared of them). Returns the status of the
// first step that failed (the step is named in `step` when it is not null), or kSuccess. Cleans up after itself.
NtStatus self_test_render(allocators::IAllocator& allocator, Kernel& kernel, uint32_t adapter_index, ArrayView<uint32_t> pixels, const char** step = nullptr);

// Does the picture look right: the clear colour in the corner and something lit in the middle.
bool self_test_picture_ok(ArrayView<const uint32_t> pixels);

// Both: the triangle is drawn and looks right.
bool self_test(allocators::IAllocator& allocator, Kernel& kernel, uint32_t adapter_index);

}  // namespace dxgk
