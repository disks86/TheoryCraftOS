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

// The pen of an I2C-HID digitizer: the report of the Digitizers page (0x0D) with tip pressure (usage 0x30) and the pen's own X and Y, found in the device's report descriptor. Fields it
// uses when the report has them: tip switch, in range, invert (the eraser end), barrel switch, secondary barrel switch, eraser, X / Y tilt (degrees; the logical range is mapped across
// the physical range if the descriptor gives one, else across -90..90). A device that reports no in-range bit is taken to be in range while the tip touches. The reports reach the pen
// through I2cHidPointer (the client side), which reads the device for it.
#pragma once
#include "Hardware/Bus/I2c/HidInputMap.hpp"
#include "Hardware/Bus/I2c/I2cHidClient.hpp"
#include "Hardware/Mouse/I2c/I2cHidPointer.hpp"
#include "Hardware/Pen/IPen.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

class I2cHidPen final : public IPen, public II2cHidClient {
 public:
  static constexpr uint32_t kQueue = 64;
  explicit I2cHidPen(I2cHidPointer& device, const char* name) : device_(device), name_(name) {}

  // Looks for the pen report in the device's map. False if there is none.
  bool initialize() override;
  const char* name() const override { return name_; }
  void shutdown() override { device_.remove_client(this); }
  bool poll(PenEvent* out) override;
  uint32_t max_x() const override { return range_x_; }
  uint32_t max_y() const override { return range_y_; }
  uint16_t max_pressure() const override { return pressure_ ? static_cast<uint16_t>(range_pressure_) : 0; }
  bool has_tilt() const override { return tilt_x_ || tilt_y_; }
  void on_report(ConstByteView report) override;

 private:
  I2cHidPointer& device_;
  const char* name_;
  kernel::Mutex mutex_;
  uint8_t report_id_ = 0;
  const HidInputField *x_ = nullptr, *y_ = nullptr, *tip_ = nullptr, *range_ = nullptr, *invert_ = nullptr, *barrel_ = nullptr, *secondary_ = nullptr, *eraser_ = nullptr, *pressure_ = nullptr,
                      *tilt_x_ = nullptr, *tilt_y_ = nullptr;
  uint32_t range_x_ = 0, range_y_ = 0, range_pressure_ = 0;
  PenEvent queue_[kQueue] = {};
  uint32_t head_ = 0, count_ = 0;
};

}  // namespace hardware
