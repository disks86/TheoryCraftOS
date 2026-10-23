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
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Font/BundledFonts.hpp"
#include "Font/FontTypes.hpp"
#include "Font/GlyphCache.hpp"
#include "Font/Shaper.hpp"
#include "Font/TrueType.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

// The font system the layout and paint code talk to.
//
//   font::FontSet fonts(allocator);
//   fonts.add_bundled_fonts();                       // once; web fonts: fonts.add_web_font(...)
//
//   font::FontQuery q;                               // from the computed style
//   q.families = "\"Helvetica Neue\", Arial, sans-serif";
//   q.weight = 700; q.size = font::fixed_from_px(16);
//   font::FontHandle f = fonts.match(q);             // cheap: cached per distinct (families, weight, style, stretch)
//
//   font::ScaledMetrics m = fonts.metrics(f);        // ascent / descent / line gap / ex / ch
//   font::Fixed w = fonts.measure(f, "Hello");       // width in 1/64 px
//   font::GlyphRun run(allocator);
//   fonts.shape(f, text, opts, &run);                // glyphs + advances, for painting and caret mapping
//   for (auto& g : run.glyphs.view()) {
//     const font::GlyphBitmap* b = fonts.glyph_bitmap(f, g.face, g.glyph, 0);   // A8 coverage, blit at pen + (left, -top)
//     pen += g.advance;
//   }
//
// Font choice follows CSS Fonts 4: the family list is walked in order; for each family the face with the closest
// stretch, then style, then weight is chosen among those whose unicode-range includes the character and that
// really have a glyph for it ("per-codepoint fallback"). After the listed families come the generic family the
// query names, the bundled sans, the bundled wide-coverage fallback, then the 8x16 console bitmap font (printable
// ASCII only) and finally a "tofu" box, so every character gets some glyph.
//
// Thread safety: none. A FontSet belongs to the thread that lays out and paints (the browser guards it like the
// rest of the page state).
namespace font {

enum class GenericFamily : uint8_t { Serif, SansSerif, Monospace };

// An inclusive code point interval, as in @font-face unicode-range.
struct CodepointRange {
  uint32_t first = 0;
  uint32_t last = 0;
};
// Parses a CSS unicode-range value ("U+0000-00FF, U+0131, U+4??") and appends the intervals. False on a syntax
// error (what was parsed so far stays in `out`) or if memory ran out.
bool parse_unicode_range(StringView css, List<CodepointRange>* out);

struct FontQuery {
  StringView families;                                  // the CSS font-family value: comma separated, names may be quoted
  uint16_t weight = 400;                                // 1..1000
  FontStyle style = FontStyle::Normal;
  uint16_t stretch = 100;                               // percent: 50 ultra-condensed .. 100 normal .. 200 ultra-expanded
  Fixed size = 16 * kFixedOne;                          // computed font-size in px
  GenericFamily default_generic = GenericFamily::Serif; // used when no listed family exists (the UA's standard font)
};

// A resolved font at a size: what layout holds per text style. Plain data, freely copied.
struct FontHandle {
  uint32_t instance = 0;
  Fixed size = 16 * kFixedOne;
};

struct WebFontDescriptor {
  StringView family;                          // the @font-face font-family name
  uint16_t weight = 0;                        // 0: take it from the file (OS/2)
  FontStyle style = FontStyle::Normal;
  bool style_from_file = false;               // true: normal/italic from the file's flags
  uint16_t stretch = 0;                       // 0: take it from the file
  ArrayView<const CodepointRange> ranges;     // empty: all of Unicode
};

struct FaceInfo {
  StringView family;
  uint16_t weight = 400;
  FontStyle style = FontStyle::Normal;
  uint16_t stretch = 100;
  bool bundled = false;
};

class FontSet {
 public:
  static constexpr uint32_t kFaceBitmap = 0xFFFFFFFEu;  // ShapedGlyph::face for the Font8x16 console font
  static constexpr uint32_t kFaceTofu = 0xFFFFFFFFu;    // ... for the "no glyph" box
  static constexpr uint32_t kMaxFaces = 4096;
  static constexpr uint64_t kMaxWebFontBytes = 32ull * 1024 * 1024;  // all web fonts together (decoded)
  static constexpr uint64_t kMaxSingleFontBytes = 8ull * 1024 * 1024;
  static constexpr uint32_t kMaxInstances = 2048;

  explicit FontSet(allocators::IAllocator& allocator, GlyphCacheLimits cache_limits = GlyphCacheLimits());
  FontSet(const FontSet&) = delete;
  FontSet& operator=(const FontSet&) = delete;

  // Registers the embedded sans, serif, mono and fallback faces (decoded and parsed lazily on first use).
  // Calling it twice does nothing the second time.
  bool add_bundled_fonts();

  // Registers a downloaded @font-face font (TrueType in a TrueType file, collection or WOFF 1.0; WOFF2 and
  // CFF are refused). The bytes are decoded and validated now and copied; false if they are not a usable font
  // or limits are hit, in which case the @font-face should be treated as failed. Names and styles match
  // case-insensitively. Handles from match() stay valid and pick up the new face on their next use.
  bool add_web_font(const WebFontDescriptor& descriptor, ConstByteView data, uint32_t* face_id = nullptr);

  // Resolves a CSS font request to a handle. Never fails; if too many distinct requests were made, the oldest
  // shared handle (plain sans-serif) is returned. Matching is cached.
  FontHandle match(const FontQuery& query);

  // Metrics of the first available font of the handle's family list (or of the console font when there is none).
  ScaledMetrics metrics(FontHandle font);

  // Width of `utf8` in 1/64 px, exactly the sum of the advances shape() yields.
  Fixed measure(FontHandle font, StringView utf8, const ShapeOptions& options = ShapeOptions());
  // Shapes `utf8` into `run` (cleared first). False only if memory ran out.
  bool shape(FontHandle font, StringView utf8, const ShapeOptions& options, GlyphRun* run);

  // The A8 coverage bitmap of a glyph from a ShapedGlyph, at the handle's size, `subpixel` (0..3) quarters of a
  // pixel to the right. Cached in a bounded LRU; the pointer is valid until the next call of this function.
  // nullptr for a missing glyph or when memory ran out.
  const GlyphBitmap* glyph_bitmap(FontHandle font, uint32_t face, uint32_t glyph, uint32_t subpixel = 0);

  // The parsed font of a face id (loads it if needed); nullptr for the special faces or a broken font.
  const TrueTypeFont* font_of(uint32_t face);
  bool face_info(uint32_t face, FaceInfo* out) const;
  uint32_t face_count() const { return static_cast<uint32_t>(faces_.count()); }

  GlyphCache& glyph_cache() { return cache_; }

 private:
  struct LoadedFont {
    explicit LoadedFont(allocators::IAllocator& a) : storage(a), font(a) {}
    ByteBuffer storage;  // the sfnt; `font` views into it
    TrueTypeFont font;
  };
  struct Face {
    explicit Face(allocators::IAllocator& a) : family(a), ranges(a) {}
    String family;
    uint16_t weight = 400;
    FontStyle style = FontStyle::Normal;
    uint16_t stretch = 100;
    bool bundled = false;
    ConstByteView bundled_data;
    List<CodepointRange> ranges;
    UniquePointer<LoadedFont> loaded;
    bool broken = false;
  };
  struct Instance {
    explicit Instance(allocators::IAllocator& a) : families(a), group_faces(a), group_starts(a), ascii_face(a), ascii_glyph(a) {}
    String families;
    uint16_t weight = 400;
    FontStyle style = FontStyle::Normal;
    uint16_t stretch = 100;
    GenericFamily default_generic = GenericFamily::Serif;
    uint32_t generation = 0xFFFFFFFFu;  // of the face list the groups were built from
    List<uint32_t> group_faces;         // face ids, family group after family group
    List<uint32_t> group_starts;        // index into group_faces where each group begins (the last group ends at count())
    List<uint32_t> ascii_face;          // per-ASCII code point cache of the chosen face (kUnresolved when not known yet)
    List<uint32_t> ascii_glyph;
  };

  static constexpr uint32_t kUnresolved = 0xFFFFFFFDu;

  bool add_face(UniquePointer<Face>&& face);
  void add_bundled_family(ArrayView<const BundledFontInfo> faces);
  Instance* instance_at(uint32_t index);
  void rebuild_groups(Instance& instance);
  void append_group(Instance& instance, StringView name);
  void append_family_faces(Instance& instance, StringView family);
  TrueTypeFont* load(uint32_t face_id);
  bool covers(const Face& face, uint32_t codepoint) const;
  uint64_t rank(const Face& face, const Instance& instance) const;
  uint32_t pick(const Instance& instance, uint32_t begin, uint32_t end, uint32_t codepoint, uint32_t* glyph);
  uint32_t primary_face(Instance& instance);
  uint32_t resolve(Instance& instance, uint32_t codepoint, uint32_t* glyph);
  Fixed shape_into(FontHandle font, StringView utf8, const ShapeOptions& options, GlyphRun* run, bool* ok);
  Fixed advance_of(uint32_t face, uint32_t glyph, Fixed size);
  const GlyphBitmap* special_bitmap(const GlyphKey& key, uint32_t codepoint, Fixed size);

  allocators::IAllocator& allocator_;
  List<UniquePointer<Face>> faces_;
  List<UniquePointer<Instance>> instances_;
  Dictionary<uint64_t, uint32_t> instance_index_;
  GlyphCache cache_;
  Instance fallback_;  // plain sans-serif: the handle of a request that could not get an instance of its own
  uint32_t generation_ = 0;
  uint64_t web_font_bytes_ = 0;
  bool bundled_added_ = false;
};

}  // namespace font
