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

#include "Hardware/HardwareMonitor/NuvotonNct668x.hpp"
#include "Hardware/DeviceProperties.hpp"
#include "Hardware/HardwareMonitor/SensorMath.hpp"

namespace hardware {

namespace {

using sensor_math::view;
using Pwm = Nct668xPwmControl;

// --- Chips. Linux's nct6683 treats the three alike; they differ in pin count, not in the EC's register map.
constexpr Nct668xLayout kNct668x = {32, 16, 8};
static_assert(kNct668x.monitor_channels <= NuvotonNct668x::kMaxMonitors && kNct668x.fan_inputs <= NuvotonNct668x::kMaxFans &&
              kNct668x.fan_outputs <= NuvotonNct668x::kMaxPwm);

// --- Monitoring sources (Linux's nct6683_mon_label), by the value of the configuration register's bits 6:0.
constexpr const char* kSources[] = {
    nullptr, "Local", "Diode 0 (curr)", "Diode 1 (curr)", "Diode 2 (curr)", "Diode 0 (volt)", "Diode 1 (volt)", "Diode 2 (volt)",
    "Thermistor 14", "Thermistor 15", "Thermistor 16", "Thermistor 0", "Thermistor 1", "Thermistor 2", "Thermistor 3", "Thermistor 4",
    "Thermistor 5", "Thermistor 6", "Thermistor 7", "Thermistor 8", "Thermistor 9", "Thermistor 10", "Thermistor 11", "Thermistor 12",   // 0x10
    "Thermistor 13", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    "PECI 0.0", "PECI 1.0", "PECI 2.0", "PECI 3.0", "PECI 0.1", "PECI 1.1", "PECI 2.1", "PECI 3.1",   // 0x20
    "PECI DIMM 0", "PECI DIMM 1", "PECI DIMM 2", "PECI DIMM 3", nullptr, nullptr, nullptr, nullptr,
    "PCH CPU", "PCH CHIP", "PCH CHIP CPU MAX", "PCH MCH", "PCH DIMM 0", "PCH DIMM 1", "PCH DIMM 2", "PCH DIMM 3",   // 0x30
    "SMBus 0", "SMBus 1", "SMBus 2", "SMBus 3", "SMBus 4", "SMBus 5", "DIMM 0", "DIMM 1",
    "DIMM 2", "DIMM 3", "AMD TSI Addr 90h", "AMD TSI Addr 92h", "AMD TSI Addr 94h", "AMD TSI Addr 96h", "AMD TSI Addr 98h",   // 0x40
    "AMD TSI Addr 9ah", "AMD TSI Addr 9ch", "AMD TSI Addr 9dh", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    "Virtual 0", "Virtual 1", "Virtual 2", "Virtual 3", "Virtual 4", "Virtual 5", "Virtual 6", "Virtual 7",   // 0x50
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    "VCC", "VSB", "AVSB", "VTT", "VBAT", "VREF", "VIN0", "VIN1", "VIN2", "VIN3", "VIN4", "VIN5", "VIN6", "VIN7", "VIN8", "VIN9",   // 0x60
    "VIN10", "VIN11", "VIN12", "VIN13", "VIN14", "VIN15", "VIN16",
};
static_assert(sizeof kSources / sizeof kSources[0] == 0x77);
constexpr uint8_t kSourceVcc = 0x60, kSourceVsb = 0x61, kSourceAvsb = 0x62, kSourceVbat = 0x64;

// --- Customers (Linux's NCT6683_CUSTOMER_ID_*). Fan header names for MSI boards follow LibreHardwareMonitor's NCT6687D table.
constexpr const char* kMsiFans[] = {"CPU Fan", "Pump Fan", "System Fan 1", "System Fan 2", "System Fan 3", "System Fan 4", "System Fan 5",
                                    "System Fan 6"};
constexpr const char* kGenericFans[] = {"FAN1", "FAN2", "FAN3", "FAN4", "FAN5", "FAN6", "FAN7", "FAN8",
                                        "FAN9", "FAN10", "FAN11", "FAN12", "FAN13", "FAN14", "FAN15", "FAN16"};
static_assert(sizeof kGenericFans / sizeof kGenericFans[0] == NuvotonNct668x::kMaxFans);
constexpr ArrayView<const char* const> kNoLabels;

const Nct668xCustomer kCustomers[] = {
    {0x0805, "Intel", Pwm::ReadOnly, kNoLabels},
    {0x0A0E, "Mitac", Pwm::Request, kNoLabels},   // (the only customer mainline Linux writes duties for)
    {0x0200, "MSI", Pwm::MsiModeBits, view(kMsiFans)},
    {0x0201, "MSI", Pwm::MsiModeBits, view(kMsiFans)},
    {0x0207, "MSI", Pwm::MsiModeBits, view(kMsiFans)},
    {0x020D, "MSI", Pwm::MsiModeBits, view(kMsiFans)},
    {0x162B, "AMD", Pwm::ReadOnly, kNoLabels},
    {0x0E2C, "ASRock", Pwm::ReadOnly, kNoLabels},
    {0x0E1B, "ASRock", Pwm::ReadOnly, kNoLabels},
    {0x1631, "ASRock", Pwm::ReadOnly, kNoLabels},
    {0x163E, "ASRock", Pwm::ReadOnly, kNoLabels},
};

bool is_voltage(uint8_t source) { return source >= NuvotonNct668x::kVoltageSourceStart; }

SensorRole role_of(const char* label) {
  using sensor_math::starts_with;
  if (starts_with(label, "PECI") || starts_with(label, "AMD TSI") || starts_with(label, "PCH CPU") || starts_with(label, "PCH CHIP CPU") ||
      starts_with(label, "CPU"))
    return SensorRole::Cpu;
  if (starts_with(label, "PCH")) return SensorRole::Chipset;
  if (starts_with(label, "Local") || starts_with(label, "System")) return SensorRole::System;
  return SensorRole::Other;
}

// Two decimal digits into `out` at `at`.
void put2(ArrayView<char> out, uint32_t at, uint8_t value) {
  out[at] = static_cast<char>('0' + value / 10 % 10);
  out[at + 1] = static_cast<char>('0' + value % 10);
}

}  // namespace

const Nct668xLayout* nct668x_layout(SuperIoChip chip) {
  switch (chip) {
    case SuperIoChip::Nct6683:
    case SuperIoChip::Nct6686:
    case SuperIoChip::Nct6687: return &kNct668x;
    default: return nullptr;
  }
}

const Nct668xCustomer* nct668x_customer(uint16_t id) {
  for (const Nct668xCustomer& customer : kCustomers) {
    if (customer.id == id) return &customer;
  }
  return nullptr;
}

const char* nct668x_source_label(uint8_t source) {
  const char* const* label = view(kSources).try_at(source);
  return label ? *label : nullptr;
}

// --- EC space access.

bool NuvotonNct668x::wait_free() {
  for (uint32_t i = 0; i < kBusyPolls; ++i) {
    if (io_.read8(port(kPagePort)) == kRelease) return true;
  }
  return false;
}

void NuvotonNct668x::select(uint16_t reg) {
  io_.write8(port(kPagePort), kRelease);
  io_.write8(port(kPagePort), static_cast<uint8_t>(reg >> 8));
  io_.write8(port(kIndexPort), static_cast<uint8_t>(reg));
}

Optional<uint8_t> NuvotonNct668x::read8(uint16_t reg) {
  if (!wait_free()) return {};
  select(reg);
  const uint8_t value = io_.read8(port(kDataPort));
  io_.write8(port(kPagePort), kRelease);
  return value;
}

Optional<uint16_t> NuvotonNct668x::read16(uint16_t reg) {
  const Optional<uint8_t> high = read8(reg);
  if (!high) return {};
  const Optional<uint8_t> low = read8(static_cast<uint16_t>(reg + 1));
  if (!low) return {};
  return static_cast<uint16_t>(*high << 8 | *low);
}

bool NuvotonNct668x::write8(uint16_t reg, uint8_t value) {
  if (!wait_free()) return false;
  select(reg);
  io_.write8(port(kDataPort), value);
  io_.write8(port(kPagePort), kRelease);
  return true;
}

// --- Setup.

bool NuvotonNct668x::initialize() {
  layout_ = nullptr;
  customer_ = nullptr;
  temperature_count_ = voltage_count_ = fan_count_ = pwm_count_ = 0;
  for (bool& m : manual_) m = false;
  if (!found_.info || found_.info->vendor != SuperIoVendor::Nuvoton || found_.sensor_base == 0) return false;
  const Nct668xLayout* layout = nct668x_layout(found_.info->chip);
  if (!layout) return false;
  // Take over whatever page the firmware left selected (Linux writes the release value before every access and never waits).
  io_.write8(port(kPagePort), kRelease);
  const Optional<uint16_t> customer = read16(kRegCustomerId);
  if (!customer) return false;
  customer_id_ = *customer;
  // Linux binds only to the customers it knows (without force=1): their firmware lays the EC space out as above. A floating bus reads 0xFFFF.
  const Nct668xCustomer* known = nct668x_customer(customer_id_);
  if (!known) return false;

  for (uint32_t i = 0; i < 3; ++i) {
    const Optional<uint8_t> b = read8(static_cast<uint16_t>(kRegBuildYear + i));
    ArrayView<uint8_t>{build_}[i] = b.value_or(0);
  }
  for (uint32_t i = 0; i < 2; ++i) {
    const Optional<uint8_t> b = read8(static_cast<uint16_t>(kRegVersion + i));
    ArrayView<uint8_t>{version_}[i] = b.value_or(0);
  }

  ArrayView<Monitor> temperatures(temperatures_), voltages(voltages_);
  for (uint32_t i = 0; i < layout->monitor_channels; ++i) {
    const Optional<uint8_t> config = read8(static_cast<uint16_t>(kRegMonitorConfig + i));
    if (!config) return false;
    const uint8_t source = *config & 0x7F;
    if (!nct668x_source_label(source)) continue;   // off, reserved or out of the table
    Monitor& m = is_voltage(source) ? voltages[voltage_count_++] : temperatures[temperature_count_++];
    m.index = static_cast<uint8_t>(i);
    m.source = source;
  }
  ArrayView<uint8_t> fans(fans_), pwms(pwms_);
  for (uint32_t i = 0; i < layout->fan_inputs; ++i) {
    const Optional<uint8_t> config = read8(static_cast<uint16_t>(kRegFanInConfig + i));
    if (!config) return false;
    if (*config & kConfigInUse) fans[fan_count_++] = static_cast<uint8_t>(i);
  }
  Optional<uint8_t> modes;
  if (known->pwm == Nct668xPwmControl::MsiModeBits) {
    modes = read8(kRegFanMode);
    if (!modes) return false;
  }
  for (uint32_t i = 0; i < layout->fan_outputs; ++i) {
    const Optional<uint8_t> config = read8(static_cast<uint16_t>(kRegFanOutConfig + i));
    if (!config) return false;
    if (!(*config & kConfigInUse)) continue;
    pwms[pwm_count_++] = static_cast<uint8_t>(i);
    ArrayView<uint8_t>{firmware_duty_}[i] = read8(static_cast<uint16_t>(kRegPwm + i)).value_or(0);
    ArrayView<bool>{firmware_manual_}[i] = modes && (*modes >> i & 1) != 0;
  }
  layout_ = layout;
  customer_ = known;
  return true;
}

void NuvotonNct668x::describe(IPropertySink& sink) const {
  if (found_.info) sink.add_text("Chip", found_.info->name);
  sink.add_hex("Chip id", found_.raw_id);
  sink.add_hex("Sensor I/O base", found_.sensor_base);
  sink.add_hex("Configuration port", found_.config_port, 2);
  if (!customer_) return;
  sink.add_text("Board vendor (customer id)", customer_->name);
  sink.add_hex("Customer id", customer_id_, 4);
  sink.add_hex("EC firmware version", static_cast<uint16_t>(version_[0] << 8 | version_[1]), 4);
  char date[10] = {'2', '0', 0, 0, '-', 0, 0, '-', 0, 0};
  const ArrayView<char> text(date);
  put2(text, 2, build_[0]);
  put2(text, 5, build_[1]);
  put2(text, 8, build_[2]);
  sink.add("EC firmware date", StringView(date, sizeof date));
  sink.add_flag("Fan control writable", pwm_writable());
}

// --- Channels.

uint32_t NuvotonNct668x::channel_count(MonitorChannel kind) const {
  if (!layout_) return 0;
  switch (kind) {
    case MonitorChannel::Temperature: return temperature_count_;
    case MonitorChannel::Fan: return fan_count_;
    case MonitorChannel::Voltage: return voltage_count_;
    case MonitorChannel::Pwm: return pwm_count_;
  }
  return 0;
}

const char* NuvotonNct668x::fan_label(uint8_t index) const {
  if (customer_ && index < customer_->fan_labels.size()) return customer_->fan_labels[index];
  return view(kGenericFans)[index];
}

const char* NuvotonNct668x::label(MonitorChannel kind, uint32_t channel) const {
  if (channel >= channel_count(kind)) return nullptr;
  switch (kind) {
    case MonitorChannel::Temperature: return nct668x_source_label(ArrayView<const Monitor>(temperatures_)[channel].source);
    case MonitorChannel::Voltage: return nct668x_source_label(ArrayView<const Monitor>(voltages_)[channel].source);
    case MonitorChannel::Fan: return fan_label(ArrayView<const uint8_t>(fans_)[channel]);
    case MonitorChannel::Pwm: return fan_label(ArrayView<const uint8_t>(pwms_)[channel]);
  }
  return nullptr;
}

SensorRole NuvotonNct668x::role(MonitorChannel kind, uint32_t channel) const {
  if (channel >= channel_count(kind) || kind == MonitorChannel::Voltage) return SensorRole::Other;
  return role_of(label(kind, channel));
}

Optional<int32_t> NuvotonNct668x::temperature_millicelsius(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Temperature)) return {};
  const Monitor& m = ArrayView<const Monitor>(temperatures_)[channel];
  const Optional<uint16_t> raw = read16(static_cast<uint16_t>(kRegMonitor + 2u * m.index));
  if (!raw) return {};
  const Optional<int32_t> value = sensor_math::celsius_half_word(*raw);   // (-128 degrees, an open input, is no reading)
  if (!value || *value < kMinMillicelsius) return {};
  return value;
}

Optional<int32_t> NuvotonNct668x::voltage_millivolts(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Voltage)) return {};
  const Monitor& m = ArrayView<const Monitor>(voltages_)[channel];
  const Optional<uint8_t> raw = read8(static_cast<uint16_t>(kRegMonitor + 2u * m.index));
  if (!raw) return {};
  const bool halved = m.source == kSourceVcc || m.source == kSourceVsb || m.source == kSourceAvsb || m.source == kSourceVbat;
  return static_cast<int32_t>(*raw) * (halved ? 32 : 16);
}

Optional<uint32_t> NuvotonNct668x::fan_rpm(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Fan)) return {};
  const uint8_t index = ArrayView<const uint8_t>(fans_)[channel];
  const Optional<uint16_t> raw = read16(static_cast<uint16_t>(kRegFanRpm + 2u * index));
  if (!raw || *raw == 0xFFFF) return {};   // (a stuck bus; no fan turns at 65535 RPM)
  return static_cast<uint32_t>(*raw);
}

Optional<PwmState> NuvotonNct668x::pwm(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Pwm)) return {};
  const uint8_t output = ArrayView<const uint8_t>(pwms_)[channel];
  const Optional<uint8_t> duty = read8(static_cast<uint16_t>(kRegPwm + output));
  if (!duty) return {};
  PwmState state;
  state.duty = *duty;
  switch (customer_->pwm) {
    case Nct668xPwmControl::ReadOnly: state.mode = PwmMode::Automatic; break;
    case Nct668xPwmControl::Request: state.mode = ArrayView<const bool>(manual_)[output] ? PwmMode::Manual : PwmMode::Automatic; break;
    case Nct668xPwmControl::MsiModeBits: {
      const Optional<uint8_t> modes = read8(kRegFanMode);
      if (!modes) return {};
      state.mode = (*modes >> output & 1) ? PwmMode::Manual : PwmMode::Automatic;
      break;
    }
  }
  return state;
}

// --- Fan control.

bool NuvotonNct668x::write_duty(uint8_t output, uint8_t duty) {
  if (!write8(kRegFanControl, kFanControlRequest)) return false;
  for (uint32_t i = 0; i < kSettleReads; ++i) (void)io_.read8(port(kPagePort));   // (give the EC time to take the request)
  if (!write8(static_cast<uint16_t>(kRegPwmWrite + output), duty)) {
    (void)write8(kRegFanControl, kFanControlDone);
    return false;
  }
  return write8(kRegFanControl, kFanControlDone);
}

bool NuvotonNct668x::set_mode_bit(uint8_t output, bool manual) {
  const Optional<uint8_t> modes = read8(kRegFanMode);
  if (!modes) return false;
  const uint8_t bit = static_cast<uint8_t>(1u << output);
  const uint8_t wanted = static_cast<uint8_t>(manual ? (*modes | bit) : (*modes & ~bit));
  if (wanted != *modes && !write8(kRegFanMode, wanted)) return false;
  const Optional<uint8_t> after = read8(kRegFanMode);
  return after && ((*after & bit) != 0) == manual;
}

bool NuvotonNct668x::set_pwm_duty(uint32_t channel, uint8_t duty) {
  if (channel >= channel_count(MonitorChannel::Pwm)) return false;
  const uint8_t output = ArrayView<const uint8_t>(pwms_)[channel];
  switch (customer_->pwm) {
    case Nct668xPwmControl::ReadOnly: return false;
    case Nct668xPwmControl::Request:
      if (!write_duty(output, duty)) return false;
      ArrayView<bool>{manual_}[output] = true;
      return true;
    case Nct668xPwmControl::MsiModeBits:
      return set_mode_bit(output, true) && write_duty(output, duty);
  }
  return false;
}

bool NuvotonNct668x::set_pwm_mode(uint32_t channel, PwmMode mode) {
  if (channel >= channel_count(MonitorChannel::Pwm)) return false;
  const uint8_t output = ArrayView<const uint8_t>(pwms_)[channel];
  switch (customer_->pwm) {
    case Nct668xPwmControl::ReadOnly: return false;
    case Nct668xPwmControl::Request: {
      if (mode == PwmMode::Automatic) return false;   // (no way back to the firmware's curve is known for these boards)
      const Optional<uint8_t> duty = read8(static_cast<uint16_t>(kRegPwm + output));
      return duty && set_pwm_duty(channel, *duty);
    }
    case Nct668xPwmControl::MsiModeBits: {
      if (mode == PwmMode::Manual) {
        const Optional<uint8_t> duty = read8(static_cast<uint16_t>(kRegPwm + output));   // keep the duty the curve had reached
        return duty && set_mode_bit(output, true) && write_duty(output, *duty);
      }
      if (ArrayView<const bool>(firmware_manual_)[output]) return false;   // the firmware ran it at a fixed duty: there is no curve
      // As LibreHardwareMonitor: clear the bit and put back the duty the firmware had, which its curve then takes over from.
      return set_mode_bit(output, false) && write_duty(output, ArrayView<const uint8_t>(firmware_duty_)[output]);
    }
  }
  return false;
}

}  // namespace hardware
