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
#include "Process/Win32k/Win32kUserMisc.hpp"

// SystemParametersInfo and its settings store (the defaults of Windows 10 at 96 dpi, independent of any registry: the store lives in memory and is shared by all processes),
// GetSystemMetrics, the system colours, the double-click time and the mouse button swap.
namespace process::win32k::user_misc {

namespace {

// One setting with a getter and a setter code. kind: 'b' BOOL, 'u' UINT, 'i' INT. pv: the new value of a set comes in pvParam (else in uiParam).
struct Simple {
  uint32_t get, set;
  char kind;
  bool pv;
  int32_t def, lo, hi;
};
constexpr int32_t kAny = 0x7FFFFFFF;
constexpr Simple kSimple[] = {
    {0x01, 0x02, 'b', false, 1, 0, 1},              // beep
    {0x05, 0x06, 'i', false, 1, 0, kAny},           // border
    {0x0A, 0x0B, 'u', false, 31, 0, 31},            // keyboard speed
    {0x0E, 0x0F, 'u', false, 600, 0, kAny},         // screen saver timeout
    {0x10, 0x11, 'b', false, 1, 0, 1},              // screen saver active
    {0x12, 0x13, 'u', false, 0, 0, kAny},           // grid granularity
    {0x16, 0x17, 'u', false, 1, 0, 3},              // keyboard delay
    {0x19, 0x1A, 'b', false, 1, 0, 1},              // icon title wrap
    {0x1B, 0x1C, 'b', false, 0, 0, 1},              // menu drop alignment
    {0, 0x1D, 'u', false, 4, 0, kAny},              // double click width
    {0, 0x1E, 'u', false, 4, 0, kAny},              // double click height
    {0, 0x20, 'u', false, 500, 0, 5000},            // double click time (read through GetDoubleClickTime)
    {0, 0x21, 'b', false, 0, 0, 1},                 // swap mouse buttons
    {0x23, 0x24, 'b', false, 1, 0, 1},              // fast task switch
    {0x26, 0x25, 'b', false, 1, 0, 1},              // drag full windows
    {0x38, 0x39, 'b', false, 0, 0, 1},              // show sounds
    {0x44, 0x45, 'b', false, 0, 0, 1},              // keyboard preference
    {0x46, 0x47, 'b', false, 0, 0, 1},              // screen reader
    {0x4A, 0x4B, 'b', false, 1, 0, 1},              // font smoothing
    {0, 0x4C, 'u', false, 4, 0, kAny},              // drag width
    {0, 0x4D, 'u', false, 4, 0, kAny},              // drag height
    {0x4F, 0x51, 'u', false, 0, 0, kAny},           // low power timeout
    {0x50, 0x52, 'u', false, 0, 0, kAny},           // power off timeout
    {0x53, 0x55, 'b', false, 0, 0, 1},              // low power active
    {0x54, 0x56, 'b', false, 0, 0, 1},              // power off active
    {0x5E, 0x5D, 'u', false, 0, 0, kAny},           // mouse trails
    {0x5F, 0x60, 'b', false, 0, 0, 1},              // snap to default button
    {0x72, 0x61, 'b', false, 0, 0, 1},              // screen saver running
    {0x62, 0x63, 'u', false, 4, 0, kAny},           // mouse hover width
    {0x64, 0x65, 'u', false, 4, 0, kAny},           // mouse hover height
    {0x66, 0x67, 'u', false, 400, 1, kAny},         // mouse hover time
    {0x68, 0x69, 'u', false, 3, 0, kAny},           // wheel scroll lines
    {0x6A, 0x6B, 'u', false, 400, 0, 10000},        // menu show delay
    {0x6C, 0x6D, 'u', false, 3, 0, kAny},           // wheel scroll chars
    {0x6E, 0x6F, 'b', false, 1, 0, 1},              // show IME UI
    {0x70, 0x71, 'u', true, 10, 1, 20},             // mouse speed
    {0x76, 0x77, 'b', false, 1, 0, 1},              // screen saver secure
    {0x78, 0x79, 'u', false, 5000, 0, kAny},        // hung app timeout
    {0x7A, 0x7B, 'u', false, 5000, 0, kAny},        // wait to kill timeout
    {0x7C, 0x7D, 'u', false, 5000, 0, kAny},        // wait to kill service timeout
    {0x7E, 0x7F, 'u', false, 1, 0, kAny},           // mouse dock threshold
    {0x80, 0x81, 'u', false, 1, 0, kAny},           // pen dock threshold
    {0x82, 0x83, 'b', false, 1, 0, 1},              // window arranging
    {0x84, 0x85, 'u', false, 1, 0, kAny},           // mouse drag out threshold
    {0x86, 0x87, 'u', false, 1, 0, kAny},           // pen drag out threshold
    {0x88, 0x89, 'u', false, 1, 0, kAny},           // mouse side move threshold
    {0x8A, 0x8B, 'u', false, 1, 0, kAny},           // pen side move threshold
    {0x8C, 0x8D, 'b', false, 1, 0, 1},              // drag from maximize
    {0x8E, 0x8F, 'b', false, 1, 0, 1},              // snap sizing
    {0x90, 0x91, 'b', false, 1, 0, 1},              // dock moving
    {0x1000, 0x1001, 'b', true, 0, 0, 1},           // active window tracking
    {0x1002, 0x1003, 'b', true, 1, 0, 1},           // menu animation
    {0x1004, 0x1005, 'b', true, 1, 0, 1},           // combo box animation
    {0x1006, 0x1007, 'b', true, 1, 0, 1},           // list box smooth scrolling
    {0x1008, 0x1009, 'b', true, 1, 0, 1},           // gradient captions
    {0x100A, 0x100B, 'b', true, 0, 0, 1},           // keyboard cues
    {0x100C, 0x100D, 'b', true, 0, 0, 1},           // active window tracking z order
    {0x100E, 0x100F, 'b', true, 1, 0, 1},           // hot tracking
    {0x1012, 0x1013, 'b', true, 1, 0, 1},           // menu fade
    {0x1014, 0x1015, 'b', true, 1, 0, 1},           // selection fade
    {0x1016, 0x1017, 'b', true, 1, 0, 1},           // tooltip animation
    {0x1018, 0x1019, 'b', true, 1, 0, 1},           // tooltip fade
    {0x101A, 0x101B, 'b', true, 1, 0, 1},           // cursor shadow
    {0x101C, 0x101D, 'b', true, 0, 0, 1},           // mouse sonar
    {0x101E, 0x101F, 'b', true, 0, 0, 1},           // mouse click lock
    {0x1020, 0x1021, 'b', true, 0, 0, 1},           // mouse vanish
    {0x1022, 0x1023, 'b', true, 1, 0, 1},           // flat menu
    {0x1024, 0x1025, 'b', true, 1, 0, 1},           // drop shadow
    {0x1026, 0x1027, 'b', true, 0, 0, 1},           // block send input resets
    {0x103E, 0x103F, 'b', true, 1, 0, 1},           // UI effects
    {0x1040, 0x1041, 'u', true, 0, 0, kAny},        // disable overlapped content
    {0x1042, 0x1043, 'b', true, 1, 0, 1},           // client area animation
    {0x1048, 0x1049, 'b', true, 1, 0, 1},           // ClearType
    {0x104A, 0x104B, 'b', true, 0, 0, 1},           // speech recognition
    {0x2000, 0x2001, 'u', true, 200000, 0, kAny},   // foreground lock timeout
    {0x2002, 0x2003, 'u', true, 500, 0, kAny},      // active window tracking timeout
    {0x2004, 0x2005, 'u', true, 3, 0, kAny},        // foreground flash count
    {0x2006, 0x2007, 'u', true, 1, 1, 20},          // caret width
    {0x2008, 0x2009, 'u', true, 1200, 0, kAny},     // mouse click lock time
    {0x200A, 0x200B, 'u', true, 2, 0, 2},           // font smoothing type
    {0x200C, 0x200D, 'u', true, 1400, 1000, 2200},  // font smoothing contrast
    {0x200E, 0x200F, 'u', true, 1, 1, kAny},        // focus border width
    {0x2010, 0x2011, 'u', true, 1, 1, kAny},        // focus border height
    {0x2012, 0x2013, 'u', true, 1, 0, 1},           // font smoothing orientation
    {0x2016, 0x2017, 'u', true, 5, 1, kAny},        // message duration
    {0x201C, 0x201D, 'u', true, 1, 0, 2},           // mouse wheel routing
};
constexpr uint32_t kSimpleCount = sizeof(kSimple) / sizeof(kSimple[0]);

// Structures stored as they are (cbSize first): the accessibility settings.
struct BlobSpec { uint32_t get, set, size; uint32_t fields[4]; uint32_t field_count; };
constexpr BlobSpec kBlobs[] = {
    {0x32, 0x33, 24, {126, 1000, 1000, 500}, 4},      // FILTERKEYS: flags, wait, delay, repeat (bounce 0)
    {0x34, 0x35, 8, {62}, 1},                          // TOGGLEKEYS
    {0x36, 0x37, 28, {62, 80, 3000, 8}, 4},            // MOUSEKEYS: flags, max speed, time to max speed, ctrl speed
    {0x3A, 0x3B, 8, {506}, 1},                         // STICKYKEYS
    {0x3C, 0x3D, 12, {2, 0}, 2},                       // ACCESSTIMEOUT
    {0x40, 0x41, 48, {0}, 1},                          // SOUNDSENTRY
    {0x42, 0x43, 16, {0x4A}, 1},                       // HIGHCONTRAST
    {0x48, 0x49, 8, {1}, 1},                           // ANIMATION
};
constexpr uint32_t kBlobCount = sizeof(kBlobs) / sizeof(kBlobs[0]);
constexpr uint32_t kBlobMax = 48;

constexpr uint32_t kLogFontSize = 92, kNcmSize = 504, kIconMetricsSize = 108, kMinimizedSize = 20;

struct Metrics {      // NONCLIENTMETRICS, ICONMETRICS, MINIMIZEDMETRICS at 96 dpi
  int32_t border = 1, scroll_w = 17, scroll_h = 17, caption_w = 22, caption_h = 22, sm_caption_w = 22, sm_caption_h = 22, menu_w = 19, menu_h = 19, padded_border = 4;
  uint8_t caption_font[kLogFontSize], sm_caption_font[kLogFontSize], menu_font[kLogFontSize], status_font[kLogFontSize], message_font[kLogFontSize], icon_font[kLogFontSize];
  int32_t icon_h_spacing = 75, icon_v_spacing = 75, icon_title_wrap = 1;
  int32_t min_width = 160, min_h_gap = 4, min_v_gap = 4, min_arrange = 8;
};

struct SettingsState {
  List<int32_t> values;
  uint8_t blobs[kBlobCount][kBlobMax];
  uint32_t colors[31];
  int32_t mouse[3] = {6, 10, 1};
  Metrics metrics;
  WideString wallpaper;
  uint64_t default_input_language = 0x04090409;
  explicit SettingsState(allocators::IAllocator& a) : values(a), wallpaper(a) {}
};
UniquePointer<SettingsState> g_settings;

void put32(uint8_t* buf, uint32_t offset, uint32_t value) { for (uint32_t i = 0; i < 4; ++i) buf[offset + i] = uint8_t(value >> (8 * i)); }
uint32_t get32(const uint8_t* buf, uint32_t offset) { return uint32_t(buf[offset]) | uint32_t(buf[offset + 1]) << 8 | uint32_t(buf[offset + 2]) << 16 | uint32_t(buf[offset + 3]) << 24; }

void make_log_font(uint8_t* lf) {
  for (uint32_t i = 0; i < kLogFontSize; ++i) lf[i] = 0;
  put32(lf, 0, uint32_t(-12));      // lfHeight
  put32(lf, 16, 400);               // lfWeight
  lf[23] = 1;                       // lfCharSet DEFAULT_CHARSET
  const char* face = "Segoe UI";
  for (uint32_t i = 0; face[i]; ++i) lf[28 + 2 * i] = uint8_t(face[i]);
}

constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return r | g << 8 | b << 16; }
constexpr uint32_t kDefaultColors[31] = {
    rgb(200, 200, 200), rgb(0, 0, 0), rgb(153, 180, 209), rgb(191, 205, 219), rgb(240, 240, 240), rgb(255, 255, 255), rgb(100, 100, 100), rgb(0, 0, 0), rgb(0, 0, 0), rgb(0, 0, 0),
    rgb(180, 180, 180), rgb(244, 247, 252), rgb(171, 171, 171), rgb(0, 120, 215), rgb(255, 255, 255), rgb(240, 240, 240), rgb(160, 160, 160), rgb(109, 109, 109), rgb(0, 0, 0), rgb(0, 0, 0),
    rgb(255, 255, 255), rgb(105, 105, 105), rgb(227, 227, 227), rgb(0, 0, 0), rgb(255, 255, 225), rgb(185, 209, 234), rgb(0, 102, 204), rgb(185, 209, 234), rgb(215, 228, 242), rgb(0, 120, 215),
    rgb(240, 240, 240)};

SettingsState* st() {
  if (!g_settings) {
    SettingsState* s = lazy_state(g_settings);
    if (!s) return nullptr;
    for (const Simple& e : kSimple) s->values.push_back(e.def);
    for (uint32_t b = 0; b < kBlobCount; ++b) {
      for (uint32_t i = 0; i < kBlobMax; ++i) s->blobs[b][i] = 0;
      put32(s->blobs[b], 0, kBlobs[b].size);
      for (uint32_t f = 0; f < kBlobs[b].field_count; ++f) put32(s->blobs[b], 4 + 4 * f, kBlobs[b].fields[f]);
    }
    for (uint32_t i = 0; i < 31; ++i) s->colors[i] = kDefaultColors[i];
    make_log_font(s->metrics.caption_font); make_log_font(s->metrics.sm_caption_font); make_log_font(s->metrics.menu_font);
    make_log_font(s->metrics.status_font); make_log_font(s->metrics.message_font); make_log_font(s->metrics.icon_font);
  }
  return g_settings.get();
}

int32_t mul_div(int32_t value, uint32_t numerator, uint32_t denominator) {
  const int64_t product = int64_t(value) * int64_t(numerator);
  const int64_t half = int64_t(denominator) / 2;
  return int32_t(product >= 0 ? (product + half) / int64_t(denominator) : (product - half) / int64_t(denominator));
}

int32_t setting_value(uint32_t set_code) {
  SettingsState* s = st();
  if (!s) return 0;
  for (uint32_t i = 0; i < kSimpleCount; ++i) if (kSimple[i].set == set_code) return s->values[i];
  return 0;
}

// --- NONCLIENTMETRICS & friends at a DPI
void scaled_font(uint8_t* out, const uint8_t* font, uint32_t dpi) {
  for (uint32_t i = 0; i < kLogFontSize; ++i) out[i] = font[i];
  put32(out, 0, uint32_t(mul_div(int32_t(get32(font, 0)), dpi, 96)));
}
void write_ncm(uint8_t* out, const Metrics& m, uint32_t dpi) {
  put32(out, 0, kNcmSize);
  const int32_t v[5] = {m.border, m.scroll_w, m.scroll_h, m.caption_w, m.caption_h};
  for (uint32_t i = 0; i < 5; ++i) put32(out, 4 + 4 * i, uint32_t(mul_div(v[i], dpi, 96)));
  scaled_font(out + 24, m.caption_font, dpi);
  put32(out, 116, uint32_t(mul_div(m.sm_caption_w, dpi, 96)));
  put32(out, 120, uint32_t(mul_div(m.sm_caption_h, dpi, 96)));
  scaled_font(out + 124, m.sm_caption_font, dpi);
  put32(out, 216, uint32_t(mul_div(m.menu_w, dpi, 96)));
  put32(out, 220, uint32_t(mul_div(m.menu_h, dpi, 96)));
  scaled_font(out + 224, m.menu_font, dpi);
  scaled_font(out + 316, m.status_font, dpi);
  scaled_font(out + 408, m.message_font, dpi);
  put32(out, 500, uint32_t(mul_div(m.padded_border, dpi, 96)));
}
void read_ncm(const uint8_t* in, Metrics* m) {
  m->border = int32_t(get32(in, 4)); m->scroll_w = int32_t(get32(in, 8)); m->scroll_h = int32_t(get32(in, 12));
  m->caption_w = int32_t(get32(in, 16)); m->caption_h = int32_t(get32(in, 20));
  for (uint32_t i = 0; i < kLogFontSize; ++i) m->caption_font[i] = in[24 + i];
  m->sm_caption_w = int32_t(get32(in, 116)); m->sm_caption_h = int32_t(get32(in, 120));
  for (uint32_t i = 0; i < kLogFontSize; ++i) m->sm_caption_font[i] = in[124 + i];
  m->menu_w = int32_t(get32(in, 216)); m->menu_h = int32_t(get32(in, 220));
  for (uint32_t i = 0; i < kLogFontSize; ++i) { m->menu_font[i] = in[224 + i]; m->status_font[i] = in[316 + i]; m->message_font[i] = in[408 + i]; }
  m->padded_border = int32_t(get32(in, 500));
}
void write_icon_metrics(uint8_t* out, const Metrics& m, uint32_t dpi) {
  put32(out, 0, kIconMetricsSize);
  put32(out, 4, uint32_t(mul_div(m.icon_h_spacing, dpi, 96)));
  put32(out, 8, uint32_t(mul_div(m.icon_v_spacing, dpi, 96)));
  put32(out, 12, uint32_t(m.icon_title_wrap));
  scaled_font(out + 16, m.icon_font, dpi);
}

// --- SystemParametersInfo
bool put_out(Call& c, uint64_t address, const uint8_t* data, uint32_t size) {
  if (c.write(address, ConstByteView(data, size))) return true;
  c.set_last_error(kErrorNoAccess);
  return false;
}

uint64_t spi(Call& c, uint32_t action, uint32_t ui, uint64_t pv, uint32_t dpi, bool for_dpi) {
  SettingsState* s = st();
  if (!s) return 0;
  // the simple settings
  for (uint32_t i = 0; i < kSimpleCount; ++i) {
    const Simple& e = kSimple[i];
    if (e.get != 0 && action == e.get) {
      if (pv == 0 || !c.write_u32(pv, uint32_t(s->values[i]))) return c.fail(kErrorNoAccess);
      return 1;
    }
    if (action == e.set) {
      int64_t v = e.pv ? int64_t(int32_t(uint32_t(pv))) : int64_t(int32_t(ui));
      if (e.kind == 'b') v = (e.pv ? uint32_t(pv) : ui) != 0 ? 1 : 0;
      if (e.kind == 'u') v = e.pv ? int64_t(uint32_t(pv)) : int64_t(ui);
      if (action == 0x20 && v == 0) v = 500;      // SetDoubleClickTime(0) selects the default
      if (v < e.lo || v > e.hi) return c.fail(kErrorInvalidSpiValue);
      if (action == 0x61 && pv != 0) c.write_u32(pv, uint32_t(s->values[i]));      // SPI_SETSCREENSAVERRUNNING returns the previous state
      s->values[i] = int32_t(v);
      return 1;
    }
  }
  for (uint32_t b = 0; b < kBlobCount; ++b) {
    const BlobSpec& spec = kBlobs[b];
    if (action != spec.get && action != spec.set) continue;
    uint8_t buf[kBlobMax];
    uint32_t size = 0;
    if (pv == 0 || !c.read_u32(pv, &size)) return c.fail(kErrorNoAccess);
    if (size != spec.size || (ui != 0 && ui != spec.size)) return c.fail(kErrorInvalidParameter);
    if (action == spec.get) return put_out(c, pv, s->blobs[b], spec.size) ? 1 : 0;
    if (!c.read(pv, ByteView(buf, spec.size))) return c.fail(kErrorNoAccess);
    for (uint32_t i = 0; i < spec.size; ++i) s->blobs[b][i] = buf[i];
    return 1;
  }
  switch (action) {
    case 0x03:      // SPI_GETMOUSE
      if (pv == 0) return c.fail(kErrorNoAccess);
      return c.write_array(pv, s->mouse, 3) ? 1 : c.fail(kErrorNoAccess);
    case 0x04: {    // SPI_SETMOUSE
      int32_t v[3];
      if (pv == 0 || !c.read_array(pv, v, 3)) return c.fail(kErrorNoAccess);
      for (uint32_t i = 0; i < 3; ++i) s->mouse[i] = v[i];
      return 1;
    }
    case 0x0D:      // SPI_ICONHORIZONTALSPACING: set when pvParam is NULL, else get
    case 0x18: {    // SPI_ICONVERTICALSPACING
      int32_t& spacing = action == 0x0D ? s->metrics.icon_h_spacing : s->metrics.icon_v_spacing;
      if (pv == 0) { if (ui < 32) return c.fail(kErrorInvalidSpiValue); spacing = int32_t(ui); return 1; }
      return c.write_u32(pv, uint32_t(mul_div(spacing, dpi, 96))) ? 1 : c.fail(kErrorNoAccess);
    }
    case 0x14: {    // SPI_SETDESKWALLPAPER
      s->wallpaper.clear();
      if (pv != 0 && pv != ~uint64_t{0}) {
        WideString text(*c.allocator());
        if (!c.read_wide_z(pv, 260, &text)) return c.fail(kErrorNoAccess);
        s->wallpaper.append(text.view());
      }
      return 1;
    }
    case 0x73: {    // SPI_GETDESKWALLPAPER
      if (pv == 0 || ui == 0) return c.fail(kErrorInvalidParameter);
      if (s->wallpaper.size() + 1 > ui) return c.fail(kErrorInsufficientBuffer);
      return c.write_wide(pv, s->wallpaper.view(), ui) >= 0 ? 1 : c.fail(kErrorNoAccess);
    }
    case 0x1F: {    // SPI_GETICONTITLELOGFONT
      if (ui != 0 && ui != kLogFontSize) return c.fail(kErrorInvalidParameter);
      uint8_t lf[kLogFontSize];
      scaled_font(lf, s->metrics.icon_font, dpi);
      return pv != 0 && put_out(c, pv, lf, kLogFontSize) ? 1 : (pv == 0 ? c.fail(kErrorNoAccess) : 0);
    }
    case 0x22: {    // SPI_SETICONTITLELOGFONT
      if (ui != 0 && ui != kLogFontSize) return c.fail(kErrorInvalidParameter);
      uint8_t lf[kLogFontSize];
      if (pv == 0 || !c.read(pv, ByteView(lf, kLogFontSize))) return c.fail(kErrorNoAccess);
      for (uint32_t i = 0; i < kLogFontSize; ++i) s->metrics.icon_font[i] = lf[i];
      return 1;
    }
    case 0x29:      // SPI_GETNONCLIENTMETRICS
    case 0x2A: {    // SPI_SETNONCLIENTMETRICS
      uint32_t size = 0;
      if (pv == 0 || !c.read_u32(pv, &size)) return c.fail(kErrorNoAccess);
      const uint32_t old_size = kNcmSize - 4;      // (before Windows Vista: no iPaddedBorderWidth)
      if ((size != kNcmSize && size != old_size) || (ui != 0 && ui != size)) return c.fail(kErrorInvalidParameter);
      uint8_t buf[kNcmSize] = {};
      if (action == 0x29) { write_ncm(buf, s->metrics, dpi); put32(buf, 0, size); return put_out(c, pv, buf, size) ? 1 : 0; }
      if (!c.read(pv, ByteView(buf, size))) return c.fail(kErrorNoAccess);
      read_ncm(buf, &s->metrics);
      return 1;
    }
    case 0x2B:      // SPI_GETMINIMIZEDMETRICS
    case 0x2C: {    // SPI_SETMINIMIZEDMETRICS
      uint32_t size = 0;
      if (pv == 0 || !c.read_u32(pv, &size)) return c.fail(kErrorNoAccess);
      if (size != kMinimizedSize || (ui != 0 && ui != kMinimizedSize)) return c.fail(kErrorInvalidParameter);
      uint8_t buf[kMinimizedSize];
      if (action == 0x2B) {
        put32(buf, 0, kMinimizedSize);
        put32(buf, 4, uint32_t(s->metrics.min_width)); put32(buf, 8, uint32_t(s->metrics.min_h_gap)); put32(buf, 12, uint32_t(s->metrics.min_v_gap)); put32(buf, 16, uint32_t(s->metrics.min_arrange));
        return put_out(c, pv, buf, kMinimizedSize) ? 1 : 0;
      }
      if (!c.read(pv, ByteView(buf, kMinimizedSize))) return c.fail(kErrorNoAccess);
      s->metrics.min_width = int32_t(get32(buf, 4)); s->metrics.min_h_gap = int32_t(get32(buf, 8)); s->metrics.min_v_gap = int32_t(get32(buf, 12)); s->metrics.min_arrange = int32_t(get32(buf, 16));
      return 1;
    }
    case 0x2D:      // SPI_GETICONMETRICS
    case 0x2E: {    // SPI_SETICONMETRICS
      uint32_t size = 0;
      if (pv == 0 || !c.read_u32(pv, &size)) return c.fail(kErrorNoAccess);
      if (size != kIconMetricsSize || (ui != 0 && ui != kIconMetricsSize)) return c.fail(kErrorInvalidParameter);
      uint8_t buf[kIconMetricsSize];
      if (action == 0x2D) { write_icon_metrics(buf, s->metrics, dpi); return put_out(c, pv, buf, kIconMetricsSize) ? 1 : 0; }
      if (!c.read(pv, ByteView(buf, kIconMetricsSize))) return c.fail(kErrorNoAccess);
      s->metrics.icon_h_spacing = int32_t(get32(buf, 4)); s->metrics.icon_v_spacing = int32_t(get32(buf, 8)); s->metrics.icon_title_wrap = int32_t(get32(buf, 12));
      for (uint32_t i = 0; i < kLogFontSize; ++i) s->metrics.icon_font[i] = buf[16 + i];
      return 1;
    }
    case 0x2F: {    // SPI_SETWORKAREA
      Rect r;
      if (pv == 0 || !c.read_struct(pv, &r)) return c.fail(kErrorNoAccess);
      const int32_t k = int32_t(user::caller_dpi_scale(c));
      r = {r.left * k, r.top * k, r.right * k, r.bottom * k};
      return set_monitor_work_area(r) ? 1 : c.fail(kErrorInvalidParameter);
    }
    case 0x30: {    // SPI_GETWORKAREA
      const Rect r = user::rect_for_caller(c, monitor_rect(monitor_handle(), true));
      return pv != 0 && c.write_struct(pv, r) ? 1 : c.fail(kErrorNoAccess);
    }
    case 0x57: case 0x58: case 0x5B:      // SPI_SETCURSORS, SPI_SETICONS (reload from the registry: there is none), SPI_SETLANGTOGGLE
      return 1;
    case 0x59: {    // SPI_GETDEFAULTINPUTLANG
      return pv != 0 && c.write_u64(pv, s->default_input_language) ? 1 : c.fail(kErrorNoAccess);
    }
    case 0x5A: {    // SPI_SETDEFAULTINPUTLANG
      uint64_t hkl = 0;
      if (pv == 0 || !c.read_u64(pv, &hkl)) return c.fail(kErrorNoAccess);
      s->default_input_language = hkl;
      return 1;
    }
    default: return c.fail(kErrorInvalidSpiValue);
  }
}

uint64_t svc_spi(Call& c) { return spi(c, c.arg32(0), c.arg32(1), c.arg(2), thread_dpi(c), false); }

uint64_t svc_spi_for_dpi(Call& c) {
  const uint32_t action = c.arg32(0), dpi = c.arg32(4);
  if (dpi == 0) return c.fail(kErrorInvalidParameter);
  if (action != 0x1F && action != 0x29 && action != 0x2D) return c.fail(kErrorInvalidParameter);      // only the metrics that scale with the DPI
  return spi(c, action, c.arg32(1), c.arg(2), dpi, true);
}

uint64_t svc_set_sys_colors(Call& c) {
  SettingsState* s = st();
  const int32_t count = c.argi(0);
  if (!s) return 0;
  if (count < 0 || count > 31) return c.fail(kErrorInvalidParameter);
  int32_t index[31];
  uint32_t value[31];
  if (count > 0 && (!c.read_array(c.arg(1), index, uint32_t(count)) || !c.read_array(c.arg(2), value, uint32_t(count)))) return c.fail(kErrorNoAccess);
  for (int32_t i = 0; i < count; ++i) if (index[i] < 0 || index[i] > 30) return c.fail(kErrorInvalidParameter);
  for (int32_t i = 0; i < count; ++i) s->colors[index[i]] = value[i] & 0x00FFFFFF;
  return 1;
}

uint64_t svc_get_double_click_time(Call&) { return uint32_t(setting_value(0x20)); }

uint64_t svc_set_double_click_time(Call& c) {
  uint32_t ms = c.arg32(0);
  if (ms > 5000) return c.fail(kErrorInvalidParameter);
  if (ms == 0) ms = 500;
  SettingsState* s = st();
  if (!s) return 0;
  for (uint32_t i = 0; i < kSimpleCount; ++i) if (kSimple[i].set == 0x20) s->values[i] = int32_t(ms);
  return 1;
}

uint64_t svc_swap_mouse_button(Call& c) {
  SettingsState* s = st();
  if (!s) return 0;
  for (uint32_t i = 0; i < kSimpleCount; ++i) {
    if (kSimple[i].set != 0x21) continue;
    const int32_t previous = s->values[i];
    s->values[i] = c.arg_bool(0) ? 1 : 0;
    return previous;
  }
  return 0;
}

uint64_t svc_update_per_user_system_parameters(Call&) { return 1; }      // (reloads the user's settings from the registry: the store here is not in the registry)

uint64_t svc_scale_system_metric(Call& c) {
  const uint32_t dpi = c.arg32(1);
  return uint32_t(system_metrics(c.arg32(0), dpi));
}

}  // namespace

uint32_t system_color(uint32_t index) {
  SettingsState* s = st();
  return s && index < 31 ? s->colors[index] : 0;
}

namespace {
bool scales(uint32_t i) {
  static const uint8_t list[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 20, 21, 28, 29, 30, 31, 32, 33, 34, 35, 38, 39, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 57, 58, 71, 72, 83, 84, 92};
  for (uint8_t v : list) if (v == i) return true;
  return false;
}
}  // namespace

int32_t system_metrics(uint32_t index, uint32_t dpi) {
  SettingsState* s = st();
  if (!s) return 0;
  if (dpi == 0) dpi = 96;
  // (DPI virtualization: the screen is in device pixels; asked for 96 dpi on a scaled system - an unaware program - its sizes are the 96-dpi units)
  const uint32_t sd = system_dpi();
  const int32_t vk = dpi == 96 && sd > 96 ? int32_t(sd / 96) : 1;
  const int32_t w = int32_t(gdi::screen_width()) / vk, h = int32_t(gdi::screen_height()) / vk;
  Rect work = monitor_rect(monitor_handle(), true);
  if (vk > 1) work = {work.left / vk, work.top / vk, work.right / vk, work.bottom / vk};
  const Metrics& m = s->metrics;
  const int32_t frame_total = 4 + 4;      // SM_CXSIZEFRAME + SM_CXPADDEDBORDER
  int32_t v = 0;
  switch (index) {
    case 0: return w;                       // SM_CXSCREEN
    case 1: return h;                       // SM_CYSCREEN
    case 2: v = m.scroll_w; break;          // SM_CXVSCROLL
    case 3: v = m.scroll_h; break;          // SM_CYHSCROLL
    case 4: v = m.caption_h + 1; break;     // SM_CYCAPTION
    case 5: case 6: v = m.border; break;    // SM_CXBORDER, SM_CYBORDER
    case 7: case 8: v = 3; break;           // SM_CXDLGFRAME, SM_CYDLGFRAME
    case 9: v = m.scroll_h; break;          // SM_CYVTHUMB
    case 10: v = m.scroll_w; break;         // SM_CXHTHUMB
    case 11: case 12: case 13: case 14: v = 32; break;      // SM_CXICON, SM_CYICON, SM_CXCURSOR, SM_CYCURSOR
    case 15: v = m.menu_h + 1; break;       // SM_CYMENU
    case 16: return work.right - work.left;     // SM_CXFULLSCREEN
    case 17: return work.bottom - work.top - (m.caption_h + 1) * int32_t(dpi) / 96;      // SM_CYFULLSCREEN
    case 18: return 0;                      // SM_CYKANJIWINDOW
    case 19: return 1;                      // SM_MOUSEPRESENT
    case 20: v = m.scroll_h; break;         // SM_CYVSCROLL
    case 21: v = m.scroll_w; break;         // SM_CXHSCROLL
    case 22: return 0;                      // SM_DEBUG
    case 23: return setting_value(0x21);    // SM_SWAPBUTTON
    case 28: case 34: v = 136; break;       // SM_CXMIN, SM_CXMINTRACK
    case 29: case 35: v = 39; break;        // SM_CYMIN, SM_CYMINTRACK
    case 30: v = 36; break;                 // SM_CXSIZE
    case 31: v = 22; break;                 // SM_CYSIZE
    case 32: case 33: v = 4; break;         // SM_CXSIZEFRAME, SM_CYSIZEFRAME
    case 36: return setting_value(0x1D);    // SM_CXDOUBLECLK
    case 37: return setting_value(0x1E);    // SM_CYDOUBLECLK
    case 38: v = m.icon_h_spacing; break;   // SM_CXICONSPACING
    case 39: v = m.icon_v_spacing; break;   // SM_CYICONSPACING
    case 40: return setting_value(0x1C);    // SM_MENUDROPALIGNMENT
    case 43: return 5;                      // SM_CMOUSEBUTTONS
    case 45: case 46: v = 2; break;         // SM_CXEDGE, SM_CYEDGE
    case 47: v = m.min_width + m.min_h_gap; break;      // SM_CXMINSPACING
    case 48: v = 28 + m.min_v_gap; break;               // SM_CYMINSPACING
    case 49: case 50: v = 16; break;        // SM_CXSMICON, SM_CYSMICON
    case 51: v = m.sm_caption_h + 1; break; // SM_CYSMCAPTION
    case 52: case 53: v = 22; break;        // SM_CXSMSIZE, SM_CYSMSIZE
    case 54: v = 18; break;                 // SM_CXMENUSIZE
    case 55: v = 19; break;                 // SM_CYMENUSIZE
    case 56: return m.min_arrange;          // SM_ARRANGE
    case 57: v = m.min_width; break;        // SM_CXMINIMIZED
    case 58: v = 28; break;                 // SM_CYMINIMIZED
    case 59: return w + 2 * frame_total * int32_t(dpi) / 96;      // SM_CXMAXTRACK
    case 60: return h + 2 * frame_total * int32_t(dpi) / 96;      // SM_CYMAXTRACK
    case 61: return work.right - work.left + 2 * frame_total * int32_t(dpi) / 96;      // SM_CXMAXIMIZED
    case 62: return work.bottom - work.top + 2 * frame_total * int32_t(dpi) / 96;      // SM_CYMAXIMIZED
    case 63: return 3;                      // SM_NETWORK
    case 67: return 0;                      // SM_CLEANBOOT
    case 68: return setting_value(0x4C);    // SM_CXDRAG
    case 69: return setting_value(0x4D);    // SM_CYDRAG
    case 70: return setting_value(0x39);    // SM_SHOWSOUNDS
    case 71: case 72: v = 15; break;        // SM_CXMENUCHECK, SM_CYMENUCHECK
    case 75: return 1;                      // SM_MOUSEWHEELPRESENT
    case 76: case 77: return 0;             // SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN
    case 78: return w;                      // SM_CXVIRTUALSCREEN
    case 79: return h;                      // SM_CYVIRTUALSCREEN
    case 80: return 1;                      // SM_CMONITORS
    case 81: return 1;                      // SM_SAMEDISPLAYFORMAT
    case 82: return 1;                      // SM_IMMENABLED
    case 83: case 84: v = setting_value(index == 83 ? 0x200E : 0x2010); break;      // SM_CXFOCUSBORDER, SM_CYFOCUSBORDER
    case 91: return 1;                      // SM_MOUSEHORIZONTALWHEELPRESENT
    case 92: v = m.padded_border; break;    // SM_CXPADDEDBORDER
    case 0x1000: return 0;                  // SM_REMOTESESSION
    case 0x2002: return 1;                  // SM_CARETBLINKINGENABLED
    case 94: return user::touch_available() ? 0xC1 : 0;      // SM_DIGITIZER: NID_INTEGRATED_TOUCH | NID_MULTI_INPUT | NID_READY once a finger came
    case 95: return user::touch_available() ? 10 : 0;        // SM_MAXIMUMTOUCHES
    case 41: case 42: case 44: case 64: case 65: case 66: case 73: case 74: case 86: case 87: case 88: case 89:
    case 0x2000: case 0x2001: case 0x2003: case 0x2004: return 0;      // pen windows, DBCS, secure, mid-east, slow machine, tablet PC, media center, starter, server R2, digitizer, touches, ...
    case 24: case 25: case 26: case 27: return 0;      // (reserved)
    default: return 0;
  }
  return scales(index) && dpi != 96 ? mul_div(v, dpi, 96) : v;
}

void settings_process_exited(uint64_t) {}

void install_settings() {
  using N = WindowsSyscallNumber;
  register_handler(N::NtUserSystemParametersInfo, svc_spi);
  register_handler(N::NtUserSystemParametersInfoForDpi, svc_spi_for_dpi);
  register_handler(N::NtUserSetSysColors, svc_set_sys_colors);
  register_handler(N::NtUserGetDoubleClickTime, svc_get_double_click_time);
  register_handler(N::NtUserSetDoubleClickTime, svc_set_double_click_time);
  register_handler(N::NtUserSwapMouseButton, svc_swap_mouse_button);
  register_handler(N::NtUserUpdatePerUserSystemParameters, svc_update_per_user_system_parameters);
  register_handler(N::NtUserScaleSystemMetricForDPIWithoutCache, svc_scale_system_metric);
}

}  // namespace process::win32k::user_misc
