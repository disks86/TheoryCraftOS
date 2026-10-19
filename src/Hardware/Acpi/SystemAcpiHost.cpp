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

#include "Hardware/Acpi/SystemAcpiHost.hpp"
#include "Hardware/IoPort.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

uint8_t SystemAcpiHost::Ports::in8(uint16_t port) { return IoPort(port).read8(); }
void SystemAcpiHost::Ports::out8(uint16_t port, uint8_t value) { IoPort(port).write8(value); }
void SystemAcpiHost::Ports::delay_us(uint32_t us) { hardware::delay_us(us); }

void SystemAcpiHost::stall_us(uint32_t us) { hardware::delay_us(us); }
void SystemAcpiHost::sleep_ms(uint32_t ms) { hardware::delay_us(uint64_t{ms} * 1000); }
uint64_t SystemAcpiHost::timer_100ns() { return hardware::uptime_us() * 10; }

bool SystemAcpiHost::memory(uint64_t address, uint32_t bits, uint64_t* value, bool write) {
  const uint32_t bytes = bits / 8;
  if (!mmio_ || address + bytes < address) return false;
  if (!mmio_->is_mapped(address, bytes)) {
    MmioRegion window;
    if (!mmio_->ensure_mapped(address, bytes, &window)) return false;
  }
  volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(address);
  switch (bits) {
    case 8: if (write) *p = static_cast<uint8_t>(*value); else *value = *p; return true;
    case 16: if (write) *reinterpret_cast<volatile uint16_t*>(p) = static_cast<uint16_t>(*value); else *value = *reinterpret_cast<volatile uint16_t*>(p); return true;
    case 32: if (write) *reinterpret_cast<volatile uint32_t*>(p) = static_cast<uint32_t>(*value); else *value = *reinterpret_cast<volatile uint32_t*>(p); return true;
    case 64: if (write) *reinterpret_cast<volatile uint64_t*>(p) = *value; else *value = *reinterpret_cast<volatile uint64_t*>(p); return true;
    default: return false;
  }
}

bool SystemAcpiHost::region_read(const acpi::RegionAccess& a, uint64_t* value) {
  switch (a.space) {
    case acpi::RegionSpace::EmbeddedControl: {
      uint8_t v = 0;
      if (a.width_bits != 8 || a.address > 0xFF || !ec_.read(static_cast<uint8_t>(a.address), &v)) return false;
      *value = v;
      return true;
    }
    case acpi::RegionSpace::SystemIo: {
      if (a.address > 0xFFFF) return false;
      const IoPort port(static_cast<uint16_t>(a.address));
      *value = a.width_bits == 8 ? port.read8() : a.width_bits == 16 ? port.read16() : port.read32();
      return a.width_bits == 8 || a.width_bits == 16 || a.width_bits == 32;
    }
    case acpi::RegionSpace::SystemMemory: return memory(a.address, a.width_bits, value, false);
    case acpi::RegionSpace::PciConfig: {
      if (!pci_ || a.address > 0xFF) return false;
      const uint32_t word = pci_->read32(PciAddress{a.pci_bus, a.pci_device, a.pci_function}, static_cast<uint16_t>(a.address & ~3ull));
      const uint32_t shift = static_cast<uint32_t>(a.address & 3) * 8;
      *value = a.width_bits == 32 ? word : (word >> shift) & (a.width_bits == 16 ? 0xFFFFu : 0xFFu);
      return a.width_bits == 8 || a.width_bits == 16 || a.width_bits == 32;
    }
    default: return false;
  }
}

bool SystemAcpiHost::region_write(const acpi::RegionAccess& a, uint64_t value) {
  switch (a.space) {
    case acpi::RegionSpace::EmbeddedControl: return a.width_bits == 8 && a.address <= 0xFF && ec_.write(static_cast<uint8_t>(a.address), static_cast<uint8_t>(value));
    case acpi::RegionSpace::SystemIo: {
      if (a.address > 0xFFFF) return false;
      const IoPort port(static_cast<uint16_t>(a.address));
      if (a.width_bits == 8) port.write8(static_cast<uint8_t>(value));
      else if (a.width_bits == 16) port.write16(static_cast<uint16_t>(value));
      else if (a.width_bits == 32) port.write32(static_cast<uint32_t>(value));
      else return false;
      return true;
    }
    case acpi::RegionSpace::SystemMemory: return memory(a.address, a.width_bits, &value, true);
    case acpi::RegionSpace::PciConfig: {
      if (!pci_ || a.address > 0xFF || (a.width_bits != 8 && a.width_bits != 16 && a.width_bits != 32)) return false;
      const PciAddress at{a.pci_bus, a.pci_device, a.pci_function};
      const uint16_t offset = static_cast<uint16_t>(a.address & ~3ull);
      if (a.width_bits == 32) {
        pci_->write32(at, offset, static_cast<uint32_t>(value));
      } else {
        const uint32_t shift = static_cast<uint32_t>(a.address & 3) * 8;
        const uint32_t mask = (a.width_bits == 16 ? 0xFFFFu : 0xFFu) << shift;
        pci_->write32(at, offset, (pci_->read32(at, offset) & ~mask) | ((static_cast<uint32_t>(value) << shift) & mask));
      }
      return true;
    }
    default: return false;
  }
}

}  // namespace hardware
