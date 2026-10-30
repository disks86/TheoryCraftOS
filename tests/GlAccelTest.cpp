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

// Host-side tests for the 3D hardware path of WebGL: the GLSL -> TGSI translator, and a GL context that hands its draws to an accelerator (a fake one here) and
// falls back to the CPU rasterizer whenever it cannot.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Graphics/Graphics3D/WebGlAccelerator.hpp"
#include "WebGl/Accelerator.hpp"
#include "WebGl/GlContext.hpp"
#include "WebGl/GpuDevice.hpp"
#include "WebGl/TgsiTranslator.hpp"

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
  gl.attach_shader(p, v);
  gl.attach_shader(p, f);
  gl.link_program(p);
  double ok = 0;
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

static bool contains(const String& s, const char* needle) { return strstr(s.c_str(), needle) != nullptr; }

// Translates the program `prog` of `gl`.
static bool translate(GlContext& gl, allocators::IAllocator& a, uint32_t prog, TgsiProgram* out, const char** why) {
  const ProgramObj* p = gl.program(prog);
  List<TgsiLink> links(a);
  for (const VaryingLink& l : p->links) links.push_back(TgsiLink{l.vs_slot, l.fs_slot, l.count});
  return translate_to_tgsi(a, *p->vsm, *p->fsm, links.view(), out, why);
}

static void test_translate_basic() {
  TestAllocator a;
  {
    GlContext gl(a, false, 16, 16, noaa());
    const uint32_t prog = make_program(gl, kVs, kFs);
    TgsiProgram t(a);
    const char* why = nullptr;
    CHECK(translate(gl, a, prog, &t, &why));
    if (why) printf("why: %s\n", why);
    CHECK(t.attributes.count() == 2);
    CHECK(t.varyings == 1);
    CHECK(t.const_rows == 0);
    CHECK(contains(t.vs, "VERT\n"));
    CHECK(contains(t.vs, "DCL OUT[0], POSITION"));
    CHECK(contains(t.vs, "DCL OUT[1], GENERIC[0]"));
    CHECK(contains(t.vs, "END"));
    CHECK(contains(t.fs, "FRAG\n"));
    CHECK(contains(t.fs, "DCL IN[0], GENERIC[0], PERSPECTIVE"));
    CHECK(contains(t.fs, "DCL OUT[0], COLOR"));
    // z is remapped from -w..w to 0..w
    CHECK(contains(t.vs, "OUT[0].z"));
  }
  CHECK(a.live == 0);
}

static void test_translate_matrix_and_functions() {
  TestAllocator a;
  {
    GlContext gl(a, false, 16, 16, noaa());
    const char* vs =
        "attribute vec3 pos; attribute vec3 normal; uniform mat4 mvp; uniform vec3 light; varying float lit;"
        "void main() { vec3 n = normalize(normal); float d = max(dot(n, light), 0.0); lit = mix(0.2, 1.0, d);"
        "  if (d > 0.5) { lit = lit * 2.0; } gl_Position = mvp * vec4(pos, 1.0); }";
    const char* fs = "precision mediump float; varying float lit; uniform vec4 tint; void main() { gl_FragColor = tint * clamp(lit, 0.0, 1.0); }";
    const uint32_t prog = make_program(gl, vs, fs);
    TgsiProgram t(a);
    const char* why = nullptr;
    CHECK(translate(gl, a, prog, &t, &why));
    if (why) printf("why: %s\n", why);
    CHECK(t.const_rows == 6);                       // mat4 (4 rows) + vec3 + vec4
    CHECK(t.constants.count() == 6);
    CHECK(contains(t.vs, "MAD"));                   // the matrix product
    CHECK(contains(t.vs, "RSQ"));                   // normalize
    CHECK(contains(t.vs, "DP3"));
    CHECK(contains(t.vs, "LRP"));                   // mix
    CHECK(contains(t.vs, "IF "));
    CHECK(contains(t.vs, "ENDIF"));
    CHECK(contains(t.vs, "DCL CONST[0..5]"));
    CHECK(contains(t.fs, "MAX") && contains(t.fs, "MIN"));
  }
  CHECK(a.live == 0);
}

static const char* kTexFs = "precision mediump float; varying vec3 vcol; uniform sampler2D tex; void main() { gl_FragColor = texture2D(tex, vcol.xy); }";

static void test_translate_texture() {
  TestAllocator a;
  {
    GlContext gl(a, false, 16, 16, noaa());
    const uint32_t prog = make_program(gl, kVs, kTexFs);
    TgsiProgram t(a);
    const char* why = nullptr;
    CHECK(translate(gl, a, prog, &t, &why));
    if (why) printf("why: %s\n", why);
    CHECK(t.samplers.count() == 1 && t.const_rows == 0);
    CHECK(contains(t.fs, "DCL SAMP[0]\nDCL SVIEW[0], 2D, FLOAT\n"));
    CHECK(contains(t.fs, "TEX TEMP["));
    CHECK(contains(t.fs, "SAMP[0], 2D"));
    CHECK(!contains(t.vs, "SAMP"));
    // two samplers, and one of them used twice
    const uint32_t two = make_program(gl, kVs, "precision mediump float; varying vec3 vcol; uniform sampler2D a; uniform sampler2D b; void main() { gl_FragColor = texture2D(a, vcol.xy) * texture2D(b, vcol.yz) + texture2D(a, vcol.zx); }");
    CHECK(translate(gl, a, two, &t, &why));
    CHECK(t.samplers.count() == 2);
    CHECK(contains(t.fs, "SAMP[1], 2D") && contains(t.fs, "DCL SAMP[1]"));
  }
  CHECK(a.live == 0);
}

static void test_translate_refusals() {
  TestAllocator a;
  {
    GlContext gl(a, false, 16, 16, noaa());
    const char* why = nullptr;
    TgsiProgram t(a);
    // a loop
    uint32_t prog = make_program(gl, "attribute vec2 p; void main() { float s = 0.0; for (int i = 0; i < 4; i++) { s += 1.0; } gl_Position = vec4(p, s, 1.0); }", kFs);
    // (kFs reads a varying that this vertex shader does not write)
    CHECK(!translate(gl, a, prog, &t, &why));
    CHECK(why != nullptr);
    // a texture lookup with an explicit level, or of a cube map
    prog = make_program(gl, kVs, "precision mediump float; varying vec3 vcol; uniform sampler2D tex; void main() { gl_FragColor = texture2DLod(tex, vcol.xy, 1.0); }");
    CHECK(!translate(gl, a, prog, &t, &why));
    prog = make_program(gl, kVs, "precision mediump float; varying vec3 vcol; uniform samplerCube tex; void main() { gl_FragColor = textureCube(tex, vcol); }");
    CHECK(!translate(gl, a, prog, &t, &why));
    // a third sampler
    prog = make_program(gl, kVs, "precision mediump float; varying vec3 vcol; uniform sampler2D a; uniform sampler2D b; uniform sampler2D c; void main() { gl_FragColor = texture2D(a, vcol.xy) + texture2D(b, vcol.xy) + texture2D(c, vcol.xy); }");
    CHECK(!translate(gl, a, prog, &t, &why));
    // a lookup in the vertex shader
    prog = make_program(gl, "attribute vec2 pos; attribute vec3 col; uniform sampler2D tex; varying vec3 vcol; void main() { vcol = col; gl_Position = texture2D(tex, pos); }", kFs);
    CHECK(!translate(gl, a, prog, &t, &why));
    // derivatives
    prog = make_program(gl, kVs, "#extension GL_OES_standard_derivatives : enable\nprecision mediump float; varying vec3 vcol; void main() { gl_FragColor = vec4(fwidth(vcol.x)); }");
    CHECK(!translate(gl, a, prog, &t, &why));
    // too many uniforms for the 128 bytes of constants
    prog = make_program(gl, kVs, "precision mediump float; varying vec3 vcol; uniform mat4 a; uniform mat4 b; uniform mat4 c; void main() { gl_FragColor = a * b * c * vec4(vcol, 1.0); }");
    CHECK(!translate(gl, a, prog, &t, &why));
  }
  CHECK(a.live == 0);
}

// An accelerator that records what it was asked and "renders" a recognisable picture: every pixel is 0x80 0x40 0x20 0xFF after a draw.
struct FakeAccel final : IAccelerator {
  const char* name() const override { return "fake"; }
  uint32_t create_program(const char* vs, const char* fs, uint32_t attributes, uint32_t varyings, uint32_t textures) override {
    ++programs;
    last_program_textures = textures;
    strncpy(vs_text, vs, sizeof vs_text - 1);
    strncpy(fs_text, fs, sizeof fs_text - 1);
    last_attributes = attributes;
    last_varyings = varyings;
    return refuse ? 0 : programs;
  }
  void destroy_program(uint32_t) override { ++programs_destroyed; }
  uint32_t create_target(uint32_t w, uint32_t h) override {
    ++targets;
    width = w;
    height = h;
    return 1;
  }
  void destroy_target(uint32_t) override { ++targets_destroyed; }
  bool clear(uint32_t, bool color, const float rgba[4], bool depth, float) override {
    ++clears;
    drawn = false;
    for (int i = 0; i < 4; ++i) clear_color[i] = rgba[i];
    return true;
  }
  bool draw(uint32_t, uint32_t, const AccelState& s, ConstByteView v, uint32_t count, ConstByteView c) override {
    ++draws;
    drawn = true;
    last_state = s;
    last_count = count;
    vertex_bytes = v.size();
    if (v.size() >= 16) memcpy(first_vertex, v.data(), 16);
    if (v.size() >= 32) memcpy(second_attr, v.data() + 16, 16);
    constant_bytes = c.size();
    if (c.size() >= 16) memcpy(first_const, c.data(), 16);
    return !fail_draw;
  }
  bool read_color(uint32_t, ByteView rgba) override {
    ++reads;
    for (uint64_t i = 0; i + 3 < rgba.size(); i += 4) {
      rgba[i] = drawn ? 0x80 : 0;
      rgba[i + 1] = drawn ? 0x40 : 0;
      rgba[i + 2] = drawn ? 0x20 : 0;
      rgba[i + 3] = 0xFF;
    }
    return true;
  }
  uint32_t create_texture(uint32_t w, uint32_t h) override {
    if (no_textures) return 0;
    ++textures_made;
    tex_w = w;
    tex_h = h;
    return 10 + textures_made;
  }
  void destroy_texture(uint32_t) override { ++textures_destroyed; }
  bool write_texture(uint32_t, ConstByteView rgba) override {
    ++texture_writes;
    texel_bytes = rgba.size();
    if (rgba.size() >= 4) memcpy(first_texel, rgba.data(), 4);
    return true;
  }
  bool no_textures = false;
  int textures_made = 0, textures_destroyed = 0, texture_writes = 0;
  uint32_t last_program_textures = 0, tex_w = 0, tex_h = 0;
  uint64_t texel_bytes = 0;
  uint8_t first_texel[4] = {};
  bool refuse = false, fail_draw = false, drawn = false;
  int programs = 0, programs_destroyed = 0, targets = 0, targets_destroyed = 0, clears = 0, draws = 0, reads = 0;
  uint32_t last_attributes = 0, last_varyings = 0, last_count = 0, width = 0, height = 0;
  uint64_t vertex_bytes = 0, constant_bytes = 0;
  char vs_text[4096] = {}, fs_text[4096] = {};
  float clear_color[4] = {};
  float first_vertex[4] = {};
  float second_attr[4] = {};
  float first_const[4] = {};
  AccelState last_state;
};

struct Scene {
  Scene(GlContext& gl) : gl(gl) {}
  GlContext& gl;
  uint32_t prog = 0;
  void setup(const char* vs, const char* fs) {
    prog = make_program(gl, vs, fs);
    gl.use_program(prog);
    const float verts[] = {-1, -1, 1, 0, 0, 1, -1, 1, 0, 0, -1, 1, 1, 0, 0};   // the lower left half, red
    const uint32_t buf = gl.create_buffer();
    gl.bind_buffer(0x8892, buf);
    gl.buffer_data(0x8892, ConstByteView(reinterpret_cast<const uint8_t*>(verts), sizeof(verts)), 0, 0x88E4);
    const int32_t pl = gl.get_attrib_location(prog, "pos"), cl = gl.get_attrib_location(prog, "col");
    gl.enable_vertex_attrib(static_cast<uint32_t>(pl), true);
    gl.vertex_attrib_pointer(static_cast<uint32_t>(pl), 2, 0x1406, false, 20, 0, false);
    gl.enable_vertex_attrib(static_cast<uint32_t>(cl), true);
    gl.vertex_attrib_pointer(static_cast<uint32_t>(cl), 3, 0x1406, false, 20, 8, false);
  }
};

static void test_hardware_draw_and_readback() {
  TestAllocator a;
  FakeAccel fake;
  set_accelerator(&fake);
  {
    GlContext gl(a, false, 64, 64, noaa());
    Scene sc(gl);
    sc.setup(kVs, kFs);
    gl.clear_color(0.25f, 0.5f, 0.75f, 1.0f);
    gl.clear(0x4000 | 0x100);
    CHECK(fake.targets == 1 && fake.width == 64 && fake.height == 64);
    CHECK(fake.clears == 1);
    CHECK(fake.clear_color[0] == 0.25f && fake.clear_color[3] == 1.0f);
    CHECK(gl.hardware_drawing());
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(gl.get_error() == 0);
    CHECK(fake.draws == 1);
    CHECK(fake.last_count == 3);
    CHECK(fake.last_attributes == 2 && fake.last_varyings == 1);
    CHECK(fake.vertex_bytes == 3 * 2 * 16);
    // the first attribute is the one the shader reads first: col, then pos (floats x y z w, a missing w is 1)
    CHECK(fake.first_vertex[0] == 1 && fake.first_vertex[1] == 0 && fake.first_vertex[2] == 0 && fake.first_vertex[3] == 1);
    CHECK(fake.last_state.vp_w == 64 && fake.last_state.vp_h == 64 && !fake.last_state.depth_test);
    CHECK(fake.last_state.cull == 0 && fake.last_state.front_ccw);
    CHECK(fake.reads == 0);   // nothing is read back per draw
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 2 && fake.reads == 0);
    // the picture comes back when the CPU needs it
    CHECK(pixel(gl, 10, 10) == 0xFF804020u);
    CHECK(fake.reads == 1);
    CHECK(pixel(gl, 11, 11) == 0xFF804020u);
    CHECK(fake.reads == 1);   // not again until the device draws again
    List<uint32_t> out(a);
    for (int i = 0; i < 64 * 64; ++i) out.push_back(0);
    gl.draw_arrays(4, 0, 3, 1);
    gpu::TouchBox box;
    CHECK(gl.present(out.view().data(), 64, &box, false));
    CHECK(fake.reads == 2);
    CHECK(out[5 * 64 + 5] == 0xFF804020u);
  }
  CHECK(fake.targets_destroyed == 1 && fake.programs_destroyed == 1);
  set_accelerator(nullptr);
  CHECK(a.live == 0);
}

static void test_fallbacks() {
  TestAllocator a;
  FakeAccel fake;
  set_accelerator(&fake);
  {
    GlContext gl(a, false, 64, 64, noaa());
    Scene sc(gl);
    sc.setup(kVs, kFs);
    gl.clear_color(0, 0, 0, 1);
    gl.clear(0x4000);
    // a draw before the first clear that covers the buffer is the CPU's, and so is one with state the device lacks
    gl.enable(0x0B90, true);   // STENCIL_TEST
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 0);
    CHECK(pixel(gl, 10, 10) == 0xFFFF0000u);   // the software rasterizer drew it: red
    gl.enable(0x0B90, false);
    // the device picture is out of date now: later draws stay on the CPU until a clear
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 0);
    gl.clear(0x4000);
    CHECK(fake.clears == 2);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 1);
    // a scissored clear is a CPU write
    gl.enable(0x0C11, true);
    gl.scissor(0, 0, 8, 8);
    gl.clear(0x4000);
    gl.enable(0x0C11, false);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 1);
    // points and lines are not for the device
    gl.clear(0x4000);
    gl.draw_arrays(1, 0, 2, 1);
    CHECK(fake.draws == 1);
    // an unsupported blend function
    gl.clear(0x4000);
    gl.enable(0x0BE2, true);
    gl.blend_func_separate(1, 1, 1, 1);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 1);
    gl.blend_func_separate(0x302, 0x303, 0x302, 0x303);
    gl.clear(0x4000);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 2 && fake.last_state.blend);
  }
  set_accelerator(nullptr);
  CHECK(a.live == 0);
}

static void test_refused_program_and_failed_draw() {
  TestAllocator a;
  FakeAccel fake;
  fake.refuse = true;
  set_accelerator(&fake);
  {
    GlContext gl(a, false, 64, 64, noaa());
    Scene sc(gl);
    sc.setup(kVs, kFs);
    gl.clear_color(0, 0, 0, 1);
    gl.clear(0x4000);
    gl.draw_arrays(4, 0, 3, 1);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.programs == 1);   // asked once, then remembered
    CHECK(fake.draws == 0);
    CHECK(pixel(gl, 10, 10) == 0xFFFF0000u);
  }
  set_accelerator(nullptr);
  // a device that fails a draw: the context goes on on the CPU
  FakeAccel bad;
  bad.fail_draw = true;
  set_accelerator(&bad);
  {
    GlContext gl(a, false, 64, 64, noaa());
    Scene sc(gl);
    sc.setup(kVs, kFs);
    gl.clear_color(0, 0, 0, 1);
    gl.clear(0x4000);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(bad.draws == 1);
    CHECK(!gl.hardware_drawing());
    CHECK(pixel(gl, 10, 10) == 0xFFFF0000u);
    CHECK(pixel(gl, 55, 55) == 0xFF000000u);
  }
  set_accelerator(nullptr);
  CHECK(a.live == 0);
}

static void test_uniforms_and_depth() {
  TestAllocator a;
  FakeAccel fake;
  set_accelerator(&fake);
  {
    GlContext gl(a, false, 32, 32, noaa());
    Scene sc(gl);
    sc.setup("attribute vec2 pos; attribute vec3 col; uniform vec4 tint; varying vec3 vcol; void main() { vcol = col * tint.xyz; gl_Position = vec4(pos, 0.0, 1.0); }", kFs);
    const float tint[4] = {0.5f, 0.25f, 2.0f, 1.0f};
    uint32_t bits[4];
    memcpy(bits, tint, 16);
    gl.uniform(gl.get_uniform_location(sc.prog, "tint"), 'f', 1, 4, 1, bits);
    gl.clear(0x4000 | 0x100);
    gl.enable(0x0B71, true);   // DEPTH_TEST
    gl.depth_func(0x0203);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 1);
    CHECK(fake.constant_bytes == 16);
    CHECK(fake.first_const[0] == 0.5f && fake.first_const[1] == 0.25f && fake.first_const[2] == 2.0f);
    CHECK(fake.last_state.depth_test && fake.last_state.depth_write);
    gl.depth_mask(false);
    gl.depth_func(0x0201);
    gl.cull_face(0x404);
    gl.enable(0x0B44, true);
    gl.front_face(0x0900);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 2 && !fake.last_state.depth_write && fake.last_state.cull == 1 && !fake.last_state.front_ccw);
    // GREATER is not the device's comparison
    gl.depth_func(0x0204);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 2);
  }
  set_accelerator(nullptr);
  CHECK(a.live == 0);
}

static void test_no_accelerator() {
  TestAllocator a;
  set_accelerator(nullptr);
  {
    GlContext gl(a, false, 64, 64, noaa());
    Scene sc(gl);
    sc.setup(kVs, kFs);
    gl.clear_color(0, 0, 0, 1);
    gl.clear(0x4000);
    CHECK(!gl.hardware_drawing());
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(pixel(gl, 10, 10) == 0xFFFF0000u);
  }
  CHECK(a.live == 0);
}

// An antialiased context renders at twice the size on the device and the viewport follows.
static void test_antialiased_target() {
  TestAllocator a;
  FakeAccel fake;
  set_accelerator(&fake);
  {
    GlContext gl(a, false, 16, 16, GlContext::Attributes());
    Scene sc(gl);
    sc.setup(kVs, kFs);
    gl.clear(0x4000);
    CHECK(fake.width == 32 && fake.height == 32);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 1 && fake.last_state.vp_w == 32);
    CHECK(pixel(gl, 3, 3) == 0xFF804020u);
  }
  set_accelerator(nullptr);
  CHECK(a.live == 0);
}

// --- The accelerator over a (fake) graphics device -------------------------------------------------------------------------------------------------------

struct FakeBuffer final : graphics::IBuffer {
  uint8_t data[4096] = {};
  uint64_t size() const override { return sizeof data; }
  ByteView map() override { return ByteView(data, sizeof data); }
  void unmap() override {}
};
struct FakeImage final : graphics::IImage {
  graphics::Extent2D e;
  graphics::Format f;
  graphics::Extent2D extent() const override { return e; }
  graphics::Format format() const override { return f; }
  bool read_pixels(ArrayView<uint32_t> out) override {
    for (uint32_t& v : out) v = 0x80102030u;   // A R G B
    return true;
  }
  bool write_pixels(ArrayView<const uint32_t> pixels) override {
    if (pixels.size() == 0) return false;
    first_written = pixels[0];
    return true;
  }
  uint32_t first_written = 0;
};
struct FakeModule final : graphics::IShaderModule {
  graphics::ShaderStage st;
  graphics::ShaderStage stage() const override { return st; }
};
struct FakePipeline final : graphics::IPipeline {
  graphics::GraphicsPipelineDesc desc;
};
struct FakeCommands final : graphics::ICommandBuffer {
  int draws = 0, pushes = 0, begins = 0;
  uint32_t last_vertices = 0, push_size = 0;
  graphics::RenderPassBegin pass;
  graphics::Viewport viewport;
  graphics::Rect2D scissor;
  const graphics::IPipeline* pipeline = nullptr;
  void begin() override { ++begins; }
  void begin_render_pass(const graphics::RenderPassBegin& p) override { pass = p; }
  void bind_pipeline(const graphics::IPipeline* p) override { pipeline = p; }
  void bind_vertex_buffer(graphics::IBuffer*, uint64_t) override {}
  void bind_index_buffer(graphics::IBuffer*, graphics::IndexType, uint64_t) override {}
  void push_constants(const void*, uint32_t size) override { ++pushes; push_size = size; }
  int bound = 0;
  graphics::IImage* bound_image[2] = {};
  graphics::SamplerDesc bound_sampler[2];
  void bind_texture(uint32_t unit, graphics::IImage* image, const graphics::SamplerDesc& s) override {
    ++bound;
    bound_image[unit] = image;
    bound_sampler[unit] = s;
  }
  void set_viewport(const graphics::Viewport& v) override { viewport = v; }
  void set_scissor(const graphics::Rect2D& r) override { scissor = r; }
  void draw(uint32_t n, uint32_t, uint32_t, uint32_t) override { ++draws; last_vertices = n; }
  void draw_indexed(uint32_t, uint32_t, int32_t, uint32_t, uint32_t) override {}
  void set_primitive_topology(graphics::PrimitiveTopology) override {}
  void end_render_pass() override {}
  bool end() override { return true; }
};
struct FakeDevice final : graphics::IGraphicsDevice {
  bool tgsi = true, textures = true;
  int images = 0, modules = 0, pipelines = 0, submits = 0;
  FakeCommands commands;
  graphics::DeviceInfo info() const override { return {"fake gpu", graphics::DeviceKind::VirtualGpu, true}; }
  graphics::IBuffer* create_buffer(uint64_t, uint32_t) override { return new FakeBuffer(); }
  void destroy_buffer(graphics::IBuffer* b) override { delete static_cast<FakeBuffer*>(b); }
  graphics::IImage* create_image(const graphics::ImageDesc& d) override {
    ++images;
    FakeImage* i = new FakeImage();
    i->e = d.extent;
    i->f = d.format;
    return i;
  }
  void destroy_image(graphics::IImage* i) override { --images; delete static_cast<FakeImage*>(i); }
  graphics::IShaderModule* create_shader_module(const graphics::ShaderModuleDesc& d) override {
    if (!tgsi || !d.tgsi) return nullptr;
    ++modules;
    FakeModule* m = new FakeModule();
    m->st = d.stage;
    return m;
  }
  void destroy_shader_module(graphics::IShaderModule* m) override { --modules; delete static_cast<FakeModule*>(m); }
  graphics::IPipeline* create_graphics_pipeline(const graphics::GraphicsPipelineDesc& d) override {
    if (d.texture_count && !textures) return nullptr;
    ++pipelines;
    FakePipeline* p = new FakePipeline();
    p->desc = d;
    return p;
  }
  void destroy_pipeline(graphics::IPipeline* p) override { --pipelines; delete static_cast<FakePipeline*>(p); }
  graphics::ICommandBuffer* create_command_buffer() override { return &commands; }
  void destroy_command_buffer(graphics::ICommandBuffer*) override {}
  bool submit(graphics::ICommandBuffer*) override { ++submits; return true; }
  bool present(graphics::IImage*) override { return true; }
};

static void test_device_accelerator() {
  TestAllocator a;
  FakeDevice dev;
  {
    graphics::WebGlAccelerator acc(dev, a);
    CHECK(strcmp(acc.name(), "fake gpu") == 0);
    const uint32_t prog = acc.create_program("VERT\nEND\n", "FRAG\nEND\n", 2, 1, 0);
    CHECK(prog != 0 && dev.modules == 2);
    const uint32_t target = acc.create_target(8, 4);
    CHECK(target != 0 && dev.images == 2);
    const float rgba[4] = {0, 0.5f, 1, 1};
    CHECK(acc.clear(target, true, rgba, true, 1.0f));
    CHECK(dev.submits == 1 && dev.commands.pass.clear_color && dev.commands.pass.clear_depth && dev.commands.pass.clear_color_value.y == 0.5f);
    CHECK(acc.clear(target, true, rgba, false, 1.0f));
    CHECK(dev.commands.pass.clear_color && !dev.commands.pass.clear_depth);

    AccelState st;
    st.vp_w = 8;
    st.vp_h = 4;
    st.depth_far = 1;
    st.front_ccw = true;
    st.cull = 2;
    st.depth_test = true;
    st.depth_write = true;
    uint8_t vertices[3 * 2 * 16] = {};
    uint8_t constants[32] = {};
    CHECK(acc.draw(target, prog, st, ConstByteView(vertices, sizeof vertices), 3, ConstByteView(constants, sizeof constants)));
    CHECK(dev.commands.draws == 1 && dev.commands.last_vertices == 3 && dev.commands.pushes == 1 && dev.commands.push_size == 32);
    CHECK(!dev.commands.pass.clear_color && !dev.commands.pass.clear_depth);   // a draw loads what is there
    CHECK(dev.commands.viewport.width == 8 && dev.commands.scissor.width == 8 && dev.commands.scissor.height == 4);
    const FakePipeline* pl = static_cast<const FakePipeline*>(dev.commands.pipeline);
    CHECK(pl->desc.attribute_count == 2 && pl->desc.vertex_stride == 32 && pl->desc.varying_count == 1);
    CHECK(pl->desc.cull_mode == graphics::CullMode::Back);
    CHECK(pl->desc.front_face == graphics::FrontFace::Clockwise);   // GL's counter-clockwise, with the device's rows upside down
    CHECK(pl->desc.depth_test && pl->desc.depth_write && !pl->desc.alpha_blend);
    // the same state reuses the pipeline; another one makes one more
    CHECK(acc.draw(target, prog, st, ConstByteView(vertices, sizeof vertices), 3, ConstByteView(constants, sizeof constants)));
    CHECK(dev.pipelines == 1);
    st.blend = true;
    st.scissor = true;
    st.sc_x = 1;
    st.sc_y = 2;
    st.sc_w = 3;
    st.sc_h = 1;
    CHECK(acc.draw(target, prog, st, ConstByteView(vertices, sizeof vertices), 3, ConstByteView()));
    CHECK(dev.pipelines == 2 && dev.commands.scissor.x == 1 && dev.commands.scissor.y == 2 && dev.commands.scissor.width == 3);
    // wrong sizes are refused
    CHECK(!acc.draw(target, prog, st, ConstByteView(vertices, 16), 3, ConstByteView()));
    CHECK(!acc.draw(target, prog, st, ConstByteView(vertices, sizeof vertices), 3, ConstByteView(constants, 8)));
    CHECK(!acc.draw(99, prog, st, ConstByteView(vertices, sizeof vertices), 3, ConstByteView()));

    uint8_t px[8 * 4 * 4];
    CHECK(acc.read_color(target, ByteView(px, sizeof px)));
    CHECK(px[0] == 0x10 && px[1] == 0x20 && px[2] == 0x30 && px[3] == 0x80);   // 0xAARRGGBB -> R G B A
    CHECK(!acc.read_color(target, ByteView(px, 16)));

    acc.destroy_program(prog);
    acc.destroy_target(target);
    CHECK(dev.pipelines == 0 && dev.modules == 0 && dev.images == 0);
    // a device without TGSI refuses the program
    dev.tgsi = false;
    CHECK(acc.create_program("VERT\nEND\n", "FRAG\nEND\n", 2, 1, 0) == 0 && dev.modules == 0);
    CHECK(acc.create_program("VERT\nEND\n", "FRAG\nEND\n", 9, 1, 0) == 0);
  }
  CHECK(dev.images == 0 && dev.modules == 0 && dev.pipelines == 0);
  CHECK(a.live == 0);
}

static void upload_2x2(GlContext& gl, uint32_t tex, const uint8_t (&texels)[16]) {
  gl.active_texture(0x84C0);
  gl.bind_texture(0x0DE1, tex);
  gl.tex_image(0x0DE1, 0, 0x1908, 2, 2, 1, 0, 0x1908, 0x1401, ConstByteView(texels, sizeof texels), true, false);
}

// A program that samples a texture runs on the device with the texture there, when the texture is one the device can read.
static void test_hardware_texture() {
  TestAllocator a;
  FakeAccel fake;
  set_accelerator(&fake);
  {
    GlContext gl(a, false, 64, 64, noaa());
    Scene sc(gl);
    sc.setup(kVs, kTexFs);
    const uint32_t unit0 = 0;
    gl.uniform(gl.get_uniform_location(sc.prog, "tex"), 'i', 1, 1, 1, &unit0);
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 9, 8, 7, 6};
    const uint32_t tex = gl.create_texture();
    upload_2x2(gl, tex, texels);
    gl.clear_color(0, 0, 0, 1);
    gl.clear(0x4000);
    // a texture with the default (mipmapped) minification filter is the CPU's
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 0 && fake.textures_made == 0);
    gl.clear(0x4000);
    gl.tex_parameter(0x0DE1, 0x2801, 0x2600);   // NEAREST
    gl.tex_parameter(0x0DE1, 0x2800, 0x2600);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 1 && fake.last_program_textures == 1);
    CHECK(fake.textures_made == 1 && fake.texture_writes == 1 && fake.tex_w == 2 && fake.tex_h == 2 && fake.texel_bytes == 16);
    CHECK(fake.first_texel[0] == 255 && fake.first_texel[1] == 0 && fake.first_texel[3] == 255);
    CHECK(fake.last_state.texture_count == 1 && fake.last_state.textures[0] == 11);
    CHECK(!fake.last_state.samplers[0].linear && fake.last_state.samplers[0].repeat_s && fake.last_state.samplers[0].repeat_t);
    // the same texels are not sent again
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 2 && fake.texture_writes == 1);
    // changed texels are
    const uint8_t patch[4] = {1, 2, 3, 4};
    gl.tex_sub_image(0x0DE1, 0, 0, 0, 0, 1, 1, 1, 0x1908, 0x1401, ConstByteView(patch, 4), false);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 3 && fake.texture_writes == 2 && fake.first_texel[0] == 1);
    // the sampler state follows the texture
    gl.tex_parameter(0x0DE1, 0x2801, 0x2601);   // LINEAR
    gl.tex_parameter(0x0DE1, 0x2800, 0x2601);
    gl.tex_parameter(0x0DE1, 0x2802, 0x812F);   // CLAMP_TO_EDGE in s
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 4 && fake.last_state.samplers[0].linear && !fake.last_state.samplers[0].repeat_s && fake.last_state.samplers[0].repeat_t);
    // mirrored repeat is not the device's, and neither is a pair of different filters
    gl.tex_parameter(0x0DE1, 0x2802, 0x8370);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 4);
    gl.clear(0x4000);
    gl.tex_parameter(0x0DE1, 0x2802, 0x2901);
    gl.tex_parameter(0x0DE1, 0x2800, 0x2600);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 4);
    // a device without textures leaves the program on the CPU
    const uint32_t other = gl.create_texture();
    uint8_t big[4 * 4 * 4] = {};
    gl.bind_texture(0x0DE1, other);
    gl.tex_image(0x0DE1, 0, 0x1908, 4, 4, 1, 0, 0x1908, 0x1401, ConstByteView(big, sizeof big), true, false);
    gl.tex_parameter(0x0DE1, 0x2801, 0x2601);
    gl.tex_parameter(0x0DE1, 0x2800, 0x2601);
    fake.no_textures = true;
    gl.clear(0x4000);
    gl.draw_arrays(4, 0, 3, 1);
    CHECK(fake.draws == 4);
    // deleting a texture frees its device copy
    const int destroyed = fake.textures_destroyed;
    gl.delete_object('t', tex);
    CHECK(fake.textures_destroyed == destroyed + 1);
  }
  CHECK(fake.textures_made == fake.textures_destroyed);
  set_accelerator(nullptr);
  CHECK(a.live == 0);
}

// The accelerator over a device: textures are images the device can sample, bound with their sampler at each draw.
static void test_device_textures() {
  TestAllocator a;
  FakeDevice dev;
  {
    graphics::WebGlAccelerator acc(dev, a);
    const uint32_t prog = acc.create_program("VERT\nEND\n", "FRAG\nEND\n", 2, 1, 1);
    CHECK(prog != 0 && dev.pipelines == 1);   // (the probe that checks the device can have the texture)
    const uint32_t tex = acc.create_texture(2, 1);
    CHECK(tex != 0 && dev.images == 1);
    const uint8_t rgba[8] = {0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0};
    CHECK(acc.write_texture(tex, ConstByteView(rgba, sizeof rgba)));
    CHECK(!acc.write_texture(tex, ConstByteView(rgba, 4)));
    CHECK(!acc.write_texture(99, ConstByteView(rgba, sizeof rgba)));
    CHECK(acc.create_texture(0, 4) == 0);
    const uint32_t target = acc.create_target(8, 4);
    AccelState st;
    st.vp_w = 8;
    st.vp_h = 4;
    st.depth_far = 1;
    uint8_t vertices[3 * 2 * 16] = {};
    // a draw has to say which texture
    CHECK(!acc.draw(target, prog, st, ConstByteView(vertices, sizeof vertices), 3, ConstByteView()));
    st.texture_count = 1;
    st.textures[0] = tex;
    st.samplers[0].linear = true;
    st.samplers[0].repeat_s = false;
    CHECK(acc.draw(target, prog, st, ConstByteView(vertices, sizeof vertices), 3, ConstByteView()));
    const FakePipeline* pl = static_cast<const FakePipeline*>(dev.commands.pipeline);
    CHECK(pl->desc.texture_count == 1);
    CHECK(dev.commands.bound == 1 && dev.commands.bound_image[0] != nullptr);
    CHECK(static_cast<FakeImage*>(dev.commands.bound_image[0])->first_written == 0x44112233u);   // 0xAARRGGBB
    CHECK(dev.commands.bound_sampler[0].filter == graphics::Filter::Linear && dev.commands.bound_sampler[0].address_u == graphics::AddressMode::ClampToEdge &&
          dev.commands.bound_sampler[0].address_v == graphics::AddressMode::Repeat);
    st.textures[0] = 99;
    CHECK(!acc.draw(target, prog, st, ConstByteView(vertices, sizeof vertices), 3, ConstByteView()));
    // too many samplers, and a device that cannot have textures
    CHECK(acc.create_program("VERT\nEND\n", "FRAG\nEND\n", 2, 1, 3) == 0);
    dev.textures = false;
    CHECK(acc.create_program("VERT\nEND\n", "FRAG\nEND\n", 2, 1, 1) == 0);
    acc.destroy_texture(tex);
    acc.destroy_target(target);
    acc.destroy_program(prog);
  }
  CHECK(dev.images == 0 && dev.modules == 0 && dev.pipelines == 0);
  CHECK(a.live == 0);
}

// --- WebGPU ------------------------------------------------------------------------------------------------------------------------------------------------

struct GpuScene {
  explicit GpuScene(allocators::IAllocator& a, const char* wgsl, bool uniform = false) : dev(a), a(a), layouts(a), targets(a), none(a) {
    const wgpu::FormatDesc* rgba = wgpu::find_format("rgba8unorm");
    tex = dev.create_texture(16, 16, 1, 1, 1, 1, rgba, 0x10 | 0x01);
    wgpu::TextureView tv;
    tv.texture = tex;
    tv.format = rgba;
    tv.dim = 1;
    view = dev.create_view(tv);
    String log(a);
    mod = dev.create_shader_module(wgsl, &log);
    if (!log.empty()) printf("wgsl: %s\n", log.c_str());
    const float verts[] = {-1, -1, 1, 0, 0, 3, -1, 1, 0, 0, -1, 3, 1, 0, 0};
    vb = dev.create_buffer(sizeof(verts), 0x20 | 0x08);
    dev.write_buffer(vb, 0, ConstByteView(reinterpret_cast<const uint8_t*>(verts), sizeof(verts)));
    UniquePointer<wgpu::VertexBufferLayout> layout = UniquePointer<wgpu::VertexBufferLayout>::make(a, a);
    layout->stride = 20;
    wgpu::VertexAttribute a0, a1;
    a0.shader_location = 0; a0.offset = 0; a0.format = wgpu::vertex_format("float32x2");
    a1.shader_location = 1; a1.offset = 8; a1.format = wgpu::vertex_format("float32x3");
    layout->attributes.push_back(a0);
    layout->attributes.push_back(a1);
    layouts.push_back(static_cast<UniquePointer<wgpu::VertexBufferLayout>&&>(layout));
    wgpu::ColorTarget ct;
    ct.format = rgba;
    targets.push_back(ct);
    rp = dev.create_render_pipeline(0, mod, "vs", mod, "fs", layouts, targets, nullptr, 3, 0, true, 0, false, 1, none, none);
    if (!rp) printf("pipeline: %s\n", dev.error.c_str());
    if (uniform) {
      ubuf = dev.create_buffer(80, 0x40 | 0x08);
      const uint32_t bgl = dev.pipeline_layout_bgl(rp, false, 0);
      List<wgpu::BgEntry> entries(a);
      wgpu::BgEntry e;
      e.binding = 0; e.kind = 0; e.id = ubuf;
      entries.push_back(e);
      bg = dev.create_bind_group(bgl, entries);
    }
  }
  bool pass(bool load) {
    wgpu::RenderPassDesc pd(a);
    wgpu::ColorAttachment ca;
    ca.view = view;
    ca.load = load;
    ca.clear[2] = 1;
    ca.clear[3] = 1;
    pd.colors.push_back(ca);
    return dev.begin_render_pass(pd) && dev.set_pipeline(rp) && dev.set_vertex_buffer(0, vb, 0, 0) && (!bg || dev.set_bind_group(0, bg, ArrayView<const uint32_t>()));
  }
  uint32_t texel(int x, int y) {
    const uint32_t out = dev.create_buffer(16 * 16 * 4, 0x08 | 0x02);
    wgpu::ImageCopy src;
    src.texture = tex;
    wgpu::BufferLayout bl;
    bl.bytes_per_row = 64;
    uint8_t px[16 * 16 * 4];
    dev.copy_texture_to_buffer(src, out, bl, 16, 16, 1);
    dev.read_buffer(out, 0, ByteView(px, sizeof(px)));
    const uint8_t* p = px + (y * 16 + x) * 4;
    return (uint32_t{p[3]} << 24) | (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2];
  }
  wgpu::Device dev;
  allocators::IAllocator& a;
  List<UniquePointer<wgpu::VertexBufferLayout>> layouts;
  List<wgpu::ColorTarget> targets;
  List<UniquePointer<wgpu::Device::ConstantValue>> none;
  uint32_t tex = 0, view = 0, mod = 0, vb = 0, rp = 0, ubuf = 0, bg = 0;
};

static const char* kWgsl =
    "struct VsOut { @builtin(position) pos: vec4<f32>, @location(0) color: vec3<f32>, }\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32, @location(0) p: vec2<f32>, @location(1) c: vec3<f32>) -> VsOut { var o: VsOut; o.pos = vec4<f32>(p, 0.0, 1.0); o.color = c; return o; }\n"
    "@fragment fn fs(in: VsOut) -> @location(0) vec4<f32> { return vec4<f32>(in.color, 1.0); }\n";

static void test_webgpu_hardware() {
  TestAllocator a;
  FakeAccel fake;
  set_accelerator(&fake);
  {
    GpuScene sc(a, kWgsl);
    CHECK(sc.rp != 0);
    CHECK(sc.pass(false));
    CHECK(fake.clears == 1 && fake.targets == 1 && fake.width == 16);
    CHECK(fake.clear_color[2] == 1.0f);
    CHECK(sc.dev.draw(3, 1, 0, 0));
    CHECK(fake.draws == 1 && fake.last_count == 3);
    CHECK(fake.last_attributes == 2 && fake.last_varyings == 1);
    CHECK(fake.vertex_bytes == 3 * 2 * 16);
    CHECK(fake.first_vertex[0] == -1 && fake.first_vertex[1] == -1 && fake.first_vertex[3] == 1);   // location 0: p, with w = 1
    CHECK(fake.second_attr[0] == 1 && fake.second_attr[1] == 0);                                      // location 1: c
    CHECK(strstr(fake.vs_text, "OUT[0].xzw") != nullptr && strstr(fake.vs_text, "OUT[0].y, -TEMP") != nullptr);   // y negated for the device
    CHECK(fake.reads == 0);
    CHECK(sc.dev.end_render_pass());
    CHECK(fake.reads == 1);   // once, at the end of the pass
    CHECK(sc.dev.error.empty());
    CHECK(sc.texel(8, 8) == 0xFF804020u);
    // a pass that loads what is there is the CPU's, and so is a pass after a failed draw
    CHECK(sc.pass(true));
    CHECK(sc.dev.draw(3, 1, 0, 0));
    CHECK(sc.dev.end_render_pass());
    CHECK(fake.draws == 1 && fake.clears == 1);
    CHECK(sc.texel(8, 8) == 0xFFFF0000u);   // the software rasterizer drew the red vertex colors
    fake.fail_draw = true;
    CHECK(sc.pass(false));
    CHECK(sc.dev.draw(3, 1, 0, 0));
    CHECK(fake.draws == 2);
    CHECK(sc.dev.end_render_pass());
    CHECK(sc.texel(8, 8) == 0xFFFF0000u);
  }
  set_accelerator(nullptr);
  CHECK(a.live == 0);
}

// A texture and a sampler in a bind group: the draw goes to the device with the texture there, when the pair is one the device can read.
static void test_webgpu_texture() {
  TestAllocator a;
  FakeAccel fake;
  set_accelerator(&fake);
  {
    const char* wgsl =
        "@group(0) @binding(0) var t: texture_2d<f32>;\n"
        "@group(0) @binding(1) var s: sampler;\n"
        "struct VsOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec3<f32>, }\n"
        "@vertex fn vs(@location(0) p: vec2<f32>, @location(1) c: vec3<f32>) -> VsOut { var o: VsOut; o.pos = vec4<f32>(p, 0.0, 1.0); o.uv = c; return o; }\n"
        "@fragment fn fs(in: VsOut) -> @location(0) vec4<f32> { return textureSample(t, s, in.uv.xy); }\n";
    GpuScene sc(a, wgsl);
    CHECK(sc.rp != 0);
    const wgpu::FormatDesc* rgba = wgpu::find_format("rgba8unorm");
    const uint32_t tex = sc.dev.create_texture(2, 2, 1, 1, 1, 1, rgba, 4 | 2);
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 9, 8, 7, 6};
    wgpu::ImageCopy dst;
    dst.texture = tex;
    wgpu::BufferLayout bl;
    bl.bytes_per_row = 8;
    bl.rows_per_image = 2;
    CHECK(sc.dev.write_texture(dst, ConstByteView(texels, sizeof texels), bl, 2, 2, 1));
    wgpu::TextureView tv;
    tv.texture = tex;
    tv.format = rgba;
    tv.dim = 1;
    const uint32_t view = sc.dev.create_view(tv);
    wgpu::Sampler sampler;
    sampler.params.min_filter = sampler.params.mag_filter = 0x2600;
    sampler.params.wrap_s = sampler.params.wrap_t = 0x812F;
    uint32_t smp = sc.dev.create_sampler(sampler);
    auto group = [&](uint32_t sampler_id) {
      List<wgpu::BgEntry> entries(a);
      wgpu::BgEntry e;
      e.binding = 0; e.kind = 2; e.id = view;
      entries.push_back(e);
      wgpu::BgEntry f;
      f.binding = 1; f.kind = 1; f.id = sampler_id;
      entries.push_back(f);
      return sc.dev.create_bind_group(sc.dev.pipeline_layout_bgl(sc.rp, false, 0), entries);
    };
    sc.bg = group(smp);
    CHECK(sc.bg != 0);
    CHECK(sc.pass(false));
    CHECK(sc.dev.draw(3, 1, 0, 0));
    CHECK(fake.draws == 1 && fake.last_program_textures == 1);
    CHECK(fake.textures_made == 1 && fake.texture_writes == 1 && fake.tex_w == 2 && fake.texel_bytes == 16 && fake.first_texel[0] == 255);
    CHECK(fake.last_state.texture_count == 1 && !fake.last_state.samplers[0].linear && !fake.last_state.samplers[0].repeat_s);
    CHECK(strstr(fake.fs_text, "TEX ") != nullptr && strstr(fake.fs_text, "DCL SAMP[0]") != nullptr);
    CHECK(sc.dev.draw(3, 1, 0, 0));
    CHECK(fake.draws == 2 && fake.texture_writes == 1);   // the same texels are not sent again
    CHECK(sc.dev.end_render_pass());
    // changed texels go again
    const uint8_t patch[4] = {1, 2, 3, 4};
    wgpu::BufferLayout one;
    one.bytes_per_row = 4;
    one.rows_per_image = 1;
    CHECK(sc.dev.write_texture(dst, ConstByteView(patch, 4), one, 1, 1, 1));
    CHECK(sc.pass(false));
    CHECK(sc.dev.draw(3, 1, 0, 0));
    CHECK(fake.draws == 3 && fake.texture_writes == 2 && fake.first_texel[0] == 1);
    CHECK(sc.dev.end_render_pass());
    // a comparison-free sampler the device lacks (mirrored repeat) is the CPU's
    wgpu::Sampler mirror = sampler;
    mirror.params.wrap_s = 0x8370;
    sc.bg = group(sc.dev.create_sampler(mirror));
    CHECK(sc.pass(false));
    CHECK(sc.dev.draw(3, 1, 0, 0));
    CHECK(fake.draws == 3);
    CHECK(sc.dev.end_render_pass());
    // destroying the texture frees the device copy
    const int destroyed = fake.textures_destroyed;
    sc.dev.destroy_texture(tex);
    CHECK(fake.textures_destroyed == destroyed + 1);
    CHECK(sc.dev.error.empty());
  }
  CHECK(fake.textures_made == fake.textures_destroyed);
  set_accelerator(nullptr);
  CHECK(a.live == 0);
}

static void test_webgpu_uniforms() {
  TestAllocator a;
  FakeAccel fake;
  set_accelerator(&fake);
  {
    const char* wgsl =
        "struct U { tint: vec4<f32>, m: mat2x2<f32>, k: f32, }\n"
        "@group(0) @binding(0) var<uniform> u: U;\n"
        "struct VsOut { @builtin(position) pos: vec4<f32>, @location(0) color: vec3<f32>, }\n"
        "@vertex fn vs(@location(0) p: vec2<f32>, @location(1) c: vec3<f32>) -> VsOut { var o: VsOut; let q = u.m * p; o.pos = vec4<f32>(q, 0.0, 1.0) * u.k; o.color = c * 2; return o; }\n"
        "@fragment fn fs(in: VsOut) -> @location(0) vec4<f32> { return u.tint * vec4<f32>(in.color, 1.0); }\n";
    GpuScene sc(a, wgsl, true);
    CHECK(sc.rp != 0 && sc.bg != 0);
    const float ub[20] = {0.5f, 0.25f, 0.125f, 1.0f, 1, 2, 3, 4, 7.0f, 0};   // tint, m (columns padded to 8 bytes), k
    sc.dev.write_buffer(sc.ubuf, 0, ConstByteView(reinterpret_cast<const uint8_t*>(ub), sizeof ub));
    CHECK(sc.pass(false));
    CHECK(sc.dev.draw(3, 1, 0, 0));
    CHECK(sc.dev.end_render_pass());
    CHECK(fake.draws == 1);
    CHECK(fake.constant_bytes == 16 * 4);   // the vertex shader's m (two columns) and k, the fragment shader's tint: only what is read
    CHECK(fake.first_const[0] == 1.0f && fake.first_const[1] == 2.0f);   // the first column of m
  }
  set_accelerator(nullptr);
  CHECK(a.live == 0);
}

static void test_webgpu_refusals() {
  TestAllocator a;
  FakeAccel fake;
  set_accelerator(&fake);
  {
    // a loop in the vertex shader, and reading a built-in input: both stay on the CPU
    const char* looped =
        "struct VsOut { @builtin(position) pos: vec4<f32>, @location(0) color: vec3<f32>, }\n"
        "@vertex fn vs(@location(0) p: vec2<f32>, @location(1) c: vec3<f32>) -> VsOut { var o: VsOut; var s = 0.0; for (var i = 0; i < 3; i = i + 1) { s = s + 1.0; } o.pos = vec4<f32>(p, 0.0, 1.0); o.color = c; return o; }\n"
        "@fragment fn fs(in: VsOut) -> @location(0) vec4<f32> { return vec4<f32>(in.color, 1.0); }\n";
    GpuScene sc(a, looped);
    CHECK(sc.pass(false));
    CHECK(sc.dev.draw(3, 1, 0, 0));
    CHECK(sc.dev.end_render_pass());
    CHECK(fake.programs == 0 && fake.draws == 0);   // (the translation refused: the device was never asked)
    CHECK(sc.texel(8, 8) == 0xFFFF0000u);
  }
  set_accelerator(nullptr);
  CHECK(a.live == 0);
}

int main() {
  test_translate_basic();
  test_translate_matrix_and_functions();
  test_translate_texture();
  test_translate_refusals();
  test_hardware_texture();
  test_device_textures();
  test_hardware_draw_and_readback();
  test_fallbacks();
  test_refused_program_and_failed_draw();
  test_uniforms_and_depth();
  test_no_accelerator();
  test_antialiased_target();
  test_device_accelerator();
  test_webgpu_hardware();
  test_webgpu_uniforms();
  test_webgpu_texture();
  test_webgpu_refusals();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("gl accel tests passed\n");
  return 0;
}
