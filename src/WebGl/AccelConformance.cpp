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

// Scenes that check the 3D hardware path of the GL contexts (GlAccel.cpp, TgsiTranslator.cpp) against their CPU rasterizer: each GLSL ES 3.00 program draws a
// small picture once with the accelerator and once without, and the two must agree to within a few levels per channel. The kernel runs them at boot on the
// machine's accelerator (main.cpp: the log says what each did); the host tests run them on a TGSI interpreter (tests/GlAccelTest.cpp). Between them they show
// whether the translation means on a real device what the shaders mean on the CPU.
#include "WebGl/AccelConformance.hpp"
#include "WebGl/GlContext.hpp"

namespace webgl {
namespace {

// Every scene: a vertex shader of pos (vec2), col (vec4) and k (float), and a fragment shader; drawn as a strip over the whole 32 x 32 picture (mode 5) or as two
// triangles of opposite winding (mode 4).
struct Scene {
  const char* name;
  const char* vs;
  const char* fs;
  uint32_t mode;
  void (*setup)(GlContext& gl, uint32_t program);
  // Desktop GL scenes (GLSL 4.00): the geometry and tessellation shaders (nullptr: none), the patch size, and how many vertices are drawn (4: the strip's, 6: the
  // triangles'; 0: as the mode says).
  bool desktop = false;
  const char* gs = nullptr;
  const char* tcs = nullptr;
  const char* tes = nullptr;
  uint32_t patch_vertices = 0;
  uint32_t count = 0;
};

#define kHead "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#define kFsHead kHead "in vec4 v; in float vk; out vec4 o;\n"
constexpr const char* kVs = "#version 300 es\nin vec2 pos; in vec4 col; in float k; out vec4 v; out float vk;\nvoid main(){ v = col; vk = k; gl_Position = vec4(pos, 0.0, 1.0); }\n";

void set_floats(GlContext& gl, uint32_t p, const char* name, uint32_t cols, uint32_t rows, uint32_t count, const float* f) {
  uint32_t lanes[64];
  const uint32_t n = cols * rows * count;
  for (uint32_t i = 0; i < n && i < 64; ++i) lanes[i] = __builtin_bit_cast(uint32_t, f[i]);
  gl.uniform(gl.get_uniform_location(p, name), 'f', cols, rows, count, lanes);
}

void depth_texture(GlContext& gl, uint32_t p, uint32_t target, uint32_t w, uint32_t h, uint32_t filter, uint32_t func);

// a 2 x 2 RGBA texture on unit 0
void upload_2x2(GlContext& gl, uint32_t tex, const uint8_t (&texels)[16]) {
  gl.active_texture(0x84C0);
  gl.bind_texture(0x0DE1, tex);
  gl.tex_image(0x0DE1, 0, 0x1908, 2, 2, 1, 0, 0x1908, 0x1401, ConstByteView(texels, sizeof texels), true, false);
}

constexpr Scene kScenes[] = {
      {"for loop with break and continue", kVs, kFsHead "void main(){ float s = 0.0; for (int i = 0; i < 8; ++i) { if (i == 2) continue; if (i == 6) break; s += v.r * float(i) * 0.05; } o = vec4(s, v.g, v.b, 1.0); }\n", 5, nullptr},
      {"while and do loops", kVs, kFsHead "void main(){ float s = 0.0; int i = 0; while (true) { s += 0.1 * v.g; i++; if (i >= 4) break; } int j = 0; do { s += 0.05; j += 1; } while (j < 3); o = vec4(s, float(i) * 0.1, float(j) * 0.1, 1.0); }\n", 5, nullptr},
      {"functions: early return, out and inout", kVs, kFsHead "float pick(float x, out float y, inout float z) { y = x * 0.5; z += 0.25; if (x > 0.5) return 1.0; return x; }\nvec3 nest(vec3 c) { for (int i = 0; i < 4; ++i) { if (c.r > 0.9) return vec3(1.0, 0.0, 1.0); c += 0.15; } return c * 0.5; }\nvoid main(){ float y, z = 0.1; float r = pick(v.r, y, z); o = vec4(r, y, z, 1.0) * 0.5 + vec4(nest(v.rgb), 0.0) * 0.5; }\n", 5, nullptr},
      {"integers", kVs, kFsHead "uniform int u_n;\nvoid main(){ int a = int(v.r * 100.0); int b = a / 7; int c = a % 7; ivec2 d = ivec2(a, b) * 2 - ivec2(1); uint e = uint(a) >> 2; int f = (a << 1) + u_n; int g = (a - 60) / 7 + (a - 60) % 7 + 20; o = vec4(float(b) / 16.0, float(c) / 7.0, float(d.x + d.y) / 400.0, float(e + uint(f)) / 400.0) * 0.5 + float(g) / 80.0; }\n", 5,
       [](GlContext& gl, uint32_t p) { const uint32_t n = 3; gl.uniform(gl.get_uniform_location(p, "u_n"), 'i', 1, 1, 1, &n); }},
      {"uniform arrays by a variable", kHead "in vec2 pos; in vec4 col; in float k; out vec4 v; uniform vec4 u_pal[4]; uniform mat2 u_m[2];\nvoid main(){ int i = int(k); v = u_pal[i] + vec4(u_m[i / 2] * col.xy, 0.0, 0.0) * 0.1 + u_pal[i + 7] * 0.0; gl_Position = vec4(pos, 0.0, 1.0); }\n",
       kFsHead "void main(){ o = v; }\n", 5, [](GlContext& gl, uint32_t p) {
         const float pal[16] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 0, 1};
         const float m[8] = {1, 0, 0, 1, 0, 1, 1, 0};
         set_floats(gl, p, "u_pal", 1, 4, 4, pal);
         set_floats(gl, p, "u_m", 2, 2, 2, m);
       }},
      {"local arrays by a variable", kVs, kFsHead "void main(){ float a[5]; for (int i = 0; i < 5; ++i) a[i] = float(i) * 0.2 * v.r; int j = int(v.g * 4.0); a[j] = 1.0; vec3 w = vec3(0.0); w[j % 3] = 0.5; mat2 m = mat2(0.1); m[j % 2] = vec2(0.3, 0.7); o = vec4(a[j], a[(j + 2) % 5], w.x + w.z, m[1].y); }\n", 5, nullptr},
      {"flat varyings", kHead "in vec2 pos; in vec4 col; in float k; flat out vec4 v; flat out int n;\nvoid main(){ v = col; n = int(k) + 1; gl_Position = vec4(pos, 0.0, 1.0); }\n",
       kHead "precision highp float;\nflat in vec4 v; flat in int n; out vec4 o;\nvoid main(){ o = vec4(v.rgb, 1.0) * float(n) * 0.25; }\n", 4, nullptr},
      {"gl_FragCoord and gl_FrontFacing", kVs, kFsHead "void main(){ o = vec4(gl_FragCoord.x / 32.0, gl_FragCoord.y / 32.0, gl_FrontFacing ? 1.0 : 0.25, gl_FragCoord.z); }\n", 4, nullptr},
      {"built-ins", kVs, kFsHead "void main(){ vec2 p = v.rg * 2.0 - 1.0; float a = atan(p.y, p.x) / 6.3 + 0.5; float b = asin(p.x * 0.9) + acos(p.y * 0.9) * 0.25; float c = tan(v.b * 0.7); bvec2 lt = lessThan(p, vec2(0.0)); vec3 f = faceforward(vec3(0.0, 0.0, 1.0), vec3(p, -0.5), vec3(0.0, 0.0, 1.0)); vec3 r = refract(normalize(vec3(p, -1.0)), vec3(0.0, 0.0, 1.0), 0.8); o = vec4(a, b * 0.3, c, any(lt) ? (all(lt) ? 1.0 : 0.5) : 0.0) * 0.5 + vec4(f.z, r.x, mix(0.2, 0.9, lt.x), 0.0) * 0.25; }\n", 5, nullptr},
      {"conditionals do not mix the side not taken", kVs, kFsHead "void main(){ float d = v.r - 0.5; float q = d > 0.0 ? 1.0 / (d * 1e-30 * 1e-30) : 0.25; o = vec4(min(q, 1.0), d == 0.0 ? 1.0 : 0.0, v.b, 1.0); }\n", 5, nullptr},
      {"discard in a loop", kVs, kFsHead "void main(){ for (int i = 0; i < 3; ++i) { if (v.r * 3.0 > float(i) + 2.4) discard; } o = v; }\n", 5, nullptr},
      {"vector equality and booleans", kVs, kFsHead "uniform bool u_b;\nvoid main(){ ivec2 c = ivec2(v.rg * 3.0); bool e = c == ivec2(1, 1); bool n = c != ivec2(0, 2); o = vec4(e ? 1.0 : 0.0, n ? 1.0 : 0.0, u_b ? 1.0 : 0.0, 1.0); }\n", 5,
       [](GlContext& gl, uint32_t p) { const uint32_t one = 1; gl.uniform(gl.get_uniform_location(p, "u_b"), 'i', 1, 1, 1, &one); }},
      {"textures in a function and a loop", kVs, kFsHead "uniform sampler2D u_t;\nvec4 look(vec2 c) { return texture(u_t, c); }\nvoid main(){ vec4 s = vec4(0.0); for (int i = 0; i < 3; ++i) s += look(v.rg + vec2(float(i) * 0.25, 0.0)) / 3.0; o = s; }\n", 5,
       [](GlContext& gl, uint32_t p) {
         const uint32_t tex = gl.create_texture();
         const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
         upload_2x2(gl, tex, texels);
         gl.tex_parameter(0x0DE1, 0x2801, 0x2600);   // MIN_FILTER NEAREST
         gl.tex_parameter(0x0DE1, 0x2800, 0x2600);   // MAG_FILTER NEAREST
         const uint32_t unit = 0;
         gl.uniform(gl.get_uniform_location(p, "u_t"), 'i', 1, 1, 1, &unit);
       }},
      {"a uniform structure", kVs, kFsHead "struct L { vec3 c; float k; };\nuniform L u_l;\nvoid main(){ o = vec4(u_l.c * v.r, u_l.k); }\n", 5,
       [](GlContext& gl, uint32_t p) {
         const float c[3] = {0.25f, 0.5f, 0.75f};
         const float k = 0.6f;
         set_floats(gl, p, "u_l.c", 1, 3, 1, c);
         set_floats(gl, p, "u_l.k", 1, 1, 1, &k);
       }},
      {"nested loops and a return from inside them", kVs, kFsHead "float f(float x) { for (int i = 0; i < 4; ++i) { for (int j = 0; j < 4; ++j) { if (float(i * 4 + j) * 0.07 > x) return float(i * 4 + j) / 16.0; } } return 1.0; }\nvoid main(){ o = vec4(f(v.r), f(v.g), f(v.b * 0.5), 1.0); }\n", 5, nullptr},
      {"continue in a do loop runs its condition", kVs, kFsHead "void main(){ int i = 0; float s = 0.0; do { i++; if (i == 2) continue; s += 0.1 * v.r; } while (i < 6); o = vec4(s, float(i) / 8.0, 0.0, 1.0); }\n", 5, nullptr},
      {"switch with fallthrough, default and break", kVs, kFsHead "void main(){ int c = int(v.r * 5.0); float s = 0.0; switch (c) { case 0: s += 0.1; case 1: s += 0.2; break; case 3: s = 0.7; break; default: s = 0.9; } switch (c + 10) { case 11: s += 0.05; break; } o = vec4(s, float(c) / 5.0, 0.0, 1.0); }\n", 5, nullptr},
      {"matrix and array varyings", kHead "in vec2 pos; in vec4 col; in float k; out mat3 m; out vec2 w[3];\nvoid main(){ m = mat3(col.rgb, col.gbr, vec3(k * 0.2)); for (int i = 0; i < 3; ++i) w[i] = col.rg * float(i) * 0.4; gl_Position = vec4(pos, 0.0, 1.0); }\n",
       kHead "precision highp float;\nin mat3 m; in vec2 w[3]; out vec4 o;\nvoid main(){ int j = int(m[2].x * 10.0) % 3; o = vec4(m * vec3(0.3, 0.3, 0.3), 1.0) * 0.5 + vec4(w[j], w[1].x, 0.0) * 0.5; }\n", 5, nullptr},
      {"initialised globals, shifts by a variable, big constants", kVs, kFsHead "float g = 0.25; int sh = 2;\nvoid main(){ int a = int(v.r * 64.0); int b = (a << sh) >> 3; float big = 3.0e20 * v.g / 3.0e20; float tiny = v.b * 1.0e-12 * 1.0e12; o = vec4(g, float(b) / 64.0, big, tiny); }\n", 5, nullptr},
      {"texture lookups with a level and projective ones", kVs, kFsHead "uniform sampler2D u_t;\nvoid main(){ o = textureLod(u_t, v.rg, 2.0) * 0.5 + textureProj(u_t, vec3(v.rg * 2.0, 2.0)) * 0.25 + texture(u_t, v.gr, 1.0) * 0.25; }\n", 5,
       [](GlContext& gl, uint32_t p) {
         const uint32_t tex = gl.create_texture();
         const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
         upload_2x2(gl, tex, texels);
         gl.tex_parameter(0x0DE1, 0x2801, 0x2601);
         gl.tex_parameter(0x0DE1, 0x2800, 0x2601);
         const uint32_t unit = 0;
         gl.uniform(gl.get_uniform_location(p, "u_t"), 'i', 1, 1, 1, &unit);
       }},
      {"matrix constructors, inverse and determinant", kVs, kFsHead "void main(){ mat4 m4 = mat4(v.r + 1.0, 0.1, 0.2, 0.0, 0.3, v.g + 1.0, 0.1, 0.0, 0.0, 0.2, v.b + 1.0, 0.0, 0.5, 0.5, 0.5, 1.0); mat3 m3 = mat3(m4); mat2 m2 = mat2(m3); mat3 back = mat3(m2); mat3 i3 = inverse(m3); mat2 i2 = inverse(m2); vec4 fl = vec4(m2); o = vec4(determinant(m3) * 0.2, (i3 * vec3(1.0)).x * 0.5, (i2 * vec2(1.0)).y * 0.5 + back[2].z * 0.1, fl.w * 0.3); }\n", 5, nullptr},
      {"more built-ins", kVs, kFsHead "void main(){ float ip; float fr = modf(v.r * 3.7, ip); vec2 x = v.rg * 2.0 - 1.0; mat2 op = outerProduct(x, vec2(0.5, 0.25)); o = vec4(fr, ip / 4.0, sinh(x.x) * 0.3 + cosh(x.y) * 0.2 + tanh(x.x * 3.0) * 0.2, atanh(x.y * 0.5) * 0.4 + asinh(x.x) * 0.2 + acosh(1.0 + v.b) * 0.2 + op[1][0]); }\n", 5, nullptr},
      {"a shadow sampler, nearest, GREATER", kVs, kFsHead "uniform highp sampler2DShadow u_t;\nvoid main(){ o = vec4(texture(u_t, vec3(v.rg, v.b)), v.g * 0.5, 0.25, 1.0); }\n", 5,
       [](GlContext& gl, uint32_t p) { depth_texture(gl, p, 0x0DE1, 8, 8, 0x2600, 0x0204); }},
      {"a shadow sampler, linear, LEQUAL, projective", kVs, kFsHead "uniform highp sampler2DShadow u_t;\nvoid main(){ o = vec4(textureProj(u_t, vec4(v.rg * 2.0, v.b * 2.0, 2.0)), 0.5, v.r, 1.0); }\n", 5,
       [](GlContext& gl, uint32_t p) { depth_texture(gl, p, 0x0DE1, 8, 4, 0x2601, 0x0203); }},
      {"arrays of structures: by a variable, whole copies, stores and ==", kVs,
       kFsHead "struct L { vec3 c; float k; vec2 w[2]; };\n"
       "void main(){ L ls[3]; for (int i = 0; i < 3; ++i) { ls[i].c = v.rgb * float(i + 1) * 0.3; ls[i].k = float(i) * 0.25; ls[i].w[0] = v.rg; ls[i].w[1] = vec2(float(i)) * 0.1; }\n"
       "  int j = int(v.r * 2.9); L pick = ls[j]; L other = ls[(j + 1) % 3]; ls[j] = other; bool same = pick == ls[(j + 2) % 3]; bool self = pick == pick;\n"
       "  o = vec4(pick.c * 0.5 + ls[j].c * 0.3, pick.k + pick.w[1].x + (same ? 0.5 : 0.0) + (self ? 0.1 : 0.0)); }\n", 5, nullptr},
      {"structure constructors, constant arrays of structures, uniform structures with arrays", kVs,
       kFsHead "struct P { vec2 a; float b; };\nconst P kp[2] = P[2](P(vec2(0.2, 0.4), 0.6), P(vec2(0.8, 0.1), 0.3));\nstruct U { vec4 tint; float s[3]; int n; };\nuniform U u;\n"
       "void main(){ int j = int(v.g * 1.9); P q = kp[j]; P r = P(v.rg, v.b); o = vec4(q.a * 0.5 + r.a * 0.5, mix(q.b, r.b, 0.5) * u.s[int(v.r * 2.9)], 1.0) * u.tint + float(u.n) * 0.05; }\n", 5,
       [](GlContext& gl, uint32_t p) {
         const float tint[4] = {0.9f, 1.0f, 0.8f, 1.0f}, sv[3] = {0.5f, 1.0f, 1.5f};
         const int32_t n = 2;
         set_floats(gl, p, "u.tint", 1, 4, 1, tint);
         set_floats(gl, p, "u.s", 1, 1, 3, sv);
         gl.uniform(gl.get_uniform_location(p, "u.n"), 'i', 1, 1, 1, reinterpret_cast<const uint32_t*>(&n));
       }},
      {"uniform arrays of structures by a variable", kVs, kFsHead "struct M { vec3 c; float k; };\nuniform M ms[3];\nvoid main(){ int j = int(v.r * 2.9); o = vec4(ms[j].c * v.g + ms[2 - j].c * 0.25, ms[j].k); }\n", 5,
       [](GlContext& gl, uint32_t p) {
         const float c[3][3] = {{1, 0, 0}, {0, 1, 0}, {0.2f, 0.4f, 1}}, k[3] = {0.25f, 0.5f, 1.0f};
         const char* const cn[3] = {"ms[0].c", "ms[1].c", "ms[2].c"};
         const char* const kn[3] = {"ms[0].k", "ms[1].k", "ms[2].k"};
         for (int i = 0; i < 3; ++i) {
           set_floats(gl, p, cn[i], 1, 3, 1, c[i]);
           set_floats(gl, p, kn[i], 1, 1, 1, &k[i]);
         }
       }},
      {"a uniform block", kVs, kFsHead "layout(std140) uniform B { vec4 tint; mat2 m; float s[3]; bool on; };\nvoid main(){ vec2 r = m * v.rg; o = vec4(r, v.b * s[int(v.r * 2.9)], 1.0) * tint + (on ? 0.1 : 0.0); }\n", 5,
       [](GlContext& gl, uint32_t p) {
         // std140: tint at 0, m's columns at 16 and 32, s[i] at 48 + 16 i, on at 96
         float block[28] = {0.9f, 0.8f, 1.0f, 1.0f, 0.5f, 0.25f, 0, 0, -0.25f, 0.75f, 0, 0, 0.3f, 0, 0, 0, 0.6f, 0, 0, 0, 0.9f, 0, 0, 0, 0, 0, 0, 0};
         ByteBuffer bytes(gl.allocator());
         for (float f : block) bytes.put32(__builtin_bit_cast(uint32_t, f));
         bytes.writable_view()[96] = 1;
         const uint32_t buf = gl.create_buffer();
         gl.bind_buffer(0x8A11, buf);
         gl.buffer_data(0x8A11, bytes.view(), 0, 0x88E4);
         gl.bind_buffer_range(0x8A11, 1, buf, 0, bytes.size());
         gl.uniform_block_binding(p, gl.get_uniform_block_index(p, StringView("B")), 1);
       }},
      {"gl_FragData", "attribute vec2 pos; attribute vec4 col; varying vec4 v; void main(){ v = col; gl_Position = vec4(pos, 0.0, 1.0); }",
       "precision mediump float; varying vec4 v; void main(){ gl_FragData[0] = v.bgra; }", 5, nullptr},
};
#undef kFsHead
#undef kHead

// A w x h depth texture (32-bit float depth) on unit 0 of `target` (2D or 1D), with a filter and a comparison function, named by uniform u_t: a pattern of depths in tenths.
void depth_texture(GlContext& gl, uint32_t p, uint32_t target, uint32_t w, uint32_t h, uint32_t filter, uint32_t func) {
  ByteBuffer bytes(gl.allocator());
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) bytes.put32(__builtin_bit_cast(uint32_t, static_cast<float>((x * 7 + y * 3) % 11) / 10.0f));
  const uint32_t tex = gl.create_texture();
  gl.active_texture(0x84C0);
  gl.bind_texture(target, tex);
  gl.tex_image(target, 0, 0x8CAC, w, h, 1, 0, 0x1902, 0x1406, bytes.view(), true, false);
  gl.tex_parameter(target, 0x2801, filter);
  gl.tex_parameter(target, 0x2800, filter);
  gl.tex_parameter(target, 0x2802, 0x812F);   // (clamped: the edges are the CPU's and the device's alike)
  gl.tex_parameter(target, 0x2803, 0x812F);
  gl.tex_parameter(target, 0x884C, 0x884E);   // COMPARE_REF_TO_TEXTURE
  gl.tex_parameter(target, 0x884D, func);
  const uint32_t unit = 0;
  gl.uniform(gl.get_uniform_location(p, "u_t"), 'i', 1, 1, 1, &unit);
}

// Clip planes on: distances 0 and 1, as GlApi sets them for a program (its clip varying), and the user planes x > -0.25 and x + y < 0.53125 (between pixel centres:
// a plane through one would leave it to the rounding of each).
void clip_two(GlContext& gl, uint32_t p) {
  const ProgramObj* po = gl.program(p);
  gl.desktop.clip_varying = po ? po->clip_varying : -1;
  gl.desktop.clip_mask = gl.desktop.clip_varying >= 0 ? 3 : 0;
  gl.desktop.clip_indexed = true;
  const float planes[2][4] = {{1, 0, 0, 0.25f}, {-1, -1, 0, 0.53125f}};
  for (int i = 0; i < 2; ++i)
    for (int k = 0; k < 4; ++k) gl.desktop.clip_planes[i][k] = planes[i][k];
}

// The scenes with geometry and tessellation stages. The vertex shader is the one above in desktop GLSL; the stages after it read v and vk per vertex.
#define kDesk "#version 400 core\n"
#define kDeskFs kDesk "in vec4 g; out vec4 o;\nvoid main(){ o = g; }\n"
constexpr const char* kDeskVs = kDesk "in vec2 pos; in vec4 col; in float k; out vec4 v; out float vk;\nvoid main(){ v = col; vk = k; gl_Position = vec4(pos, 0.0, 1.0); }\n";
constexpr const char* kPassTcs3 = kDesk "layout(vertices = 3) out;\nin vec4 v[]; out vec4 tv[];\nvoid main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position; tv[gl_InvocationID] = v[gl_InvocationID]; gl_TessLevelOuter[0] = 2.0; gl_TessLevelOuter[1] = 2.0; gl_TessLevelOuter[2] = 2.0; gl_TessLevelInner[0] = 2.0; }\n";
constexpr const char* kTriTes = kDesk "layout(triangles, equal_spacing, ccw) in;\nin vec4 tv[]; out vec4 g;\nvoid main(){ vec3 c = gl_TessCoord; gl_Position = gl_in[0].gl_Position * c.x + gl_in[1].gl_Position * c.y + gl_in[2].gl_Position * c.z; gl_Position.xy += vec2(c.x * c.y, c.y * c.z) * 0.4; g = tv[0] * c.x + tv[1] * c.y + tv[2] * c.z; g.rgb = g.rgb * 0.5 + c * c * 0.5; }\n";

constexpr Scene kStageScenes[] = {
      {"geometry: triangles passed through, a value per primitive", kDeskVs, kDeskFs, 5, nullptr, true,
       kDesk "layout(triangles) in; layout(triangle_strip, max_vertices = 3) out;\nin vec4 v[]; in float vk[]; out vec4 g;\nvoid main(){ vec4 mean = (v[0] + v[1] + v[2]) / 3.0; for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position; g = v[i] * 0.5 + mean * 0.5 + vec4(vk[i] * 0.05); EmitVertex(); } EndPrimitive(); }\n"},
      {"geometry: points made quads", kDeskVs, kDeskFs, 0, nullptr, true,
       kDesk "layout(points) in; layout(triangle_strip, max_vertices = 4) out;\nin vec4 v[]; out vec4 g;\nvoid main(){ vec4 p = gl_in[0].gl_Position; for (int i = 0; i < 4; ++i) { vec2 o = vec2(float(i % 2), float(i / 2)) * 0.5 - 0.25; gl_Position = vec4(p.xy * 0.6 + o, p.zw); g = v[0] * (0.6 + 0.1 * float(i)); EmitVertex(); } }\n",
       nullptr, nullptr, 0, 6},
      {"geometry: a band from lines with adjacency", kDeskVs, kDeskFs, 0xA, nullptr, true,
       kDesk "layout(lines_adjacency) in; layout(triangle_strip, max_vertices = 4) out;\nin vec4 v[]; out vec4 g;\nvoid main(){ vec2 a = gl_in[1].gl_Position.xy, b = gl_in[2].gl_Position.xy; vec2 d = normalize(b - a); vec2 n = vec2(-d.y, d.x) * 0.4; vec4 side = (v[0] + v[3]) * 0.5;\n"
       "  gl_Position = vec4(a + n, 0.0, 1.0); g = mix(v[1], side, 0.25); EmitVertex(); gl_Position = vec4(a - n, 0.0, 1.0); g = mix(v[1], side, 0.75); EmitVertex();\n"
       "  gl_Position = vec4(b + n, 0.0, 1.0); g = mix(v[2], side, 0.25); EmitVertex(); gl_Position = vec4(b - n, 0.0, 1.0); g = mix(v[2], side, 0.75); EmitVertex(); }\n",
       nullptr, nullptr, 0, 4},
      {"geometry: two invocations and gl_PrimitiveIDIn", kDeskVs, kDeskFs, 4, [](GlContext& gl, uint32_t) { gl.enable(0x0B71, true); }, true,
       kDesk "layout(triangles, invocations = 2) in; layout(triangle_strip, max_vertices = 3) out;\nin vec4 v[]; out vec4 g;\nvoid main(){ float s = gl_InvocationID == 0 ? 1.0 : 0.5; for (int i = 0; i < 3; ++i) { gl_Position = vec4(gl_in[i].gl_Position.xy * s, -0.2 * float(gl_InvocationID), 1.0); g = vec4(v[i].rgb * s, 1.0) + vec4(0.0, 0.0, float(gl_PrimitiveIDIn) * 0.3, 0.0); EmitVertex(); } }\n"},
      {"geometry: two strips from one triangle, flat colour", kDeskVs, kDesk "flat in vec4 g; in float t; out vec4 o;\nvoid main(){ o = vec4(g.rgb, 1.0) * 0.8 + t * 0.2; }\n", 4, nullptr, true,
       kDesk "layout(triangles) in; layout(triangle_strip, max_vertices = 6) out;\nin vec4 v[]; flat out vec4 g; out float t;\nvoid main(){ vec2 c = (gl_in[0].gl_Position.xy + gl_in[1].gl_Position.xy + gl_in[2].gl_Position.xy) / 3.0; for (int h = 0; h < 2; ++h) { for (int i = 0; i < 3; ++i) { gl_Position = vec4(mix(c, gl_in[i].gl_Position.xy, h == 0 ? 1.0 : 0.5), 0.0, 1.0); g = v[i] * (h == 0 ? 0.5 : 1.0); t = float(i) * 0.3; EmitVertex(); } EndPrimitive(); } }\n"},
      {"tessellation: triangles, levels from a uniform", kDeskVs, kDeskFs, 0xE,
       [](GlContext& gl, uint32_t p) { const float l = 3.0f; set_floats(gl, p, "u_level", 1, 1, 1, &l); }, true, nullptr,
       kDesk "layout(vertices = 3) out;\nin vec4 v[]; out vec4 tv[]; uniform float u_level;\nvoid main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position; tv[gl_InvocationID] = v[gl_InvocationID]; if (gl_InvocationID == 0) { gl_TessLevelOuter[0] = u_level; gl_TessLevelOuter[1] = u_level + 1.0; gl_TessLevelOuter[2] = 2.0; gl_TessLevelInner[0] = 3.0; } }\n",
       kTriTes, 3, 6},
      {"tessellation: quads, odd spacing, a patch varying", kDeskVs, kDeskFs, 0xE, nullptr, true, nullptr,
       kDesk "layout(vertices = 4) out;\nin vec4 v[]; out vec4 tv[]; patch out vec4 pc;\nvoid main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position; tv[gl_InvocationID] = v[gl_InvocationID]; pc = (v[0] + v[1] + v[2] + v[3]) * 0.25; gl_TessLevelOuter[0] = 3.0; gl_TessLevelOuter[1] = 5.0; gl_TessLevelOuter[2] = 3.0; gl_TessLevelOuter[3] = 1.0; gl_TessLevelInner[0] = 3.0; gl_TessLevelInner[1] = 5.0; barrier(); }\n",
       kDesk "layout(quads, fractional_odd_spacing, ccw) in;\nin vec4 tv[]; patch in vec4 pc; out vec4 g;\nvoid main(){ vec2 c = gl_TessCoord.xy; vec4 a = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, c.x), b = mix(gl_in[2].gl_Position, gl_in[3].gl_Position, c.x); gl_Position = mix(a, b, c.y); gl_Position.x += c.y * (1.0 - c.y) * 0.3; vec4 ca = mix(tv[0], tv[1], c.x), cb = mix(tv[2], tv[3], c.x); g = mix(ca, cb, c.y) * 0.6 + pc * 0.4; g.b = c.x * c.y; }\n",
       4, 4},
      {"tessellation: an evaluation stage alone, the default levels", kDeskVs, kDeskFs, 0xE,
       [](GlContext& gl, uint32_t) { gl.desktop.patch_outer[0] = 3; gl.desktop.patch_outer[1] = 2; gl.desktop.patch_outer[2] = 4; gl.desktop.patch_inner[0] = 2; }, true, nullptr, nullptr,
       kDesk "layout(triangles, equal_spacing, cw) in;\nin vec4 v[]; out vec4 g;\nvoid main(){ vec3 c = gl_TessCoord; gl_Position = gl_in[0].gl_Position * c.x + gl_in[1].gl_Position * c.y + gl_in[2].gl_Position * c.z; gl_Position.y -= c.x * c.z * 0.5; g = v[0] * c.x + v[1] * c.y + v[2] * c.z; g.b = gl_TessLevelOuter[2] * 0.2 * c.y; g.a = 1.0; }\n",
       3, 6},
      {"tessellation: outputs read across barrier(), patch outputs read back", kDeskVs, kDeskFs, 0xE, nullptr, true, nullptr,
       kDesk "layout(vertices = 3) out;\nin vec4 v[]; out vec4 tv[]; out float w[]; patch out vec4 pc;\n"
       "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position; w[gl_InvocationID] = float(gl_InvocationID) * 0.3 + v[gl_InvocationID].r * 0.2;\n"
       "  if (gl_InvocationID == 0) { gl_TessLevelOuter[0] = 2.0; gl_TessLevelOuter[1] = gl_TessLevelOuter[0] + 1.0; gl_TessLevelOuter[2] = 2.0; gl_TessLevelInner[0] = 2.0; pc = v[0]; }\n"
       "  barrier();\n"
       "  int next = (gl_InvocationID + 1) % 3; tv[gl_InvocationID] = vec4(w[next], gl_out[next].gl_Position.x * 0.5 + 0.5, pc.b, 1.0) * 0.5 + v[gl_InvocationID] * 0.5; }\n",
       kTriTes, 3, 6},
      {"geometry: a vertex shader without attributes", kDesk "out float c;\nvoid main(){ c = 0.7; }\n", kDeskFs, 0, nullptr, true,
       kDesk "layout(points) in; layout(triangle_strip, max_vertices = 4) out;\nin float c[]; out vec4 g;\nvoid main(){ for (int i = 0; i < 4; ++i) { gl_Position = vec4(float(i % 2) * 1.4 - 0.8, float(i / 2) * 1.2 - 0.6, 0.0, 1.0); g = vec4(c[0], float(i) * 0.25, 1.0 - c[0], 1.0); EmitVertex(); } }\n",
       nullptr, nullptr, 0, 1},
      {"shadow2D (legacy), nearest, LESS", "#version 120\nattribute vec2 pos; attribute vec4 col; varying vec4 v;\nvoid main(){ v = col; gl_Position = vec4(pos, 0.0, 1.0); }\n",
       "#version 120\nuniform sampler2DShadow u_t; varying vec4 v;\nvoid main(){ gl_FragColor = shadow2D(u_t, vec3(v.gr, v.b)) * 0.75 + vec4(0.0, 0.0, 0.2, 0.0); }\n", 5,
       [](GlContext& gl, uint32_t p) { depth_texture(gl, p, 0x0DE1, 16, 8, 0x2600, 0x0201); }, true},
      {"shadow1DProj (legacy), linear, GEQUAL", "#version 120\nattribute vec2 pos; attribute vec4 col; varying vec4 v;\nvoid main(){ v = col; gl_Position = vec4(pos, 0.0, 1.0); }\n",
       "#version 120\nuniform sampler1DShadow u_t; varying vec4 v;\nvoid main(){ gl_FragColor = shadow1DProj(u_t, vec4(v.r * 1.5, 0.0, v.b * 1.5, 1.5)); }\n", 5,
       [](GlContext& gl, uint32_t p) { depth_texture(gl, p, 0x0DE0, 16, 1, 0x2601, 0x0206); }, true},
      {"a rectangle texture", "#version 140\nin vec2 pos; in vec4 col; out vec4 v;\nvoid main(){ v = col; gl_Position = vec4(pos, 0.0, 1.0); }\n",
       "#version 140\nuniform sampler2DRect u_t; in vec4 v; out vec4 o;\nvoid main(){ o = texture(u_t, v.rg * vec2(2.0, 2.0)) * 0.8 + v * 0.2; }\n", 5,
       [](GlContext& gl, uint32_t p) {
         const uint32_t tex = gl.create_texture();
         const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
         gl.active_texture(0x84C0);
         gl.bind_texture(0x84F5, tex);
         gl.tex_image(0x84F5, 0, 0x1908, 2, 2, 1, 0, 0x1908, 0x1401, ConstByteView(texels, sizeof texels), true, false);
         gl.tex_parameter(0x84F5, 0x2801, 0x2600);
         gl.tex_parameter(0x84F5, 0x2800, 0x2600);
         const uint32_t unit = 0;
         gl.uniform(gl.get_uniform_location(p, "u_t"), 'i', 1, 1, 1, &unit);
       }, true},
      {"clip distances", kDesk "in vec2 pos; in vec4 col; out vec4 g;\nvoid main(){ g = col; gl_Position = vec4(pos, 0.0, 1.0); gl_ClipDistance[0] = pos.x + 0.25; gl_ClipDistance[1] = 0.5 - pos.y; }\n", kDeskFs, 5, clip_two, true},
      {"clip planes against gl_ClipVertex (compatibility)", "#version 120\nattribute vec2 pos; attribute vec4 col; varying vec4 g;\nvoid main(){ g = col; gl_Position = vec4(pos, 0.0, 1.0); gl_ClipVertex = vec4(pos, 0.0, 1.0); }\n",
       "#version 120\nvarying vec4 g;\nvoid main(){ gl_FragColor = g; }\n", 5, clip_two, true},
      {"geometry: clip distances from the geometry stage", kDeskVs, kDeskFs, 4, clip_two, true,
       kDesk "layout(triangles) in; layout(triangle_strip, max_vertices = 3) out;\nin vec4 v[]; out vec4 g;\nvoid main(){ for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position; g = v[i]; gl_ClipDistance[0] = gl_in[i].gl_Position.y + 0.25; gl_ClipDistance[1] = 1.0; EmitVertex(); } }\n"},
      {"tessellation and geometry together", kDeskVs, kDeskFs, 0xE, nullptr, true,
       kDesk "layout(triangles) in; layout(triangle_strip, max_vertices = 3) out;\nin vec4 g0[]; out vec4 g;\nvoid main(){ vec4 c = (gl_in[0].gl_Position + gl_in[1].gl_Position + gl_in[2].gl_Position) / 3.0; for (int i = 0; i < 3; ++i) { gl_Position = mix(c, gl_in[i].gl_Position, 0.8); g = g0[i]; EmitVertex(); } }\n",
       kPassTcs3,
       kDesk "layout(triangles, equal_spacing, ccw) in;\nin vec4 tv[]; out vec4 g0;\nvoid main(){ vec3 c = gl_TessCoord; gl_Position = gl_in[0].gl_Position * c.x + gl_in[1].gl_Position * c.y + gl_in[2].gl_Position * c.z; g0 = tv[0] * c.x + tv[1] * c.y + tv[2] * c.z; }\n",
       3, 6},
};
#undef kDeskFs
#undef kDesk

constexpr uint32_t kSize = 32;

// Draws a scene with whatever accelerator is installed: the picture (RGBA bytes, the bottom row first), how many draws the device took, and why not if none.
bool draw_scene(allocators::IAllocator& a, const Scene& sc, ByteView out, uint64_t* gpu_draws, const char** why) {
  GlContext::Attributes attrs;
  attrs.antialias = false;
  GlContext gl(a, true, kSize, kSize, attrs);
  if (!gl.valid()) return false;
  gl.desktop.active = sc.desktop;
  const uint32_t p = gl.create_program();
  const struct { uint32_t type; const char* source; } stages[5] = {{0x8B31, sc.vs}, {0x8B30, sc.fs}, {0x8DD9, sc.gs}, {0x8E88, sc.tcs}, {0x8E87, sc.tes}};
  for (const auto& st : stages) {
    if (!st.source) continue;
    const uint32_t s = gl.create_shader(st.type);
    gl.shader_source(s, StringView(st.source));
    gl.compile_shader(s);
    gl.attach_shader(p, s);
  }
  gl.link_program(p);
  if (sc.patch_vertices) gl.desktop.patch_vertices = sc.patch_vertices;
  double ok = 0;
  gl.program_status(p, 0x8B82, &ok);
  if (!ok) return false;
  gl.use_program(p);
  // per vertex: pos, col, k
  static const float kStrip[] = {-1, -1, 1, 0, 0, 1, 0, 1, -1, 0, 1, 0, 1, 1, -1, 1, 0, 0, 1, 1, 2, 1, 1, 1, 1, 0, 1, 3};
  static const float kTris[] = {-1, -1, 1, 0, 0, 1, 0, 1, -1, 0, 1, 0, 1, 1, -1, 1, 0, 0, 1, 1, 2,
                                1, 1, 0.5f, 0.5f, 0, 1, 3, 1, -1, 0.2f, 0.9f, 0.4f, 1, 4, -1, 1, 0.7f, 0.1f, 0.6f, 1, 5};
  const uint32_t count = sc.count ? sc.count : (sc.mode == 5 ? 4 : 6);
  const ArrayView<const float> verts = count == 4 ? ArrayView<const float>(kStrip, sizeof kStrip / sizeof kStrip[0]) : ArrayView<const float>(kTris, sizeof kTris / sizeof kTris[0]);
  ByteBuffer bytes(a);
  for (float x : verts)
    if (!bytes.put32(__builtin_bit_cast(uint32_t, x))) return false;
  const uint32_t buf = gl.create_buffer();
  gl.bind_buffer(0x8892, buf);
  gl.buffer_data(0x8892, bytes.view(), 0, 0x88E4);
  const char* const names[3] = {"pos", "col", "k"};
  const int32_t sizes[3] = {2, 4, 1};
  const uint32_t offsets[3] = {0, 8, 24};
  for (int i = 0; i < 3; ++i) {
    const int32_t l = gl.get_attrib_location(p, StringView(names[i]));
    if (l < 0) continue;
    gl.enable_vertex_attrib(static_cast<uint32_t>(l), true);
    gl.vertex_attrib_pointer(static_cast<uint32_t>(l), sizes[i], 0x1406, false, 28, offsets[i], false);
  }
  if (sc.setup) sc.setup(gl, p);
  gl.clear_color(0.1f, 0.2f, 0.3f, 1);
  gl.clear(0x4000 | 0x100);
  gl.draw_arrays(sc.mode, 0, static_cast<int32_t>(count), 1);
  gl.read_pixels(0, 0, kSize, kSize, 0x1908, 0x1401, out);
  *gpu_draws = gl.hardware_stats().gpu_draws;
  *why = gl.hardware_stats().last_refusal;
  return gl.get_error() == 0;
}

}  // namespace

uint32_t accel_scene_count() { return sizeof kScenes / sizeof kScenes[0] + sizeof kStageScenes / sizeof kStageScenes[0]; }

uint32_t run_accel_conformance(allocators::IAllocator& allocator, IAccelerator& accelerator, void (*report)(void* context, const AccelSceneResult& result), void* context) {
  IAccelerator* const before = webgl::accelerator();
  uint32_t wrong = 0;
  ByteBuffer cpu(allocator), gpu(allocator);
  if (!cpu.put_zeros(uint64_t{kSize} * kSize * 4) || !gpu.put_zeros(uint64_t{kSize} * kSize * 4)) return accel_scene_count();
  for (uint32_t si = 0; si < accel_scene_count(); ++si) {
    const uint32_t first = sizeof kScenes / sizeof kScenes[0];
    const Scene& sc = si < first ? kScenes[si] : kStageScenes[si - first];
    AccelSceneResult r;
    r.name = sc.name;
    uint64_t gpu_draws = 0;
    const char* why = nullptr;
    set_accelerator(nullptr);
    const bool cpu_ok = draw_scene(allocator, sc, cpu.writable_view(), &gpu_draws, &why);
    set_accelerator(&accelerator);
    const bool gpu_ok = draw_scene(allocator, sc, gpu.writable_view(), &gpu_draws, &why);
    set_accelerator(before);
    if (!cpu_ok || !gpu_ok) {
      r.verdict = AccelVerdict::Broken;
      ++wrong;
    } else if (gpu_draws != 1) {
      r.verdict = AccelVerdict::NotTaken;
      r.why = why;
    } else {
      r.verdict = AccelVerdict::Same;
      const ConstByteView c = cpu.view(), g = gpu.view();
      for (uint64_t i = 0; i + 3 < c.size(); i += 4) r.drawn_pixels += !(c[i] == 26 && c[i + 1] == 51 && c[i + 2] == 77);
      for (uint64_t i = 0; i < c.size(); ++i) {
        const int d = c[i] > g[i] ? c[i] - g[i] : g[i] - c[i];
        if (d <= 3) continue;
        if (!r.bad_channels) {
          r.first_x = static_cast<uint32_t>((i / 4) % kSize);
          r.first_y = static_cast<uint32_t>((i / 4) / kSize);
          r.first_channel = static_cast<uint32_t>(i % 4);
          r.cpu_value = c[i];
          r.device_value = g[i];
        }
        ++r.bad_channels;
      }
      if (r.bad_channels) {
        r.verdict = AccelVerdict::Differs;
        ++wrong;
      }
    }
    if (report) report(context, r);
  }
  return wrong;
}

}  // namespace webgl
