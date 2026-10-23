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

// What a laid out page looks like as a list of drawing operations in page coordinates (CSS pixels, the origin at the top left of the page, y down),
// in the order they are painted: backgrounds, borders, shadows, text, pictures, with the clips and opacity layers around them that overflow and
// opacity make. build_display_list() (PaintBuilder.hpp) makes it from a laid out box tree following the stacking order of CSS 2.1 appendix E;
// DisplayPainter (DisplayPainter.hpp) draws it. Every operation has the rectangle it covers, so a painter can skip what is off the screen.
// Transforms, filters, blurred shadows, clip paths and background pictures are operations too: groups (PushTransform / PushFilter) carry the
// bounds of what is inside, and the data that does not fit an Op (matrices, filter chains, shapes) lives in tables of the list.
#pragma once
#include "Font/FontSet.hpp"
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace paint {

// A rectangle in whole pixels.
struct IRect {
  int32_t x = 0, y = 0, width = 0, height = 0;
  int32_t right() const { return x + width; }
  int32_t bottom() const { return y + height; }
  bool empty() const { return width <= 0 || height <= 0; }
  bool intersects(const IRect& o) const { return !empty() && !o.empty() && x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom(); }
  IRect intersection(const IRect& o) const {
    const int32_t l = x > o.x ? x : o.x, t = y > o.y ? y : o.y;
    const int32_t r = right() < o.right() ? right() : o.right(), b = bottom() < o.bottom() ? bottom() : o.bottom();
    IRect out;
    out.x = l;
    out.y = t;
    out.width = r > l ? r - l : 0;
    out.height = b > t ? b - t : 0;
    return out;
  }
  bool contains(int32_t px, int32_t py) const { return px >= x && py >= y && px < right() && py < bottom(); }
};

enum class OpKind : uint8_t {
  FillRect,       // `rect` in `color` (rounded when `radii` is not all 0)
  Border,         // the border of `rect`: widths, colours and styles per side, rounded by `radii`
  Gradient,       // `rect` filled with gradient number `gradient`, rounded by `radii`
  Text,           // a run of text: see the text fields
  Object,         // a picture, video, canvas or svg of the element `node`, drawn by the object painter into `rect`
  PushClip,       // everything until the matching PopClip is clipped to `rect` (rounded by `radii`)
  PopClip,
  PushLayer,      // everything until the matching PopLayer is drawn as one group with `opacity` (0..255)
  PopLayer,
  Hit,            // nothing is drawn: the element `node` is under `rect` (what a click or the pointer finds), with the cursor it asks for
  PushTransform,  // everything until the matching PopTransform is drawn through the affine matrix `data` (page coordinates to page coordinates); `rect` bounds the content
  PopTransform,
  PushFilter,     // everything until the matching PopFilter is drawn as a group and run through the filter chain `data`; `rect` bounds the result
  PopFilter,
  PushClipPath,   // like PushClip, but to the shape `data` (its PopClip is a PopClip)
  Shadow,         // a box-shadow: the shadow `data` (blurred, clipped out under the box or inside it for an inset shadow)
  Image,          // a picture from the object painter (a url() background layer, a list marker): see ImageLayerData `data`, the url is the text of the op
};

// Border line styles (browser::BorderStyle's order).
enum class LineStyle : uint8_t { None, Hidden, Dotted, Dashed, Solid, Double, Groove, Ridge, Inset, Outset };

constexpr uint8_t kDecorationUnderline = 1, kDecorationStrike = 2, kDecorationOverline = 4;
enum class DecorationStyle : uint8_t { Solid, Double, Dotted, Dashed, Wavy };

// A 2D affine matrix: (x, y) goes to (a x + c y + e, b x + d y + f).
struct Affine {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  static Affine translation(double x, double y) {
    Affine m;
    m.e = x;
    m.f = y;
    return m;
  }
  double x_of(double x, double y) const { return a * x + c * y + e; }
  double y_of(double x, double y) const { return b * x + d * y + f; }
  // this after `o` (o is applied first)
  Affine operator*(const Affine& o) const {
    Affine m;
    m.a = a * o.a + c * o.b;
    m.b = b * o.a + d * o.b;
    m.c = a * o.c + c * o.d;
    m.d = b * o.c + d * o.d;
    m.e = a * o.e + c * o.f + e;
    m.f = b * o.e + d * o.f + f;
    return m;
  }
  double determinant() const { return a * d - b * c; }
  bool invert(Affine* out) const {
    const double det = determinant();
    if (!(det > 1e-12 || det < -1e-12)) return false;
    out->a = d / det;
    out->b = -b / det;
    out->c = -c / det;
    out->d = a / det;
    out->e = -(out->a * e + out->c * f);
    out->f = -(out->b * e + out->d * f);
    return true;
  }
  bool axis_aligned() const { return (b < 1e-9 && b > -1e-9) && (c < 1e-9 && c > -1e-9); }
  bool is_translation() const { return axis_aligned() && a > 1 - 1e-9 && a < 1 + 1e-9 && d > 1 - 1e-9 && d < 1 + 1e-9; }
};

// One step of a filter chain (the filter property's functions).
struct FilterStep {
  enum Kind : uint8_t { Blur, Brightness, Contrast, Grayscale, HueRotate, Invert, Opacity, Saturate, Sepia, DropShadow };
  Kind kind = Blur;
  float amount = 0;                      // Blur: pixels; HueRotate: degrees; the others a factor (1 = 100%)
  float dx = 0, dy = 0, blur = 0;        // DropShadow: offset and blur radius in pixels
  uint32_t color = 0;                    // DropShadow: 0xAARRGGBB
};
constexpr uint32_t kMaxFilterSteps = 8;
struct FilterData {
  uint32_t count = 0;
  FilterStep steps[kMaxFilterSteps];
};

// A box-shadow in page coordinates. `box` is the border box (outer shadow: the shadow is cut out under it) or the padding box (inset: the shadow is
// drawn inside it); `radii` round it. The offset, spread and blur are as the property gives them.
struct ShadowData {
  IRect box;
  graphics::CornerRadii radii;
  int32_t dx = 0, dy = 0, spread = 0;
  float blur = 0;
  uint32_t color = 0;
  bool inset = false;
};

// A clip-path shape in page coordinates: a rounded rectangle (an ellipse is one) or a polygon.
constexpr uint32_t kMaxClipPoints = 32;
struct ClipShape {
  bool polygon = false, evenodd = false;
  float x = 0, y = 0, width = 0, height = 0;      // rounded rectangle
  graphics::CornerRadii radii;
  uint32_t point_count = 0;
  graphics::Vec2 points[kMaxClipPoints];
};

// A background picture layer: the url (the op's text) drawn by the object painter into tiles. All lengths are resolved but the natural size of the picture,
// which only the object painter knows.
struct ImageLayerData {
  enum SizeMode : uint8_t { Auto, Cover, Contain, Explicit };
  IRect origin;                          // the positioning area
  IRect clip;                            // where the layer shows
  SizeMode size_mode = Auto;
  float size_w = -1, size_h = -1;        // Explicit: pixels, negative for auto
  int32_t pos_x_percent_x100 = 0, pos_x_px100 = 0, pos_y_percent_x100 = 0, pos_y_px100 = 0;
  uint8_t repeat_x = 0, repeat_y = 0;    // 0 repeat, 1 space, 2 round, 3 no-repeat
};

struct Op {
  OpKind kind = OpKind::FillRect;
  uint8_t opacity = 255;                 // PushLayer
  uint8_t decorations = 0;               // Text: kDecoration* bits
  uint8_t cursor = 0;                    // Hit: a browser::Cursor
  IRect rect;                            // where it draws (Text: the content area of the run, for hit testing and culling)
  uint32_t color = 0;                    // FillRect, Text: 0xAARRGGBB
  graphics::CornerRadii radii;           // FillRect, Border, Gradient, PushClip
  uint32_t node = 0;                     // Text and Object: the serial of the element it belongs to (0: none)
  uint32_t data = 0;                     // PushTransform, PushFilter, PushClipPath, Shadow, Image: an index into the list's table of that kind

  // Border
  uint32_t border_colors[4] = {0, 0, 0, 0};          // top, right, bottom, left
  uint16_t border_widths[4] = {0, 0, 0, 0};
  LineStyle border_styles[4] = {LineStyle::None, LineStyle::None, LineStyle::None, LineStyle::None};

  // Gradient
  uint32_t gradient = 0;                 // an index into DisplayList::gradient()

  // Text: the characters (a slice of the list's text pool), the font, and where the first glyph sits: x in 1/64 px, baseline in whole pixels
  uint32_t text_offset = 0, text_length = 0;
  uint32_t font = 0;                     // an index into DisplayList::font_spec(): fonts are described by value, so the painter can use a font set of its own
  int32_t text_x64 = 0, baseline = 0;
  int32_t letter_spacing64 = 0, word_spacing64 = 0;
  int32_t decoration_thickness = 1, decoration_offset = 0;   // underline: pixels below the baseline
  int32_t strike_offset = 0;             // line-through: pixels above the baseline (0: derived from decoration_offset)
  uint8_t decoration_style = 0;          // a DecorationStyle
  uint32_t decoration_color = 0;         // 0xAARRGGBB of the lines (0: the text colour)
  float blur = 0;                        // Text: > 0 for a text-shadow, the blur radius in pixels (the run is drawn blurred, `color` is the shadow colour)

  // Object
  uint8_t object_kind = 0;               // browser::ObjectKind
};

// A gradient fill ready to draw: a line (linear), an ellipse (radial) or a sweep around a point (conic) in page coordinates, and the stops. Stop offsets
// are along the line (0 at `from`, 1 at `to`), in radii (radial) or in turns (conic). Repeating gradients and gradients with colour hints are drawn
// pixel by pixel by the painter.
struct GradientData {
  enum Kind : uint8_t { Linear, Radial, Conic };
  Kind kind = Linear;
  bool repeating = false;
  graphics::Vec2 from, to;               // linear: the gradient line
  graphics::Vec2 center, radii;          // radial, conic (the centre)
  float start_angle = 0;                 // conic: radians clockwise from the top
  uint32_t stop_count = 0;
  graphics::GradientStop stops[graphics::kMaxGradientStops];
  float hints[graphics::kMaxGradientStops];   // the colour hint after stop i as a fraction 0..1 of the way to stop i+1 (negative: none)
  GradientData() {
    for (float& h : hints) h = -1.0f;
  }
};

// A font as the style asked for it (what FontSet::match() takes), with the family list kept here.
struct FontSpec {
  explicit FontSpec(allocators::IAllocator& allocator) : families(allocator) {}
  String families;
  uint16_t weight = 400;
  font::FontStyle style = font::FontStyle::Normal;
  uint16_t stretch = 100;
  font::Fixed size = 16 * font::kFixedOne;
  font::GenericFamily default_generic = font::GenericFamily::Serif;
};

class DisplayList {
 public:
  explicit DisplayList(allocators::IAllocator& allocator) : allocator_(allocator), ops_(allocator), text_(allocator), gradients_(allocator), fonts_(allocator), transforms_(allocator), filters_(allocator),
        shadows_(allocator), shapes_(allocator), images_(allocator) {}
  DisplayList(const DisplayList&) = delete;
  DisplayList& operator=(const DisplayList&) = delete;

  void clear() {
    ops_.clear();
    text_.clear();
    gradients_.clear();
    fonts_.clear();
    transforms_.clear();
    filters_.clear();
    shadows_.clear();
    shapes_.clear();
    images_.clear();
    width_ = height_ = 0;
    background_ = 0;
  }

  uint64_t count() const { return ops_.count(); }
  const Op& op(uint64_t index) const { return ops_[index]; }
  StringView text(const Op& op) const { return text_.view().substr(op.text_offset, op.text_length); }
  const GradientData& gradient(uint32_t index) const { return gradients_[index]; }
  uint64_t gradient_count() const { return gradients_.count(); }
  const Affine& transform(uint32_t index) const { return transforms_[index]; }
  const FilterData& filter(uint32_t index) const { return filters_[index]; }
  const ShadowData& shadow(uint32_t index) const { return shadows_[index]; }
  const ClipShape& shape(uint32_t index) const { return shapes_[index]; }
  const ImageLayerData& image(uint32_t index) const { return images_[index]; }

  // The builder patches the bounds of a group once it knows what is in it.
  Op& mutable_op(uint64_t index) { return ops_[index]; }
  // The index of the Pop that closes the Push at `begin` (count() if the list is not balanced).
  uint64_t group_end(uint64_t begin) const {
    uint64_t depth = 0;
    for (uint64_t i = begin; i < ops_.count(); ++i) {
      const OpKind k = ops_[i].kind;
      if (k == OpKind::PushClip || k == OpKind::PushLayer || k == OpKind::PushTransform || k == OpKind::PushFilter || k == OpKind::PushClipPath) {
        ++depth;
      } else if (k == OpKind::PopClip || k == OpKind::PopLayer || k == OpKind::PopTransform || k == OpKind::PopFilter) {
        if (depth <= 1) return i;
        --depth;
      }
    }
    return ops_.count();
  }

  // The size of the page's content in pixels and the colour of the page's canvas (0: none given).
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  uint32_t background() const { return background_; }
  void set_extent(int32_t width, int32_t height) {
    width_ = width;
    height_ = height;
  }
  void set_background(uint32_t color) { background_ = color; }

  uint64_t font_count() const { return fonts_.count(); }
  const FontSpec& font_spec(uint32_t index) const { return fonts_[index]; }
  // The index of the font `query` describes (added if the list does not have it yet); false if memory ran out.
  bool add_font(const font::FontQuery& query, uint32_t* index) {
    // (the fonts of a page are few: look at the last ones first, where a repeat usually is)
    for (uint64_t i = fonts_.count(); i > 0 && fonts_.count() - i < 16; --i) {
      const FontSpec& f = fonts_[i - 1];
      if (f.weight == query.weight && f.style == query.style && f.stretch == query.stretch && f.size == query.size && f.default_generic == query.default_generic &&
          f.families.view().equals(query.families)) {
        *index = static_cast<uint32_t>(i - 1);
        return true;
      }
    }
    FontSpec spec(allocator_);
    spec.weight = query.weight;
    spec.style = query.style;
    spec.stretch = query.stretch;
    spec.size = query.size;
    spec.default_generic = query.default_generic;
    if (!spec.families.assign(query.families)) return false;
    *index = static_cast<uint32_t>(fonts_.count());
    return fonts_.push_back(util::move(spec));
  }

  // Appending (false if memory ran out).
  bool add(const Op& op) { return ops_.push_back(op); }
  // Keeps `characters` in the text pool and returns where (for Op::text_offset); false if memory ran out.
  bool add_text(StringView characters, uint32_t* offset) {
    *offset = static_cast<uint32_t>(text_.size());
    return text_.append(characters);
  }
  bool add_gradient(const GradientData& g, uint32_t* index) {
    *index = static_cast<uint32_t>(gradients_.count());
    return gradients_.push_back(g);
  }
  bool add_transform(const Affine& m, uint32_t* index) {
    *index = static_cast<uint32_t>(transforms_.count());
    return transforms_.push_back(m);
  }
  bool add_filter(const FilterData& f, uint32_t* index) {
    *index = static_cast<uint32_t>(filters_.count());
    return filters_.push_back(f);
  }
  bool add_shadow(const ShadowData& sh, uint32_t* index) {
    *index = static_cast<uint32_t>(shadows_.count());
    return shadows_.push_back(sh);
  }
  bool add_shape(const ClipShape& shape, uint32_t* index) {
    *index = static_cast<uint32_t>(shapes_.count());
    return shapes_.push_back(shape);
  }
  bool add_image(const ImageLayerData& image, uint32_t* index) {
    *index = static_cast<uint32_t>(images_.count());
    return images_.push_back(image);
  }

 private:
  allocators::IAllocator& allocator_;
  List<Op> ops_;
  String text_;
  List<GradientData> gradients_;
  List<FontSpec> fonts_;
  List<Affine> transforms_;
  List<FilterData> filters_;
  List<ShadowData> shadows_;
  List<ClipShape> shapes_;
  List<ImageLayerData> images_;
  int32_t width_ = 0, height_ = 0;
  uint32_t background_ = 0;
};

}  // namespace paint
