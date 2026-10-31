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

#include "Hardware/Pen/I2c/I2cHidPen.hpp"

namespace hardware {

namespace {
// degrees from a tilt field: its physical range in degrees when it has one, else -90..90 across the logical range
int16_t tilt_degrees(const HidInputField& f, int32_t raw) {
  const int32_t lo = f.logical_min, hi = f.logical_max;
  if (hi <= lo) return 0;
  int32_t plo = f.physical_min, phi = f.physical_max;
  if (phi <= plo) { plo = -90; phi = 90; }
  if (raw < lo) raw = lo;
  if (raw > hi) raw = hi;
  const int64_t degrees = plo + (static_cast<int64_t>(raw) - lo) * (static_cast<int64_t>(phi) - plo) / (static_cast<int64_t>(hi) - lo);
  return static_cast<int16_t>(degrees < -90 ? -90 : degrees > 90 ? 90 : degrees);
}
}  // namespace

bool I2cHidPen::initialize() {
  const HidInputMap& map = device_.map();
  const HidInputField* pressure = nullptr;
  for (const HidInputField& f : map.fields())
    if (f.usage_page == hid_usage::kPageDigitizer && f.usage == hid_usage::kTipPressure && f.slot < 0) { pressure = &f; break; }
  // the pen report: the one with tip pressure, or else the one with a barrel switch / eraser / invert (a pen without pressure)
  uint8_t id = 0;
  bool found = false;
  if (pressure) {
    id = pressure->report_id;
    found = true;
  } else {
    for (const HidInputField& f : map.fields())
      if (f.usage_page == hid_usage::kPageDigitizer && f.slot < 0 && (f.usage == hid_usage::kBarrelSwitch || f.usage == hid_usage::kInvert || f.usage == hid_usage::kEraser)) { id = f.report_id; found = true; break; }
  }
  if (!found) return false;
  const int8_t none = -1;
  x_ = map.find(id, hid_usage::kPageGenericDesktop, hid_usage::kX, none);
  y_ = map.find(id, hid_usage::kPageGenericDesktop, hid_usage::kY, none);
  if (!x_ || !y_ || x_->logical_max <= x_->logical_min || y_->logical_max <= y_->logical_min) return false;
  report_id_ = id;
  tip_ = map.find(id, hid_usage::kPageDigitizer, hid_usage::kTipSwitch, none);
  range_ = map.find(id, hid_usage::kPageDigitizer, hid_usage::kInRange, none);
  invert_ = map.find(id, hid_usage::kPageDigitizer, hid_usage::kInvert, none);
  barrel_ = map.find(id, hid_usage::kPageDigitizer, hid_usage::kBarrelSwitch, none);
  secondary_ = map.find(id, hid_usage::kPageDigitizer, hid_usage::kSecondaryBarrel, none);
  eraser_ = map.find(id, hid_usage::kPageDigitizer, hid_usage::kEraser, none);
  pressure_ = map.find(id, hid_usage::kPageDigitizer, hid_usage::kTipPressure, none);
  tilt_x_ = map.find(id, hid_usage::kPageDigitizer, hid_usage::kXTilt, none);
  tilt_y_ = map.find(id, hid_usage::kPageDigitizer, hid_usage::kYTilt, none);
  range_x_ = static_cast<uint32_t>(x_->logical_max - x_->logical_min);
  range_y_ = static_cast<uint32_t>(y_->logical_max - y_->logical_min);
  range_pressure_ = pressure_ && pressure_->logical_max > pressure_->logical_min ? static_cast<uint32_t>(pressure_->logical_max - pressure_->logical_min) : 0;
  if (range_pressure_ > 0xFFFF) range_pressure_ = 0xFFFF;
  if (range_pressure_ == 0) pressure_ = nullptr;
  return device_.add_client(this);
}

void I2cHidPen::on_report(ConstByteView report) {
  const bool ids = device_.map().uses_report_ids();
  if (ids && (report.empty() || report[0] != report_id_)) return;
  if (!ids && report_id_ != 0) return;
  auto flag = [&](const HidInputField* f) { return f && hid_extract(report, f->field(), report_id_) != 0; };
  PenEvent e;
  e.tip = flag(tip_);
  e.inverted = flag(invert_);
  e.in_range = range_ ? flag(range_) : e.tip;
  if (flag(barrel_)) e.buttons |= pen_button::kBarrel;
  if (flag(secondary_)) e.buttons |= pen_button::kSecondary;
  if (flag(eraser_)) e.buttons |= pen_button::kEraser;
  e.max_x = range_x_;
  e.max_y = range_y_;
  const int32_t x = hid_extract(report, x_->field(), report_id_) - x_->logical_min, y = hid_extract(report, y_->field(), report_id_) - y_->logical_min;
  e.x = x < 0 ? 0 : static_cast<uint32_t>(x) > range_x_ ? range_x_ : static_cast<uint32_t>(x);
  e.y = y < 0 ? 0 : static_cast<uint32_t>(y) > range_y_ ? range_y_ : static_cast<uint32_t>(y);
  if (pressure_) {
    const int32_t p = hid_extract(report, pressure_->field(), report_id_) - pressure_->logical_min;
    e.max_pressure = static_cast<uint16_t>(range_pressure_);
    e.pressure = p < 0 ? 0 : static_cast<uint32_t>(p) > range_pressure_ ? static_cast<uint16_t>(range_pressure_) : static_cast<uint16_t>(p);
  }
  if (tilt_x_) e.tilt_x = tilt_degrees(*tilt_x_, hid_extract(report, tilt_x_->field(), report_id_));
  if (tilt_y_) e.tilt_y = tilt_degrees(*tilt_y_, hid_extract(report, tilt_y_->field(), report_id_));
  kernel::ScopedMutex guard(mutex_);
  if (count_ == kQueue) {                                   // the oldest goes: a program that does not read must not stall the device
    head_ = (head_ + 1) % kQueue;
    --count_;
  }
  queue_[(head_ + count_) % kQueue] = e;
  ++count_;
}

bool I2cHidPen::poll(PenEvent* out) {
  device_.pump();
  kernel::ScopedMutex guard(mutex_);
  if (count_ == 0) return false;
  if (out) *out = queue_[head_];
  head_ = (head_ + 1) % kQueue;
  --count_;
  return true;
}

}  // namespace hardware
