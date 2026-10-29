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
#include <string>
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
static const char* kKillFragment = "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\n  0: KILL_IF IN[0]\n  1: MOV OUT[0], IN[0]\n  2: END\n";
static std::string sampling_fragment(const char* op, const char* target) {
  return std::string("FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL SAMP[0]\nDCL SVIEW[0], ") + target + ", FLOAT\n  0: " + op + " OUT[0], IN[0], SAMP[0], " + target + "\n  1: END\n";
}
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
  Compiled vs, fs, tfs, kfs;
  Fixture() : vs(alloc), fs(alloc), tfs(alloc), kfs(alloc) {
    compile(alloc, kKillFragment, &kfs);
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


#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTiling.hpp"

// Two triangles that cover the whole target, each of one colour at one depth (z in the NDC, w 1): the colour and the depth are the whole difference between them.
struct Layers {
  Vertex7 v[6];
  uint32_t idx[6] = {0, 1, 2, 3, 4, 5};
  Layers(float z0, const float c0[4], float z1, const float c1[4]) {
    const float pos[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
    for (int k = 0; k < 3; ++k) {
      v[k] = {{pos[k][0], pos[k][1], z0}, {c0[0], c0[1], c0[2], c0[3]}};
      v[3 + k] = {{pos[k][0], pos[k][1], z1}, {c1[0], c1[1], c1[2], c1[3]}};
    }
  }
};

static bool is_color(const Framebuffer& fb, uint32_t x, uint32_t y, float r, float g, float b, float a) {
  const float* p = fb.pixel(x, y);
  return fabsf(p[0] - r) < 1e-5f && fabsf(p[1] - g) < 1e-5f && fabsf(p[2] - b) < 1e-5f && fabsf(p[3] - a) < 1e-5f;
}

static void test_blending(Fixture& f) {
  const float src[4] = {1, 0, 0, 0.25f};
  Layers one(0.0f, src, 0.0f, src);
  Framebuffer fb(f.alloc, 16, 16);
  DrawCall c;
  auto run = [&](const BlendState& b, uint32_t tris, float r, float g, float bl, float a) {
    fb.clear(0, 0, 1, 1);
    c = make_call(f, f.fs, kIdentity, as_words(one.v), 42, 28, 4, one.idx, tris * 3);
    c.blend = b;
    return draw(f.alloc, c, fb) == DrawStatus::Ok && is_color(fb, 8, 8, r, g, bl, a) && is_color(fb, 0, 0, r, g, bl, a) && is_color(fb, 15, 15, r, g, bl, a);
  };
  BlendState off;
  CHECK(run(off, 1, 1, 0, 0, 0.25f));
  BlendState alpha;                      // src alpha, 1 - src alpha: the colour 0.25 of the source over the blue; the alpha takes the colour's factors
  alpha.enable = true; alpha.color_src = 4; alpha.color_dst = 5;
  CHECK(run(alpha, 1, 0.25f, 0, 0.75f, 0.25f * 0.25f + 0.75f));
  BlendState separate = alpha;
  separate.separate_alpha = true; separate.alpha_src = 1; separate.alpha_dst = 0;
  CHECK(run(separate, 1, 0.25f, 0, 0.75f, 0.25f));
  BlendState add;
  add.enable = true; add.color_src = 1; add.color_dst = 1;
  CHECK(run(add, 1, 1, 0, 1, 1.25f));
  BlendState sub = add;
  sub.color_op = 1;
  CHECK(run(sub, 1, 1, 0, -1, -0.75f));
  BlendState rsub = add;
  rsub.color_op = 4;
  CHECK(run(rsub, 1, -1, 0, 1, 0.75f));
  BlendState mn = add;
  mn.color_op = 2;
  CHECK(run(mn, 1, 0, 0, 0, 0.25f));
  BlendState mx = add;
  mx.color_op = 3;
  CHECK(run(mx, 1, 1, 0, 1, 1));
  BlendState constant;                   // src * constant colour
  constant.enable = true; constant.color_src = 13; constant.color_dst = 0;
  constant.constant[0] = 0.5f; constant.constant[1] = 0.25f; constant.constant[2] = 0.0f; constant.constant[3] = 0.1f;
  CHECK(run(constant, 1, 0.5f, 0, 0, 0.025f));
  BlendState inv = constant;             // dst * (1 - constant alpha) + src * constant alpha
  inv.color_src = 19; inv.color_dst = 20;
  CHECK(run(inv, 1, 0.1f, 0, 0.9f, 0.1f * 0.25f + 0.9f));
  BlendState dst_col = add;              // src * dst colour + dst * 0
  dst_col.color_src = 8; dst_col.color_dst = 0;
  CHECK(run(dst_col, 1, 0, 0, 0, 0.25f));
  BlendState sat = add;                  // saturate: min(src alpha, 1 - dst alpha) = min(0.25, 0) for the colour, 1 for alpha; dst * 0
  sat.color_src = 10; sat.color_dst = 0;
  CHECK(run(sat, 1, 0, 0, 0, 0.25f));
  // two layers over each other: the blend sees the first layer's result
  CHECK(run(alpha, 2, 0.4375f, 0, 0.5625f, 0.671875f));
  // the write mask: only red and alpha are written (no blending)
  BlendState mask;
  mask.write_mask = 0x9;
  CHECK(run(mask, 1, 1, 0, 1, 0.25f));
  mask.write_mask = 0;
  CHECK(run(mask, 1, 0, 0, 1, 1));
  mask.write_mask = 0x3;
  mask.enable = true; mask.color_src = 1; mask.color_dst = 1;
  CHECK(run(mask, 1, 1, 0, 1, 1));
  // refused: a factor that is not one
  BlendState bad = add;
  bad.color_src = 11;
  fb.clear(0, 0, 0, 0);
  c = make_call(f, f.fs, kIdentity, as_words(one.v), 42, 28, 4, one.idx, 3);
  c.blend = bad;
  CHECK(draw(f.alloc, c, fb) == DrawStatus::BadInput);
}

static void test_depth(Fixture& f) {
  const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
  const float z_far = 0.5f, z_near = -0.5f;                            // (the viewport maps z to z / 2 + 0.5: 0.75 and 0.25)
  Framebuffer fb(f.alloc, 16, 16);
  DepthBuffer db(f.alloc, 16, 16);
  CHECK(db.valid());
  DrawCall c;
  auto run = [&](float za, float zb, uint8_t func, float clear_depth, bool write) {
    Layers l(za, red, zb, green);
    fb.clear(0, 0, 0, 1);
    db.clear(clear_depth, 0);
    c = make_call(f, f.fs, kIdentity, as_words(l.v), 42, 28, 4, l.idx, 6);
    c.depth = &db;
    c.depth_stencil.depth_test = true;
    c.depth_stencil.depth_write = write;
    c.depth_stencil.depth_func = func;
    return draw(f.alloc, c, fb);
  };
  // less: the nearer layer wins in either order, and the depth buffer holds its depth
  CHECK(run(z_far, z_near, 1, 1.0f, true) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 1, 0, 1) && fabsf(db.depth(8, 8) - 0.25f) < 1e-5f);
  CHECK(run(z_near, z_far, 1, 1.0f, true) == DrawStatus::Ok && is_color(fb, 8, 8, 1, 0, 0, 1) && fabsf(db.depth(3, 12) - 0.25f) < 1e-5f);
  // greater against a cleared 0: the farther layer wins
  CHECK(run(z_far, z_near, 4, 0.0f, true) == DrawStatus::Ok && is_color(fb, 8, 8, 1, 0, 0, 1) && fabsf(db.depth(8, 8) - 0.75f) < 1e-5f);
  // no depth write: the buffer keeps its clear value, and the second layer is tested against it as well
  CHECK(run(z_far, z_near, 1, 1.0f, false) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 1, 0, 1) && db.depth(8, 8) == 1.0f);
  // never and always
  CHECK(run(z_far, z_near, 0, 1.0f, true) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 0, 0, 1) && db.depth(8, 8) == 1.0f);
  CHECK(run(z_near, z_far, 7, 1.0f, true) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 1, 0, 1) && fabsf(db.depth(8, 8) - 0.75f) < 1e-5f);
  // equal: the same depth twice passes the second time (its depth is the one the first wrote), a different one does not
  CHECK(run(z_near, z_near, 3, 1.0f, true) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 1, 0, 1));
  CHECK(run(z_far, z_near, 2, 0.25f, true) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 1, 0, 1));
  CHECK(run(z_far, z_far, 2, 0.25f, true) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 0, 0, 1));
  // not equal and greater or equal
  CHECK(run(z_far, z_near, 5, 0.75f, true) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 1, 0, 1));
  CHECK(run(z_far, z_near, 6, 0.5f, true) == DrawStatus::Ok && is_color(fb, 8, 8, 1, 0, 0, 1));
  // outside the depth range (z = 1.5 maps to 1.25) nothing is drawn, with the test on or off
  CHECK(run(1.5f, 1.5f, 7, 1.0f, true) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 0, 0, 1) && db.depth(8, 8) == 1.0f);
  // a pixel the shader discards leaves the depth alone
  {
    const float neg[4] = {-1, 0, 0, 1};
    Layers l(z_near, neg, z_near, neg);
    fb.clear(0, 0, 0, 1);
    db.clear(1.0f, 0);
    DrawCall k = make_call(f, f.kfs, kIdentity, as_words(l.v), 42, 28, 4, l.idx, 6);
    k.depth = &db;
    k.depth_stencil.depth_test = true;
    k.depth_stencil.depth_write = true;
    k.depth_stencil.depth_func = 1;
    CHECK(draw(f.alloc, k, fb) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 0, 0, 1) && db.depth(8, 8) == 1.0f);
  }
  // the test needs a buffer of the target's size
  {
    Layers l(z_near, red, z_near, red);
    DepthBuffer small(f.alloc, 8, 8);
    DrawCall k = make_call(f, f.fs, kIdentity, as_words(l.v), 42, 28, 4, l.idx, 3);
    k.depth_stencil.depth_test = true;
    CHECK(draw(f.alloc, k, fb) == DrawStatus::BadInput);                 // none
    k.depth = &small;
    CHECK(draw(f.alloc, k, fb) == DrawStatus::BadInput);                 // too small
    DepthBuffer ok(f.alloc, 16, 16);
    k.depth = &ok;
    k.depth_stencil.depth_func = 9;
    CHECK(draw(f.alloc, k, fb) == DrawStatus::BadInput);                 // not a function
  }
}

static void test_stencil(Fixture& f) {
  const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
  Framebuffer fb(f.alloc, 16, 16);
  DepthBuffer db(f.alloc, 16, 16);
  DrawCall c;
  auto run = [&](const DepthStencilState& s, uint8_t clear_stencil, float clear_depth, float za, float zb) {
    Layers l(za, red, zb, green);
    fb.clear(0, 0, 0, 1);
    db.clear(clear_depth, clear_stencil);
    c = make_call(f, f.fs, kIdentity, as_words(l.v), 42, 28, 4, l.idx, 6);
    c.depth = &db;
    c.depth_stencil = s;
    return draw(f.alloc, c, fb);
  };
  DepthStencilState s;
  s.stencil_test = true;
  s.stencil_func = 7;                    // always, and increment (clamp) on pass: two layers count up by two
  s.zpass_op = 5;
  s.stencil_op_value = 1;
  CHECK(run(s, 0, 1.0f, 0, 0) == DrawStatus::Ok && db.stencil(8, 8) == 2 && is_color(fb, 8, 8, 0, 1, 0, 1));
  // the clamp and the wrap
  CHECK(run(s, 254, 1.0f, 0, 0) == DrawStatus::Ok && db.stencil(8, 8) == 255);
  s.zpass_op = 8;
  CHECK(run(s, 254, 1.0f, 0, 0) == DrawStatus::Ok && db.stencil(8, 8) == 0);
  s.zpass_op = 6;
  CHECK(run(s, 1, 1.0f, 0, 0) == DrawStatus::Ok && db.stencil(8, 8) == 0);
  s.zpass_op = 9;
  CHECK(run(s, 1, 1.0f, 0, 0) == DrawStatus::Ok && db.stencil(8, 8) == 255);
  // the other operations on pass: zero, ones, invert, replace with the reference, replace with the operation value
  s.stencil_ref = 0x42; s.stencil_op_value = 0x17;
  const uint8_t ops[5] = {1, 2, 7, 3, 4};
  const uint8_t want[5] = {0, 0xFF, 0xF0, 0x42, 0x17};            // (two layers: the invert is undone)
  for (int k = 0; k < 5; ++k) {
    s.zpass_op = ops[k];
    CHECK(run(s, 0xF0, 1.0f, 0, 0) == DrawStatus::Ok && db.stencil(8, 8) == want[k]);
  }
  // the test: equal to the reference under the mask; a failing pixel gets the fail operation and is not drawn
  DepthStencilState t;
  t.stencil_test = true;
  t.stencil_func = 2; t.stencil_ref = 0x05; t.stencil_mask = 0x0F;
  t.fail_op = 4; t.stencil_op_value = 0x77;
  t.zpass_op = 0;
  CHECK(run(t, 0xA5, 1.0f, 0, 0) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 1, 0, 1) && db.stencil(8, 8) == 0xA5);           // 0xA5 & 0x0F == 5: passes
  CHECK(run(t, 0xA6, 1.0f, 0, 0) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 0, 0, 1) && db.stencil(8, 8) == 0x77);           // fails, replaced
  t.stencil_write_mask = 0x0F;
  CHECK(run(t, 0xA6, 1.0f, 0, 0) == DrawStatus::Ok && db.stencil(8, 8) == 0xA7);                                              // only the low bits written
  // depth fail: the stencil takes the z fail operation, the colour is not drawn; with depth test on and the stencil passing it counts both
  DepthStencilState z;
  z.stencil_test = true; z.depth_test = true; z.depth_func = 1;
  z.zfail_op = 5; z.zpass_op = 1; z.stencil_op_value = 1;
  CHECK(run(z, 10, 0.25f, 0.5f, 0.5f) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 0, 0, 1) && db.stencil(8, 8) == 12);           // z 0.75 is not less than 0.25: two z fails
  CHECK(run(z, 10, 1.0f, 0.5f, 0.5f) == DrawStatus::Ok && is_color(fb, 8, 8, 0, 1, 0, 1) && db.stencil(8, 8) == 0);               // depth write is off: both layers pass and zero it
  // a discarded pixel gets no pass operation
  {
    const float neg[4] = {-1, 0, 0, 1};
    Layers l(0, neg, 0, neg);
    fb.clear(0, 0, 0, 1);
    db.clear(1.0f, 7);
    DrawCall k = make_call(f, f.kfs, kIdentity, as_words(l.v), 42, 28, 4, l.idx, 6);
    k.depth = &db;
    k.depth_stencil.stencil_test = true;
    k.depth_stencil.zpass_op = 1;
    CHECK(draw(f.alloc, k, fb) == DrawStatus::Ok && db.stencil(8, 8) == 7);
  }
  // refused: an operation that does not exist
  DepthStencilState bad;
  bad.stencil_test = true;
  bad.fail_op = 10;
  CHECK(run(bad, 0, 1.0f, 0, 0) == DrawStatus::BadInput);
}

// a block of memory the texture unit reads
struct TexMem final : ITexelMemory {
  uint64_t base = 0x80000;
  uint32_t words[1024] = {};
  bool read32(uint64_t address, uint32_t* out) override {
    if (address < base || (address & 3) != 0 || (address - base) / 4 >= 1024) return false;
    *out = words[(address - base) / 4];
    return true;
  }
};

static void test_descriptor_textures(Fixture& f) {
  // a 2 by 2 texture (red, green / blue, white), sampled by the pixel shader through its T# and S# with point filtering; the triangle covers the target with uv = position
  TexMem mem;
  const uint32_t texels[4] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu};         // (r g b a bytes, little endian words)
  for (int k = 0; k < 4; ++k) mem.words[k] = texels[k];
  struct TV { float p[3]; float uv[2]; };
  const TV v[3] = {{{-1.0f, -1.0f, 0.0f}, {0, 1}}, {{3.0f, -1.0f, 0.0f}, {2, 1}}, {{-1.0f, 3.0f, 0.0f}, {0, -1}}};
  const uint32_t idx[3] = {0, 1, 2};
  Framebuffer fb(f.alloc, 16, 16);
  fb.clear(0, 0, 0, 0);
  DrawCall c = make_call(f, f.tfs, kIdentity, reinterpret_cast<const uint32_t*>(v), 15, 20, 2, idx, 3);
  ImageDescriptor img;
  img.address = mem.base; img.width = 2; img.height = 2; img.pitch_pixels = 2; img.format = TexelFormat::Rgba8Unorm; img.tile_index = 8;
  SamplerDescriptor smp;
  CHECK(make_image_descriptor(ArrayView<uint32_t>(c.image_words[0], 8), img));
  CHECK(make_sampler_descriptor(ArrayView<uint32_t>(c.sampler_words[0], 4), smp));
  c.descriptors_set = 1;
  c.texel_memory = &mem;
  CHECK(draw(f.alloc, c, fb) == DrawStatus::Ok);
  // uv.x = x / 16 and uv.y = 1 - y / 16 * 2 ... u runs 0 to 1 over the target (position -1..1 maps u 0..1), v from 1 at the bottom to 0 at the top (clamped by wrap)
  CHECK(is_color(fb, 2, 14, 1, 0, 0, 1) || is_color(fb, 2, 14, 0, 0, 1, 1) || is_color(fb, 2, 14, 0, 1, 0, 1) || is_color(fb, 2, 14, 1, 1, 1, 1));
  // the left half is the first column of texels, the right half the second: red or blue on the left, green or white on the right
  uint32_t left_ok = 0, right_ok = 0;
  for (uint32_t y = 0; y < 16; ++y) {
    for (uint32_t x = 0; x < 6; ++x) left_ok += is_color(fb, x, y, 1, 0, 0, 1) || is_color(fb, x, y, 0, 0, 1, 1);
    for (uint32_t x = 10; x < 16; ++x) right_ok += is_color(fb, x, y, 0, 1, 0, 1) || is_color(fb, x, y, 1, 1, 1, 1);
  }
  CHECK(left_ok == 96 && right_ok == 96);
  // a T# the memory cannot satisfy is a shader fault; so is a unit with no descriptor and no memory
  DrawCall far = c;
  far.image_words[0][0] = 0x1000;                                       // some other address
  CHECK(draw(f.alloc, far, fb) == DrawStatus::ShaderFault);
  DrawCall none = c;
  none.texel_memory = nullptr;
  CHECK(draw(f.alloc, none, fb) == DrawStatus::ShaderFault);
  // a sampler the unit does not implement
  DrawCall badS = c;
  badS.sampler_words[0][0] |= 1u << 29;                                  // a filter mode
  CHECK(draw(f.alloc, badS, fb) == DrawStatus::ShaderFault);
}

// Two sided stencil (which faces are front depends on the winding and on front_clockwise) and the depth bounds test
static void test_faces_and_bounds(Fixture& f) {
  const float red[4] = {1, 0, 0, 1};
  Framebuffer fb(f.alloc, 16, 16);
  DepthBuffer db(f.alloc, 16, 16);
  // the triangle (-1,-1) (3,-1) (-1,3) is counter clockwise on the screen; with the vertices swapped it is clockwise
  Layers l(0.0f, red, 0.0f, red);
  const uint32_t ccw[3] = {0, 1, 2}, cw[3] = {0, 2, 1};
  auto run = [&](const uint32_t* idx, bool front_clockwise) {
    fb.clear(0, 0, 0, 1);
    db.clear(1.0f, 10);
    DrawCall c = make_call(f, f.fs, kIdentity, as_words(l.v), 42, 28, 4, idx, 3);
    c.depth = &db;
    c.depth_stencil.stencil_test = true;
    c.depth_stencil.zpass_op = 5;                       // front: add one
    c.depth_stencil.stencil_op_value = 1;
    c.depth_stencil.two_sided = true;
    c.depth_stencil.front_clockwise = front_clockwise;
    c.depth_stencil.back.zpass_op = 1;                  // back: zero
    c.depth_stencil.back.func = 7;
    CHECK(draw(f.alloc, c, fb) == DrawStatus::Ok);
    return db.stencil(8, 8);
  };
  CHECK(run(ccw, false) == 11 && run(ccw, true) == 0 && run(cw, false) == 0 && run(cw, true) == 11);
  // the back face has its own compare, reference and mask
  {
    fb.clear(0, 0, 0, 1);
    db.clear(1.0f, 0x15);
    DrawCall c = make_call(f, f.fs, kIdentity, as_words(l.v), 42, 28, 4, cw, 3);
    c.depth = &db;
    c.depth_stencil.stencil_test = true;
    c.depth_stencil.stencil_func = 0;                   // the front never passes
    c.depth_stencil.two_sided = true;
    c.depth_stencil.back.func = 2;                      // the back: equal to 5 under the mask 0x0F
    c.depth_stencil.back.ref = 0x05;
    c.depth_stencil.back.mask = 0x0F;
    c.depth_stencil.back.zpass_op = 2;
    CHECK(draw(f.alloc, c, fb) == DrawStatus::Ok && is_color(fb, 8, 8, 1, 0, 0, 1) && db.stencil(8, 8) == 0xFF);
  }
  // depth bounds: the stored depth 0.5 is inside [0.4, 0.6] and outside [0, 0.4]; a pixel outside keeps its colour and its stencil
  for (int inside = 0; inside < 2; ++inside) {
    fb.clear(0, 0, 0, 1);
    db.clear(0.5f, 3);
    DrawCall c = make_call(f, f.fs, kIdentity, as_words(l.v), 42, 28, 4, ccw, 3);
    c.depth = &db;
    c.depth_stencil.depth_bounds = true;
    c.depth_stencil.bounds_min = inside ? 0.4f : 0.0f;
    c.depth_stencil.bounds_max = inside ? 0.6f : 0.4f;
    c.depth_stencil.stencil_test = true;
    c.depth_stencil.zpass_op = 5;
    CHECK(draw(f.alloc, c, fb) == DrawStatus::Ok);
    CHECK(inside ? (is_color(fb, 8, 8, 1, 0, 0, 1) && db.stencil(8, 8) == 4) : (is_color(fb, 8, 8, 0, 0, 0, 1) && db.stencil(8, 8) == 3));
  }
  // bounds need the plane
  DrawCall c = make_call(f, f.fs, kIdentity, as_words(l.v), 42, 28, 4, ccw, 3);
  c.depth_stencil.depth_bounds = true;
  CHECK(draw(f.alloc, c, fb) == DrawStatus::BadInput);
  DepthBuffer ok(f.alloc, 16, 16);
  c.depth = &ok;
  c.depth_stencil.bounds_min = NAN;
  CHECK(draw(f.alloc, c, fb) == DrawStatus::BadInput);
}

// Textures of the other kinds through compiled shaders: the level of detail of TXL and TXB, 3D, array and cube images (the cube through v_cubeid / v_cubesc / v_cubetc / v_cubema)
struct BigTexMem final : ITexelMemory {
  uint64_t base = 0x80000;
  uint32_t words[4096] = {};
  bool read32(uint64_t address, uint32_t* out) override {
    if (address < base || (address & 3) != 0 || (address - base) / 4 >= 4096) return false;
    *out = words[(address - base) / 4];
    return true;
  }
};

static void fill_slice_level(BigTexMem& m, const ImageDescriptor& d, uint32_t level, uint32_t slice, uint8_t red) {
  LevelLayout l;
  CHECK(image_level(d, level, &l));
  for (uint32_t y = 0; y < l.height; ++y)
    for (uint32_t x = 0; x < l.width; ++x) {
      const uint8_t t[4] = {red, 0, 0, 255};
      memcpy(reinterpret_cast<uint8_t*>(m.words) + image_texel_offset(d, l, x, y, slice), t, 4);
    }
}

// draws one triangle over the target whose colour attribute is `attr` at all three vertices, the image and sampler in unit 0; returns the red of the middle pixel * 255 (or -1)
static float draw_sampled(Fixture& f, const Compiled& ps, BigTexMem& mem, const ImageDescriptor& img, const SamplerDescriptor& smp, const float attr[4]) {
  const Vertex7 v[3] = {{{-1.0f, -1.0f, 0.0f}, {attr[0], attr[1], attr[2], attr[3]}}, {{3.0f, -1.0f, 0.0f}, {attr[0], attr[1], attr[2], attr[3]}},
                        {{-1.0f, 3.0f, 0.0f}, {attr[0], attr[1], attr[2], attr[3]}}};
  const uint32_t idx[3] = {0, 1, 2};
  Framebuffer fb(f.alloc, 8, 8);
  fb.clear(-1, -1, -1, -1);
  DrawCall c = make_call(f, ps, kIdentity, as_words(v), 21, 28, 4, idx, 3);
  if (!make_image_descriptor(ArrayView<uint32_t>(c.image_words[0], 8), img) || !make_sampler_descriptor(ArrayView<uint32_t>(c.sampler_words[0], 4), smp)) return -1.0f;
  c.descriptors_set = 1;
  c.texel_memory = &mem;
  if (draw(f.alloc, c, fb) != DrawStatus::Ok) return -2.0f;
  return fb.pixel(4, 4)[0] * 255.0f;
}

static ImageDescriptor tex_image(const BigTexMem& m, uint32_t w, uint32_t h, uint32_t levels, ImageType type, uint32_t depth) {
  ImageDescriptor d;
  d.address = m.base;
  d.width = w;
  d.height = h;
  d.pitch_pixels = image_pitch(w, h, levels, 4);
  d.tile_index = 9;
  d.levels = levels;
  d.type = type;
  d.depth = depth;
  return d;
}

static void test_more_textures() {
  Fixture f;
  Compiled txl(f.alloc), txb(f.alloc), cube(f.alloc), vol(f.alloc), arr(f.alloc);
  compile(f.alloc, sampling_fragment("TXL", "2D").c_str(), &txl);
  compile(f.alloc, sampling_fragment("TXB", "2D").c_str(), &txb);
  compile(f.alloc, sampling_fragment("TEX", "CUBE").c_str(), &cube);
  compile(f.alloc, sampling_fragment("TEX", "3D").c_str(), &vol);
  compile(f.alloc, sampling_fragment("TEX", "2D_ARRAY").c_str(), &arr);
  CHECK(txl.ok && txb.ok && cube.ok && vol.ok && arr.ok);
  if (!(txl.ok && txb.ok && cube.ok && vol.ok && arr.ok)) return;
  auto near_to = [](float a, float want) { return a > want - 0.6f && a < want + 0.6f; };
  // mips: 8 by 8 with 4 levels, level k red = 60 k; TXL takes the lod from w, TXB adds it
  {
    BigTexMem m;
    ImageDescriptor d = tex_image(m, 8, 8, 4, ImageType::Image2D, 1);
    for (uint32_t k = 0; k < 4; ++k) fill_slice_level(m, d, k, 0, static_cast<uint8_t>(60 * k));
    SamplerDescriptor s;
    s.mip_filter = 2;
    const float lod0[4] = {0.5f, 0.5f, 0, 0}, lod15[4] = {0.5f, 0.5f, 0, 1.5f}, lod3[4] = {0.5f, 0.5f, 0, 3.0f};
    CHECK(near_to(draw_sampled(f, txl, m, d, s, lod0), 0) && near_to(draw_sampled(f, txl, m, d, s, lod15), 90) && near_to(draw_sampled(f, txl, m, d, s, lod3), 180));
    CHECK(near_to(draw_sampled(f, txb, m, d, s, lod15), 90));
    s.mip_filter = 1;
    CHECK(near_to(draw_sampled(f, txl, m, d, s, lod15), 120));
    // a plain TEX has no derivatives here: lod 0 plus the sampler's bias
    s.mip_filter = 2;
    s.lod_bias = 2.0f;
    CHECK(near_to(draw_sampled(f, f.tfs, m, d, s, lod0), 120));
  }
  // an array: the layer is the z coordinate
  {
    BigTexMem m;
    ImageDescriptor d = tex_image(m, 4, 4, 1, ImageType::Array2D, 3);
    for (uint32_t z = 0; z < 3; ++z) fill_slice_level(m, d, 0, z, static_cast<uint8_t>(80 * z));
    SamplerDescriptor s;
    const float l0[4] = {0.5f, 0.5f, 0.0f, 1}, l2[4] = {0.5f, 0.5f, 2.0f, 1}, l1[4] = {0.5f, 0.5f, 1.2f, 1};
    CHECK(near_to(draw_sampled(f, arr, m, d, s, l0), 0) && near_to(draw_sampled(f, arr, m, d, s, l2), 160) && near_to(draw_sampled(f, arr, m, d, s, l1), 80));
  }
  // a volume
  {
    BigTexMem m;
    ImageDescriptor d = tex_image(m, 4, 4, 1, ImageType::Image3D, 4);
    for (uint32_t z = 0; z < 4; ++z) fill_slice_level(m, d, 0, z, static_cast<uint8_t>(60 * z));
    SamplerDescriptor s;
    const float a[4] = {0.5f, 0.5f, 0.3f, 1}, b[4] = {0.5f, 0.5f, 0.9f, 1};
    CHECK(near_to(draw_sampled(f, vol, m, d, s, a), 60) && near_to(draw_sampled(f, vol, m, d, s, b), 180));
    s.z_filter = 2;
    const float mid[4] = {0.5f, 0.5f, 0.5f, 1};
    CHECK(near_to(draw_sampled(f, vol, m, d, s, mid), 90));
  }
  // a cube: red = 40 * face; the direction is the attribute; the six axes and a few off-axis directions
  {
    BigTexMem m;
    ImageDescriptor d = tex_image(m, 4, 4, 1, ImageType::Cube, 6);
    for (uint32_t z = 0; z < 6; ++z) fill_slice_level(m, d, 0, z, static_cast<uint8_t>(40 * z));
    SamplerDescriptor s;
    struct Dir { float v[4]; int face; };
    const Dir dirs[] = {{{1, 0, 0, 1}, 0}, {{-1, 0, 0, 1}, 1}, {{0, 1, 0, 1}, 2}, {{0, -1, 0, 1}, 3}, {{0, 0, 1, 1}, 4}, {{0, 0, -1, 1}, 5},
                        {{1, 0.5f, -0.25f, 1}, 0}, {{-0.3f, -2, 0.9f, 1}, 3}, {{0.2f, 0.3f, -0.8f, 1}, 5}, {{-0.9f, 0.1f, 0.2f, 1}, 1}};
    for (const Dir& dir : dirs) CHECK(near_to(draw_sampled(f, cube, m, d, s, dir.v), 40.0f * dir.face));
    // the orientation on a face (the table of the OpenGL specification: on +x sc = -z, tc = -y): face 0 holds red = 20 x + 60 y
    {
      LevelLayout l;
      image_level(d, 0, &l);
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
          const uint8_t t[4] = {static_cast<uint8_t>(20 * x + 60 * y), 0, 0, 255};
          memcpy(reinterpret_cast<uint8_t*>(m.words) + image_texel_offset(d, l, x, y, 0), t, 4);
        }
      const float a[4] = {1, 0.5f, 0.5f, 1}, b[4] = {1, -0.5f, -0.5f, 1}, c[4] = {1, -0.5f, 0.5f, 1};
      CHECK(near_to(draw_sampled(f, cube, m, d, s, a), 80));            // sc = -0.5, tc = -0.5: s = t = 0.25: texel (1, 1)
      CHECK(near_to(draw_sampled(f, cube, m, d, s, b), 240));           // s = t = 0.75: texel (3, 3)
      CHECK(near_to(draw_sampled(f, cube, m, d, s, c), 20 * 1 + 60 * 3));   // sc = -0.5 (s = 0.25), tc = +0.5 (t = 0.75): texel (1, 3)
    }
  }
}

int main() {
  Fixture f;
  if (!f.vs.ok || !f.fs.ok || !f.tfs.ok || !f.kfs.ok) { printf("FAILED: the shaders did not compile\n"); return 1; }
  test_triangle(f);
  test_quad(f);
  test_perspective(f);
  test_behind_viewer(f);
  test_textured(f);
  test_inputs(f);
  test_blending(f);
  test_depth(f);
  test_stencil(f);
  test_descriptor_textures(f);
  test_faces_and_bounds(f);
  test_more_textures();
  printf(g_failed ? "GcnDrawTest: %d FAILED\n" : "GcnDrawTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
