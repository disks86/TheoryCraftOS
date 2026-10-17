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
#include "Hardware/Printer/PrinterReport.hpp"
#include "Network/Snmp/SnmpTrap.hpp"

namespace hardware {

// A printer's unsolicited message, understood: the standard traps (cold and warm start, link up and down, authentication failure)
// and the Printer MIB's prtAlert notification (RFC 3805: severity, group, location and code of the alert that was raised or
// cleared - "toner low", "jam", "door open"...).
struct PrinterAlertEvent {
  enum class Kind : uint8_t { Alert, ColdStart, WarmStart, LinkDown, LinkUp, AuthenticationFailure, Other };
  Kind kind = Kind::Other;
  network::Ipv4Address source = network::kAnyAddress;
  uint32_t uptime = 0;                 // the printer's sysUpTime, hundredths of a second
  network::Oid trap_oid;
  // prtAlert only:
  AlertSeverity severity = AlertSeverity::Other;
  uint32_t group = 0;                  // prtAlertGroup (hrDeviceOther(1), input(8), output(9), marker(6), marker supplies(7)...)
  uint32_t group_index = 0;
  uint32_t code = 0;                   // prtAlertCode: see alert_code_name()
  char location[48] = {};
  bool inform = false;
};

// Receives the printers' traps and informs on UDP port 162 (SnmpTrapReceiver underneath) and turns them into PrinterAlertEvents.
// The printer has to be told where to send them (its web page or SNMP settings: the manager's address and the community).
class PrinterAlertListener {
 public:
  PrinterAlertListener(network::INetworkStack& stack, allocators::IAllocator& allocator, uint16_t port = 162) : receiver_(stack, allocator, port) {}
  bool start() { return receiver_.start(); }
  void stop() { receiver_.stop(); }
  void poll() { receiver_.poll(); }
  // The oldest event not yet taken. False if there is none.
  bool next(PrinterAlertEvent* out);
  network::SnmpTrapReceiver& receiver() { return receiver_; }

 private:
  network::SnmpTrapReceiver receiver_;
};

}  // namespace hardware
