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

#include "hardware/Bus/Ps2/I8042Controller.hpp"

namespace hardware {

namespace {
constexpr uint8_t kStatusOutputFull = 1 << 0;
constexpr uint8_t kStatusInputFull = 1 << 1;
constexpr uint8_t kStatusFromMouse = 1 << 5;

constexpr uint8_t kCommandReadConfig = 0x20;
constexpr uint8_t kCommandWriteConfig = 0x60;
constexpr uint8_t kCommandDisableMouse = 0xA7;
constexpr uint8_t kCommandEnableMouse = 0xA8;
constexpr uint8_t kCommandDisableKeyboard = 0xAD;
constexpr uint8_t kCommandEnableKeyboard = 0xAE;
constexpr uint8_t kCommandWriteMouse = 0xD4;

constexpr uint8_t kConfigIrqKeyboard = 1 << 0;
constexpr uint8_t kConfigIrqMouse = 1 << 1;
constexpr uint8_t kConfigKeyboardClockOff = 1 << 4;
constexpr uint8_t kConfigMouseClockOff = 1 << 5;
constexpr uint8_t kConfigTranslate = 1 << 6;  // set 2 -> set 1 scan codes

constexpr uint8_t kAck = 0xFA;
constexpr uint8_t kResend = 0xFE;
constexpr uint32_t kSpins = 200000;  // port reads; ~tens of milliseconds
}  // namespace

bool I8042Controller::wait_input_clear() const {
  for (uint32_t i = 0; i < kSpins; ++i)
    if (!(status_command_.read8() & kStatusInputFull)) return true;
  return false;
}

bool I8042Controller::write_command(uint8_t command) const {
  if (!wait_input_clear()) return false;
  status_command_.write8(command);
  return true;
}

bool I8042Controller::write_data(uint8_t byte) const {
  if (!wait_input_clear()) return false;
  data_.write8(byte);
  return true;
}

void I8042Controller::pump() {
  for (uint32_t guard = 0; guard < 16; ++guard) {
    const uint8_t status = status_command_.read8();
    if (status == 0xFF || !(status & kStatusOutputFull)) return;  // 0xFF: no controller
    const uint8_t byte = data_.read8();
    (status & kStatusFromMouse ? mouse_ : keyboard_).push(byte);
  }
}

bool I8042Controller::read_keyboard(uint8_t* out) {
  pump();
  return keyboard_.pop(out);
}

bool I8042Controller::read_mouse(uint8_t* out) {
  pump();
  return mouse_.pop(out);
}

bool I8042Controller::read_data_blocking(uint8_t* out, bool from_mouse) {
  for (uint32_t i = 0; i < kSpins; ++i) {
    if (from_mouse ? read_mouse(out) : read_keyboard(out)) return true;
  }
  return false;
}

bool I8042Controller::send(uint8_t byte, bool to_mouse) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (to_mouse && !write_command(kCommandWriteMouse)) return false;
    if (!write_data(byte)) return false;
    uint8_t reply;
    if (!read_data_blocking(&reply, to_mouse)) return false;
    if (reply == kAck) return true;
    if (reply != kResend) return false;
  }
  return false;
}

bool I8042Controller::send_keyboard(uint8_t byte) { return send(byte, false); }
bool I8042Controller::send_mouse(uint8_t byte) { return send(byte, true); }

bool I8042Controller::initialize() {
  if (status_command_.read8() == 0xFF) return false;  // floating bus: no 8042

  // Quiesce both ports, drop stale bytes, then set the configuration byte.
  write_command(kCommandDisableKeyboard);
  write_command(kCommandDisableMouse);
  for (uint32_t i = 0; i < 16 && (status_command_.read8() & kStatusOutputFull); ++i) data_.read8();

  if (!write_command(kCommandReadConfig)) return false;
  uint8_t config = 0;
  for (uint32_t i = 0; i < kSpins && !(status_command_.read8() & kStatusOutputFull); ++i) {}
  if (!(status_command_.read8() & kStatusOutputFull)) return false;
  config = data_.read8();
  config &= ~(kConfigIrqKeyboard | kConfigIrqMouse | kConfigKeyboardClockOff | kConfigMouseClockOff);
  config |= kConfigTranslate;
  if (!write_command(kCommandWriteConfig) || !write_data(config)) return false;

  write_command(kCommandEnableKeyboard);
  write_command(kCommandEnableMouse);
  for (uint32_t i = 0; i < 16 && (status_command_.read8() & kStatusOutputFull); ++i) data_.read8();
  keyboard_ = {};
  mouse_ = {};
  return true;
}

}  // namespace hardware
