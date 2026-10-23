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
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/VideoCodec/IVideoCodecDevice.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Finds every video codec device the machine has: one per GPU that carries a known video engine (NVIDIA,
// AMD, Intel), and the software codec, which is always last and always there. Sessions are asked for with
// create_decoder() / create_encoder(), which try the devices in order and use the first that serves the codec
// (a device that really accelerates it would come first; today all of them run on the CPU).
// Gets a device's registers mapped (MmioMapper does it on the real machine; a test hands out plain memory).
class IRegisterMapper {
 public:
  virtual bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) = 0;

 protected:
  constexpr IRegisterMapper() = default;
  ~IRegisterMapper() = default;
};

class SystemVideoCodecs {
 public:
  SystemVideoCodecs(const IPciBus& pci, allocators::IAllocator& allocator) : pci_(pci), allocator_(allocator), devices_(allocator) {}

  // With a mapper, initialize() maps the register BAR of Intel GPUs and looks at their video engines; without one it only names them.
  void set_mmio(IRegisterMapper* mapper) { mmio_ = mapper; }

  void initialize();
  void shutdown();

  // Adds a device (a driver loaded later, or one brought up by a test): it is initialised here and asked before the
  // devices already present. False if it fails to initialise or memory ran out.
  bool add_device(UniquePointer<IVideoCodecDevice> device);

  uint32_t count() const { return static_cast<uint32_t>(devices_.count()); }
  IVideoCodecDevice* at(uint32_t index) const { return index < devices_.count() ? devices_[index].get() : nullptr; }
  // Codecs some device decodes / encodes.
  uint32_t decode_codecs() const;
  uint32_t encode_codecs() const;

  UniquePointer<IVideoDecoder> create_decoder(const DecoderConfig& config);
  UniquePointer<IVideoEncoder> create_encoder(const EncoderConfig& config);

 private:
  MmioRegion intel_registers(const PciDeviceInfo& d) const;

  const IPciBus& pci_;
  allocators::IAllocator& allocator_;
  IRegisterMapper* mmio_ = nullptr;
  List<UniquePointer<IVideoCodecDevice>> devices_;
};

}  // namespace hardware
