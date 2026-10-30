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

// The fixed-function tessellator of Direct3D 11 (and OpenGL 4's primitive generator): from the tessellation factors of a patch it makes the domain points the domain shader turns into
// vertices, and the triangles (or lines, or points) joining them. Integer and power-of-two partitioning, for the triangle, quad and isoline domains.
//
// It follows the published Direct3D 11 reference tessellator: the outermost ring of points has as many segments as the edge factors say; inside it come rings with two segments less
// each, a triangle's rings concentric about the centre (so their points are not on the lattice of the outer ones), a quad's the rectangles of the lattice; the first inner ring is joined
// to the outer one by walking both in the order of the "ruler function", the rest by a fixed pattern. A triangle that the domain winds clockwise is made clockwise in the order of its
// indices (the output primitive says which way the pipeline wants them).
#pragma once
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"
#include "Utility/AllocatedView.hpp"

namespace graphics {

class Tessellator {
 public:
  struct Point {
    float u = 0.0f, v = 0.0f;   // (a triangle's w is 1 - u - v)
  };
  static constexpr uint32_t kMaxFactor = 64;
  // The most points and indices a patch can make (quad: 65 * 65 points, 2 * 64 * 64 triangles; triangle: fewer of both; isoline: 64 lines of 65 points).
  static constexpr uint32_t kMaxPoints = 65 * 65;
  static constexpr uint32_t kMaxIndices = 2 * kMaxFactor * kMaxFactor * 3;

  explicit Tessellator(allocators::IAllocator& allocator)
      : allocator_(allocator), points_(allocate_view<Point>(allocator, kMaxPoints)), indices_(allocate_view<uint16_t>(allocator, kMaxIndices)) {}
  ~Tessellator() {
    release_view(allocator_, points_);
    release_view(allocator_, indices_);
  }
  Tessellator(const Tessellator&) = delete;
  Tessellator& operator=(const Tessellator&) = delete;

  bool valid() const { return !points_.empty() && !indices_.empty(); }

  // Tessellates a patch. `outer` and `inner` are the factors the hull shader made (a triangle: three edges and one inside; a quad: four edges and two inside; an isoline: the line density and
  // the line detail). False when the patch is culled (a factor of an edge that is not above 0) or does not fit; then nothing is made.
  bool tessellate(TessDomain domain, TessPartitioning partitioning, TessOutput output, const float outer[4], const float inner[2]) {
    point_count_ = index_count_ = 0;
    failed_ = false;
    if (!valid()) return false;
    pow2_ = partitioning == TessPartitioning::Pow2;
    output_ = output;
    bool ok = false;
    switch (domain) {
      case TessDomain::Triangle: ok = triangle(outer, inner[0]); break;
      case TessDomain::Quad: ok = quad(outer, inner[0], inner[1]); break;
      case TessDomain::Isoline: ok = isoline(outer[0], outer[1]); break;
    }
    if (!ok || failed_) {
      point_count_ = index_count_ = 0;
      return false;
    }
    return true;
  }

  ArrayView<const Point> points() const { return points_.first(point_count_); }
  // Triangles (three indices each), lines (two) or points (one), by the output primitive.
  ArrayView<const uint16_t> indices() const { return indices_.first(index_count_); }

 private:
  // A run of points along one side of a ring: the ring's points are consecutive from `base`, in clockwise order, the side starts `start` points into them and has `count` points (the
  // last of which is the first of the next side, and the last side's last the ring's first, so the index wraps). A side may be walked backwards (the strip in the middle of a quad).
  struct Row {
    uint32_t base = 0, length = 1, start = 0, count = 0;
    bool reversed = false;
  };

  // --- Factors ---------------------------------------------------------------------------------------

  // A factor as the tessellator uses it: from 1 up to 64 (a smaller one, or NaN, is 1), rounded up (to a power of two for pow2 partitioning).
  uint32_t round_factor(float f) const {
    if (!(f > 1.0f)) return 1;
    if (f >= static_cast<float>(kMaxFactor)) return kMaxFactor;
    uint32_t c = static_cast<uint32_t>(f);
    if (static_cast<float>(c) < f) ++c;
    if (pow2_) {
      uint32_t p = 1;
      while (p < c) p <<= 1;
      c = p;
    }
    return c;
  }
  static bool culled(float f) { return !(f > 0.0f); }   // (NaN passes the test of "not above 0", as in Direct3D: culled)

  // --- Points and triangles --------------------------------------------------------------------------

  uint32_t add_point(float u, float v) {
    if (point_count_ >= points_.size()) {
      failed_ = true;
      return 0;
    }
    points_[point_count_] = {u, v};
    return point_count_++;
  }
  void add_index(uint32_t index) {
    if (index_count_ >= indices_.size() || index >= point_count_) {
      failed_ = true;
      return;
    }
    indices_[index_count_++] = static_cast<uint16_t>(index);
  }
  uint32_t at(const Row& row, uint32_t j) {
    if (j >= row.count || row.length == 0) {
      failed_ = true;
      return 0;
    }
    if (row.reversed) j = row.count - 1 - j;
    return row.base + (row.start + j) % row.length;
  }
  // A triangle that is clockwise in the domain (drawn with V down); the output primitive decides the order it is stored in. A domain shader that puts V up on the screen, as the
  // usual ones do, turns the domain's clockwise into counter-clockwise, so "triangle_cw" is the reverse of the order here (a real GPU, through virgl, culls the triangles of a
  // triangle_cw patch as back faces with Direct3D's default front face, which is what this order does too).
  void clockwise(uint32_t a, uint32_t b, uint32_t c) {
    add_index(a);
    if (output_ == TessOutput::TriangleCw) {
      add_index(c);
      add_index(b);
    } else {
      add_index(b);
      add_index(c);
    }
  }

  // --- Joining two rows ------------------------------------------------------------------------------

  // Two rows of points that have the same number apart from the two extra at the ends of the outer one (an inner ring's side and the one outside it): a corner triangle at each end and
  // quads between, the diagonals pointing from the outside toward the middle in the first half and back out in the second.
  void stitch_regular(const Row& outside, const Row& inside) {
    const uint32_t n = inside.count;
    if (n == 0 || outside.count != n + 2) {
      failed_ = true;
      return;
    }
    clockwise(at(outside, 0), at(outside, 1), at(inside, 0));
    uint32_t p = 0;
    for (; p < n / 2; ++p) {
      clockwise(at(outside, p + 1), at(inside, p + 1), at(inside, p));
      clockwise(at(outside, p + 1), at(outside, p + 2), at(inside, p + 1));
    }
    for (; p + 1 < n; ++p) {
      clockwise(at(inside, p), at(outside, p + 1), at(outside, p + 2));
      clockwise(at(inside, p), at(outside, p + 2), at(inside, p + 1));
    }
    clockwise(at(outside, n), at(outside, n + 1), at(inside, n - 1));
  }

  // The strip of quads in the middle of a quad patch (between the two long sides of the innermost ring): both rows run the same way and have the same number of points.
  // `except_middle`: the diagonals of the two halves point the other way from the one quad in the middle.
  void stitch_strip(const Row& outside, const Row& inside, bool except_middle) {
    const uint32_t n = inside.count;
    if (n < 2 || outside.count != n) {
      failed_ = true;
      return;
    }
    if (!except_middle) {
      for (uint32_t p = 0; p + 1 < n; ++p) {
        clockwise(at(inside, p), at(outside, p), at(outside, p + 1));
        clockwise(at(inside, p), at(outside, p + 1), at(inside, p + 1));
      }
      return;
    }
    uint32_t q = 0;
    for (; q + 1 < n / 2; ++q) {
      clockwise(at(outside, q), at(outside, q + 1), at(inside, q));
      clockwise(at(inside, q), at(outside, q + 1), at(inside, q + 1));
    }
    clockwise(at(outside, q), at(inside, q + 1), at(inside, q));
    clockwise(at(outside, q), at(outside, q + 1), at(inside, q + 1));
    for (++q; q + 1 < n; ++q) {
      clockwise(at(outside, q), at(outside, q + 1), at(inside, q));
      clockwise(at(inside, q), at(outside, q + 1), at(inside, q + 1));
    }
  }

  // The first inner ring against the outer one: the rows can have any numbers of points, so the points of both are walked in the order of the ruler function (the order in which the
  // points of an edge appear as its factor goes up: the middle first, then the quarters, ...) and a point of either row is taken when its turn comes. `*_half` is how many points the half
  // of the row has beyond the middle (the middle is the point in the middle of an even factor, and the pair about the middle of an odd one).
  void stitch_transition(const Row& outside, uint32_t outside_half, bool outside_odd, const Row& inside, uint32_t inside_half, bool inside_odd) {
    static const uint8_t kFinalPosition[33] = {0, 32, 16, 8, 17, 4, 18, 9, 19, 2, 20, 10, 21, 5, 22, 11, 23, 1, 24, 12, 25, 6, 26, 13, 27, 3, 28, 14, 29, 7, 30, 15, 31};
    static const uint8_t kLoopStart[33] = {1, 1, 17, 9, 9, 5, 5, 5, 5, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    static const uint8_t kLoopEnd[33] = {0, 0, 17, 17, 25, 25, 25, 25, 29, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 32};
    if (outside_half > 32 || inside_half > 32) {
      failed_ = true;
      return;
    }
    uint32_t op = 0, ip = 0;
    const uint32_t start = kLoopStart[inside_half] < kLoopStart[outside_half] ? kLoopStart[inside_half] : kLoopStart[outside_half];
    const uint32_t end = kLoopEnd[inside_half] > kLoopEnd[outside_half] ? kLoopEnd[inside_half] : kLoopEnd[outside_half];
    auto advance_outside = [&] {
      clockwise(at(outside, op), at(outside, op + 1), at(inside, ip));
      ++op;
    };
    auto advance_inside = [&] {
      clockwise(at(inside, ip), at(outside, op), at(inside, ip + 1));
      ++ip;
    };
    if (kFinalPosition[0] < outside_half) advance_outside();
    for (uint32_t i = start; i <= end; ++i) {
      if (kFinalPosition[i] < inside_half) advance_inside();
      if (kFinalPosition[i] < outside_half) advance_outside();
    }
    if (inside_odd != outside_odd || inside_odd) {
      if (inside_odd == outside_odd) {
        // (a quad in the middle)
        clockwise(at(inside, ip), at(outside, op), at(inside, ip + 1));
        clockwise(at(inside, ip + 1), at(outside, op), at(outside, op + 1));
        ++ip, ++op;
      } else if (!inside_odd) {
        // (a triangle pointing in)
        clockwise(at(inside, ip), at(outside, op), at(outside, op + 1));
        ++op;
      } else {
        // (a triangle pointing out)
        clockwise(at(inside, ip), at(outside, op), at(inside, ip + 1));
        ++ip;
      }
    }
    for (uint32_t i = end + 1; i-- > start;) {
      if (kFinalPosition[i] < outside_half) advance_outside();
      if (kFinalPosition[i] < inside_half) advance_inside();
    }
    if (kFinalPosition[0] < outside_half) advance_outside();
  }

  // --- Triangle domain -------------------------------------------------------------------------------

  struct Corner {
    float u, v;
  };
  // The points along a side from corner `a` to corner `b` in `segments` equal steps, all but the last (which starts the next side).
  void side_points(Corner a, Corner b, uint32_t segments) {
    for (uint32_t j = 0; j < segments; ++j) {
      const float t = static_cast<float>(j) / static_cast<float>(segments);
      add_point(a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t);
    }
  }

  bool triangle(const float outer[4], float inside_factor) {
    if (culled(outer[0]) || culled(outer[1]) || culled(outer[2])) return false;
    uint32_t edge[3];
    for (uint32_t e = 0; e < 3; ++e) edge[e] = round_factor(outer[e]);
    uint32_t inside = round_factor(inside_factor);
    // All factors 1: the patch itself.
    if (edge[0] == 1 && edge[1] == 1 && edge[2] == 1 && inside == 1) {
      const uint32_t a = add_point(0.0f, 1.0f), b = add_point(0.0f, 0.0f), c = add_point(1.0f, 0.0f);
      if (output_ == TessOutput::Point) {
        add_index(a), add_index(b), add_index(c);
      } else {
        clockwise(a, b, c);
      }
      return true;
    }
    if (inside == 1) inside = 2;   // (an inside factor of 1 next to bigger edge factors is taken as 2: a point in the middle)
    // The corners as (u, v): the V corner, the W corner and the U corner, the way round the outer ring goes.
    static const Corner kCorner[3] = {{0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}};
    uint32_t ring_base[34];
    uint32_t ring_length[34];
    uint32_t rings = 0;
    // The outer ring.
    ring_base[0] = point_count_;
    for (uint32_t e = 0; e < 3; ++e) side_points(kCorner[e], kCorner[(e + 1) % 3], edge[e]);
    ring_length[0] = edge[0] + edge[1] + edge[2];
    // The inner rings: triangles about the centre with two segments less on each side per ring, a point at the end if the factor is even.
    for (uint32_t k = 1; 2 * k <= inside && rings < 33; ++k) {
      const uint32_t segments = inside - 2 * k;
      ring_base[k] = point_count_;
      if (segments == 0) {
        add_point(1.0f / 3.0f, 1.0f / 3.0f);
        ring_length[k] = 1;
      } else {
        const float s = static_cast<float>(segments) / static_cast<float>(inside);
        Corner c[3];
        for (uint32_t i = 0; i < 3; ++i) {
          // (the corner moved toward the centre: a third plus two thirds of its distance for the coordinate it is the corner of, a third less a third of it for the others)
          c[i] = {1.0f / 3.0f + (kCorner[i].u - 1.0f / 3.0f) * s, 1.0f / 3.0f + (kCorner[i].v - 1.0f / 3.0f) * s};
        }
        for (uint32_t e = 0; e < 3; ++e) side_points(c[e], c[(e + 1) % 3], segments);
        ring_length[k] = 3 * segments;
      }
      rings = k;
    }
    if (failed_) return false;
    if (output_ == TessOutput::Point) {
      for (uint32_t i = 0; i < point_count_; ++i) add_index(i);
      return true;
    }
    // Outer ring against the first inner ring, then each inner ring against the next one in.
    const bool inside_odd = (inside & 1) != 0;
    uint32_t outer_start[3] = {0, edge[0], edge[0] + edge[1]};
    for (uint32_t k = 0; k < rings; ++k) {
      for (uint32_t e = 0; e < 3; ++e) {
        Row out_row, in_row;
        out_row.base = ring_base[k], out_row.length = ring_length[k];
        if (k == 0) {
          out_row.start = outer_start[e], out_row.count = edge[e] + 1;
        } else {
          const uint32_t m = inside - 2 * k + 1;   // points on a side of ring k
          out_row.start = e * (m - 1), out_row.count = m;
        }
        const uint32_t segments_in = inside - 2 * (k + 1);
        in_row.base = ring_base[k + 1], in_row.length = ring_length[k + 1];
        in_row.start = e * segments_in, in_row.count = segments_in + 1;
        if (k == 0) stitch_transition(out_row, edge[e] / 2, (edge[e] & 1) != 0, in_row, inside / 2, inside_odd);
        else stitch_regular(out_row, in_row);
      }
    }
    // An odd factor ends with a small triangle in the middle.
    if (inside_odd) {
      const uint32_t base = ring_base[rings];
      clockwise(base, base + 1, base + 2);
    }
    return !failed_;
  }

  // --- Quad domain -----------------------------------------------------------------------------------

  bool quad(const float outer[4], float inside_u, float inside_v) {
    if (culled(outer[0]) || culled(outer[1]) || culled(outer[2]) || culled(outer[3])) return false;
    uint32_t edge[4];
    for (uint32_t e = 0; e < 4; ++e) edge[e] = round_factor(outer[e]);
    uint32_t fu = round_factor(inside_u), fv = round_factor(inside_v);
    if (edge[0] == 1 && edge[1] == 1 && edge[2] == 1 && edge[3] == 1 && fu == 1 && fv == 1) {
      const uint32_t a = add_point(0.0f, 0.0f), b = add_point(1.0f, 0.0f), c = add_point(1.0f, 1.0f), d = add_point(0.0f, 1.0f);
      if (output_ == TessOutput::Point) {
        add_index(a), add_index(b), add_index(c), add_index(d);
      } else {
        clockwise(a, b, d);
        clockwise(b, c, d);
      }
      return true;
    }
    if (fu == 1) fu = 2;
    if (fv == 1) fv = 2;
    // The outer ring, clockwise from (0, 1): down the U = 0 edge, along the V = 0 edge, up the U = 1 edge, back along V = 1.
    uint32_t ring_base[34];
    uint32_t ring_length[34];
    uint32_t side_points_count[34][4];   // the points on each side of a ring
    uint32_t min_factor = fu < fv ? fu : fv;
    uint32_t rings = min_factor / 2;     // the inner rings
    ring_base[0] = point_count_;
    side_points(Corner{0.0f, 1.0f}, Corner{0.0f, 0.0f}, edge[0]);
    side_points(Corner{0.0f, 0.0f}, Corner{1.0f, 0.0f}, edge[1]);
    side_points(Corner{1.0f, 0.0f}, Corner{1.0f, 1.0f}, edge[2]);
    side_points(Corner{1.0f, 1.0f}, Corner{0.0f, 1.0f}, edge[3]);
    ring_length[0] = edge[0] + edge[1] + edge[2] + edge[3];
    for (uint32_t e = 0; e < 4; ++e) side_points_count[0][e] = edge[e] + 1;
    // The inner rings are the rectangles of the lattice (fu x fv), `k` in from the sides. A rectangle with no height or width is a line (or a point) walked there and back.
    for (uint32_t k = 1; k <= rings && k < 33; ++k) {
      const float u0 = static_cast<float>(k) / static_cast<float>(fu), u1 = static_cast<float>(fu - k) / static_cast<float>(fu);
      const float v0 = static_cast<float>(k) / static_cast<float>(fv), v1 = static_cast<float>(fv - k) / static_cast<float>(fv);
      const uint32_t wu = fu - 2 * k, wv = fv - 2 * k;   // segments across
      ring_base[k] = point_count_;
      side_points(Corner{u0, v1}, Corner{u0, v0}, wv);
      side_points(Corner{u0, v0}, Corner{u1, v0}, wu);
      side_points(Corner{u1, v0}, Corner{u1, v1}, wv);
      side_points(Corner{u1, v1}, Corner{u0, v1}, wu);
      uint32_t length = 2 * (wu + wv);
      if (length == 0) {
        // (a single point: the ring is that point, with every side one point long)
        add_point(u0, v0);
        length = 1;
      }
      ring_length[k] = length;
      side_points_count[k][0] = wv + 1, side_points_count[k][1] = wu + 1, side_points_count[k][2] = wv + 1, side_points_count[k][3] = wu + 1;
    }
    if (failed_) return false;
    if (output_ == TessOutput::Point) {
      for (uint32_t i = 0; i < point_count_; ++i) add_index(i);
      return true;
    }
    // Rows: side e of ring k.
    auto side_row = [&](uint32_t k, uint32_t e) {
      Row r;
      r.base = ring_base[k], r.length = ring_length[k], r.count = side_points_count[k][e];
      uint32_t start = 0;
      for (uint32_t t = 0; t < e; ++t) start += side_points_count[k][t] - 1;
      r.start = start;
      return r;
    };
    for (uint32_t k = 0; k < rings; ++k) {
      for (uint32_t e = 0; e < 4; ++e) {
        const Row out_row = side_row(k, e), in_row = side_row(k + 1, e);
        // (sides 0 and 2 run along V, 1 and 3 along U)
        const uint32_t f = (e & 1) ? fu : fv;
        if (k == 0) stitch_transition(out_row, edge[e] / 2, (edge[e] & 1) != 0, in_row, f / 2, (f & 1) != 0);
        else stitch_regular(out_row, in_row);
      }
    }
    // An odd smaller factor leaves a strip of quads inside the innermost ring.
    if (min_factor & 1) {
      const uint32_t k = rings;
      if (fu > fv) {
        // (long in U: the bottom side against the top one, both walked toward +U)
        Row bottom = side_row(k, 1), top = side_row(k, 3);
        top.reversed = true;
        stitch_strip(bottom, top, false);
      } else {
        // (long in V: the left side, walked toward -V, against the right one reversed)
        Row left = side_row(k, 0), right = side_row(k, 2);
        right.reversed = true;
        stitch_strip(left, right, (fv & 1) != 0);
      }
    }
    return !failed_;
  }

  // --- Isoline domain --------------------------------------------------------------------------------

  bool isoline(float density_factor, float detail_factor) {
    if (culled(density_factor) || culled(detail_factor)) return false;
    // (the density is always rounded up to an integer, even with power-of-two partitioning)
    const bool pow2 = pow2_;
    pow2_ = false;
    const uint32_t density = round_factor(density_factor);
    pow2_ = pow2;
    const uint32_t detail = round_factor(detail_factor);
    const uint32_t per_line = detail + 1;
    if (density * per_line > points_.size()) return false;
    for (uint32_t line = 0; line < density; ++line)
      for (uint32_t p = 0; p < per_line; ++p) add_point(static_cast<float>(p) / static_cast<float>(detail), static_cast<float>(line) / static_cast<float>(density));
    if (failed_) return false;
    if (output_ == TessOutput::Point) {
      for (uint32_t i = 0; i < point_count_; ++i) add_index(i);
      return !failed_;
    }
    for (uint32_t line = 0; line < density; ++line)
      for (uint32_t p = 1; p < per_line; ++p) {
        add_index(line * per_line + p - 1);
        add_index(line * per_line + p);
      }
    return !failed_;
  }

  allocators::IAllocator& allocator_;
  ArrayView<Point> points_;
  ArrayView<uint16_t> indices_;
  uint32_t point_count_ = 0, index_count_ = 0;
  bool failed_ = false, pow2_ = false;
  TessOutput output_ = TessOutput::TriangleCcw;
};

}  // namespace graphics
