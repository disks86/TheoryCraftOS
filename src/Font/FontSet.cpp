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
#include "Font/FontSet.hpp"
#include "Font/WoffDecode.hpp"
#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"

namespace font {

namespace {

constexpr StringView kSansName = "TCOS Sans";
constexpr StringView kSerifName = "TCOS Serif";
constexpr StringView kMonoName = "TCOS Mono";
constexpr StringView kFallbackName = "TCOS Fallback";
constexpr uint32_t kFallbackInstance = 0xFFFFFFFFu;
constexpr uint32_t kSpecialFaceKey = 0x3FFFFE;  // glyph cache face ids of the console font (+1: the tofu box)
constexpr uint32_t kMaxFamilies = 16;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool equals_ignore_case(StringView a, StringView b) {
  if (a.size() != b.size()) return false;
  for (uint64_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

StringView generic_family_name(GenericFamily g) {
  switch (g) {
    case GenericFamily::SansSerif: return kSansName;
    case GenericFamily::Monospace: return kMonoName;
    default: return kSerifName;
  }
}

// CSS generic family keywords (and the system-ui style ones) -> the bundled family that stands in for them.
bool generic_keyword(StringView name, StringView* bundled) {
  struct Entry { const char* keyword; int which; };
  static const Entry kEntries[] = {
      {"serif", 0}, {"ui-serif", 0}, {"fangsong", 0},
      {"sans-serif", 1}, {"system-ui", 1}, {"ui-sans-serif", 1}, {"ui-rounded", 1}, {"cursive", 1}, {"fantasy", 1}, {"emoji", 3}, {"math", 0},
      {"monospace", 2}, {"ui-monospace", 2},
  };
  for (const Entry& e : kEntries) {
    if (equals_ignore_case(name, e.keyword)) {
      *bundled = e.which == 0 ? kSerifName : e.which == 1 ? kSansName : e.which == 2 ? kMonoName : kFallbackName;
      return true;
    }
  }
  return false;
}

// Names of common fonts that a page asks for by name and that the bundled faces stand in for (the Liberation
// faces are metric compatible with Arial, Times New Roman and Courier New).
bool alias_family(StringView name, StringView* bundled) {
  static const char* const kSans[] = {"arial", "helvetica", "helvetica neue", "verdana", "tahoma", "segoe ui", "roboto", "open sans", "-apple-system",
                                      "blinkmacsystemfont", "liberation sans", "noto sans", "inter", "san francisco", "lucida grande", "trebuchet ms",
                                      "dejavu sans", "ubuntu", "cantarell", "oxygen", "fira sans", "droid sans", "arimo", "sans"};
  static const char* const kSerif[] = {"times new roman", "times", "georgia", "liberation serif", "noto serif", "palatino", "palatino linotype",
                                       "book antiqua", "garamond", "cambria", "dejavu serif", "tinos", "droid serif"};
  static const char* const kMono[] = {"courier new", "courier", "consolas", "menlo", "monaco", "sf mono", "liberation mono", "lucida console",
                                      "dejavu sans mono", "noto sans mono", "fira code", "fira mono", "source code pro", "ubuntu mono", "cousine",
                                      "jetbrains mono", "droid sans mono"};
  for (const char* n : kSans)
    if (equals_ignore_case(name, n)) { *bundled = kSansName; return true; }
  for (const char* n : kSerif)
    if (equals_ignore_case(name, n)) { *bundled = kSerifName; return true; }
  for (const char* n : kMono)
    if (equals_ignore_case(name, n)) { *bundled = kMonoName; return true; }
  return false;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// Reads the next family name of a comma separated list into `name`: quotes removed, unquoted whitespace runs
// collapsed to one space. False at the end of the list.
bool next_family(StringView list, uint64_t* pos, String* name) {
  name->clear();
  uint64_t i = *pos;
  while (i < list.size() && (is_space(list[i]) || list[i] == ',')) ++i;
  if (i >= list.size()) {
    *pos = i;
    return false;
  }
  if (list[i] == '"' || list[i] == '\'') {
    const char quote = list[i++];
    while (i < list.size() && list[i] != quote) {
      if (list[i] == '\\' && i + 1 < list.size()) ++i;
      name->push_back(list[i++]);
    }
    if (i < list.size()) ++i;
    while (i < list.size() && list[i] != ',') ++i;  // junk after the closing quote
  } else {
    bool pending_space = false;
    while (i < list.size() && list[i] != ',') {
      if (is_space(list[i])) {
        pending_space = !name->empty();
      } else {
        if (pending_space) name->push_back(' ');
        pending_space = false;
        name->push_back(list[i]);
      }
      ++i;
    }
  }
  *pos = i;
  return true;
}

uint64_t fnv(uint64_t h, uint8_t byte) { return (h ^ byte) * 1099511628211ull; }

uint64_t hash_query(StringView families, uint32_t weight, uint32_t style, uint32_t stretch, uint32_t generic) {
  uint64_t h = 1469598103934665603ull;
  for (const char c : families) h = fnv(h, static_cast<uint8_t>(c));
  h = fnv(h, 0);
  const uint32_t values[4] = {weight, style, stretch, generic};
  for (const uint32_t v : values) {
    h = fnv(h, static_cast<uint8_t>(v));
    h = fnv(h, static_cast<uint8_t>(v >> 8));
  }
  return h;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// The console font scaled to the requested size with area averaging.
bool render_console_glyph(uint32_t codepoint, Fixed size, allocators::IAllocator& allocator, GlyphBitmap* out) {
  const int32_t h = fixed_round(size) < 1 ? 1 : fixed_round(size);
  const int32_t w = fixed_round(size / 2) < 1 ? 1 : fixed_round(size / 2);
  if (h > static_cast<int32_t>(kMaxGlyphDimension) || w > static_cast<int32_t>(kMaxGlyphDimension)) return out->allocate(0, 0);
  if (!out->allocate(static_cast<uint32_t>(w), static_cast<uint32_t>(h))) return false;
  ArrayView<uint32_t> acc = allocate_view<uint32_t>(allocator, uint64_t{static_cast<uint32_t>(w)} * static_cast<uint32_t>(h));
  if (acc.empty()) return false;
  acc.fill(0);
  const ConstByteView rows = hardware::Font8x16::glyph(static_cast<char>(codepoint < 0x80 ? codepoint : '?'));
  for (int32_t sy = 0; sy < 16 && static_cast<uint64_t>(sy) < rows.size(); ++sy) {
    for (int32_t sx = 0; sx < 8; ++sx) {
      if (!(rows[static_cast<uint64_t>(sy)] & (0x80 >> sx))) continue;
      const int32_t x_lo = sx * w / 8, x_hi = ((sx + 1) * w + 7) / 8 - 1;
      const int32_t y_lo = sy * h / 16, y_hi = ((sy + 1) * h + 15) / 16 - 1;
      for (int32_t y = y_lo; y <= y_hi && y < h; ++y) {
        const int32_t oy_hi = (y + 1) * 16 < (sy + 1) * h ? (y + 1) * 16 : (sy + 1) * h, oy_lo = y * 16 > sy * h ? y * 16 : sy * h;
        if (oy_hi <= oy_lo) continue;
        for (int32_t x = x_lo; x <= x_hi && x < w; ++x) {
          const int32_t ox_hi = (x + 1) * 8 < (sx + 1) * w ? (x + 1) * 8 : (sx + 1) * w, ox_lo = x * 8 > sx * w ? x * 8 : sx * w;
          if (ox_hi <= ox_lo) continue;
          acc[uint64_t{static_cast<uint32_t>(y)} * static_cast<uint32_t>(w) + static_cast<uint32_t>(x)] +=
              static_cast<uint32_t>(ox_hi - ox_lo) * static_cast<uint32_t>(oy_hi - oy_lo);
        }
      }
    }
  }
  const ByteView pixels = out->writable_pixels();
  for (uint64_t i = 0; i < pixels.size(); ++i) {
    const uint32_t v = acc[i] * 255 / (8 * 16);
    pixels[i] = static_cast<uint8_t>(v > 255 ? 255 : v);
  }
  release_view(allocator, acc);
  out->left = 0;
  out->top = (h * 3 + 2) / 4;  // the console font's baseline sits 12 rows down a 16 row cell
  return true;
}

// A hollow box: the glyph shown for characters no font has.
bool render_tofu(Fixed size, GlyphBitmap* out) {
  int32_t w = fixed_round(size / 2), h = fixed_round(size * 7 / 10);
  if (w < 3) w = 3;
  if (h < 5) h = 5;
  const int32_t t = fixed_round(size / 16) < 1 ? 1 : fixed_round(size / 16);
  if (w > static_cast<int32_t>(kMaxGlyphDimension) || h > static_cast<int32_t>(kMaxGlyphDimension)) return out->allocate(0, 0);
  if (!out->allocate(static_cast<uint32_t>(w), static_cast<uint32_t>(h))) return false;
  const ByteView pixels = out->writable_pixels();
  for (int32_t y = 0; y < h; ++y)
    for (int32_t x = 0; x < w; ++x)
      if (x < t || y < t || x >= w - t || y >= h - t) pixels[uint64_t{static_cast<uint32_t>(y)} * static_cast<uint32_t>(w) + static_cast<uint32_t>(x)] = 255;
  out->left = fixed_round(size / 20);
  out->top = h;
  return true;
}

}  // namespace

bool parse_unicode_range(StringView css, List<CodepointRange>* out) {
  uint64_t i = 0;
  while (true) {
    while (i < css.size() && (is_space(css[i]) || css[i] == ',')) ++i;
    if (i >= css.size()) return true;
    if (i + 1 >= css.size() || lower(css[i]) != 'u' || css[i + 1] != '+') return false;
    i += 2;
    uint32_t first = 0, last = 0, digits = 0, wildcards = 0;
    while (i < css.size() && digits < 6) {
      const char c = css[i];
      if (c == '?') {
        first = first * 16;
        last = last * 16 + 15;
        ++wildcards;
      } else if (hex_value(c) >= 0 && wildcards == 0) {
        first = first * 16 + static_cast<uint32_t>(hex_value(c));
        last = first;
      } else {
        break;
      }
      ++digits;
      ++i;
    }
    if (digits == 0) return false;
    if (wildcards == 0 && i < css.size() && css[i] == '-') {
      ++i;
      uint32_t end = 0, end_digits = 0;
      while (i < css.size() && end_digits < 6 && hex_value(css[i]) >= 0) {
        end = end * 16 + static_cast<uint32_t>(hex_value(css[i]));
        ++end_digits;
        ++i;
      }
      if (end_digits == 0) return false;
      last = end;
    }
    if (i < css.size() && css[i] != ',' && !is_space(css[i])) return false;
    if (last < first || last > 0x10FFFF) continue;  // an invalid range is ignored, as CSS says
    CodepointRange range;
    range.first = first;
    range.last = last;
    if (!out->push_back(range)) return false;
  }
}

FontSet::FontSet(allocators::IAllocator& allocator, GlyphCacheLimits cache_limits)
    : allocator_(allocator), faces_(allocator), instances_(allocator), instance_index_(allocator), cache_(allocator, cache_limits), fallback_(allocator) {}

bool FontSet::add_face(UniquePointer<Face>&& face) {
  if (!face || faces_.count() >= kMaxFaces) return false;
  if (!faces_.push_back(static_cast<UniquePointer<Face>&&>(face))) return false;
  ++generation_;
  return true;
}

void FontSet::add_bundled_family(ArrayView<const BundledFontInfo> infos) {
  for (const BundledFontInfo& info : infos) {
    UniquePointer<Face> face = UniquePointer<Face>::make(allocator_, allocator_);
    if (!face) return;
    face->family.assign(info.family);
    face->weight = info.weight;
    face->style = info.italic ? FontStyle::Italic : FontStyle::Normal;
    face->stretch = 100;
    face->bundled = true;
    face->bundled_data = info.woff;
    add_face(static_cast<UniquePointer<Face>&&>(face));
  }
}

bool FontSet::add_bundled_fonts() {
  if (bundled_added_) return true;
  bundled_added_ = true;
  add_bundled_family(bundled_sans_faces());
  add_bundled_family(bundled_serif_faces());
  add_bundled_family(bundled_mono_faces());
  add_bundled_family(bundled_fallback_faces());
  return faces_.count() != 0;
}

bool FontSet::add_web_font(const WebFontDescriptor& d, ConstByteView data, uint32_t* face_id) {
  if (d.family.empty() || faces_.count() >= kMaxFaces || web_font_bytes_ >= kMaxWebFontBytes) return false;
  UniquePointer<LoadedFont> loaded = UniquePointer<LoadedFont>::make(allocator_, allocator_);
  if (!loaded) return false;
  const uint64_t room = kMaxWebFontBytes - web_font_bytes_;
  if (!font_to_sfnt(data, &loaded->storage, room < kMaxSingleFontBytes ? room : kMaxSingleFontBytes)) return false;
  if (!loaded->font.parse(loaded->storage.view())) return false;
  UniquePointer<Face> face = UniquePointer<Face>::make(allocator_, allocator_);
  if (!face || !face->family.assign(d.family)) return false;
  const FaceMetrics& fm = loaded->font.metrics();
  face->weight = d.weight ? d.weight : fm.weight;
  face->style = d.style_from_file ? (fm.italic ? FontStyle::Italic : FontStyle::Normal) : d.style;
  face->stretch = d.stretch ? d.stretch : fm.stretch;
  for (const CodepointRange& r : d.ranges)
    if (!face->ranges.push_back(r)) return false;
  web_font_bytes_ += loaded->storage.size();
  face->loaded = static_cast<UniquePointer<LoadedFont>&&>(loaded);
  const uint32_t id = static_cast<uint32_t>(faces_.count());
  if (!add_face(static_cast<UniquePointer<Face>&&>(face))) return false;
  if (face_id) *face_id = id;
  return true;
}

TrueTypeFont* FontSet::load(uint32_t face_id) {
  if (face_id >= faces_.count()) return nullptr;
  Face& f = *faces_[face_id];
  if (f.broken) return nullptr;
  if (!f.loaded) {
    UniquePointer<LoadedFont> loaded = UniquePointer<LoadedFont>::make(allocator_, allocator_);
    if (!loaded || !f.bundled || !font_to_sfnt(f.bundled_data, &loaded->storage, kMaxSingleFontBytes) || !loaded->font.parse(loaded->storage.view())) {
      f.broken = true;
      return nullptr;
    }
    f.loaded = static_cast<UniquePointer<LoadedFont>&&>(loaded);
  }
  return &f.loaded->font;
}

const TrueTypeFont* FontSet::font_of(uint32_t face) { return load(face); }

bool FontSet::face_info(uint32_t face, FaceInfo* out) const {
  if (face >= faces_.count()) return false;
  const Face& f = *faces_[face];
  out->family = f.family.view();
  out->weight = f.weight;
  out->style = f.style;
  out->stretch = f.stretch;
  out->bundled = f.bundled;
  return true;
}

bool FontSet::covers(const Face& face, uint32_t codepoint) const {
  if (face.ranges.count() == 0) return true;
  for (const CodepointRange& r : face.ranges.view())
    if (codepoint >= r.first && codepoint <= r.last) return true;
  return false;
}

// CSS Fonts 4 section 5.2 as a sort key (smaller is better): stretch distance first, then style, then weight.
uint64_t FontSet::rank(const Face& face, const Instance& instance) const {
  const uint32_t d = instance.stretch, s = face.stretch;
  const uint32_t stretch_key = d <= 100 ? (s <= d ? d - s : 1000 + (s - d)) : (s >= d ? s - d : 1000 + (d - s));
  uint32_t style_key;
  switch (instance.style) {
    case FontStyle::Italic: style_key = face.style == FontStyle::Italic ? 0 : face.style == FontStyle::Oblique ? 1 : 2; break;
    case FontStyle::Oblique: style_key = face.style == FontStyle::Oblique ? 0 : face.style == FontStyle::Italic ? 1 : 2; break;
    default: style_key = face.style == FontStyle::Normal ? 0 : face.style == FontStyle::Oblique ? 1 : 2; break;
  }
  const uint32_t wd = instance.weight, w = face.weight;
  uint32_t weight_key;
  if (wd >= 400 && wd <= 500) {
    if (w >= wd && w <= 500) weight_key = w - wd;
    else if (w < wd) weight_key = 1000 + (wd - w);
    else weight_key = 2000 + (w - 500);
  } else if (wd < 400) {
    weight_key = w <= wd ? wd - w : 1000 + (w - wd);
  } else {
    weight_key = w >= wd ? w - wd : 1000 + (wd - w);
  }
  return (uint64_t{stretch_key} << 40) | (uint64_t{style_key} << 32) | weight_key;
}

FontSet::Instance* FontSet::instance_at(uint32_t index) {
  Instance* instance = index < instances_.count() ? instances_[index].get() : &fallback_;
  if (instance->generation != generation_) rebuild_groups(*instance);
  return instance;
}

void FontSet::append_family_faces(Instance& instance, StringView family) {
  const uint32_t before = static_cast<uint32_t>(instance.group_faces.count());
  for (uint32_t i = 0; i < faces_.count(); ++i)
    if (equals_ignore_case(faces_[i]->family.view(), family)) instance.group_faces.push_back(i);
  if (instance.group_faces.count() != before) instance.group_starts.push_back(before);
}

void FontSet::append_group(Instance& instance, StringView name) {
  StringView bundled;
  if (generic_keyword(name, &bundled)) {
    append_family_faces(instance, bundled);
    return;
  }
  const uint64_t before = instance.group_faces.count();
  append_family_faces(instance, name);  // a web font (or bundled family name) of exactly that name wins ...
  if (instance.group_faces.count() == before && alias_family(name, &bundled)) append_family_faces(instance, bundled);  // ... then the stand-ins
}

void FontSet::rebuild_groups(Instance& instance) {
  instance.group_faces.clear();
  instance.group_starts.clear();
  String name(allocator_);
  uint64_t pos = 0, count = 0;
  const StringView list = instance.families.view();
  while (count < kMaxFamilies && next_family(list, &pos, &name)) {
    ++count;
    append_group(instance, name.view());
  }
  append_family_faces(instance, generic_family_name(instance.default_generic));
  append_family_faces(instance, kSansName);
  append_family_faces(instance, kFallbackName);
  instance.ascii_face.clear();
  instance.ascii_glyph.clear();
  for (uint32_t i = 0; i < 128; ++i) {
    instance.ascii_face.push_back(kUnresolved);
    instance.ascii_glyph.push_back(0);
  }
  instance.generation = generation_;
}

FontHandle FontSet::match(const FontQuery& q) {
  FontHandle handle;
  Fixed size = q.size;
  if (size < 0) size = 0;
  if (size > 16384 * kFixedOne) size = 16384 * kFixedOne;
  handle.size = size;
  const uint16_t weight = q.weight < 1 ? 1 : q.weight > 1000 ? 1000 : q.weight;
  const uint16_t stretch = q.stretch < 50 ? 50 : q.stretch > 200 ? 200 : q.stretch;
  const uint64_t h = hash_query(q.families, weight, static_cast<uint32_t>(q.style), stretch, static_cast<uint32_t>(q.default_generic));
  auto same = [&](const Instance& i) {
    return i.weight == weight && i.style == q.style && i.stretch == stretch && i.default_generic == q.default_generic && i.families.view().equals(q.families);
  };
  if (const uint32_t* found = instance_index_.find(h)) {
    if (same(*instances_[*found])) {
      handle.instance = *found;
      return handle;
    }
  }
  for (uint32_t i = 0; i < instances_.count(); ++i) {  // only reached for a hash collision or a new request
    if (same(*instances_[i])) {
      handle.instance = i;
      return handle;
    }
  }
  handle.instance = kFallbackInstance;
  if (instances_.count() >= kMaxInstances) return handle;
  UniquePointer<Instance> instance = UniquePointer<Instance>::make(allocator_, allocator_);
  if (!instance || !instance->families.assign(q.families)) return handle;
  instance->weight = weight;
  instance->style = q.style;
  instance->stretch = stretch;
  instance->default_generic = q.default_generic;
  const uint32_t index = static_cast<uint32_t>(instances_.count());
  if (!instances_.push_back(static_cast<UniquePointer<Instance>&&>(instance))) return handle;
  instance_index_.put(h, index);
  handle.instance = index;
  return handle;
}

uint32_t FontSet::pick(const Instance& instance, uint32_t begin, uint32_t end, uint32_t codepoint, uint32_t* glyph) {
  uint32_t best = kUnresolved;
  uint64_t best_rank = 0;
  for (uint32_t i = begin; i < end; ++i) {
    const uint32_t id = instance.group_faces[i];
    const Face& f = *faces_[id];
    if (f.broken || !covers(f, codepoint)) continue;
    const uint64_t r = rank(f, instance);
    if (best != kUnresolved && r >= best_rank) continue;
    const TrueTypeFont* tt = load(id);
    if (!tt) continue;
    const uint32_t g = tt->glyph_for_codepoint(codepoint);
    if (!g) continue;
    best = id;
    best_rank = r;
    *glyph = g;
  }
  return best;
}

uint32_t FontSet::resolve(Instance& instance, uint32_t codepoint, uint32_t* glyph) {
  const bool ascii = codepoint < 128 && instance.ascii_face.count() == 128;
  if (ascii && instance.ascii_face[codepoint] != kUnresolved) {
    *glyph = instance.ascii_glyph[codepoint];
    return instance.ascii_face[codepoint];
  }
  uint32_t face = kUnresolved;
  const uint32_t groups = static_cast<uint32_t>(instance.group_starts.count());
  for (uint32_t g = 0; g < groups && face == kUnresolved; ++g) {
    const uint32_t begin = instance.group_starts[g];
    const uint32_t end = g + 1 < groups ? instance.group_starts[g + 1] : static_cast<uint32_t>(instance.group_faces.count());
    face = pick(instance, begin, end, codepoint, glyph);
  }
  if (face == kUnresolved) {
    if (codepoint >= 0x20 && codepoint < 0x7F) {
      face = kFaceBitmap;
      *glyph = codepoint;
    } else {
      face = kFaceTofu;
      *glyph = codepoint;
    }
  }
  if (ascii) {
    instance.ascii_face[codepoint] = face;
    instance.ascii_glyph[codepoint] = *glyph;
  }
  return face;
}

uint32_t FontSet::primary_face(Instance& instance) {
  const uint32_t groups = static_cast<uint32_t>(instance.group_starts.count());
  for (uint32_t g = 0; g < groups; ++g) {
    const uint32_t begin = instance.group_starts[g];
    const uint32_t end = g + 1 < groups ? instance.group_starts[g + 1] : static_cast<uint32_t>(instance.group_faces.count());
    uint32_t best = kUnresolved;
    uint64_t best_rank = 0;
    for (uint32_t i = begin; i < end; ++i) {
      const uint32_t id = instance.group_faces[i];
      const Face& f = *faces_[id];
      if (f.broken || !covers(f, ' ')) continue;
      const uint64_t r = rank(f, instance);
      if (best != kUnresolved && r >= best_rank) continue;
      if (!load(id)) continue;
      best = id;
      best_rank = r;
    }
    if (best != kUnresolved) return best;
  }
  return kUnresolved;
}

ScaledMetrics FontSet::metrics(FontHandle font) {
  Instance* instance = instance_at(font.instance);
  const uint32_t face = primary_face(*instance);
  if (face != kUnresolved) return load(face)->scaled_metrics(font.size);
  ScaledMetrics m;  // the console font: 12 rows above the baseline, 4 below, 8x16 cell
  m.ascent = font.size * 3 / 4;
  m.descent = font.size / 4;
  m.x_height = font.size / 2;
  m.cap_height = font.size * 11 / 16;
  m.zero_advance = font.size / 2;
  return m;
}

const GlyphBitmap* FontSet::special_bitmap(const GlyphKey& key, uint32_t codepoint, Fixed size) {
  GlyphBitmap bitmap(allocator_);
  const bool ok = key.face == kSpecialFaceKey ? render_console_glyph(codepoint, size, allocator_, &bitmap) : render_tofu(size, &bitmap);
  if (!ok) return nullptr;
  return cache_.insert(key, static_cast<GlyphBitmap&&>(bitmap));
}

const GlyphBitmap* FontSet::glyph_bitmap(FontHandle font, uint32_t face, uint32_t glyph, uint32_t subpixel) {
  if (font.size <= 0) return nullptr;
  GlyphKey key;
  key.size = font.size;
  if (face == kFaceBitmap || face == kFaceTofu) {
    key.face = face == kFaceBitmap ? kSpecialFaceKey : kSpecialFaceKey + 1;
    key.glyph = face == kFaceBitmap ? (glyph & 0x7F) : 0;
    key.subpixel = 0;
    if (const GlyphBitmap* hit = cache_.find(key)) return hit;
    return special_bitmap(key, glyph, font.size);
  }
  key.face = face;
  key.glyph = glyph;
  key.subpixel = subpixel & 3;
  if (const GlyphBitmap* hit = cache_.find(key)) return hit;
  const TrueTypeFont* tt = load(face);
  if (!tt) return nullptr;
  GlyphBitmap bitmap(allocator_);
  if (!rasterize_glyph(*tt, glyph, font.size, key.subpixel, allocator_, &bitmap)) return nullptr;
  return cache_.insert(key, static_cast<GlyphBitmap&&>(bitmap));
}

}  // namespace font
