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

// The profiles that serve remote devices, shared by the adapters of a manager: a profile registered once is offered the devices of every adapter, and a profile registered after an
// adapter came up is offered the devices that connect from then on.
#pragma once
#include "Hardware/Bluetooth/IBluetooth.hpp"

namespace hardware {

class BluetoothProfileRegistry {
 public:
  static constexpr uint32_t kMaxProfiles = 8;
  bool add(IBluetoothProfile* profile) {
    if (!profile) return false;
    for (IBluetoothProfile* p : profiles_)
      if (p == profile) return true;
    for (IBluetoothProfile*& p : profiles_) {
      if (p) continue;
      p = profile;
      return true;
    }
    return false;
  }
  bool remove(IBluetoothProfile* profile) {
    for (IBluetoothProfile*& p : profiles_)
      if (p == profile && p) { p = nullptr; return true; }
    return false;
  }
  uint32_t count() const {
    uint32_t n = 0;
    for (IBluetoothProfile* p : profiles_) n += p != nullptr;
    return n;
  }
  IBluetoothProfile* at(uint32_t slot) const { return slot < kMaxProfiles ? profiles_[slot] : nullptr; }

 private:
  IBluetoothProfile* profiles_[kMaxProfiles] = {};
};

}  // namespace hardware
