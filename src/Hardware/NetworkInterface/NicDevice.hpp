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

// What the drivers of PCI network controllers (Realtek RTL8111/8168/8125, Intel e1000e, igb and igc) need from the machine: the controller's registers, its PCI
// configuration space, and the controller to themselves while they run. PciNicDevice is the real one; a host test hands in a model of the chip instead, so the
// drivers are tested without hardware.
#pragma once
#include <stdint.h>
#include "Hardware/Mmio.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// A network controller's register window. Accesses outside it are refused by the implementation (MmioRegion stops the OS).
class INicRegisters {
 public:
  virtual uint8_t read8(uint32_t offset) = 0;
  virtual uint16_t read16(uint32_t offset) = 0;
  virtual uint32_t read32(uint32_t offset) = 0;
  virtual void write8(uint32_t offset, uint8_t value) = 0;
  virtual void write16(uint32_t offset, uint16_t value) = 0;
  virtual void write32(uint32_t offset, uint32_t value) = 0;

  void set_bits32(uint32_t offset, uint32_t bits) { write32(offset, read32(offset) | bits); }
  void clear_bits32(uint32_t offset, uint32_t bits) { write32(offset, read32(offset) & ~bits); }

 protected:
  constexpr INicRegisters() = default;
  ~INicRegisters() = default;
};

class MmioNicRegisters final : public INicRegisters {
 public:
  MmioNicRegisters() = default;
  explicit MmioNicRegisters(const MmioRegion& region) : region_(region) {}
  bool valid() const { return region_.valid(); }

  uint8_t read8(uint32_t offset) override { return region_.read8(offset); }
  uint16_t read16(uint32_t offset) override { return region_.read16(offset); }
  uint32_t read32(uint32_t offset) override { return region_.read32(offset); }
  void write8(uint32_t offset, uint8_t value) override { region_.write8(offset, value); }
  void write16(uint32_t offset, uint16_t value) override { region_.write16(offset, value); }
  void write32(uint32_t offset, uint32_t value) override { region_.write32(offset, value); }

 private:
  MmioRegion region_;
};

class INicDevice {
 public:
  // Takes the controller over: the firmware's driver lets go of it, its registers are mapped, memory decoding and bus mastering are switched on. False if the
  // registers cannot be reached; release() is still called then.
  virtual bool claim() = 0;
  // Gives the controller back to the firmware's driver. The driver has reset it first, so it no longer reads or writes memory.
  virtual void release() = 0;
  // Valid between a successful claim() and release().
  virtual INicRegisters& registers() = 0;
  virtual uint16_t vendor_id() const = 0;
  virtual uint16_t device_id() const = 0;
  // PCI configuration space; all ones where there is none.
  virtual uint32_t config_read32(uint16_t offset) = 0;
  virtual void config_write32(uint16_t offset, uint32_t value) = 0;

  // The controller's interrupt (MSI) delivered to `handler(context)`, in interrupt context, on some core. False if the controller or the
  // machine cannot (no MSI capability, no vector left, the scheduler not running). detach_interrupt() undoes it; safe when not attached.
  using InterruptHandler = void (*)(void* context);
  virtual bool attach_interrupt(InterruptHandler handler, void* context) {
    (void)handler;
    (void)context;
    return false;
  }
  virtual void detach_interrupt() {}

 protected:
  constexpr INicDevice() = default;
  ~INicDevice() = default;
};

// The address a controller uses for memory from allocate_dma_view(): the kernel's DMA pool is identity-mapped in the first 4 GiB, so the CPU's address of the
// memory is also the bus address (the same convention as every other driver here). A test allocator hands out memory below 2 GiB for the same reason.
template <class T>
inline uint64_t nic_dma_address(ArrayView<T> view) {
  return reinterpret_cast<uint64_t>(view.data());
}

}  // namespace hardware
