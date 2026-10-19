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

#include "Hardware/HyperV/HyperV.hpp"
#include "Hardware/CentralProcessingUnit/Cpuid.hpp"
#include "Kernel/Interrupts.hpp"

namespace hardware {

namespace {
constexpr uint32_t kMsrGuestOsId = 0x40000000, kMsrHypercall = 0x40000001, kMsrVpIndex = 0x40000002, kMsrReferenceCounter = 0x40000020,
                   kMsrScontrol = 0x40000080, kMsrSiefp = 0x40000082, kMsrSimp = 0x40000083, kMsrEom = 0x40000084, kMsrSint0 = 0x40000090;
constexpr uint32_t kLeafInterface = 0x40000001, kLeafVersion = 0x40000002, kLeafFeatures = 0x40000003;
constexpr uint32_t kPrivSynic = 1u << 2, kPrivHypercall = 1u << 5, kPrivVpIndex = 1u << 6, kPrivReferenceCounter = 1u << 1;
constexpr uint32_t kPrivPostMessages = 1u << 4, kPrivSignalEvents = 1u << 5;   // (in EBX)
constexpr uint64_t kPage = 4096;
constexpr uint64_t kHypercallPostMessage = 0x5C, kHypercallSignalEvent = 0x5D, kFast = 1u << 16;

// 0x8000... marks an open-source OS; the rest identifies the vendor and version.
constexpr uint64_t kGuestOsId = (1ull << 63) | (0x0EC1ull << 48) | (1ull << 16);

uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return (uint64_t{high} << 32) | low;
}
void wrmsr(uint32_t msr, uint64_t value) {
  __asm__ volatile("wrmsr" ::"c"(msr), "a"(static_cast<uint32_t>(value)), "d"(static_cast<uint32_t>(value >> 32)) : "memory");
}

kernel::Frame* ignore_synic_interrupt(kernel::Frame* frame, void*) { return frame; }   // (auto end-of-interrupt: nothing to acknowledge)
}  // namespace

bool HyperV::detected() {
  const CpuidResult base = cpuid(0x40000000);
  // "Microsoft Hv"
  if (base.ebx != 0x7263694D || base.ecx != 0x666F736F || base.edx != 0x76482074) return false;
  if (base.eax < kLeafFeatures) return false;
  if (cpuid(kLeafInterface).eax != 0x31237648) return false;   // "Hv#1"
  const CpuidResult features = cpuid(kLeafFeatures);
  return (features.eax & kPrivHypercall) && (features.eax & kPrivSynic) && (features.eax & kPrivVpIndex) &&
         (features.ebx & kPrivPostMessages) && (features.ebx & kPrivSignalEvents);
}

bool HyperV::initialize(allocators::IAllocator& allocator) {
  if (!detected()) return false;
  allocator_ = &allocator;
  max_leaf_ = cpuid(0x40000000).eax;
  build_ = cpuid(kLeafVersion).eax;
  referenced_time_ = (cpuid(kLeafFeatures).eax & kPrivReferenceCounter) != 0;
  vp_index_ = static_cast<uint32_t>(rdmsr(kMsrVpIndex));

  hypercall_page_ = static_cast<uint8_t*>(allocator.allocate_aligned(kPage, kPage));
  message_page_ = static_cast<uint8_t*>(allocator.allocate_dma(kPage, kPage));
  event_page_ = static_cast<uint8_t*>(allocator.allocate_dma(kPage, kPage));
  post_buffer_ = static_cast<uint8_t*>(allocator.allocate_dma(kPage, kPage));
  if (!hypercall_page_ || !message_page_ || !event_page_ || !post_buffer_) {
    shutdown();
    return false;
  }
  __builtin_memset(message_page_, 0, kPage);
  __builtin_memset(event_page_, 0, kPage);
  __builtin_memset(post_buffer_, 0, kPage);

  wrmsr(kMsrGuestOsId, kGuestOsId);
  // The hypervisor writes the hypercall code into the page when the enable bit goes on.
  const uint64_t hypercall = rdmsr(kMsrHypercall);
  wrmsr(kMsrHypercall, (hypercall & 0xFFE) | reinterpret_cast<uint64_t>(hypercall_page_) | 1);
  if (!(rdmsr(kMsrHypercall) & 1)) {
    shutdown();
    return false;
  }

  kernel::set_interrupt_handler(kSynicVector, ignore_synic_interrupt, nullptr);
  const uint64_t simp = rdmsr(kMsrSimp), siefp = rdmsr(kMsrSiefp);
  wrmsr(kMsrSimp, (simp & 0xFFE) | reinterpret_cast<uint64_t>(message_page_) | 1);
  wrmsr(kMsrSiefp, (siefp & 0xFFE) | reinterpret_cast<uint64_t>(event_page_) | 1);
  wrmsr(kMsrSint0 + kMessageSint, kSynicVector | (1u << 17));   // unmasked, auto end-of-interrupt
  wrmsr(kMsrScontrol, rdmsr(kMsrScontrol) | 1);
  synic_ready_ = true;
  return true;
}

void HyperV::shutdown() {
  if (synic_ready_) {
    synic_ready_ = false;
    wrmsr(kMsrSint0 + kMessageSint, rdmsr(kMsrSint0 + kMessageSint) | (1u << 16));   // masked
    wrmsr(kMsrScontrol, rdmsr(kMsrScontrol) & ~uint64_t{1});
    wrmsr(kMsrSimp, rdmsr(kMsrSimp) & 0xFFE);
    wrmsr(kMsrSiefp, rdmsr(kMsrSiefp) & 0xFFE);
  }
  if (hypercall_page_) {
    wrmsr(kMsrHypercall, rdmsr(kMsrHypercall) & 0xFFE);
    wrmsr(kMsrGuestOsId, 0);
  }
  if (allocator_) {
    allocator_->deallocate(hypercall_page_);
    allocator_->deallocate(message_page_);
    allocator_->deallocate(event_page_);
    allocator_->deallocate(post_buffer_);
  }
  hypercall_page_ = message_page_ = event_page_ = post_buffer_ = nullptr;
}

// Microsoft x64 hypercall convention: control in RCX, input in RDX, output in R8; the status comes back in RAX. The call goes through the
// page the hypervisor filled in (a `vmcall` and a `ret`).
uint64_t HyperV::call(uint64_t control, uint64_t input, uint64_t output) {
  uint64_t status;
  register uint64_t r8 __asm__("r8") = output;
  __asm__ volatile("call *%[page]"
                   : "=a"(status), "+c"(control), "+d"(input), "+r"(r8)
                   : [page] "r"(hypercall_page_)
                   : "r9", "r10", "r11", "cc", "memory");
  return status & 0xFFFF;
}

uint64_t HyperV::post_message(uint32_t connection, uint32_t message_type, const void* payload, uint32_t size) {
  if (!ready() || size > 240) return 0xFFFF;
  uint32_t* input = reinterpret_cast<uint32_t*>(post_buffer_);
  input[0] = connection;
  input[1] = 0;
  input[2] = message_type;
  input[3] = size;
  __builtin_memcpy(post_buffer_ + 16, payload, size);
  return call(kHypercallPostMessage, reinterpret_cast<uint64_t>(post_buffer_), 0);
}

uint64_t HyperV::signal_event(uint32_t connection, uint16_t flag) {
  if (!ready()) return 0xFFFF;
  return call(kHypercallSignalEvent | kFast, connection, flag);
}

bool HyperV::take_message(HyperVMessage* out) {
  if (!synic_ready_) return false;
  volatile HyperVMessage* slot = reinterpret_cast<volatile HyperVMessage*>(message_page_) + kMessageSint;
  if (slot->type == 0) return false;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  out->type = slot->type;
  out->payload_size = slot->payload_size;
  out->flags = slot->flags;
  out->sender = slot->sender;
  for (uint32_t i = 0; i < 240 && i < slot->payload_size; ++i) out->payload[i] = slot->payload[i];
  const bool pending = slot->flags & 1;
  slot->type = 0;   // the slot is free
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (pending) wrmsr(kMsrEom, 0);   // another message is waiting: let the hypervisor put it in the slot
  return true;
}

uint64_t HyperV::reference_time() const { return referenced_time_ ? rdmsr(kMsrReferenceCounter) : 0; }

}  // namespace hardware
