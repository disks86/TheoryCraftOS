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

// Host-side tests for the software GL context: drawing, state, textures, framebuffers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "WebGl/GlContext.hpp"

using namespace webgl;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static uint32_t make_program(GlContext& gl, const char* vs, const char* fs) {
  const uint32_t v = gl.create_shader(0x8B31), f = gl.create_shader(0x8B30), p = gl.create_program();
  gl.shader_source(v, vs);
  gl.compile_shader(v);
  gl.shader_source(f, fs);
  gl.compile_shader(f);
  double ok = 0;
  gl.shader_status(v, 0x8B81, &ok);
  if (!ok) printf("vertex shader: %.*s\n", static_cast<int>(gl.shader_log(v).size()), gl.shader_log(v).data());
  gl.shader_status(f, 0x8B81, &ok);
  if (!ok) printf("fragment shader: %.*s\n", static_cast<int>(gl.shader_log(f).size()), gl.shader_log(f).data());
  gl.attach_shader(p, v);
  gl.attach_shader(p, f);
  gl.link_program(p);
  gl.program_status(p, 0x8B82, &ok);
  if (!ok) printf("link: %.*s\n", static_cast<int>(gl.program_log(p).size()), gl.program_log(p).data());
  return p;
}

static uint32_t pixel(GlContext& gl, int x, int y) {
  uint8_t px[4] = {0, 0, 0, 0};
  gl.read_pixels(x, y, 1, 1, 0x1908, 0x1401, ByteView(px, 4));
  return (uint32_t{px[3]} << 24) | (uint32_t{px[0]} << 16) | (uint32_t{px[1]} << 8) | px[2];
}

static const char* kVs = "attribute vec2 pos; attribute vec3 col; varying vec3 vcol; void main() { vcol = col; gl_Position = vec4(pos, 0.0, 1.0); }";
static const char* kFs = "precision mediump float; varying vec3 vcol; void main() { gl_FragColor = vec4(vcol, 1.0); }";

static void test_triangle() {
  TestAllocator a;
  {
    GlContext gl(a, false, 64, 64, GlContext::Attributes());
    CHECK(gl.valid());
    gl.clear_color(0, 0, 0, 1);
    gl.clear(0x4000);
    CHECK(pixel(gl, 10, 10) == 0xFF000000u);
    const uint32_t prog = make_program(gl, kVs, kFs);
    gl.use_program(prog);
    // a triangle covering the lower left half of the screen, all red
    const float verts[] = {-1, -1, 1, 0, 0, 1, -1, 1, 0, 0, -1, 1, 1, 0, 0};
    const uint32_t buf = gl.create_buffer();
    gl.bind_buffer(0x8892, buf);
    gl.buffer_data(0x8892, ConstByteView(reinterpret_cast<const uint8_t*>(verts), sizeof(verts)), 0, 0x88E4);
    const int32_t pl = gl.get_attrib_location(prog, "pos"), cl = gl.get_attrib_location(prog, "col");
    CHECK(pl >= 0 && cl >= 0);
    gl.enable_vertex_attrib(static_cast<uint32_t>(pl), true);
    gl.vertex_attrib_pointer(static_cast<uint32_t>(pl), 2, 0x1406, false, 20, 0, false);
    gl.enable_vertex_attrib(static_cast<uint32_t>(cl), true);
    gl.vertex_attrib_pointer(static_cast<uint32_t>(cl), 3, 0x1406, false, 20, 8, false);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(gl.get_error() == 0);
    CHECK(pixel(gl, 10, 10) == 0xFFFF0000u);      // inside (lower left)
    CHECK(pixel(gl, 55, 55) == 0xFF000000u);      // outside (upper right)
    CHECK(pixel(gl, 2, 60) == 0xFFFF0000u);
    // the present() copy flips the rows: GL row 10 (near the bottom) lands near the bottom of the canvas
    List<uint32_t> out(a);
    for (int i = 0; i < 64 * 64; ++i) out.push_back(0);
    gpu::TouchBox box;
    CHECK(gl.present(out.view().data(), 64, &box, true));
    CHECK(out[53 * 64 + 10] == 0xFFFF0000u);
    CHECK(out[10 * 64 + 55] == 0xFF000000u);
  }
  CHECK(a.live == 0);
}

static void test_interpolation_and_blend() {
  TestAllocator a;
  {
    GlContext gl(a, false, 32, 32, GlContext::Attributes());
    const uint32_t prog = make_program(gl, kVs, kFs);
    gl.use_program(prog);
    // full-screen quad (two triangles), colour red at the left edge, blue at the right
    const float verts[] = {-1, -1, 1, 0, 0, 1, -1, 0, 0, 1, 1, 1, 0, 0, 1, -1, -1, 1, 0, 0, 1, 1, 0, 0, 1, -1, 1, 1, 0, 0};
    const uint32_t buf = gl.create_buffer();
    gl.bind_buffer(0x8892, buf);
    gl.buffer_data(0x8892, ConstByteView(reinterpret_cast<const uint8_t*>(verts), sizeof(verts)), 0, 0x88E4);
    const int32_t pl = gl.get_attrib_location(prog, "pos"), cl = gl.get_attrib_location(prog, "col");
    gl.enable_vertex_attrib(pl, true);
    gl.vertex_attrib_pointer(pl, 2, 0x1406, false, 20, 0, false);
    gl.enable_vertex_attrib(cl, true);
    gl.vertex_attrib_pointer(cl, 3, 0x1406, false, 20, 8, false);
    gl.clear_color(0, 0, 0, 0);
    gl.clear(0x4000);
    gl.draw_arrays(4, 0, 6, 1);
    const uint32_t mid = pixel(gl, 16, 16);
    const uint32_t r = (mid >> 16) & 255, b = mid & 255;
    CHECK(r > 100 && r < 160 && b > 100 && b < 160);
    CHECK(((pixel(gl, 0, 16) >> 16) & 255) > 230);
    CHECK((pixel(gl, 31, 16) & 255) > 230);
    // blending: draw the quad again with 50% alpha over the current picture
    const char* fs_half = "precision mediump float; varying vec3 vcol; void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 0.5); }";
    const uint32_t p2 = make_program(gl, kVs, fs_half);
    gl.use_program(p2);
    gl.enable(0x0BE2, true);
    gl.blend_func_separate(0x302, 0x303, 1, 0x303);
    gl.draw_arrays(4, 0, 6, 1);
    const uint32_t m2 = pixel(gl, 16, 16);
    CHECK(((m2 >> 8) & 255) > 120 && ((m2 >> 8) & 255) < 136);
  }
  CHECK(a.live == 0);
}

static void test_depth_and_textures() {
  TestAllocator a;
  {
    GlContext::Attributes at;
    at.depth = true;
    GlContext gl(a, false, 32, 32, at);
    const char* vs = "attribute vec3 pos; uniform vec3 off; varying vec2 uv; void main() { uv = pos.xy * 0.5 + 0.5; gl_Position = vec4(pos + off, 1.0); }";
    const char* fs = "precision mediump float; uniform sampler2D tex; varying vec2 uv; void main() { gl_FragColor = texture2D(tex, uv); }";
    const uint32_t prog = make_program(gl, vs, fs);
    gl.use_program(prog);
    // a 2x2 texture: red green / blue white (row 0 = bottom)
    const uint8_t texels[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    const uint32_t tex = gl.create_texture();
    gl.bind_texture(0x0DE1, tex);
    gl.tex_image(0x0DE1, 0, 0x1908, 2, 2, 1, 0, 0x1908, 0x1401, ConstByteView(texels, sizeof(texels)), true, false);
    gl.tex_parameter(0x0DE1, 0x2801, 0x2600);
    gl.tex_parameter(0x0DE1, 0x2800, 0x2600);
    const uint32_t texloc = gl.get_uniform_location(prog, "tex");
    const uint32_t zero = 0;
    gl.uniform(static_cast<int32_t>(texloc), 'i', 1, 1, 1, &zero);
    const float verts[] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, 1, 1, 0, -1, 1, 0};
    const uint32_t buf = gl.create_buffer();
    gl.bind_buffer(0x8892, buf);
    gl.buffer_data(0x8892, ConstByteView(reinterpret_cast<const uint8_t*>(verts), sizeof(verts)), 0, 0x88E4);
    const int32_t pl = gl.get_attrib_location(prog, "pos");
    gl.enable_vertex_attrib(pl, true);
    gl.vertex_attrib_pointer(pl, 3, 0x1406, false, 0, 0, false);
    const float off0[3] = {0, 0, 0};
    uint32_t offbits[3];
    memcpy(offbits, off0, 12);
    gl.uniform(gl.get_uniform_location(prog, "off"), 'f', 1, 3, 1, offbits);
    gl.enable(0x0B71, true);
    gl.clear_color(0, 0, 0, 1);
    gl.clear(0x4000 | 0x100);
    gl.draw_arrays(4, 0, 6, 1);
    CHECK(gl.get_error() == 0);
    CHECK(pixel(gl, 4, 4) == 0xFFFF0000u);      // lower left: red
    CHECK(pixel(gl, 28, 4) == 0xFF00FF00u);     // lower right: green
    CHECK(pixel(gl, 4, 28) == 0xFF0000FFu);     // upper left: blue
    CHECK(pixel(gl, 28, 28) == 0xFFFFFFFFu);    // upper right: white
    // a nearer quad drawn afterwards covers it; a farther one drawn afterwards does not
    const float z_near = -0.5f, z_far = 0.5f;
    uint32_t zbits[3] = {0, 0, 0};
    memcpy(&zbits[2], &z_near, 4);
    gl.uniform(gl.get_uniform_location(prog, "off"), 'f', 1, 3, 1, zbits);
    gl.draw_arrays(4, 0, 6, 1);
    memcpy(&zbits[2], &z_far, 4);
    gl.uniform(gl.get_uniform_location(prog, "off"), 'f', 1, 3, 1, zbits);
    gl.clear(0x4000);
    gl.draw_arrays(4, 0, 6, 1);
    CHECK(pixel(gl, 4, 4) == 0xFF000000u);      // behind the nearer quad's depth: rejected
  }
  CHECK(a.live == 0);
}

static void test_framebuffer_object() {
  TestAllocator a;
  {
    GlContext gl(a, true, 16, 16, GlContext::Attributes());
    const uint32_t tex = gl.create_texture();
    gl.bind_texture(0x0DE1, tex);
    gl.tex_image(0x0DE1, 0, 0x8058, 8, 8, 1, 0, 0x1908, 0x1401, ConstByteView(), false, false);
    const uint32_t fbo = gl.create_framebuffer();
    gl.bind_framebuffer(0x8D40, fbo);
    gl.framebuffer_texture(0x8D40, 0x8CE0, 0x0DE1, tex, 0, 0);
    CHECK(gl.check_framebuffer_status(0x8D40) == 0x8CD5);
    gl.clear_color(0, 0, 1, 1);
    gl.clear(0x4000);
    CHECK(pixel(gl, 3, 3) == 0xFF0000FFu);
    gl.bind_framebuffer(0x8D40, 0);
    CHECK(pixel(gl, 3, 3) == 0x00000000u);
    CHECK(gl.get_error() == 0);
    // es3 shader: flat int varying, uniform block, instancing
    const char* vs = "#version 300 es\nlayout(location=0) in vec2 pos; flat out int id; void main() { id = gl_InstanceID; gl_Position = vec4(pos * 0.2 + vec2(float(gl_InstanceID) * 0.5 - 0.5, 0.0), 0.0, 1.0); }";
    const char* fs = "#version 300 es\nprecision highp float; flat in int id; out vec4 o; void main() { o = vec4(float(id) / 2.0, 1.0, 0.0, 1.0); }";
    const uint32_t prog = make_program(gl, vs, fs);
    gl.use_program(prog);
    const float verts[] = {-1, -1, 1, -1, 0, 1};
    const uint32_t buf = gl.create_buffer();
    gl.bind_buffer(0x8892, buf);
    gl.buffer_data(0x8892, ConstByteView(reinterpret_cast<const uint8_t*>(verts), sizeof(verts)), 0, 0x88E4);
    gl.enable_vertex_attrib(0, true);
    gl.vertex_attrib_pointer(0, 2, 0x1406, false, 0, 0, false);
    gl.clear_color(0, 0, 0, 0);
    gl.clear(0x4000);
    gl.draw_arrays(4, 0, 3, 3);
    CHECK(gl.get_error() == 0);
    CHECK((pixel(gl, 8, 8) & 0xFF00FF00u) == 0xFF00FF00u || (pixel(gl, 8, 8) >> 24) == 255);
    CHECK(((pixel(gl, 4, 8) >> 16) & 255) == 0 && (pixel(gl, 4, 8) >> 24) == 255);   // instance 0: red = 0
    CHECK(((pixel(gl, 12, 8) >> 16) & 255) == 255);                                  // instance 2: red = 1
  }
  CHECK(a.live == 0);
}

int main() {
  test_triangle();
  test_interpolation_and_blend();
  test_depth_and_textures();
  test_framebuffer_object();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("gl tests passed\n");
  return 0;
}
