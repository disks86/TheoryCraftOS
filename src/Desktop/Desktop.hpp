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

struct DateTime {
  uint16_t year = 0;
  uint8_t month = 0, day = 0;  // 1-based
  uint8_t hour = 0, minute = 0;  // 24-hour clock
};

// Where the taskbar clock gets the local time from; false if it cannot be read right now.
using ClockSource = bool (*)(void* context, DateTime* out);

// "MM/dd/yyyy hh:mm AM" - 12-hour clock, always 19 characters. `out` needs room for 20. Returns the length.
uint32_t format_date_time(const DateTime& time, char* out);

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
  StringView clock_text() const { return clock_.text(); }
  bool shutdown_requested() const { return shutdown_requested_; }
  void request_shutdown() { shutdown_requested_ = true; }
  // The clock shows "--/--/---- --:-- --" until a source is set (and while it fails).
  void set_clock_source(ClockSource source, void* context) { clock_source_ = source; clock_context_ = context; shown_ = ~0ull; }

  // One turn of the event loop: delivers pending input, refreshes the clock (reads the source at most
  // once per second of `uptime_us`) and redraws if anything changed. Returns whether it drew.
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
  ClockSource clock_source_ = nullptr;
  void* clock_context_ = nullptr;
  uint64_t polled_second_ = ~0ull;  // the uptime second the source was last read in
  uint64_t shown_ = ~0ull;          // what the label shows, packed (see step())
  bool shutdown_requested_ = false;
};

}  // namespace desktop
