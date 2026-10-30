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

// The PCI binding of a Thunderbolt 3 host interface: memory decoding and bus mastering (the rings are DMA), BAR 0 mapped, then the connection manager firmware brought up
// (ThunderboltIcm). The driver polls; whoever owns it calls poll() now and then to see devices come and go.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltCio.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltDevices.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltIcm.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class ThunderboltDriver {
 public:
  enum class Outcome : uint8_t { NotStarted, Started, UnknownDevice, BarUnmapped, OutOfMemory, ControllerFailed };
  static constexpr uint32_t kRegisterBytes = 0x40000;       // BAR 0 of the NHI

  struct Config {
    bool start_firmware = false;       // see ThunderboltIcm::Options
    bool reset_ports = true;
  };
  explicit ThunderboltDriver(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ThunderboltDriver(allocators::IAllocator& allocator, const Config& config) : allocator_(allocator), config_(config) {}
  ~ThunderboltDriver() { stop(); }
  ThunderboltDriver(const ThunderboltDriver&) = delete;
  ThunderboltDriver& operator=(const ThunderboltDriver&) = delete;

  static bool supports(const PciDeviceInfo& device) { return tb_find_nhi(device.vendor_id, device.device_id) != nullptr; }

  Outcome start(IPciBus& pci, const PciDeviceInfo& device, IRegisterMapper& mapper);
  // The part after the registers are reachable (tests bring their own).
  // `cio`: the PCIe-to-CIO registers if there are any (see ThunderboltCio).
  Outcome start_device(ITbRegisters& registers, TbPcie2Cio* cio = nullptr);
  void stop();

  Outcome outcome() const { return outcome_; }
  const TbNhiDevice* model() const { return model_; }
  ThunderboltIcm* icm() { return icm_.get(); }
  const char* message() const { return icm_ ? ThunderboltIcm::outcome_name(icm_->outcome()) : ""; }
  void poll() { if (icm_) icm_->poll(); }

 private:
  allocators::IAllocator& allocator_;
  Config config_;
  UniquePointer<PciFunctionConfig> upstream_config_;
  UniquePointer<TbPcie2Cio> cio_;
  UniquePointer<MmioTbRegisters> mmio_;
  UniquePointer<ThunderboltIcm> icm_;
  const TbNhiDevice* model_ = nullptr;
  Outcome outcome_ = Outcome::NotStarted;
};

}  // namespace hardware
