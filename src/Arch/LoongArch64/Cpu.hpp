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

#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Acpi/AcpiTables.hpp"
#include "Hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"

namespace hardware {

// What another core needs to join: the boot core's CRMD (so it gets the same memory access types), its stack and the
// function to run. Secondary.S reads the first two fields by offset.
struct alignas(64) SecondaryContext {
  uint64_t crmd;         // 0
  uint64_t stack_top;    // 8
  CoreFunction function;
  void* argument;
};

// The LoongArch processor as the scheduler sees it. Core 0 is the boot core; the others are the enabled CORE_PIC
// entries of the ACPI MADT, in table order, started through the IOCSR mailbox and an IPI (the protocol the firmware's
// parked cores wait for). That needs the boot core to run with direct address translation (CRMD.DA, physical = virtual),
// which is how UEFI hands over; with paging on only the boot core is used.
class LoongArch64Cpu final : public ICentralProcessingUnit {
 public:
  static constexpr uint32_t kMaxCores = 128;

  LoongArch64Cpu() = default;

  // Reads the core list from the MADT.
  bool discover(const AcpiTables& acpi, allocators::IAllocator& allocator);
  // The same from the bytes of a MADT (the table with its 36-byte header).
  bool parse_madt(ConstByteView madt, allocators::IAllocator& allocator);

  const char* name() const override { return "LoongArch CPU"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  const char* model() const override { return "LoongArch"; }
  uint32_t core_count() const override { return count_; }
  uint32_t current_core() const override { return 0; }   // only the boot core calls this (the scheduler's start)
  CoreKind core_kind(uint32_t) const override { return CoreKind::Performance; }
  bool start_on_core(uint32_t core, CoreFunction fn, void* context) override;
  bool core_busy(uint32_t core) override { return core < count_ && started_[core]; }

  void halt() const override { __asm__ volatile("idle 0"); }
  void pause() const override { __asm__ volatile("nop"); }
  void breakpoint() const override { __asm__ volatile("break 0"); }
  void disable_interrupts() const override { (void)__builtin_loongarch_csrxchg_d(0, 1u << 2, 0x0); }
  void enable_interrupts() const override { (void)__builtin_loongarch_csrxchg_d(1u << 2, 1u << 2, 0x0); }
  uint64_t timestamp_counter() const override;

 private:
  static constexpr uint32_t kSecondaryStackBytes = 64 * 1024;

  uint32_t count_ = 1;
  bool can_start_ = false;
  allocators::IAllocator* allocator_ = nullptr;
  uint32_t core_id_[kMaxCores] = {};
  bool started_[kMaxCores] = {};
  SecondaryContext contexts_[kMaxCores] = {};
};

}  // namespace hardware
