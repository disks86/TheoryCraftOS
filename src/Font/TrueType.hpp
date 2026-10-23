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
#include "Font/FontTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/String.hpp"

// A bounds-checked TrueType (glyf outline) font reader. Everything is read through ByteReader / ConstByteView,
// so truncated or corrupt data makes parse() or the individual lookups fail - it never reads outside the file.
//
// Supported: head, maxp, hhea, hmtx, cmap (formats 4 and 12), loca, glyf (simple and composite glyphs), kern
// (format 0 pair tables), OS/2 and name. CFF/OpenType-PostScript outlines, GPOS/GSUB and variations are not.
// The font file bytes are not copied: the view given to parse() must outlive the TrueTypeFont.
namespace font {

struct OutlinePoint {
  int32_t x = 0;  // font units, y up
  int32_t y = 0;
  bool on_curve = false;
};

// A glyph outline: TrueType quadratic contours. Contours are runs of points; `contour_ends[i]` is the index of the
// last point of contour i.
class Outline {
 public:
  explicit Outline(allocators::IAllocator& allocator) : points(allocator), contour_ends(allocator) {}
  void clear() {
    points.clear();
    contour_ends.clear();
  }
  List<OutlinePoint> points;
  List<uint32_t> contour_ends;
};

// Vertical metrics and a few design measurements in font units (divide by units_per_em for em).
struct FaceMetrics {
  uint32_t units_per_em = 1000;
  int32_t ascent = 0;       // above the baseline, positive
  int32_t descent = 0;      // below the baseline, positive
  int32_t line_gap = 0;     // extra leading between lines
  int32_t x_height = 0;     // 0 if the font does not say
  int32_t cap_height = 0;
  uint16_t weight = 400;    // OS/2 usWeightClass (400 if absent)
  uint16_t stretch = 100;   // OS/2 usWidthClass mapped to a percentage (50..200)
  bool italic = false;
  bool bold = false;
};

// Scaled metrics in pixels (Fixed), for a given font size. `ascent`/`descent` are not rounded: the layout rounds
// them as it sees fit (browsers round each to whole pixels).
struct ScaledMetrics {
  Fixed ascent = 0;
  Fixed descent = 0;
  Fixed line_gap = 0;
  Fixed x_height = 0;     // font x-height, or half the ascent as the CSS `ex` fallback
  Fixed cap_height = 0;
  Fixed zero_advance = 0; // advance of '0' (the CSS `ch` unit)
  Fixed line_spacing() const { return ascent + descent + line_gap; }
};

class TrueTypeFont {
 public:
  static constexpr uint32_t kMaxPoints = 20000;       // per glyph, composites included
  static constexpr uint32_t kMaxComponents = 128;     // components expanded per top-level glyph
  static constexpr uint32_t kMaxCompositeDepth = 6;   // nesting of composite glyphs

  explicit TrueTypeFont(allocators::IAllocator& allocator);
  TrueTypeFont(const TrueTypeFont&) = delete;
  TrueTypeFont& operator=(const TrueTypeFont&) = delete;

  // Reads the table directory and the tables needed to measure and draw. `face_index` selects a face of a
  // 'ttcf' collection. False if the data is not a usable TrueType font (truncated, corrupt, CFF outlines).
  bool parse(ConstByteView sfnt, uint32_t face_index = 0);
  bool valid() const { return valid_; }

  uint32_t units_per_em() const { return metrics_.units_per_em; }
  uint32_t glyph_count() const { return num_glyphs_; }
  const FaceMetrics& metrics() const { return metrics_; }
  ScaledMetrics scaled_metrics(Fixed size) const;

  // The glyph for a Unicode code point, or 0 (.notdef) if the font has none.
  uint32_t glyph_for_codepoint(uint32_t codepoint) const;
  bool has_codepoint(uint32_t codepoint) const { return glyph_for_codepoint(codepoint) != 0; }

  // Horizontal advance in font units (0 for a glyph past the end).
  uint32_t advance_units(uint32_t glyph) const;
  Fixed advance(uint32_t glyph, Fixed size) const { return scale_units(static_cast<int32_t>(advance_units(glyph)), size, metrics_.units_per_em); }
  // Pair kerning between two glyphs in font units (0 if none), from the 'kern' table.
  int32_t kerning_units(uint32_t left, uint32_t right) const;
  Fixed kerning(uint32_t left, uint32_t right, Fixed size) const { return scale_units(kerning_units(left, right), size, metrics_.units_per_em); }
  bool has_kerning() const { return kern_count_ != 0; }

  // Loads the outline of a glyph (composites expanded) in font units. An empty glyph (space) succeeds with no
  // points. False if the glyph does not exist or its data is corrupt; `out` is then cleared.
  bool load_outline(uint32_t glyph, Outline* out) const;

  // Names from the 'name' table (empty if absent).
  StringView family_name() const { return family_.view(); }
  StringView style_name() const { return style_.view(); }
  StringView full_name() const { return full_.view(); }
  StringView postscript_name() const { return postscript_.view(); }

 private:
  struct Table {
    uint32_t tag = 0;
    ConstByteView data;
  };
  static constexpr uint32_t kMaxTables = 64;
  static constexpr uint32_t kMaxKernTables = 8;

  bool find_table(uint32_t tag, ConstByteView* out) const;
  bool parse_cmap(ConstByteView cmap);
  bool parse_kern(ConstByteView kern);
  void parse_os2(ConstByteView os2);
  void parse_names(ConstByteView name);
  bool glyph_data(uint32_t glyph, ConstByteView* out) const;
  bool append_glyph(uint32_t glyph, uint32_t depth, uint32_t* budget, Outline* out) const;
  bool append_simple(ByteReader& r, int32_t contours, Outline* out) const;
  bool append_composite(ByteReader& r, uint32_t depth, uint32_t* budget, Outline* out) const;
  uint32_t lookup_format4(ConstByteView sub, uint32_t cp) const;
  uint32_t lookup_format12(ConstByteView sub, uint32_t cp) const;

  allocators::IAllocator& allocator_;
  bool valid_ = false;
  Table tables_[kMaxTables];
  uint32_t table_count_ = 0;
  ConstByteView hmtx_, loca_, glyf_, cmap4_, cmap12_;
  ConstByteView kern_[kMaxKernTables];  // the pair arrays (6 bytes each: left, right, value) of horizontal format 0 subtables
  uint32_t kern_count_ = 0;
  uint32_t num_glyphs_ = 0;
  uint32_t num_hmetrics_ = 0;
  bool long_loca_ = false;
  FaceMetrics metrics_;
  String family_, style_, full_, postscript_;
};

}  // namespace font
