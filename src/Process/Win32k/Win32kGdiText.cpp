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
#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"
#include "Process/Win32k/Win32kGdiInternal.hpp"

// Text: the built-in 8x16 bitmap font scaled to the requested size, behind a provider hook for real fonts; TextOut and the metric queries.
namespace process::win32k::gdi {

namespace {

IGdiFontProvider* g_provider = nullptr;

struct FaceName { const char* name; bool fixed; };
const FaceName kFaces[] = {{"System", false}, {"Terminal", true}, {"Fixedsys", true}, {"Courier", true}, {"Courier New", true}, {"Consolas", true}, {"Lucida Console", true},
                           {"MS Sans Serif", false}, {"MS Shell Dlg", false}, {"MS Shell Dlg 2", false}, {"Microsoft Sans Serif", false}, {"Arial", false}, {"Tahoma", false},
                           {"Segoe UI", false}, {"Verdana", false}, {"Times New Roman", false}};

bool name_equals(const char16_t* face, const char* name) {
  uint32_t i = 0;
  for (; i < 32 && name[i]; ++i) if (WideString::fold(face[i], true) != WideString::fold(char16_t(uint8_t(name[i])), true)) return false;
  return i < 32 ? face[i] == 0 : true;
}

const FaceName* find_face(const char16_t* face) {
  for (const FaceName& f : kFaces) if (name_equals(face, f.name)) return &f;
  return nullptr;
}

struct FontSpec {
  int32_t cell_h = 16, cell_w = 8, ascent = 13, descent = 3, internal_leading = 0, external_leading = 0;
  bool bold = false, italic = false, underline = false, strike = false, fixed = true;
  uint8_t charset = 0, pitch_family = 0;
  uint32_t weight = 400;
  uint32_t face_id = 0;
  bool provider = false;
  char16_t name[32] = {};
};

int32_t log_to_dev_y(const Dc& dc, int32_t v) {
  const Point a = lp_to_dp(dc, Point{0, 0}), b = lp_to_dp(dc, Point{0, v});
  return iabs(sat_sub(b.y, a.y));
}
int32_t log_to_dev_x(const Dc& dc, int32_t v) {
  const Point a = lp_to_dp(dc, Point{0, 0}), b = lp_to_dp(dc, Point{v, 0});
  return iabs(sat_sub(b.x, a.x));
}
int32_t dev_to_log_y(const Dc& dc, int32_t v) {
  const Point a = dp_to_lp(dc, Point{0, 0}), b = dp_to_lp(dc, Point{0, v});
  return iabs(sat_sub(b.y, a.y));
}
int32_t dev_to_log_x(const Dc& dc, int32_t v) {
  const Point a = dp_to_lp(dc, Point{0, 0}), b = dp_to_lp(dc, Point{v, 0});
  return iabs(sat_sub(b.x, a.x));
}

void resolve_font(const Dc& dc, FontSpec* out) {
  const Font* f = find_font(dc.s.font);
  if (!f) f = find_font(stock_handle(13));
  int32_t h = f ? (f->lf_height == 0 ? 16 : iabs(f->lf_height)) : 16;
  int32_t cell_h = log_to_dev_y(dc, h);
  if (cell_h < 4) cell_h = 4;
  if (cell_h > 512) cell_h = 512;
  out->cell_h = cell_h;
  out->cell_w = f && f->lf_width != 0 ? log_to_dev_x(dc, iabs(f->lf_width)) : cell_h / 2;
  if (out->cell_w < 1) out->cell_w = 1;
  if (out->cell_w > 512) out->cell_w = 512;
  out->ascent = (cell_h * 13 + 8) / 16;
  out->descent = cell_h - out->ascent;
  out->weight = f ? f->weight : 400;
  out->bold = f && f->weight >= 600;
  out->italic = f && f->italic;
  out->underline = f && f->underline;
  out->strike = f && f->strikeout;
  out->charset = f ? f->charset : 0;
  if (f) for (int i = 0; i < 32; ++i) out->name[i] = f->face[i];
  const FaceName* known = find_face(out->name);
  out->fixed = known ? known->fixed : (f ? (f->pitch_family & 3) == 1 : false);
  out->fixed = known ? known->fixed : false;
  out->pitch_family = uint8_t((out->fixed ? 0 : 1) | (out->fixed ? 0x30 : 0x20));
  if (!known) { const char* sys = "System"; for (int i = 0; i < 7; ++i) out->name[i] = char16_t(sys[i]); }
  if (g_provider && f) {
    GdiFontRequest req;
    uint32_t n = 0;
    while (n < 31 && f->face[n]) ++n;
    req.face = ArrayView<const char16_t>(f->face, n);
    req.height = cell_h;
    req.width = f->lf_width != 0 ? out->cell_w : 0;
    req.weight = int32_t(f->weight);
    req.italic = f->italic != 0;
    req.charset = f->charset;
    req.pitch_family = f->pitch_family;
    GdiFontMetrics m;
    if (g_provider->match(req, &out->face_id) && g_provider->metrics(out->face_id, cell_h, &m)) {
      out->provider = true;
      out->ascent = m.ascent;
      out->descent = m.descent;
      out->cell_h = m.ascent + m.descent;
      out->internal_leading = m.internal_leading;
      out->external_leading = m.external_leading;
      out->cell_w = m.ave_width > 0 ? m.ave_width : out->cell_w;
      out->fixed = m.fixed_pitch;
    }
  }
}

bool builtin_pixel(char32_t ch, const FontSpec& fs, int32_t px, int32_t py) {
  if (ch < 0x20 || ch == 0x7F) return false;
  if (ch == 0x20 || ch == 0xA0) return false;
  const char c = ch < 0x7F ? char(ch) : '?';
  int32_t shear = 0;
  if (fs.italic) shear = (fs.cell_h - 1 - py) / 4;
  const int32_t x = px - shear;
  if (x < 0) return false;
  const ConstByteView g = hardware::Font8x16::glyph(c);
  auto bit = [&](int32_t xx) {
    if (xx < 0 || xx >= fs.cell_w) return false;
    const int32_t sx = int32_t(int64_t{xx} * 8 / fs.cell_w), sy = int32_t(int64_t{py} * 16 / fs.cell_h);
    return sy < 16 && sx < 8 && ((g[uint64_t(sy)] >> (7 - sx)) & 1) != 0;
  };
  return bit(x) || (fs.bold && bit(x - 1));
}

int32_t advance_of(const FontSpec& fs, char32_t ch) {
  if (fs.provider) {
    GdiGlyph gl;
    if (g_provider->glyph(fs.face_id, fs.cell_h, ch, &gl)) return gl.advance;
  }
  return fs.cell_w;
}

}  // namespace

void set_font_provider(IGdiFontProvider* provider) { g_provider = provider; }

void text_metrics_for(const Dc& dc, TextMetricsData* out) {
  FontSpec fs;
  resolve_font(dc, &fs);
  out->height = dev_to_log_y(dc, fs.cell_h);
  out->ascent = dev_to_log_y(dc, fs.ascent);
  out->descent = out->height - out->ascent;
  out->internal_leading = dev_to_log_y(dc, fs.internal_leading);
  out->external_leading = dev_to_log_y(dc, fs.external_leading);
  out->ave_width = dev_to_log_x(dc, fs.cell_w);
  out->max_width = out->ave_width;
  out->weight = int32_t(fs.weight);
  out->overhang = 0;
  out->italic = fs.italic;
  out->underlined = fs.underline;
  out->struck = fs.strike;
  out->pitch_family = fs.pitch_family;
  out->charset = fs.charset;
  out->first = 0x20;
  out->last = 0xFFFF;
  out->default_char = 0x3F;
  out->break_char = 0x20;
}

int32_t char_advance(const Dc& dc, char32_t ch) {
  FontSpec fs;
  resolve_font(dc, &fs);
  return advance_of(fs, ch);
}

int32_t text_width(const Dc& dc, ArrayView<const char16_t> text, ArrayView<const int32_t> dx) {
  FontSpec fs;
  resolve_font(dc, &fs);
  int64_t w = 0;
  for (uint64_t i = 0; i < text.size(); ++i) {
    if (i < dx.size()) w += log_to_dev_x(dc, dx[i]) * (dx[i] < 0 ? -1 : 1);
    else w += advance_of(fs, text[i]) + dc.s.char_extra;
  }
  if (dc.s.just_breaks > 0 && dx.size() == 0) {
    uint32_t breaks = 0;
    for (char16_t ch : text) if (ch == u' ') ++breaks;
    if (breaks > 0) w += int64_t{dc.s.just_extra} * (breaks < uint32_t(dc.s.just_breaks) ? breaks : uint32_t(dc.s.just_breaks));
  }
  return clamp_i32(w);
}

bool draw_text(Dc& dc, int32_t x, int32_t y, uint32_t flags, const Rect* rect, ArrayView<const char16_t> text, ArrayView<const int32_t> dx, Size* extent) {
  FontSpec fs;
  resolve_font(dc, &fs);
  const int32_t width = text_width(dc, text, dx);
  if (extent) { extent->cx = dev_to_log_x(dc, width); extent->cy = dev_to_log_y(dc, fs.cell_h); }
  Point origin;
  const uint32_t align = uint32_t(dc.s.text_align);
  if (align & 1) origin = lp_to_dp(dc, dc.cur_pos);
  else origin = lp_to_dp(dc, Point{x, y});
  int32_t left = origin.x, top = origin.y;
  if ((align & 6) == 6) left -= width / 2;
  else if (align & 2) left -= width;
  if ((align & 24) == 24) top -= fs.ascent;
  else if (align & 8) top -= fs.cell_h;
  if (dc.path.recording) return true;
  Painter p(dc);
  if (!p.ok()) return true;
  Rect clip_rect{-0x3FFFFFFF, -0x3FFFFFFF, 0x3FFFFFFF, 0x3FFFFFFF};
  if (rect && ((flags & 6) != 0)) {
    const Rect dr = lp_to_dp_rect(dc, *rect);
    if (flags & 2) fill_rect_solid(p, dr, dc.s.bk_color & 0xFFFFFF, 13);
    if (flags & 4) clip_rect = dr;
  }
  if (text.size() == 0) return true;
  if (dc.s.bk_mode == 2 && !(flags & 2 && false)) fill_rect_solid(p, rect_and(Rect{left, top, sat_add(left, width), sat_add(top, fs.cell_h)}, clip_rect), dc.s.bk_color & 0xFFFFFF, 13);
  const ColorRef color = resolve_color(dc, dc.s.text_color);
  int64_t pen_x = left;
  for (uint64_t i = 0; i < text.size(); ++i) {
    const char32_t ch = text[i];
    const int32_t adv = i < dx.size() ? log_to_dev_x(dc, dx[i]) : advance_of(fs, ch) + dc.s.char_extra;
    const int32_t gx = clamp_i32(pen_x);
    if (fs.provider) {
      GdiGlyph gl;
      if (g_provider->glyph(fs.face_id, fs.cell_h, ch, &gl) && gl.width > 0 && gl.height > 0 && gl.coverage.size() >= uint64_t(gl.width) * uint32_t(gl.height)) {
        for (int32_t py = 0; py < gl.height; ++py) {
          for (int32_t px = 0; px < gl.width; ++px) {
            const uint32_t a = gl.coverage[uint64_t(py) * uint32_t(gl.width) + uint32_t(px)];
            if (a == 0) continue;
            const int32_t sx = gx + gl.left + px, sy = top + fs.ascent - gl.top + py;
            if (!rect_has(clip_rect, sx, sy)) continue;
            if (a >= 255) p.plot_raw(sx, sy, color);
            else {
              const ColorRef d = p.read(sx, sy);
              if (d == kClrInvalid) continue;
              p.plot_raw(sx, sy, make_rgb((cr_r(color) * a + cr_r(d) * (255 - a)) / 255, (cr_g(color) * a + cr_g(d) * (255 - a)) / 255, (cr_b(color) * a + cr_b(d) * (255 - a)) / 255));
            }
          }
        }
      }
    } else {
      for (int32_t py = 0; py < fs.cell_h; ++py) {
        for (int32_t px = 0; px < fs.cell_w + fs.cell_h / 4; ++px) {
          if (!builtin_pixel(ch, fs, px, py)) continue;
          const int32_t sx = gx + px, sy = top + py;
          if (rect_has(clip_rect, sx, sy)) p.plot_raw(sx, sy, color);
        }
      }
    }
    pen_x += adv;
  }
  if (fs.underline) fill_rect_solid(p, rect_and(Rect{left, top + fs.ascent + 1, sat_add(left, width), top + fs.ascent + 1 + (fs.cell_h >= 24 ? fs.cell_h / 16 : 1)}, clip_rect), color, 13);
  if (fs.strike) fill_rect_solid(p, rect_and(Rect{left, top + fs.ascent * 6 / 10, sat_add(left, width), top + fs.ascent * 6 / 10 + (fs.cell_h >= 24 ? fs.cell_h / 16 : 1)}, clip_rect), color, 13);
  if (align & 1) dc.cur_pos = dp_to_lp(dc, Point{sat_add(origin.x, width), origin.y});
  return true;
}

namespace {

constexpr uint32_t kMaxChars = 1u << 16;

bool read_text(Call& c, uint64_t address, uint32_t count, WideString* out) {
  if (count > kMaxChars) return false;
  return count == 0 || c.read_wide(address, count, out);
}

void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void put16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }

uint64_t NtGdiExtTextOutW(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const uint32_t flags = c.arg32(3), count = c.arg32(6);
  Rect rect;
  const bool has_rect = c.arg(4) != 0;
  if (has_rect && !c.read_struct(c.arg(4), &rect)) return 0;
  if ((flags & 6) && !has_rect) return c.fail(kErrorInvalidParameter);
  WideString text(*alloc());
  if (!read_text(c, c.arg(5), count, &text)) return 0;
  List<int32_t> dx(*alloc());
  if (c.arg(7) != 0 && count > 0) {
    const uint32_t per = (flags & 0x2000) ? 2 : 1;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v = 0;
      if (!c.read_u32(c.arg(7) + uint64_t{i} * 4 * per, &v)) return 0;
      dx.push_back(int32_t(v));
    }
  }
  if (count == 0 && !(flags & 2)) return 1;
  return ret_bool(draw_text(*dc, c.argi(1), c.argi(2), flags, has_rect ? &rect : nullptr, text.view(), ArrayView<const int32_t>(dx.view()), nullptr));
}

uint64_t NtGdiPolyTextOutW(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  const uint32_t n = c.arg32(2);
  if (!dc || n > 4096) return 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t raw[56] = {};
    if (!c.read(c.arg(1) + uint64_t{i} * 56, ByteView(raw, 56))) return 0;
    auto g32 = [&](int o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; };
    uint64_t str = 0, pdx = 0;
    for (int k = 0; k < 8; ++k) { str |= uint64_t{raw[16 + k]} << (8 * k); pdx |= uint64_t{raw[48 + k]} << (8 * k); }
    WideString text(*alloc());
    if (!read_text(c, str, g32(8), &text)) return 0;
    Rect r{int32_t(g32(28)), int32_t(g32(32)), int32_t(g32(36)), int32_t(g32(40))};
    List<int32_t> dx(*alloc());
    if (pdx) for (uint32_t k = 0; k < g32(8); ++k) { uint32_t v = 0; if (!c.read_u32(pdx + uint64_t{k} * 4, &v)) return 0; dx.push_back(int32_t(v)); }
    draw_text(*dc, int32_t(g32(0)), int32_t(g32(4)), g32(24), &r, text.view(), ArrayView<const int32_t>(dx.view()), nullptr);
  }
  return 1;
}

uint64_t extent_common(Call& c, uint64_t str, int32_t count, int32_t max_ext, uint64_t nfit, uint64_t dxs, uint64_t size) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc || count < 0) return 0;
  WideString text(*alloc());
  if (!read_text(c, str, uint32_t(count), &text)) return 0;
  FontSpec fs;
  resolve_font(*dc, &fs);
  int64_t total = 0;
  int32_t fit = 0;
  for (uint64_t i = 0; i < text.size(); ++i) {
    total += advance_of(fs, text[i]) + dc->s.char_extra;
    const int32_t logical = dev_to_log_x(*dc, clamp_i32(total));
    if (dxs != 0 && !c.write_u32(dxs + i * 4, uint32_t(logical))) return 0;
    if (max_ext < 0 || logical <= max_ext) fit = int32_t(i + 1);
  }
  if (dc->s.just_breaks > 0) {
    uint32_t breaks = 0;
    for (char16_t ch : text.view()) if (ch == u' ') ++breaks;
    total += int64_t{dc->s.just_extra} * breaks;
  }
  if (nfit != 0 && !c.write_u32(nfit, uint32_t(fit))) return 0;
  const Size sz{dev_to_log_x(*dc, clamp_i32(total)), dev_to_log_y(*dc, fs.cell_h)};
  return ret_bool(c.write_struct(size, sz));
}

uint64_t NtGdiGetTextExtentExW(Call& c) { return extent_common(c, c.arg(1), c.argi(2), c.argi(3), c.arg(4), c.arg(5), c.arg(6)); }
uint64_t NtGdiGetTextExtent(Call& c) { return extent_common(c, c.arg(1), c.argi(2), -1, 0, 0, c.arg(3)); }

uint64_t NtGdiGetTextMetricsW(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  TextMetricsData m;
  text_metrics_for(*dc, &m);
  uint8_t raw[60] = {};
  put32(raw, uint32_t(m.height)); put32(raw + 4, uint32_t(m.ascent)); put32(raw + 8, uint32_t(m.descent)); put32(raw + 12, uint32_t(m.internal_leading));
  put32(raw + 16, uint32_t(m.external_leading)); put32(raw + 20, uint32_t(m.ave_width)); put32(raw + 24, uint32_t(m.max_width)); put32(raw + 28, uint32_t(m.weight));
  put32(raw + 32, uint32_t(m.overhang)); put32(raw + 36, uint32_t(m.aspect_x)); put32(raw + 40, uint32_t(m.aspect_y));
  put16(raw + 44, m.first); put16(raw + 46, m.last); put16(raw + 48, m.default_char); put16(raw + 50, m.break_char);
  raw[52] = m.italic; raw[53] = m.underlined; raw[54] = m.struck; raw[55] = m.pitch_family; raw[56] = m.charset;
  return ret_bool(c.write(c.arg(1), ConstByteView(raw, 60)));
}

uint64_t NtGdiGetTextFaceW(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  FontSpec fs;
  resolve_font(*dc, &fs);
  uint32_t n = 0;
  while (n < 31 && fs.name[n]) ++n;
  if (c.arg(2) == 0) return n + 1;
  const int32_t cap = c.argi(1);
  if (cap <= 0) return 0;
  const uint32_t copy = n + 1 < uint32_t(cap) ? n + 1 : uint32_t(cap);
  List<char16_t> buf(*alloc());
  for (uint32_t i = 0; i < copy; ++i) buf.push_back(i < n ? fs.name[i] : char16_t(0));
  if (c.write_wide(c.arg(2), ArrayView<const char16_t>(buf.view()), copy, false) < 0) return 0;
  return copy;
}

// first/last with an optional explicit character list (then `last` is the count).
bool char_list(Call& c, uint32_t first, uint32_t last, uint64_t chars, List<char32_t>* out) {
  if (chars != 0) {
    if (last > kMaxChars) return false;
    for (uint32_t i = 0; i < last; ++i) { uint16_t v = 0; if (!c.read_u16(chars + uint64_t{i} * 2, &v)) return false; out->push_back(v); }
    return true;
  }
  if (last < first || last - first >= kMaxChars) return false;
  for (uint32_t ch = first; ch <= last; ++ch) out->push_back(ch);
  return true;
}

uint64_t NtGdiGetCharWidthW(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  List<char32_t> chars(*alloc());
  if (!char_list(c, c.arg32(1), c.arg32(2), c.arg(3), &chars)) return 0;
  FontSpec fs;
  resolve_font(*dc, &fs);
  const uint32_t flags = c.arg32(4);
  for (uint64_t i = 0; i < chars.count(); ++i) {
    const int32_t w = dev_to_log_x(*dc, advance_of(fs, chars[i]) + dc->s.char_extra);
    if (flags & 2) { if (!c.write_u32(c.arg(5) + i * 4, uint32_t(w))) return 0; }
    else { const float f = float(w); if (!c.write_array(c.arg(5) + i * 4, &f, 1)) return 0; }
  }
  return 1;
}

uint64_t NtGdiGetCharABCWidthsW(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  List<char32_t> chars(*alloc());
  if (!char_list(c, c.arg32(1), c.arg32(2), c.arg(3), &chars)) return 0;
  FontSpec fs;
  resolve_font(*dc, &fs);
  const uint32_t flags = c.arg32(4);
  for (uint64_t i = 0; i < chars.count(); ++i) {
    const int32_t w = dev_to_log_x(*dc, advance_of(fs, chars[i]));
    if (flags & 1) {
      uint8_t raw[12] = {};
      put32(raw + 4, uint32_t(w));
      if (!c.write(c.arg(5) + i * 12, ConstByteView(raw, 12))) return 0;
    } else {
      const float abc[3] = {0.0f, float(w), 0.0f};
      if (!c.write_array(c.arg(5) + i * 12, abc, 3)) return 0;
    }
  }
  return 1;
}

uint64_t NtGdiGetGlyphIndicesW(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  const int32_t count = c.argi(2);
  if (!dc || count < 0 || uint32_t(count) > kMaxChars) return ret_i32(-1);
  const bool mark = (c.arg32(4) & 1) != 0;
  for (int32_t i = 0; i < count; ++i) {
    uint16_t ch = 0;
    if (!c.read_u16(c.arg(1) + uint64_t(i) * 2, &ch)) return ret_i32(-1);
    uint16_t g = (ch >= 0x20 && ch < 0x7F) ? ch : (mark ? uint16_t(0xFFFF) : uint16_t(0x1F));
    if (!c.write_u16(c.arg(3) + uint64_t(i) * 2, g)) return ret_i32(-1);
  }
  return ret_i32(count);
}

uint64_t NtGdiGetCharSet(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return kClrInvalid;
  FontSpec fs;
  resolve_font(*dc, &fs);
  return fs.charset;
}

uint64_t NtGdiGetTextCharsetInfo(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return ret_i32(-1);
  FontSpec fs;
  resolve_font(*dc, &fs);
  if (c.arg(1) != 0) {
    uint8_t sig[24] = {};
    put32(sig, 0x00000001);      // basic Latin
    if (!c.write(c.arg(1), ConstByteView(sig, 24))) return ret_i32(-1);
  }
  return fs.charset;
}

uint64_t NtGdiGetCharWidthInfo(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const int32_t zero[3] = {0, 0, 0};
  return ret_bool(c.write_array(c.arg(1), zero, 3));
}

uint64_t NtGdiGetRealizationInfo(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  uint8_t raw[24] = {};
  put32(raw, 16); put32(raw + 4, 1); put32(raw + 8, 1); put32(raw + 12, find_font(dc->s.font) ? find_font(dc->s.font)->handle & 0xFFFF : 0);
  return ret_bool(c.write(c.arg(1), ConstByteView(raw, 16)));
}

uint64_t NtGdiGetFontUnicodeRanges(Call& c) {
  if (!dc_arg(c, c.arg32(0))) return 0;
  if (c.arg(1) == 0) return 20;
  uint8_t raw[20] = {};
  put32(raw, 20); put32(raw + 8, 95); put32(raw + 12, 1); put16(raw + 16, 0x20); put16(raw + 18, 95);
  return c.write(c.arg(1), ConstByteView(raw, 20)) ? 20 : 0;
}

uint64_t NtGdiGetKerningPairs(Call& c) { return dc_arg(c, c.arg32(0)) ? 0 : 0; }
uint64_t NtGdiFontIsLinked(Call& c) { return dc_arg(c, c.arg32(0)) ? 0 : 0; }
uint64_t NtGdiGetFontData(Call& c) {
  if (!dc_arg(c, c.arg32(0))) return 0xFFFFFFFFu;
  if (g_provider) {
    Dc* dc = dc_arg(c, c.arg32(0));
    FontSpec fs;
    resolve_font(*dc, &fs);
    if (fs.provider) {
      const uint32_t length = c.arg32(4);
      List<uint8_t> tmp(*alloc());
      for (uint32_t i = 0; i < (c.arg(3) ? length : 0) && i < (1u << 24); ++i) tmp.push_back(0);
      const int64_t n = g_provider->font_data(fs.face_id, c.arg32(1), c.arg32(2), c.arg(3) ? ByteView(tmp.view()) : ByteView());
      if (n < 0) return 0xFFFFFFFFu;
      if (c.arg(3) && n > 0 && !c.write(c.arg(3), ConstByteView(tmp.view()).first(uint64_t(n)))) return 0xFFFFFFFFu;
      return uint64_t(n);
    }
  }
  return 0xFFFFFFFFu;      // the built-in bitmap font has no sfnt tables
}

uint64_t NtGdiGetOutlineTextMetricsInternalW(Call& c) { return dc_arg(c, c.arg32(0)) ? 0 : 0; }      // only TrueType fonts have outline metrics

uint64_t NtGdiGetRasterizerCaps(Call& c) {
  if (c.arg32(1) < 6) return 0;
  const uint8_t st[6] = {6, 0, 3, 0, 0x09, 0x04};      // RASTERIZER_STATUS: nSize 6, TT_AVAILABLE | TT_ENABLED, English
  return ret_bool(c.write(c.arg(0), ConstByteView(st, 6)));
}

uint64_t NtGdiEnumFonts(Call& c) {
  if (!dc_arg(c, c.arg32(0))) return 0;
  const uint32_t name_len = c.arg32(3);
  WideString want(*alloc());
  if (name_len > 0 && !c.read_wide_z(c.arg(4), 64, &want)) return 0;
  uint32_t cap = 0;
  if (!c.read_u32(c.arg(6), &cap)) return 0;
  uint32_t matched = 0;
  for (const FaceName& f : kFaces) {
    WideString name(*alloc());
    name.append_ascii(f.name);
    if (want.size() > 0 && !name.equals(want.view(), true)) continue;
    if (c.arg(7) != 0 && matched < cap) {
      uint8_t e[452] = {};
      put32(e, 4);      // TRUETYPE_FONTTYPE
      const uint64_t lf = 4;
      put32(e + lf, uint32_t(-16)); put32(e + lf + 16, 400);
      e[lf + 23] = 0; e[lf + 27] = uint8_t((f.fixed ? 0 : 1) | (f.fixed ? 0x30 : 0x20));
      for (uint32_t i = 0; f.name[i] && i < 31; ++i) { put16(e + lf + 28 + 2 * i, uint8_t(f.name[i])); put16(e + lf + 92 + 2 * i, uint8_t(f.name[i])); }
      const char* style = "Regular";
      for (uint32_t i = 0; style[i]; ++i) put16(e + lf + 92 + 128 + 2 * i, uint8_t(style[i]));
      const char* script = "Western";
      for (uint32_t i = 0; script[i]; ++i) put16(e + lf + 92 + 128 + 64 + 2 * i, uint8_t(script[i]));
      const uint64_t tm = lf + 348;
      put32(e + tm, 16); put32(e + tm + 4, 13); put32(e + tm + 8, 3); put32(e + tm + 20, 8); put32(e + tm + 24, 8); put32(e + tm + 28, 400);
      if (!c.write(c.arg(7) + uint64_t{matched} * 452, ConstByteView(e, 452))) return 0;
    }
    ++matched;
  }
  if (c.arg(7) == 0) return ret_bool(c.write_u32(c.arg(6), matched));
  return ret_bool(c.write_u32(c.arg(6), matched < cap ? matched : cap));
}

}  // namespace

void register_text_services() {
  GDI_REGISTER(ExtTextOutW, NtGdiExtTextOutW);
  GDI_REGISTER(PolyTextOutW, NtGdiPolyTextOutW);
  GDI_REGISTER(GetTextExtentExW, NtGdiGetTextExtentExW);
  GDI_REGISTER(GetTextExtent, NtGdiGetTextExtent);
  GDI_REGISTER(GetTextMetricsW, NtGdiGetTextMetricsW);
  GDI_REGISTER(GetTextFaceW, NtGdiGetTextFaceW);
  GDI_REGISTER(GetCharWidthW, NtGdiGetCharWidthW);
  GDI_REGISTER(GetCharABCWidthsW, NtGdiGetCharABCWidthsW);
  GDI_REGISTER(GetGlyphIndicesW, NtGdiGetGlyphIndicesW);
  GDI_REGISTER(GetGlyphIndicesWInternal, NtGdiGetGlyphIndicesW);
  GDI_REGISTER(GetCharSet, NtGdiGetCharSet);
  GDI_REGISTER(GetTextCharsetInfo, NtGdiGetTextCharsetInfo);
  GDI_REGISTER(GetCharWidthInfo, NtGdiGetCharWidthInfo);
  GDI_REGISTER(GetRealizationInfo, NtGdiGetRealizationInfo);
  GDI_REGISTER(GetFontUnicodeRanges, NtGdiGetFontUnicodeRanges);
  GDI_REGISTER(GetKerningPairs, NtGdiGetKerningPairs);
  GDI_REGISTER(FontIsLinked, NtGdiFontIsLinked);
  GDI_REGISTER(GetFontData, NtGdiGetFontData);
  GDI_REGISTER(GetOutlineTextMetricsInternalW, NtGdiGetOutlineTextMetricsInternalW);
  GDI_REGISTER(GetRasterizerCaps, NtGdiGetRasterizerCaps);
  GDI_REGISTER(EnumFonts, NtGdiEnumFonts);
}

}  // namespace process::win32k::gdi
