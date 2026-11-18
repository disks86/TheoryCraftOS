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

#pragma once
#include "Hardware/GameController/GamepadTypes.hpp"
#include "Hardware/IHardware.hpp"

namespace hardware {

// One game controller, however it is attached. Its driver reads the pad when poll() is called (from the game controller service's thread)
// and keeps the last state; the service copies it out under its own lock, so a driver needs no locking of its own.
class IGameController : public IHardware {
 public:
  virtual const gamepad::Info& info() const = 0;
  // Reads what has arrived from the pad; true if the state changed.
  virtual bool poll() = 0;
  virtual const gamepad::State& state() const = 0;
  // Sends motors and lights; false if the pad has neither or the transfer failed.
  [[nodiscard]] virtual bool set_output(const gamepad::Output& output) = 0;
  // False once the pad was unplugged (its driver is then dropped).
  virtual bool present() const = 0;
  // False while a pad that can come and go without its device leaving (one paired with a wireless receiver) is not there: it then holds no
  // player slot, and gets one again when it pairs.
  virtual bool connected() const { return true; }

 protected:
  IGameController() = default;
  ~IGameController() = default;
};

}  // namespace hardware
