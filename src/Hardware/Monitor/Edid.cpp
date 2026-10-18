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

#include "Hardware/Monitor/Edid.hpp"

namespace hardware {

namespace {

constexpr uint32_t kBlock = 128;

struct Table {
  uint16_t width, height;
  uint16_t hz;
  bool interlaced;
};

// Established timings I-III, bit 7 first of each of the three bytes at 35..37 (0 width: reserved / manufacturer's).
constexpr Table kEstablished[3][8] = {
    {{720, 400, 70, false}, {720, 400, 88, false}, {640, 480, 60, false}, {640, 480, 67, false}, {640, 480, 72, false}, {640, 480, 75, false}, {800, 600, 56, false}, {800, 600, 60, false}},
    {{800, 600, 72, false}, {800, 600, 75, false}, {832, 624, 75, false}, {1024, 768, 87, true}, {1024, 768, 60, false}, {1024, 768, 70, false}, {1024, 768, 75, false}, {1280, 1024, 75, false}},
    {{1152, 870, 75, false}, {0, 0, 0, false}, {0, 0, 0, false}, {0, 0, 0, false}, {0, 0, 0, false}, {0, 0, 0, false}, {0, 0, 0, false}, {0, 0, 0, false}},
};

// CTA-861 video identification codes (the common ones).
struct Vic {
  uint8_t code;
  uint16_t width, height;
  uint16_t hundredths_hz;   // the refresh rate in hundredths of a hertz
  bool interlaced;
};
constexpr Vic kVics[] = {
    {1, 640, 480, 6000, false},    {2, 720, 480, 5994, false},   {3, 720, 480, 5994, false},   {4, 1280, 720, 6000, false},  {5, 1920, 1080, 6000, true},
    {6, 720, 480, 5994, true},     {7, 720, 480, 5994, true},    {16, 1920, 1080, 6000, false}, {17, 720, 576, 5000, false}, {18, 720, 576, 5000, false},
    {19, 1280, 720, 5000, false},  {20, 1920, 1080, 5000, true}, {21, 720, 576, 5000, true},    {22, 720, 576, 5000, true},   {31, 1920, 1080, 5000, false},
    {32, 1920, 1080, 2400, false}, {33, 1920, 1080, 2500, false}, {34, 1920, 1080, 3000, false}, {39, 1920, 1080, 5000, true}, {60, 1280, 720, 2400, false},
    {61, 1280, 720, 2500, false},  {62, 1280, 720, 3000, false}, {63, 1920, 1080, 12000, false}, {64, 1920, 1080, 10000, false}, {93, 3840, 2160, 2400, false},
    {94, 3840, 2160, 2500, false}, {95, 3840, 2160, 3000, false}, {96, 3840, 2160, 5000, false}, {97, 3840, 2160, 6000, false}, {98, 4096, 2160, 2400, false},
    {99, 4096, 2160, 2500, false}, {100, 4096, 2160, 3000, false}, {101, 4096, 2160, 5000, false}, {102, 4096, 2160, 6000, false},
};

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void add_timing(EdidInfo* info, uint32_t width, uint32_t height, uint32_t millihz, uint32_t clock_khz, bool interlaced, bool preferred) {
  if (width == 0 || height == 0 || width > 16384 || height > 16384) return;
  for (uint32_t i = 0; i < info->timing_count; ++i) {
    EdidTiming& t = info->timings[i];
    if (t.width != width || t.height != height || t.interlaced != interlaced) continue;
    const uint32_t diff = t.refresh_millihz > millihz ? t.refresh_millihz - millihz : millihz - t.refresh_millihz;
    if (diff > 600) continue;
    if (clock_khz && !t.pixel_clock_khz) {   // the detailed description of a mode known from a table: keep the precise numbers
      t.pixel_clock_khz = clock_khz;
      t.refresh_millihz = millihz;
    }
    if (preferred) {
      t.preferred = true;
      info->preferred = static_cast<int32_t>(i);
    }
    return;
  }
  if (info->timing_count >= EdidInfo::kMaxTimings) return;
  EdidTiming& t = info->timings[info->timing_count];
  t.width = width;
  t.height = height;
  t.refresh_millihz = millihz;
  t.pixel_clock_khz = clock_khz;
  t.interlaced = interlaced;
  t.preferred = preferred;
  if (preferred) info->preferred = static_cast<int32_t>(info->timing_count);
  ++info->timing_count;
}

// An 18-byte detailed timing descriptor (pixel clock not zero).
void read_detailed_timing(const uint8_t* d, bool preferred, EdidInfo* info) {
  const uint32_t clock_10khz = le16(d);
  if (clock_10khz == 0) return;
  const uint32_t hactive = d[2] | ((d[4] & 0xF0u) << 4), hblank = d[3] | ((d[4] & 0x0Fu) << 8);
  const uint32_t vactive = d[5] | ((d[7] & 0xF0u) << 4), vblank = d[6] | ((d[7] & 0x0Fu) << 8);
  const bool interlaced = (d[17] & 0x80) != 0;
  const uint64_t htotal = hactive + hblank, vtotal = vactive + vblank;
  if (hactive == 0 || vactive == 0 || htotal == 0 || vtotal == 0) return;
  const uint64_t clock_hz = uint64_t{clock_10khz} * 10000;
  // Progressive: one frame is htotal * vtotal pixel clocks. Interlaced: the numbers are per field; the picture is two fields tall and the
  // refresh rate quoted is the field rate.
  const uint32_t millihz = static_cast<uint32_t>(clock_hz * 1000 / (htotal * vtotal));
  add_timing(info, hactive, interlaced ? vactive * 2 : vactive, millihz, clock_10khz * 10, interlaced, preferred);
  if (info->width_mm == 0 && info->height_mm == 0) {
    info->width_mm = static_cast<uint16_t>(d[12] | ((d[14] & 0xF0u) << 4));
    info->height_mm = static_cast<uint16_t>(d[13] | ((d[14] & 0x0Fu) << 8));
  }
}

void read_standard_timing(const uint8_t* p, const EdidInfo& info, EdidInfo* out) {
  if (p[0] == 0x01 && p[1] == 0x01) return;   // unused
  if (p[0] == 0x00) return;
  const uint32_t width = (uint32_t{p[0]} + 31) * 8;
  uint32_t ratio_w, ratio_h;
  switch (p[1] >> 6) {
    case 0: if (info.version > 1 || info.revision >= 3) { ratio_w = 16; ratio_h = 10; } else { ratio_w = 1; ratio_h = 1; } break;
    case 1: ratio_w = 4; ratio_h = 3; break;
    case 2: ratio_w = 5; ratio_h = 4; break;
    default: ratio_w = 16; ratio_h = 9; break;
  }
  add_timing(out, width, width * ratio_h / ratio_w, ((p[1] & 0x3Fu) + 60) * 1000, 0, false, false);
}

// Copies the text of a display descriptor (13 characters, ended by a line feed, padded with blanks).
void read_text(const uint8_t* d, char* out, uint32_t capacity) {
  uint32_t n = 0;
  for (uint32_t i = 5; i < 18 && n + 1 < capacity; ++i) {
    if (d[i] == 0x0A || d[i] == 0) break;
    if (d[i] >= 32 && d[i] < 127) out[n++] = static_cast<char>(d[i]);
  }
  while (n > 0 && out[n - 1] == ' ') --n;
  out[n] = 0;
}

void read_cta_extension(const uint8_t* b, EdidInfo* info) {
  const uint32_t dtd_offset = b[2];
  if (b[3] & 0x40) info->has_audio = true;
  if (dtd_offset >= 4 && dtd_offset <= 127) {
    for (uint32_t i = 4; i < dtd_offset;) {
      const uint32_t tag = b[i] >> 5, length = b[i] & 0x1F;
      if (i + 1 + length > dtd_offset) break;
      const uint8_t* body = b + i + 1;
      if (tag == 2) {   // video: one code per byte
        for (uint32_t k = 0; k < length; ++k) {
          const uint8_t code = body[k] & 0x7F;
          for (const Vic& v : kVics)
            if (v.code == code) add_timing(info, v.width, v.height, uint32_t{v.hundredths_hz} * 10, 0, v.interlaced, false);
        }
      } else if (tag == 3 && length >= 3 && body[0] == 0x03 && body[1] == 0x0C && body[2] == 0x00) {   // vendor specific, the HDMI id
        info->hdmi = true;
      }
      i += 1 + length;
    }
  }
  if (dtd_offset >= 4)
    for (uint32_t off = dtd_offset; off + 18 <= kBlock - 1; off += 18) {
      if (b[off] == 0 && b[off + 1] == 0) break;
      read_detailed_timing(b + off, false, info);
    }
}

uint32_t isqrt(uint64_t v) {
  uint64_t x = v, y = (x + 1) / 2;
  if (v == 0) return 0;
  while (y < x) {
    x = y;
    y = (x + v / x) / 2;
  }
  return static_cast<uint32_t>(x);
}

bool block_checksum_ok(const uint8_t* b) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < kBlock; ++i) sum += b[i];
  return (sum & 0xFF) == 0;
}

}  // namespace

bool looks_like_edid(ConstByteView d) {
  static const uint8_t kHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  if (d.size() < kBlock) return false;
  for (int i = 0; i < 8; ++i)
    if (d[i] != kHeader[i]) return false;
  return true;
}

bool parse_edid(ConstByteView data, EdidInfo* out) {
  *out = EdidInfo();
  if (!looks_like_edid(data)) return false;
  const uint8_t* b = data.data();
  out->checksum_ok = block_checksum_ok(b);
  const uint16_t id = static_cast<uint16_t>((b[8] << 8) | b[9]);
  for (int i = 0; i < 3; ++i) {
    const uint32_t letter = (id >> (10 - 5 * i)) & 0x1F;
    out->manufacturer[i] = letter >= 1 && letter <= 26 ? static_cast<char>('A' + letter - 1) : '?';
  }
  out->product_code = le16(b + 10);
  out->serial_number = uint32_t{le16(b + 12)} | (uint32_t{le16(b + 14)} << 16);
  out->version = b[18];
  out->revision = b[19];
  if (out->version != 1) return false;   // (EDID 1.0 - 1.4 are the ones there are)
  if (b[16] != 0 && b[16] <= 54) out->week = b[16];   // (0xFF: the year is the model year)
  if (b[17] != 0) out->year = static_cast<uint16_t>(b[17] + 1990);
  const uint8_t input = b[20];
  out->digital = (input & 0x80) != 0;
  if (out->digital && out->revision >= 4) {
    static const uint8_t kDepth[8] = {0, 6, 8, 10, 12, 14, 16, 0};
    out->bits_per_color = kDepth[(input >> 4) & 7];
  }
  if (b[21] != 0 && b[22] != 0) {
    out->width_mm = static_cast<uint16_t>(b[21] * 10);
    out->height_mm = static_cast<uint16_t>(b[22] * 10);
  }
  if (b[23] != 0xFF) out->gamma_x100 = static_cast<uint8_t>(b[23] + 100);
  out->srgb = (b[24] & 0x04) != 0;

  for (int byte = 0; byte < 3; ++byte)
    for (int bit = 0; bit < 8; ++bit)
      if (b[35 + byte] & (0x80 >> bit)) {
        const Table& t = kEstablished[byte][bit];
        add_timing(out, t.width, t.height, uint32_t{t.hz} * 1000, 0, t.interlaced, false);
      }
  for (int i = 0; i < 8; ++i) read_standard_timing(b + 38 + 2 * i, *out, out);

  for (int i = 0; i < 4; ++i) {
    const uint8_t* d = b + 54 + 18 * i;
    if (d[0] != 0 || d[1] != 0) {
      read_detailed_timing(d, i == 0, out);   // (the first one is the preferred mode)
      continue;
    }
    switch (d[3]) {
      case 0xFC: read_text(d, out->name, sizeof out->name); break;
      case 0xFF: read_text(d, out->serial_text, sizeof out->serial_text); break;
      case 0xFD: {
        const uint8_t flags = d[4];
        out->min_vertical_hz = static_cast<uint16_t>(d[5] + ((flags & 3) == 3 ? 255 : 0));
        out->max_vertical_hz = static_cast<uint16_t>(d[6] + ((flags & 3) >= 2 ? 255 : 0));
        out->min_horizontal_khz = static_cast<uint16_t>(d[7] + (((flags >> 2) & 3) == 3 ? 255 : 0));
        out->max_horizontal_khz = static_cast<uint16_t>(d[8] + (((flags >> 2) & 3) >= 2 ? 255 : 0));
        out->max_pixel_clock_mhz = static_cast<uint16_t>(d[9] * 10);
        break;
      }
      case 0xFA:
        for (int k = 0; k < 6; ++k) read_standard_timing(d + 5 + 2 * k, *out, out);
        break;
      default: break;
    }
  }

  // Extension blocks that follow (b[126] says how many; the data may hold fewer).
  const uint32_t declared = b[126];
  for (uint32_t n = 1; n <= declared && (n + 1) * kBlock <= data.size(); ++n) {
    const uint8_t* ext = b + n * kBlock;
    ++out->extension_blocks;
    if (ext[0] == 0x02 && block_checksum_ok(ext)) read_cta_extension(ext, out);   // CTA-861; DisplayID (0x70) and the rest are left alone
  }
  if (out->preferred > 0) {   // the preferred mode comes first
    const EdidTiming p = out->timings[out->preferred];
    for (int32_t i = out->preferred; i > 0; --i) out->timings[i] = out->timings[i - 1];
    out->timings[0] = p;
    out->preferred = 0;
  }
  out->valid = true;
  return true;
}

bool EdidInfo::best_timing(EdidTiming* result) const {
  if (timing_count == 0) return false;
  if (preferred >= 0) {
    *result = timings[preferred];
    return true;
  }
  uint32_t best = 0;
  for (uint32_t i = 1; i < timing_count; ++i) {
    const uint64_t a = uint64_t{timings[i].width} * timings[i].height, c = uint64_t{timings[best].width} * timings[best].height;
    if (a > c || (a == c && timings[i].refresh_millihz > timings[best].refresh_millihz)) best = i;
  }
  *result = timings[best];
  return true;
}

int32_t EdidInfo::find_timing(uint32_t width, uint32_t height, uint32_t refresh_millihz) const {
  int32_t found = -1;
  uint32_t found_diff = 0;
  for (uint32_t i = 0; i < timing_count; ++i) {
    if (timings[i].width != width || timings[i].height != height) continue;
    const uint32_t diff = refresh_millihz == 0 ? 0 : (timings[i].refresh_millihz > refresh_millihz ? timings[i].refresh_millihz - refresh_millihz : refresh_millihz - timings[i].refresh_millihz);
    if (found < 0 || diff < found_diff) {
      found = static_cast<int32_t>(i);
      found_diff = diff;
    }
  }
  return found;
}

uint32_t EdidInfo::diagonal_inches_x10() const {
  if (width_mm == 0 || height_mm == 0) return 0;
  const uint32_t mm = isqrt(uint64_t{width_mm} * width_mm + uint64_t{height_mm} * height_mm);
  return static_cast<uint32_t>((uint64_t{mm} * 100 + 127) / 254);
}

uint32_t EdidInfo::dpi() const {
  EdidTiming t;
  if (width_mm == 0 || !best_timing(&t)) return 0;
  return static_cast<uint32_t>((uint64_t{t.width} * 254 + uint64_t{width_mm} * 5) / (uint64_t{width_mm} * 10));
}

const char* edid_vendor_name(const char* pnp) {
  struct Vendor {
    const char* id;
    const char* name;
  };
  static const Vendor kVendors[] = {
      {"ACR", "Acer"},     {"AOC", "AOC"},        {"APP", "Apple"},    {"AUO", "AU Optronics"}, {"AUS", "ASUS"},    {"BNQ", "BenQ"},    {"BOE", "BOE"},
      {"CMN", "Innolux"},  {"CMO", "Chi Mei"},    {"DEL", "Dell"},     {"ENC", "Eizo"},         {"EIZ", "Eizo"},    {"GSM", "LG"},      {"HPN", "HP"},
      {"HWP", "HP"},       {"IVM", "Iiyama"},     {"LEN", "Lenovo"},   {"LGD", "LG Display"},   {"MSI", "MSI"},     {"NEC", "NEC"},     {"PHL", "Philips"},
      {"RHT", "Red Hat"},  {"SAM", "Samsung"},    {"SDC", "Samsung Display"}, {"SHP", "Sharp"}, {"SNY", "Sony"},    {"VSC", "ViewSonic"}, {"BNQ", "BenQ"},
      {"VMW", "VMware"},   {"QMU", "QEMU"},       {"TSB", "Toshiba"},  {"MEI", "Panasonic"},    {"HSD", "HannStar"}, {"IVO", "InfoVision"}, {"ITE", "ITE"},
  };
  for (const Vendor& v : kVendors)
    if (v.id[0] == pnp[0] && v.id[1] == pnp[1] && v.id[2] == pnp[2]) return v.name;
  return pnp;
}

void edid_display_name(const EdidInfo& info, char* out, uint32_t capacity) {
  if (capacity == 0) return;
  uint32_t n = 0;
  auto put = [&](const char* s) {
    for (; *s && n + 1 < capacity; ++s) out[n++] = *s;
  };
  if (info.name[0]) {
    put(info.name);
  } else if (info.manufacturer[0]) {
    put(edid_vendor_name(info.manufacturer));
    put(" monitor");
  } else {
    put("Monitor");
  }
  out[n] = 0;
}

}  // namespace hardware
