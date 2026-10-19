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
#include <stdint.h>

namespace hardware {

// QEMU's pvpanic device (`-device pvpanic`, ISA port 0x505): the guest tells the host it has crashed, so
// the host can act on it (`-action panic=...`, a libvirt on_crash policy) instead of seeing a hung machine.
// The signalling half is safe from a panic handler: no allocation, no locks.
class PvPanic {
 public:
  static constexpr uint16_t kPort = 0x505;
  static constexpr uint8_t kPanicked = 1u << 0;
  static constexpr uint8_t kShutdown = 1u << 2;

  // Probes the port. True if a pvpanic device answers (events it understands are then in events()).
  static bool probe();
  static bool present() { return present_; }
  static uint8_t events() { return events_; }
  // Reports a guest crash to the host; false (and nothing written) if there is no device.
  static bool notify_panic();

 private:
  static inline bool present_ = false;
  static inline uint8_t events_ = 0;
};

}  // namespace hardware
