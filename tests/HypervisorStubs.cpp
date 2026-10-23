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


// The host tests of the input drivers run on an ordinary machine: no VMware backdoor answers (probe() says so without touching the port), and there is no
// VirtualBox device. These stand in for Hardware/VMware/Backdoor.cpp and Hardware/VirtualBox/VmmDev.cpp.
#include "Hardware/VMware/Backdoor.hpp"
#include "Hardware/VirtualBox/VmmDev.hpp"

namespace hardware {

void Backdoor::call(BackdoorRegisters& r) { r.eax = 0xFFFFFFFFu; }
bool Backdoor::probe() { return false; }
uint64_t Backdoor::cpu_hz() { return 0; }

bool VmmDev::initialize() { return false; }
void VmmDev::shutdown() {}
void VmmDev::service() {}
bool VmmDev::take_pointer(uint32_t*, uint32_t*) { return false; }
bool VmmDev::take_display_request(uint32_t*, uint32_t*) { return false; }
bool VmmDev::host_time_ms(uint64_t*) { return false; }

}  // namespace hardware
