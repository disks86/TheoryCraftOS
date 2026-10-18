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

// Display Settings: the list of the monitors the machine has - name from the EDID, connector, size on the desktop, place in the row - and what the
// selected one says about itself (maker, physical size and density, native mode and refresh rate, number of modes). Buttons make a monitor the
// primary one (the taskbar, new windows and dialogs go there) and move it left or right in the row; the desktop follows at once. A monitor that was
// plugged in after start-up is listed, with a note that it needs a restart to be used.
#pragma once
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/ListView.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Hardware/Monitor/SystemMonitors.hpp"

namespace desktop {

class DisplaySettings : public gui::Frame {
 public:
  static constexpr uint32_t kInfoLines = 5;

  explicit DisplaySettings(allocators::IAllocator& allocator);

  // The monitors to show and change (nullptr: none). They must outlive the window.
  void set_monitors(hardware::SystemMonitors* monitors) { monitors_ = monitors; refresh(); }
  // Fired after the arrangement was changed with the buttons (the desktop re-reads the layout).
  void on_changed(gui::Handler handler) { changed_ = handler; }
  // Fills the list again from the monitors (keeps the selection where it can).
  void refresh();

  // The command buttons (also for tests).
  bool make_primary();
  bool move_selected(int32_t direction);   // -1 left, +1 right
  // The monitor selected in the list, or nullptr.
  const hardware::Monitor* selected_monitor() const;

  gui::ListView& list() { return list_; }
  gui::Label& info_line(uint32_t index) { return info_[index]; }
  gui::Button& primary_button() { return primary_button_; }
  gui::Button& left_button() { return left_button_; }
  gui::Button& right_button() { return right_button_; }

 private:
  static void on_selection(void* self, gui::Control&);
  static void on_primary(void* self, gui::Control&);
  static void on_left(void* self, gui::Control&);
  static void on_right(void* self, gui::Control&);
  static void on_close(void* self, gui::Control&);
  void update_details();
  void update_buttons();

  hardware::SystemMonitors* monitors_ = nullptr;
  gui::ListView list_;
  gui::Panel details_, buttons_;
  gui::Label info_[kInfoLines];
  gui::Button primary_button_, left_button_, right_button_, close_button_;
  gui::Handler changed_;
  uint32_t selected_id_ = ~0u;
};

}  // namespace desktop
