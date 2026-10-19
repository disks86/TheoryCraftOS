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

// The properties of a network adapter: a table of what the adapter and its network stack know - link state, speed, hardware address, whether the
// configuration is automatic (DHCP) or manual, the IP address, subnet mask, default gateway and DNS server it was assigned, the DHCP server and
// lease, and the frame counters. It refreshes itself when update() is called (the desktop does so once a second while the window is open), so
// a cable plugged in or a lease renewed shows at once. Copy Row puts the selected line on the clipboard.
#pragma once
#include "Desktop/NetworkDetails.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/DataGrid.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Utility/String.hpp"

namespace desktop {

class NetworkProperties : public gui::Frame {
 public:
  // The lines of the table, top to bottom.
  enum Row : uint32_t { kAdapter, kLink, kSpeed, kMac, kConfiguration, kAddress, kMask, kGateway, kDns, kDhcpServer, kLease, kSent, kReceived, kDropped, kFailures, kRowCount };

  explicit NetworkProperties(allocators::IAllocator& allocator);

  // The source of the numbers (nullptr: none); it must outlive the window.
  void set_details(INetworkDetails* details) { details_ = details; update(); }
  // Shows the adapter with this number (also what update() looks at).
  void show_adapter(uint32_t id);
  uint32_t adapter() const { return id_; }
  // Reads the adapter again.
  void update();

  gui::DataGrid& grid() { return grid_; }
  // The text of a cell of the table (column 0: the property, column 1: its value).
  StringView cell(uint32_t row, uint32_t column) const;
  gui::Button& copy_button() { return copy_button_; }

 private:
  static void on_copy(void* self, gui::Control&);
  static void on_close(void* self, gui::Control&);
  void set_value(uint32_t row, StringView text);

  INetworkDetails* details_ = nullptr;
  uint32_t id_ = 0;
  gui::DataGrid grid_;
  gui::Panel buttons_;
  gui::Button copy_button_, close_button_;
  gui::Label status_;
  String values_[kRowCount];
};

}  // namespace desktop
