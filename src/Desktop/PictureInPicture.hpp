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
// The Picture-in-Picture window of the web browser: a small window that shows one video of a page (drawn by PageObjects::paint_pip()). Clicking the picture plays or
// pauses the video; closing the window, or changing its size, is reported so that the page can tell its scripts.
#include "Browser/Objects.hpp"
#include "Gui/Control.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/WindowManager.hpp"

namespace desktop {

class PictureInPictureView : public gui::Control {
 public:
  explicit PictureInPictureView(allocators::IAllocator& allocator) : Control(allocator) {}
  void show(browser::PageObjects* objects, uint32_t video) {
    gui::UiGuard lock(this);
    objects_ = objects;
    video_ = video;
    invalidate();
  }
  void paint(gui::Painter& painter) override;
  void on_mouse_down(const gui::MouseArgs& args) override;

 private:
  browser::PageObjects* objects_ = nullptr;
  uint32_t video_ = 0;
};

class PictureInPictureWindow : public gui::Frame {
 public:
  static constexpr int32_t kFrameWidth = 2 * 6, kFrameHeight = 6 * 2 + 18;   // what the window adds to the picture (borders and title bar)

  explicit PictureInPictureWindow(allocators::IAllocator& allocator);
  // Shows a video (of the page whose objects these are) in a window whose picture is width x height pixels, in the bottom right corner of the manager's area.
  bool open(gui::WindowManager& manager, browser::PageObjects* objects, uint32_t video, uint32_t width, uint32_t height);
  // Takes the window out (without the closed handler: the page asked).
  void dismiss();
  bool showing() const { return manager() != nullptr; }
  // The size of the picture area now.
  uint32_t picture_width() const { return client_rect().width; }
  uint32_t picture_height() const { return client_rect().height; }
  PictureInPictureView& view() { return view_; }
  // Called by the browser's loop: draws the next picture.
  void refresh();

 private:
  PictureInPictureView view_;
};

}  // namespace desktop
