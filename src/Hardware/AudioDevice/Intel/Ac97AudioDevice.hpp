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
#include "Allocators/IAllocator.hpp"
#include "Hardware/AudioDevice/AudioDeviceBase.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "uefi.hpp"

namespace hardware {

// The Intel ICH AC'97 audio controller (QEMU's `AC97`) with its codec. The codec
// mixer sits in one I/O range (BAR 0), the bus-master DMA engines in another
// (BAR 1). Playback uses the PCM-out engine over a ring of 32 buffer descriptors
// that is kept permanently valid, so the engine never stops. Polled; 32-bit DMA,
// so the buffers must be below 4 GiB.
class Ac97AudioDevice final : public RingAudioDevice {
 public:
  static constexpr uint32_t kEntries = 32;
  static constexpr uint32_t kEntryBytes = 2048;
  static constexpr uint32_t kRingBytes = kEntries * kEntryBytes;

  Ac97AudioDevice(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                  efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator), st_(st) {}

  static bool supports(uint16_t vendor_id, uint16_t device_id) {
    return vendor_id == 0x8086 &&
           (device_id == 0x2415 || device_id == 0x2425 || device_id == 0x2445 || device_id == 0x266E ||
            device_id == 0x27DE);
  }

  const char* name() const override { return "Intel AC'97 audio"; }
  bool initialize() override;
  void shutdown() override;

 private:
  struct BufferDescriptor {
    uint32_t address;
    uint16_t samples;  // 16-bit samples (a stereo frame is two)
    uint16_t flags;
  };

  uint16_t mixer_read(uint16_t reg) const;
  void mixer_write(uint16_t reg, uint16_t value) const;
  uint8_t bus_read8(uint16_t reg) const;
  uint16_t bus_read16(uint16_t reg) const;
  uint32_t bus_read32(uint16_t reg) const;
  void bus_write8(uint16_t reg, uint8_t value) const;
  void bus_write16(uint16_t reg, uint16_t value) const;
  void bus_write32(uint16_t reg, uint32_t value) const;

  void apply_volume() override;
  void refresh() override;
  uint32_t hardware_position();

  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;

  uint16_t mixer_ = 0;  // I/O base of the codec mixer registers
  uint16_t bus_ = 0;    // I/O base of the bus-master registers
  ArrayView<BufferDescriptor> descriptors_;
  bool owns_device_ = false;
};

}  // namespace hardware
