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

#include "Arch/LoongArch64/Cpu.hpp"
#include "Arch/LoongArch64/Csr.hpp"
#include "Utility/ByteStream.hpp"

extern "C" void tcos_secondary_entry();

namespace hardware {

namespace {
constexpr uint32_t kMadtEntries = 44;
constexpr uint8_t kTypeCorePic = 0x11;     // LoongArch Core Interrupt Controller (one per core)
constexpr uint32_t kCorePicLength = 15;

// The entry address is code, handed to the parked core as a number.
uint64_t entry_address() { return reinterpret_cast<uint64_t>(&tcos_secondary_entry); }
}  // namespace

uint64_t LoongArch64Cpu::timestamp_counter() const { return arch_loongarch64::read_counter(); }

bool LoongArch64Cpu::discover(const AcpiTables& acpi, allocators::IAllocator& allocator) {
  return parse_madt(acpi.find("APIC"), allocator);
}

bool LoongArch64Cpu::parse_madt(ConstByteView madt, allocators::IAllocator& allocator) {
  allocator_ = &allocator;
  core_id_[0] = arch_loongarch64::this_core_id();
  count_ = 1;
  started_[0] = true;
  // The parked cores come up with the reset memory access types and the boot core's addresses must mean the same to
  // them, so starting them is limited to direct address translation (UEFI's mode).
  can_start_ = (arch_loongarch64::csr_read<arch_loongarch64::kCsrCrmd>() & (arch_loongarch64::kCrmdDa | arch_loongarch64::kCrmdPg)) == arch_loongarch64::kCrmdDa;
  if (madt.size() < kMadtEntries) return false;
  bool saw_boot_core = false;
  uint32_t others[kMaxCores] = {};
  uint32_t other_count = 0;
  for (uint32_t offset = kMadtEntries; offset + 2 <= madt.size();) {
    const uint8_t type = madt[offset], length = madt[offset + 1];
    if (length < 2 || length > madt.size() - offset) break;
    if (type == kTypeCorePic && length >= kCorePicLength) {
      ByteReader r(madt.subview(offset, length));
      r.skip(2 + 1);                         // header, version
      r.skip(4);                             // ACPI processor id
      const uint32_t core = r.le32();
      const uint32_t flags = r.le32();
      if (r.ok() && (flags & 1)) {
        if (core == core_id_[0]) saw_boot_core = true;
        else if (other_count < kMaxCores - 1) others[other_count++] = core;
      }
    }
    offset += length;
  }
  if (saw_boot_core) {
    for (uint32_t i = 0; i < other_count; ++i) core_id_[count_++] = others[i];
  }
  return true;
}

bool LoongArch64Cpu::start_on_core(uint32_t core, CoreFunction fn, void* context) {
  if (core == 0 || core >= count_ || started_[core] || !allocator_ || !fn || !can_start_) return false;
  auto* stack = static_cast<uint8_t*>(allocator_->allocate_aligned(kSecondaryStackBytes, 16));
  if (!stack) return false;
  SecondaryContext& c = contexts_[core];
  c.crmd = arch_loongarch64::csr_read<arch_loongarch64::kCsrCrmd>() & ~arch_loongarch64::kCrmdIe;
  c.stack_top = reinterpret_cast<uint64_t>(stack) + kSecondaryStackBytes;
  c.function = fn;
  c.argument = context;
  __builtin_loongarch_dbar(0);   // the context is visible before the core that reads it starts
  arch_loongarch64::mail_send(core_id_[core], 0, entry_address());
  arch_loongarch64::mail_send(core_id_[core], 1, reinterpret_cast<uint64_t>(&c));
  arch_loongarch64::send_ipi(core_id_[core], 0);
  started_[core] = true;
  return true;
}

}  // namespace hardware

// Where Secondary.S hands over, with a stack.
extern "C" [[noreturn]] void tcos_secondary_main(hardware::SecondaryContext* context) {
  context->function(context->argument);
  for (;;) __asm__ volatile("idle 0");
}
