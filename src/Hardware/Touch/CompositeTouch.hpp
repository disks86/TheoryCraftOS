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

// Presents several touch devices as one: their events in turn, each contact id made unique across the devices (the device's place in the high byte).
// Pointer emulation set here reaches every device, those added later too. Children are not owned; devices may be added and removed while other threads poll.
#pragma once
#include "Hardware/Touch/ITouch.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

class CompositeTouch final : public ITouch {
 public:
  static constexpr uint32_t kMaxDevices = 4;
  constexpr CompositeTouch() = default;

  const char* name() const override { return "touchscreens"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  bool add(ITouch* touch);
  bool remove(ITouch* touch);
  uint32_t count() const;

  bool poll(TouchEvent* out) override;
  void set_pointer_emulation(bool on) override;

 private:
  mutable kernel::Mutex mutex_;
  ITouch* devices_[kMaxDevices] = {};      // a device keeps its slot while it is there (its ids' high byte)
  uint32_t next_ = 0;
  bool emulation_ = true;
};

}  // namespace hardware
