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
#include "Hardware/Printer/IPrinter.hpp"
#include "Hardware/Printer/PrinterDiscovery.hpp"

namespace hardware {

// Makes a printer object for a discovered printer: IPP if it advertises it, else raw port 9100 (advertised, or assumed for a printer
// only SNMP found), else LPD. The PrinterInfo is built from what discovery learned: languages from the announced "pdl" list (PCL
// assumed when nothing was announced), 600 dpi, mono, A4 and Letter - the details discovery does not know are defaults. Null if no
// protocol fits or memory ran out.
UniquePointer<IPrinter> make_discovered_printer(network::INetworkStack& stack, allocators::IAllocator& allocator, const DiscoveredPrinter& found);

}  // namespace hardware
