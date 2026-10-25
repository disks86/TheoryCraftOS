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

// What another hart needs to join: the boot hart's address translation (so it sees memory the same way), its stack
// and the function to run. Secondary.S reads the first two fields by offset.
struct alignas(64) SecondaryContext {
  uint64_t satp;         // 0
  uint64_t stack_top;    // 8
  CoreFunction function;
  void* argument;
};

// The RISC-V processor as the scheduler sees it. Core 0 is the boot hart; the others are the enabled RINTC entries of
// the ACPI MADT, in table order, started with SBI hart_start.
class RiscV64Cpu final : public ICentralProcessingUnit {
 public:
  static constexpr uint32_t kMaxCores = 128;

  RiscV64Cpu() = default;

  // Reads the hart list from the MADT. The boot hart id must be set (arch_riscv64::set_boot_hart_id) first; without it
  // only the boot hart is used.
  bool discover(const AcpiTables& acpi, allocators::IAllocator& allocator);

  const char* name() const override { return "RISC-V CPU"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  const char* model() const override { return "RISC-V"; }
  uint32_t core_count() const override { return count_; }
  uint32_t current_core() const override { return 0; }   // only the boot hart calls this (the scheduler's start)
  CoreKind core_kind(uint32_t) const override { return CoreKind::Performance; }
  bool start_on_core(uint32_t core, CoreFunction fn, void* context) override;
  bool core_busy(uint32_t core) override { return core < count_ && started_[core]; }

  void halt() const override { __asm__ volatile("wfi"); }
  void pause() const override { __asm__ volatile(".4byte 0x0100000F"); }
  void breakpoint() const override { __asm__ volatile("ebreak"); }
  void disable_interrupts() const override { __asm__ volatile("csrci sstatus, 2" ::: "memory"); }
  void enable_interrupts() const override { __asm__ volatile("csrsi sstatus, 2" ::: "memory"); }
  uint64_t timestamp_counter() const override {
    uint64_t v;
    __asm__ volatile("rdtime %0" : "=r"(v)::"memory");
    return v;
  }

 private:
  static constexpr uint32_t kSecondaryStackBytes = 64 * 1024;

  uint32_t count_ = 1;
  bool can_start_ = false;
  allocators::IAllocator* allocator_ = nullptr;
  uint64_t hart_[kMaxCores] = {};
  bool started_[kMaxCores] = {};
  SecondaryContext contexts_[kMaxCores] = {};
};

}  // namespace hardware
