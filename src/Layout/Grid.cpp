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

// Layout of grid containers (CSS Grid Layout Level 1): the track lists (repeat() with counts, auto-fill and auto-fit, minmax(), fit-content(),
// fr, named lines), grid-template-areas, line based placement (numbers, negative numbers, names, span) and sparse or dense auto-placement in row
// or column flow, track sizing as in section 11 of the specification (base sizes and growth limits, spanning items, flexible tracks, stretching
// of auto tracks), gaps, justify-content / align-content, and the alignment of the items in their grid areas (justify-self / align-self with
// stretch, start, end, center, baseline and auto margins). Absolutely positioned children are placed against the container as usual (the grid
// area of an absolutely positioned child is not used). Not supported: subgrid, masonry, span <name>, grid-template shorthand parsing here (the
// style sheet gives the longhands), vertical writing modes.
#include "Layout/LayoutInternal.hpp"

namespace layout {

namespace {

using browser::Length;
using browser::Overflow;
using browser::Place;
using browser::Style;

constexpr int32_t kMaxTracks = 1000;      // more tracks than this in an axis are not made (the specification allows 1000 lines too)
constexpr uint64_t kMaxItems = 2000;
constexpr int32_t kMaxRepeat = 400;
const LayoutUnit kInfinite = LayoutUnit::max();

// --- reading the style

struct TrackSize {
  enum Kind : uint8_t { Auto, Fixed, Flex, MinContent, MaxContent, FitContent };
  Kind kind = Auto;
  LayoutUnit length;    // Fixed: the size; FitContent: the limit
  int32_t flex = 0;     // Flex: the factor in hundredths
};

struct TrackDef {
  TrackSize min, max;
};

struct LineName {
  explicit LineName(allocators::IAllocator& a) : name(a) {}
  String name;
  uint32_t line = 0;   // the number of the line (0 is the first)
};

struct TrackList {
  explicit TrackList(allocators::IAllocator& a) : tracks(a), names(a) {}
  void clear() {
    tracks.clear();
    names.clear();
    auto_repeat = auto_fit = false;
    auto_first = auto_per = auto_count = 0;
  }
  List<TrackDef> tracks;
  List<LineName> names;
  bool auto_repeat = false, auto_fit = false;
  uint32_t auto_first = 0, auto_per = 0, auto_count = 0;   // the tracks of repeat(auto-fill|auto-fit): where they start, how many per repetition, how many repetitions
};

// What lengths in a track list are worked out against.
struct SizeEnv {
  const browser::StyleSheet* sheet = nullptr;
  const Style* style = nullptr;
  bool base_known = false;
  LayoutUnit base;
};

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// The whitespace separated parts of a value (brackets, parentheses and quotes keep their insides together).
bool next_word(StringView text, uint64_t* at, StringView* part) {
  uint64_t i = *at;
  while (i < text.size() && is_blank(text[i])) ++i;
  if (i >= text.size()) return false;
  const uint64_t start = i;
  int depth = 0;
  char quote = 0;
  while (i < text.size() && (depth > 0 || quote || !is_blank(text[i]))) {
    const char c = text[i];
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(' || c == '[') {
      ++depth;
    } else if ((c == ')' || c == ']') && depth > 0) {
      --depth;
    }
    ++i;
  }
  *part = text.substr(start, i - start);
  *at = i;
  return true;
}

bool parse_integer(StringView t, int32_t* out) {
  t = t.trimmed();
  if (t.empty()) return false;
  bool negative = false;
  uint64_t i = 0;
  if (t[0] == '-' || t[0] == '+') negative = t[i++] == '-';
  if (i >= t.size()) return false;
  int64_t v = 0;
  for (; i < t.size(); ++i) {
    if (t[i] < '0' || t[i] > '9') return false;
    v = v * 10 + (t[i] - '0');
    if (v > 100000) v = 100000;
  }
  *out = static_cast<int32_t>(negative ? -v : v);
  return true;
}

// A non-negative decimal number in hundredths ("1.5" is 150); false for anything else.
bool parse_decimal_x100(StringView t, int32_t* out) {
  if (t.empty()) return false;
  int64_t whole = 0, frac = 0, scale = 1;
  uint64_t i = 0;
  bool any = false;
  while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
    whole = whole * 10 + (t[i++] - '0');
    if (whole > 10000000) whole = 10000000;
    any = true;
  }
  if (i < t.size() && t[i] == '.') {
    ++i;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
      if (scale < 100) {
        frac = frac * 10 + (t[i] - '0');
        scale *= 10;
      }
      ++i;
      any = true;
    }
  }
  if (!any || i != t.size()) return false;
  *out = static_cast<int32_t>(whole * 100 + frac * 100 / scale);
  return true;
}

// One size of a track: a length, a percentage, "<n>fr", auto, min-content or max-content. A percentage of a size that is not known acts as auto.
bool parse_breadth(const SizeEnv& env, StringView v, TrackSize* out) {
  v = v.trimmed();
  *out = TrackSize();
  if (v.equals_ignore_case("auto")) return true;
  if (v.equals_ignore_case("min-content")) {
    out->kind = TrackSize::MinContent;
    return true;
  }
  if (v.equals_ignore_case("max-content")) {
    out->kind = TrackSize::MaxContent;
    return true;
  }
  if (v.size() > 2 && v.ends_with("fr")) {
    int32_t f;
    if (!parse_decimal_x100(v.first(v.size() - 2), &f)) return false;
    out->kind = TrackSize::Flex;
    out->flex = f;
    return true;
  }
  if (v.size() > 1 && v.ends_with('%')) {
    int32_t p;
    if (parse_decimal_x100(v.first(v.size() - 1), &p)) {
      if (!env.base_known) return true;
      out->kind = TrackSize::Fixed;
      out->length = max(env.base.percent(p), LayoutUnit());
      return true;
    }
  }
  if (!env.sheet || !env.style) return false;
  if (v.contains('%') && !env.base_known) return true;
  Length l;
  const int32_t base_px = env.base_known ? static_cast<int32_t>(env.base.to_px100() / 100) : 0;
  if (!env.sheet->resolve_length(v, *env.style, base_px, &l) || !l.is_px() || l.px100 < 0) return false;
  out->kind = TrackSize::Fixed;
  out->length = LayoutUnit::from_px100(l.px100);
  return true;
}

// The index of the first comma in `text` that is not inside parentheses.
uint64_t top_level_comma(StringView text) {
  int depth = 0;
  for (uint64_t i = 0; i < text.size(); ++i) {
    if (text[i] == '(') ++depth;
    else if (text[i] == ')') --depth;
    else if (text[i] == ',' && depth == 0) return i;
  }
  return StringView::npos;
}

// Reads a track list ("100px 1fr repeat(2, 50%) minmax(10px, auto) [name] auto"). `auto_reps` is how many times repeat(auto-fill|auto-fit, ...) repeats.
// False if the list cannot be read (it then counts as none).
bool parse_track_list(allocators::IAllocator& a, const SizeEnv& env, StringView text, uint32_t auto_reps, TrackList* out, uint32_t depth, bool in_auto) {
  uint64_t at = 0;
  StringView part;
  uint32_t guard = 0;
  while (next_word(text, &at, &part)) {
    if (++guard > 2000 || out->tracks.count() > static_cast<uint64_t>(kMaxTracks)) return false;
    if (part.starts_with('[')) {
      StringView inner = part.substr(1);
      if (inner.ends_with(']')) inner = inner.without_suffix(1);
      uint64_t k = 0;
      StringView word;
      while (next_word(inner, &k, &word)) {
        LineName line(a);
        if (!line.name.assign(word)) return false;
        line.line = static_cast<uint32_t>(out->tracks.count());
        if (!out->names.push_back(util::move(line))) return false;
      }
      continue;
    }
    TrackDef def;
    const uint64_t open = part.find('(');
    if (open != StringView::npos) {
      if (!part.ends_with(')')) return false;
      const StringView function = part.first(open).trimmed();
      const StringView inner = part.substr(open + 1, part.size() - open - 2);
      if (function.equals_ignore_case("repeat")) {
        const uint64_t comma = top_level_comma(inner);
        if (comma == StringView::npos || depth > 2) return false;
        const StringView count_text = inner.first(comma).trimmed(), body = inner.substr(comma + 1);
        if (count_text.equals_ignore_case("auto-fill") || count_text.equals_ignore_case("auto-fit")) {
          if (in_auto || out->auto_repeat) return false;
          out->auto_repeat = true;
          out->auto_fit = count_text.equals_ignore_case("auto-fit");
          out->auto_first = static_cast<uint32_t>(out->tracks.count());
          const uint32_t reps = auto_reps < 1 ? 1 : auto_reps;
          for (uint32_t r = 0; r < reps; ++r) {
            if (!parse_track_list(a, env, body, 1, out, depth + 1, true)) return false;
            if (r == 0) out->auto_per = static_cast<uint32_t>(out->tracks.count()) - out->auto_first;
            if (out->auto_per == 0) return false;
          }
          out->auto_count = reps;
          continue;
        }
        int32_t count;
        if (!parse_integer(count_text, &count) || count < 1) return false;
        if (count > kMaxRepeat) count = kMaxRepeat;
        for (int32_t r = 0; r < count; ++r)
          if (!parse_track_list(a, env, body, 1, out, depth + 1, in_auto)) return false;
        continue;
      }
      if (function.equals_ignore_case("minmax")) {
        const uint64_t comma = top_level_comma(inner);
        if (comma == StringView::npos) return false;
        if (!parse_breadth(env, inner.first(comma), &def.min) || !parse_breadth(env, inner.substr(comma + 1), &def.max)) return false;
        if (def.min.kind == TrackSize::Flex) def.min = TrackSize();
        if (!out->tracks.push_back(def)) return false;
        continue;
      }
      if (function.equals_ignore_case("fit-content")) {
        TrackSize limit;
        if (!parse_breadth(env, inner, &limit) || (limit.kind != TrackSize::Fixed && limit.kind != TrackSize::Auto)) return false;
        def.max.kind = limit.kind == TrackSize::Fixed ? TrackSize::FitContent : TrackSize::Auto;
        def.max.length = limit.length;
        if (!out->tracks.push_back(def)) return false;
        continue;
      }
      // calc(), min(), max(), clamp(), var() ...: a length
      TrackSize size;
      if (!parse_breadth(env, part, &size)) return false;
      def.min = def.max = size;
      if (!out->tracks.push_back(def)) return false;
      continue;
    }
    TrackSize size;
    if (!parse_breadth(env, part, &size)) return false;
    if (size.kind == TrackSize::Flex) {
      def.max = size;   // (the minimum is auto)
    } else {
      def.min = def.max = size;
    }
    if (!out->tracks.push_back(def)) return false;
  }
  return true;
}

// grid-template-areas: "a a b" "a a c" -> the name at each cell ("" for none).
struct AreaGrid {
  explicit AreaGrid(allocators::IAllocator& a) : cells(a) {}
  List<String> cells;
  uint32_t rows = 0, columns = 0;
  // The rows and columns the area `name` covers ([r0, r1) x [c0, c1)); false if no cell has that name.
  bool find(StringView name, uint32_t* r0, uint32_t* c0, uint32_t* r1, uint32_t* c1) const {
    if (name.empty()) return false;
    bool found = false;
    for (uint32_t r = 0; r < rows; ++r) {
      for (uint32_t c = 0; c < columns; ++c) {
        if (!cells[static_cast<uint64_t>(r) * columns + c].view().equals(name)) continue;
        if (!found) {
          *r0 = r;
          *c0 = c;
          *r1 = r + 1;
          *c1 = c + 1;
          found = true;
        } else {
          if (r < *r0) *r0 = r;
          if (c < *c0) *c0 = c;
          if (r + 1 > *r1) *r1 = r + 1;
          if (c + 1 > *c1) *c1 = c + 1;
        }
      }
    }
    return found;
  }
};

bool parse_areas(allocators::IAllocator& a, StringView text, AreaGrid* out) {
  // pass 0 finds the widest row, pass 1 fills the cells (short rows are padded with empty names)
  uint32_t columns = 0, rows = 0;
  for (int pass = 0; pass < 2; ++pass) {
    uint64_t i = 0;
    rows = 0;
    while (i < text.size()) {
      while (i < text.size() && text[i] != '"' && text[i] != '\'') ++i;
      if (i >= text.size()) break;
      const char q = text[i++];
      const uint64_t start = i;
      while (i < text.size() && text[i] != q) ++i;
      const StringView row = text.substr(start, i - start);
      if (i < text.size()) ++i;
      if (++rows > static_cast<uint32_t>(kMaxTracks)) return false;
      uint64_t k = 0;
      uint32_t n = 0;
      StringView word;
      while (next_word(row, &k, &word)) {
        ++n;
        if (pass == 1) {
          String name(a);
          if (!name.assign(word.starts_with('.') ? StringView() : word) || !out->cells.push_back(util::move(name))) return false;
        }
      }
      if (pass == 0) {
        if (n > columns) columns = n;
      } else {
        for (; n < columns; ++n) {
          String name(a);
          if (!out->cells.push_back(util::move(name))) return false;
        }
      }
    }
    if (pass == 0 && (rows == 0 || columns == 0 || columns > static_cast<uint32_t>(kMaxTracks))) return false;
  }
  out->rows = rows;
  out->columns = columns;
  return true;
}

// A grid line as an item names it: auto, 3, -1, span 2, area-name, name 2.
struct LineSpec {
  explicit LineSpec(allocators::IAllocator& a) : name(a) {}
  bool span = false, has_int = false;
  int32_t n = 1;
  String name;
  bool is_auto() const { return !span && !has_int && name.empty(); }
};

void parse_line_spec(StringView text, LineSpec* out) {
  uint64_t at = 0;
  StringView part;
  while (next_word(text, &at, &part)) {
    int32_t v;
    if (part.equals_ignore_case("span")) out->span = true;
    else if (part.equals_ignore_case("auto")) continue;
    else if (parse_integer(part, &v)) {
      out->has_int = true;
      out->n = v;
    } else out->name.assign(part);
  }
  if (out->span) {
    if (!out->has_int || out->n < 1) out->n = 1;
    if (out->n > kMaxTracks) out->n = kMaxTracks;
  } else if (out->has_int && out->n == 0) {
    out->has_int = false;   // (line 0 is invalid: the placement is auto)
    out->n = 1;
  }
}

// --- the grid

enum class Mode : uint8_t { Definite, MaxContent, MinContent };

struct Track {
  TrackDef def;
  LayoutUnit base, limit, planned, gap_after, pos;
  bool collapsed = false;
  bool inflexible = false;   // scratch: not flexible while a flex fraction is worked out
  bool used = false;         // some item sits in it
};

struct GridItem {
  BoxId box = kNoBox;
  bool definite[2] = {false, false};
  int32_t pos[2] = {0, 0};     // the line it starts at (axis 0 columns, 1 rows)
  int32_t span[2] = {1, 1};
  // The sizes its margin box contributes along each axis: the smallest it needs (automatic minimum), its min-content and its max-content size.
  LayoutUnit contrib[2][3];
  LayoutUnit shim;             // baseline alignment: how far down it is moved so that its baseline meets the others'
  LayoutUnit ascent;           // margin top + distance to the baseline
  bool baseline = false;       // aligned by baseline in its row
};

// Which cells of the grid are taken (the cells beyond what is stored are free).
struct Occupancy {
  explicit Occupancy(allocators::IAllocator& a) : allocator(a), cells(a) {}
  allocators::IAllocator& allocator;
  List<uint8_t> cells;
  uint32_t majors = 0, minors = 0;
  bool reset(uint32_t new_majors, uint32_t new_minors) {
    cells.clear();
    majors = minors = 0;
    return resize(new_majors, new_minors);
  }
  bool resize(uint32_t new_majors, uint32_t new_minors) {
    if (new_majors > static_cast<uint32_t>(kMaxTracks)) new_majors = kMaxTracks;
    if (new_minors > static_cast<uint32_t>(kMaxTracks)) new_minors = kMaxTracks;
    if (new_majors < majors) new_majors = majors;
    if (new_minors < minors) new_minors = minors;
    if (new_minors == minors) {
      for (uint64_t i = static_cast<uint64_t>(majors) * minors; i < static_cast<uint64_t>(new_majors) * new_minors; ++i)
        if (!cells.push_back(0)) return false;
      majors = new_majors;
      return true;
    }
    List<uint8_t> fresh(allocator);
    for (uint32_t m = 0; m < new_majors; ++m) {
      for (uint32_t n = 0; n < new_minors; ++n)
        if (!fresh.push_back(m < majors && n < minors ? cells[static_cast<uint64_t>(m) * minors + n] : 0)) return false;
    }
    cells.clear();
    for (uint64_t i = 0; i < fresh.count(); ++i)
      if (!cells.push_back(fresh[i])) return false;
    majors = new_majors;
    minors = new_minors;
    return true;
  }
  bool overlaps(int32_t major, int32_t major_span, int32_t minor, int32_t minor_span) const {
    for (int32_t m = major; m < major + major_span && m < static_cast<int32_t>(majors); ++m)
      for (int32_t n = minor; n < minor + minor_span && n < static_cast<int32_t>(minors); ++n)
        if (cells[static_cast<uint64_t>(m) * minors + static_cast<uint64_t>(n)]) return true;
    return false;
  }
  bool mark(int32_t major, int32_t major_span, int32_t minor, int32_t minor_span) {
    if (major < 0 || minor < 0) return true;
    const uint32_t need_majors = static_cast<uint32_t>(major + major_span), need_minors = static_cast<uint32_t>(minor + minor_span);
    if ((need_majors > majors || need_minors > minors) && !resize(need_majors, need_minors)) return false;
    for (int32_t m = major; m < major + major_span && m < static_cast<int32_t>(majors); ++m)
      for (int32_t n = minor; n < minor + minor_span && n < static_cast<int32_t>(minors); ++n) cells[static_cast<uint64_t>(m) * minors + static_cast<uint64_t>(n)] = 1;
    return true;
  }
};

enum class Align : uint8_t { Start, End, Center, Stretch, Baseline };

Align to_align(Place p, bool normal_stretches) {
  switch (p) {
    case Place::End: case Place::FlexEnd: case Place::Right: return Align::End;
    case Place::Center: return Align::Center;
    case Place::Stretch: return Align::Stretch;
    case Place::Baseline: return Align::Baseline;
    case Place::Normal: return normal_stretches ? Align::Stretch : Align::Start;
    default: return Align::Start;
  }
}

// Where the tracks start and how much room is added between them for a justify-content / align-content value, given the room left over.
void content_offsets(Place p, LayoutUnit free, uint64_t count, LayoutUnit* start, LayoutUnit* between) {
  *start = LayoutUnit();
  *between = LayoutUnit();
  switch (p) {
    case Place::End: case Place::FlexEnd: case Place::Right: *start = free; break;
    case Place::Center: *start = free / 2; break;
    case Place::SpaceBetween:
      if (free > LayoutUnit() && count > 1) *between = free / static_cast<int32_t>(count - 1);
      break;
    case Place::SpaceAround:
      if (free > LayoutUnit() && count > 0) {
        *between = free / static_cast<int32_t>(count);
        *start = *between / 2;
      } else {
        *start = free / 2;
      }
      break;
    case Place::SpaceEvenly:
      if (free > LayoutUnit() && count > 0) {
        *between = free / static_cast<int32_t>(count + 1);
        *start = *between;
      } else {
        *start = free / 2;
      }
      break;
    default: break;
  }
}

struct AxisData {
  explicit AxisData(allocators::IAllocator& a) : tmpl(a), auto_list(a), tracks(a) {}
  TrackList tmpl;        // the explicit tracks
  TrackList auto_list;   // grid-auto-columns / grid-auto-rows
  List<Track> tracks;
  int32_t explicit_count = 0;   // tracks of the explicit grid (the template, or what grid-template-areas needs)
  int32_t lead = 0;             // implicit tracks before the explicit ones (items placed on negative lines)
  LayoutUnit gap;
  LayoutUnit extent;            // the size of all the tracks and gaps
};

}  // namespace

struct GridLayout {
  GridLayout(LayoutContext& ctx, BoxId id)
      : ctx_(ctx), tree_(ctx.tree_), id_(id), a_(ctx.tree_.allocator()), x_(a_), y_(a_), areas_(a_), items_(a_), occ_(a_), scratch_idx_(a_), scratch_room_(a_), scratch_out_(a_) {}

  AxisData& ax(int axis) { return axis == 0 ? x_ : y_; }
  Box& box() { return tree_.box(id_); }

  // ---------------------------------------------------------------------------------------------------------------------------------------
  // The items and the templates

  bool collect_items(bool register_absolutes) {
    const uint64_t count = tree_.box(id_).children.count();
    for (uint64_t i = 0; i < count; ++i) {
      const BoxId cid = tree_.box(id_).children[i];
      const Box& c = tree_.box(cid);
      if (c.kind == BoxKind::Marker) continue;
      if (c.out_of_flow) {
        if (register_absolutes) ctx_.register_absolute(cid, id_, box().content_left(), box().content_top());
        continue;
      }
      if (items_.count() >= kMaxItems) break;
      GridItem item;
      item.box = cid;
      if (!items_.push_back(item)) return false;
    }
    // order-modified document order: a stable sort by `order`
    for (uint64_t i = 1; i < items_.count(); ++i) {
      const GridItem moving = items_[i];
      const int32_t key = tree_.box(moving.box).style.order;
      uint64_t j = i;
      while (j > 0 && tree_.box(items_[j - 1].box).style.order > key) {
        items_[j] = items_[j - 1];
        --j;
      }
      items_[j] = moving;
    }
    return true;
  }

  LayoutUnit gap_of(const Length& l, bool known, LayoutUnit base) {
    LayoutUnit v;
    if (ctx_.resolve_with(l, box().style, base, known, &v)) return max(v, LayoutUnit());
    return LayoutUnit();
  }

  // The number of repetitions of repeat(auto-fill|auto-fit) that fit in `size` (at least one).
  uint32_t auto_repetitions(const TrackList& probe, bool known, LayoutUnit size, LayoutUnit gap) {
    if (!known || probe.auto_per == 0) return 1;
    auto fixed = [](const TrackDef& d) {
      return d.max.kind == TrackSize::Fixed ? d.max.length : d.min.kind == TrackSize::Fixed ? d.min.length : LayoutUnit();
    };
    int64_t others = 0, unit = 0;
    int64_t other_count = 0;
    for (uint64_t i = 0; i < probe.tracks.count(); ++i) {
      const bool repeated = i >= probe.auto_first && i < static_cast<uint64_t>(probe.auto_first) + probe.auto_per;
      if (repeated) unit += fixed(probe.tracks[i]).raw();
      else {
        others += fixed(probe.tracks[i]).raw();
        ++other_count;
      }
    }
    const int64_t denominator = unit + static_cast<int64_t>(probe.auto_per) * gap.raw();
    if (denominator <= 0) return 1;
    const int64_t numerator = static_cast<int64_t>(size.raw()) - others - (other_count - 1) * gap.raw();
    if (numerator <= 0) return 1;
    int64_t reps = numerator / denominator;
    if (reps < 1) reps = 1;
    const int64_t cap = kMaxTracks / probe.auto_per;
    return static_cast<uint32_t>(reps > cap ? cap : reps);
  }

  bool read_templates(bool width_known, LayoutUnit width, bool height_known, LayoutUnit height) {
    const Style& s = box().style;
    const browser::StyleSheet* sheet = ctx_.options_.sheet;
    column_flow_ = s.grid_column_flow;
    dense_ = s.grid_dense;
    x_.gap = gap_of(s.column_gap, width_known, width);
    y_.gap = gap_of(s.row_gap, height_known, height);
    if (!sheet) return true;
    for (int axis = 0; axis < 2; ++axis) {
      AxisData& d = ax(axis);
      SizeEnv env;
      env.sheet = sheet;
      env.style = &s;
      env.base_known = axis == 0 ? width_known : height_known;
      env.base = axis == 0 ? width : height;
      const StringView text = sheet->text(axis == 0 ? s.grid_template_columns : s.grid_template_rows);
      if (!text.empty()) {
        bool ok = parse_track_list(a_, env, text, 1, &d.tmpl, 0, false);
        if (ok && d.tmpl.auto_repeat) {
          const uint32_t reps = auto_repetitions(d.tmpl, env.base_known, env.base, d.gap);
          if (reps != 1) {
            d.tmpl.clear();
            ok = parse_track_list(a_, env, text, reps, &d.tmpl, 0, false);
          }
        }
        if (!ok) d.tmpl.clear();
      }
      const StringView auto_text = sheet->text(axis == 0 ? s.grid_auto_columns : s.grid_auto_rows);
      if (!auto_text.empty() && !parse_track_list(a_, env, auto_text, 1, &d.auto_list, 0, true)) d.auto_list.clear();
      d.explicit_count = static_cast<int32_t>(d.tmpl.tracks.count());
    }
    const StringView area_text = sheet->text(s.grid_template_areas);
    if (!area_text.empty()) {
      have_areas_ = parse_areas(a_, area_text, &areas_);
      if (have_areas_) {
        if (static_cast<int32_t>(areas_.columns) > x_.explicit_count) x_.explicit_count = static_cast<int32_t>(areas_.columns);
        if (static_cast<int32_t>(areas_.rows) > y_.explicit_count) y_.explicit_count = static_cast<int32_t>(areas_.rows);
      }
    }
    return true;
  }

  // ---------------------------------------------------------------------------------------------------------------------------------------
  // Placement (section 8)

  // The line (0 is the first line of the explicit grid; it can be negative) a line value names.
  int32_t line_of(const LineSpec& spec, bool is_end, int axis) {
    AxisData& d = ax(axis);
    const int32_t explicit_lines = d.explicit_count;
    if (spec.has_int && spec.name.empty()) {
      const int32_t n = spec.n;
      return n > 0 ? n - 1 : explicit_lines + 1 + n;
    }
    const int32_t nth = spec.has_int ? spec.n : 1;
    const List<LineName>& names = d.tmpl.names;
    int32_t found = 0;
    if (nth > 0) {
      for (uint64_t i = 0; i < names.count(); ++i) {
        if (!names[i].name.view().equals(spec.name.view())) continue;
        if (++found == nth) return static_cast<int32_t>(names[i].line);
      }
    } else {
      for (uint64_t i = names.count(); i > 0; --i) {
        if (!names[i - 1].name.view().equals(spec.name.view())) continue;
        if (++found == -nth) return static_cast<int32_t>(names[i - 1].line);
      }
    }
    if (!spec.has_int && have_areas_) {
      uint32_t r0, c0, r1, c1;
      StringView name = spec.name.view();
      bool end = is_end;
      if (areas_.find(name, &r0, &c0, &r1, &c1)) {
        return axis == 0 ? static_cast<int32_t>(end ? c1 : c0) : static_cast<int32_t>(end ? r1 : r0);
      }
      if (name.size() > 6 && name.ends_with("-start")) {
        end = false;
        name = name.first(name.size() - 6);
      } else if (name.size() > 4 && name.ends_with("-end")) {
        end = true;
        name = name.first(name.size() - 4);
      } else {
        name = StringView();
      }
      if (areas_.find(name, &r0, &c0, &r1, &c1)) return axis == 0 ? static_cast<int32_t>(end ? c1 : c0) : static_cast<int32_t>(end ? r1 : r0);
    }
    // not enough lines of that name: the implicit lines are taken to have it
    return nth > 0 ? explicit_lines + (nth - found) : -(-nth - found);
  }

  void resolve_axis(const LineSpec& start_in, const LineSpec& end_in, int axis, GridItem* item) {
    LineSpec start(a_), end(a_);
    start.span = start_in.span; start.has_int = start_in.has_int; start.n = start_in.n; start.name.assign(start_in.name.view());
    end.span = end_in.span; end.has_int = end_in.has_int; end.n = end_in.n; end.name.assign(end_in.name.view());
    if (start.span && end.span) {   // two spans: the end one is dropped
      end.span = end.has_int = false;
      end.n = 1;
      end.name.clear();
    }
    const bool has_start = !start.span && !start.is_auto(), has_end = !end.span && !end.is_auto();
    int32_t s = 0, e = 0;
    if (has_start) s = clamp_line(line_of(start, false, axis));
    if (has_end) e = clamp_line(line_of(end, true, axis));
    int32_t span = 1;
    if (has_start && has_end) {
      if (e < s) {
        const int32_t t = s;
        s = e;
        e = t;
      }
      span = e - s < 1 ? 1 : e - s;
      item->definite[axis] = true;
      item->pos[axis] = s;
    } else if (has_start) {
      span = end.span ? end.n : 1;
      item->definite[axis] = true;
      item->pos[axis] = s;
    } else if (has_end) {
      span = start.span ? start.n : 1;
      item->definite[axis] = true;
      item->pos[axis] = e - span;
    } else {
      span = start.span ? start.n : end.span ? end.n : 1;
    }
    item->span[axis] = span > kMaxTracks ? kMaxTracks : span;
  }

  static int32_t clamp_line(int32_t v) { return v < -kMaxTracks ? -kMaxTracks : v > kMaxTracks ? kMaxTracks : v; }

  bool place_items() {
    const browser::StyleSheet* sheet = ctx_.options_.sheet;
    for (uint64_t i = 0; i < items_.count(); ++i) {
      GridItem& it = items_[i];
      const Style& s = tree_.box(it.box).style;
      LineSpec cs(a_), ce(a_), rs(a_), re(a_);
      if (sheet) {
        parse_line_spec(sheet->text(s.grid_column_start), &cs);
        parse_line_spec(sheet->text(s.grid_column_end), &ce);
        parse_line_spec(sheet->text(s.grid_row_start), &rs);
        parse_line_spec(sheet->text(s.grid_row_end), &re);
      }
      resolve_axis(cs, ce, 0, &it);
      resolve_axis(rs, re, 1, &it);
    }
    // implicit tracks before the explicit grid (negative lines)
    for (int axis = 0; axis < 2; ++axis) {
      int32_t lowest = 0;
      for (uint64_t i = 0; i < items_.count(); ++i)
        if (items_[i].definite[axis] && items_[i].pos[axis] < lowest) lowest = items_[i].pos[axis];
      ax(axis).lead = -lowest;
      for (uint64_t i = 0; i < items_.count(); ++i)
        if (items_[i].definite[axis]) items_[i].pos[axis] += ax(axis).lead;
    }
    const int major = column_flow_ ? 0 : 1, minor = 1 - major;
    int32_t minors = ax(minor).lead + ax(minor).explicit_count, majors = ax(major).lead + ax(major).explicit_count;
    for (uint64_t i = 0; i < items_.count(); ++i) {
      const GridItem& it = items_[i];
      minors = max_i(minors, it.definite[minor] ? it.pos[minor] + it.span[minor] : it.span[minor]);
      if (it.definite[major]) majors = max_i(majors, it.pos[major] + it.span[major]);
    }
    if (minors < 1) minors = 1;
    if (minors > kMaxTracks) minors = kMaxTracks;
    if (!occ_.reset(static_cast<uint32_t>(majors), static_cast<uint32_t>(minors))) return false;
    // 1: items with a definite place in both axes
    for (uint64_t i = 0; i < items_.count(); ++i) {
      const GridItem& it = items_[i];
      if (it.definite[0] && it.definite[1] && !occ_.mark(it.pos[major], it.span[major], it.pos[minor], it.span[minor])) return false;
    }
    // 2: items with a definite place along the major axis only
    List<int32_t> row_cursor(a_);
    for (uint64_t i = 0; i < items_.count(); ++i) {
      GridItem& it = items_[i];
      if (!it.definite[major] || it.definite[minor]) continue;
      const int32_t maj = it.pos[major];
      while (static_cast<int32_t>(row_cursor.count()) <= maj && row_cursor.count() < static_cast<uint64_t>(kMaxTracks) + 1)
        if (!row_cursor.push_back(0)) return false;
      int32_t m = dense_ || maj >= static_cast<int32_t>(row_cursor.count()) ? 0 : row_cursor[static_cast<uint64_t>(maj)];
      while (m + it.span[minor] <= minors && occ_.overlaps(maj, it.span[major], m, it.span[minor])) ++m;
      if (m + it.span[minor] > kMaxTracks) m = kMaxTracks - it.span[minor];
      it.pos[minor] = m;
      it.definite[minor] = true;
      if (m + it.span[minor] > minors) {
        minors = m + it.span[minor];
        if (!occ_.resize(occ_.majors, static_cast<uint32_t>(minors))) return false;
      }
      if (!dense_ && maj < static_cast<int32_t>(row_cursor.count())) row_cursor[static_cast<uint64_t>(maj)] = m + it.span[minor];
      if (!occ_.mark(maj, it.span[major], m, it.span[minor])) return false;
    }
    // 4: everything else, with the auto-placement cursor
    int32_t cursor_major = 0, cursor_minor = 0;
    for (uint64_t i = 0; i < items_.count(); ++i) {
      GridItem& it = items_[i];
      if (it.definite[major]) continue;
      if (it.definite[minor]) {
        if (dense_) cursor_major = 0;
        else if (it.pos[minor] < cursor_minor) ++cursor_major;
        cursor_minor = it.pos[minor];
        while (cursor_major < kMaxTracks && occ_.overlaps(cursor_major, it.span[major], it.pos[minor], it.span[minor])) ++cursor_major;
        it.pos[major] = cursor_major;
      } else {
        if (dense_) cursor_major = cursor_minor = 0;
        for (;;) {
          bool placed = false;
          for (int32_t m = cursor_minor; m + it.span[minor] <= minors; ++m) {
            if (occ_.overlaps(cursor_major, it.span[major], m, it.span[minor])) continue;
            it.pos[minor] = m;
            cursor_minor = m;
            placed = true;
            break;
          }
          if (placed) break;
          ++cursor_major;
          cursor_minor = 0;
          if (cursor_major >= kMaxTracks) {
            it.pos[minor] = 0;
            break;
          }
        }
        it.pos[major] = cursor_major;
      }
      it.definite[major] = it.definite[minor] = true;
      if (!occ_.mark(it.pos[major], it.span[major], it.pos[minor], it.span[minor])) return false;
    }
    // the tracks: the explicit ones, and the implicit ones the items need
    for (int axis = 0; axis < 2; ++axis) {
      AxisData& d = ax(axis);
      int32_t count = d.lead + d.explicit_count;
      for (uint64_t i = 0; i < items_.count(); ++i) count = max_i(count, items_[i].pos[axis] + items_[i].span[axis]);
      if (count > kMaxTracks) count = kMaxTracks;
      if (count < 1) count = 1;
      for (uint64_t i = 0; i < items_.count(); ++i) {   // (an item cannot reach past the last track)
        GridItem& it = items_[i];
        if (it.pos[axis] < 0) it.pos[axis] = 0;
        if (it.pos[axis] >= count) it.pos[axis] = count - 1;
        if (it.span[axis] > count - it.pos[axis]) it.span[axis] = count - it.pos[axis];
      }
      if (!build_tracks(axis, count)) return false;
    }
    return true;
  }

  static int32_t max_i(int32_t a, int32_t b) { return a > b ? a : b; }

  bool build_tracks(int axis, int32_t count) {
    AxisData& d = ax(axis);
    d.tracks.clear();
    const int64_t auto_n = static_cast<int64_t>(d.auto_list.tracks.count());
    for (int32_t i = 0; i < count; ++i) {
      Track t;
      if (i >= d.lead && i < d.lead + static_cast<int32_t>(d.tmpl.tracks.count())) {
        t.def = d.tmpl.tracks[static_cast<uint64_t>(i - d.lead)];
      } else if (auto_n > 0) {
        int64_t index;
        if (i < d.lead) index = auto_n - 1 - ((d.lead - 1 - i) % auto_n);
        else index = (i - (d.lead + d.explicit_count)) % auto_n;
        if (index < 0) index += auto_n;
        t.def = d.auto_list.tracks[static_cast<uint64_t>(index)];
      }
      if (!d.tracks.push_back(t)) return false;
    }
    for (uint64_t i = 0; i < items_.count(); ++i) {
      const GridItem& it = items_[i];
      for (int32_t k = it.pos[axis]; k < it.pos[axis] + it.span[axis] && k < count; ++k)
        if (k >= 0) d.tracks[static_cast<uint64_t>(k)].used = true;
    }
    // auto-fit: the repeated tracks nothing sits in collapse
    if (d.tmpl.auto_fit) {
      const int32_t first = d.lead + static_cast<int32_t>(d.tmpl.auto_first), last = first + static_cast<int32_t>(d.tmpl.auto_per * d.tmpl.auto_count);
      for (int32_t i = first; i < last && i < count; ++i)
        if (!d.tracks[static_cast<uint64_t>(i)].used) d.tracks[static_cast<uint64_t>(i)].collapsed = true;
    }
    return true;
  }

  // ---------------------------------------------------------------------------------------------------------------------------------------
  // The sizes items contribute

  // Along the columns: from the intrinsic widths of the item and its width, min-width and max-width.
  void column_contributions(GridItem& it, bool width_known, LayoutUnit width) {
    Box& b = tree_.box(it.box);
    const Style& s = b.style;
    LayoutUnit mn, mx;
    ctx_.intrinsic_widths(it.box, &mn, &mx);
    ctx_.resolve_edges(b, width, width_known);   // (after intrinsic_widths, which leaves the margins and paddings worked out against nothing)
    const LayoutUnit bp = b.border.horizontal() + b.padding.horizontal();
    const LayoutUnit extra = bp + b.margin.horizontal();
    LayoutUnit w;
    const bool definite = !s.width.has_percent() && ctx_.resolve_in(s.width, b, false, &w);
    if (definite) w = s.sizing == browser::BoxSizing::BorderBox ? max(w - bp, LayoutUnit()) : w;
    LayoutUnit lo = mn, hi = mx;
    if (definite) {
      lo = hi = w;
    } else if (s.width.kind == Length::MinContent) {
      hi = mn;
    } else if (s.width.kind == Length::MaxContent) {
      lo = mx;
    }
    lo = ctx_.clamp_content_width(b, lo);
    hi = ctx_.clamp_content_width(b, hi);
    LayoutUnit minimum;
    LayoutUnit specified_min;
    if (!s.min_width.has_percent() && ctx_.resolve_in(s.min_width, b, false, &specified_min)) {
      minimum = max(s.sizing == browser::BoxSizing::BorderBox ? specified_min - bp : specified_min, LayoutUnit());
    } else if (s.overflow_x != Overflow::Visible) {
      minimum = LayoutUnit();
    } else {
      minimum = definite ? min(mn, w) : mn;
      minimum = ctx_.clamp_content_width(b, minimum);
      minimum = min(minimum, hi);
    }
    it.contrib[0][0] = minimum + extra;
    it.contrib[0][1] = lo + extra;
    it.contrib[0][2] = hi + extra;
  }

  // ---------------------------------------------------------------------------------------------------------------------------------------
  // Track sizing (section 11)

  LayoutUnit limit_or_base(const Track& t) const { return t.limit.is_max() ? t.base : t.limit; }

  void compute_gaps(int axis) {
    AxisData& d = ax(axis);
    int64_t last = -1;
    for (uint64_t i = 0; i < d.tracks.count(); ++i)
      if (!d.tracks[i].collapsed) last = static_cast<int64_t>(i);
    for (uint64_t i = 0; i < d.tracks.count(); ++i) d.tracks[i].gap_after = (!d.tracks[i].collapsed && static_cast<int64_t>(i) < last) ? d.gap : LayoutUnit();
  }

  // The sum of the sizes of tracks [first, first + count) (their bases, or their growth limits) and the gaps between them.
  LayoutUnit span_sum(int axis, int32_t first, int32_t count, bool limits) {
    AxisData& d = ax(axis);
    LayoutUnit sum;
    for (int32_t i = first; i < first + count; ++i) {
      const Track& t = d.tracks[static_cast<uint64_t>(i)];
      sum += limits ? limit_or_base(t) : t.base;
      if (i + 1 < first + count) sum += t.gap_after;
    }
    return sum;
  }

  // Shares `space` equally among the tracks in scratch_idx_, each taking at most scratch_room_ (if `capped`); scratch_out_ gets the shares.
  // What the limits leave over goes on equally beyond them.
  bool share_equally(LayoutUnit space, bool capped) {
    const uint64_t n = scratch_idx_.count();
    scratch_out_.clear();
    for (uint64_t i = 0; i < n; ++i)
      if (!scratch_out_.push_back(LayoutUnit())) return false;
    if (n == 0 || space <= LayoutUnit()) return true;
    LayoutUnit remaining = space;
    if (capped) {
      List<uint8_t> frozen(a_);
      for (uint64_t i = 0; i < n; ++i)
        if (!frozen.push_back(scratch_room_[i] <= LayoutUnit() ? 1 : 0)) return false;
      for (uint64_t round = 0; round <= n && remaining > LayoutUnit(); ++round) {
        uint64_t active = 0;
        for (uint64_t i = 0; i < n; ++i) active += frozen[i] ? 0 : 1;
        if (active == 0) break;
        const LayoutUnit share = remaining / static_cast<int32_t>(active);
        if (share <= LayoutUnit()) {   // (what is left is less than a 64th of a pixel for each)
          for (uint64_t i = 0; i < n && remaining > LayoutUnit(); ++i) {
            if (frozen[i]) continue;
            const LayoutUnit give = min(LayoutUnit::from_raw(1), remaining);
            scratch_out_[i] += give;
            remaining -= give;
          }
          break;
        }
        for (uint64_t i = 0; i < n; ++i) {
          if (frozen[i]) continue;
          const LayoutUnit room = scratch_room_[i] - scratch_out_[i];
          const LayoutUnit give = min(share, room);
          scratch_out_[i] += give;
          remaining -= give;
          if (scratch_out_[i] >= scratch_room_[i]) frozen[i] = 1;
        }
      }
      if (remaining <= LayoutUnit()) return true;
    }
    const LayoutUnit each = remaining / static_cast<int32_t>(n);
    LayoutUnit dust = remaining - each * static_cast<int32_t>(n);
    for (uint64_t i = 0; i < n; ++i) {
      scratch_out_[i] += each;
      if (dust > LayoutUnit()) {
        const LayoutUnit give = min(LayoutUnit::from_raw(1), dust);
        scratch_out_[i] += give;
        dust -= give;
      }
    }
    return true;
  }

  // Does the track's sizing function take part in a step of "increase sizes to accommodate spanning items"?
  static bool min_is_intrinsic(const TrackSize& t) { return t.kind == TrackSize::Auto || t.kind == TrackSize::MinContent || t.kind == TrackSize::MaxContent; }
  static bool max_is_intrinsic(const TrackSize& t) {
    return t.kind == TrackSize::Auto || t.kind == TrackSize::MinContent || t.kind == TrackSize::MaxContent || t.kind == TrackSize::FitContent;
  }

  bool step_affects(int step, const Track& t) const {
    if (t.collapsed) return false;
    switch (step) {
      case 1: return min_is_intrinsic(t.def.min);
      case 2: return t.def.min.kind == TrackSize::MinContent || t.def.min.kind == TrackSize::MaxContent;
      case 3: return t.def.min.kind == TrackSize::Auto || t.def.min.kind == TrackSize::MaxContent;
      case 5: return max_is_intrinsic(t.def.max);
      default: return t.def.max.kind == TrackSize::MaxContent || t.def.max.kind == TrackSize::Auto || t.def.max.kind == TrackSize::FitContent;
    }
  }

  // One step of the growth of tracks for the items that span `span` tracks (none of them flexible).
  bool span_step(int axis, int32_t span, int step) {
    AxisData& d = ax(axis);
    const bool limits = step >= 5;
    const int kind = step == 1 ? 0 : (step == 2 || step == 5) ? 1 : 2;
    for (uint64_t i = 0; i < d.tracks.count(); ++i) d.tracks[i].planned = LayoutUnit();
    bool any = false;
    for (uint64_t k = 0; k < items_.count(); ++k) {
      const GridItem& it = items_[k];
      if (it.span[axis] != span || spans_flexible(axis, it)) continue;
      scratch_idx_.clear();
      scratch_room_.clear();
      for (int32_t t = it.pos[axis]; t < it.pos[axis] + span; ++t) {
        const Track& track = d.tracks[static_cast<uint64_t>(t)];
        if (!step_affects(step, track)) continue;
        if (!scratch_idx_.push_back(static_cast<uint32_t>(t))) return false;
        // (a base size may grow up to the growth limit; growth limits have no limit)
        if (!scratch_room_.push_back(limits || track.limit.is_max() ? kInfinite : max(track.limit - track.base, LayoutUnit()))) return false;
      }
      if (scratch_idx_.count() == 0) continue;
      const LayoutUnit space = it.contrib[axis][kind] - span_sum(axis, it.pos[axis], span, limits);
      if (space <= LayoutUnit()) continue;
      if (!share_equally(space, !limits)) return false;
      for (uint64_t j = 0; j < scratch_idx_.count(); ++j) {
        Track& track = d.tracks[scratch_idx_[j]];
        track.planned = max(track.planned, scratch_out_[j]);
        any = true;
      }
    }
    if (!any) return true;
    for (uint64_t i = 0; i < d.tracks.count(); ++i) {
      Track& t = d.tracks[i];
      if (t.planned <= LayoutUnit()) continue;
      if (limits) t.limit = limit_or_base(t) + t.planned;
      else t.base += t.planned;
    }
    return true;
  }

  bool spans_flexible(int axis, const GridItem& it) {
    if (it.span[axis] < 2) return false;
    AxisData& d = ax(axis);
    for (int32_t t = it.pos[axis]; t < it.pos[axis] + it.span[axis]; ++t)
      if (d.tracks[static_cast<uint64_t>(t)].def.max.kind == TrackSize::Flex) return true;
    return false;
  }

  struct FrSize {
    LayoutUnit leftover;
    int32_t sum = 0;
    LayoutUnit of(int32_t factor) const { return leftover.scaled(factor, sum == 0 ? 1 : sum); }
  };

  // Section 11.7.1, "find the size of an fr": for tracks [first, first + count) and `space` to fill.
  FrSize find_fr(int axis, int32_t first, int32_t count, LayoutUnit space) {
    AxisData& d = ax(axis);
    for (int32_t i = first; i < first + count; ++i) d.tracks[static_cast<uint64_t>(i)].inflexible = false;
    FrSize result;
    for (uint64_t guard = 0; guard <= static_cast<uint64_t>(count); ++guard) {
      LayoutUnit leftover = space;
      int64_t sum = 0;
      for (int32_t i = first; i < first + count; ++i) {
        const Track& t = d.tracks[static_cast<uint64_t>(i)];
        if (i + 1 < first + count) leftover -= t.gap_after;
        if (t.def.max.kind == TrackSize::Flex && !t.inflexible && !t.collapsed) sum += t.def.max.flex;
        else leftover -= t.base;
      }
      if (sum < 100) sum = 100;
      result.leftover = max(leftover, LayoutUnit());
      result.sum = static_cast<int32_t>(sum > 1000000000 ? 1000000000 : sum);
      bool restart = false;
      for (int32_t i = first; i < first + count; ++i) {
        Track& t = d.tracks[static_cast<uint64_t>(i)];
        if (t.def.max.kind != TrackSize::Flex || t.inflexible || t.collapsed) continue;
        if (t.base > result.of(t.def.max.flex)) {
          t.inflexible = true;
          restart = true;
        }
      }
      if (!restart) break;
    }
    return result;
  }

  bool size_tracks(int axis, Mode mode, LayoutUnit available, Place content_align) {
    AxisData& d = ax(axis);
    compute_gaps(axis);
    for (uint64_t i = 0; i < d.tracks.count(); ++i) {
      Track& t = d.tracks[i];
      t.planned = LayoutUnit();
      t.inflexible = false;
      if (t.collapsed) {
        t.base = t.limit = LayoutUnit();
        continue;
      }
      t.base = t.def.min.kind == TrackSize::Fixed ? t.def.min.length : LayoutUnit();
      t.limit = t.def.max.kind == TrackSize::Fixed ? t.def.max.length : kInfinite;
      if (t.limit < t.base) t.limit = t.base;
    }
    int32_t max_span = 1;
    for (uint64_t k = 0; k < items_.count(); ++k) max_span = max_i(max_span, items_[k].span[axis]);
    // items that span no flexible track, the smallest spans first
    for (int32_t span = 1; span <= max_span; ++span) {
      if (!span_step(axis, span, 1) || !span_step(axis, span, 2)) return false;
      if (mode == Mode::MaxContent && !span_step(axis, span, 3)) return false;
      for (uint64_t i = 0; i < d.tracks.count(); ++i)
        if (!d.tracks[i].limit.is_max() && d.tracks[i].limit < d.tracks[i].base) d.tracks[i].limit = d.tracks[i].base;
      if (!span_step(axis, span, 5) || !span_step(axis, span, 6)) return false;
    }
    // items that span flexible tracks: the flexible tracks grow to hold their min-content contribution
    for (int32_t span = 2; span <= max_span; ++span) {
      for (uint64_t i = 0; i < d.tracks.count(); ++i) d.tracks[i].planned = LayoutUnit();
      for (uint64_t k = 0; k < items_.count(); ++k) {
        const GridItem& it = items_[k];
        if (it.span[axis] != span || !spans_flexible(axis, it)) continue;
        int64_t sum = 0;
        for (int32_t t = it.pos[axis]; t < it.pos[axis] + span; ++t)
          if (d.tracks[static_cast<uint64_t>(t)].def.max.kind == TrackSize::Flex && !d.tracks[static_cast<uint64_t>(t)].collapsed) sum += d.tracks[static_cast<uint64_t>(t)].def.max.flex;
        if (sum <= 0) continue;
        const LayoutUnit space = it.contrib[axis][1] - span_sum(axis, it.pos[axis], span, false);
        if (space <= LayoutUnit()) continue;
        for (int32_t t = it.pos[axis]; t < it.pos[axis] + span; ++t) {
          Track& track = d.tracks[static_cast<uint64_t>(t)];
          if (track.def.max.kind != TrackSize::Flex || track.collapsed) continue;
          track.planned = max(track.planned, space.scaled(track.def.max.flex, static_cast<int32_t>(sum)));
        }
      }
      for (uint64_t i = 0; i < d.tracks.count(); ++i) d.tracks[i].base += d.tracks[i].planned;
    }
    // growth limits still infinite are the base sizes; fit-content tracks do not grow beyond their argument
    for (uint64_t i = 0; i < d.tracks.count(); ++i) {
      Track& t = d.tracks[i];
      if (t.collapsed) continue;
      if (t.def.max.kind == TrackSize::FitContent) t.limit = min(limit_or_base(t), max(t.base, t.def.max.length));
      if (t.limit.is_max() || t.limit < t.base) t.limit = t.base;
    }
    // maximize tracks
    LayoutUnit sizes;
    for (uint64_t i = 0; i < d.tracks.count(); ++i) sizes += d.tracks[i].base + d.tracks[i].gap_after;
    if (mode == Mode::Definite) {
      LayoutUnit free = available - sizes;
      if (free > LayoutUnit()) {
        scratch_idx_.clear();
        scratch_room_.clear();
        for (uint64_t i = 0; i < d.tracks.count(); ++i) {
          const Track& t = d.tracks[i];
          if (t.collapsed || t.limit <= t.base) continue;
          if (!scratch_idx_.push_back(static_cast<uint32_t>(i)) || !scratch_room_.push_back(t.limit - t.base)) return false;
        }
        if (scratch_idx_.count() > 0) {
          // (what the limits leave over stays free: it must not go on beyond them)
          LayoutUnit room_total;
          for (uint64_t j = 0; j < scratch_room_.count(); ++j) room_total += scratch_room_[j];
          if (!share_equally(min(free, room_total), true)) return false;
          for (uint64_t j = 0; j < scratch_idx_.count(); ++j) d.tracks[scratch_idx_[j]].base += scratch_out_[j];
        }
      }
    } else if (mode == Mode::MaxContent) {
      for (uint64_t i = 0; i < d.tracks.count(); ++i)
        if (!d.tracks[i].collapsed) d.tracks[i].base = d.tracks[i].limit;
    }
    // expand flexible tracks
    bool has_flex = false;
    for (uint64_t i = 0; i < d.tracks.count(); ++i)
      if (d.tracks[i].def.max.kind == TrackSize::Flex && !d.tracks[i].collapsed) has_flex = true;
    if (has_flex) {
      if (mode == Mode::Definite) {
        const FrSize fr = find_fr(axis, 0, static_cast<int32_t>(d.tracks.count()), available);
        for (uint64_t i = 0; i < d.tracks.count(); ++i) {
          Track& t = d.tracks[i];
          if (t.def.max.kind == TrackSize::Flex && !t.collapsed && !t.inflexible) t.base = max(t.base, fr.of(t.def.max.flex));
        }
      } else {
        LayoutUnit fr;   // the size of 1fr
        for (uint64_t i = 0; i < d.tracks.count(); ++i) {
          const Track& t = d.tracks[i];
          if (t.def.max.kind != TrackSize::Flex || t.collapsed) continue;
          fr = max(fr, t.def.max.flex > 100 ? t.base.scaled(100, t.def.max.flex) : t.base);
        }
        for (uint64_t k = 0; k < items_.count(); ++k) {
          const GridItem& it = items_[k];
          bool crosses = false;
          for (int32_t t = it.pos[axis]; t < it.pos[axis] + it.span[axis]; ++t)
            if (d.tracks[static_cast<uint64_t>(t)].def.max.kind == TrackSize::Flex && !d.tracks[static_cast<uint64_t>(t)].collapsed) crosses = true;
          if (!crosses) continue;
          const FrSize one = find_fr(axis, it.pos[axis], it.span[axis], it.contrib[axis][mode == Mode::MinContent ? 1 : 2]);
          fr = max(fr, one.of(100));
        }
        for (uint64_t i = 0; i < d.tracks.count(); ++i) {
          Track& t = d.tracks[i];
          if (t.def.max.kind == TrackSize::Flex && !t.collapsed) t.base = max(t.base, fr.scaled(t.def.max.flex, 100));
        }
      }
      for (uint64_t i = 0; i < d.tracks.count(); ++i) {
        Track& t = d.tracks[i];
        if (t.def.max.kind == TrackSize::Flex) t.limit = t.base;
      }
    }
    // stretch auto tracks
    if (mode == Mode::Definite && (content_align == Place::Normal || content_align == Place::Stretch)) {
      LayoutUnit total;
      uint64_t autos = 0;
      for (uint64_t i = 0; i < d.tracks.count(); ++i) {
        total += d.tracks[i].base + d.tracks[i].gap_after;
        if (!d.tracks[i].collapsed && d.tracks[i].def.max.kind == TrackSize::Auto) ++autos;
      }
      const LayoutUnit free = available - total;
      if (autos > 0 && free > LayoutUnit()) {
        const LayoutUnit each = free / static_cast<int32_t>(autos);
        LayoutUnit dust = free - each * static_cast<int32_t>(autos);
        for (uint64_t i = 0; i < d.tracks.count(); ++i) {
          Track& t = d.tracks[i];
          if (t.collapsed || t.def.max.kind != TrackSize::Auto) continue;
          t.base += each;
          if (dust > LayoutUnit()) {
            const LayoutUnit give = min(LayoutUnit::from_raw(1), dust);
            t.base += give;
            dust -= give;
          }
        }
      }
    }
    d.extent = LayoutUnit();
    for (uint64_t i = 0; i < d.tracks.count(); ++i) d.extent += d.tracks[i].base + d.tracks[i].gap_after;
    return true;
  }

  // Puts the tracks one after the other from `start`, with the room left over shared out as justify-content / align-content says.
  void position_tracks(int axis, bool known, LayoutUnit available, Place content_align) {
    AxisData& d = ax(axis);
    uint64_t count = 0;
    for (uint64_t i = 0; i < d.tracks.count(); ++i)
      if (!d.tracks[i].collapsed) ++count;
    LayoutUnit start, between;
    if (known) content_offsets(content_align, available - d.extent, count, &start, &between);
    LayoutUnit cursor = start;
    for (uint64_t i = 0; i < d.tracks.count(); ++i) {
      Track& t = d.tracks[i];
      t.pos = cursor;
      if (!t.collapsed && hasNonCollapsedAfter(d, i)) t.gap_after += between;
      cursor += t.base + t.gap_after;
    }
  }

  static bool hasNonCollapsedAfter(AxisData& d, uint64_t i) {
    for (uint64_t j = i + 1; j < d.tracks.count(); ++j)
      if (!d.tracks[j].collapsed) return true;
    return false;
  }

  // ---------------------------------------------------------------------------------------------------------------------------------------
  // The items

  LayoutUnit area_start(int axis, const GridItem& it) { return ax(axis).tracks[static_cast<uint64_t>(it.pos[axis])].pos; }
  LayoutUnit area_size(int axis, const GridItem& it) {
    AxisData& d = ax(axis);
    const Track& last = d.tracks[static_cast<uint64_t>(it.pos[axis] + it.span[axis] - 1)];
    return last.pos + last.base - d.tracks[static_cast<uint64_t>(it.pos[axis])].pos;
  }

  Align justify_of(const Box& b) {
    const Style& s = b.style;
    const Place p = s.justify_self != Place::Auto ? s.justify_self : box().style.justify_items;
    return to_align(p, b.kind != BoxKind::Replaced);
  }
  Align align_of(const Box& b) {
    const Style& s = b.style;
    const Place p = s.align_self != Place::Auto ? s.align_self : box().style.align_items;
    return to_align(p, b.kind != BoxKind::Replaced);
  }

  // Lays the item out for a grid area `area_w` wide. With `final_pass`, `area_h` is the height of the area too (stretching and percentage heights);
  // before that the item is laid out only to find out how high it is.
  void layout_item(GridItem& it, LayoutUnit area_w, bool final_pass, LayoutUnit area_h) {
    Box& b = tree_.box(it.box);
    const Style& s = b.style;
    LayoutContext::AutoMargins autos = ctx_.resolve_edges(b, area_w);
    const LayoutUnit bp_h = b.border.horizontal() + b.padding.horizontal(), bp_v = b.border.vertical() + b.padding.vertical();
    const bool replaced = b.kind == BoxKind::Replaced;
    LayoutContext::WidthRequest request;
    LayoutUnit v;
    const bool keyword = s.width.kind == Length::MinContent || s.width.kind == Length::MaxContent || s.width.kind == Length::FitContent;
    const bool width_given = replaced || keyword || ctx_.resolve_in(s.width, b, false, &v);
    if (!width_given) {
      if (justify_of(b) == Align::Stretch && !autos.left && !autos.right) {
        request.forced = true;
        request.content_width = ctx_.clamp_content_width(b, max(area_w - b.margin.horizontal() - bp_h, LayoutUnit()));
      } else {
        request.shrink = true;
        request.available = area_w;
      }
    }
    if (final_pass) {
      request.cb_height_known = true;
      request.cb_height = area_h;
      if (!replaced && !ctx_.resolve_with(s.height, s, area_h, true, &v) && align_of(b) == Align::Stretch && !autos.top && !autos.bottom) {
        request.has_forced_height = true;
        request.forced_height = ctx_.clamp_content_height(b, max(area_h - b.margin.vertical() - bp_v, LayoutUnit()));
      }
    }
    ctx_.layout_block(it.box, area_w, request);
  }

  // Is a second layout needed once the area's height is known?
  bool needs_final_layout(const GridItem& it, LayoutUnit area_h) {
    Box& b = tree_.box(it.box);
    const Style& s = b.style;
    if (s.height.has_percent() || s.min_height.has_percent() || s.max_height.has_percent()) return true;
    if (b.kind == BoxKind::Replaced) return false;
    LayoutUnit v;
    if (ctx_.resolve_with(s.height, s, area_h, true, &v)) return false;
    ctx_.resolve_edges(b, b.cb_width);
    if (align_of(b) != Align::Stretch || s.margin[0].is_auto() || s.margin[2].is_auto()) return false;
    const LayoutUnit target = ctx_.clamp_content_height(b, max(area_h - b.margin.vertical() - b.border.vertical() - b.padding.vertical(), LayoutUnit())) + b.border.vertical() + b.padding.vertical();
    return target != b.height;
  }

  // Row contributions: each item laid out in the width of its columns.
  bool measure_rows() {
    for (uint64_t k = 0; k < items_.count(); ++k) {
      GridItem& it = items_[k];
      layout_item(it, area_size(0, it), false, LayoutUnit());
      Box& b = tree_.box(it.box);
      const LayoutContext::AutoMargins autos = ctx_.resolve_edges(b, area_size(0, it));
      (void)autos;
      const LayoutUnit outer = b.height + b.margin.vertical();
      it.contrib[1][1] = it.contrib[1][2] = outer;
      it.contrib[1][0] = b.style.overflow_y != Overflow::Visible ? b.margin.vertical() : outer;
      it.baseline = align_of(b) == Align::Baseline && it.span[1] == 1;
      it.ascent = b.margin.top + (b.has_baseline ? b.baseline : b.height);
      it.shim = LayoutUnit();
    }
    // baseline alignment: the items of a row that share a baseline are moved down to the lowest one, which can make the row higher
    AxisData& rows = ax(1);
    List<LayoutUnit> row_ascent(a_);
    for (uint64_t i = 0; i < rows.tracks.count(); ++i)
      if (!row_ascent.push_back(LayoutUnit())) return false;
    for (uint64_t k = 0; k < items_.count(); ++k)
      if (items_[k].baseline) row_ascent[static_cast<uint64_t>(items_[k].pos[1])] = max(row_ascent[static_cast<uint64_t>(items_[k].pos[1])], items_[k].ascent);
    for (uint64_t k = 0; k < items_.count(); ++k) {
      GridItem& it = items_[k];
      if (!it.baseline) continue;
      it.shim = row_ascent[static_cast<uint64_t>(it.pos[1])] - it.ascent;
      for (int kind = 0; kind < 3; ++kind) it.contrib[1][kind] += it.shim;
    }
    return true;
  }

  bool resolve_limit(const Length& l, bool* has, LayoutUnit* out) {
    Box& b = box();
    LayoutUnit v;
    *has = ctx_.resolve_in(l, b, true, &v);
    if (*has) *out = max(b.style.sizing == browser::BoxSizing::BorderBox ? v - b.border.vertical() - b.padding.vertical() : v, LayoutUnit());
    return *has;
  }

  // The container's baseline (its first row's) from the items that were placed.
  void set_container_baseline() {
    Box& b = box();
    if (b.style.overflow_x != Overflow::Visible || b.style.overflow_y != Overflow::Visible) return;
    const GridItem* best = nullptr;
    for (uint64_t k = 0; k < items_.count(); ++k) {
      const GridItem& it = items_[k];
      if (it.pos[1] != 0 || !tree_.box(it.box).has_baseline) continue;
      if (!best || (it.baseline && !best->baseline)) best = &it;
    }
    if (!best) return;
    const Box& c = tree_.box(best->box);
    b.baseline = c.y + c.baseline;
    b.has_baseline = true;
  }

  // ---------------------------------------------------------------------------------------------------------------------------------------

  ChildrenResult lay_out(LayoutUnit content_width, bool height_known, LayoutUnit height) {
    ChildrenResult result;
    if (!collect_items(true) || !read_templates(true, content_width, height_known, height) || !place_items()) {
      ctx_.failed_ = true;
      return result;
    }
    Box& cont = box();
    const Style& cs = cont.style;
    result.empty = items_.count() == 0;
    for (uint64_t k = 0; k < items_.count(); ++k) column_contributions(items_[k], true, content_width);
    // columns
    if (!size_tracks(0, Mode::Definite, content_width, cs.justify_content)) {
      ctx_.failed_ = true;
      return result;
    }
    position_tracks(0, true, content_width, cs.justify_content);
    // rows
    if (!measure_rows()) {
      ctx_.failed_ = true;
      return result;
    }
    LayoutUnit rows_height = height;
    if (height_known) {
      ok_ = size_tracks(1, Mode::Definite, height, cs.align_content);
    } else {
      ok_ = size_tracks(1, Mode::MaxContent, LayoutUnit(), cs.align_content);
      bool has = false;
      LayoutUnit limit;
      rows_height = ax(1).extent;
      if (ok_ && resolve_limit(cs.min_height, &has, &limit) && rows_height < limit) {
        rows_height = limit;
        ok_ = size_tracks(1, Mode::Definite, limit, cs.align_content);
        height_known = true;
      } else if (ok_ && resolve_limit(cs.max_height, &has, &limit) && rows_height > limit) {
        rows_height = limit;
        ok_ = size_tracks(1, Mode::Definite, limit, cs.align_content);
        height_known = true;
      }
    }
    if (!ok_) {
      ctx_.failed_ = true;
      return result;
    }
    const LayoutUnit extent = ax(1).extent;
    position_tracks(1, height_known, rows_height, cs.align_content);
    // the items
    for (uint64_t k = 0; k < items_.count() && !ctx_.failed_; ++k) place_item(items_[k]);
    set_container_baseline();
    result.height = extent;
    return result;
  }

  void place_item(GridItem& it) {
    Box& cont = box();
    const LayoutUnit area_x = area_start(0, it), area_w = area_size(0, it), area_y = area_start(1, it), area_h = area_size(1, it);
    if (needs_final_layout(it, area_h)) layout_item(it, area_w, true, area_h);
    Box& b = tree_.box(it.box);
    const LayoutContext::AutoMargins autos = ctx_.resolve_edges(b, area_w);   // (layout_block zeroed the auto margins: they are decided here)
    // across (justify-self)
    LayoutUnit ml = b.margin.left, mr = b.margin.right, mt = b.margin.top, mb = b.margin.bottom;
    LayoutUnit x;
    if (autos.left || autos.right) {
      const LayoutUnit spare = max(area_w - b.width - ml - mr, LayoutUnit());
      if (autos.left && autos.right) {
        ml = spare / 2;
        mr = spare - ml;
      } else if (autos.left) {
        ml = spare;
      } else {
        mr = spare;
      }
      x = ml;
    } else {
      switch (justify_of(b)) {
        case Align::End: x = area_w - mr - b.width; break;
        case Align::Center: x = (area_w - b.width - ml - mr) / 2 + ml; break;
        default: x = ml; break;
      }
    }
    // along (align-self)
    LayoutUnit y;
    if (autos.top || autos.bottom) {
      const LayoutUnit spare = max(area_h - b.height - mt - mb, LayoutUnit());
      if (autos.top && autos.bottom) {
        mt = spare / 2;
        mb = spare - mt;
      } else if (autos.top) {
        mt = spare;
      } else {
        mb = spare;
      }
      y = mt;
    } else {
      switch (align_of(b)) {
        case Align::End: y = area_h - mb - b.height; break;
        case Align::Center: y = (area_h - b.height - mt - mb) / 2 + mt; break;
        case Align::Baseline: y = it.baseline ? it.shim + mt : mt; break;
        default: y = mt; break;
      }
    }
    b.margin.left = ml;
    b.margin.right = mr;
    b.margin.top = mt;
    b.margin.bottom = mb;
    b.x = cont.content_left() + area_x + x;
    b.y = cont.content_top() + area_y + y;
    ctx_.apply_relative(b);
  }

  void intrinsic(LayoutUnit* min_content, LayoutUnit* max_content) {
    *min_content = *max_content = LayoutUnit();
    if (!collect_items(false) || !read_templates(false, LayoutUnit(), false, LayoutUnit()) || !place_items()) {
      ctx_.failed_ = true;
      return;
    }
    for (uint64_t k = 0; k < items_.count(); ++k) column_contributions(items_[k], false, LayoutUnit());
    const Place align = box().style.justify_content;
    if (!size_tracks(0, Mode::MinContent, LayoutUnit(), align)) {
      ctx_.failed_ = true;
      return;
    }
    *min_content = ax(0).extent;
    if (!size_tracks(0, Mode::MaxContent, LayoutUnit(), align)) {
      ctx_.failed_ = true;
      return;
    }
    *max_content = ax(0).extent;
  }

  LayoutContext& ctx_;
  BoxTree& tree_;
  BoxId id_;
  allocators::IAllocator& a_;
  AxisData x_, y_;
  AreaGrid areas_;
  bool have_areas_ = false;
  bool column_flow_ = false, dense_ = false;
  bool ok_ = true;
  List<GridItem> items_;
  Occupancy occ_;
  List<uint32_t> scratch_idx_;
  List<LayoutUnit> scratch_room_, scratch_out_;
};

ChildrenResult LayoutContext::layout_grid(BoxId id, LayoutUnit content_width, bool height_known, LayoutUnit height) {
  GridLayout grid(*this, id);
  return grid.lay_out(content_width, height_known, height);
}

void LayoutContext::grid_intrinsic_widths(BoxId id, LayoutUnit* min_content, LayoutUnit* max_content) {
  GridLayout grid(*this, id);
  grid.intrinsic(min_content, max_content);
}

}  // namespace layout
