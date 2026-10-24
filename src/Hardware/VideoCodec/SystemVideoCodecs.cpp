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
  if (base == 0 || !mmio_->map_registers(base, IntelMediaEngineHw::register_window(intel_gpu_identify(d.device_id).architecture), &region)) return MmioRegion();
  return region;
}

// The first page of an NVIDIA GPU's register BAR, where the chip ID registers are, if it is safe to touch: a mapper, memory decoding switched on (the
// firmware did that for its display) and a register BAR the firmware assigned. Empty otherwise.
MmioRegion SystemVideoCodecs::nvidia_registers(const PciDeviceInfo& d) const {
  constexpr uint16_t kPciCommand = 0x04;
  constexpr uint32_t kMemorySpace = 1u << 1;
  MmioRegion region;
  if (!mmio_ || !(pci_.read32(d.address, kPciCommand) & kMemorySpace)) return region;
  const uint64_t base = pci_.memory_bar(d.address, 0);
  if (base == 0 || !mmio_->map_registers(base, NvidiaMediaEngineHw::kRegisterWindow, &region)) return MmioRegion();
  return region;
}

// Where the IP discovery table of an AMD GPU can be looked for, if it can be reached (the last 64 KiB of a card's video memory; an APU's whole memory BAR, up to a limit): a generation that has one, a mapper and a
// sizer, memory decoding switched on, and a memory BAR that covers the whole of video memory (a smaller window - no resizable BAR - cannot show its end,
// and then the probe finds no table). Empty otherwise.
MmioRegion SystemVideoCodecs::amd_discovery_window(const PciDeviceInfo& d, uint64_t* bar_size) const {
  constexpr uint16_t kPciCommand = 0x04;
  constexpr uint32_t kMemorySpace = 1u << 1;
  MmioRegion region;
  if (!mmio_ || !bar_sizer_ || !AmdMediaEngineHw::supported(amd_gpu_identify(d.device_id).architecture)) return region;
  if (!(pci_.read32(d.address, kPciCommand) & kMemorySpace)) return region;
  const uint64_t base = pci_.memory_bar(d.address, 0);
  uint64_t size = 0;
  if (base == 0 || !bar_sizer_->bar_size(d.address, 0, &size) || size < AmdMediaEngineHw::kDiscoveryWindow || size > UINT64_MAX - base) return region;
  *bar_size = size;
  if (!amd_gpu_identify(d.device_id).discrete) {
    // A Radeon APU: its video memory is a carve-out of system RAM whose end is not the end of the BAR, so the scan gets the BAR itself (up to the limit).
    const uint64_t mapped = size < AmdMediaEngineHw::kApertureScanLimit ? size : AmdMediaEngineHw::kApertureScanLimit;
    if (!mmio_->map_registers(base, mapped, &region)) return MmioRegion();
    return region;
  }
  if (!mmio_->map_registers(base + (size - AmdMediaEngineHw::kDiscoveryWindow), AmdMediaEngineHw::kDiscoveryWindow, &region)) return MmioRegion();
  return region;
}

// The register BAR (BAR 5) of a Radeon card, first page: where MM_INDEX and MM_DATA are. Empty if there is no mapper, decoding is off, or no BAR 5.
MmioRegion SystemVideoCodecs::amd_registers(const PciDeviceInfo& d, uint64_t length) const {
  constexpr uint16_t kPciCommand = 0x04;
  constexpr uint32_t kMemorySpace = 1u << 1;
  MmioRegion region;
  if (!mmio_ || !(pci_.read32(d.address, kPciCommand) & kMemorySpace)) return region;
  const uint64_t base = pci_.memory_bar(d.address, 5);
  if (base == 0 || !mmio_->map_registers(base, length, &region)) return MmioRegion();
  return region;
}

void SystemVideoCodecs::add_amd(const PciDeviceInfo& d) {
  uint64_t bar_size = 0;
  const MmioRegion window = amd_discovery_window(d, &bar_size);
  UniquePointer<AmdVcnVideoEngine> device = UniquePointer<AmdVcnVideoEngine>::make(allocator_, allocator_, d.device_id, window);
  if (!device) return;
  device->set_pci_address(d.address);
  device->set_pci_revision(static_cast<uint8_t>(pci_.read32(d.address, 0x08) & 0xFF));   // config space offset 8: revision ID
  if (device->gpu().discrete && AmdMediaEngineHw::supported(device->gpu().architecture)) {
    device->set_memory_bar(bar_size);
    device->set_registers(amd_registers(d, AmdMmIndexVramReader::kRegisterWindow));
  }
  // The VCN 1.0 APUs (Raven family): their whole register BAR, from which the engine reads the registers of the memory controller to find where it may go.
  if (amd_vcn1_firmware_name(d.device_id, 0)) device->set_registers(amd_registers(d, kAmdRavenRegisterBytes));
  if (!device->initialize()) {
    device->shutdown();
    return;
  }
  devices_.push_back(UniquePointer<IVideoCodecDevice>(util::move(device)));
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
    if (d.vendor_id == NvidiaVideoEngine::kPciVendor) try_add<NvidiaVideoEngine>(devices_, allocator_, d.device_id, nvidia_registers(d));
    else if (d.vendor_id == AmdVcnVideoEngine::kPciVendor) add_amd(d);
    else if (d.vendor_id == IntelMediaVideoEngine::kPciVendor) add_intel(d);
  }
  try_add<SoftwareVideoCodec>(devices_, allocator_);
}

void SystemVideoCodecs::add_intel(const PciDeviceInfo& d) {
  UniquePointer<IntelMediaVideoEngine> device = UniquePointer<IntelMediaVideoEngine>::make(allocator_, allocator_, d.device_id, intel_registers(d));
  if (!device) return;
  // A discrete card (Arc) has memory of its own behind a BAR of its own (BAR 2); how big the firmware left that window tells whether Resizable BAR is on.
  if (device->gpu().discrete && bar_sizer_) {
    constexpr uint16_t kPciCommand = 0x04;
    constexpr uint32_t kMemorySpace = 1u << 1;
    uint64_t local_memory = 0;
    if ((pci_.read32(d.address, kPciCommand) & kMemorySpace) && bar_sizer_->bar_size(d.address, 2, &local_memory)) device->set_local_memory_bar(local_memory);
  }
  IntelHardware hardware;
  // The engine driver follows the Gen9.5 layouts only (its GTT geometry, its video box); no other generation gets its GTT looked at.
  if (intel_hardware_ && device->gpu().architecture == IntelGpuArchitecture::Gen9_5 && !intel_registers_bank_ && intel_hardware_setup(d, &hardware)) {
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
