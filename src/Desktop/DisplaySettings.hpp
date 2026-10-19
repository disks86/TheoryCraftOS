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

// Display Settings. At the top is a picture of the monitors (a MonitorMap): drag a monitor next to another to set where it is in relation to
// the others, and the desktop follows at once. Click one to select it; under the picture are what it says about itself (maker, serial number,
// physical size and density, native mode) and what it shows now, and three lists to change that: the resolution, the refresh rate and the scale
// (the DPI of the desktop: 100 % is 96 dpi, 200 % 192 dpi). The lists offer only what the monitor and the graphics device can both do: the
// resolutions are those of the monitor's EDID that the device can show, the refresh rates those the monitor takes at the chosen resolution (and the
// device can set; a device that cannot set one shows the rate in use, greyed), the scales those that leave at least 640x480 on the monitor.
// Apply makes the chosen settings real and asks "Keep these display settings?": they are taken back after 15 seconds if the answer does not come, so
// a mode the screen cannot show does not leave the user blind. Make Primary puts the taskbar on the selected monitor.
#pragma once
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/DropDown.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Desktop/MonitorMap.hpp"
#include "Hardware/Monitor/SystemMonitors.hpp"

namespace desktop {

class DisplaySettings : public gui::Frame {
 public:
  static constexpr uint32_t kInfoLines = 5, kMaxResolutions = 64, kMaxRates = 16, kMaxScales = 8, kConfirmSeconds = 15;

  explicit DisplaySettings(allocators::IAllocator& allocator);

  // The monitors to show and change (nullptr: none). They must outlive the window.
  void set_monitors(hardware::SystemMonitors* monitors) { monitors_ = monitors; refresh(); }
  // How a change made to the monitors becomes real (the desktop redraws on the new screen): returns whether it worked. Without one the changes
  // only reach the layout.
  using Applier = bool (*)(void* context);
  void set_applier(Applier applier, void* context) { applier_ = applier; applier_context_ = context; }
  // Where "keep these settings?" is asked (nullptr: new settings are kept without asking). It must outlive the window.
  void set_confirm_dialog(gui::MessageDialog* dialog) { confirm_ = dialog; }
  // Fills the picture, the details and the lists from the monitors again (keeps the selection where it can).
  void refresh();
  // Called once a second (the desktop does): counts down the time left to answer the question about new settings.
  void tick();

  // --- The commands (the buttons and the picture call these).
  bool make_primary();
  // Makes the resolution, refresh rate and scale chosen in the lists real for the selected monitor, and asks whether to keep them.
  bool apply_settings();
  // Drops monitor `id` with its top left near (x, y) of the desktop.
  bool move_monitor(uint32_t id, int32_t x, int32_t y);
  // The answer to the question: keep the new settings, or go back to the ones before.
  void keep_changes();
  void revert_changes();
  bool asking() const { return asking_; }
  uint32_t seconds_left() const { return seconds_left_; }

  const hardware::Monitor* selected_monitor() const;
  MonitorMap& map() { return map_; }
  gui::DropDown& resolution_list() { return resolution_; }
  gui::DropDown& refresh_list() { return refresh_; }
  gui::DropDown& scale_list() { return scale_; }
  gui::Label& info_line(uint32_t index) { return info_[index]; }
  gui::Button& primary_button() { return primary_button_; }
  gui::Button& apply_button() { return apply_button_; }

 private:
  static void on_map_selected(void* self, gui::Control&);
  static void on_map_moved(void* self, gui::Control&);
  static void on_resolution_chosen(void* self, gui::Control&);
  static void on_primary(void* self, gui::Control&);
  static void on_apply(void* self, gui::Control&);
  static void on_close(void* self, gui::Control&);
  static void on_confirm_result(void* self, gui::Control&);
  void fill_lists();                 // the three lists for the selected monitor (what is in use is chosen)
  void fill_dependent_lists();       // the refresh rates and scales for the resolution chosen in the first list
  void update_details();
  void update_buttons();
  bool make_real();                  // the applier, or true without one

  hardware::SystemMonitors* monitors_ = nullptr;
  Applier applier_ = nullptr;
  void* applier_context_ = nullptr;
  gui::MessageDialog* confirm_ = nullptr;
  MonitorMap map_;
  gui::Panel details_, settings_, resolution_row_, refresh_row_, scale_row_, buttons_;
  gui::Label info_[kInfoLines];
  gui::Label resolution_label_, refresh_label_, scale_label_;
  gui::DropDown resolution_, refresh_, scale_;
  gui::Button primary_button_, apply_button_, close_button_;
  hardware::MonitorMode resolutions_[kMaxResolutions];
  uint32_t resolution_count_ = 0;
  uint32_t rates_[kMaxRates] = {};
  uint32_t rate_count_ = 0;
  uint32_t scales_[kMaxScales] = {};
  uint32_t scale_count_ = 0;
  uint32_t selected_id_ = MonitorMap::kNone;
  bool filling_ = false;
  hardware::SystemMonitors::State before_;
  bool asking_ = false;
  uint32_t seconds_left_ = 0;
};

}  // namespace desktop
