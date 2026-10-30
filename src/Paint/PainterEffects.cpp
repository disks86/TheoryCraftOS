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

// The display painter's drawing loop and everything it draws beyond plain rectangles and glyphs: transformed and filtered groups (drawn into an offscreen
// picture with the software renderer and put back through a resampler), blurred and inset shadows, clip-path masks, conic / repeating / hinted gradients
// (pixel by pixel), borders with their line styles and mitred joins, text decorations and url() pictures.
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Paint/DisplayPainter.hpp"
#include "Paint/Raster.hpp"
#include "Paint/Tiling.hpp"

namespace paint {

namespace {

constexpr uint32_t kMaxGroupDepth = 6;
constexpr double kPi = 3.14159265358979323846;

graphics::Rect2D to_rect(const IRect& r) {
  graphics::Rect2D out;
  out.x = r.x;
  out.y = r.y;
  out.width = r.width > 0 ? static_cast<uint32_t>(r.width) : 0;
  out.height = r.height > 0 ? static_cast<uint32_t>(r.height) : 0;
  return out;
}

bool has_radius(const graphics::CornerRadii& r) {
  return r.tl_x > 0 || r.tl_y > 0 || r.tr_x > 0 || r.tr_y > 0 || r.br_x > 0 || r.br_y > 0 || r.bl_x > 0 || r.bl_y > 0;
}

IRect grown(const IRect& r, int32_t by) {
  IRect out = r;
  out.x -= by;
  out.y -= by;
  out.width += 2 * by;
  out.height += 2 * by;
  return out;
}

IRect shifted(const IRect& r, int32_t dx, int32_t dy) {
  IRect out = r;
  out.x += dx;
  out.y += dy;
  return out;
}

float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
float fsqrt(float v) { return v > 0.0f ? static_cast<float>(fmath::sqrt(static_cast<double>(v))) : 0.0f; }
int32_t ifloor(float v) {
  if (!(v > -1e9f)) return -1000000000;
  if (v > 1e9f) return 1000000000;
  const int32_t i = static_cast<int32_t>(v);
  return static_cast<float>(i) > v ? i - 1 : i;
}
int32_t iround(float v) { return ifloor(v + 0.5f); }
uint8_t to_byte(float v) { return static_cast<uint8_t>(v <= 0.0f ? 0 : v >= 255.0f ? 255 : static_cast<int32_t>(v + 0.5f)); }

uint32_t with_alpha_scaled(uint32_t color, float k) {
  const uint32_t a = to_byte(static_cast<float>(color >> 24) * clamp01(k));
  return (color & 0x00FFFFFFu) | (a << 24);
}

// ---- gradients, pixel by pixel ------------------------------------------------------------------------------------

float fast_atan2(float y, float x) {   // radians, about 1e-4 accurate
  const float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
  const float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
  if (mx == 0.0f) return 0.0f;
  const float a = mn / mx, s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = 3.14159274f - r;
  return y < 0 ? -r : r;
}

float fract(float v) { return v - static_cast<float>(ifloor(v)); }

uint32_t gradient_color(const GradientData& g, float t) {
  const uint32_t n = g.stop_count;
  if (n == 0) return 0;
  if (t <= g.stops[0].offset || n == 1) return g.stops[0].color;
  if (t >= g.stops[n - 1].offset) return g.stops[n - 1].color;
  uint32_t i = 0;
  while (i + 2 < n && t >= g.stops[i + 1].offset) ++i;
  const float o0 = g.stops[i].offset, o1 = g.stops[i + 1].offset;
  if (o1 <= o0) return g.stops[i + 1].color;
  float u = (t - o0) / (o1 - o0);
  const float h = g.hints[i];
  if (h > 0.0f && h < 1.0f && (h < 0.4999f || h > 0.5001f)) u = static_cast<float>(fmath::pow(static_cast<double>(u), fmath::ln(0.5) / fmath::ln(static_cast<double>(h))));
  const uint32_t c0 = g.stops[i].color, c1 = g.stops[i + 1].color;
  const float a0 = static_cast<float>(c0 >> 24), a1 = static_cast<float>(c1 >> 24);
  const float a = a0 + (a1 - a0) * u;
  if (a <= 0.0f) return 0;
  float rgb[3];
  for (int k = 0; k < 3; ++k) {   // premultiplied interpolation
    const int shift = 16 - 8 * k;
    const float p0 = static_cast<float>((c0 >> shift) & 0xFF) * a0, p1 = static_cast<float>((c1 >> shift) & 0xFF) * a1;
    rgb[k] = (p0 + (p1 - p0) * u) / a;
  }
  return (static_cast<uint32_t>(to_byte(a)) << 24) | (static_cast<uint32_t>(to_byte(rgb[0])) << 16) | (static_cast<uint32_t>(to_byte(rgb[1])) << 8) | to_byte(rgb[2]);
}

// ---- borders -----------------------------------------------------------------------------------------------------------

uint32_t darker(uint32_t c) {
  auto ch = [&](uint32_t v) { return static_cast<uint32_t>(to_byte(static_cast<float>(v) * 0.6f)); };
  return (c & 0xFF000000u) | (ch((c >> 16) & 0xFF) << 16) | (ch((c >> 8) & 0xFF) << 8) | ch(c & 0xFF);
}
uint32_t lighter(uint32_t c) {
  auto ch = [&](uint32_t v) { return static_cast<uint32_t>(to_byte(static_cast<float>(v) + (255.0f - static_cast<float>(v)) * 0.35f)); };
  return (c & 0xFF000000u) | (ch((c >> 16) & 0xFF) << 16) | (ch((c >> 8) & 0xFF) << 8) | ch(c & 0xFF);
}

struct BorderGeometry {
  float width = 0, height = 0;
  RRect outer, inner;
  bool inner_empty = false;
  float side_width[4] = {0, 0, 0, 0};
  uint32_t colors[4] = {0, 0, 0, 0};
  LineStyle styles[4] = {LineStyle::None, LineStyle::None, LineStyle::None, LineStyle::None};
};

float band(float q, float lo, float hi) { return clamp01(q - lo + 0.5f) * clamp01(hi - q + 0.5f); }

// The colour of the border at a point as side `side` draws it: the line style's pattern is folded into the alpha.
uint32_t side_color(const BorderGeometry& g, int side, float cx, float cy, float q) {
  const float t = g.side_width[side];
  const LineStyle style = g.styles[side];
  uint32_t color = g.colors[side];
  if (t <= 0.0f || (color >> 24) == 0 || style == LineStyle::None || style == LineStyle::Hidden) return 0;
  const bool horizontal = side == 0 || side == 2;
  const float s = horizontal ? cx : cy;
  const float length = horizontal ? g.width : g.height;
  float cover = 1.0f;
  const bool top_left = side == 0 || side == 3;
  switch (style) {
    case LineStyle::Dotted: {
      if (t <= 2.0f) {   // (too small to be round: square dots, one on and one off)
        const float period = 2.0f * t;
        const float local = s - period * static_cast<float>(ifloor(s / period));
        cover = clamp01((local < t - local ? local : t - local) + 0.5f);
      } else {
        int32_t n = iround((length - t) / (2.0f * t)) + 1;
        if (n < 2) n = 2;
        const float step = (length - t) / static_cast<float>(n - 1);
        int32_t k = iround((s - t / 2.0f) / step);
        if (k < 0) k = 0;
        if (k > n - 1) k = n - 1;
        const float ddx = s - (t / 2.0f + static_cast<float>(k) * step), ddy = q - t / 2.0f;
        cover = clamp01(t / 2.0f - fsqrt(ddx * ddx + ddy * ddy) + 0.5f);
      }
      break;
    }
    case LineStyle::Dashed: {
      const float dash = 3.0f * t, gap = 2.0f * t;
      if (length > dash + gap) {
        int32_t n = iround((length + gap) / (dash + gap));
        if (n < 2) n = 2;
        const float step = (length - dash) / static_cast<float>(n - 1);
        int32_t k = ifloor(s / step);
        if (k > n - 1) k = n - 1;
        cover = 0.0f;
        for (int32_t kk = k - 1; kk <= k; ++kk) {
          if (kk < 0) continue;
          const float lo = static_cast<float>(kk) * step, hi = lo + dash;
          const float c = clamp01((s - lo < hi - s ? s - lo : hi - s) + 0.5f);
          if (c > cover) cover = c;
        }
      }
      break;
    }
    case LineStyle::Double:
      if (t >= 3.0f) {
        const float a = static_cast<float>(ifloor(t / 3.0f));
        cover = band(q, 0.0f, a) + band(q, t - a, t);
        if (cover > 1.0f) cover = 1.0f;
      }
      break;
    case LineStyle::Inset: color = top_left ? darker(color) : lighter(color); break;
    case LineStyle::Outset: color = top_left ? lighter(color) : darker(color); break;
    case LineStyle::Groove:
    case LineStyle::Ridge: {
      const bool outer_half = q < t / 2.0f;
      const bool dark_outer = (style == LineStyle::Groove) == top_left;   // groove: the outer half of the top and left is dark; ridge: the other way
      color = (outer_half == dark_outer) ? darker(color) : lighter(color);
      break;
    }
    default: break;
  }
  return with_alpha_scaled(color, cover);
}

uint32_t mix_colors(uint32_t c1, float w1, uint32_t c2, float w2) {
  const float a1 = static_cast<float>(c1 >> 24) * w1, a2 = static_cast<float>(c2 >> 24) * w2;
  const float a = a1 + a2;
  if (a <= 0.0f) return 0;
  uint32_t out = static_cast<uint32_t>(to_byte(a)) << 24;
  for (int shift = 0; shift < 24; shift += 8) {
    const float v = (static_cast<float>((c1 >> shift) & 0xFF) * a1 + static_cast<float>((c2 >> shift) & 0xFF) * a2) / a;
    out |= static_cast<uint32_t>(to_byte(v)) << shift;
  }
  return out;
}

uint32_t border_pixel(const BorderGeometry& g, int32_t lx, int32_t ly) {
  const float co = g.outer.coverage(lx, ly);
  if (co <= 0.0f) return 0;
  const float ci = g.inner_empty ? 0.0f : g.inner.coverage(lx, ly);
  const float ring = co * (1.0f - ci);
  if (ring <= 0.003f) return 0;
  const float cx = static_cast<float>(lx) + 0.5f, cy = static_cast<float>(ly) + 0.5f;
  const bool left = cx < g.width * 0.5f, top = cy < g.height * 0.5f;
  const int vs = top ? 0 : 2, hs = left ? 3 : 1;
  const float wv = g.side_width[vs], wh = g.side_width[hs];
  if (wv <= 0.0f && wh <= 0.0f) return 0;
  const float ex = left ? cx : g.width - cx, ey = top ? cy : g.height - cy;
  float weight_v;
  if (wv <= 0.0f) weight_v = 0.0f;
  else if (wh <= 0.0f) weight_v = 1.0f;
  else weight_v = clamp01(0.5f + (ex * wv - ey * wh) / fsqrt(wv * wv + wh * wh));   // the mitre: the line from the corner to the inner corner
  const float q = g.outer.depth(cx, cy);
  uint32_t color;
  if (weight_v >= 1.0f) color = side_color(g, vs, cx, cy, q);
  else if (weight_v <= 0.0f) color = side_color(g, hs, cx, cy, q);
  else color = mix_colors(side_color(g, vs, cx, cy, q), weight_v, side_color(g, hs, cx, cy, q), 1.0f - weight_v);
  return with_alpha_scaled(color, ring);
}

}  // namespace

// ---- the drawing loop ------------------------------------------------------------------------------------------------

bool DisplayPainter::run(graphics::IRenderer2D& r, const DisplayList& list, uint64_t begin, uint64_t end, int32_t dx, int32_t dy, const IRect& view_rect, uint32_t groups) {
  bool exact = true;
  // the area still visible after the clips pushed so far (to skip what cannot show)
  IRect visible[graphics::kMaxClipDepth + graphics::kMaxLayerDepth + 2];
  uint32_t depth = 0;
  visible[0] = view_rect;
  uint8_t kinds[graphics::kMaxClipDepth + graphics::kMaxLayerDepth + 2];   // what each entry is: 0 clip, 1 layer
  uint32_t overflow_clips = 0, overflow_layers = 0;   // (pushes beyond the limit still have to be matched with a pop)
  for (uint64_t i = begin; i < end; ++i) {
    const Op& op = list.op(i);
    IRect rect = op.rect;
    rect.x += dx;
    rect.y += dy;
    switch (op.kind) {
      case OpKind::PushClip:
      case OpKind::PushClipPath: {
        const IRect cur = visible[depth];
        const bool ok_depth = depth + 1 < sizeof(kinds);
        bool fine;
        IRect narrowed = cur.intersection(rect);
        if (op.kind == OpKind::PushClipPath) {
          const ClipShape& shape = list.shape(op.data);
          narrowed = cur.intersection(shifted(shape_bounds(shape), dx, dy));
          fine = push_clip_shape(r, shape, dx, dy, narrowed);
        } else {
          fine = has_radius(op.radii) ? r.push_clip_rounded_rect(to_rect(rect), op.radii) : r.push_clip_rect(to_rect(rect));
        }
        exact = exact && fine;
        if (ok_depth) {
          ++depth;
          visible[depth] = narrowed;
          kinds[depth] = 0;
        } else {
          ++overflow_clips;
        }
        break;
      }
      case OpKind::PopClip:
        r.pop_clip();
        if (overflow_clips > 0) --overflow_clips;
        else if (depth > 0 && kinds[depth] == 0) --depth;
        break;
      case OpKind::PushLayer: {
        const IRect cur = visible[depth];
        exact = exact && r.push_layer(to_rect(rect.intersection(cur)), op.opacity);
        if (depth + 1 < sizeof(kinds)) {
          ++depth;
          visible[depth] = cur;
          kinds[depth] = 1;
        } else {
          ++overflow_layers;
        }
        break;
      }
      case OpKind::PopLayer:
        r.pop_layer();
        if (overflow_layers > 0) --overflow_layers;
        else if (depth > 0 && kinds[depth] == 1) --depth;
        break;
      case OpKind::PushTransform:
      case OpKind::PushFilter: {
        const uint64_t close = list.group_end(i);
        const uint64_t stop = close < end ? close : end;
        exact = paint_group(r, list, i, stop, dx, dy, visible[depth], groups) && exact;
        i = stop;   // (the loop's increment steps over the pop)
        break;
      }
      case OpKind::Hit:
      case OpKind::PopTransform:
      case OpKind::PopFilter: break;
      default: {
        if (!rect.intersects(visible[depth])) break;
        switch (op.kind) {
          case OpKind::FillRect:
            if (has_radius(op.radii)) r.fill_rounded_rect(to_rect(rect), op.radii, op.color);
            else r.fill_rect(to_rect(rect), op.color);
            break;
          case OpKind::Border: draw_border(r, op, rect, visible[depth]); break;
          case OpKind::Gradient: draw_gradient(r, list, op, rect, dx, dy, visible[depth]); break;
          case OpKind::Shadow: draw_shadow(r, list.shadow(op.data), rect, dx, dy, visible[depth]); break;
          case OpKind::Image: draw_image_layer(r, list, op, dx, dy, visible[depth]); break;
          case OpKind::Text: draw_text_op(r, list, op, rect, dx, dy, visible[depth]); break;
          case OpKind::Object:
            if (objects_) objects_->paint_object(r, op.node, static_cast<browser::ObjectKind>(op.object_kind), rect.x, rect.y, static_cast<uint32_t>(rect.width), static_cast<uint32_t>(rect.height),
                                                 to_rect(rect.intersection(visible[depth])));
            break;
          default: break;
        }
      }
    }
  }
  while (depth > 0) {   // (a list that is not balanced does not leave the renderer's stacks open)
    if (kinds[depth] == 0) r.pop_clip();
    else r.pop_layer();
    --depth;
  }
  return exact;
}

// ---- groups: transforms and filters ----------------------------------------------------------------------------------

bool DisplayPainter::paint_group(graphics::IRenderer2D& r, const DisplayList& list, uint64_t begin, uint64_t close, int32_t dx, int32_t dy, const IRect& clip_visible, uint32_t groups) {
  const Op& op = list.op(begin);
  const IRect g = shifted(op.rect, dx, dy);
  Affine t;
  if (op.kind == OpKind::PushTransform) {
    // (page coordinates to target coordinates: the group's matrix works on page positions, the offset is applied around it)
    t = Affine::translation(dx, dy) * list.transform(op.data) * Affine::translation(-dx, -dy);
    if (t.is_translation()) {
      const double ix = t.e < 0 ? t.e - 0.5 : t.e + 0.5, iy = t.f < 0 ? t.f - 0.5 : t.f + 0.5;
      const double tx = static_cast<double>(static_cast<int64_t>(ix > 1e8 ? 1e8 : ix < -1e8 ? -1e8 : ix)), ty = static_cast<double>(static_cast<int64_t>(iy > 1e8 ? 1e8 : iy < -1e8 ? -1e8 : iy));
      if (t.e - tx < 0.01 && tx - t.e < 0.01 && t.f - ty < 0.01 && ty - t.f < 0.01)   // a move by whole pixels: just draw the content there
        return run(r, list, begin + 1, close, dx + static_cast<int32_t>(tx), dy + static_cast<int32_t>(ty), clip_visible, groups);
    }
  }
  if (g.empty()) return true;
  if (groups >= kMaxGroupDepth) return run(r, list, begin + 1, close, dx, dy, clip_visible, groups) && false;

  if (op.kind == OpKind::PushTransform) {
    Affine inverse;
    if (!t.invert(&inverse)) return true;   // squashed flat: nothing shows
    const IRect device = transformed_bounds(t, g).intersection(clip_visible);
    if (device.empty()) return true;
    const IRect source = grown(transformed_bounds(inverse, device), 1).intersection(g);
    if (source.empty()) return true;
    // The renderer has offscreen targets (the GPU): the content is drawn there, on the GPU, and moved by its texture units. (A picture that shrinks a lot is box filtered on the CPU below instead.)
    const double scale = t.determinant() < 0 ? -t.determinant() : t.determinant();
    if (scale >= 0.25 && r.begin_group(to_rect(source))) {
      const bool drawn = run(r, list, begin + 1, close, dx, dy, source, groups + 1);
      graphics::Matrix2D m;
      m.a = static_cast<float>(t.a);
      m.b = static_cast<float>(t.b);
      m.c = static_cast<float>(t.c);
      m.d = static_cast<float>(t.d);
      m.e = static_cast<float>(t.e);
      m.f = static_cast<float>(t.f);
      return r.end_group(m, 255, graphics::ImageFilter::Bilinear) && drawn;
    }
    Pixmap content(allocator_);
    if (!content.create(source.width, source.height)) return run(r, list, begin + 1, close, dx, dy, clip_visible, groups) && false;
    {
      graphics::SoftwareRenderer2D inner(content.surface(), nullptr, &allocator_);
      inner.set_blend(graphics::BlendMode::Alpha);
      run(inner, list, begin + 1, close, dx - source.x, dy - source.y, IRect{0, 0, source.width, source.height}, groups + 1);
    }
    if (r.accelerated()) {
      // the GPU's texture units move the picture (bilinear); a picture that shrinks a lot is box filtered on the CPU below instead
      const double scale2 = t.determinant() < 0 ? -t.determinant() : t.determinant();
      if (scale2 >= 0.25) {
        graphics::Matrix2D m;
        m.a = static_cast<float>(t.a);
        m.b = static_cast<float>(t.b);
        m.c = static_cast<float>(t.c);
        m.d = static_cast<float>(t.d);
        m.e = static_cast<float>(t.x_of(source.x, source.y));
        m.f = static_cast<float>(t.y_of(source.x, source.y));
        if (r.draw_image_transformed(content.surface(), graphics::Rect2D{0, 0, static_cast<uint32_t>(source.width), static_cast<uint32_t>(source.height)}, m, graphics::ImageFilter::Bilinear))
          return true;
      }
    }
    Pixmap moved(allocator_);
    if (!resample(content, source.x, source.y, t, device, &moved)) return false;
    r.draw_image(moved.surface(), graphics::Rect2D{0, 0, static_cast<uint32_t>(device.width), static_cast<uint32_t>(device.height)}, device.x, device.y);
    return true;
  }

  // a filter chain: what it needs around the visible part (blurs and shadows read their surroundings)
  const FilterData& chain = list.filter(op.data);
  int32_t reach = 0;
  for (uint32_t i = 0; i < chain.count && i < kMaxFilterSteps; ++i) {
    const FilterStep& step = chain.steps[i];
    if (step.kind == FilterStep::Blur) {
      reach += blur_reach(step.amount);
    } else if (step.kind == FilterStep::DropShadow) {
      const float ox = step.dx < 0 ? -step.dx : step.dx, oy = step.dy < 0 ? -step.dy : step.dy;
      reach += static_cast<int32_t>(ox > oy ? ox : oy) + blur_reach(step.blur / 2.0f) + 1;
    }
  }
  const IRect visible = g.intersection(clip_visible);
  if (visible.empty()) return true;
  // The GPU does a chain of blurs and colour functions on the picture of the content (a drop shadow stays on the CPU).
  bool on_gpu = r.supports_group_filters();
  for (uint32_t i = 0; i < chain.count && i < kMaxFilterSteps; ++i) on_gpu = on_gpu && chain.steps[i].kind != FilterStep::DropShadow;
  if (on_gpu) {
    // (the picture is the visible part and what the blurs read around it; the clip shows only the visible part when it is laid over the target)
    const IRect area = grown(visible, reach);
    r.push_clip_rect(to_rect(visible));
    bool ok = false;
    if (r.begin_filter_group(to_rect(area))) {
      ok = run(r, list, begin + 1, close, dx, dy, area, groups + 1);
      for (uint32_t i = 0; i < chain.count && i < kMaxFilterSteps; ++i) {
        const FilterStep& step = chain.steps[i];
        graphics::ColorTransform t;
        if (step.kind == FilterStep::Blur) ok = r.group_blur(step.amount) && ok;
        else if (color_filter_transform(step.kind, step.amount, &t)) ok = r.group_color(t) && ok;
      }
      ok = r.end_group(graphics::Matrix2D(), 255, graphics::ImageFilter::Nearest) && ok;
      r.pop_clip();
      return ok;
    }
    r.pop_clip();
  }
  const IRect source = grown(visible, reach).intersection(g);
  Pixmap content(allocator_);
  if (!content.create(source.width, source.height)) return run(r, list, begin + 1, close, dx, dy, clip_visible, groups) && false;
  {
    graphics::SoftwareRenderer2D inner(content.surface(), nullptr, &allocator_);
    inner.set_blend(graphics::BlendMode::Alpha);
    run(inner, list, begin + 1, close, dx - source.x, dy - source.y, IRect{0, 0, source.width, source.height}, groups + 1);
  }
  bool ok = true;
  for (uint32_t i = 0; i < chain.count && i < kMaxFilterSteps && ok; ++i) {
    const FilterStep& step = chain.steps[i];
    switch (step.kind) {
      case FilterStep::Blur: ok = blur_pixmap(content, step.amount); break;
      case FilterStep::DropShadow: ok = drop_shadow(content, iround(step.dx), iround(step.dy), step.blur / 2.0f, step.color); break;
      default: color_filter(content, step.kind, step.amount); break;
    }
  }
  if (!ok) return false;
  r.draw_image(content.surface(), graphics::Rect2D{visible.x - source.x, visible.y - source.y, static_cast<uint32_t>(visible.width), static_cast<uint32_t>(visible.height)}, visible.x,
               visible.y);
  return true;
}

bool DisplayPainter::push_clip_shape(graphics::IRenderer2D& r, const ClipShape& shape, int32_t dx, int32_t dy, const IRect& area) {
  if (area.empty()) {
    r.push_clip_rect(graphics::Rect2D{0, 0, 0, 0});   // (nothing of the shape is on screen: nothing shows)
    return true;
  }
  AlphaMask mask(allocator_);
  if (!mask.create(area.width, area.height)) {
    r.push_clip_rect(to_rect(area));
    return false;
  }
  if (shape.polygon) {
    graphics::Vec2 points[kMaxClipPoints];
    const uint32_t n = shape.point_count < kMaxClipPoints ? shape.point_count : kMaxClipPoints;
    for (uint32_t i = 0; i < n; ++i) {
      points[i].x = shape.points[i].x + static_cast<float>(dx);
      points[i].y = shape.points[i].y + static_cast<float>(dy);
    }
    fill_polygon(mask, area.x, area.y, ArrayView<const graphics::Vec2>(points, n), shape.evenodd);
  } else {
    fill_rrect(mask, area.x, area.y, RRect::make(shape.x + static_cast<float>(dx), shape.y + static_cast<float>(dy), shape.width, shape.height, shape.radii));
  }
  return r.push_clip_mask(mask.view(), area.x, area.y);
}

// ---- gradients ------------------------------------------------------------------------------------------------------

void DisplayPainter::draw_gradient(graphics::IRenderer2D& r, const DisplayList& list, const Op& op, const IRect& rect, int32_t dx, int32_t dy, const IRect& visible) {
  const GradientData& g = list.gradient(op.gradient);
  const bool rounded = has_radius(op.radii);
  const graphics::Vec2 shift{static_cast<float>(dx), static_cast<float>(dy)};
  const graphics::Vec2 from{g.from.x + shift.x, g.from.y + shift.y}, to{g.to.x + shift.x, g.to.y + shift.y}, center{g.center.x + shift.x, g.center.y + shift.y};
  bool hinted = false;
  for (uint32_t i = 0; i + 1 < g.stop_count; ++i) hinted = hinted || g.hints[i] > 0.0f;
  // A repeating gradient is a plain one when the periods that show are written out as stops: then the renderer (the GPU) draws it.
  graphics::GradientStop unrolled[graphics::kMaxGradientStops];
  uint32_t unrolled_count = 0;
  graphics::Vec2 u_from = from, u_to = to, u_radii = g.radii;
  if (g.repeating && !hinted && g.kind != GradientData::Conic && g.stop_count >= 2) {
    const float first = g.stops[0].offset, last = g.stops[g.stop_count - 1].offset, period = last - first;
    const IRect area = rect.intersection(visible);
    if (period > 1e-4f && !area.empty()) {
      const float x0 = static_cast<float>(area.x), x1 = static_cast<float>(area.right()), y0 = static_cast<float>(area.y), y1 = static_cast<float>(area.bottom());
      float lo = 0.0f, hi = 0.0f;
      bool ok = true;
      if (g.kind == GradientData::Linear) {
        const float vx = to.x - from.x, vy = to.y - from.y, len2 = vx * vx + vy * vy;
        ok = len2 > 1e-6f;
        const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
        for (int i = 0; ok && i < 4; ++i) {
          const float tt = ((xs[i] - from.x) * vx + (ys[i] - from.y) * vy) / len2;
          if (i == 0 || tt < lo) lo = tt;
          if (i == 0 || tt > hi) hi = tt;
        }
      } else {
        const float rx = g.radii.x, ry = g.radii.y;
        ok = rx > 0.01f && ry > 0.01f && first >= -1e-4f && first <= 1e-4f;   // (offset 0 is the center: a period cannot start before it)
        const float fx = (x0 > center.x ? x0 - center.x : center.x > x1 ? center.x - x1 : 0.0f), fy = (y0 > center.y ? y0 - center.y : center.y > y1 ? center.y - y1 : 0.0f);
        const float mx = (center.x - x0 > x1 - center.x ? center.x - x0 : x1 - center.x), my = (center.y - y0 > y1 - center.y ? center.y - y0 : y1 - center.y);
        lo = fsqrt((fx / rx) * (fx / rx) + (fy / ry) * (fy / ry));
        hi = fsqrt((mx / rx) * (mx / rx) + (my / ry) * (my / ry));
      }
      if (ok && hi < 1e6f && lo > -1e6f) {
        const int32_t k0 = ifloor((lo - first) / period), k1 = ifloor((hi - first) / period);
        const int64_t periods = static_cast<int64_t>(k1) - k0 + 1;
        if (periods >= 1 && periods * g.stop_count <= graphics::kMaxGradientStops) {
          const float t0 = first + period * static_cast<float>(k0), t1 = last + period * static_cast<float>(k1), span = t1 - t0;
          if (span > 1e-6f) {
            for (int32_t k = k0; k <= k1; ++k)
              for (uint32_t i = 0; i < g.stop_count; ++i) {
                graphics::GradientStop& st = unrolled[unrolled_count++];
                st.offset = (g.stops[i].offset + period * static_cast<float>(k) - t0) / span;
                st.color = g.stops[i].color;
              }
            if (g.kind == GradientData::Linear) {
              u_to = {from.x + (to.x - from.x) * t1, from.y + (to.y - from.y) * t1};
              u_from = {from.x + (to.x - from.x) * t0, from.y + (to.y - from.y) * t0};
            } else {
              u_radii = {g.radii.x * t1, g.radii.y * t1};
            }
          }
        }
      }
    }
  }
  const bool by_pixel = g.kind == GradientData::Conic || ((g.repeating || hinted) && unrolled_count == 0);
  if (rounded) r.push_clip_rounded_rect(to_rect(rect), op.radii);
  if (!by_pixel) {
    ArrayView<const graphics::GradientStop> stops = unrolled_count ? ArrayView<const graphics::GradientStop>(unrolled, unrolled_count) : ArrayView<const graphics::GradientStop>(g.stops, g.stop_count);
    if (g.kind == GradientData::Radial) r.fill_radial_gradient(to_rect(rect), center, u_radii, stops);
    else r.fill_linear_gradient(to_rect(rect), u_from, u_to, stops);
  } else if (g.stop_count > 0) {
    const IRect area = rect.intersection(visible);
    Pixmap pixels(allocator_);
    if (!area.empty() && pixels.create(area.width, area.height)) {
      const float first = g.stops[0].offset, last = g.stops[g.stop_count - 1].offset;
      const float period = last - first;
      const float vx = to.x - from.x, vy = to.y - from.y;
      const float len2 = vx * vx + vy * vy > 1e-6f ? vx * vx + vy * vy : 1.0f;
      const float rx = g.radii.x > 0.01f ? g.radii.x : 0.01f, ry = g.radii.y > 0.01f ? g.radii.y : 0.01f;
      for (int32_t j = 0; j < area.height; ++j) {
        const float py = static_cast<float>(area.y + j) + 0.5f;
        for (int32_t i = 0; i < area.width; ++i) {
          const float px = static_cast<float>(area.x + i) + 0.5f;
          float t;
          if (g.kind == GradientData::Linear) {
            t = ((px - from.x) * vx + (py - from.y) * vy) / len2;
          } else if (g.kind == GradientData::Radial) {
            const float u = (px - center.x) / rx, v = (py - center.y) / ry;
            t = fsqrt(u * u + v * v);
          } else {
            t = fract((fast_atan2(px - center.x, -(py - center.y)) - g.start_angle) / 6.2831853f);
          }
          if (g.repeating) t = period > 1e-6f ? first + period * fract((t - first) / period) : last;
          pixels.pixels()[static_cast<uint64_t>(j) * static_cast<uint32_t>(area.width) + static_cast<uint32_t>(i)] = gradient_color(g, t);
        }
      }
      r.draw_image(pixels.surface(), graphics::Rect2D{0, 0, static_cast<uint32_t>(area.width), static_cast<uint32_t>(area.height)}, area.x, area.y);
    }
  }
  if (rounded) r.pop_clip();
}

// ---- shadows --------------------------------------------------------------------------------------------------------

void DisplayPainter::draw_shadow(graphics::IRenderer2D& r, const ShadowData& sh, const IRect& bounds, int32_t dx, int32_t dy, const IRect& visible) {
  const IRect out_area = bounds.intersection(visible);
  if (out_area.empty() || (sh.color >> 24) == 0) return;
  const float sigma = sh.blur > 0.0f ? sh.blur / 2.0f : 0.0f;
  const int32_t reach = blur_reach(sigma);
  const IRect box = shifted(sh.box, dx, dy);
  if (r.supports_group_filters()) {
    // On the GPU: the shape of the shadow (or, inset, everything but it) is drawn into a filter group, blurred there, and the box is cut out of it (inset: the
    // group is laid over the target through the box's clip).
    const IRect area = grown(out_area, reach);
    const int32_t spread_px = sh.spread;
    auto grow_by = [&](float by) {   // (a square corner stays square)
      graphics::CornerRadii out = sh.radii;
      auto g = [&](float v) { return v > 0.0f ? (v + by > 0.0f ? v + by : 0.0f) : 0.0f; };
      out.tl_x = g(out.tl_x); out.tl_y = g(out.tl_y); out.tr_x = g(out.tr_x); out.tr_y = g(out.tr_y);
      out.br_x = g(out.br_x); out.br_y = g(out.br_y); out.bl_x = g(out.bl_x); out.bl_y = g(out.bl_y);
      return out;
    };
    const int32_t grow = sh.inset ? -spread_px : spread_px;
    const IRect shape{box.x + sh.dx - grow, box.y + sh.dy - grow, box.width + 2 * grow, box.height + 2 * grow};
    r.push_clip_rect(to_rect(out_area));
    if (sh.inset) r.push_clip_rounded_rect(to_rect(box), sh.radii);
    bool done = r.begin_filter_group(to_rect(area));
    if (done) {
      if (!sh.inset) {
        if (!shape.empty()) r.fill_rounded_rect(to_rect(shape), grow_by(static_cast<float>(spread_px)), sh.color);
        r.group_blur(sigma);
        r.group_cut_out(to_rect(box), sh.radii);
      } else {
        r.fill_rect(to_rect(area), sh.color);
        if (!shape.empty()) r.group_cut_out(to_rect(shape), grow_by(static_cast<float>(-spread_px)));
        r.group_blur(sigma);
      }
      done = r.end_group(graphics::Matrix2D(), 255, graphics::ImageFilter::Nearest);
    }
    if (sh.inset) r.pop_clip();
    r.pop_clip();
    if (done) return;
  }
  IRect work = grown(out_area, reach);
  if (!sh.inset) work = work.intersection(bounds);
  AlphaMask mask(allocator_);
  if (work.empty() || !mask.create(work.width, work.height)) return;
  const float spread = static_cast<float>(sh.spread);
  auto grow_radii = [&](float by) {   // (a square corner stays square)
    graphics::CornerRadii out = sh.radii;
    auto g = [&](float v) { return v > 0.0f ? (v + by > 0.0f ? v + by : 0.0f) : 0.0f; };
    out.tl_x = g(out.tl_x); out.tl_y = g(out.tl_y); out.tr_x = g(out.tr_x); out.tr_y = g(out.tr_y);
    out.br_x = g(out.br_x); out.br_y = g(out.br_y); out.bl_x = g(out.bl_x); out.bl_y = g(out.bl_y);
    return out;
  };
  const ArrayView<uint8_t> data = mask.data();
  if (!sh.inset) {
    fill_rrect(mask, work.x, work.y, RRect::make(static_cast<float>(box.x + sh.dx) - spread, static_cast<float>(box.y + sh.dy) - spread, static_cast<float>(box.width) + 2.0f * spread,
                                                 static_cast<float>(box.height) + 2.0f * spread, grow_radii(spread)));
  } else {
    fill_rrect(mask, work.x, work.y, RRect::make(static_cast<float>(box.x + sh.dx) + spread, static_cast<float>(box.y + sh.dy) + spread, static_cast<float>(box.width) - 2.0f * spread,
                                                 static_cast<float>(box.height) - 2.0f * spread, grow_radii(-spread)));
    for (uint64_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(255 - data[i]);
  }
  if (!blur_mask(mask, sigma)) return;
  // an outer shadow is cut out under the box, an inset one only shows inside it
  const RRect box_shape = RRect::make(static_cast<float>(box.x), static_cast<float>(box.y), static_cast<float>(box.width), static_cast<float>(box.height), sh.radii);
  const uint32_t pitch = static_cast<uint32_t>(work.width);
  // (inside the box, away from every corner curve, the coverage is exactly 1: no per-pixel evaluation there)
  float corner = 0.0f;
  const float radii[8] = {box_shape.tlx, box_shape.tly, box_shape.trx, box_shape.try_, box_shape.brx, box_shape.bry, box_shape.blx, box_shape.bly};
  for (const float v : radii) corner = v > corner ? v : corner;
  const int32_t margin = static_cast<int32_t>(corner) + 2;
  const IRect solid = box.width > 2 * margin && box.height > 2 * margin ? IRect{box.x + margin, box.y + margin, box.width - 2 * margin, box.height - 2 * margin} : IRect{0, 0, 0, 0};
  for (int32_t j = 0; j < work.height; ++j) {
    const int32_t y = work.y + j;
    const bool row_in_box = y >= box.y && y < box.bottom();
    const uint64_t row = static_cast<uint64_t>(j) * pitch;
    if (!row_in_box) {
      if (sh.inset) data.subview(row, pitch).fill(0);   // (an inset shadow only shows inside the box)
      continue;
    }
    const bool row_solid = y >= solid.y && y < solid.bottom();
    for (int32_t i = 0; i < work.width; ++i) {
      const int32_t x = work.x + i;
      uint8_t& m = data[row + static_cast<uint32_t>(i)];
      if (m == 0) continue;
      if (!sh.inset) {
        if (x < box.x || x >= box.right()) continue;
        if (row_solid && x >= solid.x && x < solid.right()) m = 0;
        else m = to_byte(static_cast<float>(m) * (1.0f - box_shape.coverage(x, y)));
      } else if (x < box.x || x >= box.right()) {
        m = 0;
      } else if (!(row_solid && x >= solid.x && x < solid.right())) {
        m = to_byte(static_cast<float>(m) * box_shape.coverage(x, y));
      }
    }
  }
  r.draw_mask(mask.view(), graphics::Rect2D{out_area.x - work.x, out_area.y - work.y, static_cast<uint32_t>(out_area.width), static_cast<uint32_t>(out_area.height)}, out_area.x, out_area.y,
              sh.color);
}

// ---- borders -----------------------------------------------------------------------------------------------------------

void DisplayPainter::draw_border(graphics::IRenderer2D& r, const Op& op, const IRect& rect, const IRect& visible) {
  const int32_t W = rect.width, H = rect.height;
  if (W <= 0 || H <= 0) return;
  const bool same = op.border_widths[0] == op.border_widths[1] && op.border_widths[1] == op.border_widths[2] && op.border_widths[2] == op.border_widths[3] &&
                    op.border_colors[0] == op.border_colors[1] && op.border_colors[1] == op.border_colors[2] && op.border_colors[2] == op.border_colors[3] &&
                    op.border_styles[0] == op.border_styles[1] && op.border_styles[1] == op.border_styles[2] && op.border_styles[2] == op.border_styles[3];
  if (same && op.border_styles[0] == LineStyle::Solid) {
    r.stroke_rounded_rect(to_rect(rect), op.radii, graphics::EdgeWidths::uniform(op.border_widths[0]), op.border_colors[0]);
    return;
  }
  BorderGeometry g;
  g.width = static_cast<float>(W);
  g.height = static_cast<float>(H);
  g.outer = RRect::make(0, 0, g.width, g.height, op.radii);
  int32_t w[4];
  for (int i = 0; i < 4; ++i) {
    w[i] = op.border_widths[i];
    g.side_width[i] = static_cast<float>(w[i]);
    g.colors[i] = op.border_colors[i];
    g.styles[i] = op.border_styles[i];
  }
  g.inner.x0 = g.side_width[3];
  g.inner.y0 = g.side_width[0];
  g.inner.x1 = g.width - g.side_width[1];
  g.inner.y1 = g.height - g.side_width[2];
  auto less = [](float v, float d) { return v > d ? v - d : 0.0f; };
  g.inner.tlx = less(g.outer.tlx, g.side_width[3]); g.inner.tly = less(g.outer.tly, g.side_width[0]);
  g.inner.trx = less(g.outer.trx, g.side_width[1]); g.inner.try_ = less(g.outer.try_, g.side_width[0]);
  g.inner.brx = less(g.outer.brx, g.side_width[1]); g.inner.bry = less(g.outer.bry, g.side_width[2]);
  g.inner.blx = less(g.outer.blx, g.side_width[3]); g.inner.bly = less(g.outer.bly, g.side_width[2]);
  g.inner_empty = g.inner.empty();

  auto ceil_i = [](float v) { return -ifloor(-v); };
  auto max_i = [](int32_t a, int32_t b) { return a > b ? a : b; };
  struct Piece {
    IRect area;
    int side;   // the side that owns the whole piece (a plain rectangle of one solid colour), or -1: work it out per pixel
  };
  Piece pieces[8];
  uint32_t count = 0;
  auto solid_side = [&](int side) { return g.styles[side] == LineStyle::Solid && (g.colors[side] >> 24) != 0 ? side : -1; };
  const int32_t zl_t = max_i(w[3], ceil_i(g.outer.tlx)), zr_t = max_i(w[1], ceil_i(g.outer.trx));
  const int32_t zl_b = max_i(w[3], ceil_i(g.outer.blx)), zr_b = max_i(w[1], ceil_i(g.outer.brx));
  const int32_t yl_t = max_i(w[0], ceil_i(g.outer.tly)), yr_t = max_i(w[0], ceil_i(g.outer.try_));
  const int32_t yl_b = max_i(w[2], ceil_i(g.outer.bly)), yr_b = max_i(w[2], ceil_i(g.outer.bry));
  if (zl_t + zr_t <= W && zl_b + zr_b <= W && yl_t + yl_b <= H && yr_t + yr_b <= H) {
    pieces[count++] = {IRect{0, 0, zl_t, yl_t}, -1};
    pieces[count++] = {IRect{W - zr_t, 0, zr_t, yr_t}, -1};
    pieces[count++] = {IRect{0, H - yl_b, zl_b, yl_b}, -1};
    pieces[count++] = {IRect{W - zr_b, H - yr_b, zr_b, yr_b}, -1};
    pieces[count++] = {IRect{zl_t, 0, W - zl_t - zr_t, w[0]}, solid_side(0)};
    pieces[count++] = {IRect{zl_b, H - w[2], W - zl_b - zr_b, w[2]}, solid_side(2)};
    pieces[count++] = {IRect{0, yl_t, w[3], H - yl_t - yl_b}, solid_side(3)};
    pieces[count++] = {IRect{W - w[1], yr_t, w[1], H - yr_t - yr_b}, solid_side(1)};
  } else {
    pieces[count++] = {IRect{0, 0, W, H}, -1};
  }
  // (the four middle pieces are slow ones when their side is not solid; their pattern then runs per pixel)
  for (uint32_t k = 0; k < count; ++k) {
    const Piece& p = pieces[k];
    if (p.area.empty()) continue;
    const IRect target = shifted(p.area, rect.x, rect.y).intersection(visible);
    if (target.empty()) continue;
    if (p.side >= 0) {
      r.fill_rect(to_rect(target), g.colors[p.side]);
      continue;
    }
    Pixmap pixels(allocator_);
    if (!pixels.create(target.width, target.height)) continue;
    for (int32_t j = 0; j < target.height; ++j)
      for (int32_t i = 0; i < target.width; ++i)
        pixels.pixels()[static_cast<uint64_t>(j) * static_cast<uint32_t>(target.width) + static_cast<uint32_t>(i)] = border_pixel(g, target.x - rect.x + i, target.y - rect.y + j);
    r.draw_image(pixels.surface(), graphics::Rect2D{0, 0, static_cast<uint32_t>(target.width), static_cast<uint32_t>(target.height)}, target.x, target.y);
  }
}

// ---- text ---------------------------------------------------------------------------------------------------------------

void DisplayPainter::draw_decoration(graphics::IRenderer2D& r, int32_t x, int32_t y, int32_t width, int32_t t, DecorationStyle style, uint32_t color, bool upward) {
  if (width <= 0 || t <= 0) return;
  switch (style) {
    case DecorationStyle::Double:
      r.fill_rect(to_rect(IRect{x, y, width, t}), color);
      r.fill_rect(to_rect(IRect{x, upward ? y - 2 * t : y + 2 * t, width, t}), color);
      break;
    case DecorationStyle::Dotted:
      for (int32_t xx = x; xx < x + width; xx += 2 * t) {
        const int32_t w = x + width - xx < t ? x + width - xx : t;
        if (t >= 3) r.fill_rounded_rect(to_rect(IRect{xx, y, w, t}), graphics::CornerRadii::uniform(static_cast<float>(t) / 2.0f), color);
        else r.fill_rect(to_rect(IRect{xx, y, w, t}), color);
      }
      break;
    case DecorationStyle::Dashed:
      for (int32_t xx = x; xx < x + width; xx += 5 * t) r.fill_rect(to_rect(IRect{xx, y, x + width - xx < 3 * t ? x + width - xx : 3 * t, t}), color);
      break;
    case DecorationStyle::Wavy: {
      const float amp = static_cast<float>(t < 1 ? 1 : t), period = 4.0f * amp + 4.0f;
      const int32_t rows = static_cast<int32_t>(2.0f * amp) + t + 3;
      AlphaMask mask(allocator_);
      if (!mask.create(width, rows)) break;
      for (int32_t i = 0; i < width; ++i) {
        const float phase = 6.2831853f * (static_cast<float>(i) + 0.5f) / period;
        const float yc = amp + 1.0f + amp * static_cast<float>(fmath::sin(static_cast<double>(phase)));
        const float slope = amp * 6.2831853f / period * static_cast<float>(fmath::cos(static_cast<double>(phase)));
        const float half = static_cast<float>(t) * 0.5f * fsqrt(1.0f + slope * slope);
        for (int32_t j = 0; j < rows; ++j) {
          const float top = static_cast<float>(j), bottom = top + 1.0f;
          const float lo = yc - half > top ? yc - half : top, hi = yc + half < bottom ? yc + half : bottom;
          if (hi > lo) mask.data()[static_cast<uint64_t>(j) * static_cast<uint32_t>(width) + static_cast<uint32_t>(i)] = to_byte((hi - lo) * 255.0f);
        }
      }
      r.draw_mask(mask.view(), graphics::Rect2D{0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(rows)}, x, y - static_cast<int32_t>(amp) - 1, color);
      break;
    }
    default: r.fill_rect(to_rect(IRect{x, y, width, t}), color); break;
  }
}

void DisplayPainter::draw_text_run(graphics::IRenderer2D& r, const DisplayList& list, const Op& op, const IRect& rect, int32_t dx, int32_t dy, uint32_t color) {
  draw_text(r, font_of(list, op.font), list.text(op), op.text_x64 + dx * 64, op.baseline + dy, color, op.letter_spacing64, op.word_spacing64);
  if (!op.decorations) return;
  const uint32_t line_color = op.decoration_color ? op.decoration_color : color;
  const DecorationStyle style = static_cast<DecorationStyle>(op.decoration_style);
  const int32_t t = op.decoration_thickness, baseline = op.baseline + dy;
  if (op.decorations & kDecorationUnderline) draw_decoration(r, rect.x, baseline + op.decoration_offset - t + 1, rect.width, t, style, line_color, false);
  if (op.decorations & kDecorationStrike) draw_decoration(r, rect.x, baseline - (op.strike_offset ? op.strike_offset : op.decoration_offset) - t, rect.width, t, style, line_color, false);
  if (op.decorations & kDecorationOverline) draw_decoration(r, rect.x, rect.y, rect.width, t, style, line_color, true);
}

void DisplayPainter::draw_text_op(graphics::IRenderer2D& r, const DisplayList& list, const Op& op, const IRect& rect, int32_t dx, int32_t dy, const IRect& visible) {
  if (!(op.blur > 0.0f)) {
    draw_text_run(r, list, op, rect, dx, dy, op.color);
    return;
  }
  // a blurred text-shadow: the run in white on a transparent picture, its alpha blurred, and the colour poured through it
  // (the op's rectangle is the run's grown by the blur's reach and 2 pixels)
  const float sigma = op.blur / 2.0f;
  const int32_t reach = blur_reach(sigma);
  const IRect exact = grown(rect, -(reach + 2));
  const IRect area = rect.intersection(visible);
  const IRect work = grown(area, reach).intersection(rect);
  if (area.empty() || work.empty()) return;
  Pixmap run_picture(allocator_);
  if (!run_picture.create(work.width, work.height)) return;
  {
    graphics::SoftwareRenderer2D inner(run_picture.surface(), nullptr, &allocator_);
    inner.set_blend(graphics::BlendMode::Alpha);
    draw_text_run(inner, list, op, IRect{exact.x - work.x, exact.y - work.y, exact.width, exact.height}, dx - work.x, dy - work.y, 0xFFFFFFFFu);
  }
  AlphaMask mask(allocator_);
  if (!mask.create(work.width, work.height)) return;
  for (uint64_t i = 0; i < run_picture.pixels().size(); ++i) mask.data()[i] = static_cast<uint8_t>(run_picture.pixels()[i] >> 24);
  if (!blur_mask(mask, sigma)) return;
  r.draw_mask(mask.view(), graphics::Rect2D{area.x - work.x, area.y - work.y, static_cast<uint32_t>(area.width), static_cast<uint32_t>(area.height)}, area.x, area.y, op.color);
}

// ---- pictures -----------------------------------------------------------------------------------------------------------

void DisplayPainter::draw_image_layer(graphics::IRenderer2D& r, const DisplayList& list, const Op& op, int32_t dx, int32_t dy, const IRect& visible) {
  if (!objects_) return;
  const ImageLayerData& d = list.image(op.data);
  const StringView url = list.text(op);
  const IRect origin = shifted(d.origin, dx, dy), clip = shifted(d.clip, dx, dy).intersection(visible);
  if (clip.empty() || origin.empty()) return;
  int32_t nw = 0, nh = 0;
  const bool known = objects_->background_image_size(url, &nw, &nh) && nw > 0 && nh > 0;
  if (!known && !(d.size_mode == ImageLayerData::Explicit && d.size_w >= 0 && d.size_h >= 0)) return;
  // the size of one tile
  int32_t tw = nw, th = nh;
  const float ow = static_cast<float>(origin.width), oh = static_cast<float>(origin.height);
  if (d.size_mode == ImageLayerData::Cover || d.size_mode == ImageLayerData::Contain) {
    const float sx = ow / static_cast<float>(nw), sy = oh / static_cast<float>(nh);
    const float s = (d.size_mode == ImageLayerData::Cover) == (sx > sy) ? sx : sy;
    tw = iround(static_cast<float>(nw) * s);
    th = iround(static_cast<float>(nh) * s);
  } else if (d.size_mode == ImageLayerData::Explicit) {
    if (d.size_w >= 0 && d.size_h >= 0) {
      tw = iround(d.size_w);
      th = iround(d.size_h);
    } else if (d.size_w >= 0) {
      tw = iround(d.size_w);
      th = iround(d.size_w * static_cast<float>(nh) / static_cast<float>(nw));
    } else if (d.size_h >= 0) {
      th = iround(d.size_h);
      tw = iround(d.size_h * static_cast<float>(nw) / static_cast<float>(nh));
    }
  }
  if (tw < 1) tw = 1;
  if (th < 1) th = 1;
  const TileAxis ax = lay_out_tiles(origin.x, origin.width, clip.x, clip.right(), tw, d.pos_x_percent_x100, d.pos_x_px100, d.repeat_x);
  const TileAxis ay = lay_out_tiles(origin.y, origin.height, clip.y, clip.bottom(), th, d.pos_y_percent_x100, d.pos_y_px100, d.repeat_y);
  const bool rounded = has_radius(op.radii);
  if (rounded) r.push_clip_rounded_rect(to_rect(shifted(op.rect, dx, dy)), op.radii);
  if (static_cast<int64_t>(ax.count) * ay.count > 16 && static_cast<int64_t>(tw) * th <= (1 << 20)) {
    // many tiles (a pattern): the picture is drawn once, and the visible part of the layer is made from copies of it
    Pixmap tile(allocator_), area(allocator_);
    if (tile.create(tw, th) && area.create(clip.width, clip.height)) {
      {
        graphics::SoftwareRenderer2D inner(tile.surface(), nullptr, &allocator_);
        inner.set_blend(graphics::BlendMode::Alpha);
        objects_->paint_background_image(inner, url, 0, 0, static_cast<uint32_t>(tw), static_cast<uint32_t>(th), graphics::Rect2D{0, 0, static_cast<uint32_t>(tw), static_cast<uint32_t>(th)});
      }
      for (int32_t j = 0; j < ay.count; ++j) {
        for (int32_t i = 0; i < ax.count; ++i) {
          const IRect cell{ax.first + i * ax.pitch, ay.first + j * ay.pitch, ax.size, ay.size};
          const IRect shown = cell.intersection(clip);
          if (shown.empty()) continue;
          for (int32_t y = shown.y; y < shown.bottom(); ++y) {
            for (int32_t x = shown.x; x < shown.right(); ++x) {
              // (the tile may have been stretched by `round`: pick the texel by position inside the cell)
              const int32_t sx = ax.size == tw ? x - cell.x : (x - cell.x) * tw / ax.size, sy = ay.size == th ? y - cell.y : (y - cell.y) * th / ay.size;
              uint32_t& dst = area.pixels()[static_cast<uint64_t>(y - clip.y) * static_cast<uint32_t>(clip.width) + static_cast<uint32_t>(x - clip.x)];
              dst = blend_over(tile.pixels()[static_cast<uint64_t>(sy) * static_cast<uint32_t>(tw) + static_cast<uint32_t>(sx)], dst);
            }
          }
        }
      }
      r.draw_image(area.surface(), graphics::Rect2D{0, 0, static_cast<uint32_t>(clip.width), static_cast<uint32_t>(clip.height)}, clip.x, clip.y);
    }
    if (rounded) r.pop_clip();
    return;
  }
  for (int32_t j = 0; j < ay.count; ++j) {
    for (int32_t i = 0; i < ax.count; ++i) {
      const IRect tile{ax.first + i * ax.pitch, ay.first + j * ay.pitch, ax.size, ay.size};
      const IRect shown = tile.intersection(clip);
      if (shown.empty()) continue;
      objects_->paint_background_image(r, url, tile.x, tile.y, static_cast<uint32_t>(tile.width), static_cast<uint32_t>(tile.height), to_rect(shown));
    }
  }
  if (rounded) r.pop_clip();
}

}  // namespace paint
