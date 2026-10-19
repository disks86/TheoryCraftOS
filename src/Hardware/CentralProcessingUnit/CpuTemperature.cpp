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

#include "Hardware/CentralProcessingUnit/CpuTemperature.hpp"
#include "Hardware/CentralProcessingUnit/Cpuid.hpp"

namespace hardware {

namespace {
constexpr uint32_t kMsrThermStatus = 0x19C, kMsrTemperatureTarget = 0x1A2, kMsrPackageThermStatus = 0x1B1;
constexpr uint32_t kSmnIndex = 0x60, kSmnData = 0x64, kSmnThermalTctl = 0x59800;

uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return (uint64_t{high} << 32) | low;
}

bool vendor_is(const CpuidResult& r, uint32_t ebx, uint32_t edx, uint32_t ecx) { return r.ebx == ebx && r.edx == edx && r.ecx == ecx; }

int32_t intel_temperature() {
  const CpuidResult thermal = cpuid(6);
  if (!(thermal.eax & 1)) return -1;                                  // no digital thermal sensor
  const bool package = (thermal.eax >> 6) & 1;                        // package thermal management
  const uint64_t status = rdmsr(package ? kMsrPackageThermStatus : kMsrThermStatus);
  if (!(status & (1ull << 31))) return -1;                            // reading not valid
  uint32_t tj_max = static_cast<uint32_t>((rdmsr(kMsrTemperatureTarget) >> 16) & 0xFF);
  if (tj_max < 60 || tj_max > 125) tj_max = 100;
  const uint32_t below = static_cast<uint32_t>((status >> 16) & 0x7F);  // degrees under TjMax
  return below > tj_max ? 0 : static_cast<int32_t>((tj_max - below) * 1000);
}

int32_t amd_temperature(const IPciBus* pci) {
  if (!pci) return -1;
  const PciAddress host_bridge{0, 0, 0};
  if ((pci->read32(host_bridge, 0) & 0xFFFF) != 0x1022 && (pci->read32(host_bridge, 0) & 0xFFFF) != 0x1D94) return -1;   // AMD / Hygon
  pci->write32(host_bridge, kSmnIndex, kSmnThermalTctl);
  const uint32_t value = pci->read32(host_bridge, kSmnData);
  if (value == 0xFFFFFFFFu) return -1;
  int32_t millis = static_cast<int32_t>((value >> 21) & 0x7FF) * 125;   // eighths of a degree
  if (value & (1u << 19)) millis -= 49000;                              // the -49..206 range
  return millis;
}
}  // namespace

int32_t cpu_temperature_millicelsius(const IPciBus* pci) {
  if (cpuid(1).ecx & (1u << 31)) return -1;   // a virtual machine: the sensor MSRs are not there to read
  const CpuidResult vendor = cpuid(0);
  if (vendor_is(vendor, 0x756E6547, 0x49656E69, 0x6C65746E)) return intel_temperature();   // "GenuineIntel"
  const bool amd = vendor_is(vendor, 0x68747541, 0x69746E65, 0x444D4163);                  // "AuthenticAMD"
  const bool hygon = vendor_is(vendor, 0x6F677948, 0x6E65476E, 0x656E6975);                // "HygonGenuine"
  if ((amd || hygon) && cpu_signature().family >= 0x17) return amd_temperature(pci);
  return -1;
}

}  // namespace hardware
