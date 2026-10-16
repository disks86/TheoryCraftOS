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

// The desktop shell: a full-screen window with the desktop surface and a taskbar along the bottom
// holding the Start button (its menu opens upwards) and a small clock. The Start menu has one entry
// for now, "Shut Down", which only asks for a shutdown (shutdown_requested()): the boot code owns the
// hardware and does the orderly power-off once run() returns.
#pragma once
#include "Desktop/DesktopSurface.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/MenuButton.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/InputPump.hpp"
#include "Gui/Window.hpp"

namespace desktop {

class Desktop {
 public:
  static constexpr int32_t kTaskbarHeight = 32;

  // The devices may be null (a machine without a mouse still shows the desktop).
  Desktop(allocators::IAllocator& allocator, graphics::IRenderer2D& renderer, hardware::IKeyboard* keyboard,
          hardware::IMouse* mouse);
  Desktop(const Desktop&) = delete;
  Desktop& operator=(const Desktop&) = delete;

  gui::Window& window() { return window_; }
  gui::MenuButton& start_button() { return start_; }
  bool shutdown_requested() const { return shutdown_requested_; }
  void request_shutdown() { shutdown_requested_ = true; }

  // One turn of the event loop: delivers pending input, refreshes the clock from `uptime_us`
  // and redraws if anything changed. Returns whether it drew.
  bool step(uint64_t uptime_us);

 private:
  static void on_shut_down(void* self, gui::Control&);

  gui::Theme theme_;
  gui::Window window_;
  gui::InputPump input_;
  DesktopSurface surface_;
  gui::Panel taskbar_;
  gui::MenuButton start_;
  gui::Panel tray_;
  gui::Label clock_;
  uint64_t shown_seconds_ = ~0ull;
  bool shutdown_requested_ = false;
};

}  // namespace desktop
