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

#include "Arch/RiscV64/Cpu.hpp"
#include "Arch/RiscV64/Sbi.hpp"
#include "Utility/ByteStream.hpp"

extern "C" void tcos_secondary_entry();

namespace hardware {

namespace {
constexpr uint32_t kMadtEntries = 44;
constexpr uint8_t kTypeRintc = 0x18;       // RISC-V Interrupt Controller (one per hart)
constexpr uint32_t kRintcLength = 36;

uint64_t read_satp() {
  uint64_t v;
  __asm__ volatile("csrr %0, satp" : "=r"(v));
  return v;
}

// The entry address is code, handed to the firmware as a number.
uint64_t entry_address() { return reinterpret_cast<uint64_t>(&tcos_secondary_entry); }
}  // namespace

bool RiscV64Cpu::discover(const AcpiTables& acpi, allocators::IAllocator& allocator) {
  allocator_ = &allocator;
  hart_[0] = arch_riscv64::boot_hart_id();
  count_ = 1;
  started_[0] = true;
  can_start_ = arch_riscv64::sbi::probe(arch_riscv64::sbi::kExtHsm) && arch_riscv64::sbi::probe(arch_riscv64::sbi::kExtIpi);
  const ConstByteView madt = acpi.find("APIC");
  if (madt.size() < kMadtEntries) return false;
  bool saw_boot_hart = false;
  uint64_t others[kMaxCores] = {};
  uint32_t other_count = 0;
  for (uint32_t offset = kMadtEntries; offset + 2 <= madt.size();) {
    const uint8_t type = madt[offset], length = madt[offset + 1];
    if (length < 2 || length > madt.size() - offset) break;
    if (type == kTypeRintc && length >= kRintcLength) {
      ByteReader r(madt.subview(offset, length));
      r.skip(2 + 1 + 1);                     // header, version, reserved
      const uint32_t flags = r.le32();
      const uint64_t hart = r.le64();
      if (r.ok() && (flags & 1)) {
        if (hart == hart_[0]) saw_boot_hart = true;
        else if (other_count < kMaxCores - 1) others[other_count++] = hart;
      }
    }
    offset += length;
  }
  // The other harts are only usable if the boot hart's id is known (it is what tells them apart from this one).
  if (saw_boot_hart) {
    for (uint32_t i = 0; i < other_count; ++i) hart_[count_++] = others[i];
  }
  return true;
}

bool RiscV64Cpu::start_on_core(uint32_t core, CoreFunction fn, void* context) {
  if (core == 0 || core >= count_ || started_[core] || !allocator_ || !fn || !can_start_) return false;
  auto* stack = static_cast<uint8_t*>(allocator_->allocate_aligned(kSecondaryStackBytes, 16));
  if (!stack) return false;
  SecondaryContext& c = contexts_[core];
  c.satp = read_satp();
  c.stack_top = reinterpret_cast<uint64_t>(stack) + kSecondaryStackBytes;
  c.function = fn;
  c.argument = context;
  __asm__ volatile("fence rw, rw" ::: "memory");   // the context is visible before the hart that reads it starts
  if (!arch_riscv64::sbi::hart_start(hart_[core], entry_address(), reinterpret_cast<uint64_t>(&c))) {
    allocator_->deallocate(stack);
    return false;
  }
  started_[core] = true;
  return true;
}

}  // namespace hardware

// Where Secondary.S hands over, with a stack.
extern "C" [[noreturn]] void tcos_secondary_main(hardware::SecondaryContext* context) {
  context->function(context->argument);
  for (;;) __asm__ volatile("wfi");
}
