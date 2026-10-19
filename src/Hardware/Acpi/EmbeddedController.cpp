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

#include "Hardware/Acpi/EmbeddedController.hpp"

namespace hardware {

bool EmbeddedController::wait(uint8_t mask, bool set) {
  for (uint32_t i = 0; i < kPollLimit; ++i) {
    if (((ports_.in8(command_port_) & mask) != 0) == set) return true;
    ports_.delay_us(kPollMicroseconds);
  }
  return false;
}

bool EmbeddedController::read(uint8_t address, uint8_t* value) {
  if (!configured() || broken()) return false;
  // An answer left over from an earlier event would be taken for ours.
  for (int i = 0; i < 16 && (ports_.in8(command_port_) & kStatusOutputFull); ++i) ports_.in8(data_port_);
  bool ok = wait(kStatusInputFull, false);
  if (ok) {
    ports_.out8(command_port_, kCommandRead);
    ok = wait(kStatusInputFull, false);
  }
  if (ok) {
    ports_.out8(data_port_, address);
    ok = wait(kStatusOutputFull, true);
  }
  if (ok) *value = ports_.in8(data_port_);
  finish(ok);
  return ok;
}

bool EmbeddedController::write(uint8_t address, uint8_t value) {
  if (!configured() || broken()) return false;
  for (int i = 0; i < 16 && (ports_.in8(command_port_) & kStatusOutputFull); ++i) ports_.in8(data_port_);
  bool ok = wait(kStatusInputFull, false);
  if (ok) {
    ports_.out8(command_port_, kCommandWrite);
    ok = wait(kStatusInputFull, false);
  }
  if (ok) {
    ports_.out8(data_port_, address);
    ok = wait(kStatusInputFull, false);
  }
  if (ok) {
    ports_.out8(data_port_, value);
    ok = wait(kStatusInputFull, false);
  }
  finish(ok);
  return ok;
}

}  // namespace hardware
