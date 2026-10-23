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

#include "Paint/DisplayPainter.hpp"

namespace paint {

namespace {

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

uint32_t shade(uint32_t color, int32_t percent) {   // percent > 100 lightens, < 100 darkens
  auto ch = [&](uint32_t c) {
    int32_t v = static_cast<int32_t>(c) * percent / 100;
    return static_cast<uint32_t>(v > 255 ? 255 : v < 0 ? 0 : v);
  };
  return (color & 0xFF000000u) | (ch((color >> 16) & 0xFF) << 16) | (ch((color >> 8) & 0xFF) << 8) | ch(color & 0xFF);
}

}  // namespace

void DisplayPainter::draw_text(graphics::IRenderer2D& r, font::FontHandle font, StringView text, int32_t x64, int32_t baseline, uint32_t color, int32_t letter64,
                               int32_t word64) {
  font::GlyphRun run(allocator_);
  font::ShapeOptions options;
  options.letter_spacing = letter64;
  options.word_spacing = word64;
  if (!fonts_.shape(font, text, options, &run)) return;
  int32_t pen = x64;
  for (uint64_t i = 0; i < run.glyphs.count(); ++i) {
    const font::ShapedGlyph& g = run.glyphs[i];
    const uint32_t subpixel = static_cast<uint32_t>(((pen % 64) + 64) % 64) / 16;
    const font::GlyphBitmap* bitmap = fonts_.glyph_bitmap(font, g.face, g.glyph, subpixel);
    if (bitmap && !bitmap->empty()) {
      graphics::MaskView mask;
      mask.coverage = bitmap->pixels();
      mask.width = bitmap->width;
      mask.height = bitmap->height;
      mask.pitch = bitmap->pitch();
      graphics::Rect2D source;
      source.width = bitmap->width;
      source.height = bitmap->height;
      const int32_t whole = (pen >= 0 ? pen / 64 : -((-pen + 63) / 64));
      r.draw_mask(mask, source, whole + bitmap->left, baseline - bitmap->top, color);
    }
    pen += g.advance;
  }
}

void DisplayPainter::draw_border(graphics::IRenderer2D& r, const Op& op, const IRect& rect) {
  const int32_t wt = op.border_widths[0], wr = op.border_widths[1], wb = op.border_widths[2], wl = op.border_widths[3];
  const bool same = op.border_widths[0] == op.border_widths[1] && op.border_widths[1] == op.border_widths[2] && op.border_widths[2] == op.border_widths[3] &&
                    op.border_colors[0] == op.border_colors[1] && op.border_colors[1] == op.border_colors[2] && op.border_colors[2] == op.border_colors[3] &&
                    op.border_styles[0] == op.border_styles[1] && op.border_styles[1] == op.border_styles[2] && op.border_styles[2] == op.border_styles[3];
  if (same && op.border_styles[0] == LineStyle::Solid) {
    graphics::EdgeWidths widths = graphics::EdgeWidths::uniform(op.border_widths[0]);
    r.stroke_rounded_rect(to_rect(rect), op.radii, widths, op.border_colors[0]);
    return;
  }
  // sides drawn one by one (the corners go to the top and bottom sides); rounded corners with mixed sides are drawn square
  struct Side {
    int index;
    IRect area;
    bool horizontal;
  };
  const int32_t inner_h = rect.height - wt - wb;
  const Side sides[4] = {{0, {rect.x, rect.y, rect.width, wt}, true},
                         {2, {rect.x, rect.bottom() - wb, rect.width, wb}, true},
                         {3, {rect.x, rect.y + wt, wl, inner_h}, false},
                         {1, {rect.right() - wr, rect.y + wt, wr, inner_h}, false}};
  for (const Side& side : sides) {
    const int i = side.index;
    if (op.border_widths[i] == 0 || (op.border_colors[i] >> 24) == 0 || side.area.empty()) continue;
    uint32_t color = op.border_colors[i];
    const LineStyle style = op.border_styles[i];
    const int32_t thick = op.border_widths[i];
    if (style == LineStyle::Solid) {
      r.fill_rect(to_rect(side.area), color);
    } else if (style == LineStyle::Dashed || style == LineStyle::Dotted) {
      const int32_t dash = style == LineStyle::Dotted ? thick : thick * 3;
      const int32_t gap = style == LineStyle::Dotted ? thick : thick * 2 > 4 ? thick * 2 : 4;
      if (side.horizontal) {
        for (int32_t x = side.area.x; x < side.area.right(); x += dash + gap) {
          IRect piece{x, side.area.y, dash < side.area.right() - x ? dash : side.area.right() - x, side.area.height};
          r.fill_rect(to_rect(piece), color);
        }
      } else {
        for (int32_t y = side.area.y; y < side.area.bottom(); y += dash + gap) {
          IRect piece{side.area.x, y, side.area.width, dash < side.area.bottom() - y ? dash : side.area.bottom() - y};
          r.fill_rect(to_rect(piece), color);
        }
      }
    } else if (style == LineStyle::Double && thick >= 3) {
      const int32_t line = thick / 3;
      IRect a = side.area, b = side.area;
      if (side.horizontal) {
        a.height = line;
        b.y = side.area.bottom() - line;
        b.height = line;
      } else {
        a.width = line;
        b.x = side.area.right() - line;
        b.width = line;
      }
      r.fill_rect(to_rect(a), color);
      r.fill_rect(to_rect(b), color);
    } else {
      // groove, ridge, inset and outset: the top and left sides one shade, the bottom and right another
      const bool light = (i == 0 || i == 3);
      const bool raised = style == LineStyle::Outset || style == LineStyle::Ridge;
      color = shade(color, (light == raised) ? 120 : 70);
      r.fill_rect(to_rect(side.area), color);
    }
  }
}

bool DisplayPainter::paint(graphics::IRenderer2D& r, const DisplayList& list, int32_t dx, int32_t dy, const graphics::Rect2D& view) {
  bool exact = true;
  r.set_blend(graphics::BlendMode::Alpha);
  r.set_clip(view);
  const IRect view_rect{view.x, view.y, static_cast<int32_t>(view.width), static_cast<int32_t>(view.height)};
  // the area still visible after the clips pushed so far (to skip what cannot show)
  IRect visible[graphics::kMaxClipDepth + graphics::kMaxLayerDepth + 2];
  uint32_t depth = 0;
  visible[0] = view_rect;
  uint8_t kinds[graphics::kMaxClipDepth + graphics::kMaxLayerDepth + 2];   // what each entry is: 0 clip, 1 layer
  uint32_t overflow_clips = 0, overflow_layers = 0;   // (pushes beyond the limit still have to be matched with a pop)
  for (uint64_t i = 0; i < list.count(); ++i) {
    const Op& op = list.op(i);
    IRect rect = op.rect;
    rect.x += dx;
    rect.y += dy;
    switch (op.kind) {
      case OpKind::PushClip: {
        const IRect cur = visible[depth];
        const bool ok_depth = depth + 1 < sizeof(kinds);
        const bool rounded = has_radius(op.radii);
        const bool fine = rounded ? r.push_clip_rounded_rect(to_rect(rect), op.radii) : r.push_clip_rect(to_rect(rect));
        exact = exact && fine;
        if (ok_depth) {
          ++depth;
          visible[depth] = cur.intersection(rect);
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
      case OpKind::Hit: break;
      default: {
        if (!rect.intersects(visible[depth])) break;
        switch (op.kind) {
          case OpKind::FillRect:
            if (has_radius(op.radii)) r.fill_rounded_rect(to_rect(rect), op.radii, op.color);
            else r.fill_rect(to_rect(rect), op.color);
            break;
          case OpKind::Border: draw_border(r, op, rect); break;
          case OpKind::Gradient: {
            const GradientData& g = list.gradient(op.gradient);
            const bool rounded = has_radius(op.radii);
            if (rounded) r.push_clip_rounded_rect(to_rect(rect), op.radii);
            ArrayView<const graphics::GradientStop> stops(g.stops, g.stop_count);
            graphics::Vec2 from = g.from, to = g.to, center = g.center;
            from.x += static_cast<float>(dx); from.y += static_cast<float>(dy);
            to.x += static_cast<float>(dx); to.y += static_cast<float>(dy);
            center.x += static_cast<float>(dx); center.y += static_cast<float>(dy);
            if (g.radial) r.fill_radial_gradient(to_rect(rect), center, g.radii, stops);
            else r.fill_linear_gradient(to_rect(rect), from, to, stops);
            if (rounded) r.pop_clip();
            break;
          }
          case OpKind::Text: {
            const StringView text = list.text(op);
            draw_text(r, op.font, text, op.text_x64 + dx * 64, op.baseline + dy, op.color, op.letter_spacing64, op.word_spacing64);
            if (op.decorations) {
              const int32_t width = rect.width;
              const int32_t x = rect.x;
              if (op.decorations & kDecorationUnderline) r.fill_rect(to_rect(IRect{x, op.baseline + dy + op.decoration_offset - op.decoration_thickness + 1, width, op.decoration_thickness}), op.color);
              if (op.decorations & kDecorationStrike) r.fill_rect(to_rect(IRect{x, op.baseline + dy - op.decoration_offset - op.decoration_thickness, width, op.decoration_thickness}), op.color);
            }
            break;
          }
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

}  // namespace paint
