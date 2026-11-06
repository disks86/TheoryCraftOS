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

#include "Hardware/NetworkInterface/PciNicDevice.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Hardware/Bus/Pci/PciBar.hpp"
#include "Kernel/Interrupts.hpp"
#include "Kernel/Thread.hpp"
#if defined(__x86_64__)
#include "Kernel/Apic.hpp"
#endif

namespace hardware {

namespace {
constexpr uint16_t kPciCommand = 0x04;
constexpr uint32_t kCommandMemory = 1u << 1, kCommandBusMaster = 1u << 2;
constexpr uint8_t kBarCount = 6;

#if defined(__x86_64__)
uint32_t g_network_vectors = 0;  // atomic: which of kVectorNetworkCount vectors are taken

uint8_t claim_network_vector() {
  uint32_t used = __atomic_load_n(&g_network_vectors, __ATOMIC_RELAXED);
  for (;;) {
    const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(~used));
    if (bit >= kernel::kVectorNetworkCount) return 0;
    if (__atomic_compare_exchange_n(&g_network_vectors, &used, used | (1u << bit), false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      return static_cast<uint8_t>(kernel::kVectorNetworkFirst + bit);
  }
}

void release_network_vector(uint8_t vector) {
  if (vector >= kernel::kVectorNetworkFirst && vector < kernel::kVectorNetworkFirst + kernel::kVectorNetworkCount)
    __atomic_fetch_and(&g_network_vectors, ~(1u << (vector - kernel::kVectorNetworkFirst)), __ATOMIC_ACQ_REL);
}

kernel::Frame* on_nic_interrupt(kernel::Frame* frame, void* context) {
  kernel::apic::eoi();
  PciNicDevice::InterruptHandler handler;
  void* handler_context;
  static_cast<PciNicDevice*>(context)->interrupt_target(&handler, &handler_context);
  if (handler) handler(handler_context);
  return frame;
}
#endif
}  // namespace

void PciNicDevice::interrupt_target(InterruptHandler* handler, void** context) const {
  *handler = __atomic_load_n(&handler_, __ATOMIC_ACQUIRE);
  *context = __atomic_load_n(&handler_context_, __ATOMIC_ACQUIRE);
}

bool PciNicDevice::attach_interrupt(InterruptHandler handler, void* context) {
#if defined(__x86_64__)
  if (vector_ || !handler || !kernel::running()) return false;
  const uint32_t core = kernel::choose_io_interrupt_core();
  const uint64_t apic = kernel::core_hardware_id(core);
  if (apic > 0xFF) return false;
  vector_ = claim_network_vector();
  if (!vector_) return false;
  __atomic_store_n(&handler_context_, context, __ATOMIC_RELEASE);
  __atomic_store_n(&handler_, handler, __ATOMIC_RELEASE);
  io_device_ = kernel::register_io_device("ethernet", static_cast<int32_t>(core));
  if (!kernel::set_device_interrupt_handler(vector_, io_device_, &on_nic_interrupt, this)) kernel::set_interrupt_handler(vector_, &on_nic_interrupt, this);
  if (!msi_.enable(device_.address, 0xFEE00000u | (apic << 12), vector_)) {
    detach_interrupt();
    return false;
  }
  return true;
#else
  (void)handler;
  (void)context;
  return false;
#endif
}

void PciNicDevice::detach_interrupt() {
#if defined(__x86_64__)
  msi_.disable();
  if (vector_) {
    kernel::set_device_interrupt_handler(vector_, io_device_, nullptr, nullptr);   // (clears the vector either way)
    kernel::set_interrupt_handler(vector_, nullptr, nullptr);
    release_network_vector(vector_);
    vector_ = 0;
  }
  if (io_device_ >= 0) kernel::unregister_io_device(io_device_);
  io_device_ = -1;
#endif
  __atomic_store_n(&handler_, static_cast<InterruptHandler>(nullptr), __ATOMIC_RELEASE);
}

bool PciNicDevice::claim() {
  // The firmware's driver goes first: sizing a BAR switches decoding off for a moment, which must not happen under a driver that is using the device.
  if (st_) detach_firmware_drivers(st_, device_.address);
  detached_ = true;

  MmioRegion region;
  for (uint8_t bar = 0; bar < kBarCount && !region.valid(); ++bar) {
    PciBarInfo info;
    if (!pci_read_bar(pci_, device_.address, bar, &info)) break;
    if (info.valid && !info.io && info.base != 0) {
      if (mapper_) {
        if (!mapper_->map_registers(info.base, info.size, &region)) return false;
      } else {
        region = MmioRegion(info.base, info.size);
      }
    }
    if (info.is64) ++bar;  // the upper half of this BAR is the next register
  }
  if (!region.valid()) return false;
  registers_ = MmioNicRegisters(region);

  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;  // (the status half is write-1-to-clear)
  pci_.write32(device_.address, kPciCommand, command | kCommandMemory | kCommandBusMaster);
  return true;
}

void PciNicDevice::release() {
  detach_interrupt();
  registers_ = MmioNicRegisters();
  if (!detached_) return;
  detached_ = false;
  if (st_) reattach_firmware_drivers(st_, device_.address);
}

}  // namespace hardware
