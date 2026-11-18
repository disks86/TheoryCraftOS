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

#include "Hardware/GraphicsProcessingUnit/Multi/Acpi/AmdAtpx.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/Acpi/GpuAcpiNamespace.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
constexpr uint16_t kVerifyBytes = 8, kParametersBytes = 10;
constexpr uint32_t kMaxInputBytes = 16;

// The answer's own size word, checked against the buffer and the structure; the reader then covers only the bytes the size says.
bool open_answer(ConstByteView answer, uint16_t needed, ByteReader* out) {
  ByteReader in(answer);
  const uint16_t size = in.le16();
  if (!in.ok() || size < needed || answer.size() < needed) return false;
  *out = ByteReader(answer.first(needed));
  (void)out->le16();
  return true;
}
}  // namespace

Optional<AtpxVerifyInterface> AmdAtpx::parse_verify_interface(ConstByteView answer) {
  ByteReader in;
  if (!open_answer(answer, kVerifyBytes, &in)) return {};
  AtpxVerifyInterface v;
  v.version = in.le16();
  v.functions = in.le32();
  if (!in.ok()) return {};
  return v;
}

Optional<AtpxPxParameters> AmdAtpx::parse_px_parameters(ConstByteView answer) {
  ByteReader in;
  if (!open_answer(answer, kParametersBytes, &in)) return {};
  AtpxPxParameters p;
  p.valid_flags = in.le32();
  p.flags = in.le32();
  if (!in.ok()) return {};
  return p;
}

AmdAtpxInfo AmdAtpx::derive(const AtpxVerifyInterface& verify, const Optional<AtpxPxParameters>& parameters) {
  AmdAtpxInfo info;
  info.verify = verify;
  info.parameters = parameters;
  const uint32_t f = verify.functions;
  info.power_control = f & kPowerControlSupported;
  info.display_mux = f & kDisplayMuxSupported;
  info.i2c_mux = f & kI2cMuxSupported;
  info.switch_start = f & kSwitchStartSupported;
  info.switch_end = f & kSwitchEndSupported;
  if (parameters) info.valid_bits = parameters->flags & parameters->valid_flags;
  const uint32_t v = info.valid_bits;
  if (v & kSeparateMuxForI2c) info.i2c_mux = info.display_mux = true;                              // a separate I2C mux needs both controls
  if (v & (kCrt1RgbSignalMuxed | kTvSignalMuxed | kDfpSignalMuxed)) info.display_mux = true;       // muxed outputs need the display mux
  if (v & (kDynamicPxSupported | kDynamicDgpuPowerOffSupported)) info.power_control = true;        // firmware that flags this instead
  if (v & kMsHybridGraphicsSupported) {                                                             // hybrid graphics: the legacy power path is off
    info.power_control = false;
    info.hybrid = true;
  }
  info.dgpu_needs_power_for_displays = v & kDgpuRequiresPowerForDisplays;
  return info;
}

bool AmdAtpx::call(acpi::AmlInterpreter& aml, acpi::Node* method, uint32_t function, ConstByteView input) {
  if (!method || !aml.is_method(method) || input.size() > kMaxInputBytes) return false;
  uint8_t copy[kMaxInputBytes] = {};
  acpi::Value args[2];
  args[0].kind = acpi::ValueKind::Integer;
  args[0].integer = function;
  if (input.empty()) {
    args[1].kind = acpi::ValueKind::Integer;
    args[1].integer = 0;
  } else {
    (void)ByteView(copy).first(input.size()).copy_from(input);
    args[1].kind = acpi::ValueKind::Buffer;
    args[1].length = static_cast<uint32_t>(input.size());
    args[1].bytes = copy;
  }
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  acpi::Value ignored;
  return aml.evaluate(method, args, 2, &ignored);
}

Optional<AmdAtpxInfo> AmdAtpx::read(acpi::AmlInterpreter& aml, acpi::Node* method) {
  if (!method || !aml.is_method(method)) return {};
  kernel::ScopedRecursiveMutex guard(aml.mutex());   // (the answers are valid until the next evaluation)
  acpi::Value args[2];
  args[0].kind = acpi::ValueKind::Integer;
  args[0].integer = kVerifyInterface;
  args[1].kind = acpi::ValueKind::Integer;
  args[1].integer = 0;
  acpi::Value r;
  if (!aml.evaluate(method, args, 2, &r) || r.kind != acpi::ValueKind::Buffer) return {};
  const Optional<AtpxVerifyInterface> verify = parse_verify_interface(acpi_value_bytes(r));
  if (!verify) return {};
  Optional<AtpxPxParameters> parameters;
  if (verify->functions & kPxParametersSupported) {
    args[0].integer = kGetPxParameters;
    if (!aml.evaluate(method, args, 2, &r) || r.kind != acpi::ValueKind::Buffer) return {};   // (amdgpu gives up on ATPX too)
    parameters = parse_px_parameters(acpi_value_bytes(r));
    if (!parameters) return {};
  }
  return derive(*verify, parameters);
}

bool AmdAtpx::power(uint8_t state) {
  if (!info_.power_control) return false;
  const uint8_t input[3] = {3, 0, state};
  if (!call(aml_, method_, kPowerControl, ConstByteView(input))) return false;
  if (state == 0 && sleep_ms_) sleep_ms_(kPowerOffSettleMs);   // (outside the interpreter's lock)
  return true;
}

bool AmdAtpx::power_off() { return power(0); }
bool AmdAtpx::power_on() { return power(1); }

bool AmdAtpx::mux(uint32_t function, GpuMuxTarget target) {
  const uint16_t id = target == GpuMuxTarget::Discrete ? kMuxDiscrete : kMuxIntegrated;
  const uint8_t input[4] = {4, 0, static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8)};
  return call(aml_, method_, function, ConstByteView(input));
}

bool AmdAtpx::active(GpuMuxTarget* out) {
  if (!active_) return false;
  *out = *active_;
  return true;
}

bool AmdAtpx::switch_to(GpuMuxTarget target) {
  if (!info_.display_mux) return false;
  if (info_.switch_start && !mux(kSwitchStart, target)) return false;
  if (!mux(kDisplayMuxControl, target)) return false;
  active_ = target;
  bool ok = true;
  if (info_.i2c_mux && !mux(kI2cMuxControl, target)) ok = false;
  if (info_.switch_end && !mux(kSwitchEnd, target)) ok = false;
  return ok;
}

bool AmdAtpx::switch_ddc(GpuMuxTarget target) { return info_.i2c_mux && mux(kI2cMuxControl, target); }

}  // namespace hardware
