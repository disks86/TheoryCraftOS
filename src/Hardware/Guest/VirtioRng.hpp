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
#include "Crypto/Random.hpp"
#include "Hardware/Bus/Virtio/VirtioPciDevice.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

constexpr uint16_t kVirtioRngDevice = 0x1044;              // 0x1040 + device type 4
constexpr uint16_t kVirtioRngTransitionalDevice = 0x1005;

// A virtio-rng device (QEMU's `virtio-rng-pci`): the host's entropy pool, one request queue. It is an
// entropy source for the DRBG, so a guest whose CPU has neither RDSEED nor RDRAND still gets real
// randomness instead of timing jitter.
class VirtioRng final : public IHardware, public crypto::IEntropySource {
 public:
  static constexpr uint32_t kRequestBytes = 256;
  static constexpr uint64_t kTimeoutUs = 200000;  // the host should answer well within this

  VirtioRng(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator, efi::SystemTable* st)
      : transport_(pci, device, allocator, st), allocator_(allocator) {}

  const char* name() const override { return "virtio entropy source"; }
  bool initialize() override;
  void shutdown() override;

  // Fills `out` from the host. False if the device did not answer in time (nothing is guessed).
  bool read(ByteView out) override;

 private:
  VirtioPciDevice transport_;
  allocators::IAllocator& allocator_;
  Virtqueue queue_;
  ArrayView<uint8_t> buffer_;
  bool started_ = false;
};

}  // namespace hardware
