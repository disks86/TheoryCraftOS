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

#include "Desktop/GlCubeRenderer.hpp"
#include "Kernel/Delay.hpp"
#include "OpenGl/generated/GlEnums.inc"
#include "OpenGl/generated/GlOpIds.inc"
#include "Utility/AllocatedView.hpp"

namespace desktop {

using graphics::ColoredVertex;
using graphics::TexturedVertex;

namespace {

constexpr uint64_t kWindow = 0x3D;   // the renderer's one surface (the GL names surfaces by the window they belong to)
constexpr uint64_t kThread = 1;      // the context is current on "thread" 1 of the renderer's process: only the render thread draws

// The same shaders as the other two APIs' (the built-in ones, CubeShaders/*), in desktop GLSL.
constexpr char kColoredVertex[] =
    "#version 330\n"
    "in vec3 position;\n"
    "in vec4 color;\n"
    "uniform mat4 mvp;\n"
    "out vec4 v_color;\n"
    "void main() { gl_Position = mvp * vec4(position, 1.0); v_color = color; }\n";
constexpr char kColoredFragment[] =
    "#version 330\n"
    "in vec4 v_color;\n"
    "out vec4 out_color;\n"
    "void main() { out_color = v_color; }\n";
constexpr char kTexturedVertex[] =
    "#version 330\n"
    "in vec3 position;\n"
    "in vec2 uv;\n"
    "uniform mat4 mvp;\n"
    "out vec2 v_uv;\n"
    "void main() { gl_Position = mvp * vec4(position, 1.0); v_uv = uv; }\n";
constexpr char kTexturedFragment[] =
    "#version 330\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D logo;\n"
    "out vec4 out_color;\n"
    "void main() { out_color = texture(logo, v_uv); }\n";

// Names the GL reads as C strings: with their terminating zero.
template <uint64_t N>
ConstByteView c_string(const char (&text)[N]) {
  return StringView(text, N).bytes();
}
constexpr char kPosition[] = "position";
constexpr char kColor[] = "color";
constexpr char kUv[] = "uv";
constexpr char kMvp[] = "mvp";
constexpr char kLogo[] = "logo";

uint64_t now_us() { return hardware::uptime_us(); }

}  // namespace

// --- The program's memory ---------------------------------------------------------------------

uint64_t GlCubeRenderer::ClientMemory::add(ConstByteView bytes) {
  if (count_ == kMaxRegions || bytes.empty()) return 0;
  regions_[count_] = {bytes, ByteView()};
  return (uint64_t{++count_}) * kSpacing;
}

uint64_t GlCubeRenderer::ClientMemory::add_writable(ByteView bytes) {
  if (count_ == kMaxRegions || bytes.empty()) return 0;
  regions_[count_] = {ConstByteView(bytes), bytes};
  return (uint64_t{++count_}) * kSpacing;
}

bool GlCubeRenderer::ClientMemory::find(uint64_t address, uint64_t size, uint32_t* region, uint64_t* offset) const {
  const uint64_t index = address / kSpacing;
  if (index == 0 || index > count_) return false;
  const Region& r = regions_[index - 1];
  const uint64_t at = address % kSpacing;
  if (at > r.bytes.size() || size > r.bytes.size() - at) return false;
  *region = static_cast<uint32_t>(index - 1);
  *offset = at;
  return true;
}

bool GlCubeRenderer::ClientMemory::read(uint64_t address, ByteView out) {
  uint32_t region = 0;
  uint64_t offset = 0;
  if (!find(address, out.size(), &region, &offset)) return false;
  return out.copy_from(regions_[region].bytes.subview(offset, out.size()));
}

bool GlCubeRenderer::ClientMemory::write(uint64_t address, ConstByteView data) {
  uint32_t region = 0;
  uint64_t offset = 0;
  if (!find(address, data.size(), &region, &offset) || regions_[region].writable.empty()) return false;
  return regions_[region].writable.subview(offset, data.size()).copy_from(data);
}

// --- The command stream -------------------------------------------------------------------------

// The words of GlStream.hpp, written as opengl32.dll's generated entry points write them: the header (opcode, length in words), then one word per value and two (low
// first) for an address or a GLintptr / GLsizeiptr.
class GlCubeRenderer::Commands {
 public:
  struct Arg {
    uint64_t value;
    bool wide;
  };
  static Arg u(uint32_t v) { return {v, false}; }
  static Arg i(int32_t v) { return {static_cast<uint32_t>(v), false}; }
  static Arg f(float v) { return {__builtin_bit_cast(uint32_t, v), false}; }
  static Arg p(uint64_t v) { return {v, true}; }

  explicit Commands(allocators::IAllocator& allocator) : words_(allocator) {}

  template <class... Args>
  void op(uint32_t code, Args... args) {
    uint32_t length = 1;
    ((length += args.wide ? 2u : 1u), ...);
    ok_ = ok_ && words_.put32(code | length << 16);
    (put(args), ...);
  }
  bool ok() const { return ok_; }
  ConstByteView view() const { return words_.view(); }
  void clear() {
    words_.clear();
    ok_ = true;
  }

 private:
  void put(Arg a) { ok_ = ok_ && words_.put32(static_cast<uint32_t>(a.value)) && (!a.wide || words_.put32(static_cast<uint32_t>(a.value >> 32))); }
  ByteBuffer words_;
  bool ok_ = true;
};

// Runs what is queued in the renderer's context and returns the last result (what the library's synchronous entry points return).
uint64_t GlCubeRenderer::submit(Commands& commands, bool* ok) {
  uint64_t result = 0;
  if (!commands.ok() || commands.view().size() > opengl::kMaxStreamBytes) *ok = false;
  else if (!commands.view().empty() && service_.execute(pid_, kThread, commands.view(), memory_, &result) != opengl::GlStatus::Ok) *ok = false;
  commands.clear();
  memory_.clear();
  return result;
}

// --- The renderer -----------------------------------------------------------------------------

GlCubeRenderer::GlCubeRenderer(allocators::IAllocator& allocator, opengl::GlService& service, uint64_t pid) : allocator_(allocator), service_(service), pid_(pid), log_(allocator) {}

GlCubeRenderer::~GlCubeRenderer() { close(); }

graphics::Mat4 GlCubeRenderer::transform(float angle, float aspect) {
  // (CubeRenderer::transform without the turn to the devices' y-down clip space: GL's is y-up)
  return graphics::Mat4::perspective(0.9f, aspect, 0.1f, 20.0f) * graphics::Mat4::translation(0, 0, -4.5f) * graphics::Mat4::rotation_y(angle) *
         graphics::Mat4::rotation_x(0.6f * angle);
}

void GlCubeRenderer::close() {
  // (everything the process has in the GL - the context, its objects, the surface - goes with it, as when a program ends)
  if (context_ || open_) service_.process_exited(pid_);
  context_ = 0;
  open_ = false;
  release_view(allocator_, picture_.pixels);
  picture_ = graphics::Surface();
  width_ = height_ = 0;
  colored_program_ = textured_program_ = 0;
  colored_mvp_ = textured_mvp_ = -1;
  colored_vertices_ = textured_vertices_ = indices_ = logo_ = 0;
  texture_available_ = last_textured_ = false;
  memory_.clear();
}

bool GlCubeRenderer::build_program(StringView vertex, StringView fragment, bool textured, uint32_t* program, int32_t* mvp_location) {
  Commands c(allocator_);
  bool ok = true;
  uint32_t shaders[2] = {};
  const StringView sources[2] = {vertex, fragment};
  for (uint32_t s = 0; s < 2 && ok; ++s) {
    c.op(kOp_glCreateShader, C::u(s == 0 ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER));
    shaders[s] = static_cast<uint32_t>(submit(c, &ok));
    if (!ok || !shaders[s]) return false;
    // glShaderSource(shader, 1, &text, &length): the array of addresses and the array of lengths are memory of the program too.
    if (sources[s].size() > 0x7FFFFFFF) return false;
    uint64_t text[1] = {memory_.add(sources[s].bytes())};
    int32_t length[1] = {static_cast<int32_t>(sources[s].size())};
    int32_t compiled = 0;
    const uint64_t text_at = memory_.add(bytes_of(text)), length_at = memory_.add(bytes_of(length)), compiled_at = memory_.add_writable(bytes_of(compiled));
    c.op(kOp_glShaderSource, C::u(shaders[s]), C::i(1), C::p(text_at), C::p(length_at));
    c.op(kOp_glCompileShader, C::u(shaders[s]));
    c.op(kOp_glGetShaderiv, C::u(shaders[s]), C::u(GL_COMPILE_STATUS), C::p(compiled_at));
    submit(c, &ok);
    if (ok && !compiled) {
      uint8_t text_log[512] = {};
      int32_t written = 0;
      const uint64_t written_at = memory_.add_writable(bytes_of(written)), log_at = memory_.add_writable(ByteView(text_log));
      c.op(kOp_glGetShaderInfoLog, C::u(shaders[s]), C::i(sizeof text_log), C::p(written_at), C::p(log_at));
      submit(c, &ok);
      log_.clear();
      if (written > 0 && written <= static_cast<int32_t>(sizeof text_log)) log_.append(ConstByteView(text_log).first(static_cast<uint64_t>(written)));
      return false;
    }
  }
  if (!ok) return false;
  c.op(kOp_glCreateProgram);
  *program = static_cast<uint32_t>(submit(c, &ok));
  if (!ok || !*program) return false;
  int32_t linked = 0;
  const uint64_t position_at = memory_.add(c_string(kPosition)), second_at = memory_.add(textured ? c_string(kUv) : c_string(kColor)),
                 linked_at = memory_.add_writable(bytes_of(linked));
  c.op(kOp_glAttachShader, C::u(*program), C::u(shaders[0]));
  c.op(kOp_glAttachShader, C::u(*program), C::u(shaders[1]));
  // (the locations of the vertex layout every API uses: position 0, the color or the coordinates 1)
  c.op(kOp_glBindAttribLocation, C::u(*program), C::u(0), C::p(position_at));
  c.op(kOp_glBindAttribLocation, C::u(*program), C::u(1), C::p(second_at));
  c.op(kOp_glLinkProgram, C::u(*program));
  c.op(kOp_glGetProgramiv, C::u(*program), C::u(GL_LINK_STATUS), C::p(linked_at));
  c.op(kOp_glDeleteShader, C::u(shaders[0]));   // (they stay while the program has them)
  c.op(kOp_glDeleteShader, C::u(shaders[1]));
  submit(c, &ok);
  if (!ok || !linked) {
    uint8_t text_log[512] = {};
    int32_t written = 0;
    const uint64_t written_at = memory_.add_writable(bytes_of(written)), log_at = memory_.add_writable(ByteView(text_log));
    c.op(kOp_glGetProgramInfoLog, C::u(*program), C::i(sizeof text_log), C::p(written_at), C::p(log_at));
    submit(c, &ok);
    log_.clear();
    if (written > 0 && written <= static_cast<int32_t>(sizeof text_log)) log_.append(ConstByteView(text_log).first(static_cast<uint64_t>(written)));
    return false;
  }
  c.op(kOp_glGetUniformLocation, C::u(*program), C::p(memory_.add(c_string(kMvp))));
  *mvp_location = static_cast<int32_t>(submit(c, &ok));
  if (!ok || *mvp_location < 0) return false;
  if (textured) {
    c.op(kOp_glGetUniformLocation, C::u(*program), C::p(memory_.add(c_string(kLogo))));
    const int32_t sampler = static_cast<int32_t>(submit(c, &ok));
    if (!ok || sampler < 0) return false;
    c.op(kOp_glUseProgram, C::u(*program));
    c.op(kOp_glUniform1i, C::i(sampler), C::i(0));   // (texture unit 0)
    submit(c, &ok);
  }
  return ok;
}

bool GlCubeRenderer::create_buffers() {
  TexturedVertex textured[graphics::kCubeVertices];
  ColoredVertex colored[graphics::kCubeVertices];
  uint16_t indices[graphics::kCubeIndices];
  build_cube_geometry(ArrayView<ColoredVertex>(colored), ArrayView<TexturedVertex>(textured), ArrayView<uint16_t>(indices));
  Commands c(allocator_);
  bool ok = true;
  uint32_t names[3] = {};
  c.op(kOp_glGenBuffers, C::i(3), C::p(memory_.add_writable(bytes_of(names))));
  submit(c, &ok);
  if (!ok || !names[0] || !names[1] || !names[2]) return false;
  colored_vertices_ = names[0], textured_vertices_ = names[1], indices_ = names[2];
  const ConstByteView data[3] = {bytes_of(colored), bytes_of(textured), bytes_of(indices)};
  for (uint32_t k = 0; k < 3; ++k) {
    const uint32_t target = k == 2 ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    c.op(kOp_glBindBuffer, C::u(target), C::u(names[k]));
    c.op(kOp_glBufferData, C::u(target), C::p(data[k].size()), C::p(memory_.add(data[k])), C::u(GL_STATIC_DRAW));
    submit(c, &ok);
  }
  return ok;
}

// The logo texture: best effort, as on the other APIs.
bool GlCubeRenderer::create_logo() {
  ArrayView<uint32_t> texels = allocate_view<uint32_t>(allocator_, uint64_t{kCubeLogoSize} * kCubeLogoSize);
  ByteView bytes;
  bool ok = !texels.empty() && render_cube_logo(allocator_, texels) && texels.try_cast<uint8_t>(&bytes);
  if (ok) {
    Commands c(allocator_);
    uint32_t name[1] = {};
    c.op(kOp_glGenTextures, C::i(1), C::p(memory_.add_writable(bytes_of(name))));
    submit(c, &ok);
    ok = ok && name[0];
    if (ok) {
      logo_ = name[0];
      // (the words are 0xAARRGGBB: B, G, R, A bytes; the rows go top first and so does the texture's t, as the faces' coordinates expect)
      c.op(kOp_glActiveTexture, C::u(GL_TEXTURE0));
      c.op(kOp_glBindTexture, C::u(GL_TEXTURE_2D), C::u(logo_));
      c.op(kOp_glTexParameteri, C::u(GL_TEXTURE_2D), C::u(GL_TEXTURE_MIN_FILTER), C::i(GL_LINEAR));
      c.op(kOp_glTexParameteri, C::u(GL_TEXTURE_2D), C::u(GL_TEXTURE_MAG_FILTER), C::i(GL_LINEAR));
      c.op(kOp_glTexParameteri, C::u(GL_TEXTURE_2D), C::u(GL_TEXTURE_WRAP_S), C::i(GL_CLAMP_TO_EDGE));
      c.op(kOp_glTexParameteri, C::u(GL_TEXTURE_2D), C::u(GL_TEXTURE_WRAP_T), C::i(GL_CLAMP_TO_EDGE));
      c.op(kOp_glTexImage2D, C::u(GL_TEXTURE_2D), C::i(0), C::i(GL_RGBA8), C::i(kCubeLogoSize), C::i(kCubeLogoSize), C::i(0), C::u(GL_BGRA), C::u(GL_UNSIGNED_BYTE),
           C::p(memory_.add(ConstByteView(bytes))));
      submit(c, &ok);
    }
  }
  release_view(allocator_, texels);
  return ok;
}

bool GlCubeRenderer::open(uint32_t width, uint32_t height) {
  close();
  if (!width || !height) return false;
  opengl::ContextRequest request;   // (a compatibility context of the newest version, what wglCreateContext gives)
  opengl::SurfaceRequest surface;
  surface.window = kWindow;
  surface.width = width;
  surface.height = height;
  surface.double_buffered = true;
  surface.accum_bits = 0;
  if (service_.create_context(pid_, request, &context_) != opengl::GlStatus::Ok || service_.make_current(pid_, kThread, context_, surface) != opengl::GlStatus::Ok) {
    close();
    return false;
  }
  picture_.pixels = allocate_view<uint32_t>(allocator_, uint64_t{width} * height);
  picture_.width = width;
  picture_.height = height;
  picture_.pitch = width;
  picture_.format = graphics::Format::B8G8R8A8Unorm;
  width_ = width;
  height_ = height;
  log_.clear();
  Commands c(allocator_);
  bool ok = picture_.valid();
  c.op(kOp_glViewport, C::i(0), C::i(0), C::i(static_cast<int32_t>(width)), C::i(static_cast<int32_t>(height)));
  c.op(kOp_glEnable, C::u(GL_DEPTH_TEST));
  c.op(kOp_glDepthFunc, C::u(GL_LEQUAL));
  c.op(kOp_glEnable, C::u(GL_CULL_FACE));   // (a closed cube, as on the other APIs: the far faces are never rasterized)
  c.op(kOp_glCullFace, C::u(GL_BACK));
  c.op(kOp_glFrontFace, C::u(GL_CCW));
  c.op(kOp_glClearColor, C::f(0.063f), C::f(0.063f), C::f(0.102f), C::f(1.0f));
  submit(c, &ok);
  if (!ok || !build_program(StringView(kColoredVertex), StringView(kColoredFragment), false, &colored_program_, &colored_mvp_) || !create_buffers()) {
    close();
    return false;
  }
  texture_available_ = build_program(StringView(kTexturedVertex), StringView(kTexturedFragment), true, &textured_program_, &textured_mvp_) && create_logo();
  open_ = true;
  return true;
}

// The frame as the window gets it: the front buffer after the swap, top row first, 0xAARRGGBB.
bool GlCubeRenderer::read_front() {
  ByteView out;
  uint32_t w = 0, h = 0;
  return picture_.pixels.try_cast<uint8_t>(&out) && service_.read_front(pid_, kWindow, out, width_ * 4, &w, &h) == opengl::GlStatus::Ok && w == width_ && h == height_;
}

bool GlCubeRenderer::render(float angle, bool textured, CubeFrameTimes* times, bool) {
  if (!open_) return false;
  const bool use_logo = textured && texture_available_;
  last_textured_ = use_logo;
  const graphics::Mat4 m = transform(angle, static_cast<float>(width_) / static_cast<float>(height_));
  for (uint32_t k = 0; k < 16; ++k) mvp_[k] = m.m[k];
  Commands c(allocator_);
  bool ok = true;
  const uint64_t start = now_us();
  c.op(kOp_glClear, C::u(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
  c.op(kOp_glUseProgram, C::u(use_logo ? textured_program_ : colored_program_));
  c.op(kOp_glUniformMatrix4fv, C::i(use_logo ? textured_mvp_ : colored_mvp_), C::i(1), C::u(0), C::p(memory_.add(bytes_of(mvp_))));   // (columns, as Mat4 keeps them)
  c.op(kOp_glBindBuffer, C::u(GL_ARRAY_BUFFER), C::u(use_logo ? textured_vertices_ : colored_vertices_));
  const uint32_t stride = use_logo ? sizeof(TexturedVertex) : sizeof(ColoredVertex);
  c.op(kOp_glVertexAttribPointer, C::u(0), C::i(3), C::u(GL_FLOAT), C::u(0), C::i(static_cast<int32_t>(stride)), C::p(0));
  c.op(kOp_glVertexAttribPointer, C::u(1), C::i(use_logo ? 2 : 4), C::u(GL_FLOAT), C::u(0), C::i(static_cast<int32_t>(stride)), C::p(3 * sizeof(float)));
  c.op(kOp_glEnableVertexAttribArray, C::u(0));
  c.op(kOp_glEnableVertexAttribArray, C::u(1));
  c.op(kOp_glBindBuffer, C::u(GL_ELEMENT_ARRAY_BUFFER), C::u(indices_));
  if (use_logo) c.op(kOp_glBindTexture, C::u(GL_TEXTURE_2D), C::u(logo_));
  c.op(kOp_glDrawElements, C::u(GL_TRIANGLES), C::i(graphics::kCubeIndices), C::u(GL_UNSIGNED_SHORT), C::p(0));   // (from the element buffer: offset 0)
  submit(c, &ok);
  const uint64_t submitted_at = now_us();
  // (SwapBuffers: the frame becomes the front buffer the window shows, then what the window would get is read)
  ok = ok && service_.swap(pid_, kWindow, 0, 0, true, nullptr, nullptr) == opengl::GlStatus::Ok && read_front();
  if (times) {
    times->submit_us = submitted_at - start;
    times->readback_us = now_us() - submitted_at;
  }
  return ok;
}

}  // namespace desktop
