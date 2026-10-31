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

#include "Hardware/Hid/HidDescriptor.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace hid {

namespace {
constexpr uint32_t kMaxUsages = 32, kMaxStack = 4, kMaxDepth = 16;

struct Globals {
  uint16_t page = 0;
  int32_t logical_min = 0, logical_max = 0;
  uint32_t report_size = 0, report_count = 0;
  uint8_t report_id = 0;
};

int32_t sign_extend(uint32_t value, uint32_t bytes) {
  if (bytes == 1) return static_cast<int8_t>(value);
  if (bytes == 2) return static_cast<int16_t>(value);
  return static_cast<int32_t>(value);
}

InputReport* report_for(ReportMap& map, uint8_t id) {
  for (uint32_t i = 0; i < map.report_count; ++i)
    if (map.reports[i].id == id) return &map.reports[i];
  if (map.report_count == ReportMap::kMaxReports) { map.truncated = true; return nullptr; }
  InputReport* r = &map.reports[map.report_count++];
  *r = InputReport();
  r->id = id;
  return r;
}

void add_field(InputReport& r, const InputField& f) {
  if (r.field_count == InputReport::kMaxFields) { r.truncated = true; return; }
  r.fields[r.field_count++] = f;
}
}

const InputReport* ReportMap::find(uint8_t id) const {
  for (uint32_t i = 0; i < report_count; ++i)
    if (reports[i].id == id) return &reports[i];
  return nullptr;
}

const InputReport* ReportMap::find_application(uint16_t page, uint16_t usage) const {
  for (uint32_t i = 0; i < report_count; ++i)
    if (reports[i].app_page == page && reports[i].app_usage == usage) return &reports[i];
  return nullptr;
}

bool parse_report_map(ConstByteView descriptor, ReportMap* out) {
  *out = ReportMap();
  Globals g;
  Globals stack[kMaxStack];
  uint32_t stack_size = 0;
  uint16_t usages[kMaxUsages] = {};
  uint32_t usage_count = 0;
  uint16_t usage_min = 0, usage_max = 0;
  bool has_range = false;
  uint16_t collection_usage[kMaxDepth] = {};         // the usage that opened each open collection
  uint16_t collection_page[kMaxDepth] = {};
  uint32_t depth = 0;
  uint16_t app_page = 0, app_usage = 0;               // the outermost (application) collection

  ByteReader in(descriptor);
  while (!in.at_end()) {
    const uint8_t prefix = in.u8();
    if (prefix == 0xFE) {                              // a long item: a size, a tag, the data; nothing in HID 1.11 uses them
      const uint8_t long_size = in.u8();
      in.skip(1 + uint64_t{long_size});
      if (!in.ok()) break;
      continue;
    }
    const uint32_t size = (prefix & 3) == 3 ? 4 : (prefix & 3);
    const uint8_t type = (prefix >> 2) & 3, tag = prefix >> 4;
    const ConstByteView body = in.take(size);
    if (!in.ok()) break;                               // an item that runs past the end
    uint32_t data = 0;
    for (uint32_t b = 0; b < size; ++b) data |= uint32_t{body[b]} << (8 * b);

    if (type == 1) {                                   // global
      switch (tag) {
        case 0: g.page = static_cast<uint16_t>(data); break;
        case 1: g.logical_min = sign_extend(data, size); break;
        case 2: g.logical_max = size == 4 ? static_cast<int32_t>(data) : (g.logical_min < 0 ? sign_extend(data, size) : static_cast<int32_t>(data)); break;
        case 7: g.report_size = data; break;
        case 8: g.report_id = static_cast<uint8_t>(data); break;
        case 9: g.report_count = data; break;
        case 10: if (stack_size < kMaxStack) stack[stack_size++] = g; break;     // push
        case 11: if (stack_size > 0) g = stack[--stack_size]; break;             // pop
        default: break;
      }
    } else if (type == 2) {                            // local
      switch (tag) {
        case 0: if (usage_count < kMaxUsages) usages[usage_count++] = static_cast<uint16_t>(data); break;
        case 1: usage_min = static_cast<uint16_t>(data); has_range = true; break;
        case 2: usage_max = static_cast<uint16_t>(data); has_range = true; break;
        default: break;
      }
    } else if (type == 0) {                            // main
      if (tag == 0xA) {                                // collection
        const uint16_t usage = usage_count ? usages[0] : 0;
        if (depth < kMaxDepth) { collection_usage[depth] = usage; collection_page[depth] = g.page; }
        if (depth == 0) { app_page = g.page; app_usage = usage; }
        if (depth < 0xFFFF) ++depth;
      } else if (tag == 0xC) {                         // end collection
        if (depth > 0) --depth;
        if (depth == 0) app_page = app_usage = 0;
      } else if (tag == 8) {                           // input
        InputReport* report = report_for(*out, g.report_id);
        const bool constant = data & 1, variable = data & 2, relative = data & 4;
        const uint64_t total_bits = uint64_t{g.report_size} * g.report_count;
        if (report && total_bits <= 0xFFFFFF && report->bits <= 0xFFFFFF) {
          if (report->app_page == 0 && report->app_usage == 0) { report->app_page = app_page; report->app_usage = app_usage; }
          const uint32_t base = report->bits;
          if (!constant && g.report_size > 0 && g.report_size <= 32) {
            InputField f;
            f.page = g.page;
            f.logical_min = g.logical_min;
            f.logical_max = g.logical_max;
            f.relative = relative;
            f.bit_size = g.report_size;
            if (variable && !has_range && usage_count > 0) {
              // one field per usage; the elements beyond the usages share the last one
              for (uint32_t n = 0; n < g.report_count && n < kMaxUsages; ++n) {
                const uint16_t usage = usages[n < usage_count ? n : usage_count - 1];
                f.usage_min = f.usage_max = usage;
                f.count = 1;
                f.variable = true;
                f.bit_offset = base + n * g.report_size;
                add_field(*report, f);
              }
            } else {
              f.variable = variable;
              f.usage_min = has_range ? usage_min : (usage_count ? usages[0] : 0);
              f.usage_max = has_range ? usage_max : f.usage_min;
              f.count = g.report_count;
              f.bit_offset = base;
              add_field(*report, f);
            }
          }
          report->bits = static_cast<uint32_t>(base + total_bits);
        }
      }
      usage_count = 0;                                 // every main item ends the local state
      has_range = false;
      usage_min = usage_max = 0;
    }
  }
  (void)collection_usage;
  (void)collection_page;
  return out->report_count != 0;
}

int32_t extract_bits(ConstByteView data, uint32_t bit_offset, uint32_t bits, bool is_signed) {
  if (bits == 0 || bits > 32) return 0;
  if (uint64_t{bit_offset} + bits > uint64_t{data.size()} * 8) return 0;
  uint64_t raw = 0;
  for (uint32_t b = 0; b < bits; ++b) {
    const uint32_t bit = bit_offset + b;
    if (data[bit / 8] & (1u << (bit % 8))) raw |= uint64_t{1} << b;
  }
  if (is_signed && (raw & (uint64_t{1} << (bits - 1)))) raw |= ~uint64_t{0} << bits;
  return static_cast<int32_t>(raw);
}

// ------------------------------------------------------------------ keyboards

bool KeyboardDecoder::init(const InputReport& report) {
  valid_ = has_modifiers_ = has_array_ = has_bitmap_ = false;
  for (uint32_t i = 0; i < report.field_count; ++i) {
    const InputField& f = report.fields[i];
    if (f.page != kPageKeyboard) continue;
    if (f.variable && f.bit_size == 1 && f.usage_min <= 0xE0 && f.usage_max >= 0xE7 && f.count >= 8 && !has_modifiers_) {
      modifiers_ = f;
      has_modifiers_ = true;
    } else if (!f.variable && f.bit_size >= 8 && f.bit_size <= 16 && f.count > 0 && !has_array_) {
      array_ = f;
      has_array_ = true;
    } else if (f.variable && f.bit_size == 1 && f.count > 8 && f.usage_min < 0xE0 && !has_bitmap_) {
      bitmap_ = f;
      has_bitmap_ = true;
    }
  }
  valid_ = has_modifiers_ || has_array_ || has_bitmap_;
  return valid_;
}

bool KeyboardDecoder::decode(ConstByteView data, KeyboardState* out) const {
  *out = KeyboardState();
  if (!valid_) return false;
  if (has_modifiers_) {
    if (uint64_t{modifiers_.bit_offset} + 8 > uint64_t{data.size()} * 8) return false;
    out->modifiers = static_cast<uint8_t>(extract_bits(data, modifiers_.bit_offset, 8, false));
  }
  if (has_array_) {
    if (uint64_t{array_.bit_offset} + uint64_t{array_.bit_size} * array_.count > uint64_t{data.size()} * 8) return false;
    for (uint32_t n = 0; n < array_.count; ++n) {
      const int32_t usage = extract_bits(data, array_.bit_offset + n * array_.bit_size, array_.bit_size, false);
      if (usage <= 3 || usage > 0xFF) continue;                          // none, or a rollover error
      if (out->key_count < KeyboardState::kMaxKeys) out->keys[out->key_count++] = static_cast<uint8_t>(usage);
    }
  }
  if (has_bitmap_) {
    if (uint64_t{bitmap_.bit_offset} + bitmap_.count > uint64_t{data.size()} * 8) return false;
    for (uint32_t n = 0; n < bitmap_.count; ++n) {
      const uint32_t usage = bitmap_.usage_min + n;
      if (usage < 4 || usage > 0xDF) continue;
      if (extract_bits(data, bitmap_.bit_offset + n, 1, false) && out->key_count < KeyboardState::kMaxKeys) out->keys[out->key_count++] = static_cast<uint8_t>(usage);
    }
  }
  return true;
}

// ------------------------------------------------------------------ consumer control keys

uint8_t consumer_usage_to_key(uint16_t usage) {
  switch (usage) {
    case 0xE9: return key::kVolumeUp;
    case 0xEA: return key::kVolumeDown;
    case 0xE2: return key::kMute;
    case 0xCD: return key::kMediaPlayPause;
    case 0xB7: return key::kMediaStop;
    case 0xB5: return key::kMediaNext;
    case 0xB6: return key::kMediaPrevious;
    case 0x6F: return key::kBrightnessUp;
    case 0x70: return key::kBrightnessDown;
    default: return 0;
  }
}

bool ConsumerDecoder::init(const InputReport& report) {
  valid_ = false;
  count_ = 0;
  bytes_ = 0;
  for (uint32_t i = 0; i < report.field_count; ++i) {
    const InputField& f = report.fields[i];
    if (f.page != kPageConsumer || f.bit_size == 0 || f.bit_size > 16 || f.count == 0) continue;
    bool useful = false;
    if (f.variable) {
      for (uint32_t n = 0; n < f.count && n < 0x10000; ++n) useful = useful || consumer_usage_to_key(static_cast<uint16_t>(f.usage_min + n)) != 0;
    } else {
      useful = true;                                          // (an array can name any usage; the ones we carry are found when a report arrives)
    }
    if (!useful || count_ == InputReport::kMaxFields) continue;
    fields_[count_++] = f;
    const uint32_t end = f.bit_offset + f.bit_size * f.count;
    if (end > bytes_) bytes_ = end;
  }
  bytes_ = (bytes_ + 7) / 8;
  valid_ = count_ != 0;
  return valid_;
}

bool ConsumerDecoder::decode(ConstByteView data, KeyboardState* out) const {
  *out = KeyboardState();
  if (!valid_ || data.size() < bytes_) return false;
  auto add = [&](uint16_t usage) {
    const uint8_t key = consumer_usage_to_key(usage);
    if (!key || out->key_count == KeyboardState::kMaxKeys) return;
    for (uint32_t i = 0; i < out->key_count; ++i)
      if (out->keys[i] == key) return;
    out->keys[out->key_count++] = key;
  };
  for (uint32_t i = 0; i < count_; ++i) {
    const InputField& f = fields_[i];
    if (f.variable) {
      for (uint32_t n = 0; n < f.count; ++n)
        if (extract_bits(data, f.bit_offset + n * f.bit_size, 1, false)) add(static_cast<uint16_t>(f.usage_min + n));
    } else {
      for (uint32_t n = 0; n < f.count; ++n) {
        const int32_t usage = extract_bits(data, f.bit_offset + n * f.bit_size, f.bit_size, false);
        if (usage > 0) add(static_cast<uint16_t>(usage));
      }
    }
  }
  return true;
}

// ------------------------------------------------------------------ pointers

bool PointerDecoder::init(const InputReport& report) {
  valid_ = has_wheel_ = false;
  relative_ = true;
  for (bool& b : has_button_) b = false;
  bool has_x = false, has_y = false;
  uint32_t end = 0;
  for (uint32_t i = 0; i < report.field_count; ++i) {
    const InputField& f = report.fields[i];
    if (f.bit_size == 0 || f.bit_size > 32) continue;
    const uint32_t field_end = f.bit_offset + f.bit_size * f.count;
    if (f.page == kPageButton && f.variable) {
      for (uint32_t n = 0; n < f.count; ++n) {
        const uint32_t usage = f.usage_min + n;
        if (usage >= 1 && usage <= 3 && f.bit_size == 1) {
          buttons_[usage - 1] = f;
          buttons_[usage - 1].bit_offset = f.bit_offset + n;
          has_button_[usage - 1] = true;
          if (field_end > end) end = field_end;
        }
      }
    } else if (f.page == kPageGenericDesktop && f.variable) {
      for (uint32_t n = 0; n < f.count; ++n) {
        const uint32_t usage = f.usage_min + n;
        InputField one = f;
        one.bit_offset = f.bit_offset + n * f.bit_size;
        one.count = 1;
        if (usage == kUsageX && !has_x) { x_ = one; has_x = true; }
        else if (usage == kUsageY && !has_y) { y_ = one; has_y = true; }
        else if (usage == kUsageWheel && !has_wheel_) { wheel_ = one; has_wheel_ = true; }
        else continue;
        if (one.bit_offset + one.bit_size > end) end = one.bit_offset + one.bit_size;
      }
    }
  }
  valid_ = has_x && has_y;
  relative_ = valid_ ? x_.relative : true;
  min_bytes_ = (end + 7) / 8;
  return valid_;
}

bool PointerDecoder::decode(ConstByteView data, MouseEvent* out) const {
  *out = MouseEvent{};
  if (!valid_ || data.size() < min_bytes_) return false;
  for (int b = 0; b < 3; ++b)
    if (has_button_[b] && extract_bits(data, buttons_[b].bit_offset, 1, false)) out->buttons |= static_cast<uint8_t>(1u << b);
  const int32_t x = extract_bits(data, x_.bit_offset, x_.bit_size, x_.logical_min < 0);
  const int32_t y = extract_bits(data, y_.bit_offset, y_.bit_size, y_.logical_min < 0);
  if (relative_) {
    out->dx = x;
    out->dy = y;
  } else {
    out->absolute = true;
    out->x = x < 0 ? 0 : static_cast<uint32_t>(x);
    out->y = y < 0 ? 0 : static_cast<uint32_t>(y);
    out->max_x = max_x();
    out->max_y = max_y();
  }
  if (has_wheel_) out->wheel = extract_bits(data, wheel_.bit_offset, wheel_.bit_size, wheel_.logical_min < 0);
  return true;
}

}  // namespace hid
}  // namespace hardware
