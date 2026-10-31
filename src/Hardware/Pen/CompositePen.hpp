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

// Presents several pens as one stream of events, and remembers the last event so that a program that gets its pointer from the mouse stream (the pen is a mouse as well, see PenMouse)
// can still ask for the pressure and tilt that go with the pointer. Children are not owned; pens may be added and removed while other threads poll.
#pragma once
#include "Hardware/Pen/IPen.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

class CompositePen final : public IPen {
 public:
  static constexpr uint32_t kMaxPens = 4;
  constexpr CompositePen() = default;

  const char* name() const override { return "pens"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  bool add(IPen* pen);
  bool remove(IPen* pen);
  uint32_t count() const;
  IPen* at(uint32_t index) const;

  bool poll(PenEvent* out) override;
  uint32_t max_x() const override { return 0; }
  uint32_t max_y() const override { return 0; }
  uint16_t max_pressure() const override { return 0; }
  bool has_tilt() const override { return false; }

  // The latest event any pen delivered (an empty one before the first).
  PenEvent last() const;

 private:
  mutable kernel::Mutex mutex_;
  IPen* pens_[kMaxPens] = {};
  uint32_t count_ = 0, next_ = 0;
  PenEvent last_;
};

}  // namespace hardware
