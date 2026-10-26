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

#include "Hardware/Platform/RaspberryPi/RaspberryPiPlatform.hpp"
#include "Hardware/Platform/RaspberryPi/PiCache.hpp"

namespace hardware {

namespace {
constexpr uint64_t kCacheLine = 64;
}  // namespace

PiMessageMemory::PiMessageMemory(allocators::IAllocator& allocator) : allocator_(allocator) {
  block_ = allocator.allocate_dma(kWords * 4, kCacheLine);
  if (!block_) return;
  const uint64_t address = reinterpret_cast<uint64_t>(block_);
  if (address + kWords * 4 > (uint64_t{1} << 30)) {  // beyond what the 0xC0000000 alias can name
    allocator_.deallocate(block_);
    block_ = nullptr;
    return;
  }
  words_ = ArrayView<uint32_t>(static_cast<uint32_t*>(block_), kWords);
  bus_ = bcm2711::arm_to_bus(address);
}

void PiMessageMemory::clean() { pi_cache_clean_invalidate(words_.data(), words_.size() * 4); }
void PiMessageMemory::invalidate() { pi_cache_clean_invalidate(words_.data(), words_.size() * 4); }

RaspberryPiPlatform::~RaspberryPiPlatform() {
  if (v3d_) v3d_->shutdown();
  if (firmware_ && v3d_clock_on_) firmware_->set_clock_enabled(vc_clock::kV3d, false);
}

bool RaspberryPiPlatform::initialize() {
  MmioRegion registers;
  if (!mapper_.map_registers(bcm2711::kMailboxBase, bcm2711::kMailboxLength, &registers)) return false;
  mmio_port_ = UniquePointer<MmioVcMailboxPort>::make(allocator_, registers);
  return mmio_port_ && initialize_with_port(*mmio_port_);
}

bool RaspberryPiPlatform::initialize_with_port(IVcMailboxPort& port, IVcMessageMemory* message_memory) {
  if (!message_memory) {
    memory_ = UniquePointer<PiMessageMemory>::make(allocator_, allocator_);
    if (!memory_ || !memory_->valid()) return false;
    message_memory = memory_.get();
  }
  mailbox_ = UniquePointer<VcMailbox>::make(allocator_, port, *message_memory);
  if (!mailbox_) return false;
  firmware_ = UniquePointer<VideoCoreFirmware>::make(allocator_, *mailbox_);
  if (!firmware_ || !firmware_->board(&board_)) return false;
  return board_.is_bcm2711();
}

bool RaspberryPiPlatform::start_v3d() {
  if (!firmware_ || !board_.is_bcm2711()) return false;
  MmioRegion hub, core;
  if (!mapper_.map_registers(bcm2711::kV3dHubBase, bcm2711::kV3dWindow, &hub) || !mapper_.map_registers(bcm2711::kV3dCoreBase, bcm2711::kV3dWindow, &core)) return false;
  // The core's clock is off until the firmware is asked, and sits at a low rate; ask for the highest it allows.
  uint32_t max_hz = 0, actual = 0;
  if (!firmware_->max_clock_rate(vc_clock::kV3d, &max_hz) || max_hz == 0) return false;
  if (!firmware_->set_clock_rate(vc_clock::kV3d, max_hz, &actual) || !firmware_->set_clock_enabled(vc_clock::kV3d, true)) return false;
  v3d_clock_on_ = true;
  v3d_ = UniquePointer<V3dGpu>::make(allocator_, hub, core);
  if (!v3d_ || !v3d_->initialize()) {
    v3d_.reset();
    return false;
  }
  return true;
}

bool RaspberryPiPlatform::map_hevc(MmioRegion* hevc, MmioRegion* intc) {
  return mapper_.map_registers(bcm2711::kHevcBase, bcm2711::kHevcWindow, hevc) && mapper_.map_registers(bcm2711::kHevcIntcBase, bcm2711::kHevcIntcWindow, intc);
}

}  // namespace hardware
