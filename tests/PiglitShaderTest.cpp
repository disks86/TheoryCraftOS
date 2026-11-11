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

// piglit's shader_runner for the kernel's OpenGL: runs the .shader_test files of piglit (the Mesa project's OpenGL test suite, fetched by tests/fetch_piglit.sh, not vendored)
// against GlApi through the generated client entry points, on a 250 x 250 double-buffered surface with a 3.3 compatibility context, and compares with the expected failures in
// tests/data/piglit/shader_test.xfail. A test is skipped when it asks for what the context does not have (a GLSL or GL version above 3.30, an extension it does not report,
// GLSL ES, geometry or tessellation stages) or uses a shader_runner command this runner does not know (the command is named in the summary).
//
//   PiglitShaderTest <piglit dir> [--update-xfail] [--filter substring] [--verbose]
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/asan_interface.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "OpenGl/GlApi.hpp"
#include "OpenGl/GlSurface.hpp"
#include "OpenGl/GlClient.h"
#include "OpenGl/generated/GlEnums.inc"

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return aligned_alloc(16, (size + 15) & ~uint64_t{15}); }
  void deallocate(void* p) override { free(p); }
};

struct HostMemory final : opengl::IClientMemory {
  std::vector<void*> blocks;
  bool read(uint64_t address, ByteView out) override {
    if (!address || __asan_region_is_poisoned(reinterpret_cast<void*>(address), out.size())) return false;
    memcpy(out.data(), reinterpret_cast<const void*>(address), out.size());
    return true;
  }
  bool write(uint64_t address, ConstByteView data) override {
    if (!address || __asan_region_is_poisoned(reinterpret_cast<void*>(address), data.size())) return false;
    memcpy(reinterpret_cast<void*>(address), data.data(), data.size());
    return true;
  }
  uint64_t allocate(uint64_t bytes) override { void* p = calloc(1, bytes); blocks.push_back(p); return reinterpret_cast<uint64_t>(p); }
  void release(uint64_t address, uint64_t) override {
    auto it = std::find(blocks.begin(), blocks.end(), reinterpret_cast<void*>(address));
    if (it != blocks.end()) { free(*it); blocks.erase(it); }
  }
  ~HostMemory() { for (void* p : blocks) free(p); }
};

static TestAllocator g_alloc;
static HostMemory g_memory;
static opengl::GlApi* g_api = nullptr;
static GlStream g_stream;

GlStream* gl_stream_current(void) { return &g_stream; }
uint64_t gl_stream_submit(const uint32_t* words, uint32_t count) {
  if (!g_api) return 0;
  return g_api->execute(ConstByteView(reinterpret_cast<const uint8_t*>(words), uint64_t{count} * 4), g_memory);
}

#include "OpenGl/generated/GlEntries.inc"
GLenum glGetError(void) { return tcos_glGetError(); }
void glFlush(void) { tcos_glFlush(); }
void glFinish(void) { tcos_glFinish(); }

namespace {

struct EnumName { const char* name; uint64_t value; };
const EnumName kEnums[] = {
#include "OpenGl/generated/GlEnumNames.inc"
};

bool enum_value(const std::string& name, uint32_t* out) {
  for (const EnumName& e : kEnums) if (name == e.name) { *out = static_cast<uint32_t>(e.value); return true; }
  char* end = nullptr;
  const unsigned long v = strtoul(name.c_str(), &end, 0);
  if (end && *end == 0 && end != name.c_str()) { *out = static_cast<uint32_t>(v); return true; }
  return false;
}

constexpr int kWidth = 250, kHeight = 250;

// A float as shader_runner reads one: a hexadecimal value is the bit pattern of the float.
float parse_float(const std::string& s) {
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    const uint32_t bits = static_cast<uint32_t>(strtoul(s.c_str(), nullptr, 16));
    float f;
    memcpy(&f, &bits, 4);
    return f;
  }
  return strtof(s.c_str(), nullptr);
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

std::vector<std::string> words(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '(' || c == ')' || c == ',' || c == '\r') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
    else cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// GL's version numbers as an integer: "1.20" -> 120, "3.3" -> 330.
int version_number(const std::string& v) {
  int major = 0, minor = 0;
  if (sscanf(v.c_str(), "%d.%d", &major, &minor) < 1) return 0;
  std::string after = v.substr(v.find('.') + 1);
  if (after.size() == 1) minor *= 10;
  return major * 100 + minor;
}

struct Outcome {
  enum Kind { Pass, Fail, Skip } kind = Pass;
  std::string why;
};

struct Test {
  std::vector<std::string> vs, fs;      // one per [vertex shader] / [fragment shader] section: several are linked into one stage
  std::vector<std::string> gs, tcs, tes;
  std::string vertex_data;
  bool passthrough = false;
  std::vector<std::string> commands;
  std::vector<std::string> needs;
  bool has_gs = false, has_tess = false, has_compute = false;
};

bool load(const std::string& path, Test* t) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  std::string text;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  fclose(f);
  std::string section, *body = nullptr;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(at, end - at);
    at = end + 1;
    const std::string tl = trim(line);
    if (!tl.empty() && tl[0] == '[' && tl.back() == ']') {
      section = tl;
      body = nullptr;
      if (section == "[vertex shader]") { t->vs.emplace_back(); body = &t->vs.back(); }
      else if (section == "[fragment shader]") { t->fs.emplace_back(); body = &t->fs.back(); }
      else if (section == "[vertex shader passthrough]") t->passthrough = true;
      else if (section == "[vertex data]") body = &t->vertex_data;
      else if (section == "[geometry shader]") { t->gs.emplace_back(); body = &t->gs.back(); }
      else if (section == "[tessellation control shader]") { t->tcs.emplace_back(); body = &t->tcs.back(); }
      else if (section == "[tessellation evaluation shader]") { t->tes.emplace_back(); body = &t->tes.back(); }
      else if (section.find("geometry") != std::string::npos) t->has_gs = true;      // (SPIR-V, passthrough ...)
      else if (section.find("tessellation") != std::string::npos) t->has_tess = true;
      else if (section.find("compute") != std::string::npos) t->has_compute = true;
      continue;
    }
    if (body) { *body += line; *body += "\n"; continue; }
    if (section == "[require]") { if (!tl.empty() && tl[0] != '#') t->needs.push_back(tl); continue; }
    if (section == "[test]") {
      if (tl.empty() || tl[0] == '#') continue;
      const size_t hash = tl.find('#');
      t->commands.push_back(trim(hash == std::string::npos ? tl : tl.substr(0, hash)));
    }
  }
  return true;
}

struct Runner {
  opengl::ShareGroup group{g_alloc};
  opengl::GlSurface surface;
  opengl::GlApi api;
  GLuint program = 0;
  std::string log;
  float tolerance[4] = {0.01f, 0.01f, 0.01f, 0.01f};
  std::vector<GLuint> buffers;
  std::vector<GLuint> block_buffers;

  // A uniform in a block: its bytes go to the block's buffer at the offset the GL reports.
  bool set_block_uniform(const std::string& name, const std::string& type, const std::vector<float>& f, const std::vector<int>& iv, std::string* why) {
    std::string base = name;
    int element = 0;
    const size_t br = name.find('[');
    if (br != std::string::npos && name.back() == ']') { base = name.substr(0, br); element = atoi(name.c_str() + br + 1); }
    const char* names[1] = {base.c_str()};
    GLuint index = GL_INVALID_INDEX;
    glGetUniformIndices(program, 1, names, &index);
    if (index == GL_INVALID_INDEX) return true;      // (unused: piglit only warns)
    GLint block = -1, offset = 0, array_stride = 0, matrix_stride = 0;
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_BLOCK_INDEX, &block);
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_OFFSET, &offset);
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_ARRAY_STRIDE, &array_stride);
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_MATRIX_STRIDE, &matrix_stride);
    if (block < 0 || static_cast<size_t>(block) >= block_buffers.size()) { *why = "uniform block member without a block"; return false; }
    offset += element * array_stride;
    const bool integer = type[0] == 'i' || type[0] == 'u' || type[0] == 'b';
    std::vector<uint8_t> bytes;
    const auto put = [&](size_t at, uint32_t word) { if (bytes.size() < at + 4) bytes.resize(at + 4); memcpy(&bytes[at], &word, 4); };
    if (type.compare(0, 3, "mat") == 0) {
      const int cols = type[3] - '0', rows = type.size() > 5 ? type[5] - '0' : cols;
      for (int c = 0; c < cols; ++c)
        for (int r = 0; r < rows; ++r) {
          uint32_t w;
          const float v = static_cast<size_t>(c * rows + r) < f.size() ? f[static_cast<size_t>(c * rows + r)] : 0.0f;
          memcpy(&w, &v, 4);
          put(static_cast<size_t>(c * matrix_stride + r * 4), w);
        }
    } else {
      for (size_t k = 0; k < f.size(); ++k) {
        uint32_t w;
        if (integer) w = static_cast<uint32_t>(iv[k]);
        else memcpy(&w, &f[k], 4);
        put(k * 4, w);
      }
    }
    glBindBuffer(GL_UNIFORM_BUFFER, block_buffers[static_cast<size_t>(block)]);
    glBufferSubData(GL_UNIFORM_BUFFER, offset, static_cast<GLsizeiptr>(bytes.size()), bytes.data());
    return true;
  }
  GLuint vao_attribs_mask = 0;

  static opengl::SurfaceConfig surface_config() { opengl::SurfaceConfig c; c.width = kWidth; c.height = kHeight; c.accum_bits = 0; return c; }
  static opengl::ContextConfig context_config() { opengl::ContextConfig c; c.major = 3; c.minor = 3; return c; }
  Runner() : surface(g_alloc, surface_config()), api(g_alloc, group, context_config()) {
    g_api = &api;
    api.bind_surface(&surface);
  }
  ~Runner() {
    gl_stream_flush();
    g_api = nullptr;
  }

  bool compile(GLenum type, const std::string& src, GLuint* out, std::string* why) {
    const GLuint s = glCreateShader(type);
    const char* text = src.c_str();
    glShaderSource(s, 1, &text, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char buf[1024] = {};
      glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
      const char* stage = type == GL_VERTEX_SHADER ? "vs" : type == GL_FRAGMENT_SHADER ? "fs" : type == GL_GEOMETRY_SHADER ? "gs" : type == GL_TESS_CONTROL_SHADER ? "tcs" : "tes";
      *why = std::string(stage) + " compile: " + buf;
      return false;
    }
    *out = s;
    return true;
  }

  Outcome build(const Test& t, bool expect_link_failure) {
    Outcome o;
    program = glCreateProgram();
    GLuint s = 0;
    std::vector<std::string> vs = t.vs;
    if (t.passthrough) vs.assign(1, "#version 110\nattribute vec4 piglit_vertex;\nvoid main() { gl_Position = piglit_vertex; }\n");
    for (const std::string& src : vs) {
      if (!compile(GL_VERTEX_SHADER, src, &s, &o.why)) { if (expect_link_failure) return o; o.kind = Outcome::Fail; return o; }
      glAttachShader(program, s);
    }
    const struct { const std::vector<std::string>* list; GLenum type; } stages[] = {
        {&t.tcs, GL_TESS_CONTROL_SHADER}, {&t.tes, GL_TESS_EVALUATION_SHADER}, {&t.gs, GL_GEOMETRY_SHADER}, {&t.fs, GL_FRAGMENT_SHADER}};
    for (const auto& st : stages)
      for (const std::string& src : *st.list) {
        if (!compile(st.type, src, &s, &o.why)) { if (expect_link_failure) return o; o.kind = Outcome::Fail; return o; }
        glAttachShader(program, s);
      }
    glBindAttribLocation(program, 0, "piglit_vertex");
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
      char buf[1024] = {};
      glGetProgramInfoLog(program, sizeof(buf), nullptr, buf);
      if (expect_link_failure) return o;
      o.kind = Outcome::Fail;
      o.why = std::string("link: ") + buf;
      return o;
    }
    if (expect_link_failure) { o.kind = Outcome::Fail; o.why = "linked but should not have"; return o; }
    glUseProgram(program);
    // As shader_runner does: a buffer for each uniform block, at the binding of the block's index.
    GLint blocks = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
    for (GLint b = 0; b < blocks && b < 16; ++b) {
      GLint size = 0;
      glGetActiveUniformBlockiv(program, static_cast<GLuint>(b), GL_UNIFORM_BLOCK_DATA_SIZE, &size);
      GLuint buf = 0;
      glGenBuffers(1, &buf);
      glBindBuffer(GL_UNIFORM_BUFFER, buf);
      std::vector<uint8_t> zeros(static_cast<size_t>(size > 0 ? size : 16));
      glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(zeros.size()), zeros.data(), GL_DYNAMIC_DRAW);
      glUniformBlockBinding(program, static_cast<GLuint>(b), static_cast<GLuint>(b));
      glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(b), buf);
      block_buffers.push_back(buf);
    }
    return o;
  }

  // piglit_draw_rect: through the program's piglit_vertex / piglit_texcoord when it has them, else the fixed-function arrays.
  void draw_rect(float x, float y, float w, float h, bool tex, float tx = 0, float ty = 0, float tw = 1, float th = 1, bool patch = false) {
    const float verts[16] = {x, y, 0, 1, x + w, y, 0, 1, x, y + h, 0, 1, x + w, y + h, 0, 1};
    const float tc[8] = {tx, ty, tx + tw, ty, tx, ty + th, tx + tw, ty + th};
    const GLint vloc = program ? glGetAttribLocation(program, "piglit_vertex") : -1;
    const GLint tloc = program ? glGetAttribLocation(program, "piglit_texcoord") : -1;
    if (vloc >= 0) { glVertexAttribPointer(vloc, 4, GL_FLOAT, GL_FALSE, 0, verts); glEnableVertexAttribArray(vloc); }
    else { glVertexPointer(4, GL_FLOAT, 0, verts); glEnableClientState(GL_VERTEX_ARRAY); }
    if (tex || tloc >= 0) {
      if (tloc >= 0) { glVertexAttribPointer(tloc, 2, GL_FLOAT, GL_FALSE, 0, tc); glEnableVertexAttribArray(tloc); }
      else { glTexCoordPointer(2, GL_FLOAT, 0, tc); glEnableClientState(GL_TEXTURE_COORD_ARRAY); }
    }
    if (patch) {      // (piglit_draw_rect_custom with patches: the four corners as one patch)
      glPatchParameteri(GL_PATCH_VERTICES, 4);
      glDrawArrays(GL_PATCHES, 0, 4);
    } else {
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (vloc >= 0) glDisableVertexAttribArray(vloc); else glDisableClientState(GL_VERTEX_ARRAY);
    if (tloc >= 0) glDisableVertexAttribArray(tloc); else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  }

  bool probe(int x, int y, int w, int h, const float* expected, int comps, std::string* why) {
    std::vector<float> px(static_cast<size_t>(w) * h * 4);
    glReadPixels(x, y, w, h, GL_RGBA, GL_FLOAT, px.data());
    for (int j = 0; j < h; ++j)
      for (int i = 0; i < w; ++i) {
        const float* p = &px[(static_cast<size_t>(j) * w + i) * 4];
        for (int k = 0; k < comps; ++k) {
          const float d = p[k] - expected[k];
          if (d > tolerance[k] || d < -tolerance[k]) {
            char buf[256];
            snprintf(buf, sizeof(buf), "probe at (%d,%d) expected (%g %g %g %g) got (%g %g %g %g)", x + i, y + j, expected[0], expected[1], expected[2], comps > 3 ? expected[3] : 1.0f, p[0], p[1], p[2], p[3]);
            *why = buf;
            return false;
          }
        }
      }
    return true;
  }

  bool set_uniform(const std::vector<std::string>& w, std::string* why) {
    // uniform TYPE NAME values...
    if (w.size() < 3) { *why = "bad uniform"; return false; }
    const std::string& type = w[1];
    const GLint loc = glGetUniformLocation(program, w[2].c_str());
    std::vector<float> f;
    std::vector<int> iv;
    for (size_t i = 3; i < w.size(); ++i) { f.push_back(parse_float(w[i])); iv.push_back(static_cast<int>(strtol(w[i].c_str(), nullptr, 0))); }
    if (loc < 0) return set_block_uniform(w[2], type, f, iv, why);
    const auto need = [&](size_t n) { if (f.size() < n) { *why = "uniform needs values"; return false; } return true; };
    if (type == "float") { if (!need(1)) return false; glUniform1fv(loc, 1, f.data()); }
    else if (type == "vec2") { if (!need(2)) return false; glUniform2fv(loc, 1, f.data()); }
    else if (type == "vec3") { if (!need(3)) return false; glUniform3fv(loc, 1, f.data()); }
    else if (type == "vec4") { if (!need(4)) return false; glUniform4fv(loc, 1, f.data()); }
    else if (type == "int" || type == "bool" || type == "sampler2D" || type == "sampler1D" || type == "sampler3D" || type == "samplerCube" || type == "sampler2DRect" || type == "sampler2DShadow" || type == "sampler1DShadow") { if (!need(1)) return false; glUniform1iv(loc, 1, iv.data()); }
    else if (type == "ivec2" || type == "bvec2") { if (!need(2)) return false; glUniform2iv(loc, 1, iv.data()); }
    else if (type == "ivec3" || type == "bvec3") { if (!need(3)) return false; glUniform3iv(loc, 1, iv.data()); }
    else if (type == "ivec4" || type == "bvec4") { if (!need(4)) return false; glUniform4iv(loc, 1, iv.data()); }
    else if (type == "uint") { if (!need(1)) return false; const GLuint u = static_cast<GLuint>(strtoul(w[3].c_str(), nullptr, 0)); glUniform1uiv(loc, 1, &u); }
    else if (type == "uvec2" || type == "uvec3" || type == "uvec4") {
      const size_t n = static_cast<size_t>(type[4] - '0');
      if (!need(n)) return false;
      GLuint u[4];
      for (size_t i = 0; i < n; ++i) u[i] = static_cast<GLuint>(strtoul(w[3 + i].c_str(), nullptr, 0));
      if (n == 2) glUniform2uiv(loc, 1, u); else if (n == 3) glUniform3uiv(loc, 1, u); else glUniform4uiv(loc, 1, u);
    }
    else if (type.compare(0, 3, "mat") == 0) {
      const int cols = type[3] - '0', rows = type.size() > 5 ? type[5] - '0' : cols;
      if (!need(static_cast<size_t>(cols * rows))) return false;
      if (cols == 2 && rows == 2) glUniformMatrix2fv(loc, 1, GL_FALSE, f.data());
      else if (cols == 3 && rows == 3) glUniformMatrix3fv(loc, 1, GL_FALSE, f.data());
      else if (cols == 4 && rows == 4) glUniformMatrix4fv(loc, 1, GL_FALSE, f.data());
      else if (cols == 2 && rows == 3) glUniformMatrix2x3fv(loc, 1, GL_FALSE, f.data());
      else if (cols == 2 && rows == 4) glUniformMatrix2x4fv(loc, 1, GL_FALSE, f.data());
      else if (cols == 3 && rows == 2) glUniformMatrix3x2fv(loc, 1, GL_FALSE, f.data());
      else if (cols == 3 && rows == 4) glUniformMatrix3x4fv(loc, 1, GL_FALSE, f.data());
      else if (cols == 4 && rows == 2) glUniformMatrix4x2fv(loc, 1, GL_FALSE, f.data());
      else glUniformMatrix4x3fv(loc, 1, GL_FALSE, f.data());
    }
    else { *why = "skip: uniform type " + type; return false; }
    return true;
  }

  // A texture piglit makes: rgbw (quadrants red, green, blue, white, bottom left first), checkerboard, or one colour.
  void make_texture(int unit, GLenum target, int w, int h, const std::vector<float>& rgba) {
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(target, tex);
    glTexImage2D(target, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_FLOAT, rgba.data());
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);      // (as piglit_rgbw_texture and piglit_checkerboard_texture make them)
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEnable(target);
    glActiveTexture(GL_TEXTURE0);
  }

  // Draws from [vertex data]: a header of name/type/size per column, then rows.
  bool draw_arrays(const Test& t, GLenum mode, int first, int count, std::string* why) {
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < t.vertex_data.size()) {
      size_t e = t.vertex_data.find('\n', at);
      if (e == std::string::npos) e = t.vertex_data.size();
      std::string l = trim(t.vertex_data.substr(at, e - at));
      at = e + 1;
      if (!l.empty() && l[0] != '#') lines.push_back(l);
    }
    if (lines.empty()) {      // (no vertex data: the shaders make their vertices from gl_VertexID; an empty vertex array object, as shader_runner binds)
      GLuint vao = 0;
      glGenVertexArrays(1, &vao);
      glBindVertexArray(vao);
      glDrawArrays(mode, first, count);
      glBindVertexArray(0);
      glDeleteVertexArrays(1, &vao);
      return true;
    }
    struct Column { std::string name; int comps; };
    std::vector<Column> cols;
    for (const std::string& c : words(lines[0])) {
      const size_t s1 = c.find('/'), s2 = c.rfind('/');
      Column col;
      col.name = c.substr(0, s1);
      const std::string kind = c.substr(s2 + 1);
      col.comps = kind == "float" || kind == "int" ? 1 : (kind.size() > 3 && kind[kind.size() - 1] >= '1' && kind[kind.size() - 1] <= '4') ? kind[kind.size() - 1] - '0' : atoi(kind.c_str());
      if (col.comps < 1) col.comps = 1;
      cols.push_back(col);
    }
    std::vector<std::vector<float>> data(cols.size());
    for (size_t r = 1; r < lines.size(); ++r) {
      const std::vector<std::string> w = words(lines[r]);
      size_t k = 0;
      for (size_t c = 0; c < cols.size(); ++c)
        for (int i = 0; i < cols[c].comps; ++i) data[c].push_back(k < w.size() ? parse_float(w[k++]) : 0.0f);
    }
    std::vector<GLint> enabled;
    for (size_t c = 0; c < cols.size(); ++c) {
      if (cols[c].name == "gl_Vertex") { glVertexPointer(cols[c].comps, GL_FLOAT, 0, data[c].data()); glEnableClientState(GL_VERTEX_ARRAY); continue; }
      if (cols[c].name == "gl_Color") { glColorPointer(cols[c].comps, GL_FLOAT, 0, data[c].data()); glEnableClientState(GL_COLOR_ARRAY); continue; }
      if (cols[c].name == "gl_Normal") { glNormalPointer(GL_FLOAT, 0, data[c].data()); glEnableClientState(GL_NORMAL_ARRAY); continue; }
      const GLint loc = glGetAttribLocation(program, cols[c].name.c_str());
      if (loc < 0) continue;
      glVertexAttribPointer(loc, cols[c].comps, GL_FLOAT, GL_FALSE, 0, data[c].data());
      glEnableVertexAttribArray(loc);
      enabled.push_back(loc);
    }
    glDrawArrays(mode, first, count);
    for (GLint l : enabled) glDisableVertexAttribArray(l);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    return true;
  }

  Outcome run(const Test& t) {
    Outcome o;
    bool expect_link_failure = false;
    for (const std::string& c : t.commands) if (c == "link error") expect_link_failure = true;
    o = build(t, expect_link_failure);
    if (o.kind != Outcome::Pass || expect_link_failure) return o;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    for (const std::string& cmd : t.commands) {
      const std::vector<std::string> w = words(cmd);
      if (w.empty()) continue;
      std::string why;
      const auto f = [&](size_t i) { return i < w.size() ? parse_float(w[i]) : 0.0f; };
      bool relative = w[0] == "relative";
      const size_t b = relative ? 1 : 0;
      if (cmd == "link success" || cmd == "link error") continue;
      if (w[0] == "clear" && w.size() == 1) { glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); continue; }
      if (w[0] == "clear" && w.size() >= 6 && w[1] == "color") { glClearColor(f(2), f(3), f(4), f(5)); continue; }
      if (w[0] == "clear" && w.size() >= 3 && w[1] == "depth") { glClearDepth(f(2)); continue; }
      if (w[0] == "ortho" && w.size() == 1) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, kWidth, 0, kHeight, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        continue;
      }
      if (w[0] == "draw" && w.size() >= 2 && w[1] == "rect") {
        size_t i = 2;
        bool ortho = false, tex = false, patch = false;
        while (i < w.size() && (w[i] == "ortho" || w[i] == "tex" || w[i] == "patch")) {
          if (w[i] == "ortho") ortho = true;
          else if (w[i] == "patch") patch = true;
          else tex = true;
          ++i;
        }
        float x = f(i), y = f(i + 1), ww = f(i + 2), hh = f(i + 3);
        if (ortho) { x = x * 2 / kWidth - 1; y = y * 2 / kHeight - 1; ww = ww * 2 / kWidth; hh = hh * 2 / kHeight; }
        if (tex) draw_rect(x, y, ww, hh, true, f(i + 4), f(i + 5), f(i + 6), f(i + 7));
        else draw_rect(x, y, ww, hh, false, 0, 0, 1, 1, patch);
        continue;
      }
      if (w[0] == "draw" && w.size() >= 5 && w[1] == "arrays") {
        uint32_t mode = 0;
        if (!enum_value(w[2], &mode)) { o.kind = Outcome::Skip; o.why = "skip: draw arrays mode " + w[2]; return o; }
        if (!draw_arrays(t, mode, atoi(w[3].c_str()), atoi(w[4].c_str()), &why)) { o.kind = Outcome::Fail; o.why = why; return o; }
        continue;
      }
      if (w.size() > b + 1 && w[b] == "probe") {
        // probe [all|rect] rgb|rgba ...
        size_t i = b + 1;
        bool all = false, rect = false;
        if (w[i] == "all") { all = true; ++i; }
        else if (w[i] == "rect") { rect = true; ++i; }
        if (i >= w.size() || (w[i] != "rgb" && w[i] != "rgba")) { o.kind = Outcome::Skip; o.why = "skip: probe " + w[i < w.size() ? i : b]; return o; }
        const int comps = w[i] == "rgba" ? 4 : 3;
        ++i;
        int x = 0, y = 0, rw = 1, rh = 1;
        if (all) { rw = kWidth; rh = kHeight; }
        else if (rect) {
          const float fx = f(i), fy = f(i + 1), fw = f(i + 2), fh = f(i + 3);
          if (relative) { x = static_cast<int>(fx * kWidth); y = static_cast<int>(fy * kHeight); rw = static_cast<int>(fw * kWidth); rh = static_cast<int>(fh * kHeight); }
          else { x = static_cast<int>(fx); y = static_cast<int>(fy); rw = static_cast<int>(fw); rh = static_cast<int>(fh); }
          i += 4;
        } else {
          const float fx = f(i), fy = f(i + 1);
          if (relative) { x = static_cast<int>(fx * kWidth); y = static_cast<int>(fy * kHeight); }
          else { x = static_cast<int>(fx); y = static_cast<int>(fy); }
          i += 2;
        }
        float expected[4] = {f(i), f(i + 1), f(i + 2), comps == 4 ? f(i + 3) : 1.0f};
        if (x >= kWidth) x = kWidth - 1;
        if (y >= kHeight) y = kHeight - 1;
        if (!probe(x, y, rw, rh, expected, comps, &why)) { o.kind = Outcome::Fail; o.why = why; return o; }
        continue;
      }
      if (w[0] == "uniform") {
        if (!set_uniform(w, &why)) { o.kind = why.compare(0, 5, "skip:") == 0 ? Outcome::Skip : Outcome::Fail; o.why = why; return o; }
        continue;
      }
      if ((w[0] == "enable" || w[0] == "disable") && w.size() >= 2) {
        uint32_t cap = 0;
        if (!enum_value(w[1], &cap)) { o.kind = Outcome::Skip; o.why = "skip: enum " + w[1]; return o; }
        if (w[0] == "enable") glEnable(cap); else glDisable(cap);
        continue;
      }
      if (w[0] == "texture" && w.size() >= 5 && (w[1] == "rgbw" || w[1] == "checkerboard")) {
        const int unit = atoi(w[2].c_str());
        size_t i = w[1] == "checkerboard" ? 4 : 3;
        const int tw = atoi(w[i].c_str()), th = atoi(w[i + 1].c_str());
        if (tw <= 0 || th <= 0 || tw > 4096 || th > 4096) { o.kind = Outcome::Skip; o.why = "skip: texture size"; return o; }
        std::vector<float> rgba(static_cast<size_t>(tw) * th * 4);
        for (int yy = 0; yy < th; ++yy)
          for (int xx = 0; xx < tw; ++xx) {
            float* p = &rgba[(static_cast<size_t>(yy) * tw + xx) * 4];
            if (w[1] == "rgbw") {
              const bool right = xx >= tw / 2, top = yy >= th / 2;
              const float colours[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}};
              const int q = (top ? 2 : 0) + (right ? 1 : 0);
              for (int k = 0; k < 4; ++k) p[k] = colours[q][k];
            } else {
              const bool odd = ((xx / 8) + (yy / 8)) & 1;      // (piglit's checkerboard: 8x8 cells)
              for (int k = 0; k < 4; ++k) p[k] = odd ? f(i + 2 + 4 + static_cast<size_t>(k)) : f(i + 2 + static_cast<size_t>(k));
            }
          }
        make_texture(unit, GL_TEXTURE_2D, tw, th, rgba);
        continue;
      }
      if (w[0] == "ortho" && w.size() >= 5) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(f(1), f(2), f(3), f(4), -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        continue;
      }
      if (w[0] == "color" && w.size() >= 5) { glColor4f(f(1), f(2), f(3), f(4)); continue; }
      if (w[0] == "texture" && w.size() >= 3 && w[1] == "miptree") {
        // piglit_miptree_texture: 8x8 down to 1x1, each level one colour of the wheel (red, green, blue, white)
        const int unit = atoi(w[2].c_str());
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        const float wheel[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}};
        for (int level = 0; level < 4; ++level) {
          const int size = 8 >> level;
          std::vector<float> data(static_cast<size_t>(size) * size * 4);
          for (int i = 0; i < size * size; ++i) memcpy(&data[static_cast<size_t>(i) * 4], wheel[level], 16);
          glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, size, size, 0, GL_RGBA, GL_FLOAT, data.data());
        }
        glEnable(GL_TEXTURE_2D);
        glActiveTexture(GL_TEXTURE0);
        continue;
      }
      if (w[0] == "texture" && w.size() >= 4 && (w[1] == "shadow2D" || w[1] == "shadow1D" || w[1] == "shadowRect")) {
        // piglit_depth_texture: depth x / (w - 1) across, compared GL_GREATER, nearest, clamped
        const int unit = atoi(w[2].c_str());
        const int tw = atoi(w[3].c_str()), th = w[1] == "shadow1D" ? 1 : (w.size() >= 5 ? atoi(w[4].c_str()) : 1);
        if (tw <= 1 || th <= 0 || tw > 4096 || th > 4096) { o.kind = Outcome::Skip; o.why = "skip: depth texture size"; return o; }
        const GLenum target = w[1] == "shadow1D" ? GL_TEXTURE_1D : (w[1] == "shadowRect" ? GL_TEXTURE_RECTANGLE : GL_TEXTURE_2D);
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(target, tex);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        std::vector<float> depth(static_cast<size_t>(tw) * th);
        for (int y = 0; y < th; ++y) for (int x = 0; x < tw; ++x) depth[static_cast<size_t>(y) * tw + x] = static_cast<float>(x) / static_cast<float>(tw - 1);
        if (target == GL_TEXTURE_1D) glTexImage1D(target, 0, GL_DEPTH_COMPONENT, tw, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
        else glTexImage2D(target, 0, GL_DEPTH_COMPONENT, tw, th, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
        glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
        glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC, GL_GREATER);
        if (target != GL_TEXTURE_RECTANGLE) glEnable(target);
        glActiveTexture(GL_TEXTURE0);
        continue;
      }
      if (w[0] == "texparameter" && w.size() >= 4) {
        uint32_t target = GL_TEXTURE_2D, pname = 0, value = 0;
        if (w[1] == "2D") target = GL_TEXTURE_2D; else if (w[1] == "Rect") target = GL_TEXTURE_RECTANGLE; else if (w[1] == "3D") target = GL_TEXTURE_3D; else if (w[1] == "Cube") target = GL_TEXTURE_CUBE_MAP; else if (w[1] == "1D") target = GL_TEXTURE_1D;
        const std::string p = w[2];
        if (p == "min") pname = GL_TEXTURE_MIN_FILTER; else if (p == "mag") pname = GL_TEXTURE_MAG_FILTER; else if (p == "wrap_s") pname = GL_TEXTURE_WRAP_S; else if (p == "wrap_t") pname = GL_TEXTURE_WRAP_T;
        else if (p == "wrap_r") pname = GL_TEXTURE_WRAP_R; else if (p == "compare_mode") pname = GL_TEXTURE_COMPARE_MODE; else if (p == "compare_func") pname = GL_TEXTURE_COMPARE_FUNC; else if (p == "depth_mode") pname = GL_DEPTH_TEXTURE_MODE;
        else { o.kind = Outcome::Skip; o.why = "skip: texparameter " + p; return o; }
        const std::string v = w[3];
        if (v == "nearest") value = GL_NEAREST; else if (v == "linear") value = GL_LINEAR; else if (v == "clamp_to_edge") value = GL_CLAMP_TO_EDGE; else if (v == "repeat") value = GL_REPEAT;
        else if (v == "mirrored_repeat") value = GL_MIRRORED_REPEAT; else if (v == "clamp_to_border") value = GL_CLAMP_TO_BORDER; else if (v == "compare_r_to_texture") value = GL_COMPARE_R_TO_TEXTURE;
        else if (v == "none") value = GL_NONE; else if (v == "less") value = GL_LESS; else if (v == "greater") value = GL_GREATER; else if (v == "lequal") value = GL_LEQUAL; else if (v == "gequal") value = GL_GEQUAL;
        else if (v == "equal") value = GL_EQUAL; else if (v == "notequal") value = GL_NOTEQUAL; else if (v == "always") value = GL_ALWAYS; else if (v == "never") value = GL_NEVER;
        else if (v == "intensity") value = GL_INTENSITY; else if (v == "luminance") value = GL_LUMINANCE; else if (v == "alpha") value = GL_ALPHA; else if (v == "red") value = GL_RED;
        else { o.kind = Outcome::Skip; o.why = "skip: texparameter value " + v; return o; }
        glTexParameteri(target, pname, static_cast<GLint>(value));
        continue;
      }
      if (w[0] == "tolerance" && w.size() >= 5) { for (int k = 0; k < 4; ++k) tolerance[k] = f(1 + static_cast<size_t>(k)); continue; }
      if (w[0] == "frustum" && w.size() >= 7) {
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(f(1), f(2), f(3), f(4), f(5), f(6)); glMatrixMode(GL_MODELVIEW);
        continue;
      }
      if (w[0] == "clip" && w.size() >= 7 && w[1] == "plane") {
        const double eq[4] = {f(3), f(4), f(5), f(6)};
        glClipPlane(GL_CLIP_PLANE0 + static_cast<GLenum>(atoi(w[2].c_str())), eq);
        continue;
      }
      if (w[0] == "shade" && w.size() >= 3) { glShadeModel(w[2] == "flat" ? GL_FLAT : GL_SMOOTH); continue; }
      if (w[0] == "hint") continue;
      if (w[0] == "patch" && w.size() >= 4 && w[1] == "parameter" && w[2] == "vertices") { glPatchParameteri(GL_PATCH_VERTICES, atoi(w[3].c_str())); continue; }
      if (w[0] == "patch" && w.size() >= 6 && w[1] == "parameter" && w[2] == "default" && w[3] == "level") {
        float v[4] = {};
        const bool outer = w[4] == "outer";
        for (size_t k = 0; k < (outer ? 4u : 2u) && 5 + k < w.size(); ++k) v[k] = static_cast<float>(atof(w[5 + k].c_str()));
        glPatchParameterfv(outer ? GL_PATCH_DEFAULT_OUTER_LEVEL : GL_PATCH_DEFAULT_INNER_LEVEL, v);
        continue;
      }
      o.kind = Outcome::Skip;
      o.why = "skip: command " + w[0] + (w.size() > 1 ? " " + w[1] : "");
      return o;
    }
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
      char buf[64];
      snprintf(buf, sizeof(buf), "GL error 0x%x", e);
      o.kind = Outcome::Fail;
      o.why = buf;
    }
    return o;
  }
};

// What the test asks of the context: Skip with the reason when the context does not have it.
bool supported(const Test& t, const char* extensions, std::string* why) {
  if (t.has_gs) { *why = "skip: geometry shader"; return false; }
  if (t.has_tess) { *why = "skip: tessellation"; return false; }
  if (t.has_compute) { *why = "skip: compute"; return false; }
  for (const std::string& r : t.needs) {
    const std::vector<std::string> w = words(r);
    if (w.empty()) continue;
    if (w[0] == "GLSL" && w.size() >= 3 && w[1] == "ES") { *why = "skip: GLSL ES"; return false; }
    if (w[0] == "GL" && w.size() >= 2 && w[1] == "ES") { *why = "skip: GL ES"; return false; }
    if ((w[0] == "GLSL" || w[0] == "GL") && w.size() >= 3) {
      const int v = version_number(w[2]);
      if ((w[1] == ">=" || w[1] == ">") && v > 330) { *why = "skip: " + w[0] + " " + w[2]; return false; }
      continue;
    }
    if (w[0] == "GL" && w.size() >= 2 && w[1] == "COMPAT") continue;
    if (w[0] == "GL" && w.size() >= 2 && w[1] == "CORE") continue;
    if (w[0].compare(0, 3, "GL_") == 0) {
      if (w[0] == "GL_ARB_compatibility") continue;
      const std::string want = " " + w[0] + " ";
      const std::string have = std::string(" ") + (extensions ? extensions : "") + " ";
      if (have.find(want) == std::string::npos) { *why = "skip: " + w[0]; return false; }
      continue;
    }
    if (w[0] == "SIZE" || w[0] == "rlimit" || w[0] == "fbo" || w[0] == "depthbuffer") continue;
    if (w[0].find("MAX_") != std::string::npos || w[0].compare(0, 3, "INT") == 0) continue;
    *why = "skip: require " + w[0];
    return false;
  }
  return true;
}

void collect(const std::string& dir, std::vector<std::string>* out) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    const std::string path = dir + "/" + name;
    if (e->d_type == DT_DIR) collect(path, out);
    else if (name.size() > 12 && name.compare(name.size() - 12, 12, ".shader_test") == 0) out->push_back(path);
  }
  closedir(d);
}

}  // namespace

// --accel-survey: an accelerator that takes every program the translator makes but never a clear, so the CPU keeps the picture (the results are the CPU's) while
// each draw is still offered to it: a draw it would have taken ends as "the device picture is out of date", any other reason is what keeps it off the 3D hardware.
struct SurveyAccel final : webgl::IAccelerator {
  const char* name() const override { return "survey"; }
  webgl::AccelLimits limits() const override { return webgl::AccelLimits{256, 16, 14, 16, true, true, true, true}; }
  uint32_t create_program(const char*, const char*, uint32_t, uint32_t, uint32_t) override { return ++programs; }
  uint32_t create_program_stages(const char*, const char*, uint32_t, uint32_t, uint32_t, const webgl::AccelStages&) override { return ++programs; }
  void destroy_program(uint32_t) override {}
  uint32_t create_texture(uint32_t, uint32_t) override { return 1; }
  bool write_texture(uint32_t, ConstByteView) override { return true; }
  uint32_t create_target(uint32_t, uint32_t) override { return 1; }
  void destroy_target(uint32_t) override {}
  bool clear(uint32_t, bool, const float*, bool, float) override { return false; }
  bool draw(uint32_t, uint32_t, const webgl::AccelState&, ConstByteView, uint32_t, ConstByteView) override { return false; }
  bool read_color(uint32_t, ByteView) override { return false; }
  uint32_t programs = 0;
};

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: %s <piglit dir> [--update-xfail] [--filter s] [--verbose] [--trace]\n", argv[0]); return 2; }
  const std::string root = argv[1];
  bool update = false, verbose = false, trace = false, survey = false;
  std::string filter;
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--update-xfail")) update = true;
    else if (!strcmp(argv[i], "--verbose")) verbose = true;
    else if (!strcmp(argv[i], "--trace")) trace = true;
    else if (!strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
    else if (!strcmp(argv[i], "--accel-survey")) survey = true;
  }
  SurveyAccel survey_accel;
  if (survey) webgl::set_accelerator(&survey_accel);
  std::map<std::string, int> draw_reasons;
  int survey_tests = 0, survey_all_gpu = 0;
  // The suites of the GLSL versions the context has.
  const char* const kDirs[] = {"tests/spec/glsl-1.10", "tests/spec/glsl-1.20", "tests/spec/glsl-1.30", "tests/spec/glsl-1.40", "tests/spec/glsl-1.50", "tests/spec/glsl-3.30", "tests/spec/arb_tessellation_shader", "tests/shaders"};
  std::vector<std::string> files;
  for (const char* d : kDirs) collect(root + "/" + d, &files);
  std::sort(files.begin(), files.end());
  std::map<std::string, std::string> xfail;
  const std::string xfail_path = "tests/data/piglit/shader_test.xfail";
  if (FILE* f = fopen(xfail_path.c_str(), "r")) {
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
      std::string l = trim(line);
      if (!l.empty() && l.back() == '\n') l.pop_back();
      if (l.empty() || l[0] == '#') continue;
      const size_t sp = l.find(' ');
      xfail[l.substr(0, sp)] = sp == std::string::npos ? "" : l.substr(sp + 1);
    }
    fclose(f);
  }
  std::map<std::string, int> pass, fail, skip;
  std::map<std::string, int> reasons, skip_reasons;
  std::vector<std::pair<std::string, std::string>> failures;
  int unexpected = 0, fixed = 0;
  std::string extensions;
  for (const std::string& path : files) {
    const std::string rel = path.substr(root.size() + 1);
    if (!filter.empty() && rel.find(filter) == std::string::npos) continue;
    // (rel is "tests/spec/glsl-1.10/..." or "tests/shaders/...": the suite is what follows "tests/" up to the version directory)
    const size_t second = rel.find('/', 6);
    std::string suite = rel.compare(0, 13, "tests/shaders") == 0 ? std::string("shaders") : rel.substr(6, rel.find('/', second + 1) - 6);
    Test t;
    if (!load(path, &t)) continue;
    if (trace) { fprintf(stderr, "running %s\n", rel.c_str()); fflush(stderr); }
    Outcome o;
    {
      Runner r;
      if (extensions.empty()) { const char* e = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)); extensions = e ? e : ""; }
      std::string why;
      if (!supported(t, extensions.c_str(), &why)) { o.kind = Outcome::Skip; o.why = why; }
      else o = r.run(t);
      if (survey && o.kind != Outcome::Skip) {
        const webgl::GlContext::HardwareStats& st = r.api.hardware_stats();
        bool all = st.cpu_draws > 0;
        for (const auto& reason : st.reasons) {
          if (!reason.why) continue;
          draw_reasons[reason.why] += static_cast<int>(reason.count);
          if (const char* want = getenv("SURVEY_REASON"); want && strstr(reason.why, want)) printf("  survey: %s: %s\n", rel.c_str(), reason.why);
          if (strcmp(reason.why, "the device picture is out of date") != 0) all = false;
        }
        ++survey_tests;
        if (all) ++survey_all_gpu;
      }
    }
    if (o.kind == Outcome::Skip) { ++skip[suite]; ++skip_reasons[o.why]; continue; }
    if (o.kind == Outcome::Pass) {
      ++pass[suite];
      if (xfail.count(rel)) { ++fixed; if (verbose || !update) printf("now passes (remove from xfail): %s\n", rel.c_str()); }
      continue;
    }
    ++fail[suite];
    std::string key = o.why.substr(0, 60);
    ++reasons[key];
    failures.push_back({rel, o.why});
    if (!xfail.count(rel)) { ++unexpected; if (!update) printf("FAILED %s: %s\n", rel.c_str(), o.why.c_str()); }
    else if (verbose) printf("xfail %s: %s\n", rel.c_str(), o.why.c_str());
  }
  int tp = 0, tf = 0, ts = 0;
  for (const char* d : kDirs) {
    const std::string s = std::string(d).substr(6);
    printf("%-22s pass %4d  fail %4d  skip %4d\n", s.c_str(), pass[s], fail[s], skip[s]);
    tp += pass[s]; tf += fail[s]; ts += skip[s];
  }
  printf("piglit shader_test: %d passed, %d failed (%d not in the xfail list), %d skipped, %d xfail entries now pass\n", tp, tf, unexpected, ts, fixed);
  if (survey) {
    std::vector<std::pair<int, std::string>> top;
    for (auto& r : draw_reasons) top.push_back({r.second, r.first});
    std::sort(top.rbegin(), top.rend());
    printf("accelerator survey: %d of %d tests would draw only on the 3D hardware; draws by what decided (\"the device picture is out of date\" = taken):\n", survey_all_gpu, survey_tests);
    for (size_t i = 0; i < top.size() && i < 40; ++i) printf("  %6d  %s\n", top[i].first, top[i].second.c_str());
  }
  if (verbose) {
    std::vector<std::pair<int, std::string>> top;
    for (auto& r : reasons) top.push_back({r.second, r.first});
    std::sort(top.rbegin(), top.rend());
    printf("top failure reasons:\n");
    for (size_t i = 0; i < top.size() && i < 40; ++i) printf("  %4d  %s\n", top[i].first, top[i].second.c_str());
    std::vector<std::pair<int, std::string>> st;
    for (auto& r : skip_reasons) st.push_back({r.second, r.first});
    std::sort(st.rbegin(), st.rend());
    printf("top skip reasons:\n");
    for (size_t i = 0; i < st.size() && i < 25; ++i) printf("  %4d  %s\n", st[i].first, st[i].second.c_str());
  }
  if (update) {
    FILE* f = fopen(xfail_path.c_str(), "w");
    if (!f) { printf("cannot write %s\n", xfail_path.c_str()); return 1; }
    fprintf(f, "# piglit .shader_test files that tests/PiglitShaderTest.cpp expects to fail (path, then why). Regenerate with --update-xfail after fixing things.\n");
    for (auto& fl : failures) {
      std::string why = fl.second;
      for (char& c : why) if (c == '\n' || c == '\r') c = ' ';
      fprintf(f, "%s %s\n", fl.first.c_str(), why.substr(0, 160).c_str());
    }
    fclose(f);
    return 0;
  }
  return unexpected || fixed ? 1 : 0;
}
