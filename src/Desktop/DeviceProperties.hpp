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

// The generic Properties window of a device of the Hardware tab (a sound card, a disk, a camera, ...): a table of what the device and its class
// interface tell - see IDeviceDetails. It is one window for every kind of device. It refreshes itself when update() is called (the desktop does so
// once a second while the window is open), so a changing figure shows at once; if the device is gone (or another one took its number) it says so.
// Copy Row puts the selected line on the clipboard.
#pragma once
#include "Desktop/DeviceDetails.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/DataGrid.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Utility/String.hpp"

namespace desktop {

class DeviceProperties : public gui::Frame {
 public:
  explicit DeviceProperties(allocators::IAllocator& allocator);

  // The source of the numbers (nullptr: none); it must outlive the window.
  void set_details(IDeviceDetails* details) { details_ = details; update(); }
  // Shows the device of this kind with this number (HardwareEntry::group and ::id).
  void show_device(HardwareGroup group, uint32_t id);
  HardwareGroup group() const { return group_; }
  uint32_t device() const { return id_; }
  // Reads the device again.
  void update();

  gui::DataGrid& grid() { return grid_; }
  // The text of a cell of the table (column 0: the property, column 1: its value); empty beyond the table.
  StringView cell(uint32_t row, uint32_t column) const;
  // The value of the line with this property name (empty if there is none).
  StringView value(StringView property) const;
  gui::Button& copy_button() { return copy_button_; }
  StringView status() const { return status_.text(); }

 private:
  static void on_copy(void* self, gui::Control&);
  static void on_close(void* self, gui::Control&);
  void fill(const DevicePropertyList& list);

  IDeviceDetails* details_ = nullptr;
  HardwareGroup group_ = HardwareGroup::System;
  uint32_t id_ = 0;
  String opened_name_;   // what the device was called when the window showed it first (another device with the number is not it)
  bool have_name_ = false;
  gui::DataGrid grid_;
  gui::Panel buttons_;
  gui::Button copy_button_, close_button_;
  gui::Label status_;
  List<DeviceProperty> lines_;
};

}  // namespace desktop
