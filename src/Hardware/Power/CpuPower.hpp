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

// The processor's own power controls on Intel parts: how fast it runs and how it waits.
//
// Speed. Hardware P-states (HWP, Skylake and later): the OS enables HWP once (IA32_PM_ENABLE) and gives the processor a range and a preference (minimum, maximum, and the energy
// performance preference 0 = performance .. 255 = efficiency) in IA32_HWP_REQUEST; the processor picks the frequency inside it. Each core's request is told to follow the package
// request (bit 42 "package control"), so the policy is one write to IA32_HWP_REQUEST_PKG whichever core makes it. Without HWP but with Enhanced SpeedStep the ratio is set per core in
// IA32_PERF_CTL (written on every core) from the range in MSR_PLATFORM_INFO and the energy bias in IA32_ENERGY_PERF_BIAS.
//
// Waiting. An idle core asks for a C-state with MWAIT; the hint comes from the leaf 5 sub-state counts. Where the local APIC timer stops in deep states (no ARAT) only the first state
// is used.
//
// A thermal ceiling (a percentage of the range between the lowest and the highest speed the profile allows) can be put on top of the profile. The registers are reached through
// IMsrAccess so the logic runs in host tests; HostMsr is the real thing, to be used only for MSRs the processor has (the capabilities say which).
#pragma once
#include <stdint.h>

namespace hardware {

class IMsrAccess {
 public:
  virtual bool read(uint32_t msr, uint64_t* value) = 0;
  virtual bool write(uint32_t msr, uint64_t value) = 0;
  // Writes the same register on every core (the per-core ones).
  virtual bool write_every_core(uint32_t msr, uint64_t value) = 0;

 protected:
  constexpr IMsrAccess() = default;
  ~IMsrAccess() = default;
};

// RDMSR / WRMSR on this machine; the scheduler's run_on_every_core for the per-core registers.
class HostMsr final : public IMsrAccess {
 public:
  bool read(uint32_t msr, uint64_t* value) override;
  bool write(uint32_t msr, uint64_t value) override;
  bool write_every_core(uint32_t msr, uint64_t value) override;
};

enum class PowerProfile : uint8_t { Performance, Balanced, PowerSaver };
const char* power_profile_name(PowerProfile profile);

struct CpuPowerCaps {
  bool intel = false;
  bool hwp = false, hwp_epp = false, hwp_package_request = false;
  bool eist = false, energy_bias = false;
  bool mwait = false, mwait_interrupt_break = false, always_running_timer = false;
  uint8_t mwait_substates[8] = {};      // number of sub-states of each MWAIT C-state type (leaf 5 EDX), type 0 = C1
  bool virtual_machine = false;
};
CpuPowerCaps probe_cpu_power_caps();

class CpuPower {
 public:
  explicit CpuPower(IMsrAccess& msr) : msr_(msr) {}

  // Finds out what the processor can be told (HWP range, or the SpeedStep ratios) and enables it. False if there is nothing to control (a virtual machine, another vendor).
  bool initialize(const CpuPowerCaps& caps);
  bool speed_control() const { return mode_ != Mode::None; }
  bool uses_hwp() const { return mode_ == Mode::Hwp; }
  // The profile, and the ceiling (percent of the profile's range, 1..100). Both can change at any time.
  bool set_profile(PowerProfile profile);
  bool set_ceiling_percent(uint32_t percent);
  PowerProfile profile() const { return profile_; }
  uint32_t ceiling_percent() const { return ceiling_; }
  // The range in processor ratios (multiples of 100 MHz) HWP reported: lowest, most efficient, guaranteed, highest.
  uint32_t lowest_ratio() const { return lowest_; }
  uint32_t efficient_ratio() const { return efficient_; }
  uint32_t guaranteed_ratio() const { return guaranteed_; }
  uint32_t highest_ratio() const { return highest_; }
  // The ratio the processor reports running at now (IA32_PERF_STATUS), 0 if unknown.
  uint32_t current_ratio();

  // The MWAIT hint for an idle core under the profile, 0xFFFFFFFF when MWAIT cannot be used. Applied through kernel::set_idle_mwait_hint by the caller.
  static uint32_t idle_hint(const CpuPowerCaps& caps, PowerProfile profile);
  uint32_t idle_hint(PowerProfile profile) const { return idle_hint(caps_, profile); }

 private:
  enum class Mode : uint8_t { None, Hwp, SpeedStep };
  bool apply();
  IMsrAccess& msr_;
  CpuPowerCaps caps_;
  Mode mode_ = Mode::None;
  PowerProfile profile_ = PowerProfile::Balanced;
  uint32_t ceiling_ = 100;
  uint32_t lowest_ = 0, efficient_ = 0, guaranteed_ = 0, highest_ = 0;
  uint32_t min_ratio_ = 0, max_ratio_ = 0, turbo_ratio_ = 0;      // SpeedStep
};

namespace cpu_msr {
constexpr uint32_t kPlatformInfo = 0xCE, kPerfStatus = 0x198, kPerfCtl = 0x199, kEnergyPerfBias = 0x1B0, kTurboRatioLimit = 0x1AD;
constexpr uint32_t kPmEnable = 0x770, kHwpCapabilities = 0x771, kHwpRequestPkg = 0x772, kHwpRequest = 0x774;
constexpr uint64_t kHwpPackageControl = 1ull << 42;
}  // namespace cpu_msr

}  // namespace hardware
