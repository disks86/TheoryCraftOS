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

// AMD's ATPX method (PowerXpress), after Linux amdgpu_atpx_handler.c (Copyright Advanced Micro Devices, Inc.; MIT license). ATPX(Arg0
// function, Arg1 buffer) sits on a GPU's Device (usually the integrated one's); every argument and answer is a buffer that starts with its
// own size as a 16-bit word:
//   0 VERIFY_INTERFACE     -> { u16 size, u16 version, u32 supported functions }
//   1 GET_PX_PARAMETERS    -> { u16 size, u32 valid flags, u32 flags }
//   2 POWER_CONTROL        <- { u16 size = 3, u8 state }      0 off, 1 on (200 ms to settle after off)
//   3 DISPLAY_MUX_CONTROL  <- { u16 size = 4, u16 mux }       0 integrated, 1 discrete; also 4 (I2C/DDC mux), 5 and 6 (switch start / end)
#pragma once
#include <stdint.h>
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPlatformPower.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/Optional.hpp"

namespace hardware {

struct AtpxVerifyInterface {
  uint16_t version = 0;
  uint32_t functions = 0;
};

struct AtpxPxParameters {
  uint32_t valid_flags = 0;
  uint32_t flags = 0;
};

// What ATPX can do, after amdgpu_atpx_validate().
struct AmdAtpxInfo {
  AtpxVerifyInterface verify;
  Optional<AtpxPxParameters> parameters;
  uint32_t valid_bits = 0;               // flags & valid flags
  bool power_control = false;            // function 2 may be used
  bool display_mux = false, i2c_mux = false, switch_start = false, switch_end = false;
  bool hybrid = false;                   // "MS hybrid graphics": the power goes through _PR3, ATPX power control is off
  bool dgpu_needs_power_for_displays = false;
};

class AmdAtpx final : public IGpuPlatformPower, public IGpuMux {
 public:
  static constexpr uint32_t kVerifyInterface = 0, kGetPxParameters = 1, kPowerControl = 2, kDisplayMuxControl = 3, kI2cMuxControl = 4;
  static constexpr uint32_t kSwitchStart = 5, kSwitchEnd = 6;
  // Function bits of VERIFY_INTERFACE.
  static constexpr uint32_t kPxParametersSupported = 1u << 0, kPowerControlSupported = 1u << 1, kDisplayMuxSupported = 1u << 2;
  static constexpr uint32_t kI2cMuxSupported = 1u << 3, kSwitchStartSupported = 1u << 4, kSwitchEndSupported = 1u << 5;
  // Flags of GET_PX_PARAMETERS.
  static constexpr uint32_t kCrt1RgbSignalMuxed = 1u << 3, kTvSignalMuxed = 1u << 4, kDfpSignalMuxed = 1u << 5, kSeparateMuxForI2c = 1u << 6;
  static constexpr uint32_t kDynamicPxSupported = 1u << 7, kDynamicDgpuPowerOffSupported = 1u << 10, kDgpuRequiresPowerForDisplays = 1u << 11;
  static constexpr uint32_t kMsHybridGraphicsSupported = 1u << 14;
  static constexpr uint32_t kPowerOffSettleMs = 200;
  static constexpr uint16_t kMuxIntegrated = 0, kMuxDiscrete = 1;

  using SleepFn = void (*)(uint32_t milliseconds);

  // The answers, from the firmware: every size is checked against the buffer. Empty if the buffer is shorter than the structure or its size
  // word says so (amdgpu: "ATPX buffer is too small").
  [[nodiscard]] static Optional<AtpxVerifyInterface> parse_verify_interface(ConstByteView answer);
  [[nodiscard]] static Optional<AtpxPxParameters> parse_px_parameters(ConstByteView answer);
  // amdgpu_atpx_validate(): which functions to use, from the two answers.
  [[nodiscard]] static AmdAtpxInfo derive(const AtpxVerifyInterface& verify, const Optional<AtpxPxParameters>& parameters);
  // Calls functions 0 and 1 of `method` (an ATPX method). Empty if the method faulted or answered garbage. Takes the lock.
  [[nodiscard]] static Optional<AmdAtpxInfo> read(acpi::AmlInterpreter& aml, acpi::Node* method);
  // Calls `function` with `input` (empty: the integer 0, as amdgpu passes no parameters). False if the method faulted. Takes the lock.
  [[nodiscard]] static bool call(acpi::AmlInterpreter& aml, acpi::Node* method, uint32_t function, ConstByteView input);

  // `power_on_delay_ms`: the D3cold -> D0 wait (the PCI firmware _DSM's readiness durations, see AcpiPciReadiness), 100 ms by default.
  AmdAtpx(acpi::AmlInterpreter& aml, acpi::Node* method, const AmdAtpxInfo& info, SleepFn sleep_ms = nullptr, uint32_t power_on_delay_ms = 100)
      : aml_(aml), method_(method), info_(info), sleep_ms_(sleep_ms), power_on_delay_ms_(power_on_delay_ms) {}

  GpuPowerMechanism mechanism() const override { return GpuPowerMechanism::AmdAtpx; }
  // POWER_CONTROL; false where ATPX may not be used for power (info().power_control).
  [[nodiscard]] bool power_off() override;
  [[nodiscard]] bool power_on() override;
  uint32_t power_on_delay_ms() const override { return power_on_delay_ms_; }

  [[nodiscard]] bool active(GpuMuxTarget* out) override;
  // Switch start (where supported), display mux, I2C mux (where supported), switch end, as amdgpu_atpx_switchto().
  [[nodiscard]] bool switch_to(GpuMuxTarget target) override;
  [[nodiscard]] bool switch_ddc(GpuMuxTarget target) override;

  const AmdAtpxInfo& info() const { return info_; }

 private:
  [[nodiscard]] bool power(uint8_t state);
  [[nodiscard]] bool mux(uint32_t function, GpuMuxTarget target);

  acpi::AmlInterpreter& aml_;
  acpi::Node* method_;
  AmdAtpxInfo info_;
  SleepFn sleep_ms_;
  uint32_t power_on_delay_ms_;
  Optional<GpuMuxTarget> active_;
};

}  // namespace hardware
