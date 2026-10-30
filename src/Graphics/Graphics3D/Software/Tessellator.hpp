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
// vertices, and the triangles (or lines, or points) joining them. Integer, power-of-two and fractional partitioning, for the triangle, quad and isoline domains.
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
    partitioning_ = partitioning;
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

  // What a factor comes to once it is clamped and rounded: in 16.16 fixed point, as the hardware has it, with the parity of its partitioning (odd: the segments of the edge are an odd
  // number at whole factors; an inside factor of 1 is even). `points` is how many points an edge with the factor has, `half` how many of them there are up to the middle.
  struct Factor {
    bool odd = false;
    uint32_t points = 0, half = 0, split = 0, inv_floor = 0, inv_ceil = 0, fraction = 0;
    // (what the stitching of rings takes: the points of half an edge, without the middle one)
    uint32_t stitch_half() const { return odd ? half - 1 : half; }
  };
  static constexpr uint32_t kOne = 1u << 16, kHalf = 1u << 15;

  static uint32_t fixed_ceil(uint32_t v) { return (v & 0xFFFF) ? (v & ~0xFFFFu) + kOne : v; }
  static uint32_t remove_msb(uint32_t v) {
    uint32_t bit = 0x80000000u;
    while (bit && !(v & bit)) bit >>= 1;
    return v & ~bit;
  }
  static uint32_t reciprocal(uint32_t n) { return n == 0 ? 0xFFFFFFFFu : (2 * kOne + n) / (2 * n); }   // (65536 / n, rounded)

  Factor make_factor(uint32_t fxp, bool odd) const {
    Factor f;
    f.odd = odd;
    // The points: an odd factor has an even number of them, an even one an odd number.
    const uint32_t half_factor = (fxp + 1) / 2;
    f.points = odd ? (fixed_ceil(kHalf + half_factor) * 2) >> 16 : ((fixed_ceil(half_factor) * 2) >> 16) + 1;
    uint32_t half = half_factor;
    if (odd || half == kHalf) half += kHalf;   // (an even factor of 1 is taken as 2)
    const uint32_t floor_half = half & ~0xFFFFu, ceil_half = fixed_ceil(half);
    f.fraction = half - floor_half;
    f.half = ceil_half >> 16;
    if (ceil_half == floor_half) f.split = f.half + 1;
    else if (odd) f.split = floor_half == kOne ? 0 : (remove_msb((floor_half >> 16) - 1) << 1) + 1;
    else f.split = (remove_msb(floor_half >> 16) << 1) + 1;
    uint32_t floor_segments = (floor_half * 2) >> 16, ceil_segments = (ceil_half * 2) >> 16;
    if (odd) --floor_segments, --ceil_segments;
    f.inv_floor = reciprocal(floor_segments), f.inv_ceil = reciprocal(ceil_segments);
    return f;
  }
  // Where point `point` of an edge with the factor is, from 0 to 1: the points of the first half are between those the next lower whole factor and the next higher one would put there, by how
  // close the factor is to each; the second half mirrors the first.
  float place(const Factor& f, uint32_t point) const {
    bool flip = false;
    if (point >= f.half) {
      point = (f.half << 1) - point;
      if (f.odd) point -= 1;
      flip = true;
    }
    if (point == f.half) return 0.5f;
    uint32_t on_floor = point;
    if (point > f.split) on_floor -= 1;
    const uint64_t at_floor = uint64_t{on_floor} * f.inv_floor, at_ceil = uint64_t{point} * f.inv_ceil;
    uint32_t location = static_cast<uint32_t>((at_floor * (kOne - f.fraction) + at_ceil * f.fraction + kHalf) >> 16);
    if (flip) location = kOne - location;
    return static_cast<float>(location) / static_cast<float>(kOne);
  }

  bool fractional() const { return partitioning_ == TessPartitioning::FractionalOdd || partitioning_ == TessPartitioning::FractionalEven; }
  bool fraction_odd() const { return partitioning_ == TessPartitioning::FractionalOdd; }
  static uint32_t to_fixed(float f) { return static_cast<uint32_t>(f * 65536.0f + 0.5f); }

  // The factor of an edge: clamped (1 up to 64; odd partitioning up to 63, even from 2), rounded up (to a power of two) unless fractional.
  Factor edge_factor(float f, uint32_t* whole = nullptr) const {
    float lo = 1.0f, hi = 64.0f;
    if (partitioning_ == TessPartitioning::FractionalOdd) hi = 63.0f;
    if (partitioning_ == TessPartitioning::FractionalEven) lo = 2.0f;
    if (!(f >= lo)) f = lo;
    if (f > hi) f = hi;
    if (!fractional()) {
      const uint32_t n = round_factor(f);
      if (whole) *whole = n;
      return make_factor(n << 16, (n & 1) != 0);
    }
    return make_factor(to_fixed(f), fraction_odd());
  }
  // An inside factor, the same but an even whole factor of 1 stands for 2; `frame_lo` raises the lowest value of odd partitioning a little so that its inside has a ring.
  Factor inside_factor(float f, bool frame) const {
    float lo = 1.0f, hi = 64.0f;
    if (partitioning_ == TessPartitioning::FractionalOdd) hi = 63.0f, lo = frame ? 1.0f + 1.0f / 65536.0f : 1.0f;
    if (partitioning_ == TessPartitioning::FractionalEven) lo = 2.0f;
    if (!(f >= lo)) f = lo;
    if (f > hi) f = hi;
    if (!fractional()) {
      const uint32_t n = round_factor(f);
      return make_factor(n << 16, !((n & 1) == 0 || n == 1));
    }
    return make_factor(to_fixed(f), fraction_odd());
  }
  // A factor that comes to 1 once clamped (the case of every factor being 1).
  bool is_one(float f) const {
    if (fractional() && !fraction_odd()) return false;   // (even partitioning has 2 at least)
    return to_fixed(f >= 1.0f ? (f > 64.0f ? 64.0f : f) : 1.0f) == kOne && !(fractional() && f > 1.0f + 0.5f / 65536.0f);
  }
  // A factor as a whole number (integer and power-of-two partitioning): from 1 up to 64 (a smaller one, or NaN, is 1), rounded up (to a power of two for pow2 partitioning).
  uint32_t round_factor(float f) const {
    if (!(f > 1.0f)) return 1;
    if (f >= static_cast<float>(kMaxFactor)) return kMaxFactor;
    uint32_t c = static_cast<uint32_t>(f);
    if (static_cast<float>(c) < f) ++c;
    if (partitioning_ == TessPartitioning::Pow2) {
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

  bool triangle(const float outer[4], float inside_input) {
    if (culled(outer[0]) || culled(outer[1]) || culled(outer[2])) return false;
    Factor edge[3];
    bool any_above_one = false;
    for (uint32_t e = 0; e < 3; ++e) {
      edge[e] = edge_factor(outer[e]);
      any_above_one |= outer[e] > 1.0f + 0.5f / 65536.0f;
    }
    // (odd partitioning: if any edge has more than one segment the inside has a ring, a "picture frame")
    const Factor inside = inside_factor(inside_input, any_above_one);
    if (is_one(outer[0]) && is_one(outer[1]) && is_one(outer[2]) && is_one(inside_input) && (partitioning_ != TessPartitioning::FractionalOdd || true)) {
      // All factors 1: the patch itself.
      const uint32_t a = add_point(0.0f, 1.0f), b = add_point(0.0f, 0.0f), c = add_point(1.0f, 0.0f);
      if (output_ == TessOutput::Point) {
        add_index(a), add_index(b), add_index(c);
      } else {
        clockwise(a, b, c);
      }
      return true;
    }
    const uint32_t min_points = inside.odd ? 4 : 3;
    const uint32_t inside_points = inside.points > min_points ? inside.points : min_points;
    uint32_t ring_base[40];
    uint32_t ring_length[40];
    uint32_t rings = 0;
    // The outer ring, clockwise from the V corner: down the u = 0 edge, along v = 0, back up the w = 0 edge.
    ring_base[0] = point_count_;
    uint32_t outer_start[3];
    uint32_t length = 0;
    for (uint32_t e = 0; e < 3; ++e) {
      outer_start[e] = length;
      const uint32_t n = edge[e].points;
      for (uint32_t p = 0; p + 1 < n; ++p) {
        const float location = place(edge[e], e == 1 ? p : n - 1 - p);
        if (e == 0) add_point(0.0f, location);
        else if (e == 1) add_point(location, 0.0f);
        else add_point(location, 1.0f - location);
      }
      length += n - 1;
    }
    ring_length[0] = length;
    // The inner rings: triangles about the centre with two points less on each side per ring (their points are placed along the sides as the inside factor says, pushed in toward the centre).
    for (uint32_t k = 1; 2 * k + 1 <= inside_points && rings < 38; ++k) {
      const uint32_t m = inside_points - 2 * k;   // points on a side
      ring_base[k] = point_count_;
      if (m == 1) {
        add_point(1.0f / 3.0f, 1.0f / 3.0f);
        ring_length[k] = 1;
      } else {
        const uint32_t end = inside_points - 1 - k;
        const float perp = place(inside, k) * (2.0f / 3.0f);
        for (uint32_t e = 0; e < 3; ++e)
          for (uint32_t p = k; p < end; ++p) {
            const float param = place(inside, e == 1 ? p : end - (p - k));
            const float along = param - perp * 0.5f;
            if (e == 0) add_point(perp, along);
            else if (e == 1) add_point(along, perp);
            else add_point(along, 1.0f - along - perp);
          }
        ring_length[k] = 3 * (m - 1);
      }
      rings = k;
    }
    if (failed_) return false;
    if (output_ == TessOutput::Point) {
      for (uint32_t i = 0; i < point_count_; ++i) add_index(i);
      return true;
    }
    // Outer ring against the first inner ring, then each inner ring against the next one in.
    for (uint32_t k = 0; k < rings; ++k) {
      for (uint32_t e = 0; e < 3; ++e) {
        Row out_row, in_row;
        out_row.base = ring_base[k], out_row.length = ring_length[k];
        if (k == 0) {
          out_row.start = outer_start[e], out_row.count = edge[e].points;
        } else {
          const uint32_t m = inside_points - 2 * k;   // points on a side of ring k
          out_row.start = e * (m - 1), out_row.count = m;
        }
        const uint32_t m_in = inside_points - 2 * (k + 1);
        in_row.base = ring_base[k + 1], in_row.length = ring_length[k + 1];
        in_row.start = e * (m_in - 1), in_row.count = m_in;
        if (k == 0) stitch_transition(out_row, edge[e].stitch_half(), edge[e].odd, in_row, inside.stitch_half(), inside.odd);
        else stitch_regular(out_row, in_row);
      }
    }
    // An odd factor ends with a small triangle in the middle.
    if (inside.odd) {
      const uint32_t base = ring_base[rings];
      clockwise(base, base + 1, base + 2);
    }
    return !failed_;
  }

  // --- Quad domain -----------------------------------------------------------------------------------

  bool quad(const float outer[4], float inside_u_input, float inside_v_input) {
    if (culled(outer[0]) || culled(outer[1]) || culled(outer[2]) || culled(outer[3])) return false;
    Factor edge[4];
    bool any_above_one = inside_u_input > 1.0f + 0.5f / 65536.0f || inside_v_input > 1.0f + 0.5f / 65536.0f;
    for (uint32_t e = 0; e < 4; ++e) {
      edge[e] = edge_factor(outer[e]);
      any_above_one |= outer[e] > 1.0f + 0.5f / 65536.0f;
    }
    const Factor inside_u = inside_factor(inside_u_input, any_above_one), inside_v = inside_factor(inside_v_input, any_above_one);
    if (is_one(outer[0]) && is_one(outer[1]) && is_one(outer[2]) && is_one(outer[3]) && is_one(inside_u_input) && is_one(inside_v_input)) {
      const uint32_t a = add_point(0.0f, 0.0f), b = add_point(1.0f, 0.0f), c = add_point(1.0f, 1.0f), d = add_point(0.0f, 1.0f);
      if (output_ == TessOutput::Point) {
        add_index(a), add_index(b), add_index(c), add_index(d);
      } else {
        clockwise(a, b, d);
        clockwise(b, c, d);
      }
      return true;
    }
    const uint32_t points_u = inside_u.points > (inside_u.odd ? 4u : 3u) ? inside_u.points : (inside_u.odd ? 4u : 3u);
    const uint32_t points_v = inside_v.points > (inside_v.odd ? 4u : 3u) ? inside_v.points : (inside_v.odd ? 4u : 3u);
    const uint32_t min_points = points_u < points_v ? points_u : points_v;
    const uint32_t rings = ((min_points + 1) >> 1) - 1;   // the inner rings
    uint32_t ring_base[40];
    uint32_t ring_length[40];
    uint32_t side_points_count[40][4];   // the points on each side of a ring
    if (rings >= 38) return false;
    // The outer ring, clockwise from (0, 1): down the U = 0 edge, along the V = 0 edge, up the U = 1 edge, back along V = 1.
    ring_base[0] = point_count_;
    uint32_t length = 0;
    for (uint32_t e = 0; e < 4; ++e) {
      const uint32_t n = edge[e].points;
      side_points_count[0][e] = n;
      for (uint32_t p = 0; p + 1 < n; ++p) {
        const float location = place(edge[e], (e == 1 || e == 2) ? p : n - 1 - p);
        if (e == 0) add_point(0.0f, location);
        else if (e == 1) add_point(location, 0.0f);
        else if (e == 2) add_point(1.0f, location);
        else add_point(location, 1.0f);
      }
      length += n - 1;
    }
    ring_length[0] = length;
    // The inner rings are the rectangles of the lattice (the points of each axis as its inside factor places them), `k` in from the sides. A rectangle with no height or width is a line
    // (or a point) walked there and back.
    for (uint32_t k = 1; k <= rings; ++k) {
      const uint32_t i0 = k, i1 = points_u - 1 - k, j0 = k, j1 = points_v - 1 - k;
      const uint32_t wu = i1 - i0, wv = j1 - j0;   // segments across
      auto u_at = [&](uint32_t i) { return place(inside_u, i); };
      auto v_at = [&](uint32_t j) { return place(inside_v, j); };
      ring_base[k] = point_count_;
      for (uint32_t t = 0; t < wv; ++t) add_point(u_at(i0), v_at(j1 - t));
      for (uint32_t t = 0; t < wu; ++t) add_point(u_at(i0 + t), v_at(j0));
      for (uint32_t t = 0; t < wv; ++t) add_point(u_at(i1), v_at(j0 + t));
      for (uint32_t t = 0; t < wu; ++t) add_point(u_at(i1 - t), v_at(j1));
      uint32_t ring_points = 2 * (wu + wv);
      if (ring_points == 0) {
        // (a single point: the ring is that point, with every side one point long)
        add_point(u_at(i0), v_at(j0));
        ring_points = 1;
      }
      ring_length[k] = ring_points;
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
        const Factor& along = (e & 1) ? inside_u : inside_v;
        if (k == 0) stitch_transition(out_row, edge[e].stitch_half(), edge[e].odd, in_row, along.stitch_half(), along.odd);
        else stitch_regular(out_row, in_row);
      }
    }
    // An odd smaller factor leaves a strip of quads inside the innermost ring.
    if (points_u > points_v && inside_v.odd) {
      // (long in U: the bottom side against the top one, both walked toward +U)
      Row bottom = side_row(rings, 1), top = side_row(rings, 3);
      top.reversed = true;
      stitch_strip(bottom, top, false);
    } else if (points_v >= points_u && inside_u.odd) {
      // (long in V: the left side, walked toward -V, against the right one reversed)
      Row left = side_row(rings, 0), right = side_row(rings, 2);
      right.reversed = true;
      stitch_strip(left, right, inside_v.odd);
    }
    return !failed_;
  }

  // --- Isoline domain --------------------------------------------------------------------------------

  bool isoline(float density_factor, float detail_factor) {
    if (culled(density_factor) || culled(detail_factor)) return false;
    // (the density is always rounded up to a whole number of lines, even with power-of-two partitioning)
    const TessPartitioning partitioning = partitioning_;
    partitioning_ = TessPartitioning::Integer;
    const uint32_t density = round_factor(density_factor);
    partitioning_ = partitioning;
    const Factor detail = edge_factor(detail_factor);
    const uint32_t per_line = detail.points;
    if (density * per_line > points_.size()) return false;
    for (uint32_t line = 0; line < density; ++line)
      for (uint32_t p = 0; p < per_line; ++p) add_point(place(detail, p), static_cast<float>(line) / static_cast<float>(density));
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
  bool failed_ = false;
  TessPartitioning partitioning_ = TessPartitioning::Integer;
  TessOutput output_ = TessOutput::TriangleCcw;
};

}  // namespace graphics
