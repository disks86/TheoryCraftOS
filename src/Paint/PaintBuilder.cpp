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

#include "Paint/PaintBuilder.hpp"
#include "Dom/Form.hpp"
#include "Paint/Raster.hpp"
#include "Paint/Tiling.hpp"

namespace paint {

using layout::Box;
using layout::BoxId;
using layout::BoxKind;
using layout::BoxTree;
using layout::Fragment;
using layout::kNoBox;
using layout::LayoutUnit;
using browser::Style;

namespace {

constexpr uint32_t kDefaultInk = 0xFF000000u;
constexpr uint32_t kLinkInk = 0xFF0000EEu;

IRect snap(LayoutUnit x, LayoutUnit y, LayoutUnit w, LayoutUnit h) {
  IRect r;
  r.x = x.round();
  r.y = y.round();
  r.width = (x + w).round() - r.x;
  r.height = (y + h).round() - r.y;
  return r;
}

uint32_t with_alpha(uint32_t color, uint32_t alpha_scale_255) {
  const uint32_t a = ((color >> 24) * alpha_scale_255 + 127) / 255;
  return (color & 0x00FFFFFFu) | (a << 24);
}

// An ancestor between a stacking context and a context inside it that clips (overflow other than visible) but is no context itself: what it clips is painted
// by the context above, so the clip has to be put around it again.
struct ClipRef {
  BoxId id = kNoBox;
  LayoutUnit ax, ay;
  bool positioned = false;   // (a clip of a positioned box also holds absolutely positioned descendants)
};
constexpr uint32_t kMaxClipRefs = 6;
struct ClipChain {
  ClipRef refs[kMaxClipRefs];
  uint32_t count = 0;
};

struct Entry {
  BoxId id = kNoBox;
  LayoutUnit ax, ay;
  int32_t z = 0;
  uint32_t order = 0;
  ClipChain clips;
};

class Builder {
 public:
  Builder(const BoxTree& tree, const PaintOptions& options, DisplayList* out) : tree_(tree), options_(options), out_(*out), measurer_(*options.measurer) {}

  bool run() {
    out_.clear();
    const BoxId root = tree_.root();
    if (root == kNoBox) return false;
    find_page_background(root);
    paint_context(root, LayoutUnit(), LayoutUnit());
    const Box& r = tree_.box(root);
    out_.set_extent(r.width.ceil(), r.height.ceil());
    return !failed_;
  }

 private:
  allocators::IAllocator& allocator() const { return tree_.allocator(); }

  void add(const Op& op) { failed_ |= !out_.add(op); }

  // --- the page's canvas: the background of <html>, else of <body>, covers everything and is not painted again by the box that gave it
  void find_page_background(BoxId root) {
    if (!options_.doc) return;
    const Box& r = tree_.box(root);
    for (uint64_t i = 0; i < r.children.count() && html_ == kNoBox; ++i) {
      const dom::Node* n = options_.doc->by_serial(tree_.box(r.children[i]).node);
      if (n && n->is_tag("html")) html_ = r.children[i];
    }
    if (html_ == kNoBox) return;
    const Box& h = tree_.box(html_);
    if (h.style.own_background && (h.style.background_argb >> 24) != 0) {
      out_.set_background(h.style.background_argb);
      skip_background_of_ = html_;
      return;
    }
    for (uint64_t i = 0; i < h.children.count(); ++i) {
      const BoxId c = h.children[i];
      const dom::Node* n = options_.doc->by_serial(tree_.box(c).node);
      if (n && n->is_tag("body")) {
        const Box& bb = tree_.box(c);
        if (bb.style.own_background && (bb.style.background_argb >> 24) != 0) {
          out_.set_background(bb.style.background_argb);
          skip_background_of_ = c;
        }
        return;
      }
    }
  }

  // --- stacking contexts
  // Does the box make a stacking context? An integer z-index on a positioned box does; opacity below 1 and transforms do.
  static bool makes_context(const Box& b) {
    const Style& s = b.style;
    return (s.position != browser::Position::Static && !s.z_index_auto) || s.opacity_x1000 < 1000 || !s.transform_value.empty() || !s.translate.empty() || !s.rotate.empty() ||
           !s.scale.empty() || !s.filter.empty() || !s.clip_path.empty();
  }
  // Is it painted in the positioned layer of its stacking context (as a unit)? Every positioned box is, and so is every context.
  static bool is_context(const Box& b) { return makes_context(b) || b.style.position != browser::Position::Static; }

  static bool clips(const Box& b) { return b.style.overflow_x != browser::Overflow::Visible || b.style.overflow_y != browser::Overflow::Visible; }

  // Finds the stacking contexts below `id` that are not inside a float or an inline-block or another context: they are painted by this context.
  void gather(BoxId id, LayoutUnit ax, LayoutUnit ay, List<Entry>* out, const ClipChain& chain) {
    const Box& b = tree_.box(id);
    for (uint64_t i = 0; i < b.children.count(); ++i) {
      const BoxId cid = b.children[i];
      const Box& c = tree_.box(cid);
      if (c.kind == BoxKind::Text || c.kind == BoxKind::LineBreak || c.kind == BoxKind::Marker) continue;
      if (c.kind == BoxKind::Inline) {
        gather(cid, ax, ay, out, chain);
        continue;
      }
      const LayoutUnit cx = ax + c.x, cy = ay + c.y;
      // what a clip of this box holds, for the boxes inside it
      ClipChain inside = chain;
      if (clips(c) && inside.count < kMaxClipRefs) {
        inside.refs[inside.count].id = cid;
        inside.refs[inside.count].ax = cx;
        inside.refs[inside.count].ay = cy;
        inside.refs[inside.count].positioned = c.style.position != browser::Position::Static || !c.style.transform_value.empty();
        ++inside.count;
      }
      if (is_context(c)) {
        Entry e;
        e.id = cid;
        e.ax = cx;
        e.ay = cy;
        e.z = c.style.z_index;
        e.order = order_++;
        // the clips of the boxes between: they hold everything but a fixed box and an absolute one whose containing block is outside the clipping box
        if (c.style.position != browser::Position::Fixed) {
          for (uint32_t k = 0; k < chain.count; ++k)
            if (c.style.position != browser::Position::Absolute || chain.refs[k].positioned) e.clips.refs[e.clips.count++] = chain.refs[k];
        }
        failed_ |= !out->push_back(e);
        // a positioned box without a z-index does not make a stacking context: the positioned boxes inside it belong to this one, too
        if (!makes_context(c)) gather(cid, cx, cy, out, inside);
        continue;
      }
      if (c.floated || c.kind == BoxKind::InlineBlock || (c.kind == BoxKind::Replaced && c.is_inline_level())) continue;   // (painted as units, with what is in them)
      gather(cid, cx, cy, out, inside);
    }
  }

  // A context found below `clips`: painted inside them.
  void paint_entry(const Entry& e) {
    for (uint32_t k = 0; k < e.clips.count; ++k) push_clip(tree_.box(e.clips.refs[k].id), e.clips.refs[k].ax, e.clips.refs[k].ay);
    paint_context(e.id, e.ax, e.ay);
    for (uint32_t k = 0; k < e.clips.count; ++k) pop_clip();
  }

  void paint_context(BoxId id, LayoutUnit ax, LayoutUnit ay) {
    const Box& b = tree_.box(id);
    if (b.style.opaque_zero || depth_ > 120) return;
    ++depth_;
    const Effects effects = open_effects(b, ax, ay);   // transform, clip-path and filter: the groups around the box and what is in it
    const bool layer = b.style.opacity_x1000 < 1000 && b.kind != BoxKind::Root;
    if (layer) {
      Op op;
      op.kind = OpKind::PushLayer;
      op.rect = snap(ax - LayoutUnit::from_px(64), ay - LayoutUnit::from_px(64), b.width + LayoutUnit::from_px(128), b.height + LayoutUnit::from_px(128));
      op.opacity = static_cast<uint8_t>((b.style.opacity_x1000 * 255 + 500) / 1000);
      add(op);
    }
    paint_box(id, ax, ay);
    const bool clipped = b.kind != BoxKind::Root && push_clip(b, ax, ay);

    List<Entry> contexts(allocator());
    if (b.kind == BoxKind::Root || makes_context(b) || b.floated || b.kind == BoxKind::InlineBlock || b.kind == BoxKind::Replaced) gather(id, ax, ay, &contexts, ClipChain());   // (otherwise the context above paints them)
    // order: z-index (stable, so tree order among equals)
    for (uint64_t i = 1; i < contexts.count(); ++i) {
      for (uint64_t j = i; j > 0 && contexts[j - 1].z > contexts[j].z; --j) {
        Entry t = contexts[j];
        contexts[j] = contexts[j - 1];
        contexts[j - 1] = t;
      }
    }
    for (uint64_t i = 0; i < contexts.count(); ++i)
      if (contexts[i].z < 0) paint_entry(contexts[i]);
    walk(id, ax, ay, 0);   // the backgrounds and borders of blocks
    walk(id, ax, ay, 1);   // floats
    walk(id, ax, ay, 2);   // inline content
    for (uint64_t i = 0; i < contexts.count(); ++i)
      if (contexts[i].z >= 0) paint_entry(contexts[i]);

    if (clipped) pop_clip();
    paint_outline(id, ax, ay);
    if (layer) {
      Op op;
      op.kind = OpKind::PopLayer;
      add(op);
    }
    close_effects(effects);
    --depth_;
  }

  // --- transforms, filters and clip-path: groups around a stacking context
  static constexpr uint64_t kNoOp = ~uint64_t{0};
  struct Effects {
    uint64_t transform_op = kNoOp, filter_op = kNoOp;
    bool clip_path = false;
    int32_t filter_grow = 0;
  };

  static IRect union_of(const IRect& a, const IRect& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    IRect r;
    r.x = a.x < b.x ? a.x : b.x;
    r.y = a.y < b.y ? a.y : b.y;
    r.width = (a.right() > b.right() ? a.right() : b.right()) - r.x;
    r.height = (a.bottom() > b.bottom() ? a.bottom() : b.bottom()) - r.y;
    return r;
  }

  // What the operations from `begin` on can draw (a transform inside counts as its transformed bounds).
  IRect content_bounds(uint64_t begin) const {
    IRect bounds;
    const uint64_t end = out_.count();
    for (uint64_t i = begin; i < end; ++i) {
      const Op& op = out_.op(i);
      switch (op.kind) {
        case OpKind::PushTransform: {
          bounds = union_of(bounds, transformed_bounds(out_.transform(op.data), op.rect));
          const uint64_t close = out_.group_end(i);
          i = close < end ? close : end;
          break;
        }
        case OpKind::PushFilter:
        case OpKind::FillRect:
        case OpKind::Border:
        case OpKind::Gradient:
        case OpKind::Text:
        case OpKind::Object:
        case OpKind::Shadow:
        case OpKind::Image: bounds = union_of(bounds, op.rect); break;
        default: break;
      }
    }
    return bounds;
  }

  static IRect grow_rect(const IRect& r, int32_t by) {
    IRect out = r;
    out.x -= by;
    out.y -= by;
    out.width += 2 * by;
    out.height += 2 * by;
    return out;
  }

  Effects open_effects(const Box& b, LayoutUnit ax, LayoutUnit ay) {
    Effects e;
    if (b.kind == BoxKind::Root || !options_.sheet) return e;
    const Style& s = b.style;
    const double w = static_cast<double>(b.width.to_px100()) / 100.0, h = static_cast<double>(b.height.to_px100()) / 100.0;
    const double px = static_cast<double>(ax.to_px100()) / 100.0, py = static_cast<double>(ay.to_px100()) / 100.0;
    if (!s.transform_value.empty() || !s.translate.empty() || !s.rotate.empty() || !s.scale.empty()) {
      double m[16];
      if (options_.sheet->transform_matrix(s, w, h, m, true)) {
        Affine local;   // (the 3D parts are flattened: the matrix's x and y columns and its translation)
        local.a = m[0];
        local.b = m[1];
        local.c = m[4];
        local.d = m[5];
        local.e = m[12];
        local.f = m[13];
        const Affine page = Affine::translation(px, py) * local * Affine::translation(-px, -py);
        Op op;
        op.kind = OpKind::PushTransform;
        failed_ |= !out_.add_transform(page, &op.data);
        e.transform_op = out_.count();
        add(op);
      }
    }
    if (!s.clip_path.empty()) {
      browser::ClipPath cp;
      ClipShape shape;
      if (options_.sheet->clip_path(s, &cp) && build_shape(b, cp, px, py, w, h, &shape)) {
        Op op;
        op.kind = OpKind::PushClipPath;
        op.rect = shape_bounds(shape);
        failed_ |= !out_.add_shape(shape, &op.data);
        add(op);
        e.clip_path = true;
      }
    }
    if (!s.filter.empty()) {
      List<browser::FilterFunction> fns(allocator());
      FilterData data;
      if (options_.sheet->filters(s.filter, s, &fns)) {
        for (uint64_t i = 0; i < fns.count() && data.count < kMaxFilterSteps; ++i) {
          const browser::FilterFunction& f = fns[i];
          if (f.kind == browser::FilterFunction::Url) continue;
          FilterStep step;
          step.kind = static_cast<FilterStep::Kind>(f.kind);
          step.amount = static_cast<float>(f.amount);
          if (f.kind == browser::FilterFunction::Blur) {
            if (step.amount <= 0.0f) continue;
            e.filter_grow += blur_reach(step.amount) + 1;
          } else if (f.kind == browser::FilterFunction::DropShadow) {
            step.dx = static_cast<float>(f.shadow.x_px100) / 100.0f;
            step.dy = static_cast<float>(f.shadow.y_px100) / 100.0f;
            step.blur = static_cast<float>(f.shadow.blur_px100 > 0 ? f.shadow.blur_px100 : 0) / 100.0f;
            step.color = f.shadow.current_color ? ink_of(s) : f.shadow.color;
            const float ox = step.dx < 0 ? -step.dx : step.dx, oy = step.dy < 0 ? -step.dy : step.dy;
            e.filter_grow += static_cast<int32_t>(ox > oy ? ox : oy) + blur_reach(step.blur / 2.0f) + 2;
          }
          data.steps[data.count++] = step;
        }
      }
      if (data.count > 0) {
        Op op;
        op.kind = OpKind::PushFilter;
        failed_ |= !out_.add_filter(data, &op.data);
        e.filter_op = out_.count();
        add(op);
      }
    }
    return e;
  }

  void close_effects(const Effects& e) {
    if (e.filter_op != kNoOp) {
      out_.mutable_op(e.filter_op).rect = grow_rect(content_bounds(e.filter_op + 1), e.filter_grow);
      Op op;
      op.kind = OpKind::PopFilter;
      add(op);
    }
    if (e.clip_path) pop_clip();
    if (e.transform_op != kNoOp) {
      out_.mutable_op(e.transform_op).rect = content_bounds(e.transform_op + 1);
      Op op;
      op.kind = OpKind::PopTransform;
      add(op);
    }
  }

  // The clip-path shape in page coordinates (the reference box defaults to the border box). False for the shapes that are not supported (path(), url()).
  bool build_shape(const Box& b, const browser::ClipPath& cp, double px, double py, double w, double h, ClipShape* out) const {
    using browser::ClipPath;
    if (cp.kind == ClipPath::None || cp.kind == ClipPath::Path || cp.kind == ClipPath::Url) return false;
    double rx = px, ry = py, rw = w, rh = h;   // the reference box
    if (cp.reference_box == browser::BackgroundBox::PaddingBox || cp.reference_box == browser::BackgroundBox::ContentBox) {
      rx += static_cast<double>(b.border.left.to_px100()) / 100.0;
      ry += static_cast<double>(b.border.top.to_px100()) / 100.0;
      rw -= static_cast<double>(b.border.horizontal().to_px100()) / 100.0;
      rh -= static_cast<double>(b.border.vertical().to_px100()) / 100.0;
      if (cp.reference_box == browser::BackgroundBox::ContentBox) {
        rx = px + static_cast<double>(b.content_left().to_px100()) / 100.0;
        ry = py + static_cast<double>(b.content_top().to_px100()) / 100.0;
        rw = static_cast<double>(b.content_width().to_px100()) / 100.0;
        rh = static_cast<double>(b.content_height().to_px100()) / 100.0;
      }
    }
    if (rw < 0) rw = 0;
    if (rh < 0) rh = 0;
    const int32_t w100 = static_cast<int32_t>(rw * 100), h100 = static_cast<int32_t>(rh * 100);
    auto fx = [&](const browser::Dimension& d) { return static_cast<float>(d.resolve(w100)) / 100.0f; };
    auto fy = [&](const browser::Dimension& d) { return static_cast<float>(d.resolve(h100)) / 100.0f; };
    *out = ClipShape();
    const float frx = static_cast<float>(rx), fry = static_cast<float>(ry), frw = static_cast<float>(rw), frh = static_cast<float>(rh);
    switch (cp.kind) {
      case ClipPath::Box:
        out->x = frx; out->y = fry; out->width = frw; out->height = frh;
        out->radii = radii_of(b);
        return true;
      case ClipPath::Inset: {
        const float top = fy(cp.inset[0]), right = fx(cp.inset[1]), bottom = fy(cp.inset[2]), left = fx(cp.inset[3]);
        out->x = frx + left;
        out->y = fry + top;
        out->width = frw - left - right;
        out->height = frh - top - bottom;
        if (out->width < 0) out->width = 0;
        if (out->height < 0) out->height = 0;
        graphics::CornerRadii& r = out->radii;
        r.tl_x = fx(cp.round[0].h); r.tl_y = fy(cp.round[0].v);
        r.tr_x = fx(cp.round[1].h); r.tr_y = fy(cp.round[1].v);
        r.br_x = fx(cp.round[2].h); r.br_y = fy(cp.round[2].v);
        r.bl_x = fx(cp.round[3].h); r.bl_y = fy(cp.round[3].v);
        return true;
      }
      case ClipPath::Circle:
      case ClipPath::Ellipse: {
        const float cx = frx + static_cast<float>(cp.center_x.resolve(w100)) / 100.0f, cy = fry + static_cast<float>(cp.center_y.resolve(h100)) / 100.0f;
        const float to_left = cx - frx, to_right = frx + frw - cx, to_top = cy - fry, to_bottom = fry + frh - cy;
        const float near_x = to_left < to_right ? to_left : to_right, near_y = to_top < to_bottom ? to_top : to_bottom;
        const float far_x = to_left > to_right ? to_left : to_right, far_y = to_top > to_bottom ? to_top : to_bottom;
        float radius_x, radius_y;
        if (cp.kind == ClipPath::Circle) {
          float r;
          if (cp.radius_x.is_auto()) {
            r = cp.radius_far ? (far_x > far_y ? far_x : far_y) : (near_x < near_y ? near_x : near_y);
          } else if (cp.radius_x.kind == browser::Dimension::Percent) {   // (a percentage of sqrt((w^2 + h^2) / 2))
            const float norm = static_cast<float>(fmath::sqrt(static_cast<double>(frw * frw + frh * frh) / 2.0));
            r = norm * static_cast<float>(cp.radius_x.x100) / 10000.0f;
          } else {
            r = static_cast<float>(cp.radius_x.x100) / 100.0f;
          }
          radius_x = radius_y = r;
        } else {
          radius_x = cp.radius_x.is_auto() ? (cp.radius_far ? far_x : near_x) : fx(cp.radius_x);
          radius_y = cp.radius_y.is_auto() ? (cp.radius_far ? far_y : near_y) : fy(cp.radius_y);
        }
        if (radius_x < 0) radius_x = 0;
        if (radius_y < 0) radius_y = 0;
        out->x = cx - radius_x;
        out->y = cy - radius_y;
        out->width = 2 * radius_x;
        out->height = 2 * radius_y;
        out->radii = graphics::CornerRadii{radius_x, radius_y, radius_x, radius_y, radius_x, radius_y, radius_x, radius_y};
        return true;
      }
      case ClipPath::Polygon:
        if (cp.point_count < 3) return false;
        out->polygon = true;
        out->evenodd = cp.evenodd;
        out->point_count = cp.point_count < kMaxClipPoints ? cp.point_count : kMaxClipPoints;
        for (uint32_t i = 0; i < out->point_count; ++i) {
          out->points[i].x = frx + fx(cp.points_x[i]);
          out->points[i].y = fry + fy(cp.points_y[i]);
        }
        return true;
      default: return false;
    }
  }

  // phase 0: block backgrounds, 1: floats, 2: inline content
  void walk(BoxId id, LayoutUnit ax, LayoutUnit ay, int phase) {
    const Box& b = tree_.box(id);
    if (phase == 2 && b.inline_content) paint_lines(id, ax, ay);
    for (uint64_t i = 0; i < b.children.count(); ++i) {
      const BoxId cid = b.children[i];
      const Box& c = tree_.box(cid);
      switch (c.kind) {
        case BoxKind::Text:
        case BoxKind::LineBreak: continue;
        case BoxKind::Marker:
          if (phase == 2) paint_marker(id, cid, ax, ay);
          continue;
        case BoxKind::Inline:
          if (phase == 1) walk(cid, ax, ay, phase);   // (floats can sit inside inline boxes)
          continue;
        default: break;
      }
      if (is_context(c)) continue;   // (painted by the context)
      const LayoutUnit cx = ax + c.x, cy = ay + c.y;
      if (c.floated) {
        if (phase == 1) paint_context(cid, cx, cy);
        continue;
      }
      if (c.kind == BoxKind::InlineBlock || (c.kind == BoxKind::Replaced && c.is_inline_level())) continue;   // (in a line: painted with it)
      if (phase == 0) paint_box(cid, cx, cy);
      const bool clipped = push_clip(c, cx, cy);
      walk(cid, cx, cy, phase);
      if (clipped) pop_clip();
      if (phase == 2) paint_outline(cid, cx, cy);   // (a box that is no stacking context has its outline over its content)
    }
  }

  // --- clipping
  graphics::CornerRadii radii_of(const Box& b) const {
    graphics::CornerRadii r;
    const Style& s = b.style;
    const int32_t w100 = static_cast<int32_t>(b.width.to_px100()), h100 = static_cast<int32_t>(b.height.to_px100());
    auto h = [&](int i) { return static_cast<float>(s.radius[i].h.resolve(w100)) / 100.0f; };
    auto v = [&](int i) { return static_cast<float>(s.radius[i].v.resolve(h100)) / 100.0f; };
    r.tl_x = h(0); r.tl_y = v(0);
    r.tr_x = h(1); r.tr_y = v(1);
    r.br_x = h(2); r.br_y = v(2);
    r.bl_x = h(3); r.bl_y = v(3);
    return r;
  }

  static bool has_radius(const graphics::CornerRadii& r) {
    return r.tl_x > 0 || r.tl_y > 0 || r.tr_x > 0 || r.tr_y > 0 || r.br_x > 0 || r.br_y > 0 || r.bl_x > 0 || r.bl_y > 0;
  }

  bool push_clip(const Box& b, LayoutUnit ax, LayoutUnit ay) {
    if (!clips(b)) return false;
    Op op;
    op.kind = OpKind::PushClip;
    op.rect = snap(ax + b.border.left, ay + b.border.top, b.width - b.border.horizontal(), b.height - b.border.vertical());
    graphics::CornerRadii r = radii_of(b);
    // the padding edge: the outer radii less the border widths
    const float bl = static_cast<float>(b.border.left.to_px100()) / 100.0f, br = static_cast<float>(b.border.right.to_px100()) / 100.0f;
    const float bt = static_cast<float>(b.border.top.to_px100()) / 100.0f, bb = static_cast<float>(b.border.bottom.to_px100()) / 100.0f;
    auto less = [](float v, float d) { return v > d ? v - d : 0.0f; };
    r.tl_x = less(r.tl_x, bl); r.tl_y = less(r.tl_y, bt);
    r.tr_x = less(r.tr_x, br); r.tr_y = less(r.tr_y, bt);
    r.br_x = less(r.br_x, br); r.br_y = less(r.br_y, bb);
    r.bl_x = less(r.bl_x, bl); r.bl_y = less(r.bl_y, bb);
    op.radii = r;
    add(op);
    return true;
  }

  void pop_clip() {
    Op op;
    op.kind = OpKind::PopClip;
    add(op);
  }

  // --- what the pointer finds
  void add_hit(const Box& b, const IRect& rect) {
    if (!b.node || rect.empty() || b.style.pointer_events == browser::PointerEvents::None) return;
    Op op;
    op.kind = OpKind::Hit;
    op.rect = rect;
    op.node = b.node;
    op.cursor = static_cast<uint8_t>(b.style.cursor);
    add(op);
  }

  // --- a box's own decoration: shadows, background, border
  uint32_t ink_of(const Style& s) const { return s.color ? s.color : kDefaultInk; }

  void paint_box(BoxId id, LayoutUnit ax, LayoutUnit ay) {
    const Box& b = tree_.box(id);
    if (b.kind == BoxKind::Root || b.style.visibility != browser::Visibility::Visible) return;
    const Style& s = b.style;
    const IRect rect = snap(ax, ay, b.width, b.height);
    const graphics::CornerRadii radii = radii_of(b);
    if (rect.empty() && b.kind != BoxKind::Replaced) return;
    add_hit(b, rect);
    if (!s.box_shadow.empty() && options_.sheet) paint_shadows(b, ax, ay, rect, radii, false);
    const bool skip_bg = id == skip_background_of_;
    if (!skip_bg) {
      if (s.own_background && (s.background_argb >> 24) != 0) {
        Op op;
        op.kind = OpKind::FillRect;
        op.rect = rect;
        op.color = s.background_argb;
        op.radii = radii;
        add(op);
      }
      if (!s.background_image.empty() && options_.sheet) paint_background_layers(b, ax, ay, rect, radii);
    }
    if (!s.box_shadow.empty() && options_.sheet) paint_shadows(b, ax, ay, rect, radii, true);
    paint_border(b, rect, radii);
    if (b.kind == BoxKind::Replaced) paint_replaced(id, ax, ay);
  }

  // The radii of the padding edge: the outer radii less the border widths.
  graphics::CornerRadii padding_radii(const Box& b) const {
    graphics::CornerRadii r = radii_of(b);
    const float bl = static_cast<float>(b.border.left.to_px100()) / 100.0f, br = static_cast<float>(b.border.right.to_px100()) / 100.0f;
    const float bt = static_cast<float>(b.border.top.to_px100()) / 100.0f, bb = static_cast<float>(b.border.bottom.to_px100()) / 100.0f;
    auto less = [](float v, float d) { return v > d ? v - d : 0.0f; };
    r.tl_x = less(r.tl_x, bl); r.tl_y = less(r.tl_y, bt);
    r.tr_x = less(r.tr_x, br); r.tr_y = less(r.tr_y, bt);
    r.br_x = less(r.br_x, br); r.br_y = less(r.br_y, bb);
    r.bl_x = less(r.bl_x, bl); r.bl_y = less(r.bl_y, bb);
    return r;
  }

  static int32_t px100_to_int(int32_t v) { return (v + (v >= 0 ? 50 : -50)) / 100; }

  // box-shadow: the outer shadows go under the background, the inset ones over it. The first shadow of the list is on top.
  void paint_shadows(const Box& b, LayoutUnit ax, LayoutUnit ay, const IRect& rect, const graphics::CornerRadii& radii, bool inset) {
    List<browser::Shadow> shadows(allocator());
    if (!options_.sheet->shadows(b.style.box_shadow, b.style, &shadows)) return;
    for (uint64_t i = shadows.count(); i > 0; --i) {
      const browser::Shadow& sh = shadows[i - 1];
      if (sh.inset != inset) continue;
      ShadowData d;
      d.color = sh.current_color ? ink_of(b.style) : sh.color;
      if ((d.color >> 24) == 0) continue;
      d.inset = inset;
      d.dx = px100_to_int(sh.x_px100);
      d.dy = px100_to_int(sh.y_px100);
      d.spread = px100_to_int(sh.spread_px100);
      d.blur = sh.blur_px100 > 0 ? static_cast<float>(sh.blur_px100) / 100.0f : 0.0f;
      Op op;
      op.kind = OpKind::Shadow;
      const int32_t reach = blur_reach(d.blur / 2.0f) + 1;
      if (inset) {
        d.box = snap(ax + b.border.left, ay + b.border.top, b.width - b.border.horizontal(), b.height - b.border.vertical());
        d.radii = padding_radii(b);
        op.rect = d.box;
      } else {
        d.box = rect;
        d.radii = radii;
        const int32_t grow = (d.spread > 0 ? d.spread : 0) + reach;
        op.rect = IRect{rect.x + d.dx - grow, rect.y + d.dy - grow, rect.width + 2 * grow, rect.height + 2 * grow};
      }
      if (op.rect.empty()) continue;
      failed_ |= !out_.add_shadow(d, &op.data);
      add(op);
    }
  }

  void paint_border(const Box& b, const IRect& rect, const graphics::CornerRadii& radii) {
    const Style& s = b.style;
    Op op;
    op.kind = OpKind::Border;
    op.rect = rect;
    op.radii = radii;
    bool any = false;
    for (int i = 0; i < 4; ++i) {
      const bool shown = s.border_style[i] != browser::BorderStyle::None && s.border_style[i] != browser::BorderStyle::Hidden && s.border_px100[i] > 0;
      if (!shown) continue;
      int32_t w = (s.border_px100[i] + 50) / 100;
      if (w < 1) w = 1;
      op.border_widths[i] = static_cast<uint16_t>(w > 65535 ? 65535 : w);
      op.border_styles[i] = static_cast<LineStyle>(s.border_style[i]);
      op.border_colors[i] = s.border_color[i] == browser::kTransparentBorder ? 0u : (s.border_color[i] ? s.border_color[i] : ink_of(s));
      any = any || (op.border_colors[i] >> 24) != 0;
    }
    if (any) add(op);
  }

  void paint_outline(BoxId id, LayoutUnit ax, LayoutUnit ay) {
    const Box& b = tree_.box(id);
    const Style& s = b.style;
    if (b.kind == BoxKind::Root || s.outline_style == browser::BorderStyle::None || s.outline_style == browser::BorderStyle::Hidden) return;
    if (s.visibility != browser::Visibility::Visible) return;
    int32_t w = (s.outline_width_px100 + 50) / 100;
    if (w < 1) w = 1;
    const int32_t offset = (s.outline_offset_px100 + (s.outline_offset_px100 >= 0 ? 50 : -50)) / 100;
    Op op;
    op.kind = OpKind::Border;
    const IRect base = snap(ax, ay, b.width, b.height);
    op.rect.x = base.x - offset - w;
    op.rect.y = base.y - offset - w;
    op.rect.width = base.width + 2 * (offset + w);
    op.rect.height = base.height + 2 * (offset + w);
    const uint32_t color = s.outline_color == browser::kTransparentBorder ? 0u : (s.outline_color ? s.outline_color : (s.outline_auto ? 0xFF005FCCu : ink_of(s)));
    if ((color >> 24) == 0) return;
    for (int i = 0; i < 4; ++i) {
      op.border_widths[i] = static_cast<uint16_t>(w);
      op.border_styles[i] = s.outline_auto ? LineStyle::Solid : static_cast<LineStyle>(s.outline_style);
      op.border_colors[i] = color;
    }
    op.radii = radii_of(b);
    add(op);
  }

  // --- gradients
  static float dim_px(const browser::Dimension& d, float base) {
    if (d.kind == browser::Dimension::Px) return static_cast<float>(d.x100) / 100.0f;
    if (d.kind == browser::Dimension::Percent) return base * static_cast<float>(d.x100) / 10000.0f;
    return 0.0f;
  }

  bool build_gradient(const browser::Gradient& g, const Style& style, const IRect& area, GradientData* out) {
    const float w = static_cast<float>(area.width), h = static_cast<float>(area.height);
    out->stop_count = 0;
    out->repeating = g.repeating;
    // the positions of the stops along the line, in pixels once the line is known: work out `length` first
    float length = 1.0f;
    if (g.kind == browser::Gradient::Linear) {
      float dx, dy;
      if (g.to_sides) {
        const bool top = g.to_sides & 1, right = g.to_sides & 2, bottom = g.to_sides & 4, left = g.to_sides & 8;
        if ((top || bottom) && (left || right)) {   // a corner: the 50% line passes through the other two corners
          const float norm = fmath::sqrt(static_cast<double>(w * w + h * h));
          const float n = norm > 0 ? norm : 1.0f;
          dx = (right ? h : -h) / n;
          dy = (bottom ? w : -w) / n;
        } else {
          dx = right ? 1.0f : left ? -1.0f : 0.0f;
          dy = bottom ? 1.0f : top ? -1.0f : 0.0f;
        }
      } else {
        const double a = static_cast<double>(g.angle_x100) / 100.0 * 3.14159265358979323846 / 180.0;
        dx = static_cast<float>(fmath::sin(a));
        dy = static_cast<float>(-fmath::cos(a));
      }
      length = (dx < 0 ? -dx : dx) * w + (dy < 0 ? -dy : dy) * h;
      if (length < 1.0f) length = 1.0f;
      const float cx = static_cast<float>(area.x) + w / 2, cy = static_cast<float>(area.y) + h / 2;
      out->kind = GradientData::Linear;
      out->from = {cx - dx * length / 2, cy - dy * length / 2};
      out->to = {cx + dx * length / 2, cy + dy * length / 2};
    } else if (g.kind == browser::Gradient::Radial) {
      const float cx = static_cast<float>(area.x) + static_cast<float>(g.center_x.resolve(static_cast<int32_t>(w * 100))) / 100.0f;
      const float cy = static_cast<float>(area.y) + static_cast<float>(g.center_y.resolve(static_cast<int32_t>(h * 100))) / 100.0f;
      const float rel_x = cx - static_cast<float>(area.x), rel_y = cy - static_cast<float>(area.y);
      const float near_x = rel_x < w - rel_x ? rel_x : w - rel_x, near_y = rel_y < h - rel_y ? rel_y : h - rel_y;
      const float far_x = rel_x > w - rel_x ? rel_x : w - rel_x, far_y = rel_y > h - rel_y ? rel_y : h - rel_y;
      float rx, ry;
      const float k = 1.41421356f;
      switch (g.extent) {
        case browser::Gradient::ClosestSide: rx = near_x; ry = near_y; break;
        case browser::Gradient::FarthestSide: rx = far_x; ry = far_y; break;
        case browser::Gradient::ClosestCorner: rx = near_x * k; ry = near_y * k; break;
        case browser::Gradient::Explicit: rx = dim_px(g.radius_x, w); ry = g.circle ? rx : dim_px(g.radius_y, h); break;
        default: rx = far_x * k; ry = far_y * k; break;   // farthest-corner
      }
      if (g.circle && g.extent != browser::Gradient::Explicit) {
        const float r = g.extent == browser::Gradient::ClosestSide ? (near_x < near_y ? near_x : near_y)
                        : g.extent == browser::Gradient::FarthestSide ? (far_x > far_y ? far_x : far_y)
                        : g.extent == browser::Gradient::ClosestCorner ? fmath::sqrt(static_cast<double>(near_x * near_x + near_y * near_y))
                                                                       : fmath::sqrt(static_cast<double>(far_x * far_x + far_y * far_y));
        rx = ry = static_cast<float>(r);
      }
      if (rx < 0.5f) rx = 0.5f;
      if (ry < 0.5f) ry = 0.5f;
      out->kind = GradientData::Radial;
      out->center = {cx, cy};
      out->radii = {rx, ry};
      length = rx;   // (stop positions in pixels are along the horizontal radius)
    } else {
      // conic: the stops are in turns, the sweep starts at the angle (clockwise from the top) around the centre
      out->kind = GradientData::Conic;
      out->center = {static_cast<float>(area.x) + static_cast<float>(g.center_x.resolve(static_cast<int32_t>(w * 100))) / 100.0f,
                     static_cast<float>(area.y) + static_cast<float>(g.center_y.resolve(static_cast<int32_t>(h * 100))) / 100.0f};
      // (the style's default angle is 180 for every gradient kind: a conic gradient without `from` starts at the top)
      const int32_t angle_x100 = g.angle_x100 == 18000 ? 0 : g.angle_x100;
      out->start_angle = static_cast<float>(static_cast<double>(angle_x100) / 100.0 * 3.14159265358979323846 / 180.0);
      length = 1.0f;
    }
    // the stops: positions in pixels (or percent of the length), the missing ones spread evenly, never going back
    float pos[browser::kMaxGradientStops];
    float hint_pos[browser::kMaxGradientStops];   // the colour hint after a stop, in the same unit as pos (NaN-free: has_hint says whether)
    bool given[browser::kMaxGradientStops], has_hint[browser::kMaxGradientStops];
    uint32_t n = 0;
    for (uint32_t i = 0; i < g.stop_count && n < browser::kMaxGradientStops && n < graphics::kMaxGradientStops; ++i) {
      if (g.stops[i].hint) {
        if (n > 0 && !g.stops[i].position.is_auto()) {
          has_hint[n - 1] = true;
          hint_pos[n - 1] = dim_px(g.stops[i].position, length);
        }
        continue;
      }
      has_hint[n] = false;
      hint_pos[n] = 0.0f;
      given[n] = !g.stops[i].position.is_auto();
      pos[n] = given[n] ? dim_px(g.stops[i].position, length) : 0.0f;
      out->stops[n].color = g.stops[i].current_color ? ink_of(style) : g.stops[i].color;
      ++n;
    }
    if (n == 0) return false;
    if (!given[0]) { pos[0] = 0.0f; given[0] = true; }
    if (!given[n - 1]) { pos[n - 1] = length; given[n - 1] = true; }
    for (uint32_t i = 1; i < n; ++i) {
      if (given[i] && pos[i] < pos[i - 1]) pos[i] = pos[i - 1];
    }
    for (uint32_t i = 1; i + 1 < n; ++i) {
      if (given[i]) continue;
      uint32_t j = i;
      while (j < n && !given[j]) ++j;
      const float a = pos[i - 1], c = pos[j];
      for (uint32_t k = i; k < j; ++k) pos[k] = a + (c - a) * static_cast<float>(k - i + 1) / static_cast<float>(j - i + 1);
      i = j - 1;
    }
    for (uint32_t i = 0; i < n; ++i) out->stops[i].offset = length > 0 ? pos[i] / length : 0.0f;
    for (uint32_t i = 0; i < graphics::kMaxGradientStops; ++i) out->hints[i] = -1.0f;
    for (uint32_t i = 0; i + 1 < n; ++i) {
      if (!has_hint[i] || pos[i + 1] <= pos[i]) continue;
      float u = (hint_pos[i] - pos[i]) / (pos[i + 1] - pos[i]);
      out->hints[i] = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;   // (0 and 1 are a hard transition: the painter treats them as none)
    }
    out->stop_count = n;
    return true;
  }

  void paint_background_layers(const Box& b, LayoutUnit ax, LayoutUnit ay, const IRect& border_rect, const graphics::CornerRadii& radii) {
    List<browser::BackgroundLayer> layers(allocator());
    if (!options_.sheet->background_layers(b.style, &layers)) return;
    // the first layer is on top, so paint from the last
    for (uint64_t li = layers.count(); li > 0; --li) {
      const browser::BackgroundLayer& layer = layers[li - 1];
      if (layer.image != browser::BackgroundLayer::GradientImage && layer.image != browser::BackgroundLayer::Url) continue;   // (image-set() and the like are not drawn)
      // the positioning area (padding box by default) and the clip (border box by default)
      IRect origin = border_rect;
      auto inset_by = [&](browser::BackgroundBox box) {
        IRect r = border_rect;
        if (box == browser::BackgroundBox::PaddingBox || box == browser::BackgroundBox::ContentBox) {
          r = snap(ax + b.border.left, ay + b.border.top, b.width - b.border.horizontal(), b.height - b.border.vertical());
          if (box == browser::BackgroundBox::ContentBox) r = snap(ax + b.content_left(), ay + b.content_top(), b.content_width(), b.content_height());
        }
        return r;
      };
      origin = inset_by(layer.origin);
      const IRect clip = inset_by(layer.clip == browser::BackgroundBox::Text ? browser::BackgroundBox::BorderBox : layer.clip);
      if (clip.empty() || origin.empty()) continue;
      const bool same_as_border = clip.x == border_rect.x && clip.y == border_rect.y && clip.width == border_rect.width && clip.height == border_rect.height;
      if (layer.image == browser::BackgroundLayer::Url) {   // a picture: the painter asks the object painter for its size and draws the tiles
        if (layer.url.empty()) continue;
        ImageLayerData d;
        d.origin = origin;
        d.clip = clip;
        d.size_mode = static_cast<ImageLayerData::SizeMode>(layer.size_mode);
        if (layer.size_mode == browser::BackgroundLayer::Explicit) {
          d.size_w = layer.size_w.is_auto() ? -1.0f : dim_px(layer.size_w, static_cast<float>(origin.width));
          d.size_h = layer.size_h.is_auto() ? -1.0f : dim_px(layer.size_h, static_cast<float>(origin.height));
        }
        d.pos_x_percent_x100 = layer.position_x.percent_x100;
        d.pos_x_px100 = layer.position_x.px100;
        d.pos_y_percent_x100 = layer.position_y.percent_x100;
        d.pos_y_px100 = layer.position_y.px100;
        d.repeat_x = static_cast<uint8_t>(layer.repeat_x);
        d.repeat_y = static_cast<uint8_t>(layer.repeat_y);
        Op op;
        op.kind = OpKind::Image;
        op.rect = clip;
        op.radii = same_as_border ? radii : graphics::CornerRadii();
        failed_ |= !out_.add_image(d, &op.data);
        failed_ |= !out_.add_text(layer.url, &op.text_offset);
        op.text_length = static_cast<uint32_t>(layer.url.size());
        add(op);
        continue;
      }
      // the size of one tile
      int32_t tw = origin.width, th = origin.height;
      if (layer.size_mode == browser::BackgroundLayer::Explicit) {
        if (!layer.size_w.is_auto()) tw = static_cast<int32_t>(dim_px(layer.size_w, static_cast<float>(origin.width)) + 0.5f);
        if (!layer.size_h.is_auto()) th = static_cast<int32_t>(dim_px(layer.size_h, static_cast<float>(origin.height)) + 0.5f);
      }
      if (tw <= 0 || th <= 0) continue;
      const TileAxis ax_tiles = lay_out_tiles(origin.x, origin.width, clip.x, clip.right(), tw, layer.position_x.percent_x100, layer.position_x.px100, static_cast<uint8_t>(layer.repeat_x));
      const TileAxis ay_tiles = lay_out_tiles(origin.y, origin.height, clip.y, clip.bottom(), th, layer.position_y.percent_x100, layer.position_y.px100, static_cast<uint8_t>(layer.repeat_y));
      uint32_t tiles = 0;
      for (int32_t j = 0; j < ay_tiles.count && tiles < 400; ++j) {
        for (int32_t i = 0; i < ax_tiles.count && tiles < 400; ++i) {
          IRect tile;
          tile.x = ax_tiles.first + i * ax_tiles.pitch;
          tile.y = ay_tiles.first + j * ay_tiles.pitch;
          tile.width = ax_tiles.size;
          tile.height = ay_tiles.size;
          const IRect visible = tile.intersection(clip);
          if (visible.empty()) continue;
          GradientData data;
          if (build_gradient(layer.gradient, b.style, tile, &data)) {
            uint32_t index = 0;
            failed_ |= !out_.add_gradient(data, &index);
            Op op;
            op.kind = OpKind::Gradient;
            op.rect = visible;
            op.gradient = index;
            op.radii = (visible.x == border_rect.x && visible.y == border_rect.y && visible.width == border_rect.width && visible.height == border_rect.height) ? radii : graphics::CornerRadii();
            add(op);
          }
          ++tiles;
        }
      }
    }
  }

  // --- text
  bool in_link(const Box& text) const {
    if (!options_.doc || !text.node) return false;
    const dom::Node* n = options_.doc->by_serial(text.node);
    for (n = n ? n->parent : nullptr; n; n = n->parent)
      if (n->is_tag("a") && dom::has_attribute(n, "href")) return true;
    return false;
  }

  uint32_t text_color(const Box& text) const {
    if (text.style.color) return text.style.color;
    return in_link(text) ? kLinkInk : kDefaultInk;
  }

  void emit_text(const Box& text_box, StringView text, LayoutUnit x, LayoutUnit y, LayoutUnit width, LayoutUnit height, uint32_t node, uint32_t color) {
    const Style& s = text_box.style;
    if (s.visibility != browser::Visibility::Visible || text.empty()) return;
    const layout::TextMetrics m = measurer_.metrics(s);
    const uint8_t decorations = static_cast<uint8_t>((s.underline ? kDecorationUnderline : 0) | (s.strike ? kDecorationStrike : 0));
    bool blank = true;
    for (uint64_t i = 0; i < text.size() && blank; ++i) blank = text[i] == ' ';
    if (blank && !decorations) return;
    Op op;
    op.kind = OpKind::Text;
    op.rect = snap(x, y, width, height);
    op.color = color;
    op.node = node;
    failed_ |= !out_.add_font(measurer_.query_for(s), &op.font);
    op.text_x64 = x.raw();
    op.baseline = (y + m.ascent).round();
    const font::ShapeOptions so = measurer_.shape_options(s);
    op.letter_spacing64 = so.letter_spacing;
    op.word_spacing64 = so.word_spacing;
    op.decorations = decorations;
    int32_t thickness = (s.font_px + 7) / 14;
    if (thickness < 1) thickness = 1;
    op.decoration_thickness = thickness;
    op.decoration_offset = (s.font_px + 7) / 8 + 1;
    op.strike_offset = (s.font_px * 3 + 5) / 10;
    failed_ |= !out_.add_text(text, &op.text_offset);
    op.text_length = static_cast<uint32_t>(text.size());
    if (!s.text_shadow.empty() && options_.sheet) emit_text_shadows(s, op);
    add(op);
  }

  // text-shadow: the run again, shifted, in the shadow's colour and blurred; the first shadow of the list is on top so the last one goes first.
  void emit_text_shadows(const Style& s, const Op& text_op) {
    List<browser::Shadow> shadows(allocator());
    if (!options_.sheet->shadows(s.text_shadow, s, &shadows)) return;
    for (uint64_t i = shadows.count(); i > 0; --i) {
      const browser::Shadow& sh = shadows[i - 1];
      Op op = text_op;
      op.color = sh.current_color ? text_op.color : sh.color;
      if ((op.color >> 24) == 0) continue;
      op.node = 0;   // (a shadow is not something the pointer finds)
      const int32_t dx = px100_to_int(sh.x_px100), dy = px100_to_int(sh.y_px100);
      op.blur = sh.blur_px100 > 0 ? static_cast<float>(sh.blur_px100) / 100.0f : 0.0f;
      op.text_x64 += dx * 64;
      op.baseline += dy;
      op.decoration_color = 0;
      const int32_t reach = op.blur > 0.0f ? blur_reach(op.blur / 2.0f) + 2 : 0;   // (the painter takes this growth off again to place the decorations)
      op.rect.x += dx - reach;
      op.rect.y += dy - reach;
      op.rect.width += 2 * reach;
      op.rect.height += 2 * reach;
      add(op);
    }
  }

  void paint_lines(BoxId id, LayoutUnit ax, LayoutUnit ay) {
    const Box& c = tree_.box(id);
    // the backgrounds and borders of inline boxes first, then what is on them
    for (uint64_t k = 0; k < c.fragments.count(); ++k) {
      const Fragment& f = c.fragments[k];
      const Box& fb = tree_.box(f.box);
      if (fb.kind != BoxKind::Inline || fb.style.visibility != browser::Visibility::Visible) continue;
      paint_inline_fragment(fb, f, ax, ay);
    }
    for (uint64_t k = 0; k < c.fragments.count(); ++k) {
      const Fragment& f = c.fragments[k];
      const Box& fb = tree_.box(f.box);
      if (fb.kind == BoxKind::Text) {
        emit_text(fb, fb.text.view().substr(f.text_offset, f.text_length), ax + f.x, ay + f.y, f.width, f.height, fb.node, text_color(fb));
      } else if (fb.kind != BoxKind::Inline) {
        paint_context(f.box, ax + f.x, ay + f.y);   // an atomic inline (its box already has its place in x, y: the fragment says the same)
      }
    }
  }

  void paint_inline_fragment(const Box& fb, const Fragment& f, LayoutUnit ax, LayoutUnit ay) {
    const Style& s = fb.style;
    const IRect rect = snap(ax + f.x, ay + f.y, f.width, f.height);
    if (rect.empty()) return;
    add_hit(fb, rect);
    if (s.own_background && (s.background_argb >> 24) != 0) {
      Op op;
      op.kind = OpKind::FillRect;
      op.rect = rect;
      op.color = s.background_argb;
      if (f.starts_box && f.ends_box) op.radii = radii_of(fb);
      add(op);
    }
    Op op;
    op.kind = OpKind::Border;
    op.rect = rect;
    bool any = false;
    for (int i = 0; i < 4; ++i) {
      if (i == 1 && !f.ends_box) continue;     // (no right edge where the box goes on in the next line)
      if (i == 3 && !f.starts_box) continue;   // (and no left edge where it came from the last)
      const bool shown = s.border_style[i] != browser::BorderStyle::None && s.border_style[i] != browser::BorderStyle::Hidden && s.border_px100[i] > 0;
      if (!shown) continue;
      int32_t w = (s.border_px100[i] + 50) / 100;
      if (w < 1) w = 1;
      op.border_widths[i] = static_cast<uint16_t>(w);
      op.border_styles[i] = static_cast<LineStyle>(s.border_style[i]);
      op.border_colors[i] = s.border_color[i] == browser::kTransparentBorder ? 0u : (s.border_color[i] ? s.border_color[i] : ink_of(s));
      any = any || (op.border_colors[i] >> 24) != 0;
    }
    if (any) add(op);
  }

  void paint_marker(BoxId container, BoxId marker, LayoutUnit ax, LayoutUnit ay) {
    const Box& li = tree_.box(container);
    const Box& m = tree_.box(marker);
    LayoutUnit baseline_y;
    const layout::TextMetrics tm = measurer_.metrics(m.style);
    if (li.inline_content && li.lines.count() > 0) baseline_y = ay + li.lines[0].y + li.lines[0].baseline - tm.ascent;
    else baseline_y = ay + m.y;
    if (options_.sheet && !m.style.list_style_image.empty()) {   // list-style-image: url(): the picture stands in the marker's box, at its right edge, centred on the line
      const StringView url = url_in(options_.sheet->text(m.style.list_style_image));
      if (!url.empty()) {
        ImageLayerData d;
        d.origin = d.clip = snap(ax + m.x, baseline_y, m.width, tm.height());
        d.pos_x_percent_x100 = 10000;
        d.pos_y_percent_x100 = 5000;
        d.repeat_x = d.repeat_y = kNoRepeat;
        Op op;
        op.kind = OpKind::Image;
        op.rect = d.clip;
        failed_ |= !out_.add_image(d, &op.data);
        failed_ |= !out_.add_text(url, &op.text_offset);
        op.text_length = static_cast<uint32_t>(url.size());
        add(op);
        return;
      }
    }
    emit_text(m, m.text.view(), ax + m.x, baseline_y, m.width, tm.height(), 0, m.style.color ? m.style.color : kDefaultInk);
  }

  // The address in `url(...)` (unquoted); empty if the text is not a url() (a gradient marker is not drawn).
  static StringView url_in(StringView value) {
    const uint64_t at = value.find("url(");
    if (at == StringView::npos) return StringView();
    StringView inner = value.substr(at + 4);
    const uint64_t close = inner.find(')');
    if (close == StringView::npos) return StringView();
    inner = inner.first(close).trimmed();
    if (inner.size() >= 2 && (inner[0] == '"' || inner[0] == '\'') && inner[inner.size() - 1] == inner[0]) inner = inner.substr(1, inner.size() - 2);
    return inner;
  }

  // --- pictures, controls and frames
  void paint_replaced(BoxId id, LayoutUnit ax, LayoutUnit ay) {
    const Box& b = tree_.box(id);
    const IRect content = snap(ax + b.content_left(), ay + b.content_top(), b.content_width(), b.content_height());
    if (content.empty()) return;
    if (b.object_kind != 0) {
      Op op;
      op.kind = OpKind::Object;
      op.node = b.node;
      op.object_kind = b.object_kind;
      op.rect = content;
      // object-fit: where the picture goes inside the box
      const browser::ObjectFit fit = b.style.object_fit;
      if (fit != browser::ObjectFit::Fill && b.has_natural_width && b.has_natural_height && b.natural_width > LayoutUnit() && b.natural_height > LayoutUnit()) {
        const float nw = static_cast<float>(b.natural_width.to_px100()) / 100.0f, nh = static_cast<float>(b.natural_height.to_px100()) / 100.0f;
        const float cw = static_cast<float>(content.width), ch = static_cast<float>(content.height);
        float scale = 1.0f;
        const float sx = cw / nw, sy = ch / nh;
        if (fit == browser::ObjectFit::Contain) scale = sx < sy ? sx : sy;
        else if (fit == browser::ObjectFit::Cover) scale = sx > sy ? sx : sy;
        else if (fit == browser::ObjectFit::ScaleDown) scale = (sx < sy ? sx : sy) < 1.0f ? (sx < sy ? sx : sy) : 1.0f;
        const int32_t pw = static_cast<int32_t>(nw * scale + 0.5f), ph = static_cast<int32_t>(nh * scale + 0.5f);
        const int32_t px = content.x + b.style.object_position_x.resolve((content.width - pw) * 100) / 100;
        const int32_t py = content.y + b.style.object_position_y.resolve((content.height - ph) * 100) / 100;
        op.rect.x = px;
        op.rect.y = py;
        op.rect.width = pw;
        op.rect.height = ph;
        Op clip;
        clip.kind = OpKind::PushClip;
        clip.rect = content;
        add(clip);
        add(op);
        pop_clip();
        return;
      }
      add(op);
      return;
    }
    if (options_.doc) paint_control(b, content);
  }

  // The default look of form controls the page did not style (and of frames, as a box).
  void paint_control(const Box& b, const IRect& content) {
    const dom::Node* n = options_.doc->by_serial(b.node);
    if (!n) return;
    const Style& s = b.style;
    const bool styled = s.own_background || s.border_style[0] != browser::BorderStyle::None;
    const IRect border_rect = snap(LayoutUnit(), LayoutUnit(), LayoutUnit(), LayoutUnit());
    (void)border_rect;
    StringView type;
    dom::get_attribute(n, "type", &type);
    const bool is_input = n->is_tag("input");
    const bool button = is_input && (type.equals_ignore_case("submit") || type.equals_ignore_case("button") || type.equals_ignore_case("reset") || type.equals_ignore_case("image"));
    const bool check = is_input && (type.equals_ignore_case("checkbox") || type.equals_ignore_case("radio"));
    if (n->is_tag("iframe")) {
      Op op;
      op.kind = OpKind::FillRect;
      op.rect = content;
      op.color = 0xFFFFFFFFu;
      add(op);
      return;
    }
    (void)styled;
    // (the box of the control is the border box: the chrome is drawn over the content area grown by the padding and border the style gave it)
    Op fill;
    fill.kind = OpKind::FillRect;
    fill.rect = content;
    if (check) {
      const bool radio = type.equals_ignore_case("radio");
      fill.color = 0xFFFFFFFFu;
      fill.radii = graphics::CornerRadii::uniform(radio ? static_cast<float>(content.width) / 2.0f : 2.0f);
      add(fill);
      Op line;
      line.kind = OpKind::Border;
      line.rect = content;
      line.radii = fill.radii;
      for (int i = 0; i < 4; ++i) {
        line.border_widths[i] = 1;
        line.border_styles[i] = LineStyle::Solid;
        line.border_colors[i] = 0xFF767676u;
      }
      add(line);
      if (dom::is_checked(n)) {
        Op mark;
        mark.kind = OpKind::FillRect;
        mark.rect.x = content.x + content.width / 4;
        mark.rect.y = content.y + content.height / 4;
        mark.rect.width = content.width - content.width / 2;
        mark.rect.height = content.height - content.height / 2;
        mark.color = 0xFF1A73E8u;
        mark.radii = radio ? graphics::CornerRadii::uniform(static_cast<float>(mark.rect.width) / 2.0f) : graphics::CornerRadii();
        add(mark);
      }
      return;
    }
    fill.color = button ? 0xFFEFEFEFu : 0xFFFFFFFFu;
    fill.radii = graphics::CornerRadii::uniform(button ? 3.0f : 2.0f);
    add(fill);
    Op line;
    line.kind = OpKind::Border;
    line.rect = content;
    line.radii = fill.radii;
    for (int i = 0; i < 4; ++i) {
      line.border_widths[i] = 1;
      line.border_styles[i] = LineStyle::Solid;
      line.border_colors[i] = 0xFF767676u;
    }
    add(line);
    // the text in it: a field's value (or its hint, grey), a button's label
    String current(allocator());
    bool hint = false;
    if (is_input || n->is_tag("textarea")) dom::get_value(n, &current);
    StringView value = current.view();
    if (value.empty()) {
      if (is_input && !button && dom::get_attribute(n, "placeholder", &value)) hint = true;
      else value = button && type.equals_ignore_case("submit") ? StringView("Submit") : StringView();
    }
    if (is_input && type.equals_ignore_case("password") && !hint) value = StringView();
    if (!value.empty()) {
      const layout::TextMetrics m = measurer_.metrics(s);
      const LayoutUnit width = measurer_.width(s, value);
      LayoutUnit x = LayoutUnit::from_px(content.x + 4);
      if (button) x = LayoutUnit::from_px(content.x) + (LayoutUnit::from_px(content.width) - width) / 2;
      const LayoutUnit y = LayoutUnit::from_px(content.y) + (LayoutUnit::from_px(content.height) - m.height()) / 2;
      Op clip;
      clip.kind = OpKind::PushClip;
      clip.rect = content;
      add(clip);
      emit_text(b, value, x, y, width, m.height(), b.node, hint ? 0xFF757575u : (s.color ? s.color : kDefaultInk));
      pop_clip();
    }
    paint_caret(b, n, content, current.view(), hint || value.empty(), type);
  }

  // The caret of the focused text field: a one pixel bar between two characters (none while text is selected or in the hidden half of its blink).
  void paint_caret(const Box& b, const dom::Node* n, const IRect& content, StringView text, bool nothing_drawn, StringView type) {
    const bool textarea = n->is_tag("textarea");
    if (b.node != options_.focus || options_.caret_hidden || (!n->is_tag("input") && !textarea)) return;
    if (n->is_tag("input") && !dom::is_text_field(dom::input_type(n))) return;
    if (dom::is_disabled(n)) return;
    uint32_t sel_start = 0, sel_end = 0, caret = 0;
    dom::get_selection(n, &sel_start, &sel_end, &caret);
    if (sel_start != sel_end) return;
    const Style& s = b.style;
    const layout::TextMetrics m = measurer_.metrics(s);
    const bool password = n->is_tag("input") && type.equals_ignore_case("password");
    LayoutUnit before;
    if (!password && !nothing_drawn && caret <= text.size()) before = measurer_.width(s, text.first(caret));   // (a password is not drawn, so the caret stays at the start)
    Op bar;
    bar.kind = OpKind::FillRect;
    bar.rect.x = content.x + 4 + before.round();
    bar.rect.y = content.y + (content.height - m.height().round()) / 2;
    bar.rect.width = 1;
    bar.rect.height = m.height().round();
    bar.color = s.color ? s.color : kDefaultInk;
    if (bar.rect.x >= content.x + static_cast<int32_t>(content.width)) return;
    add(bar);
  }

  const BoxTree& tree_;
  const PaintOptions& options_;
  DisplayList& out_;
  layout::FontMeasurer& measurer_;
  BoxId html_ = kNoBox, skip_background_of_ = kNoBox;
  uint32_t order_ = 0;
  uint32_t depth_ = 0;
  bool failed_ = false;
};

}  // namespace

bool build_display_list(const BoxTree& tree, const PaintOptions& options, DisplayList* out) {
  if (!options.measurer) return false;
  Builder builder(tree, options, out);
  return builder.run();
}

}  // namespace paint
