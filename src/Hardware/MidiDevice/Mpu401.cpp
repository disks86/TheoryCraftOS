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

#include "Hardware/MidiDevice/Mpu401.hpp"

namespace hardware {

namespace {
constexpr uint8_t kNotReadyToWrite = 0x40, kNoDataToRead = 0x80, kCommandReset = 0xFF, kCommandUart = 0x3F, kAck = 0xFE;
}

// Status port: bit 7 set means there is nothing to read, bit 6 set means the interface cannot take a byte yet.
bool Mpu401Output::wait_ready_to_write() {
  for (uint32_t i = 0; i < kSpinLimit; ++i)
    if (!(io_.read8(base_ + 1) & kNotReadyToWrite)) return true;
  return false;
}

bool Mpu401Output::wait_data_ready() {
  for (uint32_t i = 0; i < kSpinLimit; ++i)
    if (!(io_.read8(base_ + 1) & kNoDataToRead)) return true;
  return false;
}

bool Mpu401Output::write_command(uint8_t command) {
  if (!wait_ready_to_write()) return false;
  io_.write8(base_ + 1, command);
  return true;
}

bool Mpu401Output::write_data(uint8_t value) {
  if (!wait_ready_to_write()) return false;
  io_.write8(base_, value);
  return true;
}

bool Mpu401Output::expect_ack() {
  // (an active-sensing byte or two may come first)
  for (int i = 0; i < 4; ++i) {
    if (!wait_data_ready()) return false;
    if (io_.read8(base_) == kAck) return true;
  }
  return false;
}

bool Mpu401Output::initialize() {
  kernel::ScopedMutex lock(mutex_);
  present_ = false;
  if (io_.read8(base_ + 1) == 0xFF) return false;   // nothing answers: the address floats high
  for (int i = 0; i < 16 && !(io_.read8(base_ + 1) & kNoDataToRead); ++i) io_.read8(base_);   // old input
  bool reset_ok = false;
  for (int attempt = 0; attempt < 2 && !reset_ok; ++attempt) reset_ok = write_command(kCommandReset) && expect_ack();
  if (!reset_ok) return false;
  if (!write_command(kCommandUart)) return false;   // UART mode: bytes go out as they are written
  expect_ack();   // (some interfaces do not answer in this state)
  present_ = true;
  return true;
}

void Mpu401Output::send(uint8_t status, uint8_t data1, uint8_t data2) {
  if (!present_ || status < 0x80 || status >= 0xF0) return;
  kernel::ScopedMutex lock(mutex_);
  const uint8_t kind = status & 0xF0;
  if (!write_data(status)) return;
  if (!write_data(data1 & 0x7F)) return;
  if (kind != 0xC0 && kind != 0xD0) write_data(data2 & 0x7F);
}

void Mpu401Output::reset() {
  if (!present_) return;
  midi::silence(*this);
  for (uint8_t channel = 0; channel < 16; ++channel) {
    send(static_cast<uint8_t>(0xB0 | channel), midi::kControllerResetAll, 0);
    send(static_cast<uint8_t>(0xC0 | channel), 0, 0);
    send(static_cast<uint8_t>(0xE0 | channel), 0, 0x40);   // pitch bend centre
  }
}

}  // namespace hardware
