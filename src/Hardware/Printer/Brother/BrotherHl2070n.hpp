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
#include "Hardware/Printer/NetworkPrinter.hpp"

namespace hardware {

// The Brother HL-2070N: a monochrome laser printer with an Ethernet print server (23 pages per minute, A4 / Letter / Legal /
// Executive / A5 / B5 / A6, 600 dpi with HQ1200 at 1200 dpi, no duplex). It reads PCL 6 (which includes the PCL 5e used by PrintJob)
// and BR-Script 3 (PostScript), with PJL job control, on raw port 9100 or the LPD queue "BINARY_P1". The input tray is "Tray1" and
// the single-sheet slot "Manual". Its web page, SNMP, IPP and BRAdmin discovery are not used here: the address is given.
class BrotherHl2070n final : public NetworkPrinter {
 public:
  static const PrinterInfo& model_info();
  BrotherHl2070n(network::INetworkStack& stack, network::Ipv4Address address, allocators::IAllocator& allocator, PrintProtocol protocol = PrintProtocol::Raw9100)
      : NetworkPrinter(stack, address, model_info(), allocator, protocol) {}

  // Sensible settings for this printer: A4 at 600 dpi in PCL.
  static JobSettings default_settings() { return JobSettings{}; }
};

}  // namespace hardware
