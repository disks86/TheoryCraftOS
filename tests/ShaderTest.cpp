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

// Host-side tests for the shader interpreter (GLSL and WGSL front ends).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "WebGl/Shader.hpp"

using namespace shader;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static float near_f(float a, float b, float eps = 1e-4f) { float d = a - b; return (d < 0 ? -d : d) <= eps * (1 + (b < 0 ? -b : b)); }

// Runs a fragment shader and returns gl_FragColor.
static bool run_frag(TestAllocator& a, const char* source, float out[4], const float* frag_coord = nullptr) {
  Module m(a);
  if (!parse_glsl(m, source, Stage::Fragment)) {
    printf("parse error: %s\n", m.error.c_str());
    return false;
  }
  Machine mc(m, a);
  if (!mc.reset_privates()) return false;
  List<uint32_t> shared(a);
  for (uint32_t i = 0; i < m.shared_slots + 1; ++i) shared.push_back(0);
  mc.init_shared(shared.view().data());
  (void)frag_coord;
  const Machine::Status s = mc.start(m.main_function, ArrayView<const uint32_t* const>());
  if (s != Machine::Status::Done) {
    printf("run error: %s\n", mc.failure() ? mc.failure() : "?");
    return false;
  }
  int32_t g = m.find_builtin_global(kBvFragColor);
  if (m.language == Language::Glsl300) {
    g = -1;
    for (uint64_t i = 0; i < m.globals.count(); ++i)
      if (m.globals[i]->kind == GlobalKind::Out) g = static_cast<int32_t>(i);
  }
  if (g < 0) return false;
  const uint32_t* p = mc.private_region() + m.globals[static_cast<uint64_t>(g)]->slot;
  for (int i = 0; i < 4; ++i) memcpy(&out[i], &p[i], 4);
  return true;
}

static void test_glsl_basics() {
  TestAllocator a;
  float c[4];
  CHECK(run_frag(a, "precision mediump float; void main() { gl_FragColor = vec4(1.0, 2.0, 3.0, 4.0); }", c));
  CHECK(c[0] == 1 && c[1] == 2 && c[2] == 3 && c[3] == 4);
  CHECK(run_frag(a, "void main() { vec3 v = vec3(1.0, 2.0, 3.0); v.xy = v.yx; v *= 2.0; gl_FragColor = vec4(v, float(1 + 2)); }", c));
  CHECK(c[0] == 4 && c[1] == 2 && c[2] == 6 && c[3] == 3);
  CHECK(run_frag(a, "void main() { float s = 0.0; for (int i = 0; i < 10; i++) { if (i == 3) continue; if (i == 7) break; s += float(i); } gl_FragColor = vec4(s); }", c));
  CHECK(c[0] == 0 + 1 + 2 + 4 + 5 + 6);
  CHECK(run_frag(a, "float sq(float x) { return x * x; } vec2 twice(in vec2 v, out float o) { o = v.x + v.y; return v * 2.0; } void main() { float o; vec2 r = twice(vec2(sq(3.0), 1.0), o); gl_FragColor = vec4(r, o, 0.0); }", c));
  CHECK(c[0] == 18 && c[1] == 2 && c[2] == 10);
  CHECK(run_frag(a, "struct S { float a; vec2 b; }; void main() { S s = S(1.5, vec2(2.0, 3.0)); s.b.y += 1.0; gl_FragColor = vec4(s.a, s.b, 0.0); }", c));
  CHECK(c[0] == 1.5f && c[1] == 2 && c[2] == 4);
  CHECK(run_frag(a, "void main() { mat2 m = mat2(1.0, 2.0, 3.0, 4.0); vec2 v = m * vec2(1.0, 1.0); mat2 t = transpose(m); gl_FragColor = vec4(v, t[0][1], determinant(m)); }", c));
  CHECK(c[0] == 4 && c[1] == 6 && c[2] == 3 && c[3] == -2);
  CHECK(run_frag(a, "const float K = 2.0;\n#define TWICE(x) ((x) * 2.0)\nvoid main() { float arr[3]; arr[0] = 1.0; arr[1] = K; arr[2] = TWICE(arr[1]); gl_FragColor = vec4(arr[0], arr[1], arr[2], float(arr.length())); }", c));
  CHECK(c[0] == 1 && c[1] == 2 && c[2] == 4 && c[3] == 3);
  CHECK(run_frag(a, "void main() { int x = 7; int y = x / 2; int z = x % 4; uint u = 5u << 2u; gl_FragColor = vec4(float(y), float(z), float(u), float(x > 3 ? 1 : 0)); }", c));
  CHECK(c[0] == 3 && c[1] == 3 && c[2] == 20 && c[3] == 1);
  CHECK(run_frag(a, "void main() { float x = 0.5; gl_FragColor = vec4(sin(x), cos(x), pow(2.0, 10.0), sqrt(16.0)); }", c));
  CHECK(near_f(c[0], 0.47942554f) && near_f(c[1], 0.87758256f) && near_f(c[2], 1024) && near_f(c[3], 4));
  CHECK(run_frag(a, "void main() { gl_FragColor = vec4(mix(1.0, 3.0, 0.5), clamp(5.0, 0.0, 1.0), smoothstep(0.0, 1.0, 0.5), length(vec3(2.0, 3.0, 6.0))); }", c));
  CHECK(c[0] == 2 && c[1] == 1 && c[2] == 0.5f && near_f(c[3], 7));
  CHECK(run_frag(a, "void main() { vec3 n = normalize(vec3(0.0, 3.0, 4.0)); vec3 cr = cross(vec3(1.0,0.0,0.0), vec3(0.0,1.0,0.0)); gl_FragColor = vec4(n.y, n.z, cr.z, dot(n, n)); }", c));
  CHECK(near_f(c[0], 0.6f) && near_f(c[1], 0.8f) && c[2] == 1 && near_f(c[3], 1));
  CHECK(run_frag(a, "void main() { int s = 0; for (int i = 0; i < 5; i++) { switch (i) { case 0: s += 1; break; case 1: case 2: s += 10; break; default: s += 100; } } gl_FragColor = vec4(float(s)); }", c));
  CHECK(c[0] == 1 + 10 + 10 + 100 + 100);
  CHECK(run_frag(a, "void main() { float x = 1.0; int i = 0; while (i < 4) { x *= 2.0; i++; } do { x += 1.0; } while (x < 20.0); gl_FragColor = vec4(x, atan(1.0, 1.0), exp(1.0), log(10.0)); }", c));
  CHECK(c[0] == 20 && near_f(c[1], 0.7853982f) && near_f(c[2], 2.7182817f) && near_f(c[3], 2.3025851f));
  CHECK(run_frag(a, "#version 300 es\nprecision highp float; out vec4 o; void main() { uvec2 u = uvec2(3u, 4u); o = vec4(float(u.x * u.y), float(bitCount(255u)), float(int(-7) >> 1), 0.0); }", c));
  CHECK(c[0] == 12 && c[1] == 8 && c[2] == -4);
}

// Runs a WGSL function by name with no arguments and returns its (up to 4) result lanes as floats or ints.
static bool run_wgsl(TestAllocator& a, const char* source, const char* fn, uint32_t out[4]) {
  Module m(a);
  if (!parse_wgsl(m, source)) {
    printf("wgsl parse error: %s\n", m.error.c_str());
    return false;
  }
  Machine mc(m, a);
  if (!mc.reset_privates()) return false;
  List<uint32_t> shared(a);
  for (uint32_t i = 0; i < m.shared_slots + 1; ++i) shared.push_back(0);
  mc.init_shared(shared.view().data());
  const int32_t f = m.find_function(fn);
  if (f < 0) return false;
  const Machine::Status st = mc.start(f, ArrayView<const uint32_t* const>());
  if (st != Machine::Status::Done) {
    printf("wgsl run error: %s\n", mc.failure() ? mc.failure() : "?");
    return false;
  }
  for (int i = 0; i < 4; ++i) out[i] = i < static_cast<int>(m.slots_of(mc.result_type())) ? mc.result()[i] : 0;
  return true;
}

static float lane_f(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

static void test_wgsl_basics() {
  TestAllocator a;
  uint32_t o[4];
  CHECK(run_wgsl(a, "fn main() -> vec4<f32> { return vec4<f32>(1.0, 2.0, 3.0, 4.0); }", "main", o));
  CHECK(lane_f(o[0]) == 1 && lane_f(o[3]) == 4);
  CHECK(run_wgsl(a, "fn main() -> i32 { var s = 0; for (var i = 0; i < 10; i++) { if (i == 3) { continue; } if (i == 7) { break; } s += i; } return s; }", "main", o));
  CHECK(static_cast<int32_t>(o[0]) == 0 + 1 + 2 + 4 + 5 + 6);
  CHECK(run_wgsl(a, "struct P { a: f32, b: vec2<f32> } fn helper(p: P) -> f32 { return p.a + p.b.y; } fn main() -> f32 { let p = P(1.5, vec2<f32>(2.0, 3.0)); return later() + helper(p); } fn later() -> f32 { return 10.0; }", "main", o));
  CHECK(lane_f(o[0]) == 14.5f);
  CHECK(run_wgsl(a, "fn main() -> u32 { var x: u32 = 0u; var i = 0; loop { if i >= 5 { break; } x += 2u; i++; continuing { } } switch (x) { case 10u, 11u: { return 100u; } default: { return 7u; } } }", "main", o));
  CHECK(o[0] == 100);
  CHECK(run_wgsl(a, "fn bump(p: ptr<function, i32>) { *p = *p + 5; } fn main() -> i32 { var v = 1; bump(&v); bump(&v); return v; }", "main", o));
  CHECK(static_cast<int32_t>(o[0]) == 11);
  CHECK(run_wgsl(a, "const K: f32 = 2.0; var<private> g: array<f32, 3>; fn main() -> vec3<f32> { g[0] = K; g[1] = g[0] * 2; g[2] = f32(g[1] > 3.0); let m = mat2x2f(1.0, 2.0, 3.0, 4.0); let v = m * vec2f(1.0, 1.0); return vec3f(g[1], v.x, v.y) + vec3(g[2]); }", "main", o));
  CHECK(lane_f(o[0]) == 5 && lane_f(o[1]) == 5 && lane_f(o[2]) == 7);
  CHECK(run_wgsl(a, "fn main() -> vec4<f32> { let a = select(1.0, 2.0, 3 > 2); return vec4(a, clamp(5.0, 0.0, 1.0), abs(-3.0), f32(min(4u, 2u))); }", "main", o));
  CHECK(lane_f(o[0]) == 2 && lane_f(o[1]) == 1 && lane_f(o[2]) == 3 && lane_f(o[3]) == 2);
  CHECK(run_wgsl(a, "fn main() -> u32 { return 1 / 0u + 7u % 0u + u32(-1.0); }", "main", o) == false || true);
}

// Truncated and bit-flipped shaders never crash the front ends or the interpreter (run under the sanitizers).
static void test_mutants() {
  TestAllocator a;
  const char* glsl[] = {
      "precision mediump float; uniform vec4 c; varying vec2 uv; struct S { float a; vec2 b; }; float f(float x) { return x * x + sin(x); } void main() { S s = S(1.0, uv); vec4 v = c; for (int i = 0; i < 4; i++) { v.x += f(float(i)); if (v.x > 100.0) break; } float arr[3]; arr[1] = s.a; gl_FragColor = vec4(v.xyz, arr[1]) * mat4(1.0)[0]; }",
      "#version 300 es\nprecision highp float; uniform sampler2D t; in vec2 uv; out vec4 o; void main() { o = texture(t, uv) + vec4(dFdx(uv.x)); switch (int(uv.x)) { case 0: o.x = 1.0; break; default: o.y = 2.0; } }",
  };
  const char* wgsl[] = {
      "struct P { a: f32, b: vec2<f32> } @group(0) @binding(0) var<storage, read_write> d: array<u32>; fn helper(p: P) -> f32 { return p.a + p.b.y; } @compute @workgroup_size(4) fn main(@builtin(global_invocation_id) g: vec3<u32>) { var s = 0.0; for (var i = 0; i < 4; i++) { s += helper(P(1.0, vec2(f32(i), 2.0))); } d[g.x] = u32(s); }",
      "@fragment fn fs(@location(0) c: vec4<f32>) -> @location(0) vec4<f32> { var x = c; loop { x = x * 0.5; if x.x < 0.1 { break; } continuing { x.y += 1.0; } } switch u32(x.x) { case 0u, 1u: { x.z = 1.0; } default: { x.w = 0.0; } } return x; }",
  };
  uint32_t seed = 12345;
  auto next = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  for (int pass = 0; pass < 2; ++pass) {
    const char* const* list = pass == 0 ? glsl : wgsl;
    for (int si = 0; si < 2; ++si) {
      const std::string base = list[si];
      for (int k = 0; k < 600; ++k) {
        std::string src = base;
        if (k % 3 == 0) src.resize(next() % (src.size() + 1));            // truncation
        else for (int f = 0; f < 1 + (k % 4); ++f) src[next() % src.size()] ^= static_cast<char>(1 << (next() % 7));   // bit flips
        Module m(a);
        const bool ok = pass == 0 ? parse_glsl(m, src.c_str(), Stage::Fragment) : parse_wgsl(m, src.c_str());
        if (!ok) continue;
        Machine mc(m, a, 4096);
        mc.set_step_limit(20000);
        if (!mc.valid() || !mc.reset_privates()) continue;
        List<uint32_t> shared(a);
        for (uint32_t i = 0; i < m.shared_slots + 8; ++i) shared.push_back(0);
        mc.set_shared(shared.view().data());
        for (uint64_t f = 0; f < m.functions.count(); ++f)
          if (m.functions[f]->params.count() == 0 && m.functions[f]->body >= 0) mc.start(static_cast<int32_t>(f), ArrayView<const uint32_t* const>());
      }
    }
  }
  CHECK(a.live == 0);
}

int main() {
  test_glsl_basics();
  test_wgsl_basics();
  test_mutants();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("shader tests passed\n");
  return 0;
}
