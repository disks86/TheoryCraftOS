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

#include "Hardware/HardwareMonitor/NuvotonNct67xx.hpp"
#include "Hardware/HardwareMonitor/SensorMath.hpp"

namespace hardware {

namespace {

// --- Voltages.
constexpr uint16_t kNct6775Voltages[] = {0x020, 0x021, 0x022, 0x023, 0x024, 0x025, 0x026, 0x550, 0x551};
constexpr uint16_t kNct6775Scale[] = {800, 800, 1600, 1600, 800, 800, 800, 1600, 1600};
constexpr const char* kNct6775VoltageLabels[] = {"Vcore", "VIN1", "AVCC", "3VCC", "VIN0", "VIN2", "VIN3", "3VSB", "VBAT"};
constexpr uint16_t kNct6779Voltages[] = {0x480, 0x481, 0x482, 0x483, 0x484, 0x485, 0x486, 0x487, 0x488, 0x489, 0x48A, 0x48B, 0x48C, 0x48D, 0x48E};
constexpr uint16_t kNct6779Scale[] = {800, 800, 1600, 1600, 800, 800, 800, 1600, 1600, 800, 800, 800, 800, 800, 800};
constexpr const char* kNct6779VoltageLabels[] = {"Vcore", "VIN1", "AVSB", "3VCC", "VIN0", "VIN8", "VIN4", "3VSB", "VBAT", "VTT", "VIN5", "VIN6",
                                                 "VIN2", "VIN3", "VIN7"};

// --- Temperatures.
constexpr NuvotonTemperatureChannel kNct6775Temperatures[] = {{0x027, false, 0x621}, {0x150, true, 0x622}, {0x250, true, 0x623}};
constexpr NuvotonTemperatureChannel kNct6779Temperatures[] = {{0x027, false, 0x621}, {0x150, true, 0x622}};
constexpr const char* kNct6775Sources[] = {"", "SYSTIN", "CPUTIN", "AUXTIN", "SMBUSMASTER 0", "SMBUSMASTER 1", "SMBUSMASTER 2", "SMBUSMASTER 3",
                                           "SMBUSMASTER 4", "SMBUSMASTER 5", "SMBUSMASTER 6", "SMBUSMASTER 7", "PECI Agent 0", "PECI Agent 1",
                                           "PCH_CHIP_CPU_MAX_TEMP", "PCH_CHIP_TEMP", "PCH_CPU_TEMP", "PCH_MCH_TEMP", "PCH_DIM0_TEMP",
                                           "PCH_DIM1_TEMP", "PCH_DIM2_TEMP", "PCH_DIM3_TEMP"};
constexpr const char* kNct6779Sources[] = {"", "SYSTIN", "CPUTIN", "AUXTIN0", "AUXTIN1", "AUXTIN2", "AUXTIN3", "", "SMBUSMASTER 0",
                                           "SMBUSMASTER 1", "", "", "", "", "", "", "PECI Agent 0", "PECI Agent 1", "PCH_CHIP_CPU_MAX_TEMP",
                                           "PCH_CHIP_TEMP", "PCH_CPU_TEMP", "PCH_MCH_TEMP", "Agent0 Dimm0", "Agent0 Dimm1", "Agent1 Dimm0",
                                           "Agent1 Dimm1", "BYTE_TEMP0", "BYTE_TEMP1", "PECI Agent 0 Calibration", "PECI Agent 1 Calibration", "",
                                           "Virtual_TEMP"};

// --- Fans and their control.
constexpr uint16_t kNct6775Fans[] = {0x630, 0x632, 0x634, 0x636, 0x638};
constexpr uint16_t kNct6779Fans[] = {0x4C0, 0x4C2, 0x4C4, 0x4C6, 0x4C8, 0x4CA, 0x4CE};
constexpr const char* kFanLabels[] = {"SYSFAN", "CPUFAN", "AUXFAN0", "AUXFAN1", "AUXFAN2", "AUXFAN3", "AUXFAN4"};
constexpr uint16_t kPwmDuty[] = {0x109, 0x209, 0x309, 0x809, 0x909, 0xA09, 0xB09};
constexpr uint16_t kPwmRead[] = {0x001, 0x003, 0x011, 0x013, 0x015, 0xA09, 0xB09};
constexpr uint16_t kPwmModeReg[] = {0x102, 0x202, 0x302, 0x802, 0x902, 0xA02, 0xB02};
constexpr const char* kPwmLabels[] = {"SYSFAN", "CPUFAN", "AUXFAN0", "AUXFAN1", "AUXFAN2", "AUXFAN3", "AUXFAN4"};
static_assert(sizeof kPwmDuty / sizeof kPwmDuty[0] == NuvotonNct67xx::kMaxPwm);

using sensor_math::view;
template <uint64_t N>
constexpr ArrayView<const uint16_t> first(const uint16_t (&array)[N], uint64_t count) { return ArrayView<const uint16_t>(array, count <= N ? count : N); }

// One entry per chip generation and fan count. (The NCT6798 and NCT6799 may scale more inputs than the others; Linux's tables differ between
// versions, so all share the NCT6779 scaling until checked on a board.)
const NuvotonLayout kNct6775 = {view(kNct6775Voltages), view(kNct6775Scale), view(kNct6775VoltageLabels), view(kNct6775Temperatures),
                                view(kNct6775Sources), NuvotonFanFormat::Count16, view(kNct6775Fans), 3};
const NuvotonLayout kNct6776 = {view(kNct6775Voltages), view(kNct6775Scale), view(kNct6775VoltageLabels), view(kNct6775Temperatures),
                                view(kNct6775Sources), NuvotonFanFormat::Count13, view(kNct6775Fans), 3};
const NuvotonLayout kNct6779 = {view(kNct6779Voltages), view(kNct6779Scale), view(kNct6779VoltageLabels), view(kNct6779Temperatures),
                                view(kNct6779Sources), NuvotonFanFormat::Rpm, first(kNct6779Fans, 5), 5};
const NuvotonLayout kNct6791 = {view(kNct6779Voltages), view(kNct6779Scale), view(kNct6779VoltageLabels), view(kNct6779Temperatures),
                                view(kNct6779Sources), NuvotonFanFormat::Rpm, first(kNct6779Fans, 6), 6};
const NuvotonLayout kNct6796 = {view(kNct6779Voltages), view(kNct6779Scale), view(kNct6779VoltageLabels), view(kNct6779Temperatures),
                                view(kNct6779Sources), NuvotonFanFormat::Rpm, first(kNct6779Fans, 7), 7};

}  // namespace

const NuvotonLayout* nuvoton_layout(SuperIoChip chip) {
  switch (chip) {
    case SuperIoChip::Nct6775: return &kNct6775;
    case SuperIoChip::Nct6776: return &kNct6776;
    case SuperIoChip::Nct6779: return &kNct6779;
    case SuperIoChip::Nct6791:
    case SuperIoChip::Nct6792:
    case SuperIoChip::Nct6793:
    case SuperIoChip::Nct6795: return &kNct6791;
    case SuperIoChip::Nct6796:
    case SuperIoChip::Nct6797:
    case SuperIoChip::Nct6798:
    case SuperIoChip::Nct6799: return &kNct6796;
    default: return nullptr;
  }
}

uint8_t NuvotonNct67xx::read8(uint16_t reg) {
  ports_.write(kRegBank, static_cast<uint8_t>(reg >> 8));
  return ports_.read(static_cast<uint8_t>(reg));
}

uint16_t NuvotonNct67xx::read16(uint16_t reg) {
  ports_.write(kRegBank, static_cast<uint8_t>(reg >> 8));
  const uint8_t high = ports_.read(static_cast<uint8_t>(reg));
  const uint8_t low = ports_.read(static_cast<uint8_t>(reg + 1));
  return static_cast<uint16_t>(high << 8 | low);
}

void NuvotonNct67xx::write8(uint16_t reg, uint8_t value) {
  ports_.write(kRegBank, static_cast<uint8_t>(reg >> 8));
  ports_.write(static_cast<uint8_t>(reg), value);
}

bool NuvotonNct67xx::initialize() {
  layout_ = nullptr;
  temperature_count_ = 0;
  if (!found_.info || found_.info->vendor != SuperIoVendor::Nuvoton || found_.sensor_base == 0) return false;
  const NuvotonLayout* layout = nuvoton_layout(found_.info->chip);
  if (!layout) return false;
  // Bank 0, low byte of the vendor id selected (bit 7 of the bank register, HBACS, clear): 0xA3. Some firmware leaves HBACS set: 0x5C.
  const uint8_t vendor = read8(kRegVendor);
  if (vendor != 0xA3 && vendor != 0x5C) return false;
  layout_ = layout;
  for (const NuvotonTemperatureChannel& channel : layout->temperatures) {
    if (temperature_count_ >= kMaxTemperatures) break;
    const uint8_t source = read8(channel.source_reg) & 0x1F;
    const char* label = "";
    if (source < layout->source_labels.size()) label = layout->source_labels[source];
    if (source == 0 || !label[0]) continue;   // not monitoring anything
    Temperature& t = temperatures_[temperature_count_++];
    t.channel = &channel;
    t.label = label;
    t.role = sensor_math::role_of_label(label);
  }
  for (uint32_t i = 0; i < layout->pwm_count && i < kMaxPwm; ++i) firmware_mode_[i] = static_cast<uint8_t>(read8(view(kPwmModeReg)[i]) >> 4);
  return true;
}

void NuvotonNct67xx::describe(IPropertySink& sink) const {
  if (found_.info) sink.add_text("Chip", found_.info->name);
  sink.add_hex("Chip id", found_.raw_id);
  sink.add_hex("Sensor I/O base", found_.sensor_base);
  sink.add_hex("Configuration port", found_.config_port, 2);
}

uint32_t NuvotonNct67xx::channel_count(MonitorChannel kind) const {
  if (!layout_) return 0;
  switch (kind) {
    case MonitorChannel::Temperature: return temperature_count_;
    case MonitorChannel::Fan: return static_cast<uint32_t>(layout_->fan_regs.size());
    case MonitorChannel::Voltage: return static_cast<uint32_t>(layout_->voltage_regs.size());
    case MonitorChannel::Pwm: return layout_->pwm_count;
  }
  return 0;
}

const char* NuvotonNct67xx::label(MonitorChannel kind, uint32_t channel) const {
  if (channel >= channel_count(kind)) return nullptr;
  switch (kind) {
    case MonitorChannel::Temperature: return temperatures_[channel].label;
    case MonitorChannel::Fan: return view(kFanLabels)[channel];
    case MonitorChannel::Voltage: return layout_->voltage_labels[channel];
    case MonitorChannel::Pwm: return view(kPwmLabels)[channel];
  }
  return nullptr;
}

SensorRole NuvotonNct67xx::role(MonitorChannel kind, uint32_t channel) const {
  if (channel >= channel_count(kind)) return SensorRole::Other;
  if (kind == MonitorChannel::Temperature) return temperatures_[channel].role;
  return sensor_math::role_of_label(label(kind, channel));
}

Optional<int32_t> NuvotonNct67xx::temperature_millicelsius(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Temperature)) return {};
  const NuvotonTemperatureChannel& c = *temperatures_[channel].channel;
  if (c.half_degrees) return sensor_math::celsius_half_word(read16(c.reg));
  return sensor_math::celsius_byte(read8(c.reg));
}

Optional<uint32_t> NuvotonNct67xx::fan_rpm(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Fan)) return {};
  const uint16_t raw = read16(layout_->fan_regs[channel]);
  switch (layout_->fan_format) {
    case NuvotonFanFormat::Count16:
      return sensor_math::rpm_from_count(raw, 0xFFFF, 1350000);
    case NuvotonFanFormat::Count13: {
      // High byte count bits 12:5, low byte bits 4:0.
      const uint32_t count = (raw & 0x1Fu) | ((raw & 0xFF00u) >> 3);
      return sensor_math::rpm_from_count(count, 0x1FFF, 1350000);
    }
    case NuvotonFanFormat::Rpm:
      if (raw == 0xFFFF) return {};   // (a stuck bus; no fan turns at 65535 RPM)
      return static_cast<uint32_t>(raw);
  }
  return {};
}

Optional<int32_t> NuvotonNct67xx::voltage_millivolts(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Voltage)) return {};
  return sensor_math::scaled(read8(layout_->voltage_regs[channel]), layout_->voltage_scale[channel]);
}

Optional<PwmState> NuvotonNct67xx::pwm(uint32_t channel) {
  if (channel >= channel_count(MonitorChannel::Pwm)) return {};
  PwmState state;
  state.duty = read8(view(kPwmRead)[channel]);
  state.mode = (read8(view(kPwmModeReg)[channel]) >> 4) == 0 ? PwmMode::Manual : PwmMode::Automatic;
  return state;
}

bool NuvotonNct67xx::set_pwm_duty(uint32_t channel, uint8_t duty) {
  if (channel >= channel_count(MonitorChannel::Pwm)) return false;
  write8(view(kPwmDuty)[channel], duty);
  const uint8_t mode = read8(view(kPwmModeReg)[channel]);
  if (mode >> 4) write8(view(kPwmModeReg)[channel], static_cast<uint8_t>(mode & 0x0F));
  return read8(view(kPwmDuty)[channel]) == duty && (read8(view(kPwmModeReg)[channel]) >> 4) == 0;
}

bool NuvotonNct67xx::set_pwm_mode(uint32_t channel, PwmMode mode) {
  if (channel >= channel_count(MonitorChannel::Pwm)) return false;
  const uint8_t reg = read8(view(kPwmModeReg)[channel]);
  uint8_t wanted = 0;
  if (mode == PwmMode::Automatic) {
    wanted = firmware_mode_[channel];
    if (wanted == 0) return false;   // the firmware ran this output by hand: there is no curve to hand it back to
  } else if ((reg >> 4) != 0) {
    write8(view(kPwmDuty)[channel], read8(view(kPwmRead)[channel]));   // keep the duty the curve had reached
  }
  if ((reg >> 4) != wanted) write8(view(kPwmModeReg)[channel], static_cast<uint8_t>((reg & 0x0F) | (wanted << 4)));
  return (read8(view(kPwmModeReg)[channel]) >> 4) == wanted;
}

}  // namespace hardware
