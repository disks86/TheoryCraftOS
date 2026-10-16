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

#include "Hardware/CentralProcessingUnit/X86_64Cpu.hpp"
#include "Hardware/CentralProcessingUnit/Cpuid.hpp"

// Implemented in X86_64Cpu.S.
extern "C" {
void tcos_halt();
void tcos_pause();
void tcos_breakpoint();
void tcos_disable_interrupts();
void tcos_enable_interrupts();
uint64_t tcos_rdtsc();
}

namespace hardware {

void X86_64Cpu::halt() const { tcos_halt(); }
void X86_64Cpu::pause() const { tcos_pause(); }
void X86_64Cpu::breakpoint() const { tcos_breakpoint(); }
void X86_64Cpu::disable_interrupts() const { tcos_disable_interrupts(); }
void X86_64Cpu::enable_interrupts() const { tcos_enable_interrupts(); }
uint64_t X86_64Cpu::timestamp_counter() const { return tcos_rdtsc(); }

bool X86_64Cpu::initialize() {
  // Brand string: leaves 0x80000002-4 hold 48 ASCII bytes.
  if (cpuid(0x80000000).eax >= 0x80000004) {
    uint32_t words[12];
    for (uint32_t i = 0; i < 3; ++i) {
      CpuidResult r = cpuid(0x80000002 + i);
      words[i * 4 + 0] = r.eax;
      words[i * 4 + 1] = r.ebx;
      words[i * 4 + 2] = r.ecx;
      words[i * 4 + 3] = r.edx;
    }
    const char* src = reinterpret_cast<const char*>(words);
    uint32_t out = 0;
    for (uint32_t i = 0; i < 48 && src[i]; ++i)
      if (out || src[i] != ' ') model_[out++] = src[i];  // skip leading padding
    while (out && model_[out - 1] == ' ') --out;  // trim trailing padding
    model_[out] = '\0';
  }

  cores_ = 1;
  usable_[0] = true;
  kinds_[0] = probe_core_kind();

  efi::BootServices* bs = st_ ? st_->boot_services : nullptr;
  if (!bs || bs->locate_protocol(&efi::kMpServicesGuid, nullptr,
                                 reinterpret_cast<void**>(&mp_)) != efi::kSuccess) {
    mp_ = nullptr;
    return true;  // single core: still a working CPU
  }

  efi::Uintn total = 0, enabled = 0;
  if (mp_->get_number_of_processors(mp_, &total, &enabled) != efi::kSuccess || total == 0) {
    mp_ = nullptr;
    return true;
  }
  cores_ = total < kMaxCores ? static_cast<uint32_t>(total) : kMaxCores;

  for (uint32_t i = 0; i < cores_; ++i) {
    efi::ProcessorInformation info{};
    usable_[i] = mp_->get_processor_info(mp_, i, &info) == efi::kSuccess &&
                 (info.status_flag & efi::kProcessorEnabled) &&
                 (info.status_flag & efi::kProcessorHealthy);
  }
  usable_[current_core()] = true;

  // Each core reads its own CPUID (hybrid CPUs differ per core). Blocking call.
  if (cores_ > 1) mp_->startup_all_aps(mp_, probe_entry, 0, nullptr, 0, this, nullptr);
  return true;
}

void X86_64Cpu::shutdown() {
  efi::BootServices* bs = st_ ? st_->boot_services : nullptr;
  for (uint32_t i = 0; i < kMaxCores; ++i) {
    if (events_[i] && bs) bs->close_event(events_[i]);
    events_[i] = nullptr;
    busy_[i] = false;
  }
}

uint32_t X86_64Cpu::current_core() const {
  efi::Uintn id = 0;
  if (mp_ && mp_->who_am_i(mp_, &id) == efi::kSuccess && id < cores_)
    return static_cast<uint32_t>(id);
  return 0;
}

CoreKind X86_64Cpu::core_kind(uint32_t core) const {
  return core < cores_ ? kinds_[core] : CoreKind::Performance;
}

void X86_64Cpu::probe_entry(void* self) {
  auto* cpu = static_cast<X86_64Cpu*>(self);
  uint32_t core = cpu->current_core();
  cpu->kinds_[core] = cpu->probe_core_kind();
}

void X86_64Cpu::run_slot(void* slot) {
  auto* s = static_cast<Slot*>(slot);
  s->fn(s->context);
}

bool X86_64Cpu::start_on_core(uint32_t core, CoreFunction fn, void* context) {
  if (!mp_ || !fn || core >= cores_ || !usable_[core] || core == current_core()) return false;
  if (core_busy(core)) return false;

  efi::BootServices* bs = st_->boot_services;
  if (!events_[core] &&
      bs->create_event(0, 0, nullptr, nullptr, &events_[core]) != efi::kSuccess)
    return false;

  slots_[core] = {fn, context};
  busy_[core] = true;
  // A non-null event makes the call non-blocking; it is signaled on completion.
  if (mp_->startup_this_ap(mp_, run_slot, core, events_[core], 0, &slots_[core], nullptr) !=
      efi::kSuccess) {
    busy_[core] = false;
    return false;
  }
  return true;
}

bool X86_64Cpu::core_busy(uint32_t core) {
  if (core >= cores_ || !busy_[core]) return false;
  if (st_->boot_services->check_event(events_[core]) == efi::kSuccess) busy_[core] = false;
  return busy_[core];
}

}  // namespace hardware
