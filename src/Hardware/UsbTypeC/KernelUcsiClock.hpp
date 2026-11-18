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

// The kernel's time for UCSI: the uptime, and a sleep that gives up the core on a thread (a busy wait before the scheduler runs).
#pragma once
#include "Hardware/UsbTypeC/IUcsiTransport.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Thread.hpp"

namespace hardware {

class KernelUcsiClock final : public IUcsiClock {
 public:
  uint64_t now_us() override { return hardware::uptime_us(); }
  void sleep_us(uint32_t us) override {
    if (kernel::running() && kernel::current_thread()) kernel::sleep_us(us);
    else hardware::delay_us(us);
  }
};

}  // namespace hardware
