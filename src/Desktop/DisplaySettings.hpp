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
// Make Primary puts the taskbar on the selected monitor.
//
// The buttons mean what they mean in every property sheet (docs/UserInterfaceGuidelines.md, 6.3 and 11.5): Apply makes the choices in the lists real and
// stays; OK does that and closes; Cancel drops the choices that were not applied and closes; the close button asks whether to apply them.
//
// A new mode may be one the screen cannot show, and then the user cannot see what to do. So after the change the window asks "Keep these display
// settings?", and the question is built so that nothing depends on seeing it or on answering in time:
//  - Revert is the default button and has the focus: Enter and Escape both bring the old settings back. Keeping needs a deliberate choice (the Keep
//    button, or Alt+K).
//  - The old settings also come back by themselves when the time runs out (kConfirmSeconds). That is a safety net, not the route: it loses nothing, and
//    More Time starts it again with a longer time (kMoreSeconds) for whoever needs longer to read.
//  - Settings that were kept can still be taken back: Restore Previous (Ctrl+Z) puts the monitors back as they were before the last change made here -
//    a kept mode, another primary monitor or a monitor moved.
// The monitors are moved with the pointer (drag in the picture), or with Ctrl+arrow in the picture, or with the Move commands of a monitor's
// context menu (Menu key), which also has Make Primary.
//
// Graphics: on a laptop with a display switch (a mux: the built-in screen can be driven by the integrated or the discrete GPU), a group under the
// lists says which GPU drives the built-in screen and offers the other one ("Use <GPU> for the Built-in Screen"). The button is unavailable, with
// the reason under it, when the switch cannot be made now (the other GPU's driver cannot light the screen, a switch is already under way, ...). It
// asks first, with Cancel as the default button, because the screen goes dark for a moment; then drawing stops (the pauser), the panel and its
// monitor move (IBuiltInScreenSwitch), the monitors are made real on the new GPU (the applier) and drawing starts again. The line under the button
// says how it went; a failure is also reported in an error notice (the switch was undone, or, if that failed too, that the screen may stay dark).
// Switching back is the same button, which then names the first GPU.
#pragma once
#include "Desktop/StackPanel.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Controls/DropDown.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Desktop/MonitorMap.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuMuxPanel.hpp"
#include "Hardware/Monitor/SystemMonitors.hpp"

namespace desktop {

class DisplaySettings : public gui::Frame {
 public:
  static constexpr uint32_t kInfoLines = 5, kMaxResolutions = 64, kMaxRates = 16, kMaxScales = 8, kConfirmSeconds = 15, kMoreSeconds = 60;
  // The window's height, and what the Graphics group adds to it (the group takes most of the free space under the lists first).
  static constexpr uint32_t kHeight = 500, kGraphicsHeight = 104, kGraphicsSlack = 44;

  explicit DisplaySettings(allocators::IAllocator& allocator);

  // The monitors to show and change (nullptr: none). They must outlive the window.
  void set_monitors(hardware::SystemMonitors* monitors) { monitors_ = monitors; refresh(); }
  // How a change made to the monitors becomes real (the desktop redraws on the new screen): returns whether it worked. Without one the changes
  // only reach the layout.
  using Applier = bool (*)(void* context);
  void set_applier(Applier applier, void* context) { applier_ = applier; applier_context_ = context; }
  // Where "keep these settings?" and "apply the changes?" are asked (nullptr: new settings are kept without asking, and closing drops what was
  // not applied). It must outlive the window.
  void set_confirm_dialog(gui::MessageDialog* dialog) { confirm_ = dialog; }
  // A laptop's display switch for the built-in screen (nullptr: there is none, and the Graphics group is not shown). It must outlive the window.
  void set_graphics_switch(hardware::IBuiltInScreenSwitch* graphics);
  // How drawing is stopped while the built-in screen changes GPU (`stop` true) and started again (false). Without one nothing is stopped.
  using Pauser = void (*)(void* context, bool stop);
  void set_pauser(Pauser pauser, void* context) { pauser_ = pauser; pauser_context_ = context; }
  // Where a switch that failed is reported (nullptr: only under the button). It must outlive the window.
  void set_notice_dialog(gui::NoticeDialog* notices) { notices_ = notices; }
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
  // More Time: the countdown starts again, with kMoreSeconds.
  void more_time();
  bool asking() const { return asking_; }
  uint32_t seconds_left() const { return seconds_left_; }
  // True while a list shows something other than what the selected monitor does now (Apply and OK would change it).
  bool pending() const;
  // Puts the monitors back as they were before the last change made here that was kept. False if there is nothing to go back to.
  bool restore_previous();
  bool can_restore() const { return previous_.valid(); }
  // What OK and Cancel do.
  void accept();
  void cancel();
  // The Graphics group's button: asks whether to drive the built-in screen with the other GPU (without a confirm dialog, switches at once).
  bool ask_switch_graphics();
  // The switch itself (the answer to that question): false if it failed (and was undone where it could be).
  bool switch_graphics();
  bool graphics_shown() const { return graphics_switch_ != nullptr; }
  // The side the button offers (the one not driving the screen).
  hardware::GpuMuxTarget graphics_target() const { return graphics_target_; }

  const hardware::Monitor* selected_monitor() const;
  MonitorMap& map() { return map_; }
  gui::DropDown& resolution_list() { return resolution_; }
  gui::DropDown& refresh_list() { return refresh_; }
  gui::DropDown& scale_list() { return scale_; }
  gui::Label& info_line(uint32_t index) { return info_[index]; }
  gui::Button& primary_button() { return primary_button_; }
  gui::Button& apply_button() { return apply_button_; }
  gui::Button& ok_button() { return ok_button_; }
  gui::Button& cancel_button() { return cancel_button_; }
  gui::Button& restore_button() { return restore_button_; }
  gui::Button& graphics_button() { return graphics_button_; }
  gui::Label& graphics_line() { return graphics_line_; }
  gui::Label& graphics_note() { return graphics_note_; }
  // The context menu of a monitor in the picture: Make Primary and the four Move commands.
  gui::ContextMenu& map_menu() { return map_menu_; }

  bool on_key(const gui::KeyArgs& args) override;

 protected:
  bool close_requested() override;

 private:
  static void on_map_selected(void* self, gui::Control&);
  static void on_map_moved(void* self, gui::Control&);
  static void on_resolution_chosen(void* self, gui::Control&);
  static void on_primary(void* self, gui::Control&);
  static void on_apply(void* self, gui::Control&);
  static void on_ok(void* self, gui::Control&);
  static void on_cancel(void* self, gui::Control&);
  static void on_restore(void* self, gui::Control&);
  static void on_list_chosen(void* self, gui::Control&);
  static void on_move_left(void* self, gui::Control&);
  static void on_move_right(void* self, gui::Control&);
  static void on_move_up(void* self, gui::Control&);
  static void on_move_down(void* self, gui::Control&);
  static void on_map_menu_opening(void* self, gui::Control&);
  static void on_confirm_result(void* self, gui::Control&);
  static void on_graphics(void* self, gui::Control&);
  void update_graphics();            // the Graphics group, from the switch's state
  void show_keep_question();         // (again, with the seconds that are left)
  void fill_lists();                 // the three lists for the selected monitor (what is in use is chosen)
  void fill_dependent_lists();       // the refresh rates and scales for the resolution chosen in the first list
  void update_details();
  void update_buttons();
  bool make_real();                  // the applier, or true without one

  hardware::SystemMonitors* monitors_ = nullptr;
  Applier applier_ = nullptr;
  void* applier_context_ = nullptr;
  gui::MessageDialog* confirm_ = nullptr;
  gui::NoticeDialog* notices_ = nullptr;
  hardware::IBuiltInScreenSwitch* graphics_switch_ = nullptr;
  Pauser pauser_ = nullptr;
  void* pauser_context_ = nullptr;
  MonitorMap map_;
  StackColumn body_;       // top to bottom, the order Tab takes: the picture, the details, the lists, the buttons
  StackRow buttons_;       // left to right: Make Primary, Restore Previous, the space, OK, Cancel, Apply
  gui::Panel details_, settings_, resolution_row_, refresh_row_, scale_row_, filler_, spacer_, gap_a_, gap_b_, gap_c_;
  gui::Panel graphics_, graphics_row_;   // the Graphics group, and the row its button is in (left aligned)
  gui::Label graphics_heading_, graphics_line_, graphics_note_;
  gui::Button graphics_button_;
  hardware::GpuMuxTarget graphics_target_ = hardware::GpuMuxTarget::Discrete;
  String graphics_result_;               // how the last switch went ("" before one)
  gui::ContextMenu map_menu_;
  gui::MenuItem *primary_item_ = nullptr, *move_items_[4] = {};
  gui::Label info_[kInfoLines];
  gui::Label resolution_label_, refresh_label_, scale_label_;
  gui::DropDown resolution_, refresh_, scale_;
  gui::Button primary_button_, restore_button_, ok_button_, cancel_button_, apply_button_;
  hardware::MonitorMode resolutions_[kMaxResolutions];
  uint32_t resolution_count_ = 0;
  uint32_t rates_[kMaxRates] = {};
  uint32_t rate_count_ = 0;
  uint32_t scales_[kMaxScales] = {};
  uint32_t scale_count_ = 0;
  uint32_t selected_id_ = MonitorMap::kNone;
  bool filling_ = false;
  hardware::SystemMonitors::State before_;     // while the keep question is open: what Revert puts back
  hardware::SystemMonitors::State previous_;   // what Restore Previous puts back
  enum class Question : uint8_t { None, Keep, ApplyOnClose, SwitchGraphics };
  Question question_ = Question::None;
  bool close_when_kept_ = false;               // OK: the window closes once the new settings are kept
  bool asking_ = false;
  uint32_t seconds_left_ = 0;
};

}  // namespace desktop
