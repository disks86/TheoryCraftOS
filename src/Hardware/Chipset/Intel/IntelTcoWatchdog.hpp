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

// The TCO watchdog of the Intel PCH (Linux iTCO_wdt, versions 4 and 6): a down-counter in 0.6 s ticks that, when it runs out twice, resets the
// machine, unless NO_REBOOT is set. Registers are 16-bit I/O ports from TCOBASE: TCO_RLD (0x00, writing reloads, reading gives the ticks left),
// TCO1_STS (0x04), TCO2_STS (0x06: bit 1 SECOND_TO_STS, the last reset was this watchdog), TCO1_CNT (0x08: bit 11 halts the timer, bit 8 NMI_NOW
// which a write of 1 toggles, bit 0 NO_REBOOT on version 6) and TCOv2_TMR (0x12, the initial count, 10 bits).
//
// Version 4 keeps NO_REBOOT behind the P2SB bridge: there the timer can be halted and its state read, but not armed.
#pragma once
#include <stdint.h>
#include "Hardware/Chipset/Intel/IntelPch.hpp"

namespace hardware {

// The 16-bit TCO registers (offsets from TCOBASE). TcoIoPorts on the real machine; a model in the tests.
class ITcoPorts {
 public:
  virtual uint16_t read16(uint8_t reg) = 0;
  virtual void write16(uint8_t reg, uint16_t value) = 0;

 protected:
  constexpr ITcoPorts() = default;
  ~ITcoPorts() = default;
};

namespace tco_reg {
constexpr uint8_t kReload = 0x00, kStatus1 = 0x04, kStatus2 = 0x06, kControl1 = 0x08, kTimer = 0x12;
constexpr uint16_t kStatus1Timeout = 1u << 3;
constexpr uint16_t kStatus2SecondTimeout = 1u << 1, kStatus2Boot = 1u << 2;
constexpr uint16_t kControl1Halt = 1u << 11, kControl1NmiNow = 1u << 8, kControl1NoReboot = 1u << 0;
constexpr uint16_t kTimerMask = 0x3FF;
}  // namespace tco_reg

class IntelTcoWatchdog {
 public:
  static constexpr uint32_t kMinTicks = 4, kMaxTicks = 0x3FF;
  static uint32_t ticks_of(uint32_t seconds) { return seconds * 10 / 6; }
  static uint32_t seconds_of(uint32_t ticks) { return ticks * 6 / 10; }

  // `ports` must outlive this. False for a hub without a TCO the OS can reach.
  bool attach(ITcoPorts* ports, IntelTcoKind kind);
  bool attached() const { return ports_ != nullptr; }
  IntelTcoKind kind() const { return kind_; }
  bool can_arm() const { return kind_ == IntelTcoKind::Version6; }

  bool running() const;
  // The initial count, in seconds.
  uint32_t timeout_seconds() const;
  uint32_t remaining_seconds() const;
  // The previous reset was this watchdog running out (SECOND_TO_STS). Cleared by clear_status().
  bool caused_last_reset() const;
  void clear_status();

  // Sets the initial count (about 2.4 s .. 613 s). False if out of range or the register did not take it.
  bool set_timeout(uint32_t seconds);
  // Version 6 only: clears NO_REBOOT, reloads and starts the count. False if NO_REBOOT stayed set (the firmware locked it) or the timer did not start.
  bool start();
  // Halts the count and, on version 6, sets NO_REBOOT. True when the timer reads back halted.
  bool stop();
  // Reloads the count.
  void ping();

 private:
  // TCO1_CNT without NMI_NOW: writing that bit back as 1 would toggle it.
  uint16_t control() const;
  bool set_no_reboot(bool set);

  ITcoPorts* ports_ = nullptr;
  IntelTcoKind kind_ = IntelTcoKind::None;
};

}  // namespace hardware
