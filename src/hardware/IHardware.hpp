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

// Base interface for everything that touches hardware (CPU, ports, devices,
// firmware services). Freestanding: no RTTI, exceptions or heap, so the
// destructor is protected and non-virtual - never delete through IHardware.
#pragma once

namespace hardware {

class IHardware {
 public:
  // Short human-readable device name, for logs.
  virtual const char* name() const = 0;
  // Bring the device to a usable state. Returns false if it is unavailable.
  virtual bool initialize() = 0;
  // Quiesce the device. Safe to call when not initialized.
  virtual void shutdown() = 0;

 protected:
  constexpr IHardware() = default;
  ~IHardware() = default;
};

}  // namespace hardware
