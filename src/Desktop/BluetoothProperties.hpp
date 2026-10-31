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

// The Bluetooth window of an adapter: the devices around (found by searching), and Connect, Disconnect and Forget for the one picked. Connecting pairs the device first when it is not
// paired; if the pairing needs the user - a passkey to type on the device's keyboard, a passkey shown by the device, two numbers to compare - the window asks in its bottom row. It
// refreshes itself when update() is called (the desktop does so several times a second while the window is open).
#pragma once
#include "Desktop/BluetoothDetails.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/CheckBox.hpp"
#include "Gui/Controls/DataGrid.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Utility/String.hpp"

namespace desktop {

class BluetoothProperties : public gui::Frame {
 public:
  explicit BluetoothProperties(allocators::IAllocator& allocator);

  // The source of the devices (nullptr: none); it must outlive the window.
  void set_details(IBluetoothDetails* details) { details_ = details; update(); }
  // Shows the adapter with this number, and starts looking for devices.
  void show_adapter(uint32_t id);
  uint32_t adapter() const { return id_; }
  // Reads the adapter again.
  void update();

  gui::Label& adapter_label() { return adapter_label_; }
  gui::Label& status_label() { return status_; }
  // "Reconnect to paired devices automatically": follows the adapter's setting; ticking it changes the setting.
  gui::CheckBox& auto_reconnect_box() { return auto_reconnect_; }
  gui::DataGrid& grid() { return grid_; }
  // The text of a cell of the device list (columns: device, type, signal, status).
  StringView cell(uint32_t row, uint32_t column) const;
  gui::Button& search_button() { return search_button_; }
  gui::Button& connect_button() { return connect_button_; }
  gui::Button& disconnect_button() { return disconnect_button_; }
  gui::Button& forget_button() { return forget_button_; }
  // The pairing question: a line of text, the box for a typed passkey, and the two answers. Hidden while nothing is asked.
  gui::Panel& prompt_row() { return prompt_row_; }
  gui::Label& prompt_label() { return prompt_label_; }
  gui::TextBox& passkey_box() { return passkey_; }
  gui::Button& accept_button() { return accept_button_; }
  gui::Button& reject_button() { return reject_button_; }

 private:
  static void on_search(void* self, gui::Control&);
  static void on_connect(void* self, gui::Control&);
  static void on_disconnect(void* self, gui::Control&);
  static void on_forget(void* self, gui::Control&);
  static void on_close(void* self, gui::Control&);
  static void on_selection(void* self, gui::Control&);
  static void on_accept(void* self, gui::Control&);
  static void on_reject(void* self, gui::Control&);
  static void on_auto_reconnect(void* self, gui::Control&);
  void fill_devices(const BluetoothDeviceRow* list, uint32_t count);
  void update_prompt();
  void update_buttons();
  const BluetoothDeviceRow* selected() const;

  IBluetoothDetails* details_ = nullptr;
  uint32_t id_ = 0;
  bool known_ = false, discovering_ = false;
  BluetoothPrompt prompt_;
  gui::Label adapter_label_, status_, prompt_label_;
  gui::DataGrid grid_;
  gui::Panel prompt_row_, buttons_, gap_a_, gap_b_, gap_c_;
  gui::TextBox passkey_;
  gui::CheckBox auto_reconnect_;
  bool syncing_ = false;      // the box is being set from the adapter, not by the user
  gui::Button search_button_, connect_button_, disconnect_button_, forget_button_, close_button_, accept_button_, reject_button_;
  BluetoothDeviceRow shown_[IBluetoothDetails::kMaxDevices], fresh_[IBluetoothDetails::kMaxDevices];   // (the list on screen; the one just read)
  uint32_t shown_count_ = 0;
  mutable String cell_text_;
};

}  // namespace desktop
