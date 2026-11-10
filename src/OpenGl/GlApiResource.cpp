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

// GL_ARB_program_interface_query (GL 4.3): a linked program's resources by interface. The lists are the ones the older queries already give: the uniforms of
// glGetActiveUniform with the uniform block members after them (GlApi::block_member), the uniform blocks, the vertex attributes, the fragment shader's user outputs, and
// the transform feedback varyings (ProgramObj::tf_resources) with the buffers they go to. The program has no storage blocks, atomic counters or subroutines, so those
// interfaces are valid and empty.
#include "OpenGl/GlApi.hpp"
#include "OpenGl/generated/GlEnums.inc"

namespace opengl {

namespace {
constexpr uint32_t kVertex = 1, kTessControl = 2, kTessEval = 4, kGeometry = 8, kFragment = 16;

bool known_interface(uint32_t iface) {
  switch (iface) {
    case GL_UNIFORM: case GL_UNIFORM_BLOCK: case GL_PROGRAM_INPUT: case GL_PROGRAM_OUTPUT: case GL_TRANSFORM_FEEDBACK_VARYING: case GL_TRANSFORM_FEEDBACK_BUFFER:
    case GL_BUFFER_VARIABLE: case GL_SHADER_STORAGE_BLOCK: case GL_ATOMIC_COUNTER_BUFFER: case GL_VERTEX_SUBROUTINE: case GL_TESS_CONTROL_SUBROUTINE:
    case GL_TESS_EVALUATION_SUBROUTINE: case GL_GEOMETRY_SUBROUTINE: case GL_FRAGMENT_SUBROUTINE: case GL_COMPUTE_SUBROUTINE: case GL_VERTEX_SUBROUTINE_UNIFORM:
    case GL_TESS_CONTROL_SUBROUTINE_UNIFORM: case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM: case GL_GEOMETRY_SUBROUTINE_UNIFORM: case GL_FRAGMENT_SUBROUTINE_UNIFORM:
    case GL_COMPUTE_SUBROUTINE_UNIFORM:
      return true;
    default:
      return false;
  }
}
// Interfaces whose resources have no names.
bool nameless_interface(uint32_t iface) { return iface == GL_TRANSFORM_FEEDBACK_BUFFER || iface == GL_ATOMIC_COUNTER_BUFFER; }

// "a" and "a[0]" both name an array resource reported as "a[0]".
bool names_resource(StringView want, StringView name) {
  if (want.equals(name)) return true;
  return name.ends_with("[0]") && want.size() + 3 == name.size() && name.starts_with(want);
}
}  // namespace

// The fragment stage's user outputs: the module's Out globals, in declaration order.
static const shader::Global* fragment_output(const webgl::ProgramObj& p, uint32_t index) {
  uint32_t n = 0;
  for (const auto& g : p.fsm->globals)
    if (g->kind == shader::GlobalKind::Out && n++ == index) return g.get();
  return nullptr;
}

// The transform feedback buffer bindings a program writes, in increasing order: buffer resource i is the i-th of them.
static uint32_t tf_buffer_mask(const webgl::ProgramObj& p) {
  uint32_t mask = 0;
  for (const auto& r : p.tf_resources)
    if (r->binding >= 0 && r->binding < 4) mask |= 1u << r->binding;
  return mask;
}
static int32_t tf_buffer_binding(const webgl::ProgramObj& p, uint32_t index) {
  const uint32_t mask = tf_buffer_mask(p);
  uint32_t n = 0;
  for (int32_t b = 0; b < 4; ++b)
    if ((mask & (1u << b)) && n++ == index) return b;
  return -1;
}
static int32_t tf_buffer_index(const webgl::ProgramObj& p, int32_t binding) {
  if (binding < 0 || binding >= 4) return -1;
  const uint32_t mask = tf_buffer_mask(p);
  if (!(mask & (1u << binding))) return -1;
  int32_t n = 0;
  for (int32_t b = 0; b < binding; ++b)
    if (mask & (1u << b)) ++n;
  return n;
}

uint32_t GlApi::resource_count(uint32_t object, uint32_t iface) {
  const webgl::ProgramObj* p = gl_.program(object);
  if (!p || !p->linked) return 0;
  switch (iface) {
    case GL_UNIFORM: {
      uint32_t n = static_cast<uint32_t>(p->uniforms.count());
      BlockMember bm;
      while (block_member(object, n, &bm)) ++n;
      return n;
    }
    case GL_UNIFORM_BLOCK: return gl_.uniform_block_count(object);
    case GL_PROGRAM_INPUT: return static_cast<uint32_t>(p->attribs.count());
    case GL_PROGRAM_OUTPUT: {
      uint32_t n = 0;
      while (fragment_output(*p, n)) ++n;
      return n;
    }
    case GL_TRANSFORM_FEEDBACK_VARYING: return static_cast<uint32_t>(p->tf_resources.count());
    case GL_TRANSFORM_FEEDBACK_BUFFER: {
      uint32_t n = 0;
      for (uint32_t m = tf_buffer_mask(*p); m; m &= m - 1) ++n;
      return n;
    }
    default: return 0;
  }
}

// One resource's name and properties. Fields a resource of the interface does not have stay at their defaults.
bool GlApi::resource_at(uint32_t object, uint32_t iface, uint32_t index, Resource* out) {
  const webgl::ProgramObj* p = gl_.program(object);
  if (!p || !p->linked) return false;
  out->name.clear();
  switch (iface) {
    case GL_UNIFORM: {
      if (index < p->uniforms.count()) {
        const webgl::UniformLeaf& u = *p->uniforms[index];
        out->name.append(u.name.view());
        if (u.size > 1) out->name.append("[0]");
        out->type = u.gl_type;
        out->array_size = u.size;
        out->location = static_cast<int32_t>(u.location);
        out->stages = (u.vs_slot >= 0 ? kVertex : 0) | (u.fs_slot >= 0 ? kFragment : 0) | (u.extra_slot[webgl::kStageGeometry] >= 0 ? kGeometry : 0) |
                      (u.extra_slot[webgl::kStageTessControl] >= 0 ? kTessControl : 0) | (u.extra_slot[webgl::kStageTessEval] >= 0 ? kTessEval : 0);
        return true;
      }
      BlockMember bm;
      if (!block_member(object, index, &bm)) return false;
      const shader::Global& g = *bm.global;
      out->name.append(g.name.view());
      if (g.type.array) out->name.append("[0]");
      out->type = gl_.uniform_gl_type(g.type.element());
      out->array_size = g.type.array ? g.type.array : 1;
      out->offset = static_cast<int32_t>(bm.offset);
      out->block = static_cast<int32_t>(bm.block);
      out->array_stride = static_cast<int32_t>(bm.array_stride);
      out->matrix_stride = static_cast<int32_t>(bm.matrix_stride);
      out->stages = block_stages(object, bm.block);
      return true;
    }
    case GL_UNIFORM_BLOCK: {
      if (index >= gl_.uniform_block_count(object)) return false;
      out->name.append(gl_.uniform_block_name(object, index));
      double v[64];
      if (gl_.uniform_block_info(object, index, GL_UNIFORM_BLOCK_BINDING, v)) out->binding = static_cast<int32_t>(v[0]);
      if (gl_.uniform_block_info(object, index, GL_UNIFORM_BLOCK_DATA_SIZE, v)) out->data_size = static_cast<int32_t>(v[0]);
      out->stages = block_stages(object, index);
      return true;
    }
    case GL_PROGRAM_INPUT: {
      if (index >= p->attribs.count()) return false;
      const webgl::AttribInfo& a = *p->attribs[index];
      out->name.append(a.name.view());
      if (a.size > 1) out->name.append("[0]");
      out->type = a.gl_type;
      out->array_size = a.size;
      out->location = a.location;
      out->stages = kVertex;
      return true;
    }
    case GL_PROGRAM_OUTPUT: {
      const shader::Global* g = fragment_output(*p, index);
      if (!g) return false;
      out->name.append(g->name.view());
      if (g->type.array) out->name.append("[0]");
      out->type = gl_.uniform_gl_type(g->type.element());
      out->array_size = g->type.array ? g->type.array : 1;
      out->location = g->location >= 0 ? g->location : 0;
      for (const webgl::ProgramObj::Output& o : p->outputs)
        if (o.slot == g->slot) { out->location = o.location; break; }
      out->location_index = 0;
      out->stages = kFragment;
      return true;
    }
    case GL_TRANSFORM_FEEDBACK_VARYING: {
      if (index >= p->tf_resources.count()) return false;
      const webgl::ProgramObj::TfResource& r = *p->tf_resources[index];
      out->name.append(r.name.view());
      out->type = r.gl_type;
      out->array_size = r.size;
      out->offset = r.offset;
      out->buffer_index = tf_buffer_index(*p, r.binding);
      return true;
    }
    case GL_TRANSFORM_FEEDBACK_BUFFER: {
      const int32_t b = tf_buffer_binding(*p, index);
      if (b < 0) return false;
      out->binding = b;
      out->stride = static_cast<int32_t>(p->tf_stride[b]);
      return true;
    }
    default:
      return false;
  }
}

// The stages that use uniform block `block` (the REFERENCED_BY_* of glGetActiveUniformBlockiv).
uint32_t GlApi::block_stages(uint32_t object, uint32_t block) {
  const struct { uint32_t pname, bit; } kinds[] = {{GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER, kVertex}, {GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER, kFragment},
                                                   {GL_UNIFORM_BLOCK_REFERENCED_BY_GEOMETRY_SHADER, kGeometry}, {GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_CONTROL_SHADER, kTessControl},
                                                   {GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_EVALUATION_SHADER, kTessEval}};
  uint32_t stages = 0;
  for (const auto& k : kinds) {
    double v[64];
    if (gl_.uniform_block_info(object, block, k.pname, v) && v[0] != 0) stages |= k.bit;
  }
  return stages;
}

// The resources of `iface` that belong to resource `index` of a block-like interface (a uniform block's members, a feedback buffer's varyings), each passed to `f`.
template <class F>
void GlApi::active_variables(uint32_t object, uint32_t iface, uint32_t index, F&& f) {
  const webgl::ProgramObj* p = gl_.program(object);
  if (!p || !p->linked) return;
  if (iface == GL_UNIFORM_BLOCK) {
    BlockMember bm;
    for (uint32_t k = static_cast<uint32_t>(p->uniforms.count()); block_member(object, k, &bm); ++k)
      if (bm.block == index) f(k);
  } else if (iface == GL_TRANSFORM_FEEDBACK_BUFFER) {
    const int32_t b = tf_buffer_binding(*p, index);
    for (uint64_t k = 0; k < p->tf_resources.count(); ++k)
      if (p->tf_resources[k]->binding == b && p->tf_resources[k]->gl_type) f(static_cast<uint32_t>(k));
  }
}

void GlApi::glGetProgramInterfaceiv(GLuint program, GLenum programInterface, GLenum pname, UserPtr params) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  if (!known_interface(programInterface)) { error(GL_INVALID_ENUM); return; }
  const uint32_t n = resource_count(object, programInterface);
  int32_t v = 0;
  switch (pname) {
    case GL_ACTIVE_RESOURCES: v = static_cast<int32_t>(n); break;
    case GL_MAX_NAME_LENGTH: {
      if (nameless_interface(programInterface)) { error(GL_INVALID_OPERATION); return; }
      Resource r(a_);
      for (uint32_t i = 0; i < n; ++i)
        if (resource_at(object, programInterface, i, &r) && static_cast<int32_t>(r.name.size() + 1) > v) v = static_cast<int32_t>(r.name.size() + 1);
      break;
    }
    case GL_MAX_NUM_ACTIVE_VARIABLES: {
      if (programInterface != GL_UNIFORM_BLOCK && programInterface != GL_TRANSFORM_FEEDBACK_BUFFER && programInterface != GL_SHADER_STORAGE_BLOCK &&
          programInterface != GL_ATOMIC_COUNTER_BUFFER) { error(GL_INVALID_OPERATION); return; }
      for (uint32_t i = 0; i < n; ++i) {
        int32_t count = 0;
        active_variables(object, programInterface, i, [&](uint32_t) { ++count; });
        if (count > v) v = count;
      }
      break;
    }
    case GL_MAX_NUM_COMPATIBLE_SUBROUTINES:
      if (programInterface < GL_VERTEX_SUBROUTINE_UNIFORM || programInterface > GL_COMPUTE_SUBROUTINE_UNIFORM) { error(GL_INVALID_OPERATION); return; }
      break;
    default: error(GL_INVALID_ENUM); return;
  }
  write_values(params, &v, 1);
}

uint64_t GlApi::glGetProgramResourceIndex(GLuint program, GLenum programInterface, UserPtr name) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return GL_INVALID_INDEX; }
  if (!known_interface(programInterface) || nameless_interface(programInterface)) { error(GL_INVALID_ENUM); return GL_INVALID_INDEX; }
  String want(a_);
  if (!read_string(name, -1, &want)) { error(GL_INVALID_VALUE); return GL_INVALID_INDEX; }
  const uint32_t n = resource_count(object, programInterface);
  Resource r(a_);
  for (uint32_t i = 0; i < n; ++i)
    if (resource_at(object, programInterface, i, &r) && names_resource(want.view(), r.name.view())) return i;
  return GL_INVALID_INDEX;
}

void GlApi::glGetProgramResourceName(GLuint program, GLenum programInterface, GLuint index, GLsizei bufSize, UserPtr length, UserPtr name) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  if (!known_interface(programInterface) || nameless_interface(programInterface)) { error(GL_INVALID_ENUM); return; }
  if (bufSize < 0) { error(GL_INVALID_VALUE); return; }
  Resource r(a_);
  if (!resource_at(object, programInterface, index, &r)) { error(GL_INVALID_VALUE); return; }
  write_string(name, bufSize, length, r.name.view());
}

void GlApi::glGetProgramResourceiv(GLuint program, GLenum programInterface, GLuint index, GLsizei propCount, UserPtr props, GLsizei bufSize, UserPtr length, UserPtr params) {
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return; }
  if (!known_interface(programInterface)) { error(GL_INVALID_ENUM); return; }
  if (propCount <= 0 || bufSize < 0) { error(GL_INVALID_VALUE); return; }
  Resource r(a_);
  if (!resource_at(object, programInterface, index, &r)) { error(GL_INVALID_VALUE); return; }
  const uint32_t iface = programInterface;
  const bool variable = iface == GL_UNIFORM || iface == GL_PROGRAM_INPUT || iface == GL_PROGRAM_OUTPUT || iface == GL_TRANSFORM_FEEDBACK_VARYING;
  const bool referenced = iface == GL_UNIFORM || iface == GL_UNIFORM_BLOCK || iface == GL_PROGRAM_INPUT || iface == GL_PROGRAM_OUTPUT;
  // Every property is checked before anything is written: one the interface does not have is an error, and then nothing is written.
  List<int32_t> values(a_);
  for (int32_t i = 0; i < propCount; ++i) {
    uint32_t prop = 0;
    if (!read_values(props.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &prop, 1)) { error(GL_INVALID_VALUE); return; }
    bool fits = true;
    int32_t v = 0;
    switch (prop) {
      case GL_NAME_LENGTH: fits = !nameless_interface(iface); v = static_cast<int32_t>(r.name.size() + 1); break;
      case GL_TYPE: fits = variable; v = static_cast<int32_t>(r.type); break;
      case GL_ARRAY_SIZE: fits = variable; v = static_cast<int32_t>(r.array_size); break;
      case GL_OFFSET: fits = iface == GL_UNIFORM || iface == GL_TRANSFORM_FEEDBACK_VARYING; v = r.offset; break;
      case GL_BLOCK_INDEX: fits = iface == GL_UNIFORM; v = r.block; break;
      case GL_ARRAY_STRIDE: fits = iface == GL_UNIFORM; v = r.array_stride; break;
      case GL_MATRIX_STRIDE: fits = iface == GL_UNIFORM; v = r.matrix_stride; break;
      case GL_IS_ROW_MAJOR: fits = iface == GL_UNIFORM; v = 0; break;
      case GL_ATOMIC_COUNTER_BUFFER_INDEX: fits = iface == GL_UNIFORM; v = -1; break;
      case GL_BUFFER_BINDING: fits = iface == GL_UNIFORM_BLOCK || iface == GL_TRANSFORM_FEEDBACK_BUFFER; v = r.binding; break;
      case GL_BUFFER_DATA_SIZE: fits = iface == GL_UNIFORM_BLOCK; v = r.data_size; break;
      case GL_NUM_ACTIVE_VARIABLES:
        fits = iface == GL_UNIFORM_BLOCK || iface == GL_TRANSFORM_FEEDBACK_BUFFER;
        if (fits) active_variables(object, iface, index, [&](uint32_t) { ++v; });
        break;
      case GL_ACTIVE_VARIABLES:      // (several values: one per member)
        if (iface != GL_UNIFORM_BLOCK && iface != GL_TRANSFORM_FEEDBACK_BUFFER) { error(GL_INVALID_OPERATION); return; }
        active_variables(object, iface, index, [&](uint32_t k) { values.push_back(static_cast<int32_t>(k)); });
        continue;
      case GL_REFERENCED_BY_VERTEX_SHADER: fits = referenced; v = (r.stages & kVertex) != 0; break;
      case GL_REFERENCED_BY_TESS_CONTROL_SHADER: fits = referenced; v = (r.stages & kTessControl) != 0; break;
      case GL_REFERENCED_BY_TESS_EVALUATION_SHADER: fits = referenced; v = (r.stages & kTessEval) != 0; break;
      case GL_REFERENCED_BY_GEOMETRY_SHADER: fits = referenced; v = (r.stages & kGeometry) != 0; break;
      case GL_REFERENCED_BY_FRAGMENT_SHADER: fits = referenced; v = (r.stages & kFragment) != 0; break;
      case GL_REFERENCED_BY_COMPUTE_SHADER: fits = referenced; v = 0; break;
      case GL_TOP_LEVEL_ARRAY_SIZE: case GL_TOP_LEVEL_ARRAY_STRIDE: fits = iface == GL_BUFFER_VARIABLE; break;
      case GL_LOCATION: fits = iface == GL_UNIFORM || iface == GL_PROGRAM_INPUT || iface == GL_PROGRAM_OUTPUT; v = r.location; break;
      case GL_LOCATION_INDEX: fits = iface == GL_PROGRAM_OUTPUT; v = r.location_index; break;
      case GL_LOCATION_COMPONENT: fits = iface == GL_PROGRAM_INPUT || iface == GL_PROGRAM_OUTPUT; v = 0; break;
      case GL_IS_PER_PATCH: fits = iface == GL_PROGRAM_INPUT || iface == GL_PROGRAM_OUTPUT; v = 0; break;
      case GL_TRANSFORM_FEEDBACK_BUFFER_INDEX: fits = iface == GL_TRANSFORM_FEEDBACK_VARYING; v = r.buffer_index; break;
      case GL_TRANSFORM_FEEDBACK_BUFFER_STRIDE: fits = iface == GL_TRANSFORM_FEEDBACK_BUFFER; v = r.stride; break;
      default: error(GL_INVALID_ENUM); return;
    }
    if (!fits) { error(GL_INVALID_OPERATION); return; }
    if (!values.push_back(v)) { error(GL_OUT_OF_MEMORY); return; }
  }
  const uint64_t n = values.count() < static_cast<uint64_t>(bufSize) ? values.count() : static_cast<uint64_t>(bufSize);
  if (n) write_values(params, values.view().data(), n);
  const int32_t written = static_cast<int32_t>(n);
  if (length.address) write_values(length, &written, 1);
}

uint64_t GlApi::glGetProgramResourceLocation(GLuint program, GLenum programInterface, UserPtr name) {
  const uint64_t none = static_cast<uint64_t>(-1);
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return none; }
  if (programInterface != GL_UNIFORM && programInterface != GL_PROGRAM_INPUT && programInterface != GL_PROGRAM_OUTPUT &&
      (programInterface < GL_VERTEX_SUBROUTINE_UNIFORM || programInterface > GL_COMPUTE_SUBROUTINE_UNIFORM)) { error(GL_INVALID_ENUM); return none; }
  const webgl::ProgramObj* p = gl_.program(object);
  if (!p || !p->linked) { error(GL_INVALID_OPERATION); return none; }
  String want(a_);
  if (!read_string(name, -1, &want)) { error(GL_INVALID_VALUE); return none; }
  // uniforms: by the engine's rules ("a[3]" is the fourth element's location)
  if (programInterface == GL_UNIFORM) return static_cast<uint64_t>(static_cast<int64_t>(gl_.get_uniform_location(object, want.view())));
  const uint32_t n = resource_count(object, programInterface);
  Resource r(a_);
  for (uint32_t i = 0; i < n; ++i)
    if (resource_at(object, programInterface, i, &r) && names_resource(want.view(), r.name.view())) return static_cast<uint64_t>(static_cast<int64_t>(r.location));
  return none;
}

uint64_t GlApi::glGetProgramResourceLocationIndex(GLuint program, GLenum programInterface, UserPtr name) {
  const uint64_t none = static_cast<uint64_t>(-1);
  const uint32_t object = program_object(program);
  if (!object) { error(GL_INVALID_VALUE); return none; }
  if (programInterface != GL_PROGRAM_OUTPUT) { error(GL_INVALID_ENUM); return none; }
  const webgl::ProgramObj* p = gl_.program(object);
  if (!p || !p->linked) { error(GL_INVALID_OPERATION); return none; }
  String want(a_);
  if (!read_string(name, -1, &want)) { error(GL_INVALID_VALUE); return none; }
  const uint32_t n = resource_count(object, programInterface);
  Resource r(a_);
  for (uint32_t i = 0; i < n; ++i)
    if (resource_at(object, programInterface, i, &r) && names_resource(want.view(), r.name.view())) return static_cast<uint64_t>(static_cast<int64_t>(r.location_index));
  return none;
}

// GL 3.0's query of the same: a fragment output's location.
uint64_t GlApi::glGetFragDataLocation(GLuint program, UserPtr name) {
  return glGetProgramResourceLocation(program, GL_PROGRAM_OUTPUT, name);
}

}  // namespace opengl
