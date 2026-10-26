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

// What GraphicsPool needs of whoever already renders on the display GPU (SystemGraphics): that GPU's 3D back end and its 2D renderer. The pool reuses
// them instead of building second ones on the same GPU.
#pragma once
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "Graphics/RenderNode.hpp"

namespace graphics {

class IDisplayGraphics {
 public:
  virtual RenderNode& render_node() = 0;
  virtual IRenderer2D* renderer_2d() = 0;

 protected:
  constexpr IDisplayGraphics() = default;
  ~IDisplayGraphics() = default;
};

}  // namespace graphics
