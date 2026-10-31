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

#include "Hardware/Power/CpuPower.hpp"
#include "Hardware/CentralProcessingUnit/Cpuid.hpp"
#include "Kernel/Thread.hpp"

namespace hardware {

using namespace cpu_msr;

// ---- the real registers ---------------------------------------------------------------------------------------------------------------------------------------------------------

bool HostMsr::read(uint32_t msr, uint64_t* value) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  *value = (uint64_t{high} << 32) | low;
  return true;
}

bool HostMsr::write(uint32_t msr, uint64_t value) {
  __asm__ volatile("wrmsr" ::"c"(msr), "a"(static_cast<uint32_t>(value)), "d"(static_cast<uint32_t>(value >> 32)) : "memory");
  return true;
}

namespace {
struct PerCoreWrite {
  uint32_t msr;
  uint64_t value;
};
void write_here(void* argument) {
  const PerCoreWrite* w = static_cast<PerCoreWrite*>(argument);
  __asm__ volatile("wrmsr" ::"c"(w->msr), "a"(static_cast<uint32_t>(w->value)), "d"(static_cast<uint32_t>(w->value >> 32)) : "memory");
}
}  // namespace

bool HostMsr::write_every_core(uint32_t msr, uint64_t value) {
  PerCoreWrite w{msr, value};
  return kernel::run_on_every_core(write_here, &w);
}

// ---- what the processor can do ---------------------------------------------------------------------------------------------------------------------------------------------------

const char* power_profile_name(PowerProfile profile) {
  switch (profile) {
    case PowerProfile::Performance: return "Performance";
    case PowerProfile::Balanced: return "Balanced";
    case PowerProfile::PowerSaver: return "Power saver";
  }
  return "";
}

CpuPowerCaps probe_cpu_power_caps() {
  CpuPowerCaps caps;
  const CpuidResult vendor = cpuid(0);
  caps.intel = vendor.ebx == 0x756E6547 && vendor.edx == 0x49656E69 && vendor.ecx == 0x6C65746E;
  const CpuidResult one = cpuid(1);
  caps.virtual_machine = one.ecx & (1u << 31);
  if (!caps.intel || vendor.eax < 6) return caps;
  caps.eist = one.ecx & (1u << 7);
  caps.mwait = one.ecx & (1u << 3);
  const CpuidResult thermal = cpuid(6);
  caps.always_running_timer = thermal.eax & (1u << 2);
  caps.hwp = thermal.eax & (1u << 7);
  caps.hwp_epp = thermal.eax & (1u << 10);
  caps.hwp_package_request = thermal.eax & (1u << 11);
  caps.energy_bias = thermal.ecx & (1u << 3);
  if (caps.mwait && vendor.eax >= 5) {
    const CpuidResult m = cpuid(5);
    caps.mwait_interrupt_break = (m.ecx & 3) == 3;                  // the extensions are enumerated, and an interrupt ends the wait with the flag masked
    for (uint32_t i = 0; i < 8; ++i) caps.mwait_substates[i] = static_cast<uint8_t>((m.edx >> (4 * (i + 1))) & 0xF);   // (type 0 is the nibble above C0's)
  }
  return caps;
}

uint32_t CpuPower::idle_hint(const CpuPowerCaps& caps, PowerProfile profile) {
  if (!caps.mwait || !caps.mwait_interrupt_break || caps.virtual_machine) return 0xFFFFFFFFu;
  uint32_t deepest = 0;
  for (uint32_t i = 0; i < 8; ++i)
    if (caps.mwait_substates[i]) deepest = i;
  uint32_t type = 0;
  if (caps.always_running_timer) {                                  // deeper states stop the timer of the core unless it has ARAT
    switch (profile) {
      case PowerProfile::Performance: type = 0; break;
      case PowerProfile::Balanced: type = deepest < 2 ? deepest : 2; break;      // C6 on the mobile parts
      case PowerProfile::PowerSaver: type = deepest; break;
    }
  }
  return type << 4;                                                 // sub-state 0 (the shallowest of the type)
}

// ---- speed -----------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool CpuPower::initialize(const CpuPowerCaps& caps) {
  caps_ = caps;
  mode_ = Mode::None;
  if (!caps.intel || caps.virtual_machine) return false;
  if (caps.hwp) {
    uint64_t cap;
    if (!msr_.read(kHwpCapabilities, &cap)) return false;
    highest_ = cap & 0xFF;
    guaranteed_ = (cap >> 8) & 0xFF;
    efficient_ = (cap >> 16) & 0xFF;
    lowest_ = (cap >> 24) & 0xFF;
    if (highest_ == 0 || lowest_ == 0 || lowest_ > highest_) return false;          // not a range worth believing
    if (guaranteed_ < lowest_ || guaranteed_ > highest_) guaranteed_ = highest_;
    if (efficient_ < lowest_ || efficient_ > highest_) efficient_ = lowest_;
    if (!msr_.write(kPmEnable, 1)) return false;
    mode_ = Mode::Hwp;
    // every core follows the package request (the per-core request with the package control bit; its other fields are ignored then)
    if (caps.hwp_package_request && !msr_.write_every_core(kHwpRequest, kHwpPackageControl | (uint64_t{highest_} << 8) | lowest_)) {
      mode_ = Mode::None;
      return false;
    }
    return apply();
  }
  if (caps.eist) {
    uint64_t info;
    if (!msr_.read(kPlatformInfo, &info)) return false;
    max_ratio_ = (info >> 8) & 0xFF;
    min_ratio_ = (info >> 40) & 0xFF;
    turbo_ratio_ = max_ratio_;
    uint64_t turbo;
    if (msr_.read(kTurboRatioLimit, &turbo) && (turbo & 0xFF) > max_ratio_ && (turbo & 0xFF) < 128) turbo_ratio_ = turbo & 0xFF;
    if (max_ratio_ == 0 || min_ratio_ == 0 || min_ratio_ > max_ratio_) return false;
    lowest_ = min_ratio_;
    guaranteed_ = efficient_ = max_ratio_;
    highest_ = turbo_ratio_;
    mode_ = Mode::SpeedStep;
    return apply();
  }
  return false;
}

bool CpuPower::apply() {
  if (mode_ == Mode::None) return false;
  // the range the profile allows, and where in it the ceiling cuts
  uint32_t low = lowest_, high = highest_, preference = 0x80;
  switch (profile_) {
    case PowerProfile::Performance: low = guaranteed_; high = highest_; preference = 0x00; break;
    case PowerProfile::Balanced: low = lowest_; high = highest_; preference = 0x80; break;
    case PowerProfile::PowerSaver: low = lowest_; high = guaranteed_; preference = 0xFF; break;
  }
  if (high < low) high = low;
  const uint32_t cut = low + (high - low) * ceiling_ / 100;
  const uint32_t ceiling = cut < low ? low : cut;
  if (mode_ == Mode::Hwp) {
    // minimum | maximum << 8 | desired (0: the processor decides) << 16 | preference << 24
    uint64_t request = uint64_t{low > ceiling ? ceiling : low} | (uint64_t{ceiling} << 8) | (caps_.hwp_epp ? uint64_t{preference} << 24 : 0);
    if (caps_.hwp_package_request) return msr_.write(kHwpRequestPkg, request);
    return msr_.write_every_core(kHwpRequest, request);
  }
  // SpeedStep: the ratio to run at, and the bias on every core
  uint32_t ratio = profile_ == PowerProfile::Performance ? ceiling : profile_ == PowerProfile::Balanced ? (ceiling < max_ratio_ ? ceiling : max_ratio_) : (ceiling < min_ratio_ ? ceiling : min_ratio_);
  if (profile_ == PowerProfile::PowerSaver && ceiling > min_ratio_) ratio = min_ratio_;
  if (ratio < min_ratio_) ratio = min_ratio_;
  bool ok = msr_.write_every_core(kPerfCtl, uint64_t{ratio} << 8);
  if (caps_.energy_bias) ok = msr_.write_every_core(kEnergyPerfBias, profile_ == PowerProfile::Performance ? 0 : profile_ == PowerProfile::Balanced ? 6 : 15) && ok;
  return ok;
}

bool CpuPower::set_profile(PowerProfile profile) {
  profile_ = profile;
  return apply();
}

bool CpuPower::set_ceiling_percent(uint32_t percent) {
  ceiling_ = percent < 1 ? 1 : percent > 100 ? 100 : percent;
  return apply();
}

uint32_t CpuPower::current_ratio() {
  uint64_t status;
  if (mode_ == Mode::None || !msr_.read(kPerfStatus, &status)) return 0;
  return (status >> 8) & 0xFF;
}

}  // namespace hardware
