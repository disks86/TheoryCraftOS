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

// Shaders, programs and uniforms over the engine's. Shaders and programs share a name space, as GL says; the current program is kept here because the engine's "current program" is
// also whatever the next draw uses, which may be a fixed-function one.
#include "OpenGl/GlApi.hpp"
#include "OpenGl/GlslDesktop.hpp"
#include "OpenGl/generated/GlEnums.inc"

namespace opengl {

namespace {
bool is_shader_type(uint32_t type) {
  return type == GL_VERTEX_SHADER || type == GL_FRAGMENT_SHADER;
}
}  // namespace

uint32_t GlApi::shader_object(uint32_t name) const {
  const uint32_t object = group_->shaders_and_programs.object(name);
  return (object & kProgramObjectBit) ? 0 : object;
}
uint32_t GlApi::program_object(uint32_t name) const {
  const uint32_t object = group_->shaders_and_programs.object(name);
  return (object & kProgramObjectBit) ? object & ~kProgramObjectBit : 0;
}

uint64_t GlApi::glCreateShader(GLenum type) {
  if (!is_shader_type(type)) { error(GL_INVALID_ENUM); return 0; }
  const uint32_t object = gl_.create_shader(type);
  const uint32_t name = group_->shaders_and_programs.generate();
  group_->shaders_and_programs.set(name, object);
  return name;
}
void GlApi::glDeleteShader(GLuint shader) {
  if (!shader) return;
  const uint32_t object = shader_object(shader);
  if (!object) { error(GL_INVALID_VALUE); return; }
  gl_.delete_object('s', object);
  group_->shaders_and_programs.erase(shader);
}
uint64_t GlApi::glIsShader(GLuint shader) { return shader_object(shader) != 0; }

uint64_t GlApi::glCreateProgram() {
  const uint32_t object = gl_.create_program();
  const uint32_t name = group_->shaders_and_programs.generate();
  group_->shaders_and_programs.set(name, object | kProgramObjectBit);
  return name;
}
void GlApi::glDeleteProgram(GLuint program) {
  if (!program) return;
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  if (user_program_ == object) deleted_current_program_ = object;      // lives on while it is current
  else gl_.delete_object('p', object);
  group_->shaders_and_programs.erase(program);
}
uint64_t GlApi::glIsProgram(GLuint program) { return program_object(program) != 0; }

void GlApi::glShaderSource(GLuint shader, GLsizei count, UserPtr string, UserPtr length) {
  const uint32_t object = shader_object(shader);
  if (!object) { error(GL_INVALID_VALUE); return; }
  if (count < 0) { error(GL_INVALID_VALUE); return; }
  String whole(a_);
  String part(a_);
  for (int32_t i = 0; i < count; ++i) {
    uint64_t at = 0;
    if (!read_values(string.plus(uint64_t{static_cast<uint32_t>(i)} * 8), &at, 1)) { error(GL_INVALID_VALUE); return; }
    int32_t n = -1;
    if (!length.null() && !read_values(length.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &n, 1)) { error(GL_INVALID_VALUE); return; }
    if (!read_string(UserPtr{at}, n, &part)) { error(GL_INVALID_VALUE); return; }
    if (!whole.append(part.view())) { error(GL_OUT_OF_MEMORY); return; }
  }
  // Desktop GLSL goes to the front end as GLSL ES (GlslDesktop.hpp).
  const webgl::ShaderObj* so = gl_.shader_at(object);
  String translated(a_);
  uint32_t uses = 0;
  if (so && translate_desktop_glsl(whole.view(), so->type == GL_VERTEX_SHADER, &translated, &uses)) gl_.shader_source(object, translated.view());
  else gl_.shader_source(object, whole.view());
}
void GlApi::glCompileShader(GLuint shader) {
  const uint32_t object = shader_object(shader);
  if (!object) { error(GL_INVALID_VALUE); return; }
  gl_.compile_shader(object);
}
void GlApi::glAttachShader(GLuint program, GLuint shader) {
  const uint32_t p = program_object(program), s = shader_object(shader);
  if (!p || !s) { error(GL_INVALID_VALUE); return; }
  gl_.attach_shader(p, s);
}
void GlApi::glDetachShader(GLuint program, GLuint shader) {
  const uint32_t p = program_object(program), s = shader_object(shader);
  if (!p || !s) { error(GL_INVALID_VALUE); return; }
  gl_.detach_shader(p, s);
}
void GlApi::glLinkProgram(GLuint program) {
  const uint32_t p = program_object(program);
  if (!p) { error(GL_INVALID_VALUE); return; }
  // The compatibility attributes sit where the fixed-function arrays feed them (unless the program bound them itself).
  static constexpr struct { const char* name; uint32_t attr; } kCompat[] = {
      {"tcos_Vertex", kAttrVertex}, {"tcos_Normal", kAttrNormal}, {"tcos_Color", kAttrColor}, {"tcos_SecondaryColor", kAttrSecondary}, {"tcos_FogCoord", kAttrFog},
      {"tcos_MultiTexCoord0", kAttrTex0}, {"tcos_MultiTexCoord1", kAttrTex0 + 1}, {"tcos_MultiTexCoord2", kAttrTex0 + 2}, {"tcos_MultiTexCoord3", kAttrTex0 + 3},
      {"tcos_MultiTexCoord4", kAttrTex0 + 4}, {"tcos_MultiTexCoord5", kAttrTex0 + 5}, {"tcos_MultiTexCoord6", kAttrTex0 + 6}, {"tcos_MultiTexCoord7", kAttrTex0 + 7}};
  for (const auto& c : kCompat) gl_.bind_attrib_location(p, c.attr, c.name);
  // A compatibility program may leave out a stage: the fixed-function one stands in, written at the version of the stage that is there so that the varyings match.
  const webgl::ProgramObj* po = gl_.program(p);
  uint32_t stand_in = 0;
  if (po && compat() && (po->vs == 0) != (po->fs == 0)) {
    const uint32_t present = po->vs ? po->vs : po->fs;
    const bool modern = gl_.shader_src(present).starts_with("#version 300");
    const bool need_vertex = po->vs == 0;
    String text(a_);
    text.append(modern ? "#version 130\n" : "#version 110\n");
    if (need_vertex) {
      text.append("void main(){ gl_Position = ftransform(); gl_FrontColor = gl_Color; gl_FrontSecondaryColor = gl_SecondaryColor; gl_FogFragCoord = abs((gl_ModelViewMatrix * gl_Vertex).z);\n");
      for (uint32_t i = 0; i < kMaxTexUnits; ++i) {
        char line[96] = "  gl_TexCoord[0] = gl_TextureMatrix[0] * gl_MultiTexCoord0;\n";
        line[14] = static_cast<char>('0' + i);
        line[36] = static_cast<char>('0' + i);
        line[57] = static_cast<char>('0' + i);
        text.append(line);
      }
      text.append("}\n");
    } else {
      text.append("void main(){ gl_FragColor = gl_Color; }\n");
    }
    String translated(a_);
    uint32_t uses = 0;
    translate_desktop_glsl(text.view(), need_vertex, &translated, &uses);
    stand_in = gl_.create_shader(need_vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
    gl_.shader_source(stand_in, translated.view());
    gl_.compile_shader(stand_in);
    gl_.attach_shader(p, stand_in);
  }
  gl_.link_program(p);
  if (stand_in) {      // (the program keeps its own copy of what it was linked from)
    gl_.detach_shader(p, stand_in);
    gl_.delete_object('s', stand_in);
  }
}
void GlApi::glValidateProgram(GLuint program) {
  const uint32_t p = program_object(program);
  if (!p) { error(GL_INVALID_VALUE); return; }
  gl_.validate_program(p);
}
void GlApi::glUseProgram(GLuint program) {
  uint32_t object = 0;
  if (program) {
    object = program_object(program);
    if (!object) { error(GL_INVALID_VALUE); return; }
    const webgl::ProgramObj* po = gl_.program(object);
    if (!po || !po->linked) { error(GL_INVALID_OPERATION); return; }
  }
  if (deleted_current_program_ && deleted_current_program_ != object) {
    gl_.delete_object('p', deleted_current_program_);
    deleted_current_program_ = 0;
  }
  user_program_ = object;
  gl_.program_ = object;
  ++compat_generation_;
}
void GlApi::glBindAttribLocation(GLuint program, GLuint index, UserPtr name) {
  const uint32_t p = program_object(program);
  if (!p) { error(GL_INVALID_VALUE); return; }
  String s(a_);
  if (!read_string(name, -1, &s)) { error(GL_INVALID_VALUE); return; }
  if (index >= webgl::kMaxAttribs) { error(GL_INVALID_VALUE); return; }
  gl_.bind_attrib_location(p, index, s.view());
}
uint64_t GlApi::glGetAttribLocation(GLuint program, UserPtr name) {
  const uint32_t p = program_object(program);
  if (!p) { error(GL_INVALID_VALUE); return static_cast<uint64_t>(-1); }
  String s(a_);
  if (!read_string(name, -1, &s)) { error(GL_INVALID_VALUE); return static_cast<uint64_t>(-1); }
  return static_cast<uint64_t>(static_cast<int64_t>(gl_.get_attrib_location(p, s.view())));
}
uint64_t GlApi::glGetUniformLocation(GLuint program, UserPtr name) {
  const uint32_t p = program_object(program);
  if (!p) { error(GL_INVALID_VALUE); return static_cast<uint64_t>(-1); }
  String s(a_);
  if (!read_string(name, -1, &s)) { error(GL_INVALID_VALUE); return static_cast<uint64_t>(-1); }
  return static_cast<uint64_t>(static_cast<int64_t>(gl_.get_uniform_location(p, s.view())));
}
void GlApi::glGetShaderiv(GLuint shader, GLenum pname, UserPtr params) {
  const uint32_t object = shader_object(shader);
  if (!object) { error(GL_INVALID_VALUE); return; }
  int32_t out = 0;
  double v = 0;
  switch (pname) {
    case GL_SHADER_TYPE: case GL_COMPILE_STATUS: case GL_DELETE_STATUS:
      if (!gl_.shader_status(object, pname, &v)) { error(GL_INVALID_ENUM); return; }
      out = static_cast<int32_t>(v);
      break;
    case GL_INFO_LOG_LENGTH: { const uint64_t n = gl_.shader_log(object).size(); out = n ? static_cast<int32_t>(n + 1) : 0; break; }
    case GL_SHADER_SOURCE_LENGTH: { const uint64_t n = gl_.shader_src(object).size(); out = n ? static_cast<int32_t>(n + 1) : 0; break; }
    default: error(GL_INVALID_ENUM); return;
  }
  write_values(params, &out, 1);
}
void GlApi::glGetShaderInfoLog(GLuint shader, GLsizei bufSize, UserPtr length, UserPtr infoLog) {
  const uint32_t object = shader_object(shader);
  if (!object) { error(GL_INVALID_VALUE); return; }
  if (bufSize < 0) { error(GL_INVALID_VALUE); return; }
  write_string(infoLog, bufSize, length, gl_.shader_log(object));
}
void GlApi::glGetShaderSource(GLuint shader, GLsizei bufSize, UserPtr length, UserPtr source) {
  const uint32_t object = shader_object(shader);
  if (!object) { error(GL_INVALID_VALUE); return; }
  if (bufSize < 0) { error(GL_INVALID_VALUE); return; }
  write_string(source, bufSize, length, gl_.shader_src(object));
}
void GlApi::glGetProgramiv(GLuint program, GLenum pname, UserPtr params) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  const webgl::ProgramObj* po = gl_.program(object);
  int32_t out = 0;
  double v = 0;
  switch (pname) {
    case GL_INFO_LOG_LENGTH: { const uint64_t n = gl_.program_log(object).size(); out = n ? static_cast<int32_t>(n + 1) : 0; break; }
    case GL_ACTIVE_UNIFORM_MAX_LENGTH: {
      uint64_t longest = 0;
      for (uint64_t i = 0; i < po->uniforms.count(); ++i) {
        const uint64_t n = po->uniforms[i]->name.view().size() + (po->uniforms[i]->size > 1 ? 3 : 0);
        if (n >= longest) longest = n + 1;
      }
      out = static_cast<int32_t>(longest);
      break;
    }
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH: {
      uint64_t longest = 0;
      for (uint64_t i = 0; i < po->attribs.count(); ++i) if (po->attribs[i]->name.view().size() >= longest) longest = po->attribs[i]->name.view().size() + 1;
      out = static_cast<int32_t>(longest);
      break;
    }
    default:
      if (!gl_.program_status(object, pname, &v)) { error(GL_INVALID_ENUM); return; }
      out = static_cast<int32_t>(v);
      break;
  }
  write_values(params, &out, 1);
}
void GlApi::glGetProgramInfoLog(GLuint program, GLsizei bufSize, UserPtr length, UserPtr infoLog) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  if (bufSize < 0) { error(GL_INVALID_VALUE); return; }
  write_string(infoLog, bufSize, length, gl_.program_log(object));
}
void GlApi::glGetAttachedShaders(GLuint program, GLsizei maxCount, UserPtr count, UserPtr shaders) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  if (maxCount < 0) { error(GL_INVALID_VALUE); return; }
  uint32_t found[8];
  const uint32_t n = gl_.attached_shaders(object, found, 8);
  int32_t written = 0;
  for (uint32_t i = 0; i < n && written < maxCount; ++i) {
    const uint32_t name = group_->shaders_and_programs.name_of(found[i]);
    if (!write_values(shaders.plus(uint64_t{static_cast<uint32_t>(written)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
    ++written;
  }
  if (!count.null()) write_values(count, &written, 1);
}
void GlApi::glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, UserPtr length, UserPtr size, UserPtr type, UserPtr name) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  const webgl::UniformLeaf* u = gl_.active_uniform(object, index);
  if (!u) { error(GL_INVALID_VALUE); return; }
  const int32_t s = static_cast<int32_t>(u->size);
  write_values(size, &s, 1);
  write_values(type, &u->gl_type, 1);
  String full(a_);
  full.append(u->name.view());
  if (u->size > 1) full.append("[0]");
  write_string(name, bufSize, length, full.view());
}
void GlApi::glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, UserPtr length, UserPtr size, UserPtr type, UserPtr name) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  const webgl::AttribInfo* a = gl_.active_attrib(object, index);
  if (!a) { error(GL_INVALID_VALUE); return; }
  const int32_t s = static_cast<int32_t>(a->size);
  write_values(size, &s, 1);
  write_values(type, &a->gl_type, 1);
  write_string(name, bufSize, length, a->name.view());
}
void GlApi::glGetUniformfv(GLuint program, GLint location, UserPtr params) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  double v[16];
  uint32_t n = 0;
  if (!gl_.get_uniform(object, location, v, &n)) { error(GL_INVALID_OPERATION); return; }
  float f[16];
  for (uint32_t i = 0; i < n; ++i) f[i] = static_cast<float>(v[i]);
  write_values(params, f, n);
}
void GlApi::glGetUniformiv(GLuint program, GLint location, UserPtr params) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  double v[16];
  uint32_t n = 0;
  if (!gl_.get_uniform(object, location, v, &n)) { error(GL_INVALID_OPERATION); return; }
  int32_t f[16];
  for (uint32_t i = 0; i < n; ++i) f[i] = static_cast<int32_t>(v[i]);
  write_values(params, f, n);
}
void GlApi::glGetUniformuiv(GLuint program, GLint location, UserPtr params) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  double v[16];
  uint32_t n = 0;
  if (!gl_.get_uniform(object, location, v, &n)) { error(GL_INVALID_OPERATION); return; }
  uint32_t f[16];
  for (uint32_t i = 0; i < n; ++i) f[i] = static_cast<uint32_t>(v[i]);
  write_values(params, f, n);
}
void GlApi::glGetVertexAttribfv(GLuint index, GLenum pname, UserPtr params) {
  double v[4];
  uint32_t n = 0;
  if (!vertex_attrib_get(index, pname, v, &n)) return;
  float f[4];
  for (uint32_t i = 0; i < n; ++i) f[i] = static_cast<float>(v[i]);
  write_values(params, f, n);
}
void GlApi::glGetVertexAttribiv(GLuint index, GLenum pname, UserPtr params) {
  double v[4];
  uint32_t n = 0;
  if (!vertex_attrib_get(index, pname, v, &n)) return;
  int32_t f[4];
  for (uint32_t i = 0; i < n; ++i) f[i] = static_cast<int32_t>(v[i]);
  write_values(params, f, n);
}
void GlApi::glGetVertexAttribdv(GLuint index, GLenum pname, UserPtr params) {
  double v[4];
  uint32_t n = 0;
  if (!vertex_attrib_get(index, pname, v, &n)) return;
  write_values(params, v, n);
}
void GlApi::glGetVertexAttribPointerv(GLuint index, GLenum pname, UserPtr pointer) {
  if (index >= webgl::kMaxAttribs) { error(GL_INVALID_VALUE); return; }
  if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) { error(GL_INVALID_ENUM); return; }
  const webgl::VertexAttrib& at = gl_.current_vao_ptr()->attribs[index];
  const uint64_t value = at.from_client ? at.client : at.offset;
  write_values(pointer, &value, 1);
}
bool GlApi::vertex_attrib_get(uint32_t index, uint32_t pname, double* out, uint32_t* count) {
  if (index >= webgl::kMaxAttribs) { error(GL_INVALID_VALUE); return false; }
  const webgl::VertexAttrib& at = gl_.current_vao_ptr()->attribs[index];
  *count = 1;
  switch (pname) {
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED: *out = at.enabled; return true;
    case GL_VERTEX_ATTRIB_ARRAY_SIZE: *out = at.bgra ? GL_BGRA : at.size; return true;
    case GL_VERTEX_ATTRIB_ARRAY_STRIDE: *out = at.stride; return true;
    case GL_VERTEX_ATTRIB_ARRAY_TYPE: *out = at.type; return true;
    case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: *out = at.normalized; return true;
    case GL_VERTEX_ATTRIB_ARRAY_INTEGER: *out = at.integer; return true;
    case GL_VERTEX_ATTRIB_ARRAY_DIVISOR: *out = at.divisor; return true;
    case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: *out = at.from_client ? 0 : group_->buffers.name_of(at.buffer); return true;
    case GL_CURRENT_VERTEX_ATTRIB:
      *count = 4;
      for (int i = 0; i < 4; ++i) out[i] = at.cur_is_int ? at.cur_i[i] : at.cur_f[i];
      return true;
    default: error(GL_INVALID_ENUM); return false;
  }
}

// --- Uniforms -----------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::uniform_values(int32_t location, uint32_t kind, uint32_t cols, uint32_t rows, int32_t count, UserPtr values, bool transpose, bool doubles) {
  if (count < 0) { error(GL_INVALID_VALUE); return; }
  if (location == -1) return;
  const uint64_t comps = uint64_t{cols} * rows;
  const uint64_t n = uint64_t{static_cast<uint32_t>(count)} * comps;
  List<uint32_t> lanes(a_);
  if (!lanes.reserve(n)) { error(GL_OUT_OF_MEMORY); return; }
  for (uint64_t i = 0; i < n; ++i) lanes.push_back(0);
  if (doubles) {
    List<double> d(a_);
    if (!d.reserve(n)) { error(GL_OUT_OF_MEMORY); return; }
    for (uint64_t i = 0; i < n; ++i) d.push_back(0);
    if (!read_values(values, d.view().data(), n)) { error(GL_INVALID_VALUE); return; }
    for (uint64_t i = 0; i < n; ++i) lanes[i] = __builtin_bit_cast(uint32_t, static_cast<float>(d[i]));
  } else if (!read_values(values, lanes.view().data(), n)) {
    error(GL_INVALID_VALUE);
    return;
  }
  if (transpose && cols > 1) {
    List<uint32_t> copy(a_);
    if (!copy.reserve(n)) { error(GL_OUT_OF_MEMORY); return; }
    for (uint64_t i = 0; i < n; ++i) copy.push_back(lanes[i]);
    for (int32_t e = 0; e < count; ++e)
      for (uint32_t c = 0; c < cols; ++c)
        for (uint32_t r = 0; r < rows; ++r)
          lanes[uint64_t{static_cast<uint32_t>(e)} * comps + uint64_t{c} * rows + r] = copy[uint64_t{static_cast<uint32_t>(e)} * comps + uint64_t{r} * cols + c];
  }
  gl_.uniform(location, kind, cols, rows, static_cast<uint32_t>(count), lanes.view().data());
}
void GlApi::uniform_scalars(int32_t location, uint32_t kind, uint32_t rows, const uint32_t* lanes) {
  if (location == -1) return;
  gl_.uniform(location, kind, 1, rows, 1, lanes);
}
// Selects another program for a moment (glProgramUniform*).
void GlApi::with_program(uint32_t name, void (GlApi::*)(), bool*) { (void)name; }

uint32_t GlApi::begin_program_uniform(uint32_t program) {
  const uint32_t saved = gl_.program_;
  const uint32_t object = program_object(program);
  if (!object) error(GL_INVALID_VALUE);
  else gl_.program_ = object;
  return saved;
}
void GlApi::end_program_uniform(uint32_t saved) { gl_.program_ = saved; }

// --- Uniform blocks ---------------------------------------------------------------------------------------------------------------------------------------------

uint64_t GlApi::glGetUniformBlockIndex(GLuint program, UserPtr uniformBlockName) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return GL_INVALID_INDEX; }
  String s(a_);
  if (!read_string(uniformBlockName, -1, &s)) { error(GL_INVALID_VALUE); return GL_INVALID_INDEX; }
  return gl_.get_uniform_block_index(object, s.view());
}
void GlApi::glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  gl_.uniform_block_binding(object, uniformBlockIndex, uniformBlockBinding);
}
void GlApi::glGetActiveUniformBlockiv(GLuint program, GLuint uniformBlockIndex, GLenum pname, UserPtr params) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  double v[64];
  if (!gl_.uniform_block_info(object, uniformBlockIndex, pname, v)) { error(GL_INVALID_ENUM); return; }
  const int32_t i = static_cast<int32_t>(v[0]);
  write_values(params, &i, 1);
}
void GlApi::glGetActiveUniformBlockName(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, UserPtr length, UserPtr uniformBlockName) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  write_string(uniformBlockName, bufSize, length, gl_.uniform_block_name(object, uniformBlockIndex));
}
// Members of uniform blocks: the engine lists only the uniforms outside blocks, so these come after them (index = its uniforms + the member's number), numbered in the order of
// the vertex stage's blocks and then the fragment stage's new ones, with std140 offsets as the engine lays blocks out (GlContext::load_uniform_blocks).
bool GlApi::block_member(uint32_t object, uint32_t index, BlockMember* out) {
  const webgl::ProgramObj* po = gl_.program(object);
  if (!po || !po->linked || index < po->uniforms.count()) return false;
  uint32_t n = static_cast<uint32_t>(po->uniforms.count());
  const shader::Module* mods[2] = {po->vsm.get(), po->fsm.get()};
  for (int s = 0; s < 2; ++s) {
    const shader::Module& m = *mods[s];
    for (uint64_t b = 0; b < m.blocks.count(); ++b) {
      if (s == 1) {      // (a block both stages have counts once)
        bool seen = false;
        for (uint64_t k = 0; k < mods[0]->blocks.count(); ++k) seen = seen || mods[0]->blocks[k]->name.view().equals(m.blocks[b]->name.view());
        if (seen) continue;
      }
      uint32_t off = 0;
      for (uint64_t g = 0; g < m.globals.count(); ++g) {
        const shader::Global& gv = *m.globals[g];
        if (gv.block != static_cast<int32_t>(b)) continue;
        uint32_t al, sz;
        shader::layout_of(m, gv.type, shader::Layout::Std140, &al, &sz);
        off = (off + al - 1) / al * al;
        if (n == index) {
          out->module = &m;
          out->global = &gv;
          out->block = gl_.get_uniform_block_index(object, m.blocks[b]->name.view());
          out->offset = off;
          uint32_t eal, esz;
          shader::layout_of(m, gv.type.element(), shader::Layout::Std140, &eal, &esz);
          out->array_stride = gv.type.array ? (esz + 15) / 16 * 16 : 0;
          out->matrix_stride = gv.type.cols > 1 ? 16 : 0;
          return true;
        }
        off += sz;
        ++n;
      }
    }
  }
  return false;
}

void GlApi::glGetUniformIndices(GLuint program, GLsizei uniformCount, UserPtr uniformNames, UserPtr uniformIndices) {
  const uint32_t object = program_object(program);
  if (!object || uniformCount < 0) { error(GL_INVALID_VALUE); return; }
  const webgl::ProgramObj* po = gl_.program(object);
  String s(a_);
  for (int32_t i = 0; i < uniformCount; ++i) {
    uint64_t at = 0;
    if (!read_values(uniformNames.plus(uint64_t{static_cast<uint32_t>(i)} * 8), &at, 1) || !read_string(UserPtr{at}, -1, &s)) { error(GL_INVALID_VALUE); return; }
    // ("a[0]" names an array as "a" does)
    StringView want = s.view();
    if (want.size() > 3 && want.substr(want.size() - 3, 3).equals("[0]")) want = want.substr(0, want.size() - 3);
    uint32_t found = GL_INVALID_INDEX;
    for (uint64_t k = 0; k < po->uniforms.count() && found == GL_INVALID_INDEX; ++k) if (po->uniforms[k]->name.view().equals(want)) found = static_cast<uint32_t>(k);
    BlockMember bm;
    for (uint32_t k = static_cast<uint32_t>(po->uniforms.count()); found == GL_INVALID_INDEX && block_member(object, k, &bm); ++k) {
      const StringView member = bm.global->name.view();
      // a member is named by itself, or as Block.member when the block has an instance name
      if (member.equals(want)) { found = k; break; }
      const uint64_t dot = want.size() > member.size() ? want.size() - member.size() - 1 : 0;
      if (dot && want[dot] == '.' && want.substr(dot + 1, member.size()).equals(member)) found = k;
    }
    write_values(uniformIndices.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &found, 1);
  }
}

void GlApi::glGetActiveUniformsiv(GLuint program, GLsizei uniformCount, UserPtr uniformIndices, GLenum pname, UserPtr params) {
  const uint32_t object = program_object(program);
  if (!object || uniformCount < 0) { error(GL_INVALID_VALUE); return; }
  const webgl::ProgramObj* po = gl_.program(object);
  for (int32_t i = 0; i < uniformCount; ++i) {
    uint32_t index = 0;
    if (!read_values(uniformIndices.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &index, 1)) { error(GL_INVALID_VALUE); return; }
    int32_t out = 0;
    if (index < po->uniforms.count()) {
      const webgl::UniformLeaf& u = *po->uniforms[index];
      switch (pname) {
        case GL_UNIFORM_TYPE: out = static_cast<int32_t>(u.gl_type); break;
        case GL_UNIFORM_SIZE: out = static_cast<int32_t>(u.size); break;
        case GL_UNIFORM_NAME_LENGTH: out = static_cast<int32_t>(u.name.view().size() + 1 + (u.size > 1 ? 3 : 0)); break;
        case GL_UNIFORM_BLOCK_INDEX: case GL_UNIFORM_OFFSET: case GL_UNIFORM_ARRAY_STRIDE: case GL_UNIFORM_MATRIX_STRIDE: out = -1; break;
        case GL_UNIFORM_IS_ROW_MAJOR: out = 0; break;
        default: error(GL_INVALID_ENUM); return;
      }
    } else {
      BlockMember bm;
      if (!block_member(object, index, &bm)) { error(GL_INVALID_VALUE); return; }
      switch (pname) {
        case GL_UNIFORM_TYPE: out = static_cast<int32_t>(gl_.uniform_gl_type(bm.global->type.element())); break;
        case GL_UNIFORM_SIZE: out = bm.global->type.array ? static_cast<int32_t>(bm.global->type.array) : 1; break;
        case GL_UNIFORM_NAME_LENGTH: out = static_cast<int32_t>(bm.global->name.view().size() + 1 + (bm.global->type.array ? 3 : 0)); break;
        case GL_UNIFORM_BLOCK_INDEX: out = static_cast<int32_t>(bm.block); break;
        case GL_UNIFORM_OFFSET: out = static_cast<int32_t>(bm.offset); break;
        case GL_UNIFORM_ARRAY_STRIDE: out = static_cast<int32_t>(bm.array_stride); break;
        case GL_UNIFORM_MATRIX_STRIDE: out = static_cast<int32_t>(bm.matrix_stride); break;
        case GL_UNIFORM_IS_ROW_MAJOR: out = 0; break;
        default: error(GL_INVALID_ENUM); return;
      }
    }
    write_values(params.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &out, 1);
  }
}

// --- Transform feedback --------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::glTransformFeedbackVaryings(GLuint program, GLsizei count, UserPtr varyings, GLenum bufferMode) {
  const uint32_t object = program_object(program);
  if (!object || count < 0) { error(GL_INVALID_VALUE); return; }
  List<String> names(a_);
  List<StringView> views(a_);
  if (!names.reserve(static_cast<uint64_t>(count)) || !views.reserve(static_cast<uint64_t>(count))) { error(GL_OUT_OF_MEMORY); return; }
  for (int32_t i = 0; i < count; ++i) {
    uint64_t at = 0;
    String part(a_);
    if (!read_values(varyings.plus(uint64_t{static_cast<uint32_t>(i)} * 8), &at, 1) || !read_string(UserPtr{at}, -1, &part)) { error(GL_INVALID_VALUE); return; }
    names.push_back(static_cast<String&&>(part));
  }
  for (uint64_t i = 0; i < names.count(); ++i) views.push_back(names[i].view());
  gl_.transform_feedback_varyings(object, views.view(), bufferMode);
}
void GlApi::glBeginTransformFeedback(GLenum primitiveMode) { gl_.program_ = user_program_; gl_.begin_transform_feedback(primitiveMode); }
void GlApi::glEndTransformFeedback() { gl_.end_transform_feedback(); }

}  // namespace opengl

namespace opengl {

// --- GL_ARB_shader_objects: handles are the shared names of shaders and programs.

void GlApi::glDeleteObjectARB(GLhandleARB obj) {
  if (program_object(obj)) glDeleteProgram(obj);
  else if (shader_object(obj)) glDeleteShader(obj);
  else if (obj) error(GL_INVALID_VALUE);
}

uint64_t GlApi::glGetHandleARB(GLenum pname) {
  if (pname != GL_PROGRAM_OBJECT_ARB) { error(GL_INVALID_ENUM); return 0; }
  return user_program_ ? group_->shaders_and_programs.name_of(user_program_ | kProgramObjectBit) : 0;
}

void GlApi::glGetInfoLogARB(GLhandleARB obj, GLsizei maxLength, UserPtr length, UserPtr infoLog) {
  if (program_object(obj)) glGetProgramInfoLog(obj, maxLength, length, infoLog);
  else glGetShaderInfoLog(obj, maxLength, length, infoLog);
}

void GlApi::glGetAttachedObjectsARB(GLhandleARB containerObj, GLsizei maxCount, UserPtr count, UserPtr obj) { glGetAttachedShaders(containerObj, maxCount, count, obj); }

bool GlApi::object_parameter(uint32_t obj, uint32_t pname, int32_t* out) {
  const bool program = program_object(obj) != 0;
  if (!program && !shader_object(obj)) { error(GL_INVALID_VALUE); return false; }
  if (pname == GL_OBJECT_TYPE_ARB) { *out = static_cast<int32_t>(program ? GL_PROGRAM_OBJECT_ARB : GL_SHADER_OBJECT_ARB); return true; }
  if (pname == GL_OBJECT_SUBTYPE_ARB) {
    if (program) { error(GL_INVALID_OPERATION); return false; }
    const webgl::ShaderObj* so = gl_.shader_at(shader_object(obj));
    *out = so ? static_cast<int32_t>(so->type) : 0;
    return true;
  }
  // The other names are those of glGetShaderiv / glGetProgramiv (OBJECT_COMPILE_STATUS_ARB is GL_COMPILE_STATUS ...): answered by them through a word of our own.
  const uint32_t before = gl_.error_;
  int32_t value = 0;
  ProbeMemory probe(&value);
  IClientMemory* saved = memory_;
  memory_ = &probe;
  if (program) glGetProgramiv(obj, pname, UserPtr{ProbeMemory::kAddress});
  else glGetShaderiv(obj, pname, UserPtr{ProbeMemory::kAddress});
  memory_ = saved;
  *out = value;
  return gl_.error_ == before;
}

void GlApi::glGetObjectParameterivARB(GLhandleARB obj, GLenum pname, UserPtr params) {
  int32_t v = 0;
  if (object_parameter(obj, pname, &v)) write_values(params, &v, 1);
}

void GlApi::glGetObjectParameterfvARB(GLhandleARB obj, GLenum pname, UserPtr params) {
  int32_t v = 0;
  if (!object_parameter(obj, pname, &v)) return;
  const float f = static_cast<float>(v);
  write_values(params, &f, 1);
}

void GlApi::glGetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, UserPtr length, UserPtr uniformName) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  const webgl::UniformLeaf* u = gl_.active_uniform(object, uniformIndex);
  if (!u) { error(GL_INVALID_VALUE); return; }
  String full(a_);
  full.append(u->name.view());
  if (u->size > 1) full.append("[0]");
  write_string(uniformName, bufSize, length, full.view());
}

}  // namespace opengl
