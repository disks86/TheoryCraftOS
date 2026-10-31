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

// Something that finds storage devices that come and go and are not on a bus SystemStorageDevices watches itself: the card in an SD slot, a future optical drive tray. The system asks it
// from SystemStorageDevices::poll(); it hands over a device for each new medium. A medium that is taken out makes its device report present() == false, which the system notices.
#pragma once
#include "ObjectContainer/List.hpp"
#include "Hardware/StorageDevice/IStorageDevice.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class IRemovableMediaSource {
 public:
  // Adds to `found` the devices that appeared since the last call (initialized and ready to use).
  virtual void discover(List<UniquePointer<IStorageDevice>>& found) = 0;

 protected:
  constexpr IRemovableMediaSource() = default;
  ~IRemovableMediaSource() = default;
};

}  // namespace hardware
