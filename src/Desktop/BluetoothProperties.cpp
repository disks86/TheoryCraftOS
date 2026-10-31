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

#include "Desktop/BluetoothProperties.hpp"

namespace desktop {

using namespace gui;

const char* bluetooth_state_name(BluetoothDeviceState state) {
  switch (state) {
    case BluetoothDeviceState::Disconnected: return "Not connected";
    case BluetoothDeviceState::Connecting: return "Connecting...";
    case BluetoothDeviceState::Pairing: return "Pairing...";
    case BluetoothDeviceState::Setup: return "Setting up...";
    case BluetoothDeviceState::Connected: return "Connected";
    case BluetoothDeviceState::Failed: return "Failed";
  }
  return "";
}

const char* bluetooth_result_text(BluetoothResult result) {
  switch (result) {
    case BluetoothResult::Ok: return "";
    case BluetoothResult::Busy: return "The adapter is busy with another device; try again in a moment.";
    case BluetoothResult::NoSuchDevice: return "The device is not in range any more.";
    case BluetoothResult::Unsupported: return "This kind of device is not supported yet.";
    case BluetoothResult::NotReady: return "The Bluetooth adapter is not ready.";
  }
  return "";
}

namespace {
constexpr uint32_t kColumns = 4;

bool text_equal(const char* a, const char* b) { return StringView(a, __builtin_strlen(a)).equals(StringView(b, __builtin_strlen(b))); }

bool same_device(const BluetoothDeviceRow& a, const BluetoothDeviceRow& b) {
  for (int i = 0; i < 6; ++i)
    if (a.key[i] != b.key[i]) return false;
  return a.state == b.state && a.paired == b.paired && a.supported == b.supported && a.signal_percent == b.signal_percent && text_equal(a.name, b.name) && text_equal(a.kind, b.kind) &&
         text_equal(a.failure, b.failure) && text_equal(a.profiles, b.profiles);
}

bool same_key(const BluetoothDeviceRow& a, const uint8_t key[6]) {
  for (int i = 0; i < 6; ++i)
    if (a.key[i] != key[i]) return false;
  return true;
}

void six_digits(String* out, uint32_t number) {
  char digits[6];
  for (int i = 5; i >= 0; --i) { digits[i] = static_cast<char>('0' + number % 10); number /= 10; }
  out->append(StringView(digits, 6));
}
}  // namespace

BluetoothProperties::BluetoothProperties(allocators::IAllocator& allocator)
    : Frame(allocator),
      adapter_label_(allocator),
      status_(allocator),
      prompt_label_(allocator),
      grid_(allocator),
      prompt_row_(allocator),
      buttons_(allocator),
      gap_a_(allocator),
      gap_b_(allocator),
      gap_c_(allocator),
      passkey_(allocator),
      auto_reconnect_(allocator),
      search_button_(allocator),
      connect_button_(allocator),
      disconnect_button_(allocator),
      forget_button_(allocator),
      close_button_(allocator),
      accept_button_(allocator),
      reject_button_(allocator),
      cell_text_(allocator) {
  set_title("Bluetooth");
  set_bounds({110, 60, 560, 430});
  set_padding(Insets::all(8));
  set_minimize_enabled(true);

  adapter_label_.set_dock(DockStyle::Top);
  adapter_label_.set_size(0, 24);

  auto_reconnect_.set_text("Reconnect to paired devices automatically");
  auto_reconnect_.set_dock(DockStyle::Top);
  auto_reconnect_.set_size(0, 22);
  auto_reconnect_.set_enabled(false);
  auto_reconnect_.on_changed({&BluetoothProperties::on_auto_reconnect, this});

  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, 30);
  close_button_.set_text("Close");
  close_button_.set_dock(DockStyle::Right);
  close_button_.set_size(70, 0);
  close_button_.on_click(Handler::immediate(&BluetoothProperties::on_close, this));
  search_button_.set_text("Search");
  search_button_.set_dock(DockStyle::Left);
  search_button_.set_size(76, 0);
  search_button_.on_click({&BluetoothProperties::on_search, this});
  gap_a_.set_style(PanelStyle::Transparent);
  gap_a_.set_dock(DockStyle::Left);
  gap_a_.set_size(6, 0);
  connect_button_.set_text("Connect");
  connect_button_.set_dock(DockStyle::Left);
  connect_button_.set_size(80, 0);
  connect_button_.on_click({&BluetoothProperties::on_connect, this});
  gap_b_.set_style(PanelStyle::Transparent);
  gap_b_.set_dock(DockStyle::Left);
  gap_b_.set_size(6, 0);
  disconnect_button_.set_text("Disconnect");
  disconnect_button_.set_dock(DockStyle::Left);
  disconnect_button_.set_size(92, 0);
  disconnect_button_.on_click({&BluetoothProperties::on_disconnect, this});
  gap_c_.set_style(PanelStyle::Transparent);
  gap_c_.set_dock(DockStyle::Left);
  gap_c_.set_size(6, 0);
  forget_button_.set_text("Forget");
  forget_button_.set_dock(DockStyle::Left);
  forget_button_.set_size(70, 0);
  forget_button_.on_click({&BluetoothProperties::on_forget, this});
  buttons_.add(&close_button_);
  buttons_.add(&search_button_);
  buttons_.add(&gap_a_);
  buttons_.add(&connect_button_);
  buttons_.add(&gap_b_);
  buttons_.add(&disconnect_button_);
  buttons_.add(&gap_c_);
  buttons_.add(&forget_button_);

  status_.set_dock(DockStyle::Bottom);
  status_.set_size(0, 22);

  prompt_row_.set_style(PanelStyle::Transparent);
  prompt_row_.set_dock(DockStyle::Bottom);
  prompt_row_.set_size(0, 28);
  reject_button_.set_text("Cancel");
  reject_button_.set_dock(DockStyle::Right);
  reject_button_.set_size(70, 0);
  reject_button_.on_click({&BluetoothProperties::on_reject, this});
  accept_button_.set_text("OK");
  accept_button_.set_dock(DockStyle::Right);
  accept_button_.set_size(70, 0);
  accept_button_.on_click({&BluetoothProperties::on_accept, this});
  passkey_.set_dock(DockStyle::Right);
  passkey_.set_size(90, 0);
  passkey_.set_max_length(6);
  prompt_label_.set_dock(DockStyle::Fill);
  prompt_row_.add(&reject_button_);
  prompt_row_.add(&accept_button_);
  prompt_row_.add(&passkey_);
  prompt_row_.add(&prompt_label_);
  prompt_row_.set_visible(false);

  grid_.add_column("Device", 190);
  grid_.add_column("Type", 80);
  grid_.add_column("Signal", 55, Align::Right);
  grid_.add_column("Status", 160);
  grid_.set_dock(DockStyle::Fill);
  grid_.on_selection_changed({&BluetoothProperties::on_selection, this});

  add(&adapter_label_);
  add(&auto_reconnect_);
  add(&buttons_);
  add(&status_);
  add(&prompt_row_);
  add(&grid_);   // (Fill goes last)
  update_buttons();
}

StringView BluetoothProperties::cell(uint32_t row, uint32_t column) const {
  if (row >= shown_count_ || column >= kColumns) return StringView();
  const BluetoothDeviceRow& d = shown_[row];
  cell_text_.clear();
  switch (column) {
    case 0: cell_text_.append(d.name[0] ? d.name : d.address); break;
    case 1: cell_text_.append(d.kind); break;
    case 2:
      if (d.signal_percent >= 0) {
        cell_text_.append_uint(static_cast<uint32_t>(d.signal_percent));
        cell_text_.push_back('%');
      }
      break;
    default:
      if (d.state == BluetoothDeviceState::Failed) {
        cell_text_.append("Failed: ");
        cell_text_.append(d.failure);
      } else if (d.state == BluetoothDeviceState::Disconnected) {
        cell_text_.append(!d.supported ? "Not supported yet" : d.paired ? "Paired" : "Not paired");
      } else {
        cell_text_.append(bluetooth_state_name(d.state));
      }
      break;
  }
  return cell_text_.view();
}

const BluetoothDeviceRow* BluetoothProperties::selected() const {
  const int32_t row = grid_.selected_row();
  return row >= 0 && static_cast<uint32_t>(row) < shown_count_ ? &shown_[row] : nullptr;
}

void BluetoothProperties::show_adapter(uint32_t id) {
  UiGuard lock(this);
  id_ = id;
  shown_count_ = 0;
  grid_.clear_rows();
  status_.set_text(StringView());
  passkey_.set_text(StringView());
  if (details_) details_->set_discovery(id_, true);
  update();
}

void BluetoothProperties::fill_devices(const BluetoothDeviceRow* list, uint32_t count) {
  bool same = count == shown_count_;
  for (uint32_t i = 0; same && i < count; ++i) same = same_device(list[i], shown_[i]);
  if (same) return;   // (what the user looks at, or has selected, is left alone)
  uint8_t selected_key[6] = {};
  bool had_selection = false;
  if (const BluetoothDeviceRow* s = selected()) {
    for (int i = 0; i < 6; ++i) selected_key[i] = s->key[i];
    had_selection = true;
  }
  grid_.clear_rows();
  shown_count_ = 0;
  int32_t reselect = -1;
  for (uint32_t i = 0; i < count && i < IBluetoothDetails::kMaxDevices; ++i) {
    shown_[shown_count_] = list[i];
    String cells_text[kColumns] = {String(allocator()), String(allocator()), String(allocator()), String(allocator())};
    StringView cells[kColumns];
    for (uint32_t c = 0; c < kColumns; ++c) {
      cells_text[c].assign(cell(shown_count_, c));
      cells[c] = cells_text[c].view();
    }
    const int32_t row = grid_.add_row(cells, kColumns);
    if (row < 0) break;
    if (had_selection && same_key(list[i], selected_key)) reselect = row;
    ++shown_count_;
  }
  if (reselect >= 0) grid_.select_row(reselect);
}

void BluetoothProperties::update_prompt() {
  BluetoothPrompt now;
  if (details_ && known_) details_->prompt(id_, &now);
  const bool changed = now.kind != prompt_.kind || now.number != prompt_.number || !text_equal(now.device, prompt_.device);
  prompt_ = now;
  if (!changed) return;
  String text(allocator());
  const bool entry = now.kind == BluetoothPromptKind::EnterPasskey, compare = now.kind == BluetoothPromptKind::ConfirmNumber;
  switch (now.kind) {
    case BluetoothPromptKind::None: break;
    case BluetoothPromptKind::ShowPasskey:
      text.assign("Type ");
      six_digits(&text, now.number);
      text.append(" on ");
      text.append(now.device);
      text.append(", then press Enter.");
      break;
    case BluetoothPromptKind::ConfirmNumber:
      text.assign("Does ");
      text.append(now.device);
      text.append(" show ");
      six_digits(&text, now.number);
      text.append("?");
      break;
    case BluetoothPromptKind::EnterPasskey:
      text.assign("Type the passkey ");
      text.append(now.device);
      text.append(" shows:");
      break;
  }
  prompt_label_.set_text(text.view());
  passkey_.set_text(StringView());
  passkey_.set_visible(entry);
  accept_button_.set_visible(entry || compare);
  accept_button_.set_text(compare ? "Yes" : "OK");
  reject_button_.set_text(compare ? "No" : "Cancel");
  prompt_row_.set_visible(now.kind != BluetoothPromptKind::None);
  invalidate();
}

void BluetoothProperties::update_buttons() {
  const BluetoothDeviceRow* d = selected();
  const bool idle_state = d && (d->state == BluetoothDeviceState::Disconnected || d->state == BluetoothDeviceState::Failed);
  search_button_.set_text(discovering_ ? "Stop" : "Search");
  search_button_.set_enabled(known_);
  connect_button_.set_enabled(known_ && d && idle_state && d->supported);
  disconnect_button_.set_enabled(known_ && d && !idle_state);
  forget_button_.set_enabled(known_ && d && d->paired);
}

void BluetoothProperties::update() {
  UiGuard lock(this);
  BluetoothAdapterInfo info;
  if (!details_ || !details_->adapter(id_, &info)) {
    known_ = false;
    discovering_ = false;
    auto_reconnect_.set_enabled(false);
    adapter_label_.set_text(details_ ? "This Bluetooth adapter is not there any more." : "No Bluetooth information is available.");
    fill_devices(nullptr, 0);
    update_prompt();
    update_buttons();
    return;
  }
  known_ = info.ready;
  discovering_ = info.discovering;
  syncing_ = true;
  auto_reconnect_.set_checked(info.auto_reconnect);
  auto_reconnect_.set_enabled(true);
  syncing_ = false;
  String text(allocator());
  text.assign(info.name);
  text.append(" - Bluetooth");
  set_title(text.view());
  text.assign(info.name);
  if (info.address[0]) {
    text.append(" (");
    text.append(info.address);
    text.append(")");
  }
  text.append(info.ready ? (info.discovering ? " - looking for devices..." : " - ready") : " - not ready");
  adapter_label_.set_text(text.view());
  const uint32_t count = known_ ? details_->devices(id_, fresh_, IBluetoothDetails::kMaxDevices) : 0;
  fill_devices(fresh_, count < IBluetoothDetails::kMaxDevices ? count : IBluetoothDetails::kMaxDevices);
  update_prompt();
  update_buttons();
}

void BluetoothProperties::on_selection(void* self, Control&) { static_cast<BluetoothProperties*>(self)->update_buttons(); }
void BluetoothProperties::on_close(void* self, Control&) { static_cast<BluetoothProperties*>(self)->close(); }

void BluetoothProperties::on_search(void* self, Control&) {
  auto* w = static_cast<BluetoothProperties*>(self);
  UiGuard lock(w);
  if (!w->details_) return;
  const bool start = !w->discovering_;
  w->details_->set_discovery(w->id_, start);
  w->status_.set_text(start ? "Looking for devices..." : "Stopped looking.");
  w->update();
}

void BluetoothProperties::on_connect(void* self, Control&) {
  auto* w = static_cast<BluetoothProperties*>(self);
  UiGuard lock(w);
  const BluetoothDeviceRow* d = w->selected();
  if (!w->details_ || !d) {
    w->status_.set_text("Pick a device first.");
    return;
  }
  const BluetoothDeviceRow device = *d;
  const BluetoothResult result = w->details_->connect(w->id_, device.key);
  w->update();
  if (result != BluetoothResult::Ok) {
    w->status_.set_text(bluetooth_result_text(result));
    return;
  }
  String text(w->allocator());
  text.assign("Connecting to ");
  text.append(device.name[0] ? device.name : device.address);
  text.append("...");
  w->status_.set_text(text.view());
}

void BluetoothProperties::on_disconnect(void* self, Control&) {
  auto* w = static_cast<BluetoothProperties*>(self);
  UiGuard lock(w);
  const BluetoothDeviceRow* d = w->selected();
  if (!w->details_ || !d) return;
  const BluetoothDeviceRow device = *d;
  w->details_->disconnect(w->id_, device.key);
  w->status_.set_text("Disconnected.");
  w->update();
}

void BluetoothProperties::on_forget(void* self, Control&) {
  auto* w = static_cast<BluetoothProperties*>(self);
  UiGuard lock(w);
  const BluetoothDeviceRow* d = w->selected();
  if (!w->details_ || !d) return;
  const BluetoothDeviceRow device = *d;
  w->details_->forget(w->id_, device.key);
  w->status_.set_text("The pairing was deleted.");
  w->update();
}

void BluetoothProperties::on_auto_reconnect(void* self, Control&) {
  auto* w = static_cast<BluetoothProperties*>(self);
  if (w->syncing_ || !w->details_) return;
  UiGuard lock(w);
  w->details_->set_auto_reconnect(w->id_, w->auto_reconnect_.checked());
  w->status_.set_text(w->auto_reconnect_.checked() ? "Paired devices will connect again by themselves." : "Paired devices will not connect again by themselves.");
}

void BluetoothProperties::on_accept(void* self, Control&) {
  auto* w = static_cast<BluetoothProperties*>(self);
  UiGuard lock(w);
  if (!w->details_) return;
  if (w->prompt_.kind == BluetoothPromptKind::ConfirmNumber) {
    w->details_->answer_confirm(w->id_, true);
  } else if (w->prompt_.kind == BluetoothPromptKind::EnterPasskey) {
    const StringView typed = w->passkey_.text();
    uint32_t value = 0;
    if (typed.empty() || typed.size() > 6) {
      w->status_.set_text("Type the passkey the device shows (up to six digits).");
      return;
    }
    for (uint64_t i = 0; i < typed.size(); ++i) {
      if (typed[i] < '0' || typed[i] > '9') {
        w->status_.set_text("The passkey is digits only.");
        return;
      }
      value = value * 10 + static_cast<uint32_t>(typed[i] - '0');
    }
    w->details_->answer_passkey(w->id_, value);
  }
  w->update();
}

void BluetoothProperties::on_reject(void* self, Control&) {
  auto* w = static_cast<BluetoothProperties*>(self);
  UiGuard lock(w);
  if (!w->details_) return;
  if (w->prompt_.kind == BluetoothPromptKind::ConfirmNumber) {
    w->details_->answer_confirm(w->id_, false);
  } else {
    for (uint32_t i = 0; i < w->shown_count_; ++i) {                      // (cancel: whatever is being connected stops)
      const BluetoothDeviceState s = w->shown_[i].state;
      if (s == BluetoothDeviceState::Connecting || s == BluetoothDeviceState::Pairing || s == BluetoothDeviceState::Setup) w->details_->disconnect(w->id_, w->shown_[i].key);
    }
  }
  w->status_.set_text("Pairing cancelled.");
  w->update();
}

}  // namespace desktop
