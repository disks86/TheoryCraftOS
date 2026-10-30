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
#include "Graphics/SystemGraphics.hpp"

// Exercises the renderers on the whole screen: first the 2D renderer (shapes,
// alpha, text), then a spinning, depth-tested cube through the 3D device chosen
// by SystemGraphics - the GPU one where there is one, else the software one -
// with a readback check of the first frame. Reports on the console and returns
// false if anything failed. The screen is left showing the last frame: re-attach
// the console afterwards.
bool run_graphics_demo(graphics::SystemGraphics& graphics, allocators::IAllocator& allocator);

// Draws offscreen groups with the 2D renderer on `device` and reads them back: 1 when they come out right (group opacity, moved pictures), 2 when the group filters (blur, colour functions) came out right too, 3 when rounded and mask clips came out exact as well, 4 when drop shadows came out right as well, 0 when wrong, -1 when the
// device does not do them. Needs no screen.
int check_2d_groups(graphics::IGraphicsDevice& device, allocators::IAllocator& allocator);

// Draws glyphs with the 2D renderer's glyph atlas on `device` and reads them back: 1 when they come out right (kept on the atlas, tinted by the GPU, in order with other
// shapes), 0 when wrong, -1 when the device has no atlas.
int check_2d_glyphs(graphics::IGraphicsDevice& device, allocators::IAllocator& allocator);
