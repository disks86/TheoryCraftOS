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

// Connects the kernel-mode Direct3D driver (Kernel/Dxgkrnl) to the machine's GPUs: one D3D adapter for each GPU of the graphics pool, backed by that GPU's 3D device (its
// 3D engine where it has one we can drive, the CPU renderer otherwise), with the GPU's display driver for mode setting and its video memory size for the budget.
#pragma once
#include "Graphics/GraphicsPool.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"

namespace graphics {

// Adds the adapters; returns how many were added. The pool must outlive the kernel. An accelerated GPU is first held to the Direct3D self-test (dxgk::self_test): one whose device
// cannot draw the Direct3D 9 triangle is given the CPU renderer for Direct3D instead (counted in `*fallbacks` when it is not null).
uint32_t register_dxg_adapters(dxgk::Kernel& kernel, GraphicsPool& pool, allocators::IAllocator& allocator, uint32_t* fallbacks = nullptr);

}  // namespace graphics
