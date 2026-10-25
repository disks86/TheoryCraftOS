/*
 * TheoryCraftOS - a minimal UEFI operating system.
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

#include "Arch/AArch64/Cpu.hpp"
#include "Utility/ByteStream.hpp"

extern "C" void tcos_secondary_entry();

namespace hardware {

namespace {
constexpr uint64_t kAffinityMask = 0xFF00FFFFFFull;

uint64_t current_el() {
  uint64_t v;
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
  return v >> 2;
}

uint64_t boot_mpidr() {
  uint64_t v;
  __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
  return v & kAffinityMask;
}

// Writes the data cache lines of [address, address + bytes) out to memory: a core that starts with its caches and
// MMU off reads memory directly and must not miss what the boot core only has in its cache.
void clean_to_memory(const void* data, uint64_t bytes) {
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uint64_t line = uint64_t{4} << ((ctr >> 16) & 0xF);
  const uint64_t begin = reinterpret_cast<uint64_t>(data) & ~(line - 1);
  const uint64_t end = reinterpret_cast<uint64_t>(data) + bytes;
  for (uint64_t a = begin; a < end; a += line) __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
}

// The entry address is code, handed to the firmware as a number.
uint64_t entry_address() { return reinterpret_cast<uint64_t>(&tcos_secondary_entry); }

void read_translation_setup(SecondaryContext* c) {
  if (current_el() == 2) {
    __asm__ volatile("mrs %0, ttbr0_el2" : "=r"(c->ttbr0));
    __asm__ volatile("mrs %0, tcr_el2" : "=r"(c->tcr));
    __asm__ volatile("mrs %0, mair_el2" : "=r"(c->mair));
    __asm__ volatile("mrs %0, sctlr_el2" : "=r"(c->sctlr));
    __asm__ volatile("mrs %0, hcr_el2" : "=r"(c->hcr));
  } else {
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(c->ttbr0));
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(c->tcr));
    __asm__ volatile("mrs %0, mair_el1" : "=r"(c->mair));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(c->sctlr));
    c->hcr = 0;
  }
}
}  // namespace

bool AArch64Cpu::discover(const AcpiTables& acpi, allocators::IAllocator& allocator) {
  allocator_ = &allocator;
  conduit_ = arch_aarch64::psci_conduit(acpi);
  mpidr_[0] = boot_mpidr();
  count_ = 1;
  started_[0] = true;
  const ConstByteView madt = acpi.find("APIC");
  constexpr uint32_t kEntries = 44;
  constexpr uint8_t kTypeGicc = 0xB;
  if (madt.size() < kEntries) return false;
  for (uint32_t offset = kEntries; offset + 2 <= madt.size();) {
    const uint8_t type = madt[offset], length = madt[offset + 1];
    if (length < 2 || length > madt.size() - offset) break;
    if (type == kTypeGicc && length >= 76) {
      ByteReader r(madt.subview(offset, length));
      r.skip(2 + 2 + 4 + 4);                 // header, reserved, interface number, ACPI uid
      const uint32_t flags = r.le32();
      r.skip(68 - 16);                       // up to the MPIDR field
      const uint64_t mpidr = r.le64() & kAffinityMask;
      if (r.ok() && (flags & 1) && mpidr != mpidr_[0] && count_ < kMaxCores) mpidr_[count_++] = mpidr;
    }
    offset += length;
  }
  return true;
}

bool AArch64Cpu::start_on_core(uint32_t core, CoreFunction fn, void* context) {
  if (core == 0 || core >= count_ || started_[core] || !allocator_ || !fn) return false;
  if (conduit_ == arch_aarch64::PsciConduit::None) return false;
  auto* stack = static_cast<uint8_t*>(allocator_->allocate_aligned(kSecondaryStackBytes, 16));
  if (!stack) return false;
  SecondaryContext& c = contexts_[core];
  read_translation_setup(&c);
  c.stack_top = reinterpret_cast<uint64_t>(stack) + kSecondaryStackBytes;
  c.function = fn;
  c.argument = context;
  clean_to_memory(&c, sizeof c);
  if (!arch_aarch64::psci_cpu_on(conduit_, mpidr_[core], entry_address(), reinterpret_cast<uint64_t>(&c))) {
    allocator_->deallocate(stack);
    return false;
  }
  started_[core] = true;
  return true;
}

}  // namespace hardware

// Where Secondary.S hands over, with the MMU on and a stack.
extern "C" [[noreturn]] void tcos_secondary_main(hardware::SecondaryContext* context) {
  context->function(context->argument);
  for (;;) __asm__ volatile("wfi");
}
