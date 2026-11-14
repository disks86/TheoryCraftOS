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

// The Notifications window of the desktop: the notifications that went away by themselves before they were clicked (gui::ToastCenter keeps them), the
// newest first. The interface guidelines ask for it (11.8): what is shown only for a few seconds must also be somewhere the user can find it later.
// It is a secondary frame: no task-bar entry, Escape and Close close it. The Start menu's "Notifications" opens it, and so does the button that
// appears next to the clock when something was missed.
#pragma once
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/DataGrid.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/ToastCenter.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

class NotificationLog : public gui::Frame {
 public:
  explicit NotificationLog(allocators::IAllocator& allocator);

  // Shows what `toasts` has recorded. (Call with the UI lock held, or before the window is shown.)
  void refresh(const gui::ToastCenter& toasts);
  // The record the list was last filled from (ToastCenter::missed_serial()).
  uint32_t shown_serial() const { return serial_; }

  gui::DataGrid& grid() { return grid_; }
  gui::Button& close_button() { return close_button_; }
  StringView heading() const { return heading_.text(); }
  // What a kind of notification is called in the list (the kind is told by a word, not by a colour).
  static StringView kind_name(gui::NoticeKind kind);

  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_close(void* self, gui::Control&);

  gui::Label heading_;
  gui::DataGrid grid_;
  gui::Panel buttons_;
  gui::Button close_button_;
  uint32_t serial_ = 0;
};

}  // namespace desktop
