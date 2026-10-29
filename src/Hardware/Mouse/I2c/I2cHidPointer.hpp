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


// A pointing device on an I2C-HID link: the touchpad (a Windows Precision Touchpad: contacts with tip, confidence, id and position, a contact count, the click button of the pad),
// the touchscreen (the same contacts, reported as a tablet) or a mouse-mode HID pointer. Read when its attention line (the interrupt pin, a GPIO) says there is a report; without
// the line it is read at most every 8 ms (which some devices answer with stale reports; the machine always has the line).
//
// The touchpad becomes a relative mouse: one finger moves the pointer (16 counts to the millimetre), two fingers scroll (a wheel notch for each 6 mm), a tap clicks (one finger:
// left, two: right, three: middle; a touch shorter than 220 ms that moves less than 2.5 mm), the pad's own button clicks (the bottom right quarter is the right button). A touch
// the device marks as not confident (a palm) is ignored. The touchscreen becomes an absolute pointer: the first finger down is the left button at its position.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/I2c/HidInputMap.hpp"
#include "Hardware/Bus/I2c/IAttentionLine.hpp"
#include "Hardware/Bus/I2c/I2cHidDevice.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/Mouse/IMouse.hpp"

namespace hardware {

class I2cHidPointer final : public IMouse {
 public:
  enum class Kind : uint8_t { None, Touchpad, Touchscreen, Mouse };
  static constexpr uint32_t kMaxContacts = 10, kQueue = 64;

  I2cHidPointer(allocators::IAllocator& allocator, II2cBus& bus, uint16_t address, uint16_t descriptor_register, IClock& clock, const char* name)
      : device_(allocator, bus, address, descriptor_register), map_(allocator), clock_(clock), name_(name) {}

  void set_attention(IAttentionLine* line) { attention_ = line; }
  Kind kind() const { return kind_; }
  const I2cHidDevice& device() const { return device_; }

  const char* name() const override { return name_; }
  bool initialize() override;
  void shutdown() override;
  bool poll(MouseEvent* out) override;
  bool is_absolute() const override { return kind_ == Kind::Touchscreen; }
  uint32_t max_x() const override { return kind_ == Kind::Touchscreen ? range_x_ : 0; }
  uint32_t max_y() const override { return kind_ == Kind::Touchscreen ? range_y_ : 0; }

 private:
  struct Contact {
    bool down = false;               // tip switch (and confident)
    uint32_t id = 0;
    int32_t x = 0, y = 0;
  };
  struct Track {
    bool active = false;
    uint32_t id = 0;
    int32_t x = 0, y = 0;
  };

  bool read_one();
  void process(ConstByteView report);
  void process_mouse(ConstByteView report);
  void collect(ConstByteView report);
  void frame(const Contact* contacts, uint32_t count, bool button, uint32_t first_slot);
  void touchpad_frame(const Contact* active, uint32_t n, bool button);
  void touchscreen_frame(const Contact* active, uint32_t n);
  void push(int32_t dx, int32_t dy, int32_t wheel, uint8_t buttons, bool absolute = false, uint32_t x = 0, uint32_t y = 0);
  bool pop(MouseEvent* out);

  I2cHidDevice device_;
  HidInputMap map_;
  IClock& clock_;
  const char* name_;
  IAttentionLine* attention_ = nullptr;
  Kind kind_ = Kind::None;
  bool ready_ = false;
  uint8_t report_id_ = 0;                     // the report with the contacts
  uint32_t slots_ = 0;
  HidPointerLayout pointer_;
  uint32_t range_x_ = 0, range_y_ = 0;        // logical ranges
  int32_t min_x_ = 0, min_y_ = 0;
  float mm_per_x_ = 0.1f, mm_per_y_ = 0.1f;   // millimetres to a unit of the pad
  uint64_t next_read_ms_ = 0;
  uint32_t failures_ = 0;
  uint8_t buffer_[I2cHidDevice::kMaxInput] = {};

  // the frame being assembled (a frame can arrive in several reports)
  Contact frame_contacts_[kMaxContacts];
  uint32_t frame_count_ = 0, frame_expected_ = 0, frame_collected_ = 0;
  bool frame_open_ = false;

  // the touchpad's gesture state
  Track previous_[kMaxContacts];
  uint32_t previous_count_ = 0;
  bool session_ = false;
  uint64_t session_start_ms_ = 0;
  float session_travel_mm_ = 0;
  uint32_t session_fingers_ = 0;
  bool session_clicked_ = false;
  float scroll_mm_ = 0;
  float remainder_x_ = 0, remainder_y_ = 0;
  uint8_t buttons_ = 0;
  bool button_down_ = false;
  // the touchscreen
  bool touch_down_ = false;
  uint32_t touch_x_ = 0, touch_y_ = 0;

  MouseEvent queue_[kQueue] = {};
  uint32_t head_ = 0, count_ = 0;
};

}  // namespace hardware
