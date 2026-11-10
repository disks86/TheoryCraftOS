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

// Host tests of the kernel's OpenGL: the generated client entry points (what opengl32.dll exports) write the command stream, which runs in a GlApi drawing to a GlSurface, so the
// encoding, the decoder and the implementation are tested together. Pictures are checked by reading pixels back.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/asan_interface.h>
#include "OpenGl/GlApi.hpp"
#include "OpenGl/GlService.hpp"
#include "OpenGl/GlSurface.hpp"
#include "OpenGl/GlClient.h"
#include "OpenGl/generated/GlEnums.inc"

using opengl::ContextConfig; using opengl::ContextRequest; using opengl::GlApi; using opengl::GlService; using opengl::GlStatus; using opengl::GlSurface; using opengl::IClientMemory; using opengl::ShareGroup; using opengl::SurfaceConfig; using opengl::SurfaceRequest;

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return aligned_alloc(16, (size + 15) & ~uint64_t{15}); }
  void deallocate(void* p) override { free(p); }
};

// The program's memory is this process's: addresses are pointers. (The kernel reaches it only through these calls.)
struct HostMemory final : IClientMemory {
  bool closed = false;      // (random streams: every address is refused, as a bad one would be)
  bool read(uint64_t address, ByteView out) override {
    if (!address || closed) return false;
    // Memory outside any object is refused, as an unmapped page would be (the kernel's string reads rely on that).
    if (__asan_region_is_poisoned(reinterpret_cast<void*>(address), out.size())) return false;
    memcpy(out.data(), reinterpret_cast<const void*>(address), out.size());
    return true;
  }
  bool write(uint64_t address, ConstByteView data) override {
    if (!address || closed) return false;
    if (__asan_region_is_poisoned(reinterpret_cast<void*>(address), data.size())) return false;
    memcpy(reinterpret_cast<void*>(address), data.data(), data.size());
    return true;
  }
  void* blocks[256] = {};
  uint32_t block_count = 0;
  uint64_t allocate(uint64_t bytes) override {
    if (closed || block_count == 256) return 0;
    void* p = calloc(1, bytes);
    blocks[block_count++] = p;
    return reinterpret_cast<uint64_t>(p);
  }
  void release(uint64_t address, uint64_t) override {
    for (uint32_t i = 0; i < block_count; ++i) if (blocks[i] == reinterpret_cast<void*>(address)) { free(blocks[i]); blocks[i] = blocks[--block_count]; return; }
  }
  // (what the GL keeps for the program, like the strings of glGetString, lives as long as the program: the test's end)
  ~HostMemory() { for (uint32_t i = 0; i < block_count; ++i) free(blocks[i]); }
};

static TestAllocator g_alloc;
static HostMemory g_memory;
static GlApi* g_api = nullptr;
static GlStream g_stream;
static uint64_t g_submits = 0;

GlStream* gl_stream_current(void) { return &g_stream; }
uint64_t gl_stream_submit(const uint32_t* words, uint32_t count) {
  ++g_submits;
  if (!g_api) return 0;
  return g_api->execute(ConstByteView(reinterpret_cast<const uint8_t*>(words), uint64_t{count} * 4), g_memory);
}

#include "OpenGl/generated/GlEntries.inc"

// (what the library defines around the generated encoders: CLIENT_WRAPPED in scripts/gen-opengl.py)
GLenum glGetError(void) { return tcos_glGetError(); }
void glFlush(void) { tcos_glFlush(); }
void glFinish(void) { tcos_glFinish(); }

constexpr uint32_t W = 64, H = 64;

struct Pixel { uint8_t r, g, b, a; };
static Pixel pixel(int x, int y) {
  Pixel p{0, 0, 0, 0};
  glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &p);
  return p;
}
static bool near(const Pixel& p, int r, int g, int b, int tolerance = 3) {
  const auto d = [](int a, int b) { return a > b ? a - b : b - a; };
  return d(p.r, r) <= tolerance && d(p.g, g) <= tolerance && d(p.b, b) <= tolerance;
}

// A context drawing to a fresh 64x64 window surface, with an orthographic 0..64 projection.
struct Fixture {
  ShareGroup group{g_alloc};
  GlSurface surface;
  GlApi api;
  explicit Fixture(bool core = false, bool accum = false) : surface(g_alloc, config(accum)), api(g_alloc, group, context(core)) {
    g_api = &api;
    api.bind_surface(&surface);
    if (!core) {
      glMatrixMode(GL_PROJECTION);
      glLoadIdentity();
      glOrtho(0, W, 0, H, -1, 1);
      glMatrixMode(GL_MODELVIEW);
      glLoadIdentity();
    }
  }
  ~Fixture() { gl_stream_flush(); g_api = nullptr; }
  static SurfaceConfig config(bool accum) { SurfaceConfig c; c.width = W; c.height = H; c.accum_bits = accum ? 64 : 0; return c; }
  static ContextConfig context(bool core) { ContextConfig c; c.core_profile = core; return c; }
};

static void test_strings_and_errors() {
  Fixture f;
  const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  CHECK(version && strncmp(version, "3.3", 3) == 0);
  const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  CHECK(ext && strstr(ext, "GL_ARB_vertex_buffer_object"));
  CHECK(glGetError() == GL_NO_ERROR);
  glEnable(0x1234);
  CHECK(glGetError() == GL_INVALID_ENUM);
  CHECK(glGetError() == GL_NO_ERROR);
  GLint vp[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, vp);
  CHECK(vp[2] == int(W) && vp[3] == int(H));
  glBegin(GL_TRIANGLES);
  glEnable(GL_BLEND);      // not allowed between glBegin and glEnd
  glEnd();
  CHECK(glGetError() == GL_INVALID_OPERATION);
  glPopMatrix();
  CHECK(glGetError() == GL_STACK_UNDERFLOW);
}

static void test_clear_and_immediate() {
  Fixture f;
  glClearColor(0, 0, 1, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  CHECK(near(pixel(5, 5), 0, 0, 255));
  glColor3f(1, 0, 0);
  glBegin(GL_TRIANGLES);
  glVertex2f(0, 0);
  glVertex2f(64, 0);
  glVertex2f(0, 64);
  glEnd();
  CHECK(near(pixel(10, 10), 255, 0, 0));
  CHECK(near(pixel(60, 60), 0, 0, 255));
  // a quad with a colour per vertex: smooth by default, the last vertex's colour with GL_FLAT (quads: the fourth)
  glClear(GL_COLOR_BUFFER_BIT);
  glShadeModel(GL_FLAT);
  glBegin(GL_QUADS);
  glColor3ub(255, 0, 0); glVertex2i(16, 16);
  glColor3ub(0, 255, 0); glVertex2i(48, 16);
  glColor3ub(0, 0, 255); glVertex2i(48, 48);
  glColor3ub(255, 255, 0); glVertex2i(16, 48);
  glEnd();
  CHECK(near(pixel(20, 20), 255, 255, 0));
  CHECK(near(pixel(44, 44), 255, 255, 0));
  glShadeModel(GL_SMOOTH);
  glRectf(0, 0, 8, 8);      // with the current colour (yellow)
  CHECK(near(pixel(4, 4), 255, 255, 0));
  CHECK(glGetError() == GL_NO_ERROR);
}

static void test_polygon_and_modes() {
  Fixture f;
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(0, 1, 0);
  glBegin(GL_POLYGON);
  glVertex2f(10, 10); glVertex2f(54, 10); glVertex2f(54, 54); glVertex2f(32, 60); glVertex2f(10, 54);
  glEnd();
  CHECK(near(pixel(32, 32), 0, 255, 0));
  CHECK(near(pixel(32, 58), 0, 255, 0));
  // GL_LINE: only the outline, no inner (diagonal) edges
  glClear(GL_COLOR_BUFFER_BIT);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glBegin(GL_QUADS);
  glVertex2f(10.5f, 10.5f); glVertex2f(50.5f, 10.5f); glVertex2f(50.5f, 50.5f); glVertex2f(10.5f, 50.5f);
  glEnd();
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  CHECK(near(pixel(30, 10), 0, 255, 0));
  CHECK(near(pixel(30, 30), 0, 0, 0));      // the diagonal of the two triangles is not drawn
  CHECK(near(pixel(20, 20), 0, 0, 0));
  // culling and the front face
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_CULL_FACE);
  glBegin(GL_TRIANGLES);      // clockwise: a back face
  glVertex2f(0, 0); glVertex2f(0, 64); glVertex2f(64, 0);
  glEnd();
  CHECK(near(pixel(10, 10), 0, 0, 0));
  glFrontFace(GL_CW);
  glBegin(GL_TRIANGLES);
  glVertex2f(0, 0); glVertex2f(0, 64); glVertex2f(64, 0);
  glEnd();
  CHECK(near(pixel(10, 10), 0, 255, 0));
  glDisable(GL_CULL_FACE);
  glFrontFace(GL_CCW);
  // alpha test
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 0.5f);
  glColor4f(1, 1, 1, 0.25f);
  glRectf(0, 0, 32, 64);
  glColor4f(1, 1, 1, 0.75f);
  glRectf(32, 0, 64, 64);
  glDisable(GL_ALPHA_TEST);
  CHECK(near(pixel(10, 10), 0, 0, 0));
  CHECK(near(pixel(50, 10), 255, 255, 255));
  // logic op: XOR of white over white is black
  glEnable(GL_COLOR_LOGIC_OP);
  glLogicOp(GL_XOR);
  glColor3f(1, 1, 1);
  glRectf(32, 0, 64, 32);
  glDisable(GL_COLOR_LOGIC_OP);
  CHECK(near(pixel(50, 10), 0, 0, 0));
  CHECK(near(pixel(50, 50), 255, 255, 255));
  CHECK(glGetError() == GL_NO_ERROR);
}

static void test_depth_and_blend() {
  Fixture f;
  glClearColor(0, 0, 0, 0);
  glClearDepth(1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glColor3f(1, 0, 0);
  glBegin(GL_QUADS); glVertex3f(0, 0, 0.5f); glVertex3f(64, 0, 0.5f); glVertex3f(64, 64, 0.5f); glVertex3f(0, 64, 0.5f); glEnd();      // (glOrtho: z -0.5, nearer)
  glColor3f(0, 1, 0);
  glBegin(GL_QUADS); glVertex3f(0, 0, -0.5f); glVertex3f(64, 0, -0.5f); glVertex3f(64, 64, -0.5f); glVertex3f(0, 64, -0.5f); glEnd();
  CHECK(near(pixel(30, 30), 255, 0, 0));
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(0, 0, 1, 0.5f);
  glRectf(0, 0, 64, 64);
  glDisable(GL_BLEND);
  CHECK(near(pixel(30, 30), 128, 0, 128, 4));
  float depth = 0;
  glReadPixels(30, 30, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
  CHECK(depth > 0.2f && depth < 0.3f);
}

static void test_lighting() {
  Fixture f;
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);      // light 0: white diffuse from +z (directional)
  const float diffuse[4] = {0.5f, 0.5f, 0.5f, 1};
  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, diffuse);
  const float ambient[4] = {0, 0, 0, 1};
  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, ambient);
  glNormal3f(0, 0, 1);
  glRectf(0, 0, 32, 64);
  glNormal3f(0, 0, -1);      // facing away: dark
  glRectf(32, 0, 64, 64);
  CHECK(near(pixel(10, 10), 128, 128, 128, 3));
  CHECK(near(pixel(50, 10), 0, 0, 0));
  // colour material: the colour becomes the diffuse colour
  glEnable(GL_COLOR_MATERIAL);
  glColor3f(1, 0, 0);
  glNormal3f(0, 0, 1);
  glRectf(0, 0, 32, 64);
  CHECK(near(pixel(10, 10), 255, 0, 0));
  float got[4] = {};
  glGetMaterialfv(GL_FRONT, GL_DIFFUSE, got);
  CHECK(got[0] == 1 && got[1] == 0);
  glDisable(GL_COLOR_MATERIAL);
  glDisable(GL_LIGHTING);
  // fog: linear, all fog at the far end
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_FOG);
  glFogi(GL_FOG_MODE, GL_LINEAR);
  glFogf(GL_FOG_START, 0);
  glFogf(GL_FOG_END, 1);
  const float fog[4] = {0, 1, 0, 1};
  glFogfv(GL_FOG_COLOR, fog);
  glColor3f(1, 0, 0);
  glBegin(GL_QUADS); glVertex3f(0, 0, -1); glVertex3f(64, 0, -1); glVertex3f(64, 64, -1); glVertex3f(0, 64, -1); glEnd();
  glDisable(GL_FOG);
  CHECK(near(pixel(30, 30), 0, 255, 0, 4));
}

static void test_textures() {
  Fixture f;
  GLuint tex = 0;
  glGenTextures(1, &tex);
  CHECK(tex != 0);
  glBindTexture(GL_TEXTURE_2D, tex);
  const uint8_t texels[2 * 2 * 3] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, texels);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  CHECK(glGetError() == GL_NO_ERROR);
  GLint width = 0;
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
  CHECK(width == 2);
  glEnable(GL_TEXTURE_2D);
  glColor3f(1, 1, 1);
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0); glVertex2f(0, 0);
  glTexCoord2f(1, 0); glVertex2f(64, 0);
  glTexCoord2f(1, 1); glVertex2f(64, 64);
  glTexCoord2f(0, 1); glVertex2f(0, 64);
  glEnd();
  CHECK(near(pixel(10, 10), 255, 0, 0));
  CHECK(near(pixel(50, 10), 0, 255, 0));
  CHECK(near(pixel(10, 50), 0, 0, 255));
  CHECK(near(pixel(50, 50), 255, 255, 255));
  // modulate by the colour
  glColor3f(0.5f, 0.5f, 0.5f);
  glTexCoord2f(0.25f, 0.75f);
  glRectf(0, 0, 64, 64);      // (one texture coordinate for every corner: the blue texel)
  CHECK(near(pixel(30, 30), 0, 0, 128, 3));
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glRectf(0, 0, 64, 64);
  CHECK(near(pixel(30, 30), 0, 0, 255));
  // read back the image in another format
  uint8_t back[16] = {};
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, back);
  CHECK(back[0] == 0 && back[1] == 0 && back[2] == 255 && back[3] == 255);
  glDisable(GL_TEXTURE_2D);
  glDeleteTextures(1, &tex);
  CHECK(!glIsTexture(tex));
  CHECK(glGetError() == GL_NO_ERROR);
}

static void test_arrays() {
  Fixture f;
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  const float positions[] = {0, 0, 32, 0, 32, 32, 0, 32};
  const uint8_t colors[] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, positions);
  glColorPointer(4, GL_UNSIGNED_BYTE, 0, colors);
  glDrawArrays(GL_QUADS, 0, 4);
  CHECK(near(pixel(10, 10), 255, 0, 0));
  CHECK(near(pixel(40, 40), 0, 0, 0));
  // indexed, from client memory, and through buffer objects
  const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
  glTranslatef(32, 32, 0);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
  CHECK(near(pixel(40, 40), 255, 0, 0));
  glLoadIdentity();
  glClear(GL_COLOR_BUFFER_BIT);
  GLuint buffers[2] = {0, 0};
  glGenBuffers(2, buffers);
  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
  glVertexPointer(2, GL_FLOAT, 0, nullptr);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
  glDisableClientState(GL_COLOR_ARRAY);
  glColor3f(0, 1, 0);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
  CHECK(near(pixel(10, 10), 0, 255, 0));
  // a mapped buffer
  float* mapped = static_cast<float*>(glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY));
  CHECK(mapped != nullptr);
  if (mapped) {
    for (int i = 0; i < 8; ++i) mapped[i] = positions[i] + 32;
    CHECK(glUnmapBuffer(GL_ARRAY_BUFFER) == GL_TRUE);
  }
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
  CHECK(near(pixel(10, 10), 0, 0, 0));
  CHECK(near(pixel(40, 40), 0, 255, 0));
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glDeleteBuffers(2, buffers);
  glDisableClientState(GL_VERTEX_ARRAY);
  // interleaved
  struct V { float c[3]; float p[3]; };
  const V quad[4] = {{{0, 0, 1}, {0, 0, 0}}, {{0, 0, 1}, {64, 0, 0}}, {{0, 0, 1}, {64, 64, 0}}, {{0, 0, 1}, {0, 64, 0}}};
  glInterleavedArrays(GL_C3F_V3F, 0, quad);
  glDrawArrays(GL_QUADS, 0, 4);
  CHECK(near(pixel(30, 30), 0, 0, 255));
  glDisableClientState(GL_VERTEX_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
  CHECK(glGetError() == GL_NO_ERROR);
}

static void test_display_lists() {
  Fixture f;
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  const GLuint list = glGenLists(2);
  CHECK(list != 0 && glIsList(list));
  glNewList(list, GL_COMPILE);
  glColor3f(1, 0, 1);
  glBegin(GL_TRIANGLES); glVertex2f(0, 0); glVertex2f(32, 0); glVertex2f(0, 32); glEnd();
  const float white[4] = {1, 1, 1, 1};
  glMaterialfv(GL_FRONT, GL_EMISSION, white);      // client data is copied into the list
  glEndList();
  CHECK(near(pixel(5, 5), 0, 0, 0));      // GL_COMPILE does not draw
  glCallList(list);
  CHECK(near(pixel(5, 5), 255, 0, 255));
  float emission[4] = {};
  glGetMaterialfv(GL_FRONT, GL_EMISSION, emission);
  CHECK(emission[0] == 1);
  // a list that calls another, with the arrays as they were when compiled
  const float positions[] = {32, 32, 64, 32, 64, 64};
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, positions);
  glNewList(list + 1, GL_COMPILE);
  glColor3f(0, 1, 1);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glEndList();
  glDisableClientState(GL_VERTEX_ARRAY);
  glClear(GL_COLOR_BUFFER_BIT);
  const GLubyte which[2] = {0, 1};
  glListBase(list);
  glCallLists(2, GL_UNSIGNED_BYTE, which);
  CHECK(near(pixel(5, 5), 255, 0, 255));
  CHECK(near(pixel(60, 40), 0, 255, 255));
  glDeleteLists(list, 2);
  CHECK(!glIsList(list));
  CHECK(glGetError() == GL_NO_ERROR);
}

static void test_pixels() {
  Fixture f(false, true);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  uint8_t block[4 * 4 * 4];
  for (int i = 0; i < 16; ++i) { block[i * 4] = 10; block[i * 4 + 1] = 200; block[i * 4 + 2] = 30; block[i * 4 + 3] = 255; }
  glRasterPos2i(8, 8);
  glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, block);
  CHECK(near(pixel(9, 9), 10, 200, 30, 1));
  CHECK(near(pixel(13, 13), 0, 0, 0));
  glPixelZoom(2, 2);
  glRasterPos2i(20, 20);
  glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, block);
  glPixelZoom(1, 1);
  CHECK(near(pixel(27, 27), 10, 200, 30, 1));
  // a bitmap in the raster colour, and the raster position moves
  const uint8_t bits[8] = {0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF};
  glColor3f(1, 1, 0);
  glRasterPos2i(40, 40);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glBitmap(8, 8, 0, 0, 10, 0, bits);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  CHECK(near(pixel(40, 40), 255, 255, 0));
  CHECK(near(pixel(43, 43), 0, 0, 0));
  float raster[4] = {};
  glGetFloatv(GL_CURRENT_RASTER_POSITION, raster);
  CHECK(raster[0] == 50 && raster[1] == 40);
  // the same bitmap in a display list, rows padded to 4 bytes as the default alignment has them (what wglUseFontBitmaps makes)
  const uint8_t padded[8 * 4] = {0xFF, 0, 0, 0, 0x81, 0, 0, 0, 0x81, 0, 0, 0, 0x81, 0, 0, 0, 0x81, 0, 0, 0, 0x81, 0, 0, 0, 0x81, 0, 0, 0, 0xFF, 0, 0, 0};
  const GLuint glyph = glGenLists(1);
  glNewList(glyph, GL_COMPILE);
  glBitmap(8, 8, 0, 0, 9, 0, padded);
  glEndList();
  glRasterPos2i(50, 50);
  glCallList(glyph);
  glCallList(glyph);
  glGetFloatv(GL_CURRENT_RASTER_POSITION, raster);
  CHECK(raster[0] == 68);
  CHECK(near(pixel(59, 50), 255, 255, 0));
  CHECK(glGetError() == GL_NO_ERROR);
  // copy pixels
  glRasterPos2i(0, 40);
  glCopyPixels(8, 8, 4, 4, GL_COLOR);
  CHECK(near(pixel(1, 41), 10, 200, 30, 1));
  // read back as a packed type with row padding
  uint16_t packed[4] = {};
  glReadPixels(8, 8, 1, 1, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, packed);
  CHECK((packed[0] >> 11) == 1 && ((packed[0] >> 5) & 63) == 49);
  // accumulation: average of two pictures
  glClearAccum(0, 0, 0, 0);
  glClear(GL_ACCUM_BUFFER_BIT);
  glClearColor(1, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glAccum(GL_ACCUM, 0.5f);
  glClearColor(0, 0, 1, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glAccum(GL_ACCUM, 0.5f);
  glAccum(GL_RETURN, 1);
  CHECK(near(pixel(30, 30), 128, 0, 128, 2));
  CHECK(glGetError() == GL_NO_ERROR);
}

static void test_state_stacks_and_get() {
  Fixture f;
  glEnable(GL_LIGHTING);
  glColor3f(0.25f, 0.5f, 0.75f);
  glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
  glDisable(GL_LIGHTING);
  glColor3f(1, 1, 1);
  glPopAttrib();
  CHECK(glIsEnabled(GL_LIGHTING));
  float c[4] = {};
  glGetFloatv(GL_CURRENT_COLOR, c);
  CHECK(c[0] == 0.25f && c[2] == 0.75f);
  glTranslatef(1, 2, 3);
  float m[16] = {};
  glGetFloatv(GL_MODELVIEW_MATRIX, m);
  CHECK(m[12] == 1 && m[13] == 2 && m[14] == 3);
  glPushMatrix();
  glRotatef(90, 0, 0, 1);
  glPopMatrix();
  glGetFloatv(GL_MODELVIEW_MATRIX, m);
  CHECK(m[0] == 1 && m[12] == 1);
  GLint depth = 0;
  glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &depth);
  CHECK(depth == 1);
  GLint max_lights = 0;
  glGetIntegerv(GL_MAX_LIGHTS, &max_lights);
  CHECK(max_lights == 8);
  GLboolean mask[4] = {};
  glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
  glGetBooleanv(GL_COLOR_WRITEMASK, mask);
  CHECK(mask[0] && !mask[1] && mask[2] && !mask[3]);
  glColorMask(1, 1, 1, 1);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 3);
  CHECK(glGetError() == GL_INVALID_VALUE);
  glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
  glPopClientAttrib();
  GLint align = 0;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
  CHECK(align == 4);
  CHECK(glGetError() == GL_NO_ERROR);
}

static void test_selection_and_feedback() {
  Fixture f;
  GLuint hits[64] = {};
  glSelectBuffer(64, hits);
  glRenderMode(GL_SELECT);
  glInitNames();
  glPushName(7);
  glRectf(0, 0, 10, 10);      // inside
  glLoadName(8);
  glRectf(100, 100, 110, 110);      // outside the view volume
  glLoadName(9);
  glBegin(GL_POINTS); glVertex2f(5, 5); glEnd();
  const GLint count = glRenderMode(GL_RENDER);
  CHECK(count == 2);
  CHECK(hits[0] == 1 && hits[3] == 7);
  CHECK(hits[4] == 1 && hits[7] == 9);
  float fb[64] = {};
  glFeedbackBuffer(64, GL_2D, fb);
  glRenderMode(GL_FEEDBACK);
  glBegin(GL_POINTS); glVertex2f(5, 6); glEnd();
  glPassThrough(42);
  glBegin(GL_LINES); glVertex2f(1, 1); glVertex2f(2, 2); glEnd();
  const GLint values = glRenderMode(GL_RENDER);
  CHECK(values == 3 + 2 + 5);
  CHECK(fb[0] == GL_POINT_TOKEN && fb[1] == 5 && fb[2] == 6);
  CHECK(fb[3] == GL_PASS_THROUGH_TOKEN && fb[4] == 42);
  CHECK(fb[5] == GL_LINE_RESET_TOKEN);
  CHECK(glGetError() == GL_NO_ERROR);
}

static void test_glsl_program() {
  Fixture f;
  const char* vs = "#version 300 es\nin vec4 pos;\nuniform vec2 offset;\nvoid main(){ gl_Position = vec4(pos.xy + offset, 0.0, 1.0); }\n";
  const char* fs = "#version 300 es\nprecision mediump float;\nuniform vec4 color;\nout vec4 o;\nvoid main(){ o = color; }\n";
  const GLuint v = glCreateShader(GL_VERTEX_SHADER), s = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(v, 1, &vs, nullptr);
  glShaderSource(s, 1, &fs, nullptr);
  glCompileShader(v);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
  CHECK(ok == GL_TRUE);
  const GLuint p = glCreateProgram();
  CHECK(p != v && p != s);
  glAttachShader(p, v);
  glAttachShader(p, s);
  glBindAttribLocation(p, 3, "pos");
  glLinkProgram(p);
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  CHECK(ok == GL_TRUE);
  glUseProgram(p);
  glUniform4f(glGetUniformLocation(p, "color"), 0, 1, 0, 1);
  glUniform2f(glGetUniformLocation(p, "offset"), 0.5f, 0.5f);
  GLint current = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &current);
  CHECK(current == int(p));
  const float tri[] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f};
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 0, tri);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glDisableVertexAttribArray(3);
  CHECK(near(pixel(36, 36), 0, 255, 0));
  CHECK(near(pixel(10, 10), 0, 0, 0));
  float got[4] = {};
  glGetUniformfv(p, glGetUniformLocation(p, "color"), got);
  CHECK(got[1] == 1);
  glUseProgram(0);
  glDeleteProgram(p);
  glDeleteShader(v);
  glDeleteShader(s);
  CHECK(!glIsProgram(p));
  CHECK(glGetError() == GL_NO_ERROR);
}

static GLuint build(const char* vs, const char* fs) {
  const GLuint v = glCreateShader(GL_VERTEX_SHADER), s = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(v, 1, &vs, nullptr);
  glShaderSource(s, 1, &fs, nullptr);
  glCompileShader(v);
  glCompileShader(s);
  GLint ok = 0;
  char log[512] = {};
  glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
  if (!ok) { glGetShaderInfoLog(v, sizeof(log), nullptr, log); printf("vertex shader: %s\n", log); }
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glGetShaderInfoLog(s, sizeof(log), nullptr, log); printf("fragment shader: %s\n", log); }
  const GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, s);
  glLinkProgram(p);
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { glGetProgramInfoLog(p, sizeof(log), nullptr, log); printf("link: %s\n", log); }
  glDeleteShader(v);
  glDeleteShader(s);
  return ok ? p : 0;
}

// Desktop GLSL with the compatibility built-ins, fed by the fixed-function state.
static void test_desktop_glsl() {
  Fixture f;
  const GLuint p = build("#version 120\nvarying vec4 tint;\nvoid main(){ gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; gl_FrontColor = gl_Color; tint = vec4(gl_MultiTexCoord0.xy, 0.0, 1.0); }\n",
                         "#version 120\nvarying vec4 tint;\nvoid main(){ gl_FragColor = gl_Color + tint; }\n");
  CHECK(p != 0);
  glUseProgram(p);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(1, 0, 0);
  glTexCoord2f(0, 1);
  glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(32, 0); glVertex2f(32, 32); glVertex2f(0, 32); glEnd();
  CHECK(near(pixel(10, 10), 255, 255, 0));      // the colour plus the texture coordinate's green
  CHECK(near(pixel(50, 50), 0, 0, 0));
  glUseProgram(0);
  glDeleteProgram(p);
  // ftransform() and no #version at all (1.10)
  const GLuint q = build("void main(){ gl_Position = ftransform(); }\n", "void main(){ gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0); }\n");
  CHECK(q != 0);
  glUseProgram(q);
  glRectf(32, 32, 64, 64);
  CHECK(near(pixel(50, 50), 0, 0, 255));
  glUseProgram(0);
  glDeleteProgram(q);
  // 3.30 core style: explicit locations, own outputs
  const GLuint r = build("#version 330 core\nlayout(location = 2) in vec2 pos;\nvoid main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n",
                         "#version 330 core\nout vec4 color;\nuniform sampler2D unused;\nvoid main(){ color = vec4(0.0, 1.0, 1.0, 1.0); }\n");
  CHECK(r != 0);
  glUseProgram(r);
  const float tri[] = {-1, -1, 1, -1, -1, 1};
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, tri);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glDisableVertexAttribArray(2);
  CHECK(near(pixel(5, 5), 0, 255, 255));
  glUseProgram(0);
  glDeleteProgram(r);
  // 1.30 with gl_FragColor and texture2D
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  const uint8_t texel[4] = {200, 100, 50, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  const GLuint t = build("#version 130\nvoid main(){ gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; gl_TexCoord[0] = gl_MultiTexCoord0; }\n",
                         "#version 130\nuniform sampler2D tex;\nvoid main(){ gl_FragColor = texture2D(tex, gl_TexCoord[0].st); }\n");
  CHECK(t != 0);
  glUseProgram(t);
  glUniform1i(glGetUniformLocation(t, "tex"), 0);
  glRectf(0, 0, 64, 64);
  CHECK(near(pixel(30, 30), 200, 100, 50));
  glUseProgram(0);
  glDeleteProgram(t);
  glDeleteTextures(1, &tex);
  // the uniform state of the compatibility profile: lights, materials, the depth range
  const float diffuse[4] = {0.5f, 0.25f, 0, 1};
  glLightfv(GL_LIGHT3, GL_DIFFUSE, diffuse);
  const float emission[4] = {0, 0, 0.75f, 1};
  glMaterialfv(GL_FRONT, GL_EMISSION, emission);
  glDepthRange(0.25, 0.75);
  const GLuint st = build("#version 110\nvoid main(){ gl_Position = ftransform(); }\n",
                          "#version 110\nvoid main(){ gl_FragColor = vec4(gl_LightSource[3].diffuse.rg, gl_FrontMaterial.emission.b, gl_DepthRange.diff); }\n");
  CHECK(st != 0);
  glUseProgram(st);
  glRectf(0, 0, 64, 64);
  CHECK(near(pixel(30, 30), 128, 64, 191, 2));
  glUseProgram(0);
  glDeleteProgram(st);
  glDepthRange(0, 1);
  CHECK(glGetError() == GL_NO_ERROR);
}

// Several shaders in one stage, linked together: each compiles on its own (no main needed), globals of one name are one variable, a prototype finds its body in
// another shader, and a function is visible only where it is declared.
static GLuint build_units(const char* const* vs, int nvs, const char* const* fs, int nfs, bool expect_link) {
  const GLuint p = glCreateProgram();
  for (int stage = 0; stage < 2; ++stage) {
    const char* const* list = stage == 0 ? vs : fs;
    for (int i = 0; i < (stage == 0 ? nvs : nfs); ++i) {
      const GLuint s = glCreateShader(stage == 0 ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
      glShaderSource(s, 1, &list[i], nullptr);
      glCompileShader(s);
      GLint ok = 0;
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      CHECK(ok);
      glAttachShader(p, s);
      glDeleteShader(s);      // (stays while attached)
    }
  }
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  CHECK((ok != 0) == expect_link);
  if (expect_link && !ok) {
    char log[512] = {};
    glGetProgramInfoLog(p, sizeof(log), nullptr, log);
    printf("link: %s\n", log);
  }
  return p;
}

static void test_several_shaders_per_stage() {
  Fixture f;
  const char* vs[] = {"#version 120\nvec4 place(vec4 v);\nvoid main(){ gl_Position = place(gl_Vertex); }\n",
                      "#version 120\nuniform float scale;\nvec4 place(vec4 v) { return gl_ModelViewProjectionMatrix * vec4(v.xyz * scale, 1.0); }\n"};
  const char* fs[] = {"#version 120\nuniform float scale;\nvec4 shade();\nvoid main(){ gl_FragColor = shade(); }\n",
                      "#version 120\nuniform float scale;\nfloat abs(float x) { return 1.0; }\nvec4 shade() { return vec4(scale * 0.5, abs(-0.25), 0.0, 1.0); }\n"};
  const GLuint p = build_units(vs, 2, fs, 2, true);
  GLint n = 0;
  glGetProgramiv(p, GL_ATTACHED_SHADERS, &n);
  CHECK(n == 4);
  GLint uniforms = 0;
  glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &uniforms);
  int scales = 0;      // (scale, once: one variable in every shader)
  for (GLint i = 0; i < uniforms; ++i) {
    char name[64] = {};
    GLint size = 0;
    GLenum type = 0;
    glGetActiveUniform(p, static_cast<GLuint>(i), sizeof(name), nullptr, &size, &type, name);
    if (!strcmp(name, "scale")) ++scales;
  }
  CHECK(scales == 1);
  glUseProgram(p);
  glUniform1f(glGetUniformLocation(p, "scale"), 1.0f);
  glRectf(0, 0, 64, 64);
  CHECK(near(pixel(30, 30), 128, 255, 0, 2));      // the user abs() defined in that shader
  GLuint shaders[4] = {};
  glGetAttachedShaders(p, 4, nullptr, shaders);
  glUseProgram(0);
  glDeleteProgram(p);
  CHECK(!glIsShader(shaders[0]) && !glIsShader(shaders[3]));      // (deleted while attached: gone with the program)
  // the built-in abs() where the user one is not declared
  const char* fs2[] = {"#version 120\nfloat half_abs(float);\nfloat abs(float x) { return 1.0; }\nvoid main(){ gl_FragColor = vec4(half_abs(-1.0), 0.0, 0.0, 1.0); }\n",
                       "#version 120\nfloat half_abs(float x) { return abs(x) * 0.25; }\n"};
  const char* vs1[] = {"#version 120\nvoid main(){ gl_Position = ftransform(); }\n"};
  const GLuint q = build_units(vs1, 1, fs2, 2, true);
  glUseProgram(q);
  glRectf(0, 0, 64, 64);
  CHECK(near(pixel(30, 30), 64, 0, 0, 2));
  glUseProgram(0);
  glDeleteProgram(q);
  // link errors: two mains, one global with two types, a function called but defined nowhere
  const char* two_mains[] = {"void main(){ gl_Position = ftransform(); }\n", "void main(){ gl_Position = vec4(0.0); }\n"};
  glDeleteProgram(build_units(two_mains, 2, fs, 2, false));
  const char* types[] = {"#version 120\nuniform float scale;\nvec4 place(vec4 v) { return v * scale; }\n", "#version 120\nuniform vec2 scale;\nvec4 place(vec4 v);\nvoid main(){ gl_Position = place(gl_Vertex); }\n"};
  glDeleteProgram(build_units(types, 2, fs, 2, false));
  const char* undefined[] = {"#version 120\nvec4 place(vec4 v);\nvoid main(){ gl_Position = place(gl_Vertex); }\n"};
  glDeleteProgram(build_units(undefined, 1, fs, 2, false));
  CHECK(glGetError() == GL_NO_ERROR);
}

// Geometry and tessellation stages.
static GLuint build_stages(const char* vs, const char* tcs, const char* tes, const char* gs, const char* fs, const char* const* captured = nullptr, int ncaptured = 0,
                           GLenum capture_mode = GL_INTERLEAVED_ATTRIBS) {
  const GLuint p = glCreateProgram();
  const char* const sources[5] = {vs, tcs, tes, gs, fs};
  const GLenum types[5] = {GL_VERTEX_SHADER, GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER};
  for (int i = 0; i < 5; ++i) {
    if (!sources[i]) continue;
    const GLuint s = glCreateShader(types[i]);
    glShaderSource(s, 1, &sources[i], nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512] = {}; glGetShaderInfoLog(s, sizeof(log), nullptr, log); printf("shader %d: %s\n", i, log); }
    glAttachShader(p, s);
    glDeleteShader(s);
  }
  if (captured) glTransformFeedbackVaryings(p, ncaptured, captured, capture_mode);
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { char log[512] = {}; glGetProgramInfoLog(p, sizeof(log), nullptr, log); printf("link: %s\n", log); glDeleteProgram(p); return 0; }
  return p;
}

static void test_geometry_and_tessellation() {
  Fixture f;
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  const char* vs = "#version 150\nin vec4 pos;\nout vec4 tint;\nvoid main(){ gl_Position = pos; tint = vec4(pos.xy * 0.5 + 0.5, 0.0, 1.0); }\n";
  const char* fs = "#version 150\nin vec4 color;\nout vec4 frag;\nvoid main(){ frag = color; }\n";
  // A point becomes a quad (a strip of 4), coloured by the invocation's primitive.
  const char* gs = "#version 150\nlayout(points) in;\nlayout(triangle_strip, max_vertices = 4) out;\nin vec4 tint[];\nout vec4 color;\n"
                   "void main(){ for (int i = 0; i < 4; ++i) { gl_Position = gl_in[0].gl_Position + vec4(float(i & 1) * 0.5 - 0.25, float(i >> 1) * 0.5 - 0.25, 0.0, 0.0);\n"
                   " color = gl_PrimitiveIDIn == 0 ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 0.0, 1.0, 1.0); EmitVertex(); } EndPrimitive(); }\n";
  const GLuint g = build_stages(vs, nullptr, nullptr, gs, fs);
  CHECK(g != 0);
  GLint value = 0;
  glGetProgramiv(g, GL_GEOMETRY_VERTICES_OUT, &value);
  CHECK(value == 4);
  glGetProgramiv(g, GL_GEOMETRY_INPUT_TYPE, &value);
  CHECK(value == GL_POINTS);
  glGetProgramiv(g, GL_GEOMETRY_OUTPUT_TYPE, &value);
  CHECK(value == GL_TRIANGLE_STRIP);
  GLuint vao = 0, vbo = 0;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  const float points[8] = {-0.5f, -0.5f, 0, 1, 0.5f, 0.5f, 0, 1};
  glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STATIC_DRAW);
  const GLint at = glGetAttribLocation(g, "pos");
  glEnableVertexAttribArray(static_cast<GLuint>(at));
  glVertexAttribPointer(static_cast<GLuint>(at), 4, GL_FLOAT, GL_FALSE, 0, nullptr);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(g);
  glDrawArrays(GL_POINTS, 0, 2);
  CHECK(glGetError() == GL_NO_ERROR);
  CHECK(near(pixel(16, 16), 255, 0, 0));      // around (-0.5, -0.5): the first point's quad
  CHECK(near(pixel(48, 48), 0, 0, 255));      // the second's
  CHECK(near(pixel(32, 32), 0, 0, 0));        // between them
  glDrawArrays(GL_TRIANGLES, 0, 3);           // (not the geometry stage's input primitive)
  CHECK(glGetError() == GL_INVALID_OPERATION);
  glUseProgram(0);
  glDeleteProgram(g);

  // Triangles through a geometry stage that passes them on with a varying of its own.
  const char* gs_tri = "#version 150\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 3) out;\nin vec4 tint[3];\nout vec4 color;\n"
                       "void main(){ for (int i = 0; i < gl_in.length(); ++i) { gl_Position = gl_in[i].gl_Position; color = vec4(0.0, 1.0, 0.0, 1.0) + tint[i] * 0.0; EmitVertex(); } }\n";
  const GLuint t = build_stages(vs, nullptr, nullptr, gs_tri, fs);
  CHECK(t != 0);
  const float tri[12] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1};
  glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(t);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  CHECK(glGetError() == GL_NO_ERROR);
  CHECK(near(pixel(10, 10), 0, 255, 0));
  CHECK(near(pixel(60, 60), 0, 0, 0));
  glUseProgram(0);
  glDeleteProgram(t);

  // Tessellation: a quad patch of four corners, the control stage passing them on (with a barrier between writing and reading the outputs), cut in 4 x 4.
  const char* tcs = "#version 150\n#extension GL_ARB_tessellation_shader : require\nlayout(vertices = 4) out;\nin vec4 tint[];\nout vec4 c_tint[];\n"
                    "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position; c_tint[gl_InvocationID] = tint[gl_InvocationID]; barrier();\n"
                    " if (gl_InvocationID == 0) { gl_TessLevelOuter[0] = 4.0; gl_TessLevelOuter[1] = 4.0; gl_TessLevelOuter[2] = 4.0; gl_TessLevelOuter[3] = 4.0;\n"
                    " gl_TessLevelInner[0] = 4.0; gl_TessLevelInner[1] = c_tint[3].w * 4.0; } }\n";
  const char* tes = "#version 150\n#extension GL_ARB_tessellation_shader : require\nlayout(quads, equal_spacing, ccw) in;\nin vec4 c_tint[];\nout vec4 color;\n"
                    "void main(){ vec4 a = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, gl_TessCoord.x), b = mix(gl_in[3].gl_Position, gl_in[2].gl_Position, gl_TessCoord.x);\n"
                    " gl_Position = mix(a, b, gl_TessCoord.y); color = vec4(1.0, 1.0, 0.0, 1.0) * c_tint[0].w; }\n";
  const GLuint q = build_stages(vs, tcs, tes, nullptr, fs);
  CHECK(q != 0);
  glGetProgramiv(q, GL_TESS_CONTROL_OUTPUT_VERTICES, &value);
  CHECK(value == 4);
  glGetProgramiv(q, GL_TESS_GEN_MODE, &value);
  CHECK(value == GL_QUADS);
  const float quad[16] = {-1, -1, 0, 1, 0, -1, 0, 1, 0, 0, 0, 1, -1, 0, 0, 1};
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(q);
  glPatchParameteri(GL_PATCH_VERTICES, 4);
  glDrawArrays(GL_PATCHES, 0, 4);
  CHECK(glGetError() == GL_NO_ERROR);
  CHECK(near(pixel(8, 8), 255, 255, 0) && near(pixel(24, 24), 255, 255, 0) && near(pixel(30, 2), 255, 255, 0));      // the lower left quarter, all of it
  CHECK(near(pixel(40, 40), 0, 0, 0) && near(pixel(40, 10), 0, 0, 0));
  glDrawArrays(GL_TRIANGLES, 0, 3);      // (a tessellation program draws patches only)
  CHECK(glGetError() == GL_INVALID_OPERATION);
  glUseProgram(0);
  glDeleteProgram(q);

  // An evaluation stage alone: the levels from glPatchParameterfv; triangles.
  const char* tes_tri = "#version 150\n#extension GL_ARB_tessellation_shader : require\nlayout(triangles) in;\nout vec4 color;\n"
                        "void main(){ gl_Position = gl_TessCoord.x * gl_in[0].gl_Position + gl_TessCoord.y * gl_in[1].gl_Position + gl_TessCoord.z * gl_in[2].gl_Position;\n"
                        " color = vec4(gl_TessLevelOuter[0] / 8.0, 0.0, 1.0, 1.0); }\n";
  const GLuint e = build_stages(vs, nullptr, tes_tri, nullptr, fs);
  CHECK(e != 0);
  glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
  const float outer[4] = {4, 4, 4, 4}, inner[2] = {3, 3};
  glPatchParameterfv(GL_PATCH_DEFAULT_OUTER_LEVEL, outer);
  glPatchParameterfv(GL_PATCH_DEFAULT_INNER_LEVEL, inner);
  glPatchParameteri(GL_PATCH_VERTICES, 3);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(e);
  glDrawArrays(GL_PATCHES, 0, 3);
  CHECK(glGetError() == GL_NO_ERROR);
  CHECK(near(pixel(10, 10), 128, 0, 255, 2));
  CHECK(near(pixel(60, 60), 0, 0, 0));
  glUseProgram(0);
  glDeleteProgram(e);
  // link errors: a control stage without an evaluation stage; a geometry stage without its layouts
  CHECK(build_stages(vs, tcs, nullptr, nullptr, fs) == 0);
  CHECK(build_stages(vs, nullptr, nullptr, "#version 150\nvoid main(){ EmitVertex(); }\n", fs) == 0);
  glDeleteBuffers(1, &vbo);
  glDeleteVertexArrays(1, &vao);
  CHECK(glGetError() == GL_NO_ERROR);
}

// The floats a transform feedback buffer holds (and how many of `n` were written: the rest stay at -1).
static void read_floats(GLuint buffer, float* out, int n) {
  glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
  glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, n * 4, out);
}
static void reset_floats(GLuint buffer, int n) {
  float fill[64];
  for (int i = 0; i < 64; ++i) fill[i] = -1;
  glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
  glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, n * 4, fill, GL_DYNAMIC_COPY);
}

// Transform feedback from each stage that may end vertex processing: strips as separate primitives, the geometry and evaluation stages' vertices, the primitive
// queries, overflow, interleaved specials (gl_SkipComponents, gl_NextBuffer), pause/resume, objects and glDrawTransformFeedback.
static void test_transform_feedback_stages() {
  Fixture f;
  GLuint vao = 0, vbo = 0, tfb[2] = {0, 0}, queries[2] = {0, 0};
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glGenBuffers(2, tfb);
  glGenQueries(2, queries);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  const float verts[16] = {0, 0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 1, 3, 0, 0, 1};      // x = the vertex's number
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
  const char* fs = "#version 150\nout vec4 frag;\nvoid main(){ frag = vec4(1.0); }\n";
  const auto attrib = [&](GLuint p) {
    const GLint at = glGetAttribLocation(p, "pos");
    glEnableVertexAttribArray(static_cast<GLuint>(at));
    glVertexAttribPointer(static_cast<GLuint>(at), 4, GL_FLOAT, GL_FALSE, 0, nullptr);
  };
  float got[64];

  // The vertex stage, a strip of 4: two triangles (0 1 2) (2 1 3); the queries count both.
  const char* vs = "#version 150\nin vec4 pos;\nout float id;\nvoid main(){ gl_Position = vec4(0.0); id = pos.x; }\n";
  const char* id_name[] = {"id"};
  const GLuint v = build_stages(vs, nullptr, nullptr, nullptr, fs, id_name, 1);
  CHECK(v != 0);
  attrib(v);
  glUseProgram(v);
  reset_floats(tfb[0], 8);
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfb[0]);
  glEnable(GL_RASTERIZER_DISCARD);
  glBeginQuery(GL_PRIMITIVES_GENERATED, queries[0]);
  glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, queries[1]);
  glBeginTransformFeedback(GL_TRIANGLES);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glEndTransformFeedback();
  glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
  glEndQuery(GL_PRIMITIVES_GENERATED);
  CHECK(glGetError() == GL_NO_ERROR);
  read_floats(tfb[0], got, 7);
  CHECK(got[0] == 0 && got[1] == 1 && got[2] == 2 && got[3] == 2 && got[4] == 1 && got[5] == 3 && got[6] == -1);
  GLuint count = 0;
  glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &count);
  CHECK(count == 2);
  glGetQueryObjectuiv(queries[1], GL_QUERY_RESULT, &count);
  CHECK(count == 2);
  // a mode other than glBeginTransformFeedback's
  glBeginTransformFeedback(GL_POINTS);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  CHECK(glGetError() == GL_INVALID_OPERATION);
  glEndTransformFeedback();
  // overflow: room for one triangle; the second is generated but neither written nor counted as written
  reset_floats(tfb[0], 4);
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfb[0]);
  glBeginQuery(GL_PRIMITIVES_GENERATED, queries[0]);
  glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, queries[1]);
  glBeginTransformFeedback(GL_TRIANGLES);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glEndTransformFeedback();
  glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
  glEndQuery(GL_PRIMITIVES_GENERATED);
  read_floats(tfb[0], got, 4);
  CHECK(got[0] == 0 && got[1] == 1 && got[2] == 2 && got[3] == -1);
  glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &count);
  CHECK(count == 2);
  glGetQueryObjectuiv(queries[1], GL_QUERY_RESULT, &count);
  CHECK(count == 1);
  // paused: nothing captured until resumed
  reset_floats(tfb[0], 8);
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfb[0]);
  glBeginTransformFeedback(GL_POINTS);
  glDrawArrays(GL_POINTS, 0, 1);
  glPauseTransformFeedback();
  GLint flag = 0;
  glGetIntegerv(GL_TRANSFORM_FEEDBACK_PAUSED, &flag);
  CHECK(flag == 1);
  glDrawArrays(GL_POINTS, 1, 1);
  glResumeTransformFeedback();
  glDrawArrays(GL_POINTS, 2, 2);
  glEndTransformFeedback();
  read_floats(tfb[0], got, 4);
  CHECK(got[0] == 0 && got[1] == 2 && got[2] == 3 && got[3] == -1);
  glResumeTransformFeedback();      // (not active)
  CHECK(glGetError() == GL_INVALID_OPERATION);
  glUseProgram(0);
  glDeleteProgram(v);

  // The geometry stage: each point becomes a strip of 4 (ids 10p + 0..3); the captured triangles are the strip's, wound as drawn.
  const char* gs = "#version 150\nlayout(points) in;\nlayout(triangle_strip, max_vertices = 4) out;\nin float vid[];\nout float id;\nout vec2 pair;\n"
                   "void main(){ for (int i = 0; i < 4; ++i) { gl_Position = vec4(0.0); id = vid[0] * 10.0 + float(i); pair = vec2(float(i), -float(i)); EmitVertex(); } }\n";
  const char* g_names[] = {"id"};
  const GLuint g = build_stages("#version 150\nin vec4 pos;\nout float vid;\nvoid main(){ gl_Position = vec4(0.0); vid = pos.x; }\n", nullptr, nullptr, gs, fs, g_names, 1);
  CHECK(g != 0);
  attrib(g);
  glUseProgram(g);
  reset_floats(tfb[0], 16);
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfb[0]);
  glBeginQuery(GL_PRIMITIVES_GENERATED, queries[0]);
  glBeginTransformFeedback(GL_TRIANGLES);
  glDrawArrays(GL_POINTS, 1, 2);
  glEndTransformFeedback();
  glEndQuery(GL_PRIMITIVES_GENERATED);
  CHECK(glGetError() == GL_NO_ERROR);
  read_floats(tfb[0], got, 13);
  CHECK(got[0] == 10 && got[1] == 11 && got[2] == 12 && got[3] == 12 && got[4] == 11 && got[5] == 13);
  CHECK(got[6] == 20 && got[11] == 23 && got[12] == -1);
  glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &count);
  CHECK(count == 4);
  glBeginTransformFeedback(GL_POINTS);      // (the geometry stage puts out triangles)
  glDrawArrays(GL_POINTS, 0, 1);
  CHECK(glGetError() == GL_INVALID_OPERATION);
  glEndTransformFeedback();
  glUseProgram(0);
  glDeleteProgram(g);

  // Interleaved specials from the geometry stage: buffer 0 takes id and a skipped component, gl_NextBuffer moves on, buffer 1 takes pair.
  const char* specials[] = {"id", "gl_SkipComponents1", "gl_NextBuffer", "pair"};
  const GLuint s = build_stages("#version 150\nin vec4 pos;\nout float vid;\nvoid main(){ gl_Position = vec4(0.0); vid = pos.x; }\n", nullptr, nullptr,
                                "#version 150\nlayout(points) in;\nlayout(points, max_vertices = 2) out;\nin float vid[];\nout float id;\nout vec2 pair;\n"
                                "void main(){ for (int i = 0; i < 2; ++i) { id = vid[0] + float(i); pair = vec2(id, -id); EmitVertex(); } }\n",
                                fs, specials, 4);
  CHECK(s != 0);
  GLsizei len = 0, size = 0;
  GLenum type = 0;
  char name[32] = {};
  glGetTransformFeedbackVarying(s, 3, sizeof(name), &len, &size, &type, name);
  CHECK(type == GL_FLOAT_VEC2 && size == 1 && len == 4 && name[0] == 'p');
  attrib(s);
  glUseProgram(s);
  reset_floats(tfb[0], 8);
  reset_floats(tfb[1], 8);
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfb[0]);
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 1, tfb[1]);
  glBeginTransformFeedback(GL_POINTS);
  glDrawArrays(GL_POINTS, 2, 1);
  glEndTransformFeedback();
  CHECK(glGetError() == GL_NO_ERROR);
  read_floats(tfb[0], got, 5);
  CHECK(got[0] == 2 && got[1] == -1 && got[2] == 3 && got[3] == -1 && got[4] == -1);      // id, a skipped component, id, skipped
  read_floats(tfb[1], got, 5);
  CHECK(got[0] == 2 && got[1] == -2 && got[2] == 3 && got[3] == -3 && got[4] == -1);
  glUseProgram(0);
  glDeleteProgram(s);
  // a name that is not an output fails the link; an array element past the end too
  const char* bad[] = {"nothing"};
  CHECK(build_stages(vs, nullptr, nullptr, nullptr, fs, bad, 1) == 0);
  const char* past[] = {"gl_ClipDistance[9]"};
  CHECK(build_stages(vs, nullptr, nullptr, nullptr, fs, past, 1) == 0);

  // The evaluation stage: one triangle (levels 1), its points' gl_TessCoord.x; then glDrawTransformFeedback draws what an object captured.
  const char* tes = "#version 150\n#extension GL_ARB_tessellation_shader : require\nlayout(triangles) in;\nout float u;\nvoid main(){ gl_Position = vec4(0.0); u = gl_TessCoord.x; }\n";
  const char* u_name[] = {"u"};
  const GLuint e = build_stages("#version 150\nin vec4 pos;\nvoid main(){ gl_Position = pos; }\n", nullptr, tes, nullptr, fs, u_name, 1);
  CHECK(e != 0);
  attrib(e);
  glUseProgram(e);
  const float ones[4] = {1, 1, 1, 1};
  glPatchParameterfv(GL_PATCH_DEFAULT_OUTER_LEVEL, ones);
  glPatchParameterfv(GL_PATCH_DEFAULT_INNER_LEVEL, ones);
  glPatchParameteri(GL_PATCH_VERTICES, 3);
  GLuint object = 0;
  glGenTransformFeedbacks(1, &object);
  glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, object);
  GLint bound = 0;
  glGetIntegerv(GL_TRANSFORM_FEEDBACK_BINDING, &bound);
  CHECK(bound == static_cast<GLint>(object) && glIsTransformFeedback(object));
  reset_floats(tfb[0], 8);
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfb[0]);
  glBeginTransformFeedback(GL_TRIANGLES);
  glDrawArrays(GL_PATCHES, 0, 3);
  glEndTransformFeedback();
  CHECK(glGetError() == GL_NO_ERROR);
  read_floats(tfb[0], got, 4);
  float sum = 0;
  for (int i = 0; i < 3; ++i) sum += got[i];
  CHECK(sum == 1 && got[3] == -1);      // (the corners: u = 1 at one, 0 at the others)
  glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);
  glUseProgram(0);
  glDeleteProgram(e);
  // the vertex stage again, drawing the object's 3 captured vertices as points: three primitives generated
  const GLuint d = build_stages(vs, nullptr, nullptr, nullptr, fs);
  CHECK(d != 0);
  attrib(d);
  glUseProgram(d);
  glBeginQuery(GL_PRIMITIVES_GENERATED, queries[0]);
  glDrawTransformFeedback(GL_POINTS, object);
  glEndQuery(GL_PRIMITIVES_GENERATED);
  glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &count);
  CHECK(count == 3);
  CHECK(glGetError() == GL_NO_ERROR);
  glUseProgram(0);
  glDeleteProgram(d);
  glDeleteTransformFeedbacks(1, &object);
  CHECK(!glIsTransformFeedback(object));

  // The shader's own layout (GLSL 4.40): offsets with gaps, a buffer's stride, a block's members one after another; glTransformFeedbackVaryings not called.
  const GLuint x = build_stages("#version 440\nin vec4 pos;\nlayout(xfb_offset = 4) out float a;\nlayout(xfb_offset = 12) out float b;\n"
                                "layout(xfb_buffer = 1, xfb_stride = 12) out;\nlayout(xfb_buffer = 1, xfb_offset = 0) out Blk { float p; float q; } blk;\n"
                                "void main(){ gl_Position = vec4(0.0); a = pos.x; b = pos.x + 100.0; blk.p = -pos.x; blk.q = pos.x * 2.0; }\n",
                                nullptr, nullptr, nullptr, fs);
  CHECK(x != 0);
  attrib(x);
  glUseProgram(x);
  reset_floats(tfb[0], 16);
  reset_floats(tfb[1], 16);
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfb[0]);
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 1, tfb[1]);
  glBeginTransformFeedback(GL_POINTS);
  glDrawArrays(GL_POINTS, 2, 2);
  glEndTransformFeedback();
  CHECK(glGetError() == GL_NO_ERROR);
  read_floats(tfb[0], got, 9);
  CHECK(got[0] == -1 && got[1] == 2 && got[2] == -1 && got[3] == 102 && got[5] == 3 && got[7] == 103 && got[8] == -1);
  read_floats(tfb[1], got, 7);
  CHECK(got[0] == -2 && got[1] == 4 && got[2] == -1 && got[3] == -3 && got[4] == 6 && got[5] == -1 && got[6] == -1);
  glUseProgram(0);
  glDeleteProgram(x);
  // overlapping offsets, and a stride too small, fail the link
  CHECK(build_stages("#version 440\nlayout(xfb_offset = 0) out vec2 a;\nlayout(xfb_offset = 4) out float b;\nvoid main(){ gl_Position = vec4(0.0); a = vec2(0.0); b = 0.0; }\n",
                     nullptr, nullptr, nullptr, fs) == 0);
  CHECK(build_stages("#version 440\nlayout(xfb_stride = 4) out;\nlayout(xfb_offset = 0) out vec2 a;\nvoid main(){ gl_Position = vec4(0.0); a = vec2(0.0); }\n",
                     nullptr, nullptr, nullptr, fs) == 0);

  // Vertex streams: stream 0 to buffer 0, stream 1 (twice per point) to buffer 1; the indexed queries count each stream, and glDrawTransformFeedbackStream draws one.
  const char* stream_names[] = {"s0", "gl_NextBuffer", "s1"};
  const GLuint m = build_stages("#version 400\nin vec4 pos;\nout float vid;\nvoid main(){ gl_Position = vec4(0.0); vid = pos.x; }\n", nullptr, nullptr,
                                "#version 400\nlayout(points) in;\nlayout(points, max_vertices = 2) out;\nin float vid[];\n"
                                "layout(stream = 0) out float s0;\nlayout(stream = 1) out float s1;\n"
                                "void main(){ s0 = 1.0 + vid[0]; EmitStreamVertex(0); s1 = 10.0 + vid[0]; EmitStreamVertex(1); s1 = 20.0 + vid[0]; EmitStreamVertex(1); }\n",
                                fs, stream_names, 3);
  CHECK(m != 0);
  attrib(m);
  glUseProgram(m);
  reset_floats(tfb[0], 8);
  reset_floats(tfb[1], 8);
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfb[0]);
  glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 1, tfb[1]);
  glBeginQueryIndexed(GL_PRIMITIVES_GENERATED, 1, queries[0]);
  glBeginQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 0, queries[1]);
  glBeginTransformFeedback(GL_POINTS);
  glDrawArrays(GL_POINTS, 1, 2);
  glEndTransformFeedback();
  glEndQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 0);
  glEndQueryIndexed(GL_PRIMITIVES_GENERATED, 1);
  CHECK(glGetError() == GL_NO_ERROR);
  read_floats(tfb[0], got, 3);
  CHECK(got[0] == 2 && got[1] == 3 && got[2] == -1);
  read_floats(tfb[1], got, 5);
  CHECK(got[0] == 11 && got[1] == 21 && got[2] == 12 && got[3] == 22 && got[4] == -1);
  glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &count);
  CHECK(count == 4);
  glGetQueryObjectuiv(queries[1], GL_QUERY_RESULT, &count);
  CHECK(count == 2);
  glUseProgram(0);
  glDeleteProgram(m);
  const GLuint d1 = build_stages(vs, nullptr, nullptr, nullptr, fs);
  attrib(d1);
  glUseProgram(d1);
  glBeginQuery(GL_PRIMITIVES_GENERATED, queries[0]);
  glDrawTransformFeedbackStream(GL_POINTS, 0, 1);
  glEndQuery(GL_PRIMITIVES_GENERATED);
  glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &count);
  CHECK(count == 4);
  glUseProgram(0);
  glDeleteProgram(d1);
  // several streams need points out; an output's stream outside a geometry shader is an error
  CHECK(build_stages("#version 400\nvoid main(){ gl_Position = vec4(0.0); }\n", nullptr, nullptr,
                     "#version 400\nlayout(points) in;\nlayout(line_strip, max_vertices = 2) out;\nlayout(stream = 1) out float s1;\nvoid main(){ s1 = 1.0; EmitStreamVertex(1); }\n",
                     fs) == 0);
  CHECK(glGetError() == GL_NO_ERROR);
  glDisable(GL_RASTERIZER_DISCARD);
  glDeleteQueries(2, queries);
  glDeleteBuffers(2, tfb);
  glDeleteBuffers(1, &vbo);
  glDeleteVertexArrays(1, &vao);
  CHECK(glGetError() == GL_NO_ERROR);
}

// The compatibility varyings through the geometry and tessellation stages (gl_in[].gl_FrontColor in, gl_FrontColor and gl_TexCoord[] out), the fixed-function
// fragment stage standing in after them, and vertex colour clamping (GL_CLAMP_VERTEX_COLOR).
static void test_compat_stage_varyings() {
  Fixture f;
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glClearColor(0, 0, 0, 1);
  // vertex colour in, the geometry stage passes it on and adds a texture coordinate the fragment shader reads
  const char* vs = "#version 150 compatibility\nvoid main(){ gl_Position = gl_Vertex; gl_FrontColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";
  const char* gs = "#version 150 compatibility\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
                   "void main(){ for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position; gl_FrontColor = gl_in[i].gl_FrontColor; gl_TexCoord[2] = vec4(0.5); EmitVertex(); } }\n";
  const char* fs = "#version 110\nvoid main(){ gl_FragColor = vec4(gl_TexCoord[2].x, gl_Color.g, 0.0, 1.0); }\n";
  const GLuint p = build_stages(vs, nullptr, nullptr, gs, fs);
  CHECK(p != 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(p);
  glRectf(-1, -1, 1, 1);
  CHECK(near(pixel(32, 32), 128, 255, 0));
  glUseProgram(0);
  glDeleteProgram(p);
  // no fragment shader: the fixed-function one shows the evaluation stage's gl_FrontColor
  const char* tes = "#version 150 compatibility\n#extension GL_ARB_tessellation_shader : require\nlayout(triangles) in;\n"
                    "void main(){ gl_Position = gl_TessCoord.x * gl_in[0].gl_Position + gl_TessCoord.y * gl_in[1].gl_Position + gl_TessCoord.z * gl_in[2].gl_Position;\n"
                    " gl_FrontColor = gl_in[0].gl_FrontColor * 0.0 + vec4(0.0, 0.0, 1.0, 1.0); }\n";
  const GLuint t = build_stages(vs, nullptr, tes, nullptr, nullptr);
  CHECK(t != 0);
  const float ones[4] = {1, 1, 1, 1};
  glPatchParameterfv(GL_PATCH_DEFAULT_OUTER_LEVEL, ones);
  glPatchParameterfv(GL_PATCH_DEFAULT_INNER_LEVEL, ones);
  glPatchParameteri(GL_PATCH_VERTICES, 3);
  GLuint vao = 0, vbo = 0;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  const float tri[12] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1};
  glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(4, GL_FLOAT, 0, nullptr);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(t);
  glDrawArrays(GL_PATCHES, 0, 3);
  CHECK(glGetError() == GL_NO_ERROR);
  CHECK(near(pixel(10, 10), 0, 0, 255) && near(pixel(60, 60), 0, 0, 0));
  glUseProgram(0);
  glDeleteProgram(t);
  glDisableClientState(GL_VERTEX_ARRAY);
  glDeleteBuffers(1, &vbo);
  glDeleteVertexArrays(1, &vao);
  // clamping: a colour of 2 is 1 when it reaches the fragment stage, unless turned off
  const GLuint c = build("#version 110\nvoid main(){ gl_Position = gl_Vertex; gl_FrontColor = vec4(2.0, -1.0, 0.5, 1.0); }\n",
                         "#version 110\nvoid main(){ gl_FragColor = gl_Color * 0.5 + vec4(0.0, 0.5, 0.0, 0.5); }\n");
  CHECK(c != 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(c);
  glRectf(-1, -1, 1, 1);
  CHECK(near(pixel(32, 32), 128, 128, 64, 2));
  glClampColor(GL_CLAMP_VERTEX_COLOR, GL_FALSE);
  GLint clamping = 1;
  glGetIntegerv(GL_CLAMP_VERTEX_COLOR, &clamping);
  CHECK(clamping == GL_FALSE);
  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-1, -1, 1, 1);
  CHECK(near(pixel(32, 32), 255, 0, 64, 2));
  glClampColor(GL_CLAMP_VERTEX_COLOR, GL_TRUE);
  glUseProgram(0);
  glDeleteProgram(c);
  // two-sided colour: a back-facing rectangle shows gl_BackColor only with GL_VERTEX_PROGRAM_TWO_SIDE; through the vertex stage, then a geometry stage
  const char* two_fs = "#version 110\nvoid main(){ gl_FragColor = gl_Color + gl_SecondaryColor; }\n";
  const GLuint two_vs = build("#version 110\nvoid main(){ gl_Position = gl_Vertex; gl_FrontColor = vec4(1.0, 0.0, 0.0, 1.0); gl_BackColor = vec4(0.0, 0.0, 1.0, 1.0);\n"
                              " gl_FrontSecondaryColor = vec4(0.0); gl_BackSecondaryColor = vec4(0.0, 0.5, 0.0, 0.0); }\n", two_fs);
  const GLuint two_gs = build_stages("#version 150 compatibility\nvoid main(){ gl_Position = gl_Vertex; }\n", nullptr, nullptr,
                                     "#version 150 compatibility\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
                                     "void main(){ for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position; gl_FrontColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
                                     " gl_BackColor = vec4(0.0, 0.0, 1.0, 1.0); gl_FrontSecondaryColor = vec4(0.0); gl_BackSecondaryColor = vec4(0.0, 0.5, 0.0, 0.0); EmitVertex(); } }\n",
                                     two_fs);
  CHECK(two_vs != 0 && two_gs != 0);
  const GLuint twos[2] = {two_vs, two_gs};
  for (const GLuint two : twos) {
    glUseProgram(two);
    glClear(GL_COLOR_BUFFER_BIT);
    glRectf(1, -1, -1, 1);      // (clockwise: the back faces the viewer)
    CHECK(near(pixel(32, 32), 255, 0, 0));
    glEnable(GL_VERTEX_PROGRAM_TWO_SIDE);
    glClear(GL_COLOR_BUFFER_BIT);
    glRectf(1, -1, -1, 1);
    CHECK(near(pixel(32, 32), 0, 128, 255, 2));
    glClear(GL_COLOR_BUFFER_BIT);
    glRectf(-1, -1, 1, 1);      // (counter-clockwise: the front)
    CHECK(near(pixel(32, 32), 255, 0, 0));
    glDisable(GL_VERTEX_PROGRAM_TWO_SIDE);
  }
  glUseProgram(0);
  glDeleteProgram(two_vs);
  glDeleteProgram(two_gs);
  CHECK(glGetError() == GL_NO_ERROR);
}

// Clip distances: gl_ClipDistance (enabled by GL_CLIP_DISTANCEi), a compatibility shader's gl_ClipVertex against the user planes, and gl_Position when it writes neither.
static void test_clip_distances() {
  Fixture f;
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  const char* fs = "#version 130\nvoid main(){ gl_FragColor = vec4(1.0); }\n";
  // the left half (x < 0) clipped by distance 0; distance 1 written but its plane not enabled
  const GLuint d = build("#version 130\nvoid main(){ gl_Position = gl_Vertex; gl_ClipDistance[0] = gl_Vertex.x; gl_ClipDistance[1] = -1.0; }\n", fs);
  CHECK(d != 0);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(d);
  glEnable(GL_CLIP_DISTANCE0);
  glRectf(-1, -1, 1, 1);
  CHECK(near(pixel(10, 32), 0, 0, 0) && near(pixel(54, 32), 255, 255, 255));
  glDisable(GL_CLIP_DISTANCE0);      // nothing clipped
  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-1, -1, 1, 1);
  CHECK(near(pixel(10, 32), 255, 255, 255));
  glUseProgram(0);
  glDeleteProgram(d);
  // gl_ClipVertex against the plane y >= 0 (eye space; the modelview is the identity)
  const double plane[4] = {0, 1, 0, 0};
  glClipPlane(GL_CLIP_PLANE0, plane);
  glEnable(GL_CLIP_PLANE0);
  const GLuint v = build("#version 120\nvoid main(){ gl_Position = gl_Vertex; gl_ClipVertex = gl_Vertex; }\n", "#version 120\nvoid main(){ gl_FragColor = vec4(1.0); }\n");
  CHECK(v != 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(v);
  glRectf(-1, -1, 1, 1);
  CHECK(near(pixel(32, 10), 0, 0, 0) && near(pixel(32, 54), 255, 255, 255));
  glUseProgram(0);
  glDeleteProgram(v);
  // neither written: gl_Position is compared with the plane
  const GLuint n = build("#version 120\nvoid main(){ gl_Position = gl_Vertex; }\n", "#version 120\nvoid main(){ gl_FragColor = vec4(1.0); }\n");
  CHECK(n != 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(n);
  glRectf(-1, -1, 1, 1);
  CHECK(near(pixel(32, 10), 0, 0, 0) && near(pixel(32, 54), 255, 255, 255));
  glUseProgram(0);
  glDeleteProgram(n);
  // a fragment shader alone: the fixed-function stand-in clips in eye space
  const GLuint s = glCreateProgram();
  const GLuint sf = glCreateShader(GL_FRAGMENT_SHADER);
  const char* src = "#version 120\nvoid main(){ gl_FragColor = vec4(1.0); }\n";
  glShaderSource(sf, 1, &src, nullptr);
  glCompileShader(sf);
  glAttachShader(s, sf);
  glLinkProgram(s);
  GLint ok = 0;
  glGetProgramiv(s, GL_LINK_STATUS, &ok);
  CHECK(ok);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(s);
  glRectf(-1, -1, 1, 1);
  CHECK(near(pixel(32, 10), 0, 0, 0) && near(pixel(32, 54), 255, 255, 255));
  glUseProgram(0);
  glDeleteProgram(s);
  glDeleteShader(sf);
  glDisable(GL_CLIP_PLANE0);
  CHECK(glGetError() == GL_NO_ERROR);
}

static void test_framebuffer_object() {
  Fixture f;
  GLuint fbo = 0, tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  glViewport(0, 0, 16, 16);
  glClearColor(1, 0.5f, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, W, H);
  glEnable(GL_TEXTURE_2D);
  glColor3f(1, 1, 1);
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0); glVertex2f(0, 0); glTexCoord2f(1, 0); glVertex2f(64, 0); glTexCoord2f(1, 1); glVertex2f(64, 64); glTexCoord2f(0, 1); glVertex2f(0, 64);
  glEnd();
  CHECK(near(pixel(30, 30), 255, 128, 0, 2));
  CHECK(glGetError() == GL_NO_ERROR);
}

static void test_evaluator() {
  Fixture f;
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  // a flat patch over the whole window
  const float ctrl[2][2][3] = {{{0, 0, 0}, {0, 64, 0}}, {{64, 0, 0}, {64, 64, 0}}};
  glMap2f(GL_MAP2_VERTEX_3, 0, 1, 6, 2, 0, 1, 3, 2, &ctrl[0][0][0]);
  glEnable(GL_MAP2_VERTEX_3);
  glMapGrid2f(4, 0, 1, 4, 0, 1);
  glColor3f(0, 1, 0);
  glEvalMesh2(GL_FILL, 0, 4, 0, 4);
  CHECK(near(pixel(30, 30), 0, 255, 0));
  GLint order[2] = {};
  glGetMapiv(GL_MAP2_VERTEX_3, GL_ORDER, order);
  CHECK(order[0] == 2 && order[1] == 2);
  CHECK(glGetError() == GL_NO_ERROR);
}

static void test_core_profile() {
  Fixture f(true);
  glBegin(GL_TRIANGLES);
  CHECK(glGetError() == GL_INVALID_OPERATION);      // removed from the core profile
  GLint profile = 0;
  glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
  CHECK(profile == GL_CONTEXT_CORE_PROFILE_BIT);
  const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, 0));
  CHECK(e && strncmp(e, "GL_", 3) == 0);
}

// wglShareLists on a context that already has state: it keeps it and sees the other context's objects.
static void test_join_group() {
  Fixture first;
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glClear(GL_COLOR_BUFFER_BIT);
  glEndList();
  {
    Fixture second;
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glEnable(GL_LIGHTING);
    CHECK(!glIsTexture(tex) && !glIsList(list));
    second.api.join_group(first.group);
    CHECK(glIsTexture(tex) && glIsList(list));
    float c[4] = {};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, c);
    CHECK(c[0] == 0.25f && c[2] == 0.75f);
    CHECK(glIsEnabled(GL_LIGHTING));
    glCallList(list);
    CHECK(near(pixel(1, 1), 64, 128, 191, 2));
    CHECK(glGetError() == GL_NO_ERROR);
  }
  g_api = &first.api;
  CHECK(glIsTexture(tex));
}

static void test_service() {
  GlService service(g_alloc);
  ContextRequest r;
  uint32_t a = 0, b = 0;
  CHECK(service.create_context(1, r, &a) == GlStatus::Ok);
  r.share = a;
  CHECK(service.create_context(1, r, &b) == GlStatus::Ok && b != a);
  SurfaceRequest s;
  s.window = 0x100;
  s.width = 8;
  s.height = 8;
  CHECK(service.make_current(1, 10, a, s) == GlStatus::Ok);
  CHECK(service.make_current(1, 11, a, s) == GlStatus::Busy);
  // a stream through the service: clear the window red
  g_stream.used = 0;
  uint32_t words[64];
  uint32_t n = 0;
  // (the service's own encoder is the client's: build the stream from the entry points, then hand it over)
  g_api = nullptr;
  glClearColor(1, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  for (uint32_t i = 0; i < g_stream.used; ++i) words[n++] = g_stream.words[i];
  g_stream.used = 0;
  uint64_t result = 0;
  CHECK(service.execute(1, 10, ConstByteView(reinterpret_cast<const uint8_t*>(words), n * 4), g_memory, &result) == GlStatus::Ok);
  CHECK(service.execute(1, 12, ConstByteView(reinterpret_cast<const uint8_t*>(words), n * 4), g_memory, &result) == GlStatus::NoContext);
  struct Seen { uint32_t w = 0, h = 0; uint8_t first[4] = {}; } seen;
  CHECK(service.swap(1, 0x100, 8, 8, true, [](void* c, uint64_t, ConstByteView rgba, uint32_t w, uint32_t h) {
    Seen& s = *static_cast<Seen*>(c);
    s.w = w; s.h = h;
    for (int i = 0; i < 4; ++i) s.first[i] = rgba[static_cast<uint64_t>(i)];
  }, &seen) == GlStatus::Ok);
  CHECK(seen.w == 8 && seen.first[0] == 255 && seen.first[1] == 0);
  uint8_t front[8 * 8 * 4];
  uint32_t w = 0, h = 0;
  CHECK(service.read_front(1, 0x100, ByteView(front, sizeof(front)), 32, &w, &h) == GlStatus::Ok);
  CHECK(front[0] == 0 && front[2] == 255);      // B, G, R, A
  // a current context with state of its own takes another's objects and keeps its state
  uint32_t c = 0;
  r.share = 0;
  CHECK(service.create_context(1, r, &c) == GlStatus::Ok);
  CHECK(service.make_current(1, 10, c, s) == GlStatus::Ok);
  CHECK(service.share_objects(1, a, c) == GlStatus::Ok);
  CHECK(service.context_count(1) == 3);
  CHECK(service.delete_context(1, c) == GlStatus::Ok);
  CHECK(service.context_count(1) == 2);
  CHECK(service.delete_context(1, b) == GlStatus::Ok);
  CHECK(service.delete_context(1, b) == GlStatus::InvalidHandle);
  service.process_exited(1);
  CHECK(service.context_count(1) == 0);
}

// A stream that makes no sense is stopped, not run off the end.
static void test_bad_streams() {
  Fixture f;
  g_memory.closed = true;
  uint32_t junk[32];
  uint32_t seed = 12345;
  for (int round = 0; round < 2000; ++round) {
    for (uint32_t& w : junk) { seed = seed * 1103515245u + 12345u; w = seed; }
    // keep the opcodes of commands that read memory out: only scalar commands may be tried blind
    const uint32_t n = 1 + seed % 32;
    g_api->execute(ConstByteView(reinterpret_cast<const uint8_t*>(junk), uint64_t{n} * 4 - (seed % 4)), g_memory);
  }
  // truncated real commands
  glClearColor(0, 1, 0, 1);
  const uint32_t len = g_stream.used;
  for (uint32_t cut = 0; cut < len * 4; ++cut) g_api->execute(ConstByteView(reinterpret_cast<const uint8_t*>(g_stream.words), cut), g_memory);
  g_stream.used = 0;
  g_memory.closed = false;
  glGetError();
  CHECK(true);
}

int main() {
  test_strings_and_errors();
  test_clear_and_immediate();
  test_polygon_and_modes();
  test_depth_and_blend();
  test_lighting();
  test_textures();
  test_arrays();
  test_display_lists();
  test_pixels();
  test_state_stacks_and_get();
  test_selection_and_feedback();
  test_glsl_program();
  test_desktop_glsl();
  test_several_shaders_per_stage();
  test_geometry_and_tessellation();
  test_clip_distances();
  test_transform_feedback_stages();
  test_compat_stage_varyings();
  test_framebuffer_object();
  test_evaluator();
  test_core_profile();
  test_join_group();
  test_service();
  test_bad_streams();
  printf("opengl: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
