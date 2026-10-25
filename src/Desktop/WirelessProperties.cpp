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

#include "Desktop/WirelessProperties.hpp"

namespace desktop {

using namespace gui;

const char* wireless_security_name(WirelessSecurity security) {
  switch (security) {
    case WirelessSecurity::Open: return "Open";
    case WirelessSecurity::Wep: return "WEP";
    case WirelessSecurity::Wpa: return "WPA";
    case WirelessSecurity::Wpa2: return "WPA2";
    case WirelessSecurity::Wpa3: return "WPA3";
    case WirelessSecurity::Enterprise: return "Enterprise";
  }
  return "";
}

const char* wireless_state_name(WirelessState state) {
  switch (state) {
    case WirelessState::Disconnected: return "Not connected";
    case WirelessState::Scanning: return "Scanning";
    case WirelessState::Connecting: return "Connecting";
    case WirelessState::Connected: return "Connected";
  }
  return "";
}

const char* wireless_result_text(WirelessResult result) {
  switch (result) {
    case WirelessResult::Ok: return "";
    case WirelessResult::NoSuchNetwork: return "The network is not in range.";
    case WirelessResult::BadPassphrase: return "The password is not right.";
    case WirelessResult::Unsupported: return "This kind of network is not supported.";
    case WirelessResult::InvalidArgument: return "Not a valid network name.";
    case WirelessResult::Busy: return "The adapter is busy; try again in a moment.";
    case WirelessResult::Failed: return "The connection failed.";
  }
  return "";
}

namespace {
constexpr uint32_t kColumns = 4;

bool same_network(const WirelessNetworkInfo& a, const WirelessNetworkInfo& b) {
  for (int i = 0; i < 6; ++i)
    if (a.bssid[i] != b.bssid[i]) return false;
  return a.security == b.security && a.channel == b.channel && a.signal_percent == b.signal_percent && StringView(a.ssid, __builtin_strlen(a.ssid)).equals(StringView(b.ssid, __builtin_strlen(b.ssid)));
}

bool has_ssid(const WirelessNetworkInfo& n, StringView ssid) { return StringView(n.ssid, __builtin_strlen(n.ssid)).equals(ssid); }

void signal_text(String* out, uint32_t percent) {
  out->append_uint(percent);
  out->push_back('%');
}
}  // namespace

WirelessProperties::WirelessProperties(allocators::IAllocator& allocator)
    : Frame(allocator),
      connection_(allocator),
      status_(allocator),
      password_label_(allocator),
      grid_(allocator),
      password_row_(allocator),
      buttons_(allocator),
      gap_a_(allocator),
      gap_b_(allocator),
      password_(allocator),
      scan_button_(allocator),
      connect_button_(allocator),
      disconnect_button_(allocator),
      close_button_(allocator),
      cell_text_(allocator) {
  set_title("Wi-Fi Properties");
  set_bounds({120, 70, 470, 420});
  set_padding(Insets::all(8));
  set_minimize_enabled(true);

  connection_.set_dock(DockStyle::Top);
  connection_.set_size(0, 24);

  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, 30);
  close_button_.set_text("Close");
  close_button_.set_dock(DockStyle::Right);
  close_button_.set_size(80, 0);
  close_button_.on_click(Handler::immediate(&WirelessProperties::on_close, this));
  scan_button_.set_text("Scan");
  scan_button_.set_dock(DockStyle::Left);
  scan_button_.set_size(80, 0);
  scan_button_.on_click({&WirelessProperties::on_scan, this});
  gap_a_.set_style(PanelStyle::Transparent);
  gap_a_.set_dock(DockStyle::Left);
  gap_a_.set_size(6, 0);
  connect_button_.set_text("Connect");
  connect_button_.set_dock(DockStyle::Left);
  connect_button_.set_size(90, 0);
  connect_button_.on_click({&WirelessProperties::on_connect, this});
  gap_b_.set_style(PanelStyle::Transparent);
  gap_b_.set_dock(DockStyle::Left);
  gap_b_.set_size(6, 0);
  disconnect_button_.set_text("Disconnect");
  disconnect_button_.set_dock(DockStyle::Left);
  disconnect_button_.set_size(100, 0);
  disconnect_button_.on_click({&WirelessProperties::on_disconnect, this});
  buttons_.add(&close_button_);
  buttons_.add(&scan_button_);
  buttons_.add(&gap_a_);
  buttons_.add(&connect_button_);
  buttons_.add(&gap_b_);
  buttons_.add(&disconnect_button_);

  status_.set_dock(DockStyle::Bottom);
  status_.set_size(0, 22);

  password_label_.set_text("Password:");
  password_label_.set_dock(DockStyle::Left);
  password_label_.set_size(80, 0);
  password_.set_dock(DockStyle::Fill);
  password_.set_password(true);
  password_.set_max_length(63);
  password_row_.set_style(PanelStyle::Transparent);
  password_row_.set_dock(DockStyle::Bottom);
  password_row_.set_size(0, 28);
  password_row_.add(&password_label_);
  password_row_.add(&password_);

  grid_.add_column("Network", 190);
  grid_.add_column("Signal", 60, Align::Right);
  grid_.add_column("Security", 80);
  grid_.add_column("Channel", 60, Align::Right);
  grid_.set_dock(DockStyle::Fill);
  grid_.on_selection_changed({&WirelessProperties::on_selection, this});

  add(&connection_);
  add(&buttons_);
  add(&status_);
  add(&password_row_);
  add(&grid_);   // (Fill goes last)
  update_buttons();
}

StringView WirelessProperties::cell(uint32_t row, uint32_t column) const {
  if (row >= shown_count_ || column >= kColumns) return StringView();
  const WirelessNetworkInfo& n = shown_[row];
  cell_text_.clear();
  switch (column) {
    case 0: cell_text_.append(n.ssid); break;
    case 1: signal_text(&cell_text_, n.signal_percent); break;
    case 2: cell_text_.append(wireless_security_name(n.security)); break;
    default: cell_text_.append_uint(n.channel); break;
  }
  return cell_text_.view();
}

void WirelessProperties::show_adapter(uint32_t id) {
  UiGuard lock(this);
  id_ = id;
  shown_count_ = 0;
  grid_.clear_rows();
  status_.set_text(StringView());
  password_.set_text(StringView());
  if (details_) details_->scan(id_);
  update();
}

void WirelessProperties::fill_networks(const WirelessNetworkInfo* list, uint32_t count, StringView connected_ssid) {
  bool same = count == shown_count_;
  for (uint32_t i = 0; same && i < count; ++i) same = same_network(list[i], shown_[i]);
  if (same) return;   // (what the user looks at, or has selected, is left alone)
  String selected(allocator());
  if (grid_.selected_row() >= 0 && static_cast<uint32_t>(grid_.selected_row()) < shown_count_) selected.assign(StringView(shown_[grid_.selected_row()].ssid, __builtin_strlen(shown_[grid_.selected_row()].ssid)));
  grid_.clear_rows();
  shown_count_ = 0;
  int32_t reselect = -1;
  String name(allocator());
  for (uint32_t i = 0; i < count && i < IWirelessDetails::kMaxNetworks; ++i) {
    shown_[shown_count_] = list[i];
    String signal(allocator());
    signal_text(&signal, list[i].signal_percent);
    String channel(allocator());
    channel.append_uint(list[i].channel);
    name.assign(list[i].ssid);
    if (!connected_ssid.empty() && has_ssid(list[i], connected_ssid)) name.append(" (connected)");
    const StringView cells[kColumns] = {name.view(), signal.view(), wireless_security_name(list[i].security), channel.view()};
    const int32_t row = grid_.add_row(cells, kColumns);
    if (row < 0) break;
    if (!selected.empty() && has_ssid(list[i], selected.view())) reselect = row;
    ++shown_count_;
  }
  if (reselect >= 0) grid_.select_row(reselect);
}

void WirelessProperties::update_buttons() {
  const bool joined = known_ && (state_ == WirelessState::Connected || state_ == WirelessState::Connecting);
  const bool busy = state_ == WirelessState::Connecting || state_ == WirelessState::Scanning;
  scan_button_.set_enabled(known_ && !busy);
  connect_button_.set_enabled(known_ && !busy && grid_.selected_row() >= 0);
  disconnect_button_.set_enabled(joined);
  password_.set_enabled(known_);
}

void WirelessProperties::update() {
  UiGuard lock(this);
  WirelessAdapterInfo info;
  if (!details_ || !details_->adapter(id_, &info)) {
    known_ = false;
    state_ = WirelessState::Disconnected;
    connection_.set_text(details_ ? "This Wi-Fi adapter is not there any more." : "No Wi-Fi information is available.");
    fill_networks(nullptr, 0, StringView());
    update_buttons();
    return;
  }
  known_ = true;
  state_ = info.state;
  String text(allocator());
  text.assign(info.name);
  text.append(" - Wi-Fi Properties");
  set_title(text.view());
  text.assign(wireless_state_name(info.state));
  if (info.state == WirelessState::Connected || info.state == WirelessState::Connecting) {
    text.append(" to ");
    text.append(info.network.ssid);
    text.append(" - ");
    text.append(wireless_security_name(info.network.security));
    text.append(", channel ");
    text.append_uint(info.network.channel);
    text.append(", signal ");
    signal_text(&text, info.network.signal_percent);
  }
  connection_.set_text(text.view());
  if (info.state == WirelessState::Disconnected && info.last_result != WirelessResult::Ok) status_.set_text(wireless_result_text(info.last_result));
  const uint32_t count = details_->networks(id_, fresh_, IWirelessDetails::kMaxNetworks);
  fill_networks(fresh_, count < IWirelessDetails::kMaxNetworks ? count : IWirelessDetails::kMaxNetworks,
                info.state == WirelessState::Connected ? StringView(info.network.ssid, __builtin_strlen(info.network.ssid)) : StringView());
  update_buttons();
}

void WirelessProperties::on_selection(void* self, Control&) { static_cast<WirelessProperties*>(self)->update_buttons(); }
void WirelessProperties::on_close(void* self, Control&) { static_cast<WirelessProperties*>(self)->close(); }

void WirelessProperties::on_scan(void* self, Control&) {
  auto* w = static_cast<WirelessProperties*>(self);
  UiGuard lock(w);
  if (!w->details_) return;
  w->status_.set_text(w->details_->scan(w->id_) ? "Looking for networks..." : "The adapter is busy; try again in a moment.");
  w->update();
}

void WirelessProperties::on_connect(void* self, Control&) {
  auto* w = static_cast<WirelessProperties*>(self);
  UiGuard lock(w);
  const int32_t row = w->grid_.selected_row();
  if (!w->details_ || row < 0 || static_cast<uint32_t>(row) >= w->shown_count_) {
    w->status_.set_text("Pick a network first.");
    return;
  }
  const WirelessNetworkInfo network = w->shown_[row];
  const StringView ssid(network.ssid, __builtin_strlen(network.ssid));
  if (network.security != WirelessSecurity::Open && w->password_.text().empty()) {
    w->status_.set_text("Type the password of the network first.");
    return;
  }
  const WirelessResult result = w->details_->connect(w->id_, ssid, w->password_.text());
  w->update();
  if (result != WirelessResult::Ok) {
    w->status_.set_text(wireless_result_text(result));
    return;
  }
  w->password_.set_text(StringView());
  String text(w->allocator());
  text.assign(w->state_ == WirelessState::Connected ? "Connected to " : "Connecting to ");
  text.append(ssid);
  text.append(w->state_ == WirelessState::Connected ? "." : "...");
  w->status_.set_text(text.view());
}

void WirelessProperties::on_disconnect(void* self, Control&) {
  auto* w = static_cast<WirelessProperties*>(self);
  UiGuard lock(w);
  if (!w->details_) return;
  w->details_->disconnect(w->id_);
  w->status_.set_text("Disconnected.");
  w->update();
}

}  // namespace desktop
