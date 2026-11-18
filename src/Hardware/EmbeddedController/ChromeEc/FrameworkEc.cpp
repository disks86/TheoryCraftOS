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

#include "Hardware/EmbeddedController/ChromeEc/FrameworkEc.hpp"

namespace hardware {

using namespace chromeec;

Optional<FrameworkChargeLimit> FrameworkEc::charge_limit() {
  const uint8_t in[3] = {kChargeLimitGet, 0, 0};
  uint8_t out[2] = {};
  const EcReply r = ec_.command(command::kFrameworkChargeLimitControl, 0, ConstByteView(in), ByteView(out));
  if (!r.ok() || r.data_bytes < 2) return {};
  FrameworkChargeLimit limit;
  // (an EC with the limit off reports 0 or 100 as the maximum; a value above 100 is nonsense and not believed)
  if (out[0] > 100 || out[1] > 100) return {};
  limit.max_percent = out[0] == 0 ? 100 : out[0];
  limit.min_percent = out[1];
  return limit;
}

bool FrameworkEc::set_charge_limit(uint8_t max_percent) {
  if (max_percent < kMinChargeLimit || max_percent > 100) return false;
  const uint8_t in[3] = {max_percent == 100 ? kChargeLimitDisable : kChargeLimitSet, max_percent == 100 ? uint8_t{0} : max_percent, 0};
  uint8_t out[2] = {};
  return ec_.command(command::kFrameworkChargeLimitControl, 0, ConstByteView(in), ByteView(out)).ok();
}

Optional<FrameworkPrivacySwitches> FrameworkEc::privacy_switches() {
  uint8_t out[2] = {};
  const EcReply r = ec_.command(command::kFrameworkPrivacySwitches, 0, ConstByteView(), ByteView(out));
  if (!r.ok() || r.data_bytes < 2) return {};
  FrameworkPrivacySwitches s;
  s.microphone_connected = out[0] != 0;
  s.camera_connected = out[1] != 0;
  return s;
}

Optional<FrameworkChassisIntrusion> FrameworkEc::chassis_intrusion() {
  const uint8_t in[2] = {0, 0};   // no clear
  uint8_t out[4] = {};
  const EcReply r = ec_.command(command::kFrameworkChassisIntrusion, 0, ConstByteView(in), ByteView(out));
  if (!r.ok() || r.data_bytes < 4) return {};
  FrameworkChassisIntrusion c;
  c.ever_opened = out[0] != 0;
  c.coin_cell_ever_removed = out[1] != 0;
  c.open_count = out[2];
  c.open_count_powered_off = out[3];
  return c;
}

Optional<FrameworkFingerprintLed> FrameworkEc::fingerprint_led() {
  const uint8_t in[2] = {0, 1};   // get
  uint8_t out[2] = {};
  const EcReply r = ec_.command(command::kFrameworkFpLedLevel, 0, ConstByteView(in), ByteView(out));
  if (!r.ok() || r.data_bytes < 2 || out[0] > 100) return {};
  FrameworkFingerprintLed led;
  led.percent = out[0];
  led.level = out[1];
  return led;
}

bool FrameworkEc::set_fingerprint_led(FpLedLevel level) {
  if (static_cast<uint8_t>(level) > static_cast<uint8_t>(FpLedLevel::Low)) return false;
  const uint8_t in[2] = {static_cast<uint8_t>(level), 0};
  uint8_t out[2] = {};
  return ec_.command(command::kFrameworkFpLedLevel, 0, ConstByteView(in), ByteView(out)).ok();
}

Optional<FrameworkInputDeck> FrameworkEc::input_deck() {
  const uint8_t mode = 0;   // read only: never switch the deck's power
  uint8_t out[9] = {};
  const EcReply r = ec_.command(command::kFrameworkDeckState, 0, ConstByteView(&mode, 1), ByteView(out));
  if (!r.ok() || r.data_bytes < 9) return {};
  FrameworkInputDeck deck;
  ArrayView<uint8_t>(deck.board_ids).copy_from(ConstByteView(out).first(8));
  deck.state = out[8];
  return deck;
}

Optional<FrameworkExpansionBay> FrameworkEc::expansion_bay() {
  uint8_t out[3] = {};
  const EcReply r = ec_.command(command::kFrameworkExpansionBayStatus, 0, ConstByteView(), ByteView(out));
  if (!r.ok() || r.data_bytes < 3) return {};
  FrameworkExpansionBay bay;
  bay.enabled = out[0] & kBayEnabled;
  bay.fault = out[0] & kBayFault;
  bay.door_closed = out[0] & kBayDoorClosed;
  bay.board_id_0 = out[1];
  bay.board_id_1 = out[2];
  return bay;
}

}  // namespace hardware
