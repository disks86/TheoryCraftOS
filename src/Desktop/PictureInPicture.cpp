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
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

void PictureInPictureView::paint(gui::Painter& painter) {
  const graphics::Rect2D area = painter.clip_rect();
  if (!objects_) {
    painter.fill({0, 0, bounds_.width, bounds_.height}, 0xFF000000u);   // (the black of a screen with no picture: content, not interface)
  } else {
    objects_->paint_pip(painter.renderer(), video_, painter.origin_x(), painter.origin_y(), bounds_.width, bounds_.height, area);
  }
  if (focused() && bounds_.width > 4 && bounds_.height > 4) painter.focus_frame({1, 1, bounds_.width - 2, bounds_.height - 2});
}

bool PictureInPictureView::toggle() {
  gui::UiGuard lock(this);
  if (!objects_) return false;
  objects_->video_toggle(video_);
  invalidate();
  return true;
}

bool PictureInPictureView::paused() const {
  if (!objects_) return true;
  browser::PageObjects::VideoState state;
  objects_->video_state(video_, &state);
  return state.paused;
}

void PictureInPictureView::on_mouse_down(const gui::MouseArgs& args) {
  if (args.button == gui::MouseButton::Left) toggle();
}

// Space or Enter does what a click on the picture does.
bool PictureInPictureView::on_key(const gui::KeyArgs& args) {
  namespace key = hardware::key;
  if (args.modifiers & (hardware::modifier::kControl | hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt)) return false;
  if (args.usage != key::kSpace && args.usage != key::kEnter && args.usage != key::kKeypadEnter) return false;
  return toggle();
}

PictureInPictureWindow::PictureInPictureWindow(allocators::IAllocator& allocator) : Frame(allocator), view_(allocator), menu_(allocator) {
  set_title("Picture-in-Picture");
  // A palette: only a Close button, and no entry on the task bar (the browser's window is the entry).
  set_minimize_enabled(false);
  set_maximize_enabled(false);
  set_in_taskbar(false);
  // The picture's menu: what a click does, named after what it will do, and Close (which the title bar has too).
  toggle_item_ = menu_.add_item("Play", {&PictureInPictureWindow::on_toggle, this});
  menu_.add_separator();
  menu_.add_item("Close", {&PictureInPictureWindow::on_close_menu, this});
  menu_.on_opening(gui::Handler::immediate(&PictureInPictureWindow::on_menu_opening, this));
  view_.set_context_menu(&menu_);
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

void PictureInPictureWindow::on_toggle(void* self, gui::Control&) { static_cast<PictureInPictureWindow*>(self)->view_.toggle(); }
void PictureInPictureWindow::on_close_menu(void* self, gui::Control&) { static_cast<PictureInPictureWindow*>(self)->close(); }
void PictureInPictureWindow::on_menu_opening(void* self, gui::Control&) {
  auto* w = static_cast<PictureInPictureWindow*>(self);
  if (w->toggle_item_) w->toggle_item_->text.assign(w->view_.paused() ? "Play" : "Pause");
}

void PictureInPictureWindow::refresh() {
  if (showing()) view_.invalidate();
}

}  // namespace desktop
