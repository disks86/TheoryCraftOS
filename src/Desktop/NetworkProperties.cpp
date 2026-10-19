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

#include "Desktop/NetworkProperties.hpp"

namespace desktop {

using namespace gui;

namespace {
const char* const kProperties[NetworkProperties::kRowCount] = {
    "Adapter", "Link", "Speed", "Hardware address", "IP configuration", "IPv4 address", "Subnet mask", "Default gateway", "DNS server",
    "DHCP server", "Lease", "Frames sent", "Frames received", "Frames dropped", "Send failures"};

void append_ip(String* out, const uint8_t* octets) {
  for (int i = 0; i < 4; ++i) {
    if (i) out->push_back('.');
    out->append_uint(octets[i]);
  }
}

bool all_zero(const uint8_t* octets) { return !(octets[0] | octets[1] | octets[2] | octets[3]); }

void append_hex2(String* out, uint8_t value) {
  static const char kDigits[] = "0123456789ABCDEF";
  out->push_back(kDigits[value >> 4]);
  out->push_back(kDigits[value & 15]);
}
}  // namespace

NetworkProperties::NetworkProperties(allocators::IAllocator& allocator)
    : Frame(allocator),
      grid_(allocator),
      buttons_(allocator),
      copy_button_(allocator),
      close_button_(allocator),
      status_(allocator),
      values_{String(allocator), String(allocator), String(allocator), String(allocator), String(allocator), String(allocator), String(allocator), String(allocator),
              String(allocator), String(allocator), String(allocator), String(allocator), String(allocator), String(allocator), String(allocator)} {
  set_title("Network Adapter Properties");
  set_bounds({130, 80, 440, 400});
  set_padding(Insets::all(8));
  set_minimize_enabled(true);

  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, 30);
  close_button_.set_text("Close");
  close_button_.set_dock(DockStyle::Right);
  close_button_.set_size(90, 0);
  close_button_.on_click(Handler::immediate(&NetworkProperties::on_close, this));
  copy_button_.set_text("Copy Row");
  copy_button_.set_dock(DockStyle::Left);
  copy_button_.set_size(100, 0);
  copy_button_.on_click({&NetworkProperties::on_copy, this});
  buttons_.add(&close_button_);
  buttons_.add(&copy_button_);

  status_.set_dock(DockStyle::Bottom);
  status_.set_size(0, 22);

  grid_.add_column("Property", 150);
  grid_.add_column("Value", 240);
  grid_.set_dock(DockStyle::Fill);
  for (uint32_t i = 0; i < kRowCount; ++i) {
    const StringView cells[2] = {kProperties[i], StringView()};
    grid_.add_row(cells, 2);
  }
  add(&buttons_);
  add(&status_);
  add(&grid_);   // (Fill goes last)
}

StringView NetworkProperties::cell(uint32_t row, uint32_t column) const {
  if (row >= kRowCount) return StringView();
  return column == 0 ? StringView(kProperties[row]) : values_[row].view();
}

void NetworkProperties::set_value(uint32_t row, StringView text) {
  values_[row].assign(text);
  grid_.set_cell(row, 1, text);
}

void NetworkProperties::show_adapter(uint32_t id) {
  id_ = id;
  update();
}

void NetworkProperties::on_copy(void* self, Control&) { static_cast<NetworkProperties*>(self)->grid_.copy_selected_row(); }
void NetworkProperties::on_close(void* self, Control&) { static_cast<NetworkProperties*>(self)->close(); }

void NetworkProperties::update() {
  UiGuard lock(this);
  NetworkAdapterInfo info;
  if (!details_ || !details_->adapter(id_, &info)) {
    for (uint32_t i = 0; i < kRowCount; ++i) set_value(i, StringView());
    status_.set_text(details_ ? "This adapter is not there any more." : "No network information is available.");
    return;
  }
  status_.set_text(StringView());
  String title(allocator());
  title.assign(info.name);
  title.append(" - Properties");
  set_title(title.view());
  String text(allocator());
  set_value(kAdapter, info.name);
  set_value(kLink, info.link_up ? "Connected" : "Disconnected");
  if (!info.link_up) {
    text.assign("-");
  } else if (info.speed_mbps == 0) {
    text.assign("Not reported by the adapter");
  } else {
    if (info.speed_mbps >= 1000 && info.speed_mbps % 1000 == 0) {
      text.assign("");
      text.append_uint(info.speed_mbps / 1000);
      text.append(" Gbps");
    } else {
      text.assign("");
      text.append_uint(info.speed_mbps);
      text.append(" Mbps");
    }
    if (info.duplex == 2) text.append(", full duplex");
    else if (info.duplex == 1) text.append(", half duplex");
  }
  set_value(kSpeed, text.view());
  text.clear();
  for (int i = 0; i < 6; ++i) {
    if (i) text.push_back(':');
    append_hex2(&text, info.mac[i]);
  }
  set_value(kMac, text.view());
  if (!info.has_stack) {
    set_value(kConfiguration, "No network stack on this adapter");
  } else {
    text.assign(info.dhcp ? "Automatic (DHCP" : "Manual");
    if (info.dhcp) {
      if (info.dhcp_state[0]) {
        text.append(", ");
        text.append(info.dhcp_state);
      }
      text.push_back(')');
    }
    set_value(kConfiguration, text.view());
  }
  auto address_row = [&](uint32_t row, const uint8_t* octets) {
    if (!info.has_stack || (!info.configured && all_zero(octets))) {
      set_value(row, "Not assigned");
      return;
    }
    text.clear();
    append_ip(&text, octets);
    set_value(row, text.view());
  };
  address_row(kAddress, info.address);
  address_row(kMask, info.netmask);
  address_row(kGateway, info.gateway);
  address_row(kDns, info.dns);
  if (info.dhcp && !all_zero(info.dhcp_server)) {
    text.clear();
    append_ip(&text, info.dhcp_server);
    set_value(kDhcpServer, text.view());
  } else {
    set_value(kDhcpServer, "-");
  }
  if (info.dhcp && info.lease_seconds) {
    text.clear();
    const uint32_t s = info.lease_seconds;
    if (s >= 3600) {
      text.append_uint(s / 3600);
      text.append(" h ");
      text.append_uint(s / 60 % 60);
      text.append(" min");
    } else {
      text.append_uint(s / 60);
      text.append(" min ");
      text.append_uint(s % 60);
      text.append(" s");
    }
    set_value(kLease, text.view());
  } else {
    set_value(kLease, "-");
  }
  const uint64_t counters[4] = {info.frames_sent, info.frames_received, info.frames_dropped, info.send_failures};
  const Row rows[4] = {kSent, kReceived, kDropped, kFailures};
  for (int i = 0; i < 4; ++i) {
    text.clear();
    text.append_uint(counters[i]);
    set_value(rows[i], text.view());
  }
}

}  // namespace desktop
