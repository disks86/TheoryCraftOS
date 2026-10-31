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

// The ACPI embedded controller (the small processor of a laptop that knows the battery, the keys and the fans): its register space is read and written
// byte by byte through two I/O ports, a data port and a command/status port (ACPI specification, chapter 12). This is the polling protocol; the
// controller's events are found by the caller: status() shows SCI_EVT, query() asks which event it was.
#pragma once
#include <stdint.h>
#include "Kernel/Sync.hpp"

namespace hardware {

// What the controller needs from the machine: the ports and a short delay.
class IEcPorts {
 public:
  virtual uint8_t in8(uint16_t port) = 0;
  virtual void out8(uint16_t port, uint8_t value) = 0;
  virtual void delay_us(uint32_t us) = 0;

 protected:
  constexpr IEcPorts() = default;
  ~IEcPorts() = default;
};

class EmbeddedController {
 public:
  explicit EmbeddedController(IEcPorts& ports) : ports_(ports) {}

  void configure(uint16_t data_port, uint16_t command_port) {
    data_port_ = data_port;
    command_port_ = command_port;
    failures_ = 0;
  }
  bool configured() const { return command_port_ != 0; }
  // False if the controller did not answer in time. After kMaxFailures in a row it is given up on (a broken or absent controller would otherwise stall
  // every read).
  bool read(uint8_t address, uint8_t* value);
  bool write(uint8_t address, uint8_t value);
  bool broken() const { return failures_ >= kMaxFailures; }
  // The status register: bit 5 (kStatusSciEvent) says the controller has an event for the OS.
  bool status(uint8_t* value);
  // QR_EC: the number of the pending event (0 if there is none); the method _Qxx of that number handles it.
  bool query(uint8_t* event);
  static constexpr uint8_t kStatusSciEvent = 0x20;

  static constexpr uint32_t kMaxFailures = 3;

 private:
  static constexpr uint8_t kStatusOutputFull = 0x01, kStatusInputFull = 0x02;
  static constexpr uint8_t kCommandRead = 0x80, kCommandWrite = 0x81, kCommandQuery = 0x84;
  static constexpr uint32_t kPollMicroseconds = 10, kPollLimit = 5000;   // up to 50 ms per step
  bool wait(uint8_t mask, bool set);
  void finish(bool ok) { failures_ = ok ? 0 : failures_ + 1; }

  IEcPorts& ports_;
  kernel::Mutex mutex_;                // a transaction on the two ports is not interleaved with another (the battery's interpreter and the event interpreter share the controller)
  uint16_t data_port_ = 0, command_port_ = 0;
  uint32_t failures_ = 0;
};

}  // namespace hardware
