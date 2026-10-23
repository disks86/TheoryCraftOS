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

#include "Browser/Svg.hpp"
#include "Browser/Css.hpp"
#include "Dom/HtmlBuilder.hpp"
#include "Dom/Form.hpp"
#include "Math/FloatMath.hpp"

namespace browser {

namespace {

constexpr double kPi = 3.14159265358979323846;

StringView attr(const dom::Node* n, const char* name) {
  StringView v;
  return dom::get_attribute(n, name, &v) ? v : StringView();
}

bool is_sep(char c) { return c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// Reads the number at the start of `s` from `*i` (a sign, digits, a point, an exponent); false if there is none.
bool read_number(StringView s, uint64_t* i, double* out) {
  uint64_t p = *i;
  while (p < s.size() && is_sep(s[p])) ++p;
  const uint64_t start = p;
  if (p < s.size() && (s[p] == '-' || s[p] == '+')) ++p;
  bool digits = false;
  while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p, digits = true;
  if (p < s.size() && s[p] == '.') {
    ++p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p, digits = true;
  }
  if (!digits) return false;
  if (p < s.size() && (s[p] == 'e' || s[p] == 'E') && p + 1 < s.size() && ((s[p + 1] >= '0' && s[p + 1] <= '9') || s[p + 1] == '-' || s[p + 1] == '+')) {
    ++p;
    if (s[p] == '-' || s[p] == '+') ++p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
  }
  StringView t = s.substr(start, p - start);
  if (t.starts_with('+')) t = t.substr(1);
  if (t.ends_with('.')) t = t.first(t.size() - 1);
  *i = p;
  return dom::parse_number(t, out);
}

// A length: a number with an optional unit (px, pt, em ... all taken as pixels, a percentage of `percent_of`).
bool read_length(StringView s, double percent_of, double* out) {
  s = s.trimmed();
  uint64_t i = 0;
  double v;
  if (!read_number(s, &i, &v)) return false;
  const StringView unit = s.substr(i).trimmed();
  if (unit.equals("%")) v = v * percent_of / 100;
  else if (unit.equals("pt")) v *= 4.0 / 3.0;
  else if (unit.equals("pc")) v *= 16;
  else if (unit.equals("in")) v *= 96;
  else if (unit.equals("cm")) v *= 96 / 2.54;
  else if (unit.equals("mm")) v *= 96 / 25.4;
  else if (unit.equals("em")) v *= 16;
  *out = v;
  return true;
}

struct Paint {
  enum class Kind : uint8_t { None, Color, Url, Current };
  Kind kind = Kind::Color;
  uint32_t color = 0xFF000000u;
  String url;
  explicit Paint(allocators::IAllocator& a) : url(a) {}
};

struct Style {
  explicit Style(allocators::IAllocator& a) : fill(a), stroke(a) { stroke.kind = Paint::Kind::None; }
  Paint fill, stroke;
  uint32_t color = 0xFF000000u;
  double stroke_width = 1, opacity = 1, fill_opacity = 1, stroke_opacity = 1, miter = 4, dash_offset = 0, font_size = 16;
  uint8_t cap = 0, join = 0, anchor = 0;
  bool even_odd = false, bold = false, hidden = false;
  double dash[8];
  uint32_t dash_count = 0;
};

class SvgDrawer {
 public:
  SvgDrawer(CanvasBitmap& bitmap, const dom::Node* root) : bitmap_(bitmap), root_(root), a_(root->owner->allocator()) {}

  bool draw(const dom::Node* svg) {
    Style s(a_);
    for (const dom::Node* c = svg->first_child; c && ok_; c = c->next) node(c, s, 0);
    return ok_;
  }

 private:
  struct Box {
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    void add(double x, double y) {
      if (x < x0) x0 = x;
      if (y < y0) y0 = y;
      if (x > x1) x1 = x;
      if (y > y1) y1 = y;
    }
    bool empty() const { return x1 < x0; }
  };

  // --- Style
  bool paint_value(StringView v, Paint* p, const Style& base) {
    v = v.trimmed();
    if (v.empty() || v.equals_ignore_case("inherit")) return false;
    if (v.equals_ignore_case("none")) {
      p->kind = Paint::Kind::None;
      return true;
    }
    if (v.equals_ignore_case("currentcolor")) {
      p->kind = Paint::Kind::Color;
      p->color = base.color;
      return true;
    }
    if (v.starts_with("url(")) {
      uint64_t close = v.find(')');
      StringView inner = v.substr(4, (close == StringView::npos ? v.size() : close) - 4).trimmed();
      if (inner.size() >= 2 && (inner[0] == '\'' || inner[0] == '"')) inner = inner.substr(1, inner.size() - 2);
      if (inner.starts_with('#')) inner = inner.substr(1);
      p->kind = Paint::Kind::Url;
      p->url.assign(inner);
      // (a colour after the address is what is used if the address is not there)
      return true;
    }
    uint32_t c;
    if (parse_css_color_alpha(v, &c)) {
      p->kind = Paint::Kind::Color;
      p->color = c;
      return true;
    }
    return false;
  }

  static bool number_value(StringView v, double* out) {
    v = v.trimmed();
    uint64_t i = 0;
    double d;
    if (!read_number(v, &i, &d)) return false;
    if (v.substr(i).trimmed().equals("%")) d /= 100;
    *out = d;
    return true;
  }

  void property(Style* s, StringView name, StringView v) {
    v = v.trimmed();
    if (name.equals("fill")) paint_value(v, &s->fill, *s);
    else if (name.equals("stroke")) paint_value(v, &s->stroke, *s);
    else if (name.equals("color")) {
      uint32_t c;
      if (parse_css_color_alpha(v, &c)) s->color = c;
    } else if (name.equals("stroke-width")) {
      double d;
      if (read_length(v, 100, &d) && d >= 0) s->stroke_width = d;
    } else if (name.equals("opacity")) {
      number_value(v, &s->opacity);
    } else if (name.equals("fill-opacity")) {
      number_value(v, &s->fill_opacity);
    } else if (name.equals("stroke-opacity")) {
      number_value(v, &s->stroke_opacity);
    } else if (name.equals("stroke-miterlimit")) {
      number_value(v, &s->miter);
    } else if (name.equals("stroke-dashoffset")) {
      read_length(v, 100, &s->dash_offset);
    } else if (name.equals("stroke-linecap")) {
      s->cap = v.equals("round") ? 1 : v.equals("square") ? 2 : 0;
    } else if (name.equals("stroke-linejoin")) {
      s->join = v.equals("round") ? 1 : v.equals("bevel") ? 2 : 0;
    } else if (name.equals("fill-rule")) {
      s->even_odd = v.equals("evenodd");
    } else if (name.equals("stroke-dasharray")) {
      s->dash_count = 0;
      uint64_t i = 0;
      double d;
      while (s->dash_count < 8 && read_number(v, &i, &d)) s->dash[s->dash_count++] = d < 0 ? 0 : d;
      if (s->dash_count & 1) {   // (an odd list is repeated)
        for (uint32_t k = 0; k < s->dash_count && s->dash_count + k < 8; ++k) s->dash[s->dash_count + k] = s->dash[k];
        s->dash_count = s->dash_count * 2 > 8 ? 8 : s->dash_count * 2;
      }
    } else if (name.equals("font-size")) {
      double d;
      if (read_length(v, 16, &d) && d > 0) s->font_size = d;
    } else if (name.equals("font-weight")) {
      s->bold = v.equals("bold") || v.equals("bolder") || (v.size() == 3 && v[0] >= '6');
    } else if (name.equals("text-anchor")) {
      s->anchor = v.equals("middle") ? 1 : v.equals("end") ? 2 : 0;
    } else if (name.equals("display")) {
      if (v.equals("none")) s->hidden = true;
    } else if (name.equals("visibility")) {
      s->hidden = v.equals("hidden") || v.equals("collapse");
    }
  }

  // The style of an element: the parent's, then the presentation attributes, then the style attribute.
  Style style_of(const dom::Node* n, const Style& parent, bool* display_none) {
    Style s = copy(parent);
    s.opacity = 1;   // (opacity is not inherited: it is applied where it is set)
    static const char* const kNames[] = {"fill", "stroke", "color", "stroke-width", "opacity", "fill-opacity", "stroke-opacity", "stroke-miterlimit", "stroke-dashoffset",
                                         "stroke-linecap", "stroke-linejoin", "fill-rule", "stroke-dasharray", "font-size", "font-weight", "text-anchor", "visibility"};
    // (color first: currentColor is read through it)
    StringView v;
    if (dom::get_attribute(n, "color", &v)) property(&s, "color", v);
    for (const char* name : kNames)
      if (dom::get_attribute(n, name, &v)) property(&s, name, v);
    *display_none = false;
    if (dom::get_attribute(n, "display", &v) && v.trimmed().equals("none")) *display_none = true;
    StringView css;
    if (dom::get_attribute(n, "style", &css)) {
      uint64_t i = 0;
      // color first, so that currentColor sees it
      for (int pass = 0; pass < 2; ++pass) {
        i = 0;
        while (i < css.size()) {
          uint64_t end = css.find(';', i);
          if (end == StringView::npos) end = css.size();
          const StringView decl = css.substr(i, end - i);
          const uint64_t colon = decl.find(':');
          if (colon != StringView::npos) {
            const StringView name = decl.first(colon).trimmed();
            StringView value = decl.substr(colon + 1).trimmed();
            const uint64_t important = value.find('!');
            if (important != StringView::npos) value = value.first(important).trimmed();
            if ((pass == 0) == name.equals("color")) {
              if (name.equals("display") && value.equals("none")) *display_none = true;
              else property(&s, name, value);
            }
          }
          i = end + 1;
        }
      }
    }
    return s;
  }

  Style copy(const Style& p) {
    Style s(a_);
    s.fill.kind = p.fill.kind;
    s.fill.color = p.fill.color;
    s.fill.url.assign(p.fill.url.view());
    s.stroke.kind = p.stroke.kind;
    s.stroke.color = p.stroke.color;
    s.stroke.url.assign(p.stroke.url.view());
    s.color = p.color;
    s.stroke_width = p.stroke_width;
    s.opacity = p.opacity;
    s.fill_opacity = p.fill_opacity;
    s.stroke_opacity = p.stroke_opacity;
    s.miter = p.miter;
    s.dash_offset = p.dash_offset;
    s.font_size = p.font_size;
    s.cap = p.cap;
    s.join = p.join;
    s.anchor = p.anchor;
    s.even_odd = p.even_odd;
    s.bold = p.bold;
    s.hidden = p.hidden;
    s.dash_count = p.dash_count;
    for (uint32_t i = 0; i < 8; ++i) s.dash[i] = p.dash[i];
    return s;
  }

  // --- Paint (colours and gradients)
  const dom::Node* by_id(StringView id) const { return root_->owner->element_by_id(id); }

  static uint32_t with_alpha(uint32_t color, double alpha) {
    double a = ((color >> 24) & 255) * (alpha < 0 ? 0 : alpha > 1 ? 1 : alpha);
    return (static_cast<uint32_t>(a + 0.5) << 24) | (color & 0x00FFFFFFu);
  }

  // Fills `out` for a gradient element, in the space of `box` (objectBoundingBox) or the user space. False if the address is not a gradient.
  bool gradient(const dom::Node* g, const Box& box, CanvasPaint* out, double opacity, const Style& style, int depth = 0) {
    const bool linear = g->is_tag("lineargradient"), radial = g->is_tag("radialgradient");
    if (!linear && !radial) return false;
    const bool user = attr(g, "gradientunits").trimmed().equals("userSpaceOnUse");
    const double bw = box.empty() ? 1 : box.x1 - box.x0, bh = box.empty() ? 1 : box.y1 - box.y0;
    auto coordinate = [&](const char* name, double fallback_fraction, bool horizontal) {
      StringView v = attr(g, name).trimmed();
      double d;
      if (v.empty() || !number_value(v, &d)) return user ? fallback_fraction * (horizontal ? view_w_ : view_h_) : fallback_fraction;
      if (!user) {
        if (!v.ends_with('%') && false) d /= 1;
        return d;   // (a fraction of the box: percentages were turned into fractions)
      }
      return v.ends_with('%') ? d * (horizontal ? view_w_ : view_h_) : d;
    };
    auto place_x = [&](double f) { return user ? f : box.x0 + f * bw; };
    auto place_y = [&](double f) { return user ? f : box.y0 + f * bh; };
    if (linear) {
      out->kind = CanvasPaint::Kind::Linear;
      out->x0 = place_x(coordinate("x1", 0, true));
      out->y0 = place_y(coordinate("y1", 0, false));
      out->x1 = place_x(coordinate("x2", 1, true));
      out->y1 = place_y(coordinate("y2", 0, false));
    } else {
      out->kind = CanvasPaint::Kind::Radial;
      const double cx = coordinate("cx", 0.5, true), cy = coordinate("cy", 0.5, false), r = coordinate("r", 0.5, true);
      out->x0 = out->x1 = place_x(cx);
      out->y0 = out->y1 = place_y(cy);
      out->r0 = 0;
      out->r1 = user ? r : r * (bw < bh ? bw : bh);
      StringView fx = attr(g, "fx");
      if (!fx.empty() && false) out->x0 = place_x(coordinate("fx", cx, true));
    }
    // the stops: its own, else those of the gradient it points to
    const dom::Node* source = g;
    for (int hops = 0; hops < 4 && source; ++hops) {
      bool any = false;
      for (const dom::Node* c = source->first_child; c; c = c->next) any = any || c->is_tag("stop");
      if (any) break;
      StringView href = attr(source, "href");
      if (href.empty()) href = attr(source, "xlink:href");
      source = href.starts_with('#') ? by_id(href.substr(1)) : nullptr;
    }
    double last = 0;
    if (source) {
      for (const dom::Node* c = source->first_child; c; c = c->next) {
        if (!c->is_tag("stop")) continue;
        bool none;
        Style ss = style_of(c, style, &none);
        double offset = 0;
        number_value(attr(c, "offset"), &offset);
        if (offset < last) offset = last;
        if (offset > 1) offset = 1;
        last = offset;
        uint32_t color = 0xFF000000u;
        StringView sc = attr(c, "stop-color");
        double stop_opacity = 1;
        number_value(attr(c, "stop-opacity"), &stop_opacity);
        StringView css = attr(c, "style");
        if (!css.empty()) {
          uint64_t i = 0;
          while (i < css.size()) {
            uint64_t end = css.find(';', i);
            if (end == StringView::npos) end = css.size();
            const StringView decl = css.substr(i, end - i);
            const uint64_t colon = decl.find(':');
            if (colon != StringView::npos) {
              const StringView name = decl.first(colon).trimmed(), value = decl.substr(colon + 1).trimmed();
              if (name.equals("stop-color")) sc = value;
              if (name.equals("stop-opacity")) number_value(value, &stop_opacity);
            }
            i = end + 1;
          }
        }
        uint32_t parsed;
        if (sc.equals_ignore_case("currentcolor")) color = ss.color;
        else if (!sc.empty() && parse_css_color_alpha(sc, &parsed)) color = parsed;
        out->add_stop(offset, with_alpha(color, stop_opacity * opacity));
      }
    }
    if (out->stop_count == 0) return false;
    return true;
  }

  // The CanvasPaint for a fill or stroke; false if nothing is to be drawn.
  bool make_paint(const Paint& p, const Style& style, const Box& box, double opacity, CanvasPaint* out) {
    if (p.kind == Paint::Kind::None) return false;
    if (p.kind == Paint::Kind::Url) {
      const dom::Node* g = by_id(p.url.view());
      if (g && gradient(g, box, out, opacity, style)) return true;
      return false;
    }
    out->kind = CanvasPaint::Kind::Solid;
    out->color = with_alpha(p.color, opacity);
    return (out->color >> 24) != 0;
  }

  // --- Shapes
  void paint_path(Path& path, const Style& s, const Box& box) {
    CanvasState& st = bitmap_.state();
    CanvasPaint paint;
    if (make_paint(s.fill, s, box, s.fill_opacity * s.opacity, &paint)) {
      st.fill = paint;
      (void)path;
      ok_ &= bitmap_.fill_path(nullptr, s.even_odd);   // (the current path: its points went through the transform when they were added)
    }
    CanvasPaint line;
    if (s.stroke_width > 0 && make_paint(s.stroke, s, box, s.stroke_opacity * s.opacity, &line)) {
      st.stroke = line;
      st.line_width = s.stroke_width;
      st.line_cap = s.cap;
      st.line_join = s.join;
      st.miter_limit = s.miter;
      st.dash_count = s.dash_count;
      for (uint32_t i = 0; i < s.dash_count; ++i) st.dash[i] = s.dash[i];
      st.dash_offset = s.dash_offset;
      ok_ &= bitmap_.stroke_path(nullptr);
      st.dash_count = 0;
    }
  }

  static void quad_to_cubic_box(Box* box, double x, double y) { box->add(x, y); }

  // Path data: the commands of the d attribute.
  void path_data(StringView d, Path* path, Box* box) {
    uint64_t i = 0;
    char cmd = 0;
    double cx = 0, cy = 0, sx = 0, sy = 0, last_cx = 0, last_cy = 0;   // current point, subpath start, last control point
    char prev = 0;
    auto num = [&](double* v) { return read_number(d, &i, v); };
    while (true) {
      while (i < d.size() && is_sep(d[i])) ++i;
      if (i >= d.size()) break;
      const char c = d[i];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        cmd = c;
        ++i;
        if (cmd == 'z' || cmd == 'Z') {
          path->close();
          cx = sx;
          cy = sy;
          prev = 'z';
          continue;
        }
      } else if (cmd == 0 || cmd == 'z' || cmd == 'Z') {
        break;
      }
      const bool rel = cmd >= 'a';
      const char up = static_cast<char>(rel ? cmd - 32 : cmd);
      double v[7];
      const int need = up == 'M' || up == 'L' || up == 'T' ? 2 : up == 'H' || up == 'V' ? 1 : up == 'C' ? 6 : up == 'S' || up == 'Q' ? 4 : up == 'A' ? 7 : 0;
      if (need == 0) break;
      bool good = true;
      for (int k = 0; k < need && good; ++k) {
        if (up == 'A' && (k == 3 || k == 4)) {   // (the flags may be written without a separator)
          while (i < d.size() && is_sep(d[i])) ++i;
          if (i < d.size() && (d[i] == '0' || d[i] == '1')) v[k] = d[i++] - '0';
          else good = false;
        } else {
          good = num(&v[k]);
        }
      }
      if (!good) break;
      const double ox = rel ? cx : 0, oy = rel ? cy : 0;
      switch (up) {
        case 'M':
          cx = v[0] + ox;
          cy = v[1] + oy;
          path->move_to(cx, cy);
          sx = cx;
          sy = cy;
          box->add(cx, cy);
          cmd = rel ? 'l' : 'L';   // (more pairs are lines)
          break;
        case 'L':
          cx = v[0] + ox;
          cy = v[1] + oy;
          path->line_to(cx, cy);
          box->add(cx, cy);
          break;
        case 'H':
          cx = v[0] + ox;
          path->line_to(cx, cy);
          box->add(cx, cy);
          break;
        case 'V':
          cy = v[0] + oy;
          path->line_to(cx, cy);
          box->add(cx, cy);
          break;
        case 'C': {
          path->bezier_to(v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy, v[4] + ox, v[5] + oy);
          box->add(v[0] + ox, v[1] + oy);
          box->add(v[2] + ox, v[3] + oy);
          last_cx = v[2] + ox;
          last_cy = v[3] + oy;
          cx = v[4] + ox;
          cy = v[5] + oy;
          box->add(cx, cy);
          break;
        }
        case 'S': {
          const bool smooth = prev == 'C' || prev == 'S';
          const double c1x = smooth ? 2 * cx - last_cx : cx, c1y = smooth ? 2 * cy - last_cy : cy;
          path->bezier_to(c1x, c1y, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy);
          box->add(c1x, c1y);
          box->add(v[0] + ox, v[1] + oy);
          last_cx = v[0] + ox;
          last_cy = v[1] + oy;
          cx = v[2] + ox;
          cy = v[3] + oy;
          box->add(cx, cy);
          break;
        }
        case 'Q': {
          path->quad_to(v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy);
          box->add(v[0] + ox, v[1] + oy);
          last_cx = v[0] + ox;
          last_cy = v[1] + oy;
          cx = v[2] + ox;
          cy = v[3] + oy;
          box->add(cx, cy);
          break;
        }
        case 'T': {
          const bool smooth = prev == 'Q' || prev == 'T';
          const double qx = smooth ? 2 * cx - last_cx : cx, qy = smooth ? 2 * cy - last_cy : cy;
          path->quad_to(qx, qy, v[0] + ox, v[1] + oy);
          box->add(qx, qy);
          last_cx = qx;
          last_cy = qy;
          cx = v[0] + ox;
          cy = v[1] + oy;
          box->add(cx, cy);
          break;
        }
        case 'A': {
          arc(path, box, cx, cy, v[0], v[1], v[2], v[3] != 0, v[4] != 0, v[5] + ox, v[6] + oy);
          cx = v[5] + ox;
          cy = v[6] + oy;
          break;
        }
        default: break;
      }
      prev = up;
    }
  }

  static double angle_between(double ux, double uy, double vx, double vy) {
    const double dot = ux * vx + uy * vy, cross = ux * vy - uy * vx;
    return canvas_atan2(cross, dot);
  }

  // An elliptical arc from (x1, y1) to (x2, y2) as the path data says it (the endpoint form; SVG 1.1 appendix F.6).
  void arc(Path* path, Box* box, double x1, double y1, double rx, double ry, double rotation, bool large, bool sweep, double x2, double y2) {
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    box->add(x2, y2);
    if ((x1 == x2 && y1 == y2)) return;
    if (rx == 0 || ry == 0) {
      path->line_to(x2, y2);
      return;
    }
    const double phi = rotation * kPi / 180, cp = fmath::cos(phi), sp = fmath::sin(phi);
    const double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    const double x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    const double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1) {
      const double s = fmath::sqrt(lambda);
      rx *= s;
      ry *= s;
    }
    const double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    const double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double coef = den == 0 ? 0 : fmath::sqrt(num / den > 0 ? num / den : 0);
    if (large == sweep) coef = -coef;
    const double cxp = coef * (rx * y1p / ry), cyp = coef * -(ry * x1p / rx);
    const double ccx = cp * cxp - sp * cyp + (x1 + x2) / 2, ccy = sp * cxp + cp * cyp + (y1 + y2) / 2;
    const double theta1 = angle_between(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    double delta = angle_between((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!sweep && delta > 0) delta -= 2 * kPi;
    if (sweep && delta < 0) delta += 2 * kPi;
    path->ellipse(ccx, ccy, rx, ry, phi, theta1, theta1 + delta, delta < 0);
    box->add(ccx - rx, ccy - ry);
    box->add(ccx + rx, ccy + ry);
  }

  // transform="rotate(30 10 10) translate(5)": applied to the bitmap's transform.
  void apply_transform(StringView t) {
    uint64_t i = 0;
    while (i < t.size()) {
      while (i < t.size() && (is_sep(t[i]))) ++i;
      uint64_t name_end = i;
      while (name_end < t.size() && t[name_end] != '(' && !is_sep(t[name_end])) ++name_end;
      const StringView name = t.substr(i, name_end - i);
      if (name.empty()) break;
      i = name_end;
      while (i < t.size() && t[i] != '(') ++i;
      if (i >= t.size()) break;
      ++i;
      double v[6] = {0, 0, 0, 0, 0, 0};
      int n = 0;
      double d;
      while (n < 6 && read_number(t, &i, &d)) v[n++] = d;
      while (i < t.size() && t[i] != ')') ++i;
      ++i;
      if (name.equals("matrix") && n == 6) bitmap_.transform_by(make_transform(v[0], v[1], v[2], v[3], v[4], v[5]));
      else if (name.equals("translate")) bitmap_.transform_by(make_transform(1, 0, 0, 1, v[0], n > 1 ? v[1] : 0));
      else if (name.equals("scale")) bitmap_.transform_by(make_transform(v[0], 0, 0, n > 1 ? v[1] : v[0], 0, 0));
      else if (name.equals("rotate")) {
        const double r = v[0] * kPi / 180, c = fmath::cos(r), s = fmath::sin(r);
        if (n >= 3) bitmap_.transform_by(make_transform(1, 0, 0, 1, v[1], v[2]));
        bitmap_.transform_by(make_transform(c, s, -s, c, 0, 0));
        if (n >= 3) bitmap_.transform_by(make_transform(1, 0, 0, 1, -v[1], -v[2]));
      } else if (name.equals("skewX")) bitmap_.transform_by(make_transform(1, 0, fmath::tan(v[0] * kPi / 180), 1, 0, 0));
      else if (name.equals("skewY")) bitmap_.transform_by(make_transform(1, fmath::tan(v[0] * kPi / 180), 0, 1, 0, 0));
    }
  }

  double length_attr(const dom::Node* n, const char* name, double percent_of, double fallback = 0) {
    double d;
    return read_length(attr(n, name), percent_of, &d) ? d : fallback;
  }

  void text_element(const dom::Node* n, const Style& s) {
    CanvasState& st = bitmap_.state();
    String text(a_);
    dom::text_content(n, &text);
    // white space is collapsed
    String line(a_);
    bool space = true;
    for (uint64_t i = 0; i < text.size(); ++i) {
      const char c = text[i];
      if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
        if (!space) line.push_back(' ');
        space = true;
      } else {
        line.push_back(c);
        space = false;
      }
    }
    while (line.size() && line[line.size() - 1] == ' ') line.truncate(line.size() - 1);
    if (line.empty()) return;
    const double x = length_attr(n, "x", view_w_), y = length_attr(n, "y", view_h_);
    st.font_size = s.font_size;
    st.font_bold = s.bold;
    st.font_text_length = 0;
    st.text_align = s.anchor == 1 ? 4 : s.anchor == 2 ? 3 : 2;
    st.text_baseline = 0;
    Box none;
    CanvasPaint paint;
    if (make_paint(s.fill, s, none, s.fill_opacity * s.opacity, &paint)) {
      st.fill = paint;
      ok_ &= bitmap_.fill_text(line.view(), x, y, 1e9);
    }
    CanvasPaint outline;
    if (s.stroke.kind != Paint::Kind::None && make_paint(s.stroke, s, none, s.stroke_opacity * s.opacity, &outline)) {
      st.stroke = outline;
      st.line_width = s.stroke_width;
      ok_ &= bitmap_.stroke_text(line.view(), x, y, 1e9);
    }
  }

  void node(const dom::Node* n, const Style& parent, int depth) {
    if (!ok_ || !n->is_element() || depth > 24) return;
    const StringView tag = n->name.view();
    if (tag.equals("defs") || tag.equals("title") || tag.equals("desc") || tag.equals("metadata") || tag.equals("style") || tag.equals("script") || tag.equals("lineargradient") ||
        tag.equals("radialgradient") || tag.equals("clippath") || tag.equals("mask") || tag.equals("marker") || tag.equals("pattern") || tag.equals("symbol") ||
        tag.equals("filter") || tag.equals("stop"))
      return;
    bool none;
    Style s = style_of(n, parent, &none);
    if (none) return;
    bitmap_.save();
    StringView transform = attr(n, "transform");
    if (!transform.empty()) apply_transform(transform);
    if (!s.hidden || tag.equals("g") || tag.equals("svg") || tag.equals("a") || tag.equals("switch")) shape(n, tag, s, depth);
    bitmap_.restore();
  }

  void children(const dom::Node* n, const Style& s, int depth) {
    for (const dom::Node* c = n->first_child; c && ok_; c = c->next) node(c, s, depth + 1);
  }

  void shape(const dom::Node* n, StringView tag, const Style& s, int depth) {
    Path& path = bitmap_.path();
    path.clear();
    Box box;
    if (tag.equals("g") || tag.equals("a") || tag.equals("switch")) {
      children(n, s, depth);
      return;
    }
    if (tag.equals("svg")) {   // (nested: placed at x, y; its own viewBox is not applied)
      bitmap_.transform_by(make_transform(1, 0, 0, 1, length_attr(n, "x", view_w_), length_attr(n, "y", view_h_)));
      children(n, s, depth);
      return;
    }
    if (tag.equals("use")) {
      StringView href = attr(n, "href");
      if (href.empty()) href = attr(n, "xlink:href");
      if (!href.starts_with('#')) return;
      const dom::Node* target = by_id(href.substr(1));
      if (!target || target == n || use_depth_ > 6) return;
      bitmap_.transform_by(make_transform(1, 0, 0, 1, length_attr(n, "x", view_w_), length_attr(n, "y", view_h_)));
      ++use_depth_;
      if (target->is_tag("symbol")) children(target, s, depth);
      else node(target, s, depth + 1);
      --use_depth_;
      return;
    }
    if (tag.equals("text")) {
      text_element(n, s);
      return;
    }
    if (tag.equals("rect")) {
      const double x = length_attr(n, "x", view_w_), y = length_attr(n, "y", view_h_), w = length_attr(n, "width", view_w_), h = length_attr(n, "height", view_h_);
      if (w <= 0 || h <= 0) return;
      double rx = length_attr(n, "rx", view_w_, -1), ry = length_attr(n, "ry", view_h_, -1);
      if (rx < 0 && ry < 0) rx = ry = 0;
      else if (rx < 0) rx = ry;
      else if (ry < 0) ry = rx;
      if (rx > w / 2) rx = w / 2;
      if (ry > h / 2) ry = h / 2;
      if (rx > 0 && ry > 0) {   // (rounded corners: four elliptical arcs)
        path.move_to(x + rx, y);
        path.line_to(x + w - rx, y);
        path.ellipse(x + w - rx, y + ry, rx, ry, 0, -kPi / 2, 0, false);
        path.line_to(x + w, y + h - ry);
        path.ellipse(x + w - rx, y + h - ry, rx, ry, 0, 0, kPi / 2, false);
        path.line_to(x + rx, y + h);
        path.ellipse(x + rx, y + h - ry, rx, ry, 0, kPi / 2, kPi, false);
        path.line_to(x, y + ry);
        path.ellipse(x + rx, y + ry, rx, ry, 0, kPi, 1.5 * kPi, false);
        path.close();
      } else {
        path.rect(x, y, w, h);
      }
      box.add(x, y);
      box.add(x + w, y + h);
    } else if (tag.equals("circle") || tag.equals("ellipse")) {
      const double cx = length_attr(n, "cx", view_w_), cy = length_attr(n, "cy", view_h_);
      double rx, ry;
      if (tag.equals("circle")) rx = ry = length_attr(n, "r", (view_w_ + view_h_) / 2);
      else {
        rx = length_attr(n, "rx", view_w_, -1);
        ry = length_attr(n, "ry", view_h_, -1);
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
      }
      if (rx <= 0 || ry <= 0) return;
      path.ellipse(cx, cy, rx, ry, 0, 0, 2 * kPi, false);
      path.close();
      box.add(cx - rx, cy - ry);
      box.add(cx + rx, cy + ry);
    } else if (tag.equals("line")) {
      const double x1 = length_attr(n, "x1", view_w_), y1 = length_attr(n, "y1", view_h_), x2 = length_attr(n, "x2", view_w_), y2 = length_attr(n, "y2", view_h_);
      path.move_to(x1, y1);
      path.line_to(x2, y2);
      box.add(x1, y1);
      box.add(x2, y2);
    } else if (tag.equals("polyline") || tag.equals("polygon")) {
      StringView pts = attr(n, "points");
      uint64_t i = 0;
      double x, y;
      bool first = true;
      while (read_number(pts, &i, &x) && read_number(pts, &i, &y)) {
        if (first) path.move_to(x, y);
        else path.line_to(x, y);
        box.add(x, y);
        first = false;
      }
      if (first) return;
      if (tag.equals("polygon")) path.close();
    } else if (tag.equals("path")) {
      path_data(attr(n, "d"), &path, &box);
    } else {
      return;
    }
    if (box.empty()) return;
    paint_path(path, s, box);
  }

 public:
  void set_view(double w, double h) {
    view_w_ = w;
    view_h_ = h;
  }

 private:
  CanvasBitmap& bitmap_;
  const dom::Node* root_;
  allocators::IAllocator& a_;
  double view_w_ = 300, view_h_ = 150;
  int use_depth_ = 0;
  bool ok_ = true;
};

bool parse_view_box(const dom::Node* svg, double* v) {
  StringView s = attr(svg, "viewbox");
  uint64_t i = 0;
  for (int k = 0; k < 4; ++k)
    if (!read_number(s, &i, &v[k])) return false;
  return v[2] > 0 && v[3] > 0;
}

}  // namespace

void svg_natural_size(const dom::Node* svg, uint32_t* width, uint32_t* height, bool* shape_only) {
  double w = 0, h = 0, vb[4];
  const bool has_w = read_length(attr(svg, "width"), 300, &w) && !attr(svg, "width").trimmed().ends_with('%') && w > 0;
  const bool has_h = read_length(attr(svg, "height"), 150, &h) && !attr(svg, "height").trimmed().ends_with('%') && h > 0;
  const bool box = parse_view_box(svg, vb);
  if (shape_only) *shape_only = !has_w && !has_h && box && vb[2] > 0;
  if (!has_w && !has_h) {
    w = box ? vb[2] : 300;
    h = box ? vb[3] : 150;
    if (box && w > 0) {   // (without a size, the picture is as wide as the default and as tall as its shape says)
      const double scale = 300 / vb[2];
      w = 300;
      h = vb[3] * scale;
    }
  } else if (has_w && !has_h) {
    h = box ? w * vb[3] / vb[2] : 150;
  } else if (!has_w && has_h) {
    w = box ? h * vb[2] / vb[3] : 300;
  }
  *width = static_cast<uint32_t>(w < 1 ? 1 : w > 4096 ? 4096 : w);
  *height = static_cast<uint32_t>(h < 1 ? 1 : h > 4096 ? 4096 : h);
}

bool draw_svg(const dom::Node* svg, CanvasBitmap* bitmap) {
  const double W = bitmap->width(), H = bitmap->height();
  double vb[4];
  double minx = 0, miny = 0, vw = W, vh = H;
  if (parse_view_box(svg, vb)) {
    minx = vb[0];
    miny = vb[1];
    vw = vb[2];
    vh = vb[3];
  }
  double sx = W / vw, sy = H / vh, tx, ty;
  const StringView aspect = attr(svg, "preserveaspectratio").trimmed();
  if (aspect.starts_with("none")) {
    tx = -minx * sx;
    ty = -miny * sy;
  } else {
    const bool slice = aspect.contains("slice");
    const double s = slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    sx = sy = s;
    const double room_x = W - vw * s, room_y = H - vh * s;
    tx = -minx * s + (aspect.contains("xMin") ? 0 : aspect.contains("xMax") ? room_x : room_x / 2);
    ty = -miny * s + (aspect.contains("YMin") ? 0 : aspect.contains("YMax") ? room_y : room_y / 2);
  }
  bitmap->set_transform(make_transform(sx, 0, 0, sy, tx, ty));
  SvgDrawer drawer(*bitmap, svg);
  drawer.set_view(vw, vh);
  return drawer.draw(svg);
}

bool looks_like_svg(StringView text) {
  const uint64_t head = text.size() < 1024 ? text.size() : 1024;
  for (uint64_t i = 0; i + 4 <= head; ++i)
    if (text[i] == '<' && text.substr(i + 1, 3).equals_ignore_case("svg") && (i + 4 >= text.size() || text[i + 4] == ' ' || text[i + 4] == '>' || text[i + 4] == '\n' || text[i + 4] == '\t')) return true;
  return false;
}

bool rasterize_svg(allocators::IAllocator& allocator, StringView text, media::Image* out) {
  if (!looks_like_svg(text)) return false;
  dom::Document doc(allocator);
  if (!dom::build_document(doc, text, nullptr)) return false;
  const dom::Node* svg = nullptr;
  for (const dom::Node* n = doc.root(); n && !svg; n = dom::next_in_tree(n, doc.root()))
    if (n->is_tag("svg")) svg = n;
  if (!svg) return false;
  uint32_t w, h;
  svg_natural_size(svg, &w, &h);
  if (w > 2048) w = 2048;
  if (h > 2048) h = 2048;
  CanvasBitmap bitmap(allocator, w, h);
  if (!bitmap.valid() || !draw_svg(svg, &bitmap)) return false;
  const ImageSource pixels = bitmap.snapshot();
  if (!pixels.valid() || !out->create(w, h)) return false;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) out->frame(0)[y * w + x] = pixels.pixels[y * pixels.pitch + x];
  return true;
}

}  // namespace browser
