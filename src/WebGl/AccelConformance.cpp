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
};

#define kHead "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#define kFsHead kHead "in vec4 v; in float vk; out vec4 o;\n"
const char* const kVs = "#version 300 es\nin vec2 pos; in vec4 col; in float k; out vec4 v; out float vk;\nvoid main(){ v = col; vk = k; gl_Position = vec4(pos, 0.0, 1.0); }\n";

void set_floats(GlContext& gl, uint32_t p, const char* name, uint32_t cols, uint32_t rows, uint32_t count, const float* f) {
  uint32_t lanes[64];
  const uint32_t n = cols * rows * count;
  for (uint32_t i = 0; i < n && i < 64; ++i) lanes[i] = __builtin_bit_cast(uint32_t, f[i]);
  gl.uniform(gl.get_uniform_location(p, name), 'f', cols, rows, count, lanes);
}

// a 2 x 2 RGBA texture on unit 0
void upload_2x2(GlContext& gl, uint32_t tex, const uint8_t (&texels)[16]) {
  gl.active_texture(0x84C0);
  gl.bind_texture(0x0DE1, tex);
  gl.tex_image(0x0DE1, 0, 0x1908, 2, 2, 1, 0, 0x1908, 0x1401, ConstByteView(texels, sizeof texels), true, false);
}

const Scene kScenes[] = {
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
      {"gl_FragData", "attribute vec2 pos; attribute vec4 col; varying vec4 v; void main(){ v = col; gl_Position = vec4(pos, 0.0, 1.0); }",
       "precision mediump float; varying vec4 v; void main(){ gl_FragData[0] = v.bgra; }", 5, nullptr},
};
#undef kFsHead
#undef kHead

constexpr uint32_t kSize = 32;

// Draws a scene with whatever accelerator is installed: the picture (RGBA bytes, the bottom row first), how many draws the device took, and why not if none.
bool draw_scene(allocators::IAllocator& a, const Scene& sc, ByteView out, uint64_t* gpu_draws, const char** why) {
  GlContext::Attributes attrs;
  attrs.antialias = false;
  GlContext gl(a, true, kSize, kSize, attrs);
  if (!gl.valid()) return false;
  const uint32_t v = gl.create_shader(0x8B31), f = gl.create_shader(0x8B30), p = gl.create_program();
  gl.shader_source(v, StringView(sc.vs));
  gl.compile_shader(v);
  gl.shader_source(f, StringView(sc.fs));
  gl.compile_shader(f);
  gl.attach_shader(p, v);
  gl.attach_shader(p, f);
  gl.link_program(p);
  double ok = 0;
  gl.program_status(p, 0x8B82, &ok);
  if (!ok) return false;
  gl.use_program(p);
  // per vertex: pos, col, k
  static const float kStrip[] = {-1, -1, 1, 0, 0, 1, 0, 1, -1, 0, 1, 0, 1, 1, -1, 1, 0, 0, 1, 1, 2, 1, 1, 1, 1, 0, 1, 3};
  static const float kTris[] = {-1, -1, 1, 0, 0, 1, 0, 1, -1, 0, 1, 0, 1, 1, -1, 1, 0, 0, 1, 1, 2,
                                1, 1, 0.5f, 0.5f, 0, 1, 3, 1, -1, 0.2f, 0.9f, 0.4f, 1, 4, -1, 1, 0.7f, 0.1f, 0.6f, 1, 5};
  const ArrayView<const float> verts = sc.mode == 5 ? ArrayView<const float>(kStrip, sizeof kStrip / sizeof kStrip[0]) : ArrayView<const float>(kTris, sizeof kTris / sizeof kTris[0]);
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
  gl.draw_arrays(sc.mode, 0, sc.mode == 5 ? 4 : 6, 1);
  gl.read_pixels(0, 0, kSize, kSize, 0x1908, 0x1401, out);
  *gpu_draws = gl.hardware_stats().gpu_draws;
  *why = gl.hardware_stats().last_refusal;
  return gl.get_error() == 0;
}

}  // namespace

uint32_t accel_scene_count() { return sizeof kScenes / sizeof kScenes[0]; }

uint32_t run_accel_conformance(allocators::IAllocator& allocator, IAccelerator& accelerator, void (*report)(void* context, const AccelSceneResult& result), void* context) {
  IAccelerator* const before = webgl::accelerator();
  uint32_t wrong = 0;
  ByteBuffer cpu(allocator), gpu(allocator);
  if (!cpu.put_zeros(uint64_t{kSize} * kSize * 4) || !gpu.put_zeros(uint64_t{kSize} * kSize * 4)) return accel_scene_count();
  for (const Scene& sc : kScenes) {
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
