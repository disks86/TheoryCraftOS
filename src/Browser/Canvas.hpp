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

// The drawing surface behind a <canvas> element's "2d" context: the state the context keeps (transform, styles, line settings, clip, alpha, composite
// operation), paths (lines, curves, arcs, rectangles), filling and stroking them, text, images, pixel reads and writes.
//
// How it draws: every drawing operation is turned into device-space rectangles and image blits, and the ones that a display can do by itself - solid
// colours with ordinary alpha blending inside a rectangular clip, which is nearly everything a game or a chart draws - are only *recorded*. When the canvas
// is shown, paint() plays the record back through the window's IRenderer2D: the GPU renderer when the machine has one (the canvas is then drawn by the
// graphics processor, with no pixels computed by the CPU), else the software renderer. Operations the display cannot do (gradients, patterns, odd
// composite operations, clip shapes, partial erasing, rotated images) and every read of the pixels first flatten the record into the canvas's own pixel
// buffer, which is also what a canvas that is shown scaled, or captured with toDataURL(), is drawn from. clearRect() over the whole canvas throws the record
// away, so an animation that clears and draws every frame never builds up anything. Everything is thread safe only through the owner (PageObjects).
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "Math/Matrix.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace browser {

using graphics::Rect2D;

// A canvas transform is the affine 3x3 matrix [a c e; b d f; 0 0 1] the canvas API names a b c d e f - the same math::Matrix3d the rest of the Math code uses,
// stored column-major like every matrix in this OS (data = a b 0 c d 0 e f 1; the 3D code's graphics::Mat4 is the 4x4 column-major one). The functions below
// put the canvas's names on it. A default Matrix3d is all zeros, so start from Transform::Identity().
using Transform = math::Matrix3d;

inline Transform make_transform(double a, double b, double c, double d, double e, double f) { return Transform(a, b, 0, c, d, 0, e, f, 1); }
inline double tr_a(const Transform& m) { return m.at(0, 0); }
inline double tr_b(const Transform& m) { return m.at(1, 0); }
inline double tr_c(const Transform& m) { return m.at(0, 1); }
inline double tr_d(const Transform& m) { return m.at(1, 1); }
inline double tr_e(const Transform& m) { return m.at(0, 2); }
inline double tr_f(const Transform& m) { return m.at(1, 2); }
inline void transform_point(const Transform& m, double x, double y, double* ox, double* oy) {
  *ox = tr_a(m) * x + tr_c(m) * y + tr_e(m);
  *oy = tr_b(m) * x + tr_d(m) * y + tr_f(m);
}
// outer * inner: the points go through `inner` first.
inline Transform transform_times(const Transform& outer, const Transform& inner) {
  Transform r = Transform::Identity();
  static_cast<math::Matrix<double, 3, 3>&>(r) = outer * inner;
  return r;
}
inline bool transform_invert(const Transform& m, Transform* out) {
  const double det = m.determinant();
  if (det == 0 || det != det) return false;
  *out = m.inverse();
  return true;
}
inline bool transform_axis_aligned(const Transform& m) { return tr_b(m) == 0 && tr_c(m) == 0; }

// Angle of the vector (x, y) in radians, -pi..pi.
double canvas_atan2(double y, double x);

struct CanvasPoint {
  double x = 0, y = 0;
};

// Pixels [x0, x1) of row y.
struct CanvasSpan {
  int32_t y, x0, x1;
};

// Lines and curves. The points go through the transform that was set when they were added (a canvas's current path takes the context's transform;
// a Path2D stays in its own space, and the context's transform is applied when it is drawn).
class Path {
 public:
  enum class Verb : uint8_t { Move, Line, Cubic, Close };
  struct Sub {
    uint32_t first = 0, count = 0;   // points of a flattened polyline
    bool closed = false;
  };

  explicit Path(allocators::IAllocator& allocator) : verbs_(allocator), points_(allocator) {}

  void set_transform(const Transform& m) { transform_ = m; }
  void clear();
  bool empty() const { return verbs_.count() == 0; }
  bool failed() const { return failed_; }

  void move_to(double x, double y);
  void line_to(double x, double y);
  void quad_to(double cx, double cy, double x, double y);
  void bezier_to(double c1x, double c1y, double c2x, double c2y, double x, double y);
  // False if the radius is negative.
  bool arc(double x, double y, double radius, double start, double end, bool counter_clockwise);
  bool arc_to(double x1, double y1, double x2, double y2, double radius);
  bool ellipse(double x, double y, double rx, double ry, double rotation, double start, double end, bool counter_clockwise);
  void rect(double x, double y, double w, double h);
  void round_rect(double x, double y, double w, double h, double radius);
  void close();
  void add_path(const Path& other, const Transform& m);

  // The path as polylines in device space (the stored points taken through `m` first).
  bool flatten(const Transform& m, List<CanvasPoint>* points, List<Sub>* subs) const;

  bool has_current_point() const { return has_current_; }

 private:
  void add_point(Verb verb, double x, double y);
  void add_user(Verb verb, double x, double y);
  bool ensure_start(double x, double y);
  void restart_after_close();

  List<Verb> verbs_;
  List<CanvasPoint> points_;
  Transform transform_ = Transform::Identity();
  double cur_x_ = 0, cur_y_ = 0, start_x_ = 0, start_y_ = 0;   // user space
  bool has_current_ = false, failed_ = false, after_close_ = false;
};

enum class Composite : uint8_t {
  SourceOver, SourceIn, SourceOut, SourceAtop, DestinationOver, DestinationIn, DestinationOut, DestinationAtop, Lighter, Copy, Xor,
  Multiply, Screen, Overlay, Darken, Lighten, Difference, Exclusion,
};
bool parse_composite(StringView name, Composite* out);
const char* composite_name(Composite op);

struct GradientStop {
  float offset = 0;
  uint32_t color = 0;
};

// A fill or stroke style.
struct CanvasPaint {
  enum class Kind : uint8_t { Solid, Linear, Radial, Conic, Pattern };
  static constexpr uint32_t kMaxStops = 32;
  Kind kind = Kind::Solid;
  uint32_t color = 0xFF000000u;                 // Solid: 0xAARRGGBB
  double x0 = 0, y0 = 0, r0 = 0, x1 = 0, y1 = 0, r1 = 0;   // Linear: (x0,y0) to (x1,y1); Radial: circles; Conic: centre (x0,y0), start angle r0
  uint32_t stop_count = 0;
  GradientStop stops[kMaxStops];
  uint32_t pattern = 0;                          // Pattern: which picture (CanvasBitmap::add_pattern())
  bool repeat_x = true, repeat_y = true;

  void add_stop(double offset, uint32_t color);
};

// A picture to draw from: straight-alpha 0xAARRGGBB pixels (row y starts at pixels[y * pitch]).
struct ImageSource {
  ArrayView<const uint32_t> pixels;
  uint32_t width = 0, height = 0, pitch = 0;   // pitch in pixels
  bool valid() const { return !pixels.empty() && width && height && pitch >= width && pixels.size() >= uint64_t{pitch} * (height - 1) + width; }
};

struct CanvasState {
  Transform ctm = Transform::Identity();
  CanvasPaint fill, stroke;
  double line_width = 1, miter_limit = 10, dash_offset = 0, global_alpha = 1;
  uint8_t line_cap = 0, line_join = 0;           // cap: 0 butt, 1 round, 2 square; join: 0 miter, 1 round, 2 bevel
  static constexpr uint32_t kMaxDashes = 8;
  double dash[kMaxDashes];
  uint32_t dash_count = 0;
  Composite composite = Composite::SourceOver;
  int32_t clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;   // device pixels, [x0,x1) x [y0,y1)
  uint32_t clip_mask = 0;                         // 0: the clip is just the rectangle; else a mask (index + 1)
  double font_size = 10;
  bool font_bold = false;
  uint8_t font_text_length = 0;                   // the font as the script wrote it ("bold 20px Arial"); 0: the default, "10px sans-serif"
  char font_text[96];
  uint8_t text_align = 0, text_baseline = 0;     // align: 0 start, 1 end, 2 left, 3 right, 4 center; baseline: 0 alphabetic, 1 top, 2 hanging, 3 middle, 4 ideographic, 5 bottom
  bool smoothing = true;
  uint32_t shadow_color = 0;
  double shadow_blur = 0, shadow_x = 0, shadow_y = 0;
};

class CanvasBitmap {
 public:
  static constexpr uint32_t kMaxDimension = 16384;
  static constexpr uint64_t kMaxPixels = 1u << 24;
  static constexpr uint32_t kMaxPendingOps = 24000;
  static constexpr uint32_t kMaxStates = 128;

  CanvasBitmap(allocators::IAllocator& allocator, uint32_t width, uint32_t height);
  ~CanvasBitmap();
  CanvasBitmap(const CanvasBitmap&) = delete;
  CanvasBitmap& operator=(const CanvasBitmap&) = delete;

  bool valid() const { return valid_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  // Changes with every drawing operation.
  uint64_t version() const { return version_; }
  // How many operations are recorded and not flattened yet, and how often the record has been flattened (for tests).
  uint32_t pending_ops() const { return static_cast<uint32_t>(ops_.count()); }
  uint32_t flattened_count() const { return flattened_; }

  // --- State
  CanvasState& state() { return state_; }
  const CanvasState& state() const { return state_; }
  void save();
  void restore();
  void reset();                       // the context's reset(): blank, default state, no path
  void set_transform(const Transform& m);
  void transform_by(const Transform& m) { state_.ctm = transform_times(state_.ctm, m); }

  // --- Paths (the current path takes the current transform)
  // The current path, ready to be added to (points go through the current transform).
  Path& path() {
    path_.set_transform(state_.ctm);
    return path_;
  }
  void begin_path();
  bool fill_path(const Path* path, bool even_odd);       // nullptr: the current path
  bool stroke_path(const Path* path);
  bool clip_path(const Path* path, bool even_odd);
  bool in_path(const Path* path, double x, double y, bool even_odd);
  bool in_stroke(const Path* path, double x, double y);

  // --- Rectangles
  bool fill_rect(double x, double y, double w, double h);
  bool stroke_rect(double x, double y, double w, double h);
  bool clear_rect(double x, double y, double w, double h);

  // --- Text (the 8x16 screen font, scaled to the font size)
  bool fill_text(StringView text, double x, double y, double max_width);
  bool stroke_text(StringView text, double x, double y, double max_width);
  double measure_text(StringView text) const;

  // --- Pictures and pixels
  bool draw_image(const ImageSource& source, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh);
  // Reads w x h pixels at (x, y) (outside the canvas: transparent black) as 0xAARRGGBB straight alpha.
  bool get_image_data(int32_t x, int32_t y, uint32_t w, uint32_t h, ArrayView<uint32_t> out);
  // Writes pixels (not affected by the transform, clip or alpha); the dirty rectangle is within the picture.
  bool put_image_data(const ImageSource& source, int32_t dx, int32_t dy, int32_t dirty_x, int32_t dirty_y, int32_t dirty_w, int32_t dirty_h);
  // The pixels as they are now (flattens first).
  ImageSource snapshot();
  // A picture held for createPattern(); returns its id (0 if memory ran out).
  uint32_t add_pattern(const ImageSource& source);

  // --- Showing it
  // Draws the canvas with its top left at (x, y) in `dest`, w x h pixels (scaled if that is not its size), clipped to `clip` (absolute, like x and y).
  // Afterwards the renderer's clip is `clip` again.
  void paint(graphics::IRenderer2D& dest, int32_t x, int32_t y, uint32_t w, uint32_t h, const Rect2D& clip);

 private:
  struct Op {
    uint8_t kind = 0;                    // 0 rectangle, 1 picture
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
    uint32_t color = 0;                  // rectangle
    uint32_t image = 0;                  // picture: index in op_images_
    int32_t cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
  };
  struct Block {
    ArrayView<uint32_t> pixels;
    uint32_t width = 0, height = 0;
  };
  using Span = CanvasSpan;
  struct Mask {
    ArrayView<uint8_t> bytes;
  };

  bool record_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
  bool record_image(int32_t x, int32_t y, const Block& block);
  void flatten();
  void replay(graphics::IRenderer2D& r, int32_t ox, int32_t oy, const Rect2D& view);
  void clear_all();
  bool fill_polygons(const List<CanvasPoint>& points, const List<Path::Sub>& subs, bool even_odd, const CanvasPaint& paint, bool use_shadow);
  bool emit_spans(const List<Span>& spans, const CanvasPaint& paint, const Transform& paint_space, bool use_shadow);
  bool emit_solid(const List<Span>& spans, int32_t dx, int32_t dy, uint32_t color);
  bool cpu_spans(const List<Span>& spans, const CanvasPaint& paint, const Transform& inverse, int32_t dx, int32_t dy, double alpha, Composite op);
  bool stroke_polygons(const List<CanvasPoint>& points, const List<Path::Sub>& subs, List<CanvasPoint>* out_points, List<Path::Sub>* out_subs);
  bool text_mask(StringView text, bool bold, Block* mask);
  bool sample_picture(const ImageSource& src, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh, Block* block, int32_t* ox, int32_t* oy);
  bool place_block(Block* block, int32_t ox, int32_t oy, double alpha, bool use_shadow);
  bool clip_block(const Block* block, int32_t* ox, int32_t* oy, Block* out);
  void mix_block(const Block& block, int32_t ox, int32_t oy);
  bool draw_text(StringView text, double x, double y, double max_width, bool stroke);
  uint32_t paint_color(const CanvasPaint& p, const Transform& inverse, double x, double y) const;
  uint32_t solid_for(const CanvasPaint& p, double alpha) const;
  bool paint_is_solid(const CanvasPaint& p) const { return p.kind == CanvasPaint::Kind::Solid; }
  bool gpu_eligible(const CanvasPaint& p, double alpha) const;
  bool make_block(uint32_t w, uint32_t h, Block* block);
  void release_block(Block* block);
  void release_blocks(List<Block>& blocks);
  bool ensure_base();
  bool new_mask_from(const List<Span>& spans, uint32_t* id);
  uint8_t mask_at(uint32_t id, int32_t x, int32_t y) const;
  void touch() { ++version_; }

  allocators::IAllocator& allocator_;
  uint32_t width_, height_;
  bool valid_ = false;
  ArrayView<uint32_t> base_;            // the flattened pixels
  bool base_empty_ = true;              // every base pixel is transparent
  List<Op> ops_;
  List<Block> op_images_;
  List<Block> patterns_;
  List<Mask> masks_;
  uint64_t mask_bytes_ = 0;
  CanvasState state_;
  List<CanvasState> saved_;
  Path path_;
  uint64_t version_ = 1;
  uint32_t flattened_ = 0;
  // the scaled copy shown when the canvas is displayed at another size
  ArrayView<uint32_t> scaled_;
  uint32_t scaled_w_ = 0, scaled_h_ = 0;
  uint64_t scaled_version_ = 0;
};

}  // namespace browser
