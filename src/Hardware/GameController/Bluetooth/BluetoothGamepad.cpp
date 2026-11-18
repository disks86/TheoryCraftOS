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

#include "Hardware/GameController/Bluetooth/BluetoothGamepad.hpp"
#include "Hardware/GameController/GameControllerCatalog.hpp"
#include "Hardware/GameController/HidGamepad.hpp"
#include "Hardware/GameController/SonyProtocol.hpp"
#include "Hardware/GameController/SwitchProtocol.hpp"
#include "Hardware/GameController/XboxProtocol.hpp"

namespace hardware {

namespace {
constexpr uint16_t kMicrosoft = 0x045E;

enum class Kind : uint8_t { None, Xbox, DualShock4, DualSense, SwitchPro, Hid };

// The gamepad report of a report map, or null (the map is untrusted: parse_report_map checks it).
bool gamepad_report(ConstByteView report_map, hid::ReportMap* map, const hid::InputReport** report) {
  *report = nullptr;
  if (report_map.empty() || !hid::parse_report_map(report_map, map)) return false;
  *report = hid::find_gamepad_report(*map);
  return *report != nullptr;
}

Kind kind_of(const BluetoothPadIdentity& id, bool has_gamepad_report) {
  if (const gamepad::CatalogEntry* e = gamepad::find_controller(id.vendor_id, id.product_id)) {
    if (e->protocol == gamepad::Protocol::DualShock4) return Kind::DualShock4;
    if (e->protocol == gamepad::Protocol::DualSense) return Kind::DualSense;
    if (e->protocol == gamepad::Protocol::SwitchPro) return Kind::SwitchPro;
  }
  if (!has_gamepad_report) return Kind::None;
  return id.vendor_id == kMicrosoft ? Kind::Xbox : Kind::Hid;
}

class BluetoothGamepad final : public IGameController {
 public:
  BluetoothGamepad(SharedPointer<HidPadChannel> channel, Kind kind, const BluetoothPadIdentity& identity)
      : channel_(util::move(channel)), kind_(kind) {
    switch (kind) {
      case Kind::Xbox: info_.protocol = gamepad::Protocol::XboxOne; break;
      case Kind::DualShock4: info_.protocol = gamepad::Protocol::DualShock4; break;
      case Kind::DualSense: info_.protocol = gamepad::Protocol::DualSense; break;
      case Kind::SwitchPro: info_.protocol = gamepad::Protocol::SwitchPro; break;
      default: info_.protocol = gamepad::Protocol::Hid; break;
    }
    info_.vendor_id = identity.vendor_id;
    info_.product_id = identity.product_id;
    info_.capabilities = kind == Kind::Xbox ? gamepad::kCapRumble | gamepad::kCapTriggers | gamepad::kCapRightStick
                                            : kind == Kind::Hid ? 0 : gamepad::protocol_capabilities(info_.protocol) | gamepad::kCapBattery;
    const gamepad::CatalogEntry* entry = gamepad::find_controller(identity.vendor_id, identity.product_id);
    const char* name = entry ? entry->name : identity.name && identity.name[0] ? identity.name : "Bluetooth game controller";
    uint32_t i = 0;
    for (; name[i] && i + 1 < sizeof(info_.name); ++i) info_.name[i] = name[i];
    info_.name[i] = 0;
  }

  // The HID decoder for Xbox and generic pads.
  bool set_report(const hid::InputReport& report) {
    if (!hid_.init(report)) return false;
    hid_report_id_ = report.id;
    if (kind_ == Kind::Xbox) hid_.set_button_map(ArrayView<const uint32_t>(xbox::kBluetoothButtonMap));
    info_.capabilities |= hid_.capabilities();
    return true;
  }

  const char* name() const override { return info_.name; }

  bool initialize() override { return true; }

  // What a pad needs to start sending its full reports. Done from the first poll (the game controller service's thread), never from the
  // transport's: a feature read waits for the transport to bring the answer in.
  void start() {
    started_ = true;
    uint8_t buffer[96];
    uint32_t length = 0;
    switch (kind_) {
      case Kind::DualShock4:
        (void)channel_->get_feature(sony::kDualShock4BtEnableFeature, ByteView(buffer), &length);   // (the read itself is the switch; no answer: it stays reduced)
        break;
      case Kind::DualSense:
        (void)channel_->get_feature(sony::kDualSenseBtEnableFeature, ByteView(buffer), &length);
        break;
      case Kind::SwitchPro:
        (void)channel_->send_output(ConstByteView(buffer).first(nintendo::encode_full_report_mode(sequence_++, ByteView(buffer))));
        break;
      default:
        break;
    }
  }
  void shutdown() override {
    if (!present()) return;
    gamepad::Output quiet;
    quiet.player = output_.player;
    (void)set_output(quiet);
  }
  const gamepad::Info& info() const override { return info_; }
  const gamepad::State& state() const override { return state_; }
  bool present() const override { return channel_->attached(); }

  bool poll() override {
    if (!present()) return false;
    if (!started_) start();
    bool changed = false;
    uint8_t report[HidPadChannel::kMaxReport];
    uint32_t length = 0;
    for (uint32_t budget = 0; budget < HidPadChannel::kRing && channel_->take(ByteView(report), &length); ++budget) {
      const gamepad::State before = state_;
      if (!decode(ConstByteView(report).first(length))) continue;
      changed = changed || before.buttons != state_.buttons || before.left_x != state_.left_x || before.left_y != state_.left_y ||
                before.right_x != state_.right_x || before.right_y != state_.right_y || before.left_trigger != state_.left_trigger ||
                before.right_trigger != state_.right_trigger;
    }
    return changed;
  }

  bool set_output(const gamepad::Output& output) override {
    if (!present()) return false;
    const gamepad::Output before = output_;
    output_ = output;
    uint8_t message[96];
    switch (kind_) {
      case Kind::Xbox:
        return channel_->send_output(ConstByteView(message).first(xbox::encode_bt_rumble(output.low_frequency, output.high_frequency, ByteView(message))));
      case Kind::DualShock4:
        return channel_->send_output(ConstByteView(message).first(sony::encode_dualshock4_bt_output(output, ByteView(message))));
      case Kind::DualSense:
        return channel_->send_output(ConstByteView(message).first(sony::encode_dualsense_bt_output(output, sequence_++, ByteView(message))));
      case Kind::SwitchPro:
        if (output.player == before.player) return output.low_frequency == 0 && output.high_frequency == 0;   // (no rumble)
        return channel_->send_output(ConstByteView(message).first(nintendo::encode_player_leds(sequence_++, output.player, ByteView(message))));
      default:
        return false;
    }
  }

 private:
  bool decode(ConstByteView r) {
    switch (kind_) {
      case Kind::DualShock4:
        return sony::decode_dualshock4_bt(r, &state_) || sony::decode_bt_reduced(r, &state_);
      case Kind::DualSense:
        return sony::decode_dualsense_bt(r, &state_) || sony::decode_bt_reduced(r, &state_);
      case Kind::SwitchPro:
        return nintendo::decode_full_report(r, &state_) || nintendo::decode_simple_report(r, &state_);
      case Kind::Xbox:
      case Kind::Hid: {
        ConstByteView data = r;
        if (hid_report_id_ != 0) {
          if (data.empty() || data[0] != hid_report_id_) return false;
          data = data.dropped(1);
        }
        if (!hid_.decode(data, &state_)) return false;
        if (kind_ == Kind::Xbox) state_.battery_kind = gamepad::BatteryKind::Unknown;   // (the level is in the Battery service, not read here)
        return true;
      }
      case Kind::None:
        break;
    }
    return false;
  }

  SharedPointer<HidPadChannel> channel_;
  Kind kind_;
  gamepad::Info info_;
  gamepad::State state_;
  gamepad::Output output_;
  hid::GamepadDecoder hid_;
  uint8_t hid_report_id_ = 0;
  uint8_t sequence_ = 0;
  bool started_ = false;
};
}  // namespace

bool is_bluetooth_gamepad(const BluetoothPadIdentity& identity, ConstByteView report_map) {
  hid::ReportMap map;
  const hid::InputReport* report = nullptr;
  return kind_of(identity, gamepad_report(report_map, &map, &report)) != Kind::None;
}

UniquePointer<IGameController> make_bluetooth_gamepad(allocators::IAllocator& allocator, SharedPointer<HidPadChannel> channel,
                                                      const BluetoothPadIdentity& identity, ConstByteView report_map) {
  if (!channel) return UniquePointer<IGameController>();
  hid::ReportMap map;
  const hid::InputReport* report = nullptr;
  const bool has_report = gamepad_report(report_map, &map, &report);
  const Kind kind = kind_of(identity, has_report);
  if (kind == Kind::None) return UniquePointer<IGameController>();
  UniquePointer<BluetoothGamepad> pad = UniquePointer<BluetoothGamepad>::make(allocator, util::move(channel), kind, identity);
  if (!pad) return UniquePointer<IGameController>();
  if ((kind == Kind::Xbox || kind == Kind::Hid) && !pad->set_report(*report)) return UniquePointer<IGameController>();
  if (!pad->initialize()) return UniquePointer<IGameController>();
  return UniquePointer<IGameController>(util::move(pad));
}

}  // namespace hardware
