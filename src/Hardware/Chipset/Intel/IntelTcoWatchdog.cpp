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

#include "Hardware/Chipset/Intel/IntelTcoWatchdog.hpp"

namespace hardware {

using namespace tco_reg;

bool IntelTcoWatchdog::attach(ITcoPorts* ports, IntelTcoKind kind) {
  ports_ = nullptr;
  kind_ = IntelTcoKind::None;
  if (!ports || kind == IntelTcoKind::None) return false;
  if (ports->read16(kControl1) == 0xFFFF) return false;   // nothing decodes the ports
  ports_ = ports;
  kind_ = kind;
  return true;
}

uint16_t IntelTcoWatchdog::control() const { return static_cast<uint16_t>(ports_->read16(kControl1) & ~kControl1NmiNow); }

bool IntelTcoWatchdog::running() const { return ports_ && !(ports_->read16(kControl1) & kControl1Halt); }

uint32_t IntelTcoWatchdog::timeout_seconds() const { return ports_ ? seconds_of(ports_->read16(kTimer) & kTimerMask) : 0; }

uint32_t IntelTcoWatchdog::remaining_seconds() const { return ports_ ? seconds_of(ports_->read16(kReload) & kTimerMask) : 0; }

bool IntelTcoWatchdog::caused_last_reset() const { return ports_ && (ports_->read16(kStatus2) & kStatus2SecondTimeout); }

void IntelTcoWatchdog::clear_status() {
  if (!ports_) return;
  ports_->write16(kStatus1, kStatus1Timeout);   // write 1 to clear
  ports_->write16(kStatus2, kStatus2SecondTimeout);
}

bool IntelTcoWatchdog::set_timeout(uint32_t seconds) {
  if (!ports_ || seconds > kMaxTicks) return false;
  const uint32_t ticks = ticks_of(seconds);
  if (ticks < kMinTicks || ticks > kMaxTicks) return false;
  const uint16_t value = static_cast<uint16_t>((ports_->read16(kTimer) & ~kTimerMask) | ticks);
  ports_->write16(kTimer, value);
  return (ports_->read16(kTimer) & kTimerMask) == ticks;
}

bool IntelTcoWatchdog::set_no_reboot(bool set) {
  if (kind_ != IntelTcoKind::Version6) return false;
  const uint16_t value = static_cast<uint16_t>(set ? control() | kControl1NoReboot : control() & ~kControl1NoReboot);
  ports_->write16(kControl1, value);
  return control() == value;
}

bool IntelTcoWatchdog::start() {
  if (!ports_ || !can_arm() || !set_no_reboot(false)) return false;
  ports_->write16(kReload, 1);
  ports_->write16(kControl1, static_cast<uint16_t>(control() & ~kControl1Halt));
  return running();
}

bool IntelTcoWatchdog::stop() {
  if (!ports_) return false;
  ports_->write16(kControl1, static_cast<uint16_t>(control() | kControl1Halt));
  if (kind_ == IntelTcoKind::Version6) set_no_reboot(true);
  return !running();
}

void IntelTcoWatchdog::ping() {
  if (ports_) ports_->write16(kReload, 1);
}

}  // namespace hardware
