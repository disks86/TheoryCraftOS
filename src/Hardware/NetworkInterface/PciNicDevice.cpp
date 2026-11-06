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
uint32_t g_network_vectors = 0;  // atomic: which of kVectorNetworkCount vectors are taken (bits 0-7 the first range, 8-15 the second)

uint8_t vector_of(uint32_t bit) {
  return static_cast<uint8_t>(bit < 8 ? kernel::kVectorNetworkFirst + bit : kernel::kVectorNetworkSecondFirst + (bit - 8));
}

uint8_t claim_network_vector() {
  uint32_t used = __atomic_load_n(&g_network_vectors, __ATOMIC_RELAXED);
  for (;;) {
    const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(~used));
    if (bit >= kernel::kVectorNetworkCount) return 0;
    if (__atomic_compare_exchange_n(&g_network_vectors, &used, used | (1u << bit), false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return vector_of(bit);
  }
}

void release_network_vector(uint8_t vector) {
  for (uint32_t bit = 0; bit < kernel::kVectorNetworkCount; ++bit)
    if (vector_of(bit) == vector) __atomic_fetch_and(&g_network_vectors, ~(1u << bit), __ATOMIC_ACQ_REL);
}

kernel::Frame* on_nic_queue_interrupt(kernel::Frame* frame, void* context) {
  kernel::apic::eoi();
  const PciNicDevice::VectorEntry* entry = static_cast<const PciNicDevice::VectorEntry*>(context);
  PciNicDevice::QueueInterruptHandler handler;
  void* handler_context;
  entry->device->queue_target(&handler, &handler_context);
  if (handler) handler(handler_context, entry->index);
  return frame;
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

void PciNicDevice::queue_target(QueueInterruptHandler* handler, void** context) const {
  *handler = __atomic_load_n(&queue_handler_, __ATOMIC_ACQUIRE);
  *context = __atomic_load_n(&queue_context_, __ATOMIC_ACQUIRE);
}

uint32_t PciNicDevice::attach_interrupts(uint32_t count, QueueInterruptHandler handler, void* context) {
#if defined(__x86_64__)
  if (vector_ || vector_count_ || !handler || count == 0 || count > kMaxVectors || !mapper_ || !kernel::running()) return 0;
  if (msix_.table_size(device_.address) < count) return 0;
  const uint32_t first = kernel::choose_io_interrupt_core();
  const uint32_t cores = kernel::core_count() ? kernel::core_count() : 1;
  uint32_t apic_ids[kMaxVectors];
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t core = (first + i) % cores;
    const uint64_t apic = kernel::core_hardware_id(core);
    vectors_[i] = claim_network_vector();
    if (apic > 0xFF || !vectors_[i]) {
      vector_count_ = i + (vectors_[i] ? 1 : 0);
      detach_interrupt();
      return 0;
    }
    apic_ids[i] = static_cast<uint32_t>(apic);
    entries_[i] = VectorEntry{this, i};
    vector_count_ = i + 1;
    io_devices_[i] = kernel::register_io_device("ethernet-queue", static_cast<int32_t>(core));
  }
  __atomic_store_n(&queue_context_, context, __ATOMIC_RELEASE);
  __atomic_store_n(&queue_handler_, handler, __ATOMIC_RELEASE);
  for (uint32_t i = 0; i < count; ++i)
    if (!kernel::set_device_interrupt_handler(vectors_[i], io_devices_[i], &on_nic_queue_interrupt, &entries_[i]))
      kernel::set_interrupt_handler(vectors_[i], &on_nic_queue_interrupt, &entries_[i]);
  if (!msix_.enable(device_.address, vectors_, apic_ids, count)) {
    detach_interrupt();
    return 0;
  }
  return count;
#else
  (void)count;
  (void)handler;
  (void)context;
  return 0;
#endif
}

bool PciNicDevice::attach_interrupt(InterruptHandler handler, void* context) {
#if defined(__x86_64__)
  if (vector_ || vector_count_ || !handler || !kernel::running()) return false;
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
  msix_.disable();
  for (uint32_t i = 0; i < vector_count_; ++i) {
    if (vectors_[i]) {
      kernel::set_device_interrupt_handler(vectors_[i], io_devices_[i], nullptr, nullptr);
      kernel::set_interrupt_handler(vectors_[i], nullptr, nullptr);
      release_network_vector(vectors_[i]);
      vectors_[i] = 0;
    }
    if (io_devices_[i] >= 0) kernel::unregister_io_device(io_devices_[i]);
    io_devices_[i] = -1;
  }
  vector_count_ = 0;
  __atomic_store_n(&queue_handler_, static_cast<QueueInterruptHandler>(nullptr), __ATOMIC_RELEASE);
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
