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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuProbe.hpp"

namespace hardware {

namespace {
constexpr uint16_t kPciCommand = 0x04;
constexpr uint32_t kMemorySpace = 1u << 1;
constexpr uint64_t kGiB = 1ull << 30;
constexpr uint64_t kMaxLocalMemory = 128ull << 30;   // more than any of these cards has
constexpr uint64_t kGsmBaseAlign = 1ull << 20;       // the stolen memory starts on a MiB (the low bits hold the lock bit)
constexpr uint32_t kTileSizeMask = 0x3FF00, kTileSizeShift = 8;   // GENMASK(17, 8)
constexpr uint32_t kTempMask = 0xFF;                 // TEMP_MASK, REG_GENMASK(7, 0)
}  // namespace

IntelGpuProbe::Platform IntelGpuProbe::platform_of(uint16_t device_id) {
  const IntelGpuInfo info = intel_gpu_identify(device_id);
  if (!info.discrete) return Platform::None;
  switch (info.architecture) {
    case IntelGpuArchitecture::XeLp: return Platform::Dg1;
    case IntelGpuArchitecture::XeHpg: return Platform::Dg2;
    case IntelGpuArchitecture::Xe2: return Platform::Battlemage;
    default: return Platform::None;   // (Data Center GPU Max: not a laptop part, and its tiles need more than this)
  }
}

uint64_t IntelGpuProbe::tile_bytes(uint32_t value) {
  if (value == 0xFFFFFFFFu) return 0;
  const uint64_t bytes = uint64_t{(value & kTileSizeMask) >> kTileSizeShift} * kGiB;
  return bytes > kMaxLocalMemory ? 0 : bytes;
}

int32_t IntelGpuProbe::package_temperature(uint32_t value) {
  if (value == 0xFFFFFFFFu) return -1;
  const int32_t degrees = static_cast<int32_t>(value & kTempMask);
  return degrees == 0 ? -1 : degrees * 1000;
}

IntelGpuProbe::IntelGpuProbe(const GpuContext& context)
    : context_(context), info_(intel_gpu_identify(context.device.device_id)), platform_(platform_of(context.device.device_id)) {}

IntelGpuProbe::IntelGpuProbe(intel_gt::IRegisterBank& registers, uint16_t device_id)
    : info_(intel_gpu_identify(device_id)), platform_(platform_of(device_id)), registers_(&registers) {}

bool IntelGpuProbe::map_registers() {
  if (registers_) return true;
  if (!context_.registers || !context_.pci) return fail("no register mapper");
  const PciAddress address = context_.device.address;
  if (!(context_.pci->read32(address, kPciCommand) & kMemorySpace)) return fail("memory decoding is off");
  const uint64_t bar0 = context_.pci->memory_bar(address, 0);
  MmioRegion window;
  if (bar0 == 0 || !context_.registers->map_registers(bar0, kRegisterBytes, &window)) return fail("BAR 0 not mapped");
  mmio_ = intel_gt::MmioRegisterBank(window);
  registers_ = &mmio_;
  return true;
}

bool IntelGpuProbe::read_vram(uint64_t* bytes) {
  *bytes = 0;
  const uint32_t gsm_low = registers_->read32(kGsmBase), gsm_high = registers_->read32(kGsmBase + 4);
  if (platform_ == Platform::Dg1) {
    if (gsm_low == 0xFFFFFFFFu && gsm_high == 0xFFFFFFFFu) return false;
    const uint64_t base = ((uint64_t{gsm_high} << 32) | gsm_low) & ~(kGsmBaseAlign - 1);
    *bytes = base > kMaxLocalMemory ? 0 : base;
    return true;
  }
  const uint32_t range = registers_->read32(kTileAddrRange0);
  if (range == 0xFFFFFFFFu && gsm_low == 0xFFFFFFFFu) return false;
  *bytes = tile_bytes(range);
  return true;
}

bool IntelGpuProbe::start() {
  started_ = false;
  if (platform_ == Platform::None) return fail("not a discrete Intel GPU this probe knows");
  if (!map_registers()) return false;
  uint64_t bytes = 0;
  if (!read_vram(&bytes)) return fail("the GPU does not answer (powered down?)");
  vram_bytes_ = bytes;
  status_ = bytes ? "identified" : "identified; the local memory size reads 0";
  started_ = true;
  return true;
}

bool IntelGpuProbe::resume() {
  if (!started_) return fail("not started");
  uint64_t bytes = 0;
  if (!read_vram(&bytes)) return fail("the GPU does not answer (powered down?)");
  // (The card's own firmware sets these up again at power-on; should it not have yet, the size from the first look stays.)
  if (bytes) vram_bytes_ = bytes;
  status_ = bytes ? "identified" : "identified; the local memory size reads 0 since the power cut";
  return true;
}

int32_t IntelGpuProbe::temperature_millicelsius() {
  if (!started_) return -1;
  switch (platform_) {
    case Platform::Dg1:
    case Platform::Dg2: return package_temperature(registers_->read32(kPcuPackageTemperature));
    case Platform::Battlemage: return package_temperature(registers_->read32(kBmgPackageTemperature));
    default: return -1;
  }
}

}  // namespace hardware
