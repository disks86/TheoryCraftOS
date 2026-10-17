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

// A drawing surface: a bitmap of opaque 0xAARRGGBB pixels that is painted with the mouse, shown at an integer zoom
// with scroll bars when it is larger than the control.
//  - Tools (set_tool): Pencil and Eraser follow the pointer (brush_size wide), Line, Rectangle and Ellipse are dragged
//    out from corner to corner and show a preview until the button is released, Fill floods the area under the
//    pointer, Picker takes the color under it. The left button paints with the primary color, the right button with
//    the secondary (the eraser always paints the secondary color, at least 3 pixels wide). A filled shape has its
//    outline in the button's color and its inside in the other.
//  - Undo and redo (Ctrl+Z, Ctrl+Y, or the methods) go back over strokes within a memory budget; Escape during a
//    stroke cancels it.
//  - Zoom steps are 1, 2, 3, 4, 6, 8, 12 and 16; from 6 up an optional pixel grid can be shown.
//  - Coordinates are pixels of the bitmap (0,0 = top left).
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/ScrollBar.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/AllocatedView.hpp"

namespace gui {

enum class PaintTool : uint8_t { Pencil, Eraser, Line, Rectangle, Ellipse, Fill, Picker };

class DrawingCanvas : public Control {
 public:
  static constexpr int32_t kBorder = 2;
  static constexpr uint32_t kMaxSide = 4096;
  static constexpr uint64_t kMaxHistoryBytes = 24u << 20;
  static constexpr uint32_t kMaxHistoryEntries = 64;
  static constexpr uint32_t kBackground = 0xFF808080;  // around the bitmap

  explicit DrawingCanvas(allocators::IAllocator& allocator);
  ~DrawingCanvas();

  // --- The bitmap
  // A new bitmap filled with `fill`; forgets the history and counts as unmodified. False (nothing changes) if the size is
  // zero or over kMaxSide, or memory ran out.
  bool new_image(uint32_t width, uint32_t height, uint32_t fill = 0xFFFFFFFF);
  // The same from existing pixels (width * height of them, straight alpha): transparent parts are laid over white.
  bool set_pixels(const uint32_t* argb, uint32_t width, uint32_t height);
  uint32_t image_width() const { return width_; }
  uint32_t image_height() const { return height_; }
  ArrayView<const uint32_t> pixels() const { return ArrayView<const uint32_t>(pixels_.data(), pixels_.size()); }
  uint32_t pixel(uint32_t x, uint32_t y) const { return x < width_ && y < height_ ? pixels_[uint64_t{y} * width_ + x] : 0; }
  // The bitmap as a Surface (for saving); valid until the next new_image / set_pixels.
  graphics::Surface surface() const;
  bool modified() const { return modified_; }
  void mark_saved() { UiGuard lock(this); modified_ = false; }
  // Fired when a stroke is finished, on undo / redo and when a bitmap is set.
  void on_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  // --- Tools and colors
  void set_tool(PaintTool tool);
  PaintTool tool() const { return tool_; }
  void set_brush_size(uint32_t size);  // 1 .. 64
  uint32_t brush_size() const { return brush_size_; }
  void set_filled(bool filled) { UiGuard lock(this); filled_ = filled; }
  bool filled() const { return filled_; }
  void set_primary(uint32_t argb) { UiGuard lock(this); primary_ = argb | 0xFF000000u; }
  void set_secondary(uint32_t argb) { UiGuard lock(this); secondary_ = argb | 0xFF000000u; }
  uint32_t primary() const { return primary_; }
  uint32_t secondary() const { return secondary_; }
  // Fired when the Picker took a color; read primary() / secondary() for it.
  void on_color_picked(Handler handler) { UiGuard lock(this); picked_ = handler; }

  // --- History
  bool can_undo() const { return undo_.count() > 0; }
  bool can_redo() const { return redo_.count() > 0; }
  void undo();
  void redo();
  bool stroke_active() const { return drawing_; }

  // --- View
  uint32_t zoom() const { return zoom_; }
  void set_zoom(uint32_t zoom);  // the nearest step
  void zoom_in();
  void zoom_out();
  void on_zoom_changed(Handler handler) { UiGuard lock(this); zoom_changed_ = handler; }
  void set_grid(bool grid);
  bool grid() const { return grid_; }
  // The pointer in bitmap coordinates; false (and -1) when it is not over the bitmap.
  bool pointer_position(int32_t* x, int32_t* y) const;
  void on_pointer_moved(Handler handler) { UiGuard lock(this); pointer_moved_ = handler; }
  Rect2D viewport() const;  // where the bitmap shows, in this control's coordinates
  Point scroll_position() const { return {scroll_x_, scroll_y_}; }
  ScrollBar& vertical_scroll_bar() { return vscroll_; }
  ScrollBar& horizontal_scroll_bar() { return hscroll_; }
  // Where a bitmap pixel is on screen (this control's coordinates), and the bitmap pixel at a point (false if outside).
  Point image_to_view(int32_t x, int32_t y) const;
  bool view_to_image(int32_t x, int32_t y, int32_t* image_x, int32_t* image_y) const;

  bool focusable() const override { return true; }
  CursorShape cursor_shape(int32_t x, int32_t y) const override;
  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  void on_mouse_leave() override;
  void on_mouse_wheel(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {400, 300}; }

 private:
  bool push_history();
  void drop_history(List<ArrayView<uint32_t>>* list);
  void trim_history();
  void swap_with(List<ArrayView<uint32_t>>* from, List<ArrayView<uint32_t>>* to);
  void finish_change();
  void begin_stroke(const MouseArgs& args);
  void draw_to(int32_t x, int32_t y);
  void cancel_stroke();
  void update_scroll_bars();
  void rebuild_cache();
  void after_pixels_changed();
  bool pointer_to_image(const MouseArgs& args, int32_t* x, int32_t* y) const;
  void on_scroll_changed();
  static void vertical_changed(void* self, Control&);
  static void horizontal_changed(void* self, Control&);

  ArrayView<uint32_t> pixels_, cache_;
  uint32_t width_ = 0, height_ = 0, cache_w_ = 0, cache_h_ = 0;
  List<ArrayView<uint32_t>> undo_, redo_;
  uint64_t history_bytes_ = 0;
  PaintTool tool_ = PaintTool::Pencil;
  uint32_t brush_size_ = 1;
  bool filled_ = false, grid_ = false, modified_ = false;
  uint32_t primary_ = 0xFF000000, secondary_ = 0xFFFFFFFF;
  uint32_t zoom_ = 1;
  int32_t scroll_x_ = 0, scroll_y_ = 0;
  bool show_v_ = false, show_h_ = false, syncing_ = false, cache_dirty_ = true;
  ScrollBar vscroll_, hscroll_;
  // The stroke in progress.
  bool drawing_ = false;
  MouseButton button_ = MouseButton::None;
  int32_t start_x_ = 0, start_y_ = 0, last_x_ = 0, last_y_ = 0;
  int32_t pointer_x_ = -1, pointer_y_ = -1;
  Handler changed_, picked_, zoom_changed_, pointer_moved_;
};

}  // namespace gui
