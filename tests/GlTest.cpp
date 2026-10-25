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
#include "WebGl/GlCompressed.hpp"
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

static GlContext::Attributes noaa() {
  GlContext::Attributes a;
  a.antialias = false;
  return a;
}

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
    GlContext gl(a, false, 64, 64, noaa());
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
    GlContext gl(a, false, 32, 32, noaa());
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
    GlContext::Attributes at = noaa();
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
    GlContext gl(a, true, 16, 16, noaa());
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

static void test_transform_feedback() {
  TestAllocator a;
  {
    GlContext gl(a, true, 8, 8, noaa());
    const uint32_t v = gl.create_shader(0x8B31), f = gl.create_shader(0x8B30), p = gl.create_program();
    gl.shader_source(v, "#version 300 es\nlayout(location=0) in vec2 pos; out vec2 doubled; out float idx; void main() { doubled = pos * 2.0; idx = float(gl_VertexID); gl_Position = vec4(pos, 0.0, 1.0); }");
    gl.compile_shader(v);
    gl.shader_source(f, "#version 300 es\nprecision highp float; out vec4 o; void main() { o = vec4(1.0); }");
    gl.compile_shader(f);
    gl.attach_shader(p, v);
    gl.attach_shader(p, f);
    const StringView names[] = {"doubled", "idx"};
    gl.transform_feedback_varyings(p, ArrayView<const StringView>(names, 2), 0x8C8C);
    gl.link_program(p);
    gl.use_program(p);
    const float verts[] = {1, 2, 3, 4, 5, 6};
    const uint32_t vb = gl.create_buffer(), tb = gl.create_buffer();
    gl.bind_buffer(0x8892, vb);
    gl.buffer_data(0x8892, ConstByteView(reinterpret_cast<const uint8_t*>(verts), sizeof(verts)), 0, 0x88E4);
    gl.enable_vertex_attrib(0, true);
    gl.vertex_attrib_pointer(0, 2, 0x1406, false, 0, 0, false);
    gl.bind_buffer(0x8C8E, tb);
    gl.buffer_data(0x8C8E, ConstByteView(), 36, 0x88E8);
    gl.bind_buffer_range(0x8C8E, 0, tb, 0, 0);
    gl.enable(0x8C89, true);
    gl.begin_transform_feedback(4);
    gl.draw_arrays(4, 0, 3, 1);
    gl.end_transform_feedback();
    float out[9];
    CHECK(gl.get_buffer_sub_data(0x8C8E, 0, ByteView(reinterpret_cast<uint8_t*>(out), 36)));
    CHECK(out[0] == 2 && out[1] == 4 && out[2] == 0 && out[3] == 6 && out[4] == 8 && out[5] == 1 && out[6] == 10 && out[7] == 12 && out[8] == 2);
  }
  CHECK(a.live == 0);
}

static void test_multisample() {
  TestAllocator a;
  {
    GlContext gl(a, true, 16, 16, noaa());
    const uint32_t rb = gl.create_renderbuffer(), fbo = gl.create_framebuffer();
    gl.bind_renderbuffer(rb);
    gl.renderbuffer_storage(0x8058, 16, 16, 4);
    double w = 0;
    gl.get_renderbuffer_parameter(0x8D42, &w);
    CHECK(w == 16);
    gl.bind_framebuffer(0x8D40, fbo);
    gl.framebuffer_renderbuffer(0x8D40, 0x8CE0, rb);
    CHECK(gl.check_framebuffer_status(0x8D40) == 0x8CD5);
    gl.clear_color(0, 0, 0, 1);
    gl.clear(0x4000);
    const uint32_t prog = make_program(gl, "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }", "void main() { gl_FragColor = vec4(1.0); }");
    gl.use_program(prog);
    // a triangle whose edge cuts pixels in half: the diagonal from (-1,-1) to (1,1)
    const float verts[] = {-1, -1, 1, -1, 1, 1};
    const uint32_t vb = gl.create_buffer();
    gl.bind_buffer(0x8892, vb);
    gl.buffer_data(0x8892, ConstByteView(reinterpret_cast<const uint8_t*>(verts), sizeof(verts)), 0, 0x88E4);
    gl.enable_vertex_attrib(0, true);
    gl.vertex_attrib_pointer(0, 2, 0x1406, false, 0, 0, false);
    gl.draw_arrays(4, 0, 3, 1);
    // reading a multisampled framebuffer directly is an error; resolving with a blit is not
    uint8_t px[4];
    gl.read_pixels(8, 4, 1, 1, 0x1908, 0x1401, ByteView(px, 4));
    CHECK(gl.get_error() == 0x502);
    const uint32_t tex = gl.create_texture(), fbo2 = gl.create_framebuffer();
    gl.bind_texture(0x0DE1, tex);
    gl.tex_image(0x0DE1, 0, 0x8058, 16, 16, 1, 0, 0x1908, 0x1401, ConstByteView(), false, false);
    gl.bind_framebuffer(0x8CA8, fbo);
    gl.bind_framebuffer(0x8CA9, fbo2);
    gl.framebuffer_texture(0x8CA9, 0x8CE0, 0x0DE1, tex, 0, 0);
    gl.blit_framebuffer(0, 0, 16, 16, 0, 0, 16, 16, 0x4000, 0x2600);
    CHECK(gl.get_error() == 0);
    gl.bind_framebuffer(0x8CA8, fbo2);
    // pixel (8,8) lies on the diagonal: half covered; (12,4) is inside the lower right half; (4,12) outside
    uint8_t edge[4], inside[4], outside[4];
    gl.read_pixels(8, 8, 1, 1, 0x1908, 0x1401, ByteView(edge, 4));
    gl.read_pixels(12, 4, 1, 1, 0x1908, 0x1401, ByteView(inside, 4));
    gl.read_pixels(4, 12, 1, 1, 0x1908, 0x1401, ByteView(outside, 4));
    CHECK(edge[0] > 60 && edge[0] < 200);
    CHECK(inside[0] == 255 && outside[0] == 0);
  }
  CHECK(a.live == 0);
}

static void test_default_antialias() {
  TestAllocator a;
  {
    GlContext::Attributes at;
    at.antialias = true;
    GlContext gl(a, false, 16, 16, at);
    CHECK(gl.antialiased());
    double samples = 0;
    uint32_t n;
    gl.get_integer(0x80A9, &samples, &n);
    CHECK(samples == 4);
    const uint32_t prog = make_program(gl, "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }", "void main() { gl_FragColor = vec4(1.0); }");
    gl.use_program(prog);
    const float verts[] = {-1, -1, 1, -1, 1, 0.3f};
    const uint32_t vb = gl.create_buffer();
    gl.bind_buffer(0x8892, vb);
    gl.buffer_data(0x8892, ConstByteView(reinterpret_cast<const uint8_t*>(verts), sizeof(verts)), 0, 0x88E4);
    gl.enable_vertex_attrib(0, true);
    gl.vertex_attrib_pointer(0, 2, 0x1406, false, 0, 0, false);
    gl.clear_color(0, 0, 0, 1);
    gl.clear(0x4000);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(gl.get_error() == 0);
    bool partial = false;
    for (int y = 0; y < 16; ++y) { const uint32_t r = (pixel(gl, 8, y) >> 16) & 255; partial = partial || (r > 20 && r < 235); }
    CHECK(partial);
    CHECK(pixel(gl, 12, 1) == 0xFFFFFFFFu && pixel(gl, 4, 14) == 0xFF000000u);
    List<uint32_t> out(a);
    for (int i = 0; i < 256; ++i) out.push_back(0);
    gpu::TouchBox box;
    CHECK(gl.present(out.view().data(), 16, &box, true));
    bool shown_partial = false;
    for (int y = 0; y < 16; ++y) { const uint32_t r = (out[y * 16 + 8] >> 16) & 255; shown_partial = shown_partial || (r > 20 && r < 235); }
    CHECK(shown_partial);
    GlContext::Attributes off;
    off.antialias = false;
    GlContext plain(a, false, 16, 16, off);
    CHECK(!plain.antialiased());
  }
  CHECK(a.live == 0);
}

static void test_cube_seams() {
  TestAllocator a;
  {
    GlContext gl(a, false, 8, 8, noaa());
    const uint32_t tex = gl.create_texture();
    gl.bind_texture(0x8513, tex);
    uint8_t white[16], black[16];
    for (int i = 0; i < 16; ++i) { white[i] = 255; black[i] = i % 4 == 3 ? 255 : 0; }
    for (uint32_t f = 0; f < 6; ++f) gl.tex_image(0x8515 + f, 0, 0x1908, 2, 2, 1, 0, 0x1908, 0x1401, ConstByteView(f == 0 ? white : black, 16), true, false);
    gl.tex_parameter(0x8513, 0x2801, 0x2601);
    gl.tex_parameter(0x8513, 0x2800, 0x2601);
    const uint32_t prog = make_program(gl, "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }",
                                       "precision mediump float; uniform samplerCube t; uniform vec3 d; void main() { gl_FragColor = textureCube(t, d); }");
    gl.use_program(prog);
    const float verts[] = {-1, -1, 3, -1, -1, 3};
    const uint32_t vb = gl.create_buffer();
    gl.bind_buffer(0x8892, vb);
    gl.buffer_data(0x8892, ConstByteView(reinterpret_cast<const uint8_t*>(verts), sizeof(verts)), 0, 0x88E4);
    gl.enable_vertex_attrib(0, true);
    gl.vertex_attrib_pointer(0, 2, 0x1406, false, 0, 0, false);
    const uint32_t zero = 0;
    gl.uniform(gl.get_uniform_location(prog, "t"), 'i', 1, 1, 1, &zero);
    auto draw_dir = [&](float x, float y, float z) {
      const float v[3] = {x, y, z};
      uint32_t bits[3];
      memcpy(bits, v, 12);
      gl.uniform(gl.get_uniform_location(prog, "d"), 'f', 1, 3, 1, bits);
      gl.draw_arrays(4, 0, 3, 1);
      return pixel(gl, 4, 4) & 255;
    };
    CHECK(draw_dir(1, 0, 0) == 255);              // the middle of the white face
    const uint32_t edge = draw_dir(1, 0, 0.99f);  // next to the black +Z face: the filter reaches over the seam
    CHECK(edge > 60 && edge < 200);
    CHECK(draw_dir(0, 0, 1) == 0);                // the black face itself
  }
  CHECK(a.live == 0);
}

static void test_wide_lines() {
  TestAllocator a;
  {
    GlContext gl(a, false, 16, 16, noaa());
    const uint32_t prog = make_program(gl, "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }", "void main() { gl_FragColor = vec4(1.0); }");
    gl.use_program(prog);
    const float verts[] = {-1, 0, 1, 0};   // across the middle
    const uint32_t vb = gl.create_buffer();
    gl.bind_buffer(0x8892, vb);
    gl.buffer_data(0x8892, ConstByteView(reinterpret_cast<const uint8_t*>(verts), sizeof(verts)), 0, 0x88E4);
    gl.enable_vertex_attrib(0, true);
    gl.vertex_attrib_pointer(0, 2, 0x1406, false, 0, 0, false);
    auto rows_lit = [&]() {
      int n = 0;
      for (int y = 0; y < 16; ++y) n += (pixel(gl, 8, y) & 255) == 255;
      return n;
    };
    gl.clear_color(0, 0, 0, 0);
    gl.clear(0x4000);
    gl.draw_arrays(1, 0, 2, 1);
    CHECK(rows_lit() == 1);
    gl.clear(0x4000);
    gl.line_width(5);
    gl.draw_arrays(1, 0, 2, 1);
    const int n = rows_lit();
    CHECK(n >= 4 && n <= 6);
    gl.line_width(0);
    CHECK(gl.get_error() == 0x501);
    double range[2];
    uint32_t cnt;
    gl.get_integer(0x846E, range, &cnt);
    CHECK(range[1] >= 5);
  }
  CHECK(a.live == 0);
}

static void test_compressed() {
  TestAllocator a;
  {
    GlContext gl(a, true, 8, 8, noaa());
    const uint32_t tex = gl.create_texture(), fbo = gl.create_framebuffer();
    gl.bind_texture(0x0DE1, tex);
    gl.bind_framebuffer(0x8D40, fbo);
    gl.framebuffer_texture(0x8D40, 0x8CE0, 0x0DE1, tex, 0, 0);
    auto texel = [&](int x, int y, uint8_t out[4]) { gl.read_pixels(x, y, 1, 1, 0x1908, 0x1401, ByteView(out, 4)); };
    uint8_t px[4];
    // BC1: red and blue end points, one of each index in the first row
    const uint8_t bc1[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00};
    gl.compressed_tex_image(0x0DE1, 0, 0x83F0, 4, 4, 1, 0, ConstByteView(bc1, 8), false);
    CHECK(gl.get_error() == 0);
    texel(0, 0, px); CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255);
    texel(1, 0, px); CHECK(px[0] == 0 && px[2] == 255);
    texel(2, 0, px); CHECK(px[0] == 170 && px[2] == 85);
    texel(3, 0, px); CHECK(px[0] == 85 && px[2] == 170);
    // BC4: 200 and 100 end points; index 2 is (6 * 200 + 100) / 7
    const uint8_t bc4[8] = {200, 100, 0x88, 0, 0, 0, 0, 0};
    gl.compressed_tex_image(0x0DE1, 0, 0x8DBB, 4, 4, 1, 0, ConstByteView(bc4, 8), false);
    texel(0, 0, px); CHECK(px[0] == 200);
    texel(1, 0, px); CHECK(px[0] == 100);
    texel(2, 0, px); CHECK(px[0] == 185);
    // BC3: colour from a BC1 block, alpha from a BC4 block (255 and 0, all index 0 -> 255; pixel 1 index 1 -> 0)
    const uint8_t bc3[16] = {255, 0, 0x08, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    gl.compressed_tex_image(0x0DE1, 0, 0x83F3, 4, 4, 1, 0, ConstByteView(bc3, 16), false);
    texel(0, 0, px); CHECK(px[0] == 255 && px[3] == 255);
    texel(1, 0, px); CHECK(px[3] == 0);
    // ETC1, individual mode: left half 136 + 2, right half 0 + 2; then with the flip bit: top and bottom
    uint8_t etc[8] = {0x80, 0x80, 0x80, 0x00, 0, 0, 0, 0};
    gl.compressed_tex_image(0x0DE1, 0, 0x8D64, 4, 4, 1, 0, ConstByteView(etc, 8), false);
    texel(0, 3, px); CHECK(px[0] == 138);
    texel(3, 3, px); CHECK(px[0] == 2);
    etc[3] = 0x01;
    gl.compressed_tex_image(0x0DE1, 0, 0x8D64, 4, 4, 1, 0, ConstByteView(etc, 8), false);
    texel(3, 0, px); CHECK(px[0] == 138);
    texel(3, 3, px); CHECK(px[0] == 2);
    // sizes and mixing with uncompressed calls
    gl.compressed_tex_image(0x0DE1, 0, 0x83F0, 4, 4, 1, 0, ConstByteView(bc1, 4), false);
    CHECK(gl.get_error() == 0x501);
    gl.compressed_tex_image(0x0DE1, 0, 0x83F0, 4, 4, 1, 0, ConstByteView(bc1, 8), false);
    const uint8_t rgba[4] = {1, 2, 3, 4};
    gl.tex_sub_image(0x0DE1, 0, 0, 0, 0, 1, 1, 1, 0x1908, 0x1401, ConstByteView(rgba, 4), false);
    CHECK(gl.get_error() == 0x502);
    gl.compressed_tex_sub_image(0x0DE1, 0, 1, 0, 0, 4, 4, 1, 0x83F0, ConstByteView(bc1, 8), false);
    CHECK(gl.get_error() == 0x501);   // not on a block boundary
    gl.compressed_tex_image(0x0DE1, 0, 0x9999, 4, 4, 1, 0, ConstByteView(bc1, 8), false);
    CHECK(gl.get_error() == 0x500);
    // 6x6 in 2x2 blocks: the part past the edge is dropped
    uint8_t blocks[32];
    for (int i = 0; i < 4; ++i) memcpy(blocks + i * 8, bc1, 8);
    gl.compressed_tex_image(0x0DE1, 0, 0x83F0, 6, 6, 1, 0, ConstByteView(blocks, 32), false);
    CHECK(gl.get_error() == 0);
    texel(5, 5, px); CHECK(px[0] == 255);
  }
  CHECK(a.live == 0);
}

#include "GlAstcHdrVectors.inc"
#include "GlCompressedVectors.inc"

// BC7, BC6H and ASTC blocks against the output of a reference decoder, then truncated and bit-flipped data.
static void test_bptc_astc() {
  TestAllocator a;
  {
    size_t o = 0;
    int checked = 0;
    while (o < sizeof(kRefVectors)) {
      const uint32_t fmt = kRefVectors[o] | (kRefVectors[o + 1] << 8) | (kRefVectors[o + 2] << 16) | (uint32_t{kRefVectors[o + 3]} << 24);
      const CompressedFormat* cf = find_compressed(fmt);
      CHECK(cf != nullptr);
      if (!cf) break;
      const uint8_t* block = kRefVectors + o + 4;
      const uint8_t* want = block + 16;
      const uint32_t n = uint32_t{cf->bw} * cf->bh;
      o += 4 + 16 + n * 4;
      Image img(a);
      CHECK(img.allocate(find_format(cf->decoded), cf->bw, cf->bh, 1));
      CHECK(decode_compressed(*cf, ConstByteView(block, 16), cf->bw, cf->bh, 1, &img, 0, 0, 0));
      bool same = true;
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t r[4];
        read_texel(img, i % cf->bw, i / cf->bw, 0, r);
        for (int k = 0; k < (cf->codec == Codec::Bc6U || cf->codec == Codec::Bc6S ? 3 : 4); ++k) {
          int got;
          if (cf->codec == Codec::Bc6U || cf->codec == Codec::Bc6S || (cf->codec == Codec::Astc && !cf->srgb)) {
            float f;
            memcpy(&f, &r[k], 4);
            got = static_cast<int>(f < 0 ? 0 : f > 1 ? 255 : f * 255 + 0.5f);
          } else if (cf->codec == Codec::EacR || cf->codec == Codec::EacRs || cf->codec == Codec::EacRg || cf->codec == Codec::EacRgs) {
            // the reference keeps 8 of the 11 bits (the signed form shifted to 0..255)
            float f;
            memcpy(&f, &r[k == 1 ? 1 : 0], 4);
            const bool sg = cf->codec == Codec::EacRs || cf->codec == Codec::EacRgs;
            const bool has = k == 0 || (k == 1 && (cf->codec == Codec::EacRg || cf->codec == Codec::EacRgs));
            const int v = static_cast<int>(f * (sg ? 1023.0f : 2047.0f) + (f < 0 ? -0.5f : 0.5f));
            got = has ? ((sg ? v + 1023 : v) >> 3) : (k == 3 ? 255 : 0);
          } else {
            got = static_cast<int>(r[k]);
          }
          if (cf->codec == Codec::Etc2A1 && k < 3 && r[3] == 0 && want[i * 4 + 3] == 0) continue;   // the colour of a transparent texel is not defined
          if (got - want[i * 4 + k] > 1 || want[i * 4 + k] - got > 1) same = false;
        }
      }
      if (!same) printf("  reference mismatch: format %x\n", fmt);
      CHECK(same);
      ++checked;
    }
    CHECK(checked == 248);
    // truncated data is refused, never read past the end
    const CompressedFormat* astc = find_compressed(0x93B7);
    const CompressedFormat* bc7 = find_compressed(0x8E8C);
    CHECK(astc && bc7);
    uint8_t blk[16] = {0};
    Image img(a);
    CHECK(img.allocate(find_format(astc->decoded), 16, 8, 1));
    CHECK(!decode_compressed(*astc, ConstByteView(blk, 16), 16, 8, 1, &img, 0, 0, 0));   // two blocks needed
    CHECK(compressed_size(*astc, 16, 8, 1) == 32);
    CHECK(compressed_size(*astc, 9, 9, 1) == 64);
    // every single-bit flip of a valid block decodes (or is rejected) without a sanitizer report
    uint32_t seed = 99;
    for (int round = 0; round < 40; ++round) {
      for (int i = 0; i < 16; ++i) { seed = seed * 1664525u + 1013904223u; blk[i] = static_cast<uint8_t>(seed >> 24); }
      const CompressedFormat* const flip_formats[10] = {astc, bc7, find_compressed(0x8E8F), find_compressed(0x93BD), find_compressed(0x93D4), find_compressed(0x9274), find_compressed(0x9276), find_compressed(0x9279), find_compressed(0x9271), find_compressed(0x9272)};
      for (const CompressedFormat* cf : flip_formats) {
        for (int bit = 0; bit < 128; ++bit) {
          uint8_t flipped[16];
          memcpy(flipped, blk, 16);
          flipped[bit >> 3] = static_cast<uint8_t>(flipped[bit >> 3] ^ (1 << (bit & 7)));
          Image t(a);
          CHECK(t.allocate(find_format(cf->decoded), cf->bw, cf->bh, 1));
          (void)decode_compressed(*cf, ConstByteView(flipped, 16), cf->bw, cf->bh, 1, &t, 0, 0, 0);
        }
      }
    }
  }
  {
    // through the context: an ASTC 6x6 void-extent block (one colour), sub-image alignment by block size
    GlContext gl(a, true, 16, 16, noaa());
    const uint32_t tex = gl.create_texture(), fbo = gl.create_framebuffer();
    gl.bind_texture(0x0DE1, tex);
    gl.bind_framebuffer(0x8D40, fbo);
    gl.framebuffer_texture(0x8D40, 0x8CE0, 0x0DE1, tex, 0, 0);
    uint8_t block[16] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff};
    for (int i = 8; i < 16; ++i) block[i] = 0xff;   // the colour: four 16-bit values
    gl.compressed_tex_image(0x0DE1, 0, 0x93B4, 6, 6, 1, 0, ConstByteView(block, 16), false);
    CHECK(gl.get_error() == 0);
    uint8_t px[4];
    gl.read_pixels(5, 5, 1, 1, 0x1908, 0x1401, ByteView(px, 4));
    CHECK(px[0] == 0xff && px[1] == 0xff && px[2] == 0xff && px[3] == 0xff);
    // 3D textures take the RGTC and BPTC formats (two layers of one block), not S3TC or ASTC
    {
      const uint32_t t3 = gl.create_texture();
      gl.bind_texture(0x806F, t3);
      uint8_t two[32] = {};
      gl.compressed_tex_image(0x806F, 0, 0x8E8C, 4, 4, 2, 0, ConstByteView(two, 32), true);
      CHECK(gl.get_error() == 0);
      gl.compressed_tex_image(0x806F, 0, 0x8DBB, 4, 4, 2, 0, ConstByteView(two, 16), true);
      CHECK(gl.get_error() == 0);
      gl.compressed_tex_sub_image(0x806F, 0, 0, 0, 1, 4, 4, 1, 0x8DBB, ConstByteView(two, 8), true);
      CHECK(gl.get_error() == 0);
      gl.compressed_tex_image(0x806F, 0, 0x83F0, 4, 4, 2, 0, ConstByteView(two, 16), true);
      CHECK(gl.get_error() == 0x502);
      gl.compressed_tex_image(0x806F, 0, 0x93B0, 4, 4, 2, 0, ConstByteView(two, 32), true);
      CHECK(gl.get_error() == 0x502);
      gl.bind_texture(0x0DE1, tex);
    }
    gl.compressed_tex_sub_image(0x0DE1, 0, 3, 0, 0, 6, 6, 1, 0x93B4, ConstByteView(block, 16), false);
    CHECK(gl.get_error() == 0x501);   // not on a 6 x 6 boundary
    gl.compressed_tex_image(0x0DE1, 0, 0x93B4, 6, 6, 1, 0, ConstByteView(block, 15), false);
    CHECK(gl.get_error() == 0x501);
  }
  CHECK(a.live == 0);
}

// ASTC blocks of the linear formats against astcenc's HDR profile: exact float results (HDR endpoint modes, constant half-float blocks, LDR blocks decoded as 16 bit).
static void test_astc_hdr() {
  TestAllocator a;
  {
    size_t o = 0;
    int checked = 0, hdr_texels = 0;
    while (o < sizeof(kAstcHdrVectors)) {
      const uint32_t fmt = kAstcHdrVectors[o] | (kAstcHdrVectors[o + 1] << 8) | (kAstcHdrVectors[o + 2] << 16) | (uint32_t{kAstcHdrVectors[o + 3]} << 24);
      const CompressedFormat* cf = find_compressed(fmt);
      CHECK(cf != nullptr);
      if (!cf) break;
      const uint8_t* block = kAstcHdrVectors + o + 4;
      const uint32_t n = uint32_t{cf->bw} * cf->bh;
      const uint8_t* want = block + 16;
      o += 4 + 16 + n * 16;
      Image img(a);
      CHECK(img.allocate(find_format(cf->decoded), cf->bw, cf->bh, 1));
      CHECK(decode_compressed(*cf, ConstByteView(block, 16), cf->bw, cf->bh, 1, &img, 0, 0, 0));
      bool same = true;
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t r[4];
        read_texel(img, i % cf->bw, i / cf->bw, 0, r);
        for (int k = 0; k < 4; ++k) {
          uint32_t w;
          memcpy(&w, want + (i * 4 + k) * 4, 4);
          float wf, gf;
          memcpy(&wf, &w, 4);
          memcpy(&gf, &r[k], 4);
          if (wf > 1.0001f) ++hdr_texels;
          if (wf != wf ? gf == gf : (w != r[k])) same = false;
        }
      }
      if (!same) printf("  hdr mismatch: format %x\n", fmt);
      CHECK(same);
      ++checked;
    }
    CHECK(checked == 29 && hdr_texels > 100);
    // an HDR constant block whose colours are half floats, and a bad void extent / too few bits for the endpoints
    const CompressedFormat* f = find_compressed(0x93B0);
    Image img(a);
    CHECK(img.allocate(find_format(f->decoded), 4, 4, 1));
    uint8_t blk[16] = {0xfc, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x3c, 0x00, 0x40, 0x00, 0x44, 0x00, 0x38};   // 1, 2, 4, 0.5 as halves
    CHECK(decode_compressed(*f, ConstByteView(blk, 16), 4, 4, 1, &img, 0, 0, 0));
    uint32_t px[4];
    read_texel(img, 3, 3, 0, px);
    float c[4];
    memcpy(c, px, 16);
    CHECK(c[0] == 1.0f && c[1] == 2.0f && c[2] == 4.0f && c[3] == 0.5f);
    blk[1] = 0xf3;   // reserved bits of a void extent: the block is an error (magenta)
    CHECK(decode_compressed(*f, ConstByteView(blk, 16), 4, 4, 1, &img, 0, 0, 0));
    read_texel(img, 0, 0, 0, px);
    memcpy(c, px, 16);
    CHECK(c[0] == 1.0f && c[1] == 0.0f && c[2] == 1.0f && c[3] == 1.0f);
    // the sRGB formats stay LDR: an HDR block is an error
    const CompressedFormat* s = find_compressed(0x93D0);
    CHECK(s && s->decoded == 0x8C43);
  }
  CHECK(a.live == 0);
}

int main() {
  test_astc_hdr();
  test_bptc_astc();
  test_compressed();
  test_wide_lines();
  test_cube_seams();
  test_default_antialias();
  test_multisample();
  test_transform_feedback();
  test_triangle();
  test_interpolation_and_blend();
  test_depth_and_textures();
  test_framebuffer_object();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("gl tests passed\n");
  return 0;
}
