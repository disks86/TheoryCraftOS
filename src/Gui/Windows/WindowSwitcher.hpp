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

// The taskbar's window switcher: one button per window (dialogs are left out), in the order the windows were
// created. The active window's button is pressed in; clicking it minimizes that window, clicking any
// other button restores (if needed), raises and activates its window. Ignores clicks while a modal
// window is open.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Windows/WindowManager.hpp"

namespace gui {

class WindowSwitcher : public Control {
 public:
  static constexpr int32_t kMaxButtonWidth = 160, kMinButtonWidth = 24, kGap = 2;

  WindowSwitcher(allocators::IAllocator& allocator, WindowManager& manager) : Control(allocator), manager_(manager) {}

  // The buttons, left to right.
  uint32_t button_count() const;
  Frame* frame_at(uint32_t button) const;
  Rect2D button_rect(uint32_t button) const;

  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;

 private:
  int32_t button_at(int32_t x, int32_t y) const;  // -1 = none

  WindowManager& manager_;
  int32_t pressed_ = -1;
};

}  // namespace gui
