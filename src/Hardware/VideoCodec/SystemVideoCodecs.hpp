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
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRegisterBank.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/VideoCodec/IVideoCodecDevice.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Tells how big a memory BAR is (the GPU's video memory aperture, for the AMD probe). Sizing a BAR briefly switches the device's decoding off;
// the OS has a routine for it (pci_read_bar), and a test hands out a number.
class IBarSizer {
 public:
  virtual bool bar_size(PciAddress address, uint8_t index, uint64_t* size) = 0;

 protected:
  constexpr IBarSizer() = default;
  ~IBarSizer() = default;
};

// Finds every video codec device the machine has: one per GPU that carries a known video engine (NVIDIA,
// AMD, Intel), and the software codec, which is always last and always there. Sessions are asked for with
// create_decoder() / create_encoder(), which try the devices in order and use the first that serves the codec
// (a device that really accelerates it would come first; today all of them run on the CPU).
class SystemVideoCodecs {
 public:
  SystemVideoCodecs(const IPciBus& pci, allocators::IAllocator& allocator) : pci_(pci), allocator_(allocator), devices_(allocator) {}

  // With a mapper, initialize() maps the register BAR of Intel GPUs and looks at their video engines, and reads the chip ID of NVIDIA GPUs; without one it only names them.
  void set_mmio(IRegisterMapper* mapper) { mmio_ = mapper; }
  // With a sizer too, initialize() reads the IP discovery table at the end of the video memory of AMD GPUs (RDNA 1 and newer) and reports what the video engines are.
  void set_bar_sizer(IBarSizer* sizer) { bar_sizer_ = sizer; }
  // H.264 decoding on the video engine of Gen9.5 Intel GPUs (needs set_mmio, so it is only tried where the registers could be mapped). On by default; pass false
  // to keep every session on the CPU. That path has only been run against a simulated GPU, so a real GPU that does not answer as expected leaves the sessions on the CPU.
  void set_intel_hardware_decode(bool enable) { intel_hardware_ = enable; }
  // The UVD 6 video engine of the Radeon RX Vega M (Kaby Lake-G): with this on, the card gets an AmdUvd6VideoEngine that can be planned and started; off (the default),
  // it is only named like any other Radeon. Opt-in because the start has never run on a real card (it does not power the engine itself).
  void set_amd_uvd6(bool enable) { amd_uvd6_ = enable; }
  // The Vega M's H.264 encoder (VCE 3.4) along with its decoder; only with set_amd_uvd6(). Opt-in: it has not run on a real card.
  void set_amd_vce3(bool enable) { amd_vce3_ = enable; }
  // The memory the engine driver takes buffers from; by default the OS allocator's DMA pool. (For tests, which have no memory at device addresses.)
  void set_intel_gpu_memory(intel_gt::IGpuMemory* memory) { gpu_memory_override_ = memory; }

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
  MmioRegion nvidia_registers(const PciDeviceInfo& d) const;
  MmioRegion amd_discovery_window(const PciDeviceInfo& d, uint64_t* bar_size) const;
  MmioRegion amd_registers(const PciDeviceInfo& d, uint64_t length) const;
  void add_amd(const PciDeviceInfo& d);
  bool intel_hardware_setup(const PciDeviceInfo& d, struct IntelHardware* out);
  void add_intel(const PciDeviceInfo& d);
  void add_amd_uvd6(const PciDeviceInfo& d);

  const IPciBus& pci_;
  allocators::IAllocator& allocator_;
  IRegisterMapper* mmio_ = nullptr;
  IBarSizer* bar_sizer_ = nullptr;
  bool intel_hardware_ = true;
  bool amd_uvd6_ = false;
  bool amd_vce3_ = false;
  intel_gt::AllocatorGpuMemory gpu_memory_{allocator_};
  intel_gt::IGpuMemory* gpu_memory_override_ = nullptr;
  UniquePointer<intel_gt::MmioRegisterBank> intel_registers_bank_;  // lives as long as the device that uses it
  List<UniquePointer<IVideoCodecDevice>> devices_;
};

}  // namespace hardware
