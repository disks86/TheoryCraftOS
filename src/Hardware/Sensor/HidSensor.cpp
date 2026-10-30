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

#include "Hardware/Sensor/HidSensor.hpp"

namespace hardware {

namespace {

constexpr uint16_t kModifierMask = 0xF000, kUsageMask = 0x0FFF;
constexpr uint16_t kPropertyPowerState = 0x0319, kPropertyReportingState = 0x0316;
constexpr uint16_t kReportingAllEvents = 0x0841, kPowerD0 = 0x0851;

struct Layout {
  SensorKind kind;
  uint16_t first_usage;
  uint32_t axes;
};
constexpr Layout kLayouts[] = {
    {SensorKind::Accelerometer, 0x453, 3},
    {SensorKind::Gyroscope, 0x457, 3},
    {SensorKind::Magnetometer, 0x485, 3},
    {SensorKind::AmbientLight, 0x4D1, 1},
};

const HidInputField* data_field(const HidInputMap& map, uint8_t id, uint16_t usage) {
  for (const HidInputField& f : map.fields())
    if (f.report_id == id && f.usage_page == hid_usage::kPageSensor && (f.usage & kModifierMask) == 0 && (f.usage & kUsageMask) == usage) return &f;
  return nullptr;
}

float power_of_ten(int32_t exponent) {
  float v = 1.0f;
  for (int32_t e = exponent; e > 0 && e < 16; --e) v *= 10.0f;
  for (int32_t e = exponent; e < 0 && e > -16; ++e) v /= 10.0f;
  return v;
}

void put_bits(ByteView report, uint32_t bit_offset, uint32_t bits, uint32_t value) {   // (bit offsets count the id byte, which the buffer holds first)
  for (uint32_t i = 0; i < bits; ++i) {
    const uint32_t at = bit_offset + i;
    if (at / 8 >= report.size()) return;
    if ((value >> i) & 1) report[at / 8] |= static_cast<uint8_t>(1u << (at % 8));
    else report[at / 8] &= static_cast<uint8_t>(~(1u << (at % 8)));
  }
}

}  // namespace

uint32_t HidSensor::discover(const HidInputMap& map, SensorKind* kinds, uint8_t* ids) {
  uint32_t found = 0;
  for (const HidInputField& f : map.fields()) {
    if (f.usage_page != hid_usage::kPageSensor || (f.usage & kModifierMask) != 0) continue;
    for (const Layout& l : kLayouts) {
      if ((f.usage & kUsageMask) != l.first_usage) continue;
      bool known = false;
      for (uint32_t i = 0; i < found; ++i) known = known || (ids[i] == f.report_id && kinds[i] == l.kind);
      if (!known && found < kMaxPerDevice) {
        kinds[found] = l.kind;
        ids[found++] = f.report_id;
      }
    }
  }
  return found;
}

bool HidSensor::initialize() {
  const HidInputMap& map = device_.map();
  for (const Layout& l : kLayouts) {
    if (l.kind != kind_) continue;
    axes_ = l.axes;
    for (uint32_t a = 0; a < l.axes; ++a) {
      fields_[a] = data_field(map, report_id_, static_cast<uint16_t>(l.first_usage + a));
      if (!fields_[a]) return false;
      scale_[a] = power_of_ten(fields_[a]->unit_exponent);
    }
  }
  if (axes_ == 0) return false;
  latest_.kind = kind_;
  enable();                                    // (a device that cannot be told may report anyway)
  return device_.add_client(this);
}

// Powers the sensor and asks for all events: the feature report of the properties, with the selector indexes in their fields (the other fields of the report stay 0).
bool HidSensor::enable() {
  const HidInputMap& map = device_.map();
  uint8_t id = 0;
  bool have_id = false;
  if (!fields_[0]) return false;
  uint8_t buffer[64] = {};
  uint32_t size = 0;
  for (const HidFeature& f : map.features()) {
    if (f.owner_page != hid_usage::kPageSensor || f.application != fields_[0]->application) continue;      // (this sensor's own properties)
    const uint16_t property = f.owner_usage & kUsageMask;
    const uint16_t wanted = property == kPropertyPowerState ? kPowerD0 : property == kPropertyReportingState ? kReportingAllEvents : 0;
    if (!wanted) continue;
    uint32_t value = 0;
    if (f.is_array) {
      bool found = false;
      for (uint32_t i = 0; i < f.usage_count; ++i)
        if (f.usages[i] == wanted) { value = i; found = true; }
      if (!found) continue;
    } else {
      value = 1;
    }
    // the report is shared by the sensors of a hub: only one report id is written per call
    if (!have_id) { id = f.report_id; have_id = true; size = map.feature_report_bytes(id); if (size == 0 || size > sizeof buffer) return false; }
    if (f.report_id != id) continue;
    buffer[0] = id;
    put_bits(ByteView(buffer, size), f.bit_offset, f.bit_size, value);
  }
  if (!have_id) return false;
  const bool ids = map.uses_report_ids();
  return device_.device().set_feature(ids ? id : 0, ConstByteView(buffer + (ids ? 1 : 0), size - (ids ? 1 : 0)));
}

void HidSensor::on_report(ConstByteView report) {
  const bool ids = device_.map().uses_report_ids();
  if (ids && (report.empty() || report[0] != report_id_)) return;
  float v[3] = {};
  for (uint32_t a = 0; a < axes_; ++a) v[a] = static_cast<float>(hid_extract(report, fields_[a]->field(), report_id_)) * scale_[a];
  kernel::ScopedMutex guard(mutex_);
  latest_.x = v[0];
  latest_.y = v[1];
  latest_.z = v[2];
  ++latest_.sequence;
  have_ = true;
}

bool HidSensor::read(SensorReading* out) {
  device_.pump();
  kernel::ScopedMutex guard(mutex_);
  if (!have_) return false;
  if (out) *out = latest_;
  return true;
}

}  // namespace hardware
