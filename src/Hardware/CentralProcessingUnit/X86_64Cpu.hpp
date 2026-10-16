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

#pragma once
#include "Hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"
#include "uefi.hpp"

namespace hardware {

// Behavior shared by every x86_64 CPU: instructions, CPUID, and multi-core
// through the UEFI MP services (available until boot services exit). Vendor
// subclasses refine per-core probing. Used directly for unrecognized vendors.
class X86_64Cpu : public ICentralProcessingUnit {
 public:
  static constexpr uint32_t kMaxCores = 128;

  explicit X86_64Cpu(efi::SystemTable* st) : st_(st) {}

  const char* name() const override { return "x86_64 CPU"; }
  const char* model() const override { return model_; }
  bool initialize() override;
  void shutdown() override;

  void halt() const override;
  void pause() const override;
  void breakpoint() const override;
  void disable_interrupts() const override;
  void enable_interrupts() const override;
  uint64_t timestamp_counter() const override;

  uint32_t core_count() const override { return cores_; }
  uint32_t current_core() const override;
  CoreKind core_kind(uint32_t core) const override;
  bool start_on_core(uint32_t core, CoreFunction fn, void* context) override;
  bool core_busy(uint32_t core) override;

 protected:
  // Runs on the core being probed; vendors read their own CPUID leaves here.
  virtual CoreKind probe_core_kind() const { return CoreKind::Performance; }

 private:
  struct Slot {  // what an application processor executes
    CoreFunction fn;
    void* context;
  };

  static void probe_entry(void* self);
  static void run_slot(void* slot);

  efi::SystemTable* st_;
  efi::MpServices* mp_ = nullptr;
  uint32_t cores_ = 1;
  char model_[49] = "unknown x86_64";
  CoreKind kinds_[kMaxCores] = {};
  bool usable_[kMaxCores] = {};
  bool busy_[kMaxCores] = {};
  efi::Event events_[kMaxCores] = {};
  Slot slots_[kMaxCores] = {};
};

}  // namespace hardware
