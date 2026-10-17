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
#include "Hardware/Printer/PrinterTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// What a printer says about itself in its IEEE 1284 device ID string, e.g.
//   "MFG:Brother;CMD:PJL,PCL,PCLXL,POSTSCRIPT;MDL:HL-2070N series;CLS:PRINTER;"
// (keys also appear in long form: MANUFACTURER, MODEL, COMMAND SET). USB printers send it on request, parallel printers
// and LPT-over-network ones too.
struct PrinterIdentity {
  char manufacturer[48] = {};
  char model[48] = {};
  uint8_t languages = 0;  // PrinterLanguage bits found in the command set (PCL, POSTSCRIPT / PS)
  bool pjl = false;
  bool pcl_xl = false;
};

// Parses a device ID (without its two-byte length prefix). Unknown keys are ignored; over-long values are cut.
PrinterIdentity parse_device_id(ConstByteView id);

}  // namespace hardware
