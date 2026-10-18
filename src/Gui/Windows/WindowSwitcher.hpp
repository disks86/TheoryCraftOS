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
// window is open. The mouse wheel over the buttons goes through the windows: down to the next one, up to the previous one, each activated (and restored if
// minimized); it stops at the first and last. A right click on a button opens that window's menu: Restore, Minimize, Maximize, Close.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Windows/WindowManager.hpp"

namespace gui {

class WindowSwitcher : public Control {
 public:
  static constexpr int32_t kMaxButtonWidth = 160, kMinButtonWidth = 24, kGap = 2;

  WindowSwitcher(allocators::IAllocator& allocator, WindowManager& manager);
  // The window menu (for tests). It acts on the window whose button was clicked.
  ContextMenu& window_menu() { return menu_; }
  Frame* menu_frame() const { return menu_frame_; }
  bool accepts_context_menu(int32_t x, int32_t y) const override { return !manager_.has_modal() && (button_at(x, y) >= 0 || x < 0); }
  void prepare_context_menu(int32_t x, int32_t y) override;

  // The buttons, left to right.
  uint32_t button_count() const;
  Frame* frame_at(uint32_t button) const;
  Rect2D button_rect(uint32_t button) const;

  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  void on_mouse_wheel(const MouseArgs& args) override;

 private:
  int32_t button_at(int32_t x, int32_t y) const;  // -1 = none
  static void opening(void* self, Control&);
  static void run_restore(void* self, Control&);
  static void run_minimize(void* self, Control&);
  static void run_maximize(void* self, Control&);
  static void run_close(void* self, Control&);

  WindowManager& manager_;
  ContextMenu menu_;
  MenuItem *restore_item_ = nullptr, *minimize_item_ = nullptr, *maximize_item_ = nullptr, *close_item_ = nullptr;
  Frame* menu_frame_ = nullptr;   // the window the menu is open for
  int32_t pressed_ = -1;
};

}  // namespace gui
