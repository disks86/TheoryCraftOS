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

#include "Hardware/Bus/Thunderbolt/ThunderboltDriver.hpp"

namespace hardware {

namespace {
constexpr uint16_t kPciCommand = 0x04;
}

ThunderboltDriver::Outcome ThunderboltDriver::start(IPciBus& pci, const PciDeviceInfo& device, IRegisterMapper& mapper) {
  stop();
  model_ = tb_find_nhi(device.vendor_id, device.device_id);
  if (!model_) return outcome_ = Outcome::UnknownDevice;
  // memory space and bus mastering; the status bits above the command register clear when written, so only the low half is kept
  const uint32_t command = pci.read32(device.address, kPciCommand) & 0xFFFF;
  pci.write32(device.address, kPciCommand, command | (1u << 1) | (1u << 2));
  const uint64_t base = pci.memory_bar(device.address, 0);
  MmioRegion region;
  if (!base || !mapper.map_registers(base, kRegisterBytes, &region)) return outcome_ = Outcome::BarUnmapped;
  mmio_ = UniquePointer<MmioTbRegisters>::make(allocator_, region);
  if (!mmio_) return outcome_ = Outcome::OutOfMemory;
  attach_power(pci, device.address);
  // the controller's own registers through the switch's upstream port, if it has them
  PciAddress upstream;
  if (tb_find_upstream_port(pci, device, &upstream)) {
    upstream_config_ = UniquePointer<PciFunctionConfig>::make(allocator_, pci, upstream);
    const uint16_t capability = upstream_config_ ? tb_find_vendor_capability(*upstream_config_) : 0;
    if (capability != 0) cio_ = UniquePointer<TbPcie2Cio>::make(allocator_, *upstream_config_, capability);
  }
  nhi_config_ = UniquePointer<PciFunctionConfig>::make(allocator_, pci, device.address);
  return start_device(*mmio_, cio_.get(), model_->generation, nhi_config_.get());
}

ThunderboltDriver::Outcome ThunderboltDriver::start_device(ITbRegisters& registers, TbPcie2Cio* cio, TbGeneration generation, ITbConfigSpace* nhi_config) {
  icl_ = UniquePointer<TbIceLake>();
  if (generation == TbGeneration::IceLake) {
    // the firmware runs only while the controller is force powered
    if (!nhi_config) return outcome_ = Outcome::ControllerFailed;
    icl_ = UniquePointer<TbIceLake>::make(allocator_, *nhi_config);
    if (!icl_ || !icl_->force_power(true)) return outcome_ = Outcome::ControllerFailed;
    icl_->set_ltr();
  }
  if (generation == TbGeneration::Usb4) {
    // no firmware to ask: the host router is configured by Usb4Domain through the control channel (the controller is powered like an Ice Lake one)
    if (nhi_config) {
      icl_ = UniquePointer<TbIceLake>::make(allocator_, *nhi_config);
      if (!icl_ || !icl_->force_power(true)) return outcome_ = Outcome::ControllerFailed;
      icl_->set_ltr();
    }
    usb4_nhi_ = UniquePointer<TbNhi>::make(allocator_, registers);
    if (!usb4_nhi_) return outcome_ = Outcome::OutOfMemory;
    usb4_nhi_->disable_interrupts();
    usb4_control_ = UniquePointer<TbControl>::make(allocator_, *usb4_nhi_, allocator_);
    if (!usb4_control_ || !usb4_control_->start()) {
      usb4_error_ = "the control channel did not start";
      return outcome_ = Outcome::ControllerFailed;
    }
    usb4_ = UniquePointer<Usb4Domain>::make(allocator_, *usb4_control_, *usb4_nhi_, allocator_);
    if (!usb4_) return outcome_ = Outcome::OutOfMemory;
    if (!usb4_->start(&usb4_error_)) return outcome_ = Outcome::ControllerFailed;
    return outcome_ = Outcome::Started;
  }
  icm_ = UniquePointer<ThunderboltIcm>::make(allocator_, registers, allocator_);
  if (!icm_) return outcome_ = Outcome::OutOfMemory;
  ThunderboltIcm::Options options;
  options.cio = cio;
  options.start_firmware = config_.start_firmware;
  options.generation = generation;
  options.reset_ports = config_.reset_ports;
  icm_->set_options(options);
  const ThunderboltIcm::Outcome result = icm_->start();
  // (a controller in safe mode still answers, with no devices: reported as a failure so nobody waits for devices)
  return outcome_ = result == ThunderboltIcm::Outcome::Started ? Outcome::Started : Outcome::ControllerFailed;
}

void ThunderboltDriver::probe_control() {
  uint32_t word = 0;
  if (icm_) icm_->control().read_config(0, 0, TbConfigSpace::Switch, 0, ArrayView<uint32_t>(&word, 1), 100000, nullptr, icm_.get());
  else if (usb4_control_) usb4_control_->read_config(0, 0, TbConfigSpace::Switch, 0, ArrayView<uint32_t>(&word, 1), 100000, nullptr, usb4_.get());
}

bool ThunderboltDriver::sleep(bool wake, bool runtime) {
  if (!icm_ && !usb4_) return false;
  bool devices = device_count() != 0 || xdomain_count() != 0;
  if (icm_) {
    if (!(runtime ? icm_->runtime_suspend() : icm_->suspend())) return false;
  } else {
    if (runtime) return false;                                // (the software connection manager has no runtime power management)
    usb4_->suspend();                                          // (routers asleep, tunnels down; the control channel stops with it)
    usb4_control_->stop();
  }
  if (icl_ && !devices) {
    // (icl_nhi_suspend: with nothing connected the link controller is told to get ready for a reset and the power forced off; with something connected the controller stays powered)
    icl_->lc_mailbox(tb_icl::kPrepareForReset);
    icl_->force_power(false);
    icl_powered_ = false;
  }
  wake_armed_ = false;
  if (power_ && power_->available()) {
    // (a wake that cannot be armed still lets the function sleep)
    wake_armed_ = wake && power_->enter_d3hot(true);
    if (!wake_armed_) power_->enter_d3hot(false);
  }
  return true;
}

ThunderboltDriver::Outcome ThunderboltDriver::resume() {
  if (!icm_ && !usb4_) return outcome_;
  if (power_ && power_->available()) power_->enter_d0();
  wake_armed_ = false;
  if (icl_ && !icl_powered_) {
    if (!icl_->force_power(true)) return outcome_ = Outcome::ControllerFailed;
    icl_->set_ltr();
    icl_powered_ = true;
  }
  if (usb4_) {
    usb4_nhi_->disable_interrupts();
    if (!usb4_control_->start()) return outcome_ = Outcome::ControllerFailed;
    return outcome_ = usb4_->resume(&usb4_error_) ? Outcome::Started : Outcome::ControllerFailed;
  }
  return outcome_ = icm_->resume() == ThunderboltIcm::Outcome::Started ? Outcome::Started : Outcome::ControllerFailed;
}

void ThunderboltDriver::stop() {
  usb4_ = UniquePointer<Usb4Domain>();
  if (usb4_control_) usb4_control_->stop();
  usb4_control_ = UniquePointer<TbControl>();
  usb4_nhi_ = UniquePointer<TbNhi>();
  icm_ = UniquePointer<ThunderboltIcm>();                       // (tells the firmware first)
  if (icl_ && icl_powered_) icl_->force_power(false);           // (the controller is shut down: the firmware may stop)
  icl_ = UniquePointer<TbIceLake>();
  icl_powered_ = true;
  cio_ = UniquePointer<TbPcie2Cio>();
  power_ = UniquePointer<TbPciPower>();
  upstream_config_ = UniquePointer<PciFunctionConfig>();
  mmio_ = UniquePointer<MmioTbRegisters>();
  outcome_ = Outcome::NotStarted;
}

}  // namespace hardware
