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

// Draw calls run on the wave emulator (GcnDraw): TGSI shaders compiled by the GCN compiler, vertices fetched through V#s, triangles rasterized, pixels shaded 64 to a wave. The image
// is checked against an independent computation (double precision barycentrics from the triangle's areas, no edge-function rules) for pixels away from the edges, the coverage
// against the area, a quad of two triangles for the top-left rule (every pixel shaded once), a perspective triangle for the perspective-correct interpolation, and a texture.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnDraw.hpp"

using namespace gcn;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

static const char* kVertex =
    "VERT\nDCL IN[0]\nDCL IN[1]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\nDCL CONST[0..3]\nDCL TEMP[0]\n"
    "  0: MUL TEMP[0], CONST[0], IN[0].xxxx\n  1: MAD TEMP[0], CONST[1], IN[0].yyyy, TEMP[0]\n  2: MAD TEMP[0], CONST[2], IN[0].zzzz, TEMP[0]\n  3: ADD OUT[0], TEMP[0], CONST[3]\n  4: MOV OUT[1], IN[1]\n  5: END\n";
static const char* kColoredFragment = "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\n  0: MOV OUT[0], IN[0]\n  1: END\n";
static const char* kTexturedFragment = "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL SAMP[0]\nDCL SVIEW[0], 2D, FLOAT\n  0: TEX OUT[0], IN[0], SAMP[0], 2D\n  1: END\n";

struct Compiled {
  List<uint32_t> words;
  ShaderInfo info;
  explicit Compiled(allocators::IAllocator& a) : words(a) {}
  bool ok = false;
};

static void compile(allocators::IAllocator& alloc, const char* text, Compiled* out) {
  TgsiShader sh(alloc);
  const char* error = nullptr;
  uint32_t line = 0;
  if (!parse_tgsi(alloc, StringView(text), &sh, &error, &line)) { printf("parse error %s\n", error); return; }
  Code code(alloc);
  if (!compile_tgsi(sh, code, &out->info, &error)) { printf("compile error %s\n", error); return; }
  for (const uint32_t w : code.words()) out->words.push_back(w);
  out->ok = true;
}

static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static const float kDepthIsW[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};            // w = z

struct Vertex7 { float p[3]; float c[4]; };

struct Fixture {
  TestAllocator alloc;
  Compiled vs, fs, tfs;
  Fixture() : vs(alloc), fs(alloc), tfs(alloc) {
    compile(alloc, kVertex, &vs);
    compile(alloc, kColoredFragment, &fs);
    compile(alloc, kTexturedFragment, &tfs);
  }
};

static DrawCall make_call(const Fixture& f, const Compiled& ps, const float* mvp, const uint32_t* vdata, uint32_t vwords, uint32_t stride, uint32_t comps1, const uint32_t* indices, uint32_t index_count) {
  DrawCall c;
  c.vertex_code = ArrayView<const uint32_t>(f.vs.words.view().data(), f.vs.words.count());
  c.pixel_code = ArrayView<const uint32_t>(ps.words.view().data(), ps.words.count());
  c.vertex_info = f.vs.info;
  c.pixel_info = ps.info;
  c.vertex_constants = ArrayView<const float>(mvp, 16);
  c.vertex_data = ArrayView<const uint32_t>(vdata, vwords);
  c.vertex_stride = stride;
  c.attributes[0] = {3, 0};
  c.attributes[1] = {comps1, 12};
  c.indices = ArrayView<const uint32_t>(indices, index_count);
  return c;
}

static const uint32_t* as_words(const Vertex7* v) { return reinterpret_cast<const uint32_t*>(v); }       // (floats and words are the same size and alignment)

// The reference colour at pixel centre (x, y) of the triangle, or false if the centre is within `margin` of an edge or outside. Positions are NDC, w the clip w of each vertex.
static bool reference(const Vertex7 v[3], const float w[3], uint32_t W, uint32_t H, uint32_t x, uint32_t y, double margin, double out[4], bool* inside_loose) {
  double sx[3], sy[3];
  for (int k = 0; k < 3; ++k) {
    sx[k] = (v[k].p[0] / w[k] * 0.5 + 0.5) * W;
    sy[k] = (0.5 - v[k].p[1] / w[k] * 0.5) * H;
  }
  const double px = x + 0.5, py = y + 0.5;
  const double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
  const double l0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
  const double l1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
  const double l2 = 1.0 - l0 - l1;
  *inside_loose = l0 >= -margin && l1 >= -margin && l2 >= -margin;
  if (!(l0 > margin && l1 > margin && l2 > margin)) return false;
  const double q0 = l0 / w[0], q1 = l1 / w[1], q2 = l2 / w[2], s = q0 + q1 + q2;
  for (int c = 0; c < 4; ++c) out[c] = (q0 * v[0].c[c] + q1 * v[1].c[c] + q2 * v[2].c[c]) / s;
  return true;
}

static void check_image(const char* name, const Framebuffer& fb, const Vertex7 v[3], const float w[3], double tolerance) {
  uint32_t inside = 0, bad = 0;
  for (uint32_t y = 0; y < fb.height(); ++y)
    for (uint32_t x = 0; x < fb.width(); ++x) {
      double want[4];
      bool loose;
      const bool in = reference(v, w, fb.width(), fb.height(), x, y, 0.002, want, &loose);
      const float* got = fb.pixel(x, y);
      if (in) {
        ++inside;
        for (int c = 0; c < 4; ++c)
          if (fabs(got[c] - want[c]) > tolerance && bad++ < 5) printf("  %s: pixel %u,%u comp %d got %f want %f\n", name, x, y, c, got[c], want[c]);
      } else if (!loose) {
        if (got[0] != -1.0f || got[3] != -1.0f) { if (bad++ < 5) printf("  %s: pixel %u,%u outside but drawn (%f)\n", name, x, y, got[0]); }
      }
    }
  if (bad || inside < 20) { printf("FAILED %s: %u bad pixels, %u inside\n", name, bad, inside); ++g_failed; }
}

static void test_triangle(Fixture& f) {
  const uint32_t W = 40, H = 30;
  Vertex7 v[3] = {{{-0.8f, -0.7f, 0.5f}, {1, 0, 0, 1}}, {{0.9f, -0.5f, 0.5f}, {0, 1, 0, 0.5f}}, {{-0.1f, 0.8f, 0.5f}, {0, 0, 1, 0.25f}}};
  const uint32_t idx[3] = {0, 1, 2};
  const float w1[3] = {1, 1, 1};
  for (int winding = 0; winding < 2; ++winding) {
    const uint32_t idx2[3] = {0, 2, 1};
    Framebuffer fb(f.alloc, W, H);
    fb.clear(-1, -1, -1, -1);
    DrawCall c = make_call(f, f.fs, kIdentity, as_words(v), 21, 28, 4, winding ? idx2 : idx, 3);
    DrawStats st;
    CHECK(draw(f.alloc, c, fb, &st) == DrawStatus::Ok);
    check_image(winding ? "triangle (other winding)" : "triangle", fb, v, w1, 1e-5);
    CHECK(st.vertex_waves == 1 && st.triangles == 1 && st.pixel_waves == (st.pixels + 63) / 64);
    // coverage against the area of the triangle in pixels
    double sx[3], sy[3];
    for (int k = 0; k < 3; ++k) { sx[k] = (v[k].p[0] * 0.5 + 0.5) * W; sy[k] = (0.5 - v[k].p[1] * 0.5) * H; }
    const double area = fabs((sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0])) / 2;
    CHECK(fabs(static_cast<double>(st.pixels) - area) < 0.06 * area);
  }
}

static void test_quad(Fixture& f) {
  // a full-screen quad of two triangles: every pixel is shaded exactly once (the top-left rule on the shared diagonal)
  const uint32_t W = 37, H = 29;
  const Vertex7 v[4] = {{{-1, -1, 0.5f}, {1, 0, 0, 1}}, {{1, -1, 0.5f}, {0, 1, 0, 1}}, {{1, 1, 0.5f}, {0, 0, 1, 1}}, {{-1, 1, 0.5f}, {1, 1, 0, 1}}};
  const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
  Framebuffer fb(f.alloc, W, H);
  fb.clear(-1, -1, -1, -1);
  DrawCall c = make_call(f, f.fs, kIdentity, as_words(v), 28, 28, 4, idx, 6);
  DrawStats st;
  CHECK(draw(f.alloc, c, fb, &st) == DrawStatus::Ok);
  CHECK(st.pixels == W * H && st.triangles == 2);
  uint32_t missing = 0;
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x)
      if (fb.pixel(x, y)[0] == -1.0f) ++missing;
  CHECK(missing == 0);
  // the corner pixels are nearly the corner colours: the top left pixel is nearest the vertex at (-1, 1) (yellow)
  const float* tl = fb.pixel(0, 0);
  CHECK(tl[0] > 0.9f && tl[1] > 0.9f && tl[2] < 0.1f);
  const float* bl = fb.pixel(0, H - 1);
  CHECK(bl[0] > 0.9f && bl[1] < 0.1f);
}

static void test_perspective(Fixture& f) {
  const uint32_t W = 48, H = 36;
  // w = z: the clip positions are (x, y, z, z)
  Vertex7 v[3] = {{{-0.9f, -0.9f, 0.4f}, {1, 0, 0, 1}}, {{3.0f, -2.0f, 4.0f}, {0, 1, 0, 1}}, {{-0.5f, 1.5f, 1.5f}, {0, 0, 1, 1}}};
  const uint32_t idx[3] = {0, 1, 2};
  const float w[3] = {0.4f, 4.0f, 1.5f};
  Framebuffer fb(f.alloc, W, H);
  fb.clear(-1, -1, -1, -1);
  DrawCall c = make_call(f, f.fs, kDepthIsW, as_words(v), 21, 28, 4, idx, 3);
  CHECK(draw(f.alloc, c, fb) == DrawStatus::Ok);
  check_image("perspective", fb, v, w, 1e-4);
  // perspective-correct is not the same as affine: the centre of the screen is much closer to the near vertex' colour than to a third of each
  // (checked by check_image against the correct values; here that the image is not trivially flat)
  const float* p = fb.pixel(W / 2, H / 2);
  CHECK(p[0] + p[1] + p[2] > 0.99f && p[0] + p[1] + p[2] < 1.01f);
}

static void test_behind_viewer(Fixture& f) {
  // a vertex at w <= 0 drops the triangle
  const Vertex7 v[3] = {{{-0.5f, -0.5f, 1.0f}, {1, 0, 0, 1}}, {{0.5f, -0.5f, 1.0f}, {0, 1, 0, 1}}, {{0, 0.5f, -1.0f}, {0, 0, 1, 1}}};
  const uint32_t idx[3] = {0, 1, 2};
  Framebuffer fb(f.alloc, 16, 16);
  fb.clear(-1, -1, -1, -1);
  DrawCall c = make_call(f, f.fs, kDepthIsW, as_words(v), 21, 28, 4, idx, 3);
  DrawStats st;
  CHECK(draw(f.alloc, c, fb, &st) == DrawStatus::Ok && st.triangles == 0 && st.pixels == 0 && fb.pixel(8, 8)[0] == -1.0f);
}

struct UvTexture final : ITextureSource {
  uint32_t last_unit = 99;
  void sample(uint32_t unit, float u, float v, float out[4]) override { last_unit = unit; out[0] = u; out[1] = v; out[2] = 0.25f; out[3] = 1.0f; }
};

static void test_textured(Fixture& f) {
  // uv in the second attribute (2 floats: the fetch gives u, v, 0, 1), sampled by the pixel shader and written as the colour
  struct TV { float p[3]; float uv[2]; };
  const TV v[3] = {{{-0.9f, -0.8f, 0.5f}, {0, 0}}, {{0.9f, -0.8f, 1.0f}, {1, 0}}, {{0.0f, 0.9f, 2.0f}, {0.5f, 1}}};
  const uint32_t idx[3] = {0, 1, 2};
  const float w[3] = {0.5f, 1.0f, 2.0f};
  const uint32_t W = 40, H = 40;
  Framebuffer fb(f.alloc, W, H);
  fb.clear(-1, -1, -1, -1);
  DrawCall c = make_call(f, f.tfs, kDepthIsW, reinterpret_cast<const uint32_t*>(v), 15, 20, 2, idx, 3);
  UvTexture tex;
  c.textures = &tex;
  CHECK(draw(f.alloc, c, fb) == DrawStatus::Ok);
  CHECK(tex.last_unit == 0);
  // compare with the reference of the (u, v, 0, 1) attribute of the same triangle
  Vertex7 r[3];
  for (int k = 0; k < 3; ++k) { memcpy(r[k].p, v[k].p, 12); r[k].c[0] = v[k].uv[0]; r[k].c[1] = v[k].uv[1]; r[k].c[2] = 0.25f; r[k].c[3] = 1.0f; }
  // (the interpolated third and fourth components of the reference are those of the texture: 0.25 and 1)
  uint32_t inside = 0, bad = 0;
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      double want[4];
      bool loose;
      if (!reference(r, w, W, H, x, y, 0.002, want, &loose)) continue;
      ++inside;
      const float* got = fb.pixel(x, y);
      if (fabs(got[0] - want[0]) > 1e-4 || fabs(got[1] - want[1]) > 1e-4 || fabs(got[2] - 0.25) > 1e-6 || fabs(got[3] - 1.0) > 1e-6) ++bad;
    }
  CHECK(inside > 100 && bad == 0);
}

static void test_inputs(Fixture& f) {
  const Vertex7 v[3] = {{{-0.5f, -0.5f, 0.5f}, {1, 0, 0, 1}}, {{0.5f, -0.5f, 0.5f}, {0, 1, 0, 1}}, {{0, 0.5f, 0.5f}, {0, 0, 1, 1}}};
  const uint32_t idx[3] = {0, 1, 2};
  const uint32_t bad_idx[3] = {0, 1, 3};
  const uint32_t two[2] = {0, 1};
  Framebuffer fb(f.alloc, 16, 16);
  Framebuffer empty(f.alloc, 0, 4);
  CHECK(!empty.valid());
  DrawCall ok = make_call(f, f.fs, kIdentity, as_words(v), 21, 28, 4, idx, 3);
  CHECK(draw(f.alloc, ok, empty) == DrawStatus::BadInput);
  DrawCall c = ok;
  c.indices = ArrayView<const uint32_t>(bad_idx, 3);
  CHECK(draw(f.alloc, c, fb) == DrawStatus::BadInput);                // an index beyond the vertex buffer
  c = ok;
  c.indices = ArrayView<const uint32_t>(two, 2);
  CHECK(draw(f.alloc, c, fb) == DrawStatus::BadInput);                // not a whole triangle
  c = ok;
  c.vertex_stride = 0;
  CHECK(draw(f.alloc, c, fb) == DrawStatus::BadInput);
  c = ok;
  c.attributes[1] = {4, 20};                                          // 16 bytes at offset 20 of a 28 byte vertex
  CHECK(draw(f.alloc, c, fb) == DrawStatus::BadInput);
  c = ok;
  c.vertex_constants = ArrayView<const float>(kIdentity, 8);          // too few rows
  CHECK(draw(f.alloc, c, fb) == DrawStatus::BadInput);
  c = ok;
  c.pixel_info = f.vs.info;                                           // a vertex shader as the pixel shader
  CHECK(draw(f.alloc, c, fb) == DrawStatus::BadInput);
  CHECK(draw(f.alloc, ok, fb) == DrawStatus::Ok);
}

int main() {
  Fixture f;
  if (!f.vs.ok || !f.fs.ok || !f.tfs.ok) { printf("FAILED: the shaders did not compile\n"); return 1; }
  test_triangle(f);
  test_quad(f);
  test_perspective(f);
  test_behind_viewer(f);
  test_textured(f);
  test_inputs(f);
  printf(g_failed ? "GcnDrawTest: %d FAILED\n" : "GcnDrawTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
