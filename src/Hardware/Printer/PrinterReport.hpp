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

// What a printer reports about itself right now: engine state, error conditions, page count, supplies (toner and so on) and
// the active alerts. Network printers fill it from SNMP (the Host Resources and Printer MIBs, RFC 2790 / 3805); a USB printer
// can only give the error flags that its port status byte carries.
enum class PrinterEngineState : uint8_t { Unknown, Idle, Printing, Warmup, Other };

// Bits of PrinterReport::errors (the order of hrPrinterDetectedErrorState).
enum PrinterError : uint16_t {
  kErrorLowPaper = 1 << 0,
  kErrorNoPaper = 1 << 1,
  kErrorLowToner = 1 << 2,
  kErrorNoToner = 1 << 3,
  kErrorDoorOpen = 1 << 4,
  kErrorJammed = 1 << 5,
  kErrorOffline = 1 << 6,
  kErrorServiceRequested = 1 << 7,
  kErrorInputTrayMissing = 1 << 8,
  kErrorOutputTrayMissing = 1 << 9,
  kErrorMarkerSupplyMissing = 1 << 10,
  kErrorOutputNearFull = 1 << 11,
  kErrorOutputFull = 1 << 12,
  kErrorInputTrayEmpty = 1 << 13,
  kErrorOverduePreventMaintenance = 1 << 14,
};

enum class SupplyKind : uint8_t { Other, Toner, Drum, WasteToner, Fuser, Ink, Staples, Developer, Oil, Cleaner, Transfer };

struct SupplyInfo {
  static constexpr int32_t kLevelOther = -1, kLevelUnknown = -2, kLevelSomeRemaining = -3;
  char description[48] = {};
  SupplyKind kind = SupplyKind::Other;
  bool consumed = true;       // a supply that runs out (toner), as opposed to a receptacle that fills up (waste toner)
  int32_t level_percent = kLevelUnknown;  // 0..100 of the capacity, or one of the k constants
  bool empty() const { return level_percent == 0; }
  // Running out: 10 % or less left of a consumable, or 90 % or more of a receptacle full.
  bool low() const { return level_percent >= 0 && (consumed ? level_percent <= 10 : level_percent >= 90); }
};

// prtAlertSeverityLevel
enum class AlertSeverity : uint8_t { Other = 1, Critical = 3, Warning = 4, WarningBinary = 5 };

struct AlertInfo {
  AlertSeverity severity = AlertSeverity::Other;
  uint32_t code = 0;          // prtAlertCode (RFC 3805): 1 other, 3 cover open, 8 jam, 12 almost empty, 13 empty, ...
  char description[64] = {};  // the printer's own words
};
// A short name for an alert code ("jam", "subunit empty", "door open"...), or "alert" for one that is not listed.
const char* alert_code_name(uint32_t code);

struct PrinterReport {
  static constexpr uint32_t kMaxSupplies = 8, kMaxAlerts = 8;
  bool valid = false;               // filled in by a completed query
  PrinterEngineState engine = PrinterEngineState::Unknown;
  uint16_t errors = 0;              // PrinterError bits
  bool has_page_count = false;
  uint32_t page_count = 0;          // pages printed in the printer's life
  char description[80] = {};        // sysDescr / device description
  char serial[32] = {};
  char display[64] = {};            // the text on the printer's own display ("READY", "TONER LOW")
  SupplyInfo supplies[kMaxSupplies];
  uint32_t supply_count = 0;
  AlertInfo alerts[kMaxAlerts];
  uint32_t alert_count = 0;

  bool has_error(PrinterError e) const { return (errors & e) != 0; }
  // The first supply of a kind, or nullptr.
  const SupplyInfo* supply(SupplyKind kind) const {
    for (uint32_t i = 0; i < supply_count; ++i)
      if (supplies[i].kind == kind) return &supplies[i];
    return nullptr;
  }
  bool toner_low() const { return has_error(kErrorLowToner) || has_error(kErrorNoToner) || (supply(SupplyKind::Toner) && supply(SupplyKind::Toner)->low()); }
};

}  // namespace hardware
