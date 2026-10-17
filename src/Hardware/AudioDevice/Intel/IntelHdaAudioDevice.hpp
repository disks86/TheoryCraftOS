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

// An Intel High Definition Audio controller (QEMU's `intel-hda`, and the audio
// of virtually every PC chipset since 2004) with the first codec that answers.
// The driver talks to the codec through the CORB/RIRB command rings, finds an
// output pin (line out, speaker or headphones) and the DAC behind it, unmutes the
// path and plays through output stream 0 from one cyclic buffer described by a
// buffer descriptor list. Polled: no interrupts. Any vendor's controller that
// follows the specification (PCI class 0x0403) is used.
class IntelHdaAudioDevice final : public RingAudioDevice {
 public:
  static constexpr uint32_t kEntries = 8;
  static constexpr uint32_t kEntryBytes = 8192;
  static constexpr uint32_t kRingBytes = kEntries * kEntryBytes;
  static constexpr uint8_t kStreamTag = 1;

  IntelHdaAudioDevice(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                      efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator), st_(st) {}

  const char* name() const override { return "Intel High Definition Audio"; }
  bool initialize() override;
  void shutdown() override;

 private:
  struct BufferDescriptor {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
  };
  // One node on the way from the output pin to the converter.
  struct Hop {
    uint8_t nid;
    uint8_t select;       // index of the connection that leads on towards the converter
    uint8_t connections;  // how many connections the node has
  };
  struct Amplifier {
    bool present = false;
    bool can_mute = false;
    uint8_t steps = 0;    // gain values run 0 .. steps
    uint8_t offset = 0;   // the gain value for 0 dB
  };
  static constexpr uint32_t kMaxPath = 5;

  uint8_t read8(uint32_t reg) const;
  uint16_t read16(uint32_t reg) const;
  uint32_t read32(uint32_t reg) const;
  void write8(uint32_t reg, uint8_t value) const;
  void write16(uint32_t reg, uint16_t value) const;
  void write32(uint32_t reg, uint32_t value) const;
  bool wait_register(uint32_t reg, uint32_t mask, uint32_t value, uint32_t timeout_us) const;

  bool reset_controller();
  bool start_command_rings();
  bool verb(uint8_t nid, uint32_t command, uint32_t* response);
  bool get_parameter(uint8_t nid, uint8_t parameter, uint32_t* response);
  bool find_output_path();
  bool search_converter(uint8_t nid, uint32_t depth);
  uint32_t connections(uint8_t nid, uint8_t* out, uint32_t capacity);
  Amplifier amplifier(uint8_t nid, uint8_t parameter);
  bool configure_path();
  bool start_stream();
  bool dma_reachable(uint64_t address, uint64_t bytes) const;

  void apply_volume() override;
  uint32_t hardware_position() override;

  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;

  uint64_t registers_ = 0;
  uint32_t stream_ = 0;  // register offset of output stream 0's descriptor
  bool dma64_ = false;
  uint8_t codec_ = 0;

  ByteView corb_, rirb_;
  uint32_t corb_entries_ = 0, rirb_entries_ = 0;
  uint32_t corb_write_ = 0, rirb_read_ = 0;
  ArrayView<BufferDescriptor> descriptors_;

  uint8_t function_group_ = 0;
  uint32_t widget_caps_[256] = {};  // audio widget capabilities by node id (0 = no such widget)
  Hop path_[kMaxPath] = {};
  uint32_t path_length_ = 0;
  uint32_t pin_control_ = 0x40;
  bool pin_has_eapd_ = false;
  Amplifier volume_amp_;
  uint8_t volume_nid_ = 0;
  bool owns_device_ = false;
};

}  // namespace hardware
