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
#include "Kernel/Sync.hpp"
#include "Hardware/Mouse/IMouse.hpp"

namespace hardware {

// Event queue shared by mice. A subclass implements fetch(), which reads
// whatever the device has and calls push() for each event.
class MouseBase : public IMouse {
 public:
  bool poll(MouseEvent* out) override;

 protected:
  constexpr MouseBase() = default;
  ~MouseBase() = default;

  virtual void fetch() = 0;
  void push(const MouseEvent& event);

 private:
  static constexpr uint32_t kCapacity = 64;
  MouseEvent queue_[kCapacity] = {};
  uint32_t head_ = 0, count_ = 0;
  kernel::Mutex mutex_;  // polling threads take turns
};

}  // namespace hardware
