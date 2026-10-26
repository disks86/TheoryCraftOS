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
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpu.hpp"
#include "Hardware/Platform/RaspberryPi/Bcm2711.hpp"
#include "Hardware/Platform/RaspberryPi/RaspberryPiBoard.hpp"
#include "Hardware/Platform/RaspberryPi/VcMailbox.hpp"
#include "Hardware/Platform/RaspberryPi/VideoCoreFirmware.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// A message buffer taken from the allocator's DMA memory, reachable through the VideoCore's 0xC0000000 bus alias (which covers the first 1 GiB of ARM memory only; a
// buffer above that is refused). The cache maintenance is the AArch64 data cache cleaning by address, a no-op elsewhere (host tests).
class PiMessageMemory final : public IVcMessageMemory {
 public:
  static constexpr uint64_t kWords = 128;
  explicit PiMessageMemory(allocators::IAllocator& allocator);
  ~PiMessageMemory() { allocator_.deallocate(block_); }
  PiMessageMemory(const PiMessageMemory&) = delete;
  PiMessageMemory& operator=(const PiMessageMemory&) = delete;

  bool valid() const { return words_.size() != 0; }
  ArrayView<uint32_t> words() override { return words_; }
  uint32_t bus_address() const override { return bus_; }
  void clean() override;
  void invalidate() override;

 private:
  allocators::IAllocator& allocator_;
  void* block_ = nullptr;
  ArrayView<uint32_t> words_;
  uint32_t bus_ = 0;
};

// The Raspberry Pi 4 / Pi 400 platform: the firmware mailbox, the board it reports, and the media hardware of the BCM2711 behind it (the V3D 3D core and the HEVC
// decoder block). Registers are reached through an IRegisterMapper (the UEFI image's mapping is the identity: the firmware already maps the peripherals).
class RaspberryPiPlatform {
 public:
  RaspberryPiPlatform(allocators::IAllocator& allocator, IRegisterMapper& mapper) : allocator_(allocator), mapper_(mapper) {}
  ~RaspberryPiPlatform();

  // Maps the mailbox, asks the firmware which board this is, and keeps going only for a BCM2711 board. Call only on a machine known to be a Raspberry Pi 4 family
  // board (touching the mailbox on other machines faults).
  bool initialize();
  // For tests and ports: a mailbox that is not in the BCM2711 place, and (optionally) a message buffer of its own. The buffer must outlive the platform.
  bool initialize_with_port(IVcMailboxPort& port, IVcMessageMemory* message_memory = nullptr);

  const PiBoard& board() const { return board_; }
  VideoCoreFirmware* firmware() { return firmware_.get(); }

  // Asks the firmware for the V3D clock at its maximum, maps the V3D registers and starts the core. False if the core is not a V3D 4.x.
  bool start_v3d();
  V3dGpu* v3d() { return v3d_.get(); }

  // Maps the HEVC decoder block's registers and its interrupt controller (for an RpiHevcBlock or a BroadcomVideoEngine). False if either cannot be mapped.
  bool map_hevc(MmioRegion* hevc, MmioRegion* intc);

 private:
  allocators::IAllocator& allocator_;
  IRegisterMapper& mapper_;
  UniquePointer<MmioVcMailboxPort> mmio_port_;
  UniquePointer<PiMessageMemory> memory_;
  UniquePointer<VcMailbox> mailbox_;
  UniquePointer<VideoCoreFirmware> firmware_;
  UniquePointer<V3dGpu> v3d_;
  PiBoard board_;
  bool v3d_clock_on_ = false;
};

}  // namespace hardware
