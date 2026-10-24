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

#include "Hardware/VideoCodec/SystemVideoCodecs.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnVideoEngine.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaVideoEngine.hpp"
#include "Hardware/VideoCodec/NVIDIA/NvidiaVideoEngine.hpp"

namespace hardware {

namespace {
template <class T, class... Args>
void try_add(List<UniquePointer<IVideoCodecDevice>>& list, allocators::IAllocator& allocator, Args... args) {
  UniquePointer<T> device = UniquePointer<T>::make(allocator, allocator, args...);
  if (!device) return;
  if (!device->initialize()) {
    device->shutdown();
    return;
  }
  list.push_back(UniquePointer<IVideoCodecDevice>(util::move(device)));
}
}  // namespace

// The register window of an Intel GPU, if it is safe to touch: a generation whose media registers are known, memory decoding switched on (the
// firmware did that for its display), and a register BAR the firmware assigned. Empty otherwise.
MmioRegion SystemVideoCodecs::intel_registers(const PciDeviceInfo& d) const {
  constexpr uint16_t kPciCommand = 0x04;
  constexpr uint32_t kMemorySpace = 1u << 1;
  MmioRegion region;
  if (!mmio_ || !IntelMediaEngineHw::supported(intel_gpu_identify(d.device_id).architecture)) return region;
  if (!(pci_.read32(d.address, kPciCommand) & kMemorySpace)) return region;
  const uint64_t base = pci_.memory_bar(d.address, 0);
  if (base == 0 || !mmio_->map_registers(base, IntelMediaEngineHw::kRegisterWindow, &region)) return MmioRegion();
  return region;
}

// What the engine driver needs of a Gen9.5 GPU, found from its PCI function: the first 2 MiB of the register BAR, the global GTT table (halfway into the 16 MiB BAR,
// its size from the GMCH control word), and eight entries of it nothing uses.
struct IntelHardware {
  MmioRegion registers, ggtt_table;
  uint64_t first_page = 0;
};

bool SystemVideoCodecs::intel_hardware_setup(const PciDeviceInfo& d, IntelHardware* out) {
  constexpr uint16_t kPciCommand = 0x04, kGmchControl = 0x50;
  constexpr uint32_t kMemorySpace = 1u << 1;
  constexpr uint64_t kRegisters = 0x200000, kBarSize = 16u << 20, kGuardPages = 16;
  if (!mmio_ || !(pci_.read32(d.address, kPciCommand) & kMemorySpace)) return false;
  const uint64_t base = pci_.memory_bar(d.address, 0);
  intel_gt::GgttGeometry geometry;
  if (base == 0 || !intel_gt::ggtt_geometry(static_cast<uint16_t>(pci_.read32(d.address, kGmchControl)), kBarSize, &geometry)) return false;
  if (!mmio_->map_registers(base, kRegisters, &out->registers) || !mmio_->map_registers(base + geometry.table_offset, geometry.table_bytes, &out->ggtt_table)) return false;
  return intel_gt::find_unused_ggtt_range(out->ggtt_table, intel_gt::VcsDriver::kGgttPagesNeeded, kGuardPages, &out->first_page);
}

void SystemVideoCodecs::initialize() {
  PciDeviceInfo d;
  for (uint32_t i = 0; pci_.pci_device(i, &d); ++i) {
    if (d.class_code != kPciClassDisplay) continue;
    if (d.vendor_id == NvidiaVideoEngine::kPciVendor) try_add<NvidiaVideoEngine>(devices_, allocator_);
    else if (d.vendor_id == AmdVcnVideoEngine::kPciVendor) try_add<AmdVcnVideoEngine>(devices_, allocator_);
    else if (d.vendor_id == IntelMediaVideoEngine::kPciVendor) add_intel(d);
  }
  try_add<SoftwareVideoCodec>(devices_, allocator_);
}

void SystemVideoCodecs::add_intel(const PciDeviceInfo& d) {
  UniquePointer<IntelMediaVideoEngine> device = UniquePointer<IntelMediaVideoEngine>::make(allocator_, allocator_, d.device_id, intel_registers(d));
  if (!device) return;
  IntelHardware hardware;
  if (intel_hardware_ && !intel_registers_bank_ && intel_hardware_setup(d, &hardware)) {
    intel_registers_bank_ = UniquePointer<intel_gt::MmioRegisterBank>::make(allocator_, hardware.registers);
    if (intel_registers_bank_) {
      IntelMediaVideoEngine::HardwareSetup setup;
      setup.registers = intel_registers_bank_.get();
      setup.ggtt_table = hardware.ggtt_table;
      setup.ggtt_first_page = hardware.first_page;
      setup.memory = gpu_memory_override_ ? gpu_memory_override_ : &gpu_memory_;
      device->set_hardware(setup);
    }
  }
  if (!device->initialize()) {
    device->shutdown();
    return;
  }
  devices_.push_back(UniquePointer<IVideoCodecDevice>(util::move(device)));
}

bool SystemVideoCodecs::add_device(UniquePointer<IVideoCodecDevice> device) {
  if (!device) return false;
  if (!device->initialize()) {
    device->shutdown();
    return false;
  }
  return devices_.insert(0, util::move(device));
}

void SystemVideoCodecs::shutdown() {
  for (uint64_t i = devices_.count(); i > 0; --i) devices_[i - 1]->shutdown();
  devices_.clear();
}

uint32_t SystemVideoCodecs::decode_codecs() const {
  uint32_t mask = 0;
  for (const UniquePointer<IVideoCodecDevice>& d : devices_) mask |= d->info().decode_codecs;
  return mask;
}

uint32_t SystemVideoCodecs::encode_codecs() const {
  uint32_t mask = 0;
  for (const UniquePointer<IVideoCodecDevice>& d : devices_) mask |= d->info().encode_codecs;
  return mask;
}

UniquePointer<IVideoDecoder> SystemVideoCodecs::create_decoder(const DecoderConfig& config) {
  for (UniquePointer<IVideoCodecDevice>& d : devices_) {
    if (!d->can_decode(config.codec)) continue;
    UniquePointer<IVideoDecoder> session = d->create_decoder(config);
    if (session) return session;
  }
  return UniquePointer<IVideoDecoder>();
}

UniquePointer<IVideoEncoder> SystemVideoCodecs::create_encoder(const EncoderConfig& config) {
  for (UniquePointer<IVideoCodecDevice>& d : devices_) {
    if (!d->can_encode(config.codec)) continue;
    UniquePointer<IVideoEncoder> session = d->create_encoder(config);
    if (session) return session;
  }
  return UniquePointer<IVideoEncoder>();
}

}  // namespace hardware
