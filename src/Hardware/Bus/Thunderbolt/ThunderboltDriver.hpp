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
#include "Hardware/Bus/Thunderbolt/ThunderboltIcl.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltIcm.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltPower.hpp"
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
  // `nhi_config`: the NHI's PCI configuration space, which an Ice Lake controller needs for its power handling (force power, the link controller's mailbox); not otherwise.
  Outcome start_device(ITbRegisters& registers, TbPcie2Cio* cio = nullptr, TbGeneration generation = TbGeneration::AlpineRidge, ITbConfigSpace* nhi_config = nullptr);
  void stop();

  Outcome outcome() const { return outcome_; }
  const TbNhiDevice* model() const { return model_; }
  ThunderboltIcm* icm() { return icm_.get(); }
  const char* message() const { return icm_ ? ThunderboltIcm::outcome_name(icm_->outcome()) : ""; }
  void poll() { if (icm_) icm_->poll(); }
  // Interrupt-driven operation. `on_interrupt()` is what the MSI handler calls: it acknowledges the chip and counts. `interrupts()` is the count, for a loop that polls only after one
  // came (and now and then regardless, in case one was lost). `use_interrupts(true)` unmasks the control channel's ring interrupts in the chip; the MSI itself is the caller's.
  void on_interrupt() {
    if (icm_ && icm_->nhi().acknowledge_interrupt()) __atomic_fetch_add(&interrupts_, 1, __ATOMIC_ACQ_REL);
  }
  uint32_t interrupts() const { return __atomic_load_n(&interrupts_, __ATOMIC_ACQUIRE); }
  // `msix_vector`: the entry of the MSI-X table that carries them (see PciMsix), or -1 for a single MSI.
  bool use_interrupts(bool on, int msix_vector = -1) { return icm_ && icm_->use_interrupts(on, msix_vector); }
  // Sleep. suspend() puts the controller to sleep (ThunderboltIcm::suspend) and the PCI function in D3hot, with its wake-up (PME) armed if `wake` is asked for and the function can raise one
  // from D3hot (`wake_armed()` says); resume() brings the function back to D0 and the controller up again. runtime_suspend() is the same while the machine runs, for a controller the
  // firmware allows that for (ThunderboltIcm::runtime_allowed): not with a path to another host open or while the firmware vetoes it.
  bool suspend(bool wake = false) { return sleep(wake, false); }
  bool runtime_suspend(bool wake = true) { return sleep(wake, true); }
  Outcome resume();
  bool wake_armed() const { return wake_armed_; }
  // Does the function say something woke it? (PME status)
  bool wake_pending() { return power_ && power_->wake_pending(); }
  // The power management of the PCI function (done by start(); a test supplies its own).
  void attach_power(IPciBus& bus, PciAddress address) { power_ = UniquePointer<TbPciPower>::make(allocator_, bus, address); }

 private:
  bool sleep(bool wake, bool runtime);
  allocators::IAllocator& allocator_;
  Config config_;
  uint32_t interrupts_ = 0;
  bool wake_armed_ = false, icl_powered_ = true;
  UniquePointer<TbPciPower> power_;
  UniquePointer<PciFunctionConfig> nhi_config_;
  UniquePointer<TbIceLake> icl_;
  UniquePointer<PciFunctionConfig> upstream_config_;
  UniquePointer<TbPcie2Cio> cio_;
  UniquePointer<MmioTbRegisters> mmio_;
  UniquePointer<ThunderboltIcm> icm_;
  const TbNhiDevice* model_ = nullptr;
  Outcome outcome_ = Outcome::NotStarted;
};

}  // namespace hardware
