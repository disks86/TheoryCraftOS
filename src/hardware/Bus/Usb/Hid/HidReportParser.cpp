#include "hardware/Bus/Usb/Hid/HidReportParser.hpp"

namespace hardware {

namespace {
constexpr uint16_t kPageGenericDesktop = 0x01;
constexpr uint16_t kPageButton = 0x09;
constexpr uint16_t kUsageX = 0x30;
constexpr uint16_t kUsageY = 0x31;
constexpr uint16_t kUsageWheel = 0x38;

constexpr uint32_t kMaxUsages = 16;

struct State {
  // Global items.
  uint16_t usage_page = 0;
  int32_t logical_min = 0, logical_max = 0;
  uint32_t report_size = 0, report_count = 0;
  uint8_t report_id = 0;
  // Local items (cleared after every main item).
  uint16_t usages[kMaxUsages] = {};
  uint32_t usage_count = 0;
  uint16_t usage_min = 0, usage_max = 0;
  bool has_range = false;

  void clear_locals() {
    usage_count = 0;
    has_range = false;
    usage_min = usage_max = 0;
  }
};

int32_t sign_extend(uint32_t value, uint32_t bytes) {
  if (bytes == 1) return static_cast<int8_t>(value);
  if (bytes == 2) return static_cast<int16_t>(value);
  return static_cast<int32_t>(value);
}

void assign(HidField& field, const State& s, uint32_t bit_offset, bool relative) {
  field.valid = true;
  field.bit_offset = bit_offset;
  field.bit_size = static_cast<uint8_t>(s.report_size);
  field.is_signed = s.logical_min < 0;
  field.relative = relative;
  field.logical_min = s.logical_min;
  field.logical_max = s.logical_max;
}
}  // namespace

bool parse_hid_pointer(const uint8_t* d, uint16_t length, HidPointerLayout* out) {
  if (!d || !out) return false;
  *out = HidPointerLayout{};

  State s;
  uint32_t bit_offset = 0;  // position within the report of the next Input field
  for (uint16_t i = 0; i < length;) {
    const uint8_t prefix = d[i];
    if (prefix == 0xFE) {  // long item: skip it whole
      if (i + 1 >= length) break;
      i += 3 + d[i + 1];
      continue;
    }
    const uint32_t size = (prefix & 3) == 3 ? 4 : (prefix & 3);
    const uint8_t type = (prefix >> 2) & 3;  // 0 main, 1 global, 2 local
    const uint8_t tag = prefix >> 4;
    if (i + 1 + size > length) break;
    uint32_t data = 0;
    for (uint32_t b = 0; b < size; ++b) data |= uint32_t{d[i + 1 + b]} << (8 * b);
    i += 1 + size;

    if (type == 1) {  // global
      switch (tag) {
        case 0: s.usage_page = static_cast<uint16_t>(data); break;
        case 1: s.logical_min = sign_extend(data, size); break;
        case 2: s.logical_max = sign_extend(data, size); break;
        case 7: s.report_size = data; break;
        case 8: s.report_id = static_cast<uint8_t>(data); break;
        case 9: s.report_count = data; break;
        default: break;
      }
    } else if (type == 2) {  // local
      switch (tag) {
        case 0:
          if (s.usage_count < kMaxUsages) s.usages[s.usage_count++] = static_cast<uint16_t>(data);
          break;
        case 1: s.usage_min = static_cast<uint16_t>(data); s.has_range = true; break;
        case 2: s.usage_max = static_cast<uint16_t>(data); s.has_range = true; break;
        default: break;
      }
    } else if (type == 0) {  // main
      if (tag == 8) {        // Input
        const bool constant = data & 0x01;
        const bool variable = data & 0x02;
        const bool relative = data & 0x04;
        // Reports with an ID carry it as a leading byte; only one ID is supported.
        const uint32_t base = s.report_id ? bit_offset + 8 : bit_offset;
        if (!constant && variable && s.report_size) {
          for (uint32_t n = 0; n < s.report_count; ++n) {
            uint16_t usage = 0;
            if (s.has_range) usage = s.usage_min + n;
            else if (s.usage_count) usage = s.usages[n < s.usage_count ? n : s.usage_count - 1];
            const uint32_t at = base + n * s.report_size;
            if (s.usage_page == kPageButton && usage >= 1 && usage <= 3) {
              assign(out->buttons[usage - 1], s, at, false);
            } else if (s.usage_page == kPageGenericDesktop) {
              if (usage == kUsageX) assign(out->x, s, at, relative);
              else if (usage == kUsageY) assign(out->y, s, at, relative);
              else if (usage == kUsageWheel) assign(out->wheel, s, at, relative);
            }
            if (s.report_id) out->report_id = s.report_id;
          }
        }
        bit_offset += s.report_count * s.report_size;
      }
      s.clear_locals();  // Output, Feature, Collection ... all end the local state
    }
  }
  out->valid = out->x.valid && out->y.valid;
  return out->valid;
}

int32_t hid_extract(const uint8_t* report, uint16_t report_length, const HidField& f,
                    uint8_t report_id) {
  if (!f.valid || f.bit_size == 0 || f.bit_size > 32) return 0;
  if (report_id && (report_length == 0 || report[0] != report_id)) return 0;
  if ((f.bit_offset + f.bit_size + 7) / 8 > report_length) return 0;

  uint64_t raw = 0;
  for (uint32_t b = 0; b < f.bit_size; ++b) {
    const uint32_t bit = f.bit_offset + b;
    if (report[bit / 8] & (1u << (bit % 8))) raw |= uint64_t{1} << b;
  }
  if (f.is_signed && (raw & (uint64_t{1} << (f.bit_size - 1))))
    raw |= ~uint64_t{0} << f.bit_size;
  return static_cast<int32_t>(raw);
}

}  // namespace hardware
