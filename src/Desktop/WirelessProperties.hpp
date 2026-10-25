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

// The Wi-Fi properties of a wireless adapter: the network it is joined to (name, signal, security, channel), and the networks in range with their
// signal, security and channel. Pick a network, type its password if it has one and choose Connect; Disconnect leaves the current one; Scan looks
// again. It refreshes itself when update() is called (the desktop does so once a second while the window is open).
#pragma once
#include "Desktop/WirelessDetails.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/DataGrid.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Utility/String.hpp"

namespace desktop {

class WirelessProperties : public gui::Frame {
 public:
  explicit WirelessProperties(allocators::IAllocator& allocator);

  // The source of the networks (nullptr: none); it must outlive the window.
  void set_details(IWirelessDetails* details) { details_ = details; update(); }
  // Shows the adapter with this number, and looks for networks.
  void show_adapter(uint32_t id);
  uint32_t adapter() const { return id_; }
  // Reads the adapter again.
  void update();

  // The line that says what the adapter is joined to ("Connected to Home - WPA2, channel 6, signal 80%", "Not connected").
  gui::Label& connection_label() { return connection_; }
  gui::Label& status_label() { return status_; }
  gui::DataGrid& grid() { return grid_; }
  // The text of a cell of the list of networks (columns: name, signal, security, channel).
  StringView cell(uint32_t row, uint32_t column) const;
  gui::TextBox& password_box() { return password_; }
  gui::Button& scan_button() { return scan_button_; }
  gui::Button& connect_button() { return connect_button_; }
  gui::Button& disconnect_button() { return disconnect_button_; }

 private:
  static void on_scan(void* self, gui::Control&);
  static void on_connect(void* self, gui::Control&);
  static void on_disconnect(void* self, gui::Control&);
  static void on_close(void* self, gui::Control&);
  static void on_selection(void* self, gui::Control&);
  void fill_networks(const WirelessNetworkInfo* list, uint32_t count, StringView connected_ssid);
  void update_buttons();

  IWirelessDetails* details_ = nullptr;
  uint32_t id_ = 0;
  WirelessState state_ = WirelessState::Disconnected;
  bool known_ = false;
  gui::Label connection_, status_, password_label_;
  gui::DataGrid grid_;
  gui::Panel password_row_, buttons_, gap_a_, gap_b_;
  gui::TextBox password_;
  gui::Button scan_button_, connect_button_, disconnect_button_, close_button_;
  WirelessNetworkInfo shown_[IWirelessDetails::kMaxNetworks], fresh_[IWirelessDetails::kMaxNetworks];   // (the list on screen; the one just read)
  uint32_t shown_count_ = 0;
  mutable String cell_text_;
};

}  // namespace desktop
