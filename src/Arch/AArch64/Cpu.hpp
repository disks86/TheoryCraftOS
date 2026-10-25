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
#include "Arch/AArch64/Psci.hpp"
#include "Hardware/Acpi/AcpiTables.hpp"
#include "Hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"

namespace hardware {

// What a secondary core needs to join: the boot core's translation setup (so it sees the same memory the same
// way), its stack and the function to run. Secondary.S reads the first six fields by offset.
struct alignas(64) SecondaryContext {
  uint64_t ttbr0, tcr, mair, sctlr, hcr;   // 0, 8, 16, 24, 32
  uint64_t stack_top;                      // 40
  CoreFunction function;
  void* argument;
};

// The AArch64 processor as the scheduler sees it. Core 0 is the boot core; the others are the enabled GICC entries
// of the ACPI MADT, in table order, started with PSCI CPU_ON.
class AArch64Cpu final : public ICentralProcessingUnit {
 public:
  static constexpr uint32_t kMaxCores = 128;

  AArch64Cpu() = default;

  // Reads the core list from the MADT and the PSCI conduit from the FADT. Without PSCI only the boot core is usable.
  bool discover(const AcpiTables& acpi, allocators::IAllocator& allocator);

  const char* name() const override { return "AArch64 CPU"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  const char* model() const override { return "AArch64"; }
  uint32_t core_count() const override { return count_; }
  uint32_t current_core() const override { return 0; }   // only the boot core calls this (the scheduler's start)
  CoreKind core_kind(uint32_t) const override { return CoreKind::Performance; }
  bool start_on_core(uint32_t core, CoreFunction fn, void* context) override;
  bool core_busy(uint32_t core) override { return core < count_ && started_[core]; }

  void halt() const override { __asm__ volatile("wfi"); }
  void pause() const override { __asm__ volatile("yield"); }
  void breakpoint() const override { __asm__ volatile("brk #0"); }
  void disable_interrupts() const override { __asm__ volatile("msr daifset, #2" ::: "memory"); }
  void enable_interrupts() const override { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
  uint64_t timestamp_counter() const override {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
  }

 private:
  static constexpr uint32_t kSecondaryStackBytes = 64 * 1024;

  uint32_t count_ = 1;
  arch_aarch64::PsciConduit conduit_ = arch_aarch64::PsciConduit::None;
  allocators::IAllocator* allocator_ = nullptr;
  uint64_t mpidr_[kMaxCores] = {};
  bool started_[kMaxCores] = {};
  SecondaryContext contexts_[kMaxCores] = {};
};

}  // namespace hardware
