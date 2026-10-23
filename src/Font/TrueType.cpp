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
#include "Font/TrueType.hpp"
#include "Utility/ByteBuffer.hpp"

namespace font {

namespace {

constexpr uint32_t tag(char a, char b, char c, char d) {
  return (uint32_t{static_cast<uint8_t>(a)} << 24) | (uint32_t{static_cast<uint8_t>(b)} << 16) | (uint32_t{static_cast<uint8_t>(c)} << 8) |
         uint32_t{static_cast<uint8_t>(d)};
}

// Big-endian reads at an absolute offset of a table; `*ok` is cleared (never set) when the read would leave the
// table, and the value is then 0.
uint32_t be16_at(ConstByteView v, uint64_t offset, bool* ok) {
  ByteReader r(v);
  if (!r.seek(offset)) {
    *ok = false;
    return 0;
  }
  const uint32_t value = r.be16();
  if (!r.ok()) *ok = false;
  return value;
}
uint32_t be32_at(ConstByteView v, uint64_t offset, bool* ok) {
  ByteReader r(v);
  if (!r.seek(offset)) {
    *ok = false;
    return 0;
  }
  const uint32_t value = r.be32();
  if (!r.ok()) *ok = false;
  return value;
}
int32_t be16s_at(ConstByteView v, uint64_t offset, bool* ok) { return static_cast<int16_t>(be16_at(v, offset, ok)); }

int32_t clamp_coordinate(int64_t v) {
  constexpr int64_t kLimit = int64_t{1} << 26;  // far beyond any real glyph (they live in +-32768), keeps later products in range
  return static_cast<int32_t>(v < -kLimit ? -kLimit : v > kLimit ? kLimit : v);
}

// (a*x + c*y) >> 14 for an F2Dot14 matrix entry pair.
int32_t transform14(int32_t m1, int32_t x, int32_t m2, int32_t y) {
  const int64_t sum = int64_t{m1} * x + int64_t{m2} * y;
  return clamp_coordinate((sum + (sum >= 0 ? 0x2000 : -0x2000)) / 0x4000);
}

// Decodes a name record string: UTF-16BE (platform 0 and 3) or single bytes (Mac Roman, ASCII range kept).
bool append_name_string(String* out, ConstByteView raw, bool utf16) {
  constexpr uint64_t kMaxChars = 256;
  uint64_t chars = 0;
  if (utf16) {
    for (uint64_t i = 0; i + 1 < raw.size() && chars < kMaxChars; i += 2, ++chars) {
      const uint32_t u = (uint32_t{raw[i]} << 8) | raw[i + 1];
      if (!out->push_back(u >= 0x20 && u < 0x7F ? static_cast<char>(u) : '?')) return false;
    }
  } else {
    for (uint64_t i = 0; i < raw.size() && chars < kMaxChars; ++i, ++chars)
      if (!out->push_back(raw[i] >= 0x20 && raw[i] < 0x7F ? static_cast<char>(raw[i]) : '?')) return false;
  }
  return true;
}

}  // namespace

TrueTypeFont::TrueTypeFont(allocators::IAllocator& allocator)
    : allocator_(allocator), family_(allocator), style_(allocator), full_(allocator), postscript_(allocator) {}

bool TrueTypeFont::find_table(uint32_t t, ConstByteView* out) const {
  for (uint32_t i = 0; i < table_count_; ++i) {
    if (tables_[i].tag == t) {
      *out = tables_[i].data;
      return true;
    }
  }
  return false;
}

bool TrueTypeFont::parse(ConstByteView sfnt, uint32_t face_index) {
  valid_ = false;
  table_count_ = 0;
  kern_count_ = 0;
  num_glyphs_ = 0;
  family_.clear();
  style_.clear();
  full_.clear();
  postscript_.clear();
  metrics_ = FaceMetrics();

  ByteReader r(sfnt);
  uint32_t version = r.be32();
  if (!r.ok()) return false;
  if (version == tag('t', 't', 'c', 'f')) {
    r.be32();  // collection version
    const uint32_t count = r.be32();
    if (!r.ok() || count == 0 || face_index >= count || count > 1024) return false;
    if (!r.skip(uint64_t{face_index} * 4)) return false;
    const uint32_t offset = r.be32();
    if (!r.ok() || !r.seek(offset)) return false;
    version = r.be32();
    if (!r.ok()) return false;
  } else if (face_index != 0) {
    return false;
  }
  if (version != 0x00010000 && version != tag('t', 'r', 'u', 'e')) return false;  // 'OTTO' (CFF) is not supported
  const uint32_t num_tables = r.be16();
  r.skip(6);
  if (!r.ok() || num_tables == 0 || num_tables > kMaxTables) return false;
  for (uint32_t i = 0; i < num_tables; ++i) {
    const uint32_t t = r.be32();
    r.be32();  // checksum
    const uint32_t offset = r.be32();
    const uint32_t length = r.be32();
    if (!r.ok()) return false;
    ConstByteView data;
    if (!sfnt.try_subview(offset, length, &data)) return false;  // a table that runs past the file is corrupt
    tables_[table_count_].tag = t;
    tables_[table_count_].data = data;
    ++table_count_;
  }

  ConstByteView head, maxp, hhea, cmap;
  if (!find_table(tag('h', 'e', 'a', 'd'), &head) || !find_table(tag('m', 'a', 'x', 'p'), &maxp) || !find_table(tag('h', 'h', 'e', 'a'), &hhea) ||
      !find_table(tag('h', 'm', 't', 'x'), &hmtx_) || !find_table(tag('c', 'm', 'a', 'p'), &cmap) || !find_table(tag('l', 'o', 'c', 'a'), &loca_) ||
      !find_table(tag('g', 'l', 'y', 'f'), &glyf_))
    return false;

  bool ok = true;
  if (be32_at(head, 12, &ok) != 0x5F0F3CF5) return false;
  const uint32_t upem = be16_at(head, 18, &ok);
  const uint32_t mac_style = be16_at(head, 44, &ok);
  const int32_t loca_format = be16s_at(head, 50, &ok);
  if (!ok || upem < 16 || upem > 16384 || (loca_format != 0 && loca_format != 1)) return false;
  long_loca_ = loca_format == 1;
  metrics_.units_per_em = upem;
  metrics_.bold = (mac_style & 1) != 0;
  metrics_.italic = (mac_style & 2) != 0;
  metrics_.weight = metrics_.bold ? 700 : 400;

  num_glyphs_ = be16_at(maxp, 4, &ok);
  if (!ok || num_glyphs_ == 0) return false;
  // loca has numGlyphs + 1 offsets
  const uint64_t loca_needed = (uint64_t{num_glyphs_} + 1) * (long_loca_ ? 4 : 2);
  if (loca_.size() < loca_needed) return false;

  metrics_.ascent = be16s_at(hhea, 4, &ok);
  metrics_.descent = -be16s_at(hhea, 6, &ok);
  metrics_.line_gap = be16s_at(hhea, 8, &ok);
  num_hmetrics_ = be16_at(hhea, 34, &ok);
  if (!ok || num_hmetrics_ == 0) return false;
  if (num_hmetrics_ > num_glyphs_) num_hmetrics_ = num_glyphs_;
  if (hmtx_.size() < uint64_t{num_hmetrics_} * 4) return false;

  if (!parse_cmap(cmap)) return false;
  ConstByteView kern, os2, name;
  if (find_table(tag('k', 'e', 'r', 'n'), &kern)) parse_kern(kern);  // optional: a bad kern table just means no kerning
  if (find_table(tag('O', 'S', '/', '2'), &os2)) parse_os2(os2);
  if (find_table(tag('n', 'a', 'm', 'e'), &name)) parse_names(name);
  valid_ = true;
  return true;
}

bool TrueTypeFont::parse_cmap(ConstByteView cmap) {
  ByteReader r(cmap);
  r.be16();  // version
  const uint32_t count = r.be16();
  if (!r.ok()) return false;
  // Prefer the full Unicode table (format 12), keep the BMP table (format 4) as a second choice.
  int rank4 = -1, rank12 = -1;
  cmap4_ = ConstByteView();
  cmap12_ = ConstByteView();
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t platform = r.be16();
    const uint32_t encoding = r.be16();
    const uint32_t offset = r.be32();
    if (!r.ok()) return false;
    ConstByteView sub = cmap.dropped(offset > cmap.size() ? cmap.size() : offset);
    bool ok = true;
    const uint32_t format = be16_at(sub, 0, &ok);
    if (!ok) continue;
    const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
    if (!unicode) continue;
    const int rank = (platform == 3) ? 2 : 1;
    if (format == 4 && rank > rank4) {
      rank4 = rank;
      cmap4_ = sub;
    } else if (format == 12 && rank > rank12) {
      rank12 = rank;
      cmap12_ = sub;
    }
  }
  return !cmap4_.empty() || !cmap12_.empty();
}

bool TrueTypeFont::parse_kern(ConstByteView kern) {
  ByteReader r(kern);
  if (r.be16() != 0) return false;  // Apple's 32-bit version is not handled
  const uint32_t n = r.be16();
  if (!r.ok()) return false;
  uint64_t pos = 4;
  for (uint32_t i = 0; i < n && kern_count_ < kMaxKernTables; ++i) {
    bool ok = true;
    const uint32_t length = be16_at(kern, pos + 2, &ok);
    const uint32_t coverage = be16_at(kern, pos + 4, &ok);
    if (!ok) return kern_count_ != 0;
    uint64_t end = pos + (length == 0 ? kern.size() : length);  // a length of 0 is used by tables over 64 KiB
    if (end > kern.size()) end = kern.size();
    const bool horizontal = (coverage & 1) != 0, minimum = (coverage & 2) != 0, cross = (coverage & 4) != 0;
    const uint32_t format = coverage >> 8;
    if (format == 0 && horizontal && !minimum && !cross && end >= pos + 14) {
      uint64_t pairs = be16_at(kern, pos + 6, &ok);
      const uint64_t room = (end - (pos + 14)) / 6;
      if (pairs > room) pairs = room;  // a count that overstates the data is clamped to what is there
      if (pairs) kern_[kern_count_++] = kern.subview(pos + 14, pairs * 6);
    }
    if (end <= pos) break;
    pos = end;
  }
  return kern_count_ != 0;
}

void TrueTypeFont::parse_os2(ConstByteView os2) {
  bool ok = true;
  const uint32_t version = be16_at(os2, 0, &ok);
  const uint32_t weight = be16_at(os2, 4, &ok);
  const uint32_t width = be16_at(os2, 6, &ok);
  if (!ok) return;
  if (weight >= 1 && weight <= 1000) metrics_.weight = static_cast<uint16_t>(weight);
  static const uint16_t kWidthPercent[10] = {100, 50, 62, 75, 87, 100, 112, 125, 150, 200};
  metrics_.stretch = width < 10 ? kWidthPercent[width] : 100;
  if (os2.size() < 78) return;
  const uint32_t selection = be16_at(os2, 62, &ok);
  if (selection & 1) metrics_.italic = true;
  if (selection & 0x20) metrics_.bold = true;
  const int32_t typo_asc = be16s_at(os2, 68, &ok), typo_desc = be16s_at(os2, 70, &ok), typo_gap = be16s_at(os2, 72, &ok);
  const uint32_t win_asc = be16_at(os2, 74, &ok), win_desc = be16_at(os2, 76, &ok);
  if (!ok) return;
  if ((selection & 0x80) && (typo_asc != 0 || typo_desc != 0)) {  // USE_TYPO_METRICS
    metrics_.ascent = typo_asc;
    metrics_.descent = -typo_desc;
    metrics_.line_gap = typo_gap;
  } else if (metrics_.ascent == 0 && metrics_.descent == 0) {
    metrics_.ascent = static_cast<int32_t>(win_asc);
    metrics_.descent = static_cast<int32_t>(win_desc);
    metrics_.line_gap = 0;
  }
  if (version >= 2 && os2.size() >= 90) {
    metrics_.x_height = be16s_at(os2, 86, &ok);
    metrics_.cap_height = be16s_at(os2, 88, &ok);
    if (!ok || metrics_.x_height < 0) metrics_.x_height = 0;
    if (metrics_.cap_height < 0) metrics_.cap_height = 0;
  }
}

void TrueTypeFont::parse_names(ConstByteView name) {
  ByteReader r(name);
  r.be16();
  const uint32_t count = r.be16();
  const uint32_t string_offset = r.be16();
  if (!r.ok()) return;
  // Pick, per name id, the Windows Unicode record, else the Unicode, else the Mac one.
  String* targets[7] = {nullptr, &family_, &style_, nullptr, &full_, nullptr, &postscript_};
  int ranks[7] = {-1, -1, -1, -1, -1, -1, -1};
  ConstByteView chosen[7];
  bool utf16[7] = {};
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t platform = r.be16();
    r.be16();  // encoding
    r.be16();  // language
    const uint32_t id = r.be16();
    const uint32_t length = r.be16();
    const uint32_t offset = r.be16();
    if (!r.ok()) return;
    if (id >= 7 || !targets[id]) continue;
    const int rank = platform == 3 ? 3 : platform == 0 ? 2 : platform == 1 ? 1 : 0;
    if (rank == 0 || rank <= ranks[id]) continue;
    ConstByteView raw;
    if (!name.try_subview(uint64_t{string_offset} + offset, length, &raw)) continue;
    ranks[id] = rank;
    chosen[id] = raw;
    utf16[id] = platform != 1;
  }
  for (uint32_t id = 0; id < 7; ++id)
    if (targets[id] && ranks[id] > 0) append_name_string(targets[id], chosen[id], utf16[id]);
}

ScaledMetrics TrueTypeFont::scaled_metrics(Fixed size) const {
  ScaledMetrics m;
  const uint32_t upem = metrics_.units_per_em;
  m.ascent = scale_units(metrics_.ascent, size, upem);
  m.descent = scale_units(metrics_.descent, size, upem);
  m.line_gap = scale_units(metrics_.line_gap, size, upem);
  m.x_height = metrics_.x_height ? scale_units(metrics_.x_height, size, upem) : m.ascent / 2;
  m.cap_height = metrics_.cap_height ? scale_units(metrics_.cap_height, size, upem) : m.ascent;
  m.zero_advance = advance(glyph_for_codepoint('0'), size);
  return m;
}

uint32_t TrueTypeFont::lookup_format4(ConstByteView sub, uint32_t cp) const {
  if (cp > 0xFFFF) return 0;
  bool ok = true;
  const uint64_t seg_x2 = be16_at(sub, 6, &ok);
  if (!ok || seg_x2 < 2) return 0;
  const uint64_t seg_count = seg_x2 / 2;
  const uint64_t ends = 14, starts = 16 + seg_x2, deltas = 16 + 2 * seg_x2, offsets = 16 + 3 * seg_x2;
  uint64_t lo = 0, hi = seg_count;  // first segment whose end code is >= cp
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo) / 2;
    const uint32_t end = be16_at(sub, ends + 2 * mid, &ok);
    if (!ok) return 0;
    if (end < cp) lo = mid + 1;
    else hi = mid;
  }
  if (lo >= seg_count) return 0;
  const uint32_t start = be16_at(sub, starts + 2 * lo, &ok);
  if (!ok || cp < start) return 0;
  const uint32_t delta = be16_at(sub, deltas + 2 * lo, &ok);
  const uint32_t range_offset = be16_at(sub, offsets + 2 * lo, &ok);
  if (!ok) return 0;
  uint32_t glyph;
  if (range_offset == 0) {
    glyph = (cp + delta) & 0xFFFF;
  } else {
    glyph = be16_at(sub, offsets + 2 * lo + range_offset + 2 * uint64_t{cp - start}, &ok);
    if (!ok) return 0;
    if (glyph != 0) glyph = (glyph + delta) & 0xFFFF;
  }
  return glyph < num_glyphs_ ? glyph : 0;
}

uint32_t TrueTypeFont::lookup_format12(ConstByteView sub, uint32_t cp) const {
  bool ok = true;
  uint64_t groups = be32_at(sub, 12, &ok);
  if (!ok) return 0;
  const uint64_t room = sub.size() >= 16 ? (sub.size() - 16) / 12 : 0;
  if (groups > room) groups = room;
  uint64_t lo = 0, hi = groups;
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo) / 2;
    const uint32_t end = be32_at(sub, 16 + 12 * mid + 4, &ok);
    if (!ok) return 0;
    if (end < cp) lo = mid + 1;
    else hi = mid;
  }
  if (lo >= groups) return 0;
  const uint32_t start = be32_at(sub, 16 + 12 * lo, &ok);
  const uint32_t first_glyph = be32_at(sub, 16 + 12 * lo + 8, &ok);
  if (!ok || cp < start) return 0;
  const uint64_t glyph = uint64_t{first_glyph} + (cp - start);
  return glyph < num_glyphs_ ? static_cast<uint32_t>(glyph) : 0;
}

uint32_t TrueTypeFont::glyph_for_codepoint(uint32_t cp) const {
  if (!valid_) return 0;
  if (!cmap12_.empty()) {
    const uint32_t g = lookup_format12(cmap12_, cp);
    if (g) return g;
  }
  if (!cmap4_.empty()) return lookup_format4(cmap4_, cp);
  return 0;
}

uint32_t TrueTypeFont::advance_units(uint32_t glyph) const {
  if (!valid_ || glyph >= num_glyphs_) return 0;
  const uint32_t index = glyph < num_hmetrics_ ? glyph : num_hmetrics_ - 1;
  bool ok = true;
  const uint32_t advance = be16_at(hmtx_, uint64_t{index} * 4, &ok);
  return ok ? advance : 0;
}

int32_t TrueTypeFont::kerning_units(uint32_t left, uint32_t right) const {
  if (left > 0xFFFF || right > 0xFFFF) return 0;
  const uint32_t key = (left << 16) | right;
  int32_t total = 0;
  for (uint32_t t = 0; t < kern_count_; ++t) {
    const ConstByteView pairs = kern_[t];
    uint64_t lo = 0, hi = pairs.size() / 6;
    while (lo < hi) {
      const uint64_t mid = lo + (hi - lo) / 2;
      bool ok = true;
      const uint32_t k = be32_at(pairs, mid * 6, &ok);
      if (!ok) break;
      if (k == key) {
        total += be16s_at(pairs, mid * 6 + 4, &ok);
        break;
      }
      if (k < key) lo = mid + 1;
      else hi = mid;
    }
  }
  return total;
}

bool TrueTypeFont::glyph_data(uint32_t glyph, ConstByteView* out) const {
  if (glyph >= num_glyphs_) return false;
  bool ok = true;
  uint64_t start, end;
  if (long_loca_) {
    start = be32_at(loca_, uint64_t{glyph} * 4, &ok);
    end = be32_at(loca_, uint64_t{glyph} * 4 + 4, &ok);
  } else {
    start = uint64_t{be16_at(loca_, uint64_t{glyph} * 2, &ok)} * 2;
    end = uint64_t{be16_at(loca_, uint64_t{glyph} * 2 + 2, &ok)} * 2;
  }
  if (!ok || end < start) return false;
  return glyf_.try_subview(start, end - start, out);
}

bool TrueTypeFont::load_outline(uint32_t glyph, Outline* out) const {
  out->clear();
  if (!valid_) return false;
  uint32_t budget = kMaxComponents;
  if (append_glyph(glyph, 0, &budget, out)) return true;
  out->clear();
  return false;
}

bool TrueTypeFont::append_glyph(uint32_t glyph, uint32_t depth, uint32_t* budget, Outline* out) const {
  if (depth > kMaxCompositeDepth) return false;
  ConstByteView data;
  if (!glyph_data(glyph, &data)) return false;
  if (data.empty()) return true;  // no outline (a space)
  ByteReader r(data);
  const int32_t contours = static_cast<int16_t>(r.be16());
  r.skip(8);  // bounding box
  if (!r.ok()) return false;
  return contours >= 0 ? append_simple(r, contours, out) : append_composite(r, depth, budget, out);
}

bool TrueTypeFont::append_simple(ByteReader& r, int32_t contours, Outline* out) const {
  if (contours == 0) return true;
  const uint64_t base = out->points.count();
  uint32_t last = 0;
  for (int32_t c = 0; c < contours; ++c) {
    const uint32_t end = r.be16();
    if (!r.ok() || (c > 0 && end <= last)) return false;  // contour ends must increase
    last = end;
    if (base + end >= UINT32_MAX || !out->contour_ends.push_back(static_cast<uint32_t>(base + end))) return false;
  }
  const uint64_t total = uint64_t{last} + 1;
  if (total > kMaxPoints || base + total > kMaxPoints) return false;
  const uint32_t instructions = r.be16();
  if (!r.skip(instructions)) return false;

  ByteBuffer flags(allocator_);
  while (flags.size() < total) {
    const uint8_t f = r.u8();
    if (!r.ok()) return false;
    if (!flags.put8(f)) return false;
    if (f & 8) {
      uint32_t repeat = r.u8();
      if (!r.ok() || repeat > total - flags.size()) return false;
      while (repeat--)
        if (!flags.put8(f)) return false;
    }
  }
  if (!out->points.reserve(base + total)) return false;
  const ConstByteView fl = flags.view();
  for (uint64_t i = 0; i < total; ++i) {
    OutlinePoint p;
    p.on_curve = (fl[i] & 1) != 0;
    if (!out->points.push_back(p)) return false;
  }
  int64_t v = 0;
  for (uint64_t i = 0; i < total; ++i) {
    const uint8_t f = fl[i];
    if (f & 2) {
      const int32_t d = r.u8();
      v += (f & 16) ? d : -d;
    } else if (!(f & 16)) {
      v += static_cast<int16_t>(r.be16());
    }
    if (!r.ok()) return false;
    out->points[base + i].x = clamp_coordinate(v);
  }
  v = 0;
  for (uint64_t i = 0; i < total; ++i) {
    const uint8_t f = fl[i];
    if (f & 4) {
      const int32_t d = r.u8();
      v += (f & 32) ? d : -d;
    } else if (!(f & 32)) {
      v += static_cast<int16_t>(r.be16());
    }
    if (!r.ok()) return false;
    out->points[base + i].y = clamp_coordinate(v);
  }
  return true;
}

bool TrueTypeFont::append_composite(ByteReader& r, uint32_t depth, uint32_t* budget, Outline* out) const {
  uint32_t flags;
  do {
    flags = r.be16();
    const uint32_t component = r.be16();
    if (!r.ok()) return false;
    int32_t arg1, arg2;
    if (flags & 1) {  // ARG_1_AND_2_ARE_WORDS
      const uint32_t a = r.be16(), b = r.be16();
      arg1 = (flags & 2) ? static_cast<int16_t>(a) : static_cast<int32_t>(a);
      arg2 = (flags & 2) ? static_cast<int16_t>(b) : static_cast<int32_t>(b);
    } else {
      const uint32_t a = r.u8(), b = r.u8();
      arg1 = (flags & 2) ? static_cast<int8_t>(a) : static_cast<int32_t>(a);
      arg2 = (flags & 2) ? static_cast<int8_t>(b) : static_cast<int32_t>(b);
    }
    int32_t m00 = 0x4000, m01 = 0, m10 = 0, m11 = 0x4000;  // x' = m00*x + m10*y, y' = m01*x + m11*y
    if (flags & 8) {
      m00 = m11 = static_cast<int16_t>(r.be16());
    } else if (flags & 0x40) {
      m00 = static_cast<int16_t>(r.be16());
      m11 = static_cast<int16_t>(r.be16());
    } else if (flags & 0x80) {
      m00 = static_cast<int16_t>(r.be16());
      m01 = static_cast<int16_t>(r.be16());
      m10 = static_cast<int16_t>(r.be16());
      m11 = static_cast<int16_t>(r.be16());
    }
    if (!r.ok() || *budget == 0) return false;
    --*budget;

    const uint64_t start = out->points.count();
    if (!append_glyph(component, depth + 1, budget, out)) return false;
    const uint64_t end = out->points.count();

    const bool identity = m00 == 0x4000 && m01 == 0 && m10 == 0 && m11 == 0x4000;
    if (!identity) {
      for (uint64_t i = start; i < end; ++i) {
        OutlinePoint& p = out->points[i];
        const int32_t x = p.x, y = p.y;
        p.x = transform14(m00, x, m10, y);
        p.y = transform14(m01, x, m11, y);
      }
    }
    int32_t dx, dy;
    if (flags & 2) {  // ARGS_ARE_XY_VALUES
      dx = arg1;
      dy = arg2;
      if (!identity && (flags & 0x800) && !(flags & 0x1000)) {  // SCALED_COMPONENT_OFFSET
        const int32_t sx = transform14(m00, arg1, m10, arg2), sy = transform14(m01, arg1, m11, arg2);
        dx = sx;
        dy = sy;
      }
    } else {  // point matching: align point arg2 of the component with point arg1 of what is already there
      if (static_cast<uint64_t>(arg1) >= start || start + static_cast<uint64_t>(arg2) >= end) return false;
      dx = out->points[static_cast<uint64_t>(arg1)].x - out->points[start + static_cast<uint64_t>(arg2)].x;
      dy = out->points[static_cast<uint64_t>(arg1)].y - out->points[start + static_cast<uint64_t>(arg2)].y;
    }
    if (dx != 0 || dy != 0) {
      for (uint64_t i = start; i < end; ++i) {
        OutlinePoint& p = out->points[i];
        p.x = clamp_coordinate(int64_t{p.x} + dx);
        p.y = clamp_coordinate(int64_t{p.y} + dy);
      }
    }
  } while (flags & 0x20);  // MORE_COMPONENTS
  return true;
}

}  // namespace font
