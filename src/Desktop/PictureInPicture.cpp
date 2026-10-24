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
#include "Desktop/PictureInPicture.hpp"
#include "Gui/Painter.hpp"

namespace desktop {

void PictureInPictureView::paint(gui::Painter& painter) {
  const graphics::Rect2D area = painter.clip_rect();
  if (!objects_) {
    painter.fill({0, 0, bounds_.width, bounds_.height}, 0xFF000000u);
    return;
  }
  objects_->paint_pip(painter.renderer(), video_, painter.origin_x(), painter.origin_y(), bounds_.width, bounds_.height, area);
}

void PictureInPictureView::on_mouse_down(const gui::MouseArgs& args) {
  if (args.button != gui::MouseButton::Left || !objects_) return;
  objects_->video_toggle(video_);
  invalidate();
}

PictureInPictureWindow::PictureInPictureWindow(allocators::IAllocator& allocator) : Frame(allocator), view_(allocator) {
  set_title("Picture-in-Picture");
  set_minimize_enabled(false);
  set_maximize_enabled(false);
  set_padding(gui::Insets::all(0));
  view_.set_dock(gui::DockStyle::Fill);
  add(&view_);
}

bool PictureInPictureWindow::open(gui::WindowManager& manager, browser::PageObjects* objects, uint32_t video, uint32_t width, uint32_t height) {
  gui::UiGuard lock(&manager);
  view_.show(objects, video);
  const graphics::Rect2D area = manager.dialog_area();
  const uint32_t w = width + kFrameWidth, h = height + kFrameHeight;
  const int32_t x = area.x + static_cast<int32_t>(area.width > w + 16 ? area.width - w - 16 : 0);
  const int32_t y = area.y + static_cast<int32_t>(area.height > h + 16 ? area.height - h - 16 : 0);
  set_title("Picture-in-Picture");
  set_bounds({x, y, w, h});
  if (showing()) {
    activate();
    return true;
  }
  return manager.add_window(this);
}

void PictureInPictureWindow::dismiss() {
  if (gui::WindowManager* m = manager()) m->remove_window(this);
  view_.show(nullptr, 0);
}

void PictureInPictureWindow::refresh() {
  if (showing()) view_.invalidate();
}

}  // namespace desktop
