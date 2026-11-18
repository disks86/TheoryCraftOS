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

// The machine's Chrome EC (Framework laptops, Chromebooks), on the real I/O ports. probe() looks for the EC device in the ACPI namespace
// (GOOG0004, FRMW0004, FRMW0005); without one it only reads the memory map signature, so nothing is written to the ports of a machine that
// does not have the EC. When the ACPI tables name the EC, the interpreter's lock is held around each host command, so the firmware's AML
// (which on Framework's AMD boards issues host commands too, under its ECMT mutex) never runs in the middle of one.
//
// Once found, the EC's sensors and fans are registered with the system hardware monitor (system_hardware_monitor()->embedded_controller())
// and the EC is reachable through system_chrome_ec() for the charge limit, the privacy switches and the keyboard backlight.
#pragma once
#include <stdint.h>
#include "Hardware/EmbeddedController/ChromeEc/ChromeEcStack.hpp"

namespace acpi {
class AmlInterpreter;
}

namespace hardware {

class SystemChromeEc {
 public:
  SystemChromeEc() : stack_(ports_) {}
  ~SystemChromeEc();
  SystemChromeEc(const SystemChromeEc&) = delete;
  SystemChromeEc& operator=(const SystemChromeEc&) = delete;

  // `vm` (may be nullptr): the interpreter that loaded the machine's ACPI tables. True when an EC answered; then it is registered (above).
  [[nodiscard]] bool probe(acpi::AmlInterpreter* vm);
  // Another interpreter that runs the firmware's AML (the ACPI event handler's): its lock is held around host commands as well.
  void add_interpreter(acpi::AmlInterpreter& vm);

  bool present() const { return stack_.present(); }
  bool found_in_acpi() const { return found_in_acpi_; }
  const char* problem() const { return stack_.problem(); }
  ChromeEcStack& stack() { return stack_; }

 private:
  // The interpreters' locks, taken in the order they were added. Each set is filled before it is handed to the EC and not changed after.
  struct AmlLocks final : IEcFirmwareLock {
    kernel::RecursiveMutex* mutexes[2] = {};
    uint32_t count = 0;
    void lock() override;
    void unlock() override;
  };

  SystemChromeEcPorts ports_;
  ChromeEcStack stack_;
  AmlLocks one_, two_;
  bool found_in_acpi_ = false;
};

SystemChromeEc* system_chrome_ec();
void set_system_chrome_ec(SystemChromeEc* ec);

}  // namespace hardware
