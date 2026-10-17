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
#include "Network/Snmp/SnmpClient.hpp"

namespace hardware {

// Asks a network printer's SNMP agent how it is (RFC 3805 Printer MIB and RFC 2790 Host Resources MIB, which every network laser
// printer implements - the Brother HL-2070N's print server among them) and builds a PrinterReport:
//   - sysDescr, serial number, the engine state (hrPrinterStatus) and error bits (hrPrinterDetectedErrorState: low and no paper,
//     low and no toner, door open, jam, off line, service requested...),
//   - the page counter (prtMarkerLifeCount),
//   - every supply (prtMarkerSupplies: description, type, class, level against capacity),
//   - the active alerts (prtAlertTable: severity, code, the printer's description),
//   - the text on the printer's display (prtConsoleDisplayBuffer).
// One request is in flight at a time and a refresh takes about two requests per supply and per alert, so poll() it while the
// network stack runs and read report() when busy() is false. Device index 1 is assumed (a print server with several printers behind
// it is not distinguished). The community string is usually "public".
class PrinterMonitor {
 public:
  PrinterMonitor(network::INetworkStack& stack, allocators::IAllocator& allocator, network::Ipv4Address printer, const char* community = "public")
      : client_(stack, allocator, printer, community) {}

  // Begins a refresh. False if one is already running.
  bool start();
  void poll();
  bool busy() const { return step_ != Step::Idle && step_ != Step::Done && step_ != Step::Failed; }
  // True once the last refresh completed (report() is then current); false after a timeout or before the first.
  bool succeeded() const { return step_ == Step::Done; }
  bool failed() const { return step_ == Step::Failed; }
  const PrinterReport& report() const { return report_; }
  network::SnmpClient& client() { return client_; }

 private:
  enum class Step : uint8_t { Idle, Basics, SupplyWalk, SupplyDetail, AlertWalk, AlertDetail, Display, Done, Failed };
  void issue(Step step);
  void handle();
  void finish_supply();

  network::SnmpClient client_;
  PrinterReport report_;
  Step step_ = Step::Idle;
  network::Oid cursor_;       // where the current walk stands
  network::Oid suffix_;       // the row (device index, entry index) being detailed
  SupplyInfo pending_supply_;
  AlertInfo pending_alert_;
  int64_t level_ = -2, capacity_ = -2, type_ = 2, klass_ = 3;
};

}  // namespace hardware
