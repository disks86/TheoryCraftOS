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

// What the renderer, the layout and the page view need to know about the pictures, videos and canvases of a page (PageObjects, in Objects.hpp, is what
// answers).
#pragma once
#include "Dom/Dom.hpp"
#include "Graphics/Graphics2D/IRenderer2D.hpp"

namespace browser {

using graphics::Rect2D;

enum class ObjectKind : uint8_t { None = 0, Image = 1, Video = 2, Canvas = 3, Audio = 4, Svg = 5 };   // (an Audio is the control bar of an <audio controls>)

struct ObjectInfo {
  ObjectKind kind = ObjectKind::None;
  uint32_t width = 0, height = 0;   // the natural size in pixels
  bool ready = false;               // the picture is there (an image that is not shows its alternative text)
};

// What the renderer asks about an element that may be an object.
class IObjectSource {
 public:
  virtual bool object_info(const dom::Node* element, ObjectInfo* out) = 0;

 protected:
  IObjectSource() = default;
  ~IObjectSource() = default;
};

// What the page view asks for when a run is an object.
class IObjectPainter {
 public:
  // Draws the object of the element with this serial into the rectangle (absolute coordinates) on `r`; nothing outside `clip` may change, and `r`'s clip is
  // `clip` again afterwards.
  virtual void paint_object(graphics::IRenderer2D& r, uint32_t node, ObjectKind kind, int32_t x, int32_t y, uint32_t w, uint32_t h, const Rect2D& clip) = 0;

  // CSS url() pictures: background-image layers and list-style-image markers. `url` is the address as the style wrote it (unquoted, not resolved: the page
  // resolves it against its base address). The defaults draw nothing, so painters written before this still work.
  // The natural size of the picture in pixels; false if it is not (yet) available, in which case the layer is not drawn.
  virtual bool background_image_size(StringView url, int32_t* width, int32_t* height) { return false; }
  // Draws the picture stretched over the rectangle (x, y, w, h) (one tile of the layer); nothing outside `clip` may change (push it with push_clip_rect and pop it).
  virtual void paint_background_image(graphics::IRenderer2D& r, StringView url, int32_t x, int32_t y, uint32_t w, uint32_t h, const Rect2D& clip) {}

 protected:
  IObjectPainter() = default;
  ~IObjectPainter() = default;
};

}  // namespace browser
