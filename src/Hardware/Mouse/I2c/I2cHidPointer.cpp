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


#include "Hardware/Mouse/I2c/I2cHidPointer.hpp"

namespace hardware {

namespace {

constexpr float kCountsPerMm = 16.0f, kScrollMm = 6.0f, kTapMaxMm = 2.5f;
constexpr uint64_t kTapMaxMs = 220, kPollEveryMs = 8, kRetryMs = 100;
constexpr uint32_t kReadsPerPoll = 8;

// millimetres to one logical unit of a coordinate field: from its physical range when the unit is centimetres or inches, else the pad is taken to be 100 mm across
float mm_per_unit(const HidInputField& f) {
  const float logical = static_cast<float>(f.logical_max) - static_cast<float>(f.logical_min);
  if (logical <= 0.0f) return 0.0f;
  const float physical = static_cast<float>(f.physical_max) - static_cast<float>(f.physical_min);
  if (physical > 0.0f && (f.unit == 0x11 || f.unit == 0x13)) {
    float scale = f.unit == 0x11 ? 10.0f : 25.4f;
    for (int32_t e = f.unit_exponent; e > 0; --e) scale *= 10.0f;
    for (int32_t e = f.unit_exponent; e < 0; ++e) scale /= 10.0f;
    return physical * scale / logical;
  }
  return 100.0f / logical;
}

float absolute(float v) { return v < 0 ? -v : v; }

}  // namespace

bool I2cHidPointer::initialize() {
  ready_ = false;
  kind_ = Kind::None;
  if (!device_.initialize() || !map_.parse(device_.report_descriptor())) return false;
  const HidInputField* tip = nullptr;
  for (const HidInputField& f : map_.fields())
    if (f.usage_page == hid_usage::kPageDigitizer && f.usage == hid_usage::kTipSwitch && f.slot >= 0) { tip = &f; break; }
  if (tip && (map_.has_application(hid_usage::kPageDigitizer, hid_usage::kTouchPad) || map_.has_application(hid_usage::kPageDigitizer, hid_usage::kTouchScreen))) {
    report_id_ = tip->report_id;
    slots_ = map_.slots(report_id_);
    if (slots_ > kMaxContacts) slots_ = kMaxContacts;
    const HidInputField* x = map_.find(report_id_, hid_usage::kPageGenericDesktop, hid_usage::kX, 0);
    const HidInputField* y = map_.find(report_id_, hid_usage::kPageGenericDesktop, hid_usage::kY, 0);
    if (slots_ == 0 || !x || !y || x->logical_max <= x->logical_min || y->logical_max <= y->logical_min) return false;
    range_x_ = static_cast<uint32_t>(x->logical_max - x->logical_min);
    range_y_ = static_cast<uint32_t>(y->logical_max - y->logical_min);
    min_x_ = x->logical_min;
    min_y_ = y->logical_min;
    mm_per_x_ = mm_per_unit(*x);
    mm_per_y_ = mm_per_unit(*y);
    kind_ = map_.has_application(hid_usage::kPageDigitizer, hid_usage::kTouchPad) ? Kind::Touchpad : Kind::Touchscreen;
  } else if (parse_hid_pointer(device_.report_descriptor(), &pointer_) && pointer_.valid && pointer_.x.relative) {
    kind_ = Kind::Mouse;
  } else {
    return false;
  }
  ready_ = true;
  return true;
}

void I2cHidPointer::shutdown() {
  if (ready_) device_.set_power(false);
  ready_ = false;
}

void I2cHidPointer::push(int32_t dx, int32_t dy, int32_t wheel, uint8_t buttons, bool abs, uint32_t x, uint32_t y) {
  if (count_ >= kQueue) return;
  MouseEvent& e = queue_[(head_ + count_) % kQueue];
  e.dx = dx;
  e.dy = dy;
  e.wheel = wheel;
  e.buttons = buttons;
  e.absolute = abs;
  e.x = x;
  e.y = y;
  e.max_x = abs ? range_x_ : 0;
  e.max_y = abs ? range_y_ : 0;
  ++count_;
}

bool I2cHidPointer::pop(MouseEvent* out) {
  if (count_ == 0) return false;
  *out = queue_[head_];
  head_ = (head_ + 1) % kQueue;
  --count_;
  return true;
}

bool I2cHidPointer::poll(MouseEvent* out) {
  if (!ready_) return false;
  if (pop(out)) return true;
  const uint64_t now = clock_.now_ms();
  if (now < next_read_ms_) return false;
  for (uint32_t n = 0; n < kReadsPerPoll && count_ == 0; ++n) {
    if (attention_ ? !attention_->asserted() : (n > 0 || now < next_read_ms_)) break;
    if (!read_one()) {
      if (++failures_ >= 3) next_read_ms_ = now + kRetryMs;          // a device that does not answer is left alone for a moment
      break;
    }
    if (!attention_) next_read_ms_ = now + kPollEveryMs;
  }
  return pop(out);
}

bool I2cHidPointer::read_one() {
  uint32_t length = 0;
  if (!device_.read_input(ByteView(buffer_, sizeof buffer_), &length)) return false;
  failures_ = 0;
  if (length != 0) process(ConstByteView(buffer_, length));
  return true;
}

void I2cHidPointer::process(ConstByteView report) {
  if (kind_ == Kind::Mouse) process_mouse(report);
  else collect(report);
}

void I2cHidPointer::process_mouse(ConstByteView report) {
  const uint8_t id = pointer_.report_id;
  if (id != 0 && (report.empty() || report[0] != id)) return;
  const int32_t dx = hid_extract(report, pointer_.x, id), dy = hid_extract(report, pointer_.y, id), wheel = pointer_.wheel.valid ? hid_extract(report, pointer_.wheel, id) : 0;
  uint8_t buttons = 0;
  const ArrayView<const HidField> b = pointer_.buttons();
  for (uint32_t i = 0; i < b.size() && i < 3; ++i)
    if (b[i].valid && hid_extract(report, b[i], id) != 0) buttons = static_cast<uint8_t>(buttons | (1u << i));
  push(dx, dy, wheel, buttons);
}

void I2cHidPointer::collect(ConstByteView report) {
  if (report_id_ != 0 && (report.empty() || report[0] != report_id_)) return;
  auto get = [&](const HidInputField* f) -> int32_t { return f ? hid_extract(report, f->field(), report_id_) : 0; };
  const HidInputField* count_field = map_.find(report_id_, hid_usage::kPageDigitizer, hid_usage::kContactCount, -1);
  if (count_field) {
    const int32_t count = get(count_field);
    if (count > 0) {
      frame_open_ = true;
      frame_expected_ = static_cast<uint32_t>(count) < 32 ? static_cast<uint32_t>(count) : 32;
      frame_collected_ = 0;
      frame_count_ = 0;
    } else if (!frame_open_ || frame_collected_ >= frame_expected_) {              // an empty frame: nothing touches
      frame_open_ = true;
      frame_expected_ = 0;
      frame_collected_ = 0;
      frame_count_ = 0;
    }
  } else {
    frame_open_ = true;
    frame_expected_ = slots_;
    frame_collected_ = 0;
    frame_count_ = 0;
  }
  const uint32_t room = frame_expected_ - frame_collected_;
  const uint32_t take = count_field ? (slots_ < room ? slots_ : room) : slots_;
  for (uint32_t s = 0; s < take; ++s) {
    const int8_t slot = static_cast<int8_t>(s);
    const HidInputField* tip = map_.find(report_id_, hid_usage::kPageDigitizer, hid_usage::kTipSwitch, slot);
    const HidInputField* conf = map_.find(report_id_, hid_usage::kPageDigitizer, hid_usage::kConfidence, slot);
    const HidInputField* id = map_.find(report_id_, hid_usage::kPageDigitizer, hid_usage::kContactId, slot);
    const HidInputField* x = map_.find(report_id_, hid_usage::kPageGenericDesktop, hid_usage::kX, slot);
    const HidInputField* y = map_.find(report_id_, hid_usage::kPageGenericDesktop, hid_usage::kY, slot);
    if (frame_count_ >= kMaxContacts) break;
    Contact c;
    c.down = (tip ? get(tip) != 0 : true) && (conf ? get(conf) != 0 : true);
    c.id = id ? static_cast<uint32_t>(get(id)) : s;
    c.x = get(x);
    c.y = get(y);
    frame_contacts_[frame_count_++] = c;
  }
  frame_collected_ += take;
  if (!count_field || frame_collected_ >= frame_expected_) {
    const HidInputField* button = map_.find(report_id_, hid_usage::kPageButton, 1, -1);
    frame_open_ = false;
    frame(frame_contacts_, frame_count_, button && get(button) != 0, 0);
  }
}

void I2cHidPointer::frame(const Contact* contacts, uint32_t count, bool button, uint32_t) {
  Contact active[kMaxContacts];
  uint32_t n = 0;
  for (uint32_t i = 0; i < count && n < kMaxContacts; ++i)
    if (contacts[i].down) active[n++] = contacts[i];
  if (kind_ == Kind::Touchpad) touchpad_frame(active, n, button);
  else touchscreen_frame(active, n);
}

void I2cHidPointer::touchscreen_frame(const Contact* active, uint32_t n) {
  if (n == 0) {
    if (touch_down_) push(0, 0, 0, 0, true, touch_x_, touch_y_);
    touch_down_ = false;
    return;
  }
  const Contact* first = &active[0];
  for (uint32_t i = 1; i < n; ++i)
    if (active[i].id < first->id) first = &active[i];
  int32_t x = first->x - min_x_, y = first->y - min_y_;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (static_cast<uint32_t>(x) > range_x_) x = static_cast<int32_t>(range_x_);
  if (static_cast<uint32_t>(y) > range_y_) y = static_cast<int32_t>(range_y_);
  if (!touch_down_ || static_cast<uint32_t>(x) != touch_x_ || static_cast<uint32_t>(y) != touch_y_) push(0, 0, 0, mouse_button::kLeft, true, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
  touch_down_ = true;
  touch_x_ = static_cast<uint32_t>(x);
  touch_y_ = static_cast<uint32_t>(y);
}

void I2cHidPointer::touchpad_frame(const Contact* active, uint32_t n, bool button) {
  const uint64_t now = clock_.now_ms();
  if (n > 0 && !session_) {
    session_ = true;
    session_start_ms_ = now;
    session_travel_mm_ = 0;
    session_fingers_ = 0;
    session_clicked_ = false;
    scroll_mm_ = 0;
    remainder_x_ = remainder_y_ = 0;
  }
  if (session_ && n > session_fingers_) session_fingers_ = n;

  // the pad's button: the right button in the bottom right quarter, the left elsewhere; the one chosen when it went down stays until it comes up
  if (button && !button_down_) {
    button_down_ = true;
    session_clicked_ = true;
    bool right = false;
    if (n >= 1) {
      const float fx = static_cast<float>(active[0].x - min_x_) / static_cast<float>(range_x_), fy = static_cast<float>(active[0].y - min_y_) / static_cast<float>(range_y_);
      right = fx >= 0.75f && fy >= 0.6f;
    }
    buttons_ = right ? mouse_button::kRight : mouse_button::kLeft;
    push(0, 0, 0, buttons_);
  } else if (!button && button_down_) {
    button_down_ = false;
    buttons_ = 0;
    push(0, 0, 0, 0);
  }

  // movement of the contacts that were there in the last frame as well
  auto before = [&](uint32_t id) -> const Track* {
    for (uint32_t i = 0; i < previous_count_; ++i)
      if (previous_[i].active && previous_[i].id == id) return &previous_[i];
    return nullptr;
  };
  if (n == 1 && previous_count_ == 1) {
    if (const Track* p = before(active[0].id)) {
      const float dxmm = static_cast<float>(active[0].x - p->x) * mm_per_x_, dymm = static_cast<float>(active[0].y - p->y) * mm_per_y_;
      session_travel_mm_ += absolute(dxmm) + absolute(dymm);
      const float cx = dxmm * kCountsPerMm + remainder_x_, cy = dymm * kCountsPerMm + remainder_y_;
      const int32_t ix = static_cast<int32_t>(cx), iy = static_cast<int32_t>(cy);
      remainder_x_ = cx - static_cast<float>(ix);
      remainder_y_ = cy - static_cast<float>(iy);
      if (ix != 0 || iy != 0) push(ix, iy, 0, buttons_);
    }
  } else if (n == 2 && previous_count_ == 2) {
    float sum = 0;
    uint32_t matched = 0;
    for (uint32_t i = 0; i < n; ++i)
      if (const Track* p = before(active[i].id)) {
        sum += static_cast<float>(active[i].y - p->y) * mm_per_y_;
        ++matched;
      }
    if (matched > 0) {
      const float mean = sum / static_cast<float>(matched);
      session_travel_mm_ += absolute(mean);
      scroll_mm_ += mean;
      const int32_t notches = static_cast<int32_t>(scroll_mm_ / kScrollMm);
      if (notches != 0) {
        scroll_mm_ -= static_cast<float>(notches) * kScrollMm;
        push(0, 0, notches, buttons_);
      }
    }
  }

  // a tap: everything lifted after a short touch that hardly moved
  if (n == 0 && session_) {
    if (!session_clicked_ && now - session_start_ms_ < kTapMaxMs && session_travel_mm_ < kTapMaxMm && session_fingers_ >= 1) {
      const uint8_t tap = session_fingers_ == 1 ? mouse_button::kLeft : session_fingers_ == 2 ? mouse_button::kRight : mouse_button::kMiddle;
      push(0, 0, 0, tap);
      push(0, 0, 0, 0);
    }
    session_ = false;
  }
  for (uint32_t i = 0; i < n; ++i) {
    previous_[i].active = true;
    previous_[i].id = active[i].id;
    previous_[i].x = active[i].x;
    previous_[i].y = active[i].y;
  }
  previous_count_ = n;
}

}  // namespace hardware
