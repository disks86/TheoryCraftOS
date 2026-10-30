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


#include "Hardware/Bus/I2c/HidInputMap.hpp"

namespace hardware {

namespace {

constexpr uint32_t kMaxCollectionDepth = 16, kMaxPushed = 4, kMaxUsages = 64, kMaxReportBits = 8 * 1024;

struct Globals {
  uint16_t usage_page = 0;
  int32_t logical_min = 0, logical_max = 0, physical_min = 0, physical_max = 0, unit = 0, unit_exponent = 0;
  uint32_t report_size = 0, report_count = 0;
  uint8_t report_id = 0;
};

struct Collection {
  uint16_t page = 0, usage = 0;
  int8_t slot = -1;
};

int32_t item_value(const uint8_t* p, uint32_t size, bool is_signed) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < size; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  if (is_signed && size > 0 && size < 4 && (v & (1u << (8 * size - 1)))) v |= ~0u << (8 * size);
  return static_cast<int32_t>(v);
}

}  // namespace

bool HidInputMap::parse(ConstByteView d) {
  fields_.clear();
  application_count_ = 0;
  uses_ids_ = false;
  for (uint32_t& b : report_bits_) b = 0;
  for (uint8_t& s : slots_) s = 0;

  Globals g;
  Globals pushed[kMaxPushed];
  uint32_t pushed_count = 0;
  Collection stack[kMaxCollectionDepth];
  uint32_t depth = 0;
  uint32_t usages[kMaxUsages];
  uint16_t usage_pages[kMaxUsages];
  uint32_t usage_count = 0;
  uint32_t range_min = 0, range_max = 0;
  bool have_range = false;

  uint64_t at = 0;
  while (at < d.size()) {
    const uint8_t prefix = d[at++];
    if (prefix == 0xFE) {                                       // a long item: skip it
      if (at + 2 > d.size()) return false;
      const uint32_t length = d[at];
      at += 2;
      if (at + length > d.size()) return false;
      at += length;
      continue;
    }
    const uint32_t size = (prefix & 3) == 3 ? 4 : (prefix & 3);
    const uint32_t type = (prefix >> 2) & 3, tag = prefix >> 4;
    if (at + size > d.size()) return false;
    uint8_t raw[4] = {};
    for (uint32_t i = 0; i < size; ++i) raw[i] = d[at + i];
    at += size;
    const uint32_t unsigned_value = static_cast<uint32_t>(item_value(raw, size, false));
    const int32_t signed_value = item_value(raw, size, true);

    if (type == 1) {                                            // global
      switch (tag) {
        case 0: g.usage_page = static_cast<uint16_t>(unsigned_value); break;
        case 1: g.logical_min = signed_value; break;
        case 2: g.logical_max = signed_value; break;
        case 3: g.physical_min = signed_value; break;
        case 4: g.physical_max = signed_value; break;
        case 5: g.unit_exponent = signed_value > 7 ? signed_value - 16 : signed_value; break;            // (a 4 bit signed value)
        case 6: g.unit = static_cast<int32_t>(unsigned_value); break;
        case 7: g.report_size = unsigned_value; break;
        case 8:
          g.report_id = static_cast<uint8_t>(unsigned_value);
          if (unsigned_value == 0 || unsigned_value > 255) return false;
          uses_ids_ = true;
          if (report_bits_[g.report_id] == 0) report_bits_[g.report_id] = 8;           // (the id byte)
          break;
        case 9: g.report_count = unsigned_value; break;
        case 10: if (pushed_count >= kMaxPushed) return false; pushed[pushed_count++] = g; break;
        case 11: if (pushed_count == 0) return false; g = pushed[--pushed_count]; break;
        default: break;
      }
    } else if (type == 2) {                                     // local
      switch (tag) {
        case 0:
          if (usage_count < kMaxUsages) {
            usages[usage_count] = size == 4 ? (unsigned_value & 0xFFFF) : unsigned_value;
            usage_pages[usage_count++] = size == 4 ? static_cast<uint16_t>(unsigned_value >> 16) : g.usage_page;
          } else {
            return false;
          }
          break;
        case 1: range_min = unsigned_value; have_range = true; break;
        case 2: range_max = unsigned_value; have_range = true; break;
        default: break;
      }
    } else if (type == 0) {                                     // main
      if (have_range && range_max >= range_min && range_max - range_min < kMaxUsages) {
        for (uint32_t u = range_min; u <= range_max && usage_count < kMaxUsages; ++u) {
          usages[usage_count] = u;
          usage_pages[usage_count++] = g.usage_page;
        }
      }
      if (tag == 10) {                                          // collection
        if (depth >= kMaxCollectionDepth) return false;
        Collection c;
        c.page = usage_count > 0 ? usage_pages[0] : g.usage_page;
        c.usage = usage_count > 0 ? static_cast<uint16_t>(usages[0]) : 0;
        if (c.page == hid_usage::kPageDigitizer && c.usage == hid_usage::kFinger) c.slot = static_cast<int8_t>(slots_[g.report_id] < HidInputMap::kMaxSlots - 1 ? slots_[g.report_id]++ : HidInputMap::kMaxSlots - 1);
        if (depth == 0 && unsigned_value == 1 && application_count_ < kMaxApplications) {      // an application collection
          applications_[application_count_].usage_page = c.page;
          applications_[application_count_].usage = c.usage;
          ++application_count_;
        }
        stack[depth++] = c;
      } else if (tag == 12) {                                   // end collection
        if (depth == 0) return false;
        --depth;
      } else if (tag == 8) {                                    // input
        const bool constant = unsigned_value & 1, variable = unsigned_value & 2, relative = unsigned_value & 4;
        const uint64_t bits = static_cast<uint64_t>(g.report_size) * g.report_count;
        if (g.report_size > 32 || bits + report_bits_[g.report_id] > kMaxReportBits) return false;
        if (!constant && variable) {
          int8_t slot = -1;
          for (uint32_t k = depth; k > 0; --k)
            if (stack[k - 1].slot >= 0) { slot = stack[k - 1].slot; break; }
          for (uint32_t i = 0; i < g.report_count; ++i) {
            HidInputField f;
            f.report_id = g.report_id;
            const uint32_t ui = usage_count == 0 ? 0 : (i < usage_count ? i : usage_count - 1);
            f.usage_page = usage_count == 0 ? g.usage_page : usage_pages[ui];
            f.usage = usage_count == 0 ? 0 : static_cast<uint16_t>(usages[ui]);
            f.bit_offset = report_bits_[g.report_id] + i * g.report_size;
            f.bit_size = static_cast<uint8_t>(g.report_size);
            f.is_signed = g.logical_min < 0;
            f.relative = relative;
            f.logical_min = g.logical_min;
            f.logical_max = g.logical_max;
            f.physical_min = g.physical_min;
            f.physical_max = g.physical_max;
            f.unit = g.unit;
            f.unit_exponent = g.unit_exponent;
            f.slot = slot;
            if (fields_.count() >= kMaxFields || !fields_.push_back(f)) return false;
          }
        }
        report_bits_[g.report_id] += static_cast<uint32_t>(bits);
      }
      usage_count = 0;
      have_range = false;
      range_min = range_max = 0;
    }
  }
  return depth == 0;
}

const HidInputField* HidInputMap::find(uint8_t report_id, uint16_t page, uint16_t usage, int8_t slot) const {
  for (const HidInputField& f : fields())
    if (f.report_id == report_id && f.usage_page == page && f.usage == usage && f.slot == slot) return &f;
  return nullptr;
}

uint32_t HidInputMap::slots(uint8_t report_id) const { return slots_[report_id]; }

uint32_t HidInputMap::report_bytes(uint8_t report_id) const {
  uint32_t bits = report_bits_[report_id];
  if (report_id == 0 && !uses_ids_) return (bits + 7) / 8;
  return (bits + 7) / 8;
}

bool HidInputMap::has_application(uint16_t page, uint16_t usage) const {
  for (uint32_t i = 0; i < application_count_; ++i)
    if (applications_[i].usage_page == page && applications_[i].usage == usage) return true;
  return false;
}

}  // namespace hardware
