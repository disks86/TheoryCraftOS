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
#include "Utility/Bounds.hpp"

namespace hardware {

// A window onto memory-mapped device registers (MMIO): where it is and how big, and the only way to touch it. Every access has the exact width asked for
// (a volatile access of 8, 16, 32 or 64 bits, never split or merged by the compiler), must be naturally aligned, and is checked against the window: an
// offset outside it stops the OS (bounds_violation) instead of reaching into another device. The memory behind has to be mapped uncached (MmioMapper does
// that); x86 keeps such accesses in program order, and the compiler is told not to move other memory accesses across them.
class MmioRegion {
 public:
  constexpr MmioRegion() = default;
  constexpr MmioRegion(uint64_t address, uint64_t length) : address_(address), length_(length) {}

  bool valid() const { return length_ != 0; }
  uint64_t address() const { return address_; }
  uint64_t length() const { return length_; }
  // The address of the register at `offset`, for handing to code that takes a raw address (the register must lie in the window).
  uint64_t address_of(uint64_t offset) const { check(offset, 1); return address_ + offset; }

  uint8_t read8(uint64_t offset) const { check(offset, 1); const uint8_t v = *ptr<uint8_t>(offset); fence(); return v; }
  uint16_t read16(uint64_t offset) const { check(offset, 2); const uint16_t v = *ptr<uint16_t>(offset); fence(); return v; }
  uint32_t read32(uint64_t offset) const { check(offset, 4); const uint32_t v = *ptr<uint32_t>(offset); fence(); return v; }
  uint64_t read64(uint64_t offset) const { check(offset, 8); const uint64_t v = *ptr<uint64_t>(offset); fence(); return v; }
  void write8(uint64_t offset, uint8_t value) const { check(offset, 1); fence(); *ptr<uint8_t>(offset) = value; fence(); }
  void write16(uint64_t offset, uint16_t value) const { check(offset, 2); fence(); *ptr<uint16_t>(offset) = value; fence(); }
  void write32(uint64_t offset, uint32_t value) const { check(offset, 4); fence(); *ptr<uint32_t>(offset) = value; fence(); }
  void write64(uint64_t offset, uint64_t value) const { check(offset, 8); fence(); *ptr<uint64_t>(offset) = value; fence(); }

  // A 64-bit register that the device wants as two 32-bit accesses, low half first (xHCI, AHCI, NVMe before the controller is enabled...).
  uint64_t read64_split(uint64_t offset) const { const uint64_t low = read32(offset); return low | (static_cast<uint64_t>(read32(offset + 4)) << 32); }
  void write64_split(uint64_t offset, uint64_t value) const {
    write32(offset, static_cast<uint32_t>(value));
    write32(offset + 4, static_cast<uint32_t>(value >> 32));
  }

  // Read-modify-write of a 32-bit register: the bits in `mask` take the corresponding bits of `value`.
  void update32(uint64_t offset, uint32_t mask, uint32_t value) const { write32(offset, (read32(offset) & ~mask) | (value & mask)); }
  void set_bits32(uint64_t offset, uint32_t bits) const { update32(offset, bits, bits); }
  void clear_bits32(uint64_t offset, uint32_t bits) const { update32(offset, bits, 0); }
  // Reads the register until (value & mask) == expected, at most `attempts` times. True if it happened. (The caller decides how long a try is: the
  // read itself takes about a microsecond on a PCI Express device.)
  bool wait32(uint64_t offset, uint32_t mask, uint32_t expected, uint32_t attempts) const {
    for (uint32_t i = 0; i < attempts; ++i)
      if ((read32(offset) & mask) == expected) return true;
    return false;
  }

  // The part of the window from `offset`, `length` bytes long (must lie inside).
  MmioRegion subregion(uint64_t offset, uint64_t length) const {
    if (length == 0 || offset > length_ || length > length_ - offset) bounds_violation("MmioRegion::subregion", offset + length, length_);
    return MmioRegion(address_ + offset, length);
  }

 private:
  void check(uint64_t offset, uint64_t size) const {
    if (offset > length_ || size > length_ - offset || ((address_ + offset) & (size - 1)) != 0) bounds_violation("MmioRegion", offset, length_);
  }
  template <class T>
  volatile T* ptr(uint64_t offset) const { return reinterpret_cast<volatile T*>(address_ + offset); }
  static void fence() { __asm__ volatile("" ::: "memory"); }

  uint64_t address_ = 0;
  uint64_t length_ = 0;
};

}  // namespace hardware
