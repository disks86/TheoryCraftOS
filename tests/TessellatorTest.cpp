// Host-side tests for the fixed-function tessellator (src/Graphics/Graphics3D/Software/Tessellator.hpp): for every domain and a spread of factors, the triangles must tile the domain
// exactly once (their areas add up, no edge is left open inside, none is shared the same way twice), all wind the same way, and the outer edge has the segments the edge factors say.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <utility>
#include "Graphics/Graphics3D/Software/Tessellator.hpp"

using namespace graphics;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

typedef std::pair<long, long> Key;
static Key key_of(const Tessellator::Point& p) { return {lround(p.u * 100000.0), lround(p.v * 100000.0)}; }

struct Stats {
  double area = 0.0;          // the sum of the absolute areas
  double signed_area = 0.0;
  int positive = 0, negative = 0, zero = 0;
  int triangles = 0;
  int open_edges = 0;         // directed edges with no opposite one
  int open_off_boundary = 0;  // ... of which not on the boundary of the domain
  int boundary_edges = 0;
  int duplicated = 0;         // a directed edge made twice
};

static Stats measure(const Tessellator& t, TessDomain domain) {
  Stats s;
  auto pts = t.points();
  auto idx = t.indices();
  std::map<std::pair<Key, Key>, int> edges;
  for (uint64_t i = 0; i + 3 <= idx.size(); i += 3) {
    const Tessellator::Point a = pts[idx[i]], b = pts[idx[i + 1]], c = pts[idx[i + 2]];
    const double area = 0.5 * ((b.u - a.u) * (c.v - a.v) - (c.u - a.u) * (b.v - a.v));
    s.signed_area += area;
    s.area += fabs(area);
    if (area > 1e-9) ++s.positive; else if (area < -1e-9) ++s.negative; else ++s.zero;
    ++s.triangles;
    const Key k[3] = {key_of(a), key_of(b), key_of(c)};
    for (int e = 0; e < 3; ++e) ++edges[{k[e], k[(e + 1) % 3]}];
  }
  for (auto& kv : edges) {
    if (kv.second > 1) ++s.duplicated;
    if (edges.count({kv.first.second, kv.first.first}) == 0) {
      ++s.open_edges;
      const double u0 = kv.first.first.first / 100000.0, v0 = kv.first.first.second / 100000.0, u1 = kv.first.second.first / 100000.0, v1 = kv.first.second.second / 100000.0;
      bool on = false;
      if (domain == TessDomain::Triangle) on = (fabs(u0) < 1e-4 && fabs(u1) < 1e-4) || (fabs(v0) < 1e-4 && fabs(v1) < 1e-4) || (fabs(u0 + v0 - 1) < 1e-4 && fabs(u1 + v1 - 1) < 1e-4);
      else on = (fabs(u0) < 1e-4 && fabs(u1) < 1e-4) || (fabs(v0) < 1e-4 && fabs(v1) < 1e-4) || (fabs(u0 - 1) < 1e-4 && fabs(u1 - 1) < 1e-4) || (fabs(v0 - 1) < 1e-4 && fabs(v1 - 1) < 1e-4);
      if (!on) ++s.open_off_boundary;
      ++s.boundary_edges;
    }
  }
  return s;
}

static uint32_t rounded(float f) { return f <= 1.0f ? 1 : static_cast<uint32_t>(ceilf(f)); }

int main() {
  TestAllocator allocator;
  {
    Tessellator t(allocator);
    CHECK(t.valid());
    const float factors[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 17, 31, 64};
    // Triangles: three edge factors and the inside factor from the list (not every combination: the first, one of each parity apart, and some that differ a lot).
    int cases = 0;
    for (float a : factors)
      for (float b : {1.0f, 2.0f, 3.0f, 6.0f, 9.0f})
        for (float c : {1.0f, 4.0f, 5.0f})
          for (float in : factors) {
            const float outer[4] = {a, b, c, 0}, inner[2] = {in, 0};
            for (TessOutput out : {TessOutput::TriangleCw, TessOutput::TriangleCcw}) {
              CHECK(t.tessellate(TessDomain::Triangle, TessPartitioning::Integer, out, outer, inner));
              const Stats s = measure(t, TessDomain::Triangle);
              const bool ok = fabs(s.area - 0.5) < 1e-4 && s.zero == 0 && (s.positive == 0 || s.negative == 0) && s.duplicated == 0 && s.open_off_boundary == 0 &&
                              s.boundary_edges == static_cast<int>(rounded(a) + rounded(b) + rounded(c));
              if (!ok) printf("triangle %g %g %g inside %g: area %f (+%d -%d 0:%d) dup %d open %d off %d boundary %d\n", a, b, c, in, s.area, s.positive, s.negative, s.zero, s.duplicated, s.open_edges, s.open_off_boundary, s.boundary_edges);
              CHECK(ok);
              ++cases;
              if (out == TessOutput::TriangleCw) CHECK(s.negative == 0 || s.positive == 0);
            }
          }
    printf("triangle cases: %d\n", cases);
    // The two windings are opposite.
    {
      const float outer[4] = {4, 4, 4, 0}, inner[2] = {4, 0};
      t.tessellate(TessDomain::Triangle, TessPartitioning::Integer, TessOutput::TriangleCw, outer, inner);
      const double cw = measure(t, TessDomain::Triangle).signed_area;
      t.tessellate(TessDomain::Triangle, TessPartitioning::Integer, TessOutput::TriangleCcw, outer, inner);
      const double ccw = measure(t, TessDomain::Triangle).signed_area;
      CHECK(cw * ccw < 0);
    }
    // The point counts of a uniform triangle: the outer ring 3N points, then rings of 3(N - 2k), and a centre point if N is even.
    {
      const float outer[4] = {4, 4, 4, 0}, inner[2] = {4, 0};
      t.tessellate(TessDomain::Triangle, TessPartitioning::Integer, TessOutput::TriangleCw, outer, inner);
      CHECK(t.points().size() == 12 + 6 + 1);
      CHECK(t.indices().size() == 3 * 24);   // (18 between the outer ring and the first inner one, 6 around the centre)
      // (the centre point is in the middle)
      bool centre = false;
      for (const Tessellator::Point& p : t.points()) centre |= fabs(p.u - 1.0f / 3) < 1e-5 && fabs(p.v - 1.0f / 3) < 1e-5;
      CHECK(centre);
      // (the inner ring of 4 is the triangle of half the size about the centre: its corner is (1/6, 2/3))
      bool corner = false;
      for (const Tessellator::Point& p : t.points()) corner |= fabs(p.u - 1.0f / 6) < 1e-5 && fabs(p.v - 2.0f / 3) < 1e-5;
      CHECK(corner);
    }
    // Factors that cull, and factors that round.
    {
      const float zero[4] = {0, 1, 1, 0}, inner[2] = {1, 0};
      CHECK(!t.tessellate(TessDomain::Triangle, TessPartitioning::Integer, TessOutput::TriangleCw, zero, inner));
      CHECK(t.points().empty() && t.indices().empty());
      const float nan[4] = {NAN, 1, 1, 0};
      CHECK(!t.tessellate(TessDomain::Triangle, TessPartitioning::Integer, TessOutput::TriangleCw, nan, inner));
      const float ones[4] = {0.3f, 1, 1, 0};   // (below 1 but above 0: 1)
      CHECK(!t.tessellate(TessDomain::Triangle, TessPartitioning::Integer, TessOutput::TriangleCw, zero, inner));
      CHECK(t.tessellate(TessDomain::Triangle, TessPartitioning::Integer, TessOutput::TriangleCw, ones, inner));
      CHECK(t.points().size() == 3 && t.indices().size() == 3);
      const float p3[4] = {3, 3, 3, 0}, in3[2] = {3, 0};
      CHECK(t.tessellate(TessDomain::Triangle, TessPartitioning::Pow2, TessOutput::TriangleCw, p3, in3));
      const Stats s = measure(t, TessDomain::Triangle);
      CHECK(s.boundary_edges == 12);   // (3 rounds up to 4)
      const float f21[4] = {2.1f, 2.1f, 2.1f, 0}, i21[2] = {2.1f, 0};
      CHECK(t.tessellate(TessDomain::Triangle, TessPartitioning::Integer, TessOutput::TriangleCw, f21, i21));
      CHECK(measure(t, TessDomain::Triangle).boundary_edges == 9);
    }
    // Point output: every point once.
    {
      const float outer[4] = {5, 5, 5, 0}, inner[2] = {5, 0};
      CHECK(t.tessellate(TessDomain::Triangle, TessPartitioning::Integer, TessOutput::Point, outer, inner));
      CHECK(t.indices().size() == t.points().size());
    }
    // Quads.
    cases = 0;
    for (float e0 : factors)
      for (float e1 : {1.0f, 2.0f, 3.0f, 8.0f})
        for (float e2 : {1.0f, 5.0f})
          for (float e3 : {2.0f, 3.0f})
            for (float iu : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 9.0f})
              for (float iv : {1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 7.0f, 9.0f}) {
                const float outer[4] = {e0, e1, e2, e3}, inner[2] = {iu, iv};
                CHECK(t.tessellate(TessDomain::Quad, TessPartitioning::Integer, TessOutput::TriangleCw, outer, inner));
                const Stats s = measure(t, TessDomain::Quad);
                const bool ok = fabs(s.area - 1.0) < 1e-4 && s.zero == 0 && (s.positive == 0 || s.negative == 0) && s.duplicated == 0 && s.open_off_boundary == 0 &&
                                s.boundary_edges == static_cast<int>(rounded(e0) + rounded(e1) + rounded(e2) + rounded(e3));
                if (!ok) printf("quad %g %g %g %g inside %g %g: area %f (+%d -%d 0:%d) dup %d open %d off %d boundary %d\n", e0, e1, e2, e3, iu, iv, s.area, s.positive, s.negative, s.zero, s.duplicated, s.open_edges, s.open_off_boundary, s.boundary_edges);
                CHECK(ok);
                ++cases;
              }
    printf("quad cases: %d\n", cases);
    // A quad of 64 everywhere uses all the room.
    {
      const float outer[4] = {64, 64, 64, 64}, inner[2] = {64, 64};
      CHECK(t.tessellate(TessDomain::Quad, TessPartitioning::Integer, TessOutput::TriangleCw, outer, inner));
      CHECK(t.indices().size() == 2 * 64 * 64 * 3);
      const float big[4] = {1000, 1000, 1000, 1000}, bigin[2] = {1000, 1000};
      CHECK(t.tessellate(TessDomain::Quad, TessPartitioning::Integer, TessOutput::TriangleCw, big, bigin));
      CHECK(t.indices().size() == 2 * 64 * 64 * 3);
    }
    // Isolines: `density` lines of `detail` segments.
    {
      const float outer[4] = {3, 4, 0, 0}, inner[2] = {0, 0};
      CHECK(t.tessellate(TessDomain::Isoline, TessPartitioning::Integer, TessOutput::Line, outer, inner));
      CHECK(t.points().size() == 3 * 5 && t.indices().size() == 3 * 4 * 2);
      CHECK(fabs(t.points()[5].v - 1.0f / 3) < 1e-6 && fabs(t.points()[7].u - 0.5f) < 1e-6);
      const float p2[4] = {3, 3, 0, 0};   // (power of two: the detail 3 becomes 4, the density stays 3)
      CHECK(t.tessellate(TessDomain::Isoline, TessPartitioning::Pow2, TessOutput::Line, p2, inner));
      CHECK(t.points().size() == 3 * 5);
      CHECK(t.tessellate(TessDomain::Isoline, TessPartitioning::Integer, TessOutput::Point, outer, inner));
      CHECK(t.indices().size() == 15);
      const float cull[4] = {0, 4, 0, 0};
      CHECK(!t.tessellate(TessDomain::Isoline, TessPartitioning::Integer, TessOutput::Line, cull, inner));
    }
  }
  CHECK(allocator.live == 0);
  if (g_failed) {
    printf("TessellatorTest: %d failed\n", g_failed);
    return 1;
  }
  printf("TessellatorTest: all passed\n");
  return 0;
}
