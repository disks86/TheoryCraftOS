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
#include "Gui/Control.hpp"
#include "Utility/AllocatedView.hpp"

namespace gui {

// Shows a picture, zoomed and panned. The picture is a graphics::Surface (0xAARRGGBB pixels, see
// Graphics/GraphicsTypes.hpp) that the view borrows: it must stay valid while it is shown, and
// image_changed() asks for a redraw after its pixels were altered. Transparent pixels show a chequerboard.
//  - Zoom is a percentage, 1 to 3200. Zooming in repeats pixels; zooming out averages them.
//  - The picture starts "fitted": shrunk to fit the view if it is larger (never enlarged), and kept fitted
//    when the view is resized, until the zoom is set by hand.
//  - Dragging with the left button pans; the mouse wheel zooms around the pointer; the keys +/- zoom, 0 shows
//    actual size (100%), F fits, the arrow keys pan.
// The view draws into a cache the size of its viewport, so painting a still picture costs one copy.
class ImageView : public Control {
 public:
  static constexpr int32_t kBorder = 2, kMinZoom = 1, kMaxZoom = 3200;

  explicit ImageView(allocators::IAllocator& allocator) : Control(allocator) {}
  ~ImageView() { release_view(allocator(), cache_); }

  // Shows `image` (nullptr clears). The view keeps a pointer, not a copy.
  void set_image(const graphics::Surface* image);
  const graphics::Surface* image() const { return image_; }
  // The pixels changed in place.
  void image_changed();

  int32_t zoom_percent() const { return zoom_; }
  bool fitted() const { return fit_; }
  // Zooms to `percent` (clamped), keeping the point at the centre of the view where it is; leaves fit mode.
  void set_zoom(int32_t percent);
  // Zoom to the next / previous step of 10, 25, 50, 75, 100, 150, 200, 300, 400, 600, 800, 1200, 1600, 3200 %.
  void zoom_in();
  void zoom_out();
  void actual_size() { set_zoom(100); }
  void fit();
  void on_zoom_changed(Handler handler) { UiGuard lock(this); zoom_changed_ = handler; }

  // The part of the picture shown: the picture-space (zoomed) coordinate at the top left of the viewport.
  Point scroll_position() const { return {origin_x_, origin_y_}; }
  void scroll_to(int32_t x, int32_t y);
  // The viewport: where the picture is drawn, in this control's coordinates.
  Rect2D viewport() const;

  bool focusable() const override { return true; }
  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs&) override { panning_ = false; }
  void on_mouse_wheel(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {200, 150}; }

 private:
  int32_t zoomed_width() const;
  int32_t zoomed_height() const;
  void clamp_origin();
  void apply_zoom(int32_t percent, int32_t focus_x, int32_t focus_y);  // keeps the picture point under the viewport position fixed
  int32_t fit_zoom() const;
  void refit();
  void rebuild_cache();
  uint32_t sample(int32_t zx, int32_t zy) const;  // the picture at a zoomed-picture position (alpha as is)

  const graphics::Surface* image_ = nullptr;
  uint32_t generation_ = 1;
  int32_t zoom_ = 100;
  bool fit_ = true;
  int32_t origin_x_ = 0, origin_y_ = 0;
  Handler zoom_changed_;

  // The cache: the viewport as it was last drawn, and what it was drawn from.
  ArrayView<uint32_t> cache_;
  uint32_t cache_width_ = 0, cache_height_ = 0;
  int32_t cache_zoom_ = 0, cache_x_ = 0, cache_y_ = 0;
  uint32_t cache_generation_ = 0;
  const graphics::Surface* cache_image_ = nullptr;

  bool panning_ = false;
  int32_t pan_x_ = 0, pan_y_ = 0, pan_origin_x_ = 0, pan_origin_y_ = 0;
};

}  // namespace gui
