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

// Shared by the monitor tests: EDID blocks built by hand (a 27" 2560x1440 Dell look-alike with a CTA extension and a 1920x1080 one), and a fake
// display device with a fake monitor source behind it.
#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>
#include "Hardware/Monitor/SystemMonitors.hpp"

namespace fakemon {

using namespace hardware;
typedef std::vector<uint8_t> Bytes;

inline void fix_checksum(uint8_t* block) {
  uint32_t sum = 0;
  for (int i = 0; i < 127; ++i) sum += block[i];
  block[127] = static_cast<uint8_t>(256 - (sum & 0xFF));
}

inline void put_dtd(uint8_t* d, uint32_t clock_10khz, uint32_t hactive, uint32_t hblank, uint32_t vactive, uint32_t vblank, uint32_t w_mm, uint32_t h_mm, bool interlaced) {
  d[0] = static_cast<uint8_t>(clock_10khz);
  d[1] = static_cast<uint8_t>(clock_10khz >> 8);
  d[2] = static_cast<uint8_t>(hactive);
  d[3] = static_cast<uint8_t>(hblank);
  d[4] = static_cast<uint8_t>(((hactive >> 8) << 4) | (hblank >> 8));
  d[5] = static_cast<uint8_t>(vactive);
  d[6] = static_cast<uint8_t>(vblank);
  d[7] = static_cast<uint8_t>(((vactive >> 8) << 4) | (vblank >> 8));
  d[12] = static_cast<uint8_t>(w_mm);
  d[13] = static_cast<uint8_t>(h_mm);
  d[14] = static_cast<uint8_t>(((w_mm >> 8) << 4) | (h_mm >> 8));
  d[17] = interlaced ? 0x80 : 0;
}

inline void put_text(uint8_t* d, uint8_t tag, const char* text) {
  d[0] = d[1] = d[2] = 0;
  d[3] = tag;
  d[4] = 0;
  size_t n = strlen(text);
  memset(d + 5, ' ', 13);
  memcpy(d + 5, text, n);
  if (n < 13) d[5 + n] = 0x0A;
}

// A Dell U2720Q look-alike: 27", 2560x1440 native, with a CTA extension (video codes, HDMI vendor block, audio).
inline Bytes dell_edid(bool with_extension = true) {
  Bytes e(with_extension ? 256 : 128, 0);
  uint8_t* b = e.data();
  const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  memcpy(b, header, 8);
  b[8] = 0x10; b[9] = 0xAC;                 // "DEL"
  b[10] = 0xC7; b[11] = 0xA0;               // product 0xA0C7
  b[12] = 0x78; b[13] = 0x56; b[14] = 0x34; b[15] = 0x12;   // serial 0x12345678
  b[16] = 10; b[17] = 30;                   // week 10 of 2020
  b[18] = 1; b[19] = 4;
  b[20] = 0xA5;                             // digital, 8 bits, DisplayPort
  b[21] = 60; b[22] = 34;                   // 60 x 34 cm
  b[23] = 120;                              // gamma 2.20
  b[24] = 0x06;                             // sRGB, preferred timing is the first descriptor
  b[35] = 0x21; b[36] = 0x08;               // 640x480@60, 800x600@60, 1024x768@60
  b[38] = 129; b[39] = 0xC0;                // 1280x720@60 (16:9)
  b[40] = 209; b[41] = 0xC0;                // 1920x1080@60
  for (int i = 42; i < 54; i += 2) { b[i] = 1; b[i + 1] = 1; }
  put_dtd(b + 54, 24150, 2560, 160, 1440, 41, 597, 336, false);
  put_text(b + 72, 0xFC, "DELL U2720Q");
  b[90] = b[91] = b[92] = 0; b[93] = 0xFD; b[94] = 0; b[95] = 48; b[96] = 86; b[97] = 30; b[98] = 140; b[99] = 108;
  put_text(b + 108, 0xFF, "ABC123");
  if (with_extension) {
    b[126] = 1;
    uint8_t* x = b + 128;
    x[0] = 0x02; x[1] = 3;
    x[3] = 0x40;                            // basic audio
    uint8_t* p = x + 4;
    *p++ = (2 << 5) | 4; *p++ = 0x90; *p++ = 4; *p++ = 31; *p++ = 0x61;   // video: 1080p60 (native), 720p60, 1080p50, 4K60
    *p++ = (3 << 5) | 5; *p++ = 0x03; *p++ = 0x0C; *p++ = 0x00; *p++ = 0x10; *p++ = 0x00;   // vendor: HDMI
    x[2] = static_cast<uint8_t>(p - x);
    fix_checksum(x);
  }
  fix_checksum(b);
  return e;
}

struct FakeSource final : IMonitorSource {
  struct Connector {
    ConnectorInfo info;
    Bytes edid;
  };
  std::vector<Connector> connectors;
  bool multi = true, accepts = true, changed = false;
  int applies = 0;
  std::vector<ScanoutPlacement> last;
  uint32_t last_w = 0, last_h = 0;

  uint32_t connector_count() const override { return static_cast<uint32_t>(connectors.size()); }
  bool connector_info(uint32_t i, ConnectorInfo* out) const override { if (i >= connectors.size()) return false; *out = connectors[i].info; return true; }
  uint32_t read_edid(uint32_t i, ByteView out) override {
    if (i >= connectors.size() || !connectors[i].info.connected) return 0;
    const Bytes& e = connectors[i].edid;
    for (size_t k = 0; k < e.size() && k < out.size(); ++k) out[k] = e[k];
    return static_cast<uint32_t>(e.size() < out.size() ? e.size() : out.size());
  }
  bool refresh_connectors() override { const bool c = changed; changed = false; return c; }
  bool supports_multiple_displays() const override { return multi; }
  bool apply_layout(const ScanoutPlacement* p, uint32_t n, uint32_t w, uint32_t h) override {
    ++applies;
    if (!accepts) return false;
    last.assign(p, p + n);
    last_w = w;
    last_h = h;
    return true;
  }
  void add(const char* name, const Bytes& edid, bool connected = true, ConnectorType type = ConnectorType::DisplayPort) {
    Connector c;
    c.info.connected = connected;
    c.info.type = type;
    strncpy(c.info.name, name, sizeof c.info.name - 1);
    c.edid = edid;
    connectors.push_back(c);
  }
};

struct FakeGpu final : IGraphicsProcessingUnit {
  FakeSource* source = nullptr;
  uint32_t w = 1024, h = 768;
  const char* name() const override { return "fake gpu"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t mode_count() const override { return 1; }
  bool mode_info(uint32_t, DisplayMode* out) const override { *out = {w, h}; return true; }
  uint32_t current_mode() const override { return 0; }
  bool set_mode(uint32_t) override { return true; }
  uint32_t width() const override { return w; }
  uint32_t height() const override { return h; }
  void put_pixel(uint32_t, uint32_t, uint32_t) override {}
  void fill_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void copy_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void clear(uint32_t) override {}
  uint32_t glyph_width() const override { return 8; }
  uint32_t glyph_height() const override { return 16; }
  void draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t) override {}
  void draw_text(uint32_t, uint32_t, const char*, uint32_t, uint32_t) override {}
  IMonitorSource* monitor_source() override { return source; }
};

// A 1920x1080 monitor of another make, from the Dell one.
inline Bytes small_edid() {
  Bytes e = dell_edid(false);
  e[8] = 0x4C; e[9] = 0x2D;   // "SAM"
  e[21] = 53; e[22] = 30;
  put_dtd(&e[54], 14850, 1920, 280, 1080, 45, 527, 296, false);
  put_text(&e[72], 0xFC, "S24F350");
  fix_checksum(e.data());
  return e;
}


}  // namespace fakemon
