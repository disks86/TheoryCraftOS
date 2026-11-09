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

// Vertex arrays and draw commands. Arrays in the program's memory are copied into hidden buffers for the draw, and primitives the engine does not know (quads, polygons) become indices;
// after that every draw is the engine's. Before a draw without a user program the fixed-function program for the current state is selected.
#include "OpenGl/FixedFunction.hpp"
#include "OpenGl/GlApi.hpp"
#include "OpenGl/generated/GlEnums.inc"

namespace opengl {

namespace {
uint32_t index_size(uint32_t type) { return type == GL_UNSIGNED_BYTE ? 1 : (type == GL_UNSIGNED_SHORT ? 2 : 4); }
uint32_t attrib_element_bytes(const webgl::VertexAttrib& at) {
  switch (at.type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return at.size;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return at.size * 2;
    case GL_DOUBLE: return at.size * 8;
    case GL_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV: return 4;
    default: return at.size * 4;
  }
}
uint32_t read_index_at(ConstByteView bytes, uint32_t size, uint64_t i) {
  const uint64_t at = i * size;
  if (at + size > bytes.size()) return 0;
  uint32_t v = 0;
  for (uint32_t k = 0; k < size; ++k) v |= uint32_t{bytes[at + k]} << (8 * k);
  return v;
}
}  // namespace

// --- Array state ---------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::client_pointer(uint32_t attr, int32_t size, uint32_t type, bool normalized, int32_t stride, UserPtr pointer, bool integer) {
  if (stride < 0) { error(GL_INVALID_VALUE); return; }
  webgl::Vao* vao = gl_.current_vao_ptr();
  webgl::VertexAttrib& at = vao->attribs[attr];
  // In a core context a vertex array object is required and arrays must be in buffers.
  const uint32_t bound = gl_.array_buffer_;
  if (config_.core_profile && (gl_.vao_id_ == 0 || (bound == 0 && !pointer.null()))) { error(GL_INVALID_OPERATION); return; }
  at.size = static_cast<uint32_t>(size == GL_BGRA ? 4 : size);
  at.bgra = size == GL_BGRA;
  at.type = type;
  at.normalized = normalized;
  at.integer = integer;
  at.stride = static_cast<uint32_t>(stride);
  if (bound) {
    at.buffer = bound;
    at.offset = pointer.address;
    at.from_client = false;
    at.client = 0;
  } else {
    at.buffer = 0;
    at.offset = 0;
    at.from_client = true;
    at.client = pointer.address;
  }
}

void GlApi::glVertexPointer(GLint size, GLenum type, GLsizei stride, UserPtr pointer) {
  if (size < 2 || size > 4) { error(GL_INVALID_VALUE); return; }
  if (type != GL_SHORT && type != GL_INT && type != GL_FLOAT && type != GL_DOUBLE && type != GL_HALF_FLOAT && type != GL_INT_2_10_10_10_REV && type != GL_UNSIGNED_INT_2_10_10_10_REV) { error(GL_INVALID_ENUM); return; }
  client_pointer(kAttrVertex, size, type, false, stride, pointer, false);
}
void GlApi::glNormalPointer(GLenum type, GLsizei stride, UserPtr pointer) {
  if (type != GL_BYTE && type != GL_SHORT && type != GL_INT && type != GL_FLOAT && type != GL_DOUBLE && type != GL_HALF_FLOAT && type != GL_INT_2_10_10_10_REV && type != GL_UNSIGNED_INT_2_10_10_10_REV) { error(GL_INVALID_ENUM); return; }
  client_pointer(kAttrNormal, 3, type, true, stride, pointer, false);
}
void GlApi::glColorPointer(GLint size, GLenum type, GLsizei stride, UserPtr pointer) {
  if ((size < 3 || size > 4) && size != GL_BGRA) { error(GL_INVALID_VALUE); return; }
  client_pointer(kAttrColor, size, type, true, stride, pointer, false);
}
void GlApi::glSecondaryColorPointer(GLint size, GLenum type, GLsizei stride, UserPtr pointer) {
  if (size != 3 && size != GL_BGRA) { error(GL_INVALID_VALUE); return; }
  client_pointer(kAttrSecondary, size, type, true, stride, pointer, false);
}
void GlApi::glFogCoordPointer(GLenum type, GLsizei stride, UserPtr pointer) {
  if (type != GL_FLOAT && type != GL_DOUBLE && type != GL_HALF_FLOAT) { error(GL_INVALID_ENUM); return; }
  client_pointer(kAttrFog, 1, type, false, stride, pointer, false);
}
void GlApi::glIndexPointer(GLenum type, GLsizei stride, UserPtr pointer) { client_pointer(kAttrIndex, 1, type, false, stride, pointer, false); }
void GlApi::glEdgeFlagPointer(GLsizei stride, UserPtr pointer) { client_pointer(kAttrEdge, 1, GL_UNSIGNED_BYTE, false, stride, pointer, false); }
void GlApi::glTexCoordPointer(GLint size, GLenum type, GLsizei stride, UserPtr pointer) {
  if (size < 1 || size > 4) { error(GL_INVALID_VALUE); return; }
  if (type != GL_SHORT && type != GL_INT && type != GL_FLOAT && type != GL_DOUBLE && type != GL_HALF_FLOAT && type != GL_INT_2_10_10_10_REV && type != GL_UNSIGNED_INT_2_10_10_10_REV) { error(GL_INVALID_ENUM); return; }
  client_pointer(kAttrTex0 + (client_active_unit_ < kMaxTexUnits ? client_active_unit_ : 0), size, type, false, stride, pointer, false);
}
void GlApi::glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, UserPtr pointer) {
  if (index >= webgl::kMaxAttribs || ((size < 1 || size > 4) && size != GL_BGRA)) { error(GL_INVALID_VALUE); return; }
  client_pointer(index, size, type, normalized != 0, stride, pointer, false);
}
void GlApi::glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, UserPtr pointer) {
  if (index >= webgl::kMaxAttribs || size < 1 || size > 4) { error(GL_INVALID_VALUE); return; }
  if (type != GL_BYTE && type != GL_UNSIGNED_BYTE && type != GL_SHORT && type != GL_UNSIGNED_SHORT && type != GL_INT && type != GL_UNSIGNED_INT) { error(GL_INVALID_ENUM); return; }
  client_pointer(index, size, type, false, stride, pointer, true);
}

void GlApi::glEnableVertexAttribArray(GLuint index) {
  if (index >= webgl::kMaxAttribs) { error(GL_INVALID_VALUE); return; }
  gl_.enable_vertex_attrib(index, true);
}
void GlApi::glDisableVertexAttribArray(GLuint index) {
  if (index >= webgl::kMaxAttribs) { error(GL_INVALID_VALUE); return; }
  gl_.enable_vertex_attrib(index, false);
}
void GlApi::glVertexAttribDivisor(GLuint index, GLuint divisor) {
  if (index >= webgl::kMaxAttribs) { error(GL_INVALID_VALUE); return; }
  gl_.vertex_attrib_divisor(index, divisor);
}

void GlApi::client_state(uint32_t array, bool on) {
  uint32_t attr;
  switch (array) {
    case GL_VERTEX_ARRAY: attr = kAttrVertex; break;
    case GL_NORMAL_ARRAY: attr = kAttrNormal; break;
    case GL_COLOR_ARRAY: attr = kAttrColor; break;
    case GL_SECONDARY_COLOR_ARRAY: attr = kAttrSecondary; break;
    case GL_FOG_COORD_ARRAY: attr = kAttrFog; break;
    case GL_INDEX_ARRAY: attr = kAttrIndex; break;
    case GL_EDGE_FLAG_ARRAY: attr = kAttrEdge; break;
    case GL_TEXTURE_COORD_ARRAY: attr = kAttrTex0 + (client_active_unit_ < kMaxTexUnits ? client_active_unit_ : 0); break;
    default: error(GL_INVALID_ENUM); return;
  }
  gl_.enable_vertex_attrib(attr, on);
}

void GlApi::glEnableClientState(GLenum array) { client_state(array, true); }
void GlApi::glDisableClientState(GLenum array) { client_state(array, false); }

void GlApi::glClientActiveTexture(GLenum texture) {
  if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + kMaxTexUnits) { error(GL_INVALID_ENUM); return; }
  client_active_unit_ = texture - GL_TEXTURE0;
}

void GlApi::glInterleavedArrays(GLenum format, GLsizei stride, UserPtr pointer) {
  if (stride < 0) { error(GL_INVALID_VALUE); return; }
  // tex coords, colour, normal, vertex: sizes and whether each comes first.
  int32_t ts = 0, cs = 0, vs = 0;
  bool normal = false;
  uint32_t ctype = GL_FLOAT;
  uint32_t pc = 0, pn = 0, pv = 0;
  uint32_t total = 0;
  switch (format) {
    case GL_V2F: vs = 2; pv = 0; total = 8; break;
    case GL_V3F: vs = 3; pv = 0; total = 12; break;
    case GL_C4UB_V2F: cs = 4; ctype = GL_UNSIGNED_BYTE; vs = 2; pc = 0; pv = 4; total = 12; break;
    case GL_C4UB_V3F: cs = 4; ctype = GL_UNSIGNED_BYTE; vs = 3; pc = 0; pv = 4; total = 16; break;
    case GL_C3F_V3F: cs = 3; vs = 3; pc = 0; pv = 12; total = 24; break;
    case GL_N3F_V3F: normal = true; vs = 3; pn = 0; pv = 12; total = 24; break;
    case GL_C4F_N3F_V3F: cs = 4; normal = true; vs = 3; pc = 0; pn = 16; pv = 28; total = 40; break;
    case GL_T2F_V3F: ts = 2; vs = 3; pv = 8; total = 20; break;
    case GL_T4F_V4F: ts = 4; vs = 4; pv = 16; total = 32; break;
    case GL_T2F_C4UB_V3F: ts = 2; cs = 4; ctype = GL_UNSIGNED_BYTE; vs = 3; pc = 8; pv = 12; total = 24; break;
    case GL_T2F_C3F_V3F: ts = 2; cs = 3; vs = 3; pc = 8; pv = 20; total = 32; break;
    case GL_T2F_N3F_V3F: ts = 2; normal = true; vs = 3; pn = 8; pv = 20; total = 32; break;
    case GL_T2F_C4F_N3F_V3F: ts = 2; cs = 4; normal = true; vs = 3; pc = 8; pn = 24; pv = 36; total = 48; break;
    case GL_T4F_C4F_N3F_V4F: ts = 4; cs = 4; normal = true; vs = 4; pc = 16; pn = 32; pv = 44; total = 60; break;
    default: error(GL_INVALID_ENUM); return;
  }
  const int32_t s = stride ? stride : static_cast<int32_t>(total);
  client_state(GL_EDGE_FLAG_ARRAY, false);
  client_state(GL_INDEX_ARRAY, false);
  client_state(GL_FOG_COORD_ARRAY, false);
  client_state(GL_SECONDARY_COLOR_ARRAY, false);
  client_state(GL_TEXTURE_COORD_ARRAY, ts != 0);
  if (ts) glTexCoordPointer(ts, GL_FLOAT, s, pointer);
  client_state(GL_COLOR_ARRAY, cs != 0);
  if (cs) glColorPointer(cs, ctype, s, pointer.plus(pc));
  client_state(GL_NORMAL_ARRAY, normal);
  if (normal) glNormalPointer(GL_FLOAT, s, pointer.plus(pn));
  client_state(GL_VERTEX_ARRAY, true);
  glVertexPointer(vs, GL_FLOAT, s, pointer.plus(pv));
}

// --- Draws -------------------------------------------------------------------------------------------------------------------------------------------------

bool GlApi::upload_client_arrays(int64_t lowest, int64_t highest, uint32_t instances) {
  webgl::Vao* vao = gl_.current_vao_ptr();
  const auto required = [&](uint32_t attr) {
    if (gl_.program_ == 0) return false;
    return true;
    (void)attr;
  };
  (void)required;
  for (uint32_t attr = 0; attr < webgl::kMaxAttribSlots; ++attr) {
    webgl::VertexAttrib& at = vao->attribs[attr];
    if (!at.enabled || !at.from_client) continue;
    if (at.client == 0) { at.enabled = false; continue; }      // (a null array: nothing to fetch)
    const uint32_t element = attrib_element_bytes(at);
    const uint64_t stride = at.stride ? at.stride : element;
    const uint64_t last = at.divisor ? (uint64_t{instances} - 1) / at.divisor : static_cast<uint64_t>(highest);
    const uint64_t first = at.divisor ? 0 : static_cast<uint64_t>(lowest);
    if (last > (1ull << 31) || last * stride > (1ull << 31)) { error(GL_OUT_OF_MEMORY); return false; }
    const uint64_t bytes = last * stride + element;
    if (client_buffers_[attr] == 0) client_buffers_[attr] = gl_.create_buffer();
    webgl::Buffer* buffer = gl_.buffer_at(client_buffers_[attr]);
    if (!buffer) { error(GL_OUT_OF_MEMORY); return false; }
    if (buffer->data.size() < bytes && !buffer->data.put_zeros(bytes - buffer->data.size())) { error(GL_OUT_OF_MEMORY); return false; }
    ByteView all = buffer->data.writable_view();
    // A tightly packed array is one read; otherwise element by element, so that memory between elements need not be readable.
    if (stride == element) {
      if (!read_client(UserPtr{at.client + first * stride}, all.subview(first * stride, (last - first + 1) * stride))) { error(GL_INVALID_OPERATION); return false; }
    } else {
      const uint64_t span = (last - first) * stride + element;
      if (!read_client(UserPtr{at.client + first * stride}, all.subview(first * stride, span))) {
        for (uint64_t i = first; i <= last; ++i)
          if (!read_client(UserPtr{at.client + i * stride}, all.subview(i * stride, element))) { error(GL_INVALID_OPERATION); return false; }
      }
    }
    at.buffer = client_buffers_[attr];
    at.offset = 0;
    if (at.stride == 0) at.stride = element;
  }
  return true;
}

// Selects the program: the user's, or the fixed-function one for this state; loads what the program needs from the state.
bool GlApi::prepare_draw(uint32_t, int64_t lowest, int64_t highest, uint32_t instances) {
  webgl::DesktopState& ds = gl_.desktop;
  if (user_program_ != 0) {
    gl_.program_ = user_program_;
    fixed_current_ = nullptr;
    ds.flat_count = 0;
    ds.clip_mask = 0;
    load_compat_uniforms(user_program_);
  } else {
    if (config_.core_profile) { error(GL_INVALID_OPERATION); return false; }
    FixedKey key;
    make_fixed_key(&key);
    FixedProgram* p = fixed_program(key);
    if (!p) { error(GL_OUT_OF_MEMORY); return false; }
    fixed_current_ = p;
    gl_.program_ = p->program;
    load_fixed_uniforms(*p);
    ds.flat_first = 0;
    ds.flat_count = p->flat_count;
    ds.clip_mask = fx_.clip_enabled;
    ds.clip_varying = p->clip_first;
    ds.ff_1d_mask = 0;
    ds.ff_rect_mask = 0;
    for (uint32_t i = 0; i < kMaxTexUnits; ++i) {
      if (key.tex_dim[i] == kDimFixed1D) ds.ff_1d_mask |= 1u << i;
      if (key.tex_dim[i] == kDimFixedRect) ds.ff_rect_mask |= 1u << i;
    }
    // Values for arrays that are not enabled.
    webgl::Vao* vao = gl_.current_vao_ptr();
    const auto current = [&](uint32_t attr, const float* v, uint32_t n) {
      webgl::VertexAttrib& at = vao->attribs[attr];
      for (uint32_t i = 0; i < 4; ++i) at.cur_f[i] = i < n ? v[i] : (i == 3 ? 1.0f : 0.0f);
      at.cur_is_int = false;
    };
    current(kAttrNormal, fx_.current_normal, 3);
    current(kAttrColor, fx_.current_color, 4);
    current(kAttrSecondary, fx_.current_secondary, 4);
    current(kAttrFog, &fx_.current_fog, 1);
    for (uint32_t i = 0; i < kMaxTexUnits; ++i) current(kAttrTex0 + i, fx_.current_tex[i], 4);
    const webgl::VertexAttrib& vertex = vao->attribs[kAttrVertex];
    if (!vertex.enabled) return false;      // without a vertex array the compatibility profile draws nothing
  }
  if (!upload_client_arrays(lowest, highest, instances)) return false;
  return true;
}

void GlApi::finish_draw() {
  gl_.desktop.triangle_edges = ConstByteView();
  gl_.desktop.base_vertex = 0;
  gl_.desktop.base_instance = 0;
  // glUseProgram state lives here, not in the engine's current program: uniforms and so on name programs directly.
  gl_.program_ = user_program_;
}

// Any draw: `indexed` with indices in the element buffer at the offset, or in the program's memory. Quads and polygons become triangles here; a list of indices then goes in a hidden
// element buffer, bound for this draw.
void GlApi::draw_any(uint32_t mode, int32_t first, int32_t count, uint32_t type, UserPtr indices, bool indexed, int32_t instances, int32_t base_vertex, uint32_t base_instance) {
  if (count < 0 || instances < 0) { error(GL_INVALID_VALUE); return; }
  if (!indexed && first < 0) { error(GL_INVALID_VALUE); return; }
  if (mode > GL_PATCHES) { error(GL_INVALID_ENUM); return; }
  if (config_.core_profile && gl_.vao_id_ == 0) { error(GL_INVALID_OPERATION); return; }
  if (indexed && type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT) { error(GL_INVALID_ENUM); return; }
  if (count == 0 || instances == 0) return;
  if (render_mode_ != GL_RENDER && !gl_.desktop.primitive_sink) gl_.desktop.primitive_sink = feedback_sink();
  if (coherent_maps_) sync_coherent_maps();

  webgl::Vao* vao = gl_.current_vao_ptr();
  const uint32_t isize = indexed ? index_size(type) : 0;
  ConstByteView index_bytes;
  ByteBuffer client_indices(a_);
  const webgl::Buffer* element = indexed ? gl_.buffer_at(vao->element_buffer) : nullptr;
  uint64_t index_offset = 0;
  if (indexed) {
    const uint64_t bytes = uint64_t{static_cast<uint32_t>(count)} * isize;
    if (element) {
      index_offset = indices.address;
      if (index_offset > element->data.size() || bytes > element->data.size() - index_offset) { error(GL_INVALID_OPERATION); return; }
      index_bytes = element->data.view().subview(index_offset, bytes);
    } else {
      if (indices.null() || !client_indices.put_zeros(bytes)) { error(indices.null() ? GL_INVALID_OPERATION : GL_OUT_OF_MEMORY); return; }
      if (!read_client(indices, client_indices.writable_view())) { error(GL_INVALID_OPERATION); return; }
      index_bytes = client_indices.view();
    }
  }
  // The range of vertices, for copying client arrays.
  int64_t lowest = first, highest = int64_t{first} + count - 1;
  if (indexed) {
    lowest = 0xFFFFFFFFll;
    highest = 0;
    const bool restarts = gl_.desktop.primitive_restart || gl_.desktop.primitive_restart_fixed;
    const uint32_t restart = gl_.desktop.primitive_restart_fixed ? (type == GL_UNSIGNED_BYTE ? 0xFFu : type == GL_UNSIGNED_SHORT ? 0xFFFFu : 0xFFFFFFFFu) : gl_.desktop.restart_index;
    bool any = false;
    for (int32_t i = 0; i < count; ++i) {
      const uint32_t v = read_index_at(index_bytes, isize, static_cast<uint64_t>(i));
      if (restarts && v == restart) continue;
      const int64_t rebased = int64_t{v} + base_vertex;
      if (rebased < 0) { error(GL_INVALID_OPERATION); return; }
      if (rebased < lowest) lowest = rebased;
      if (rebased > highest) highest = rebased;
      any = true;
    }
    if (!any) return;
  }
  if (!prepare_draw(mode, lowest, highest, static_cast<uint32_t>(instances))) { finish_draw(); return; }
  gl_.desktop.base_vertex = base_vertex;
  gl_.desktop.base_instance = base_instance;
  const bool polygon_modes = gl_.desktop.polygon_mode_front != GL_FILL || gl_.desktop.polygon_mode_back != GL_FILL;
  if (polygon_modes && vao->attribs[kAttrEdge].enabled) gl_.desktop.edge_flag_attrib = kAttrEdge;
  else gl_.desktop.edge_flag_attrib = -1;

  if (mode < GL_QUADS || mode > GL_POLYGON) {
    // The engine's own primitives.
    if (indexed) {
      if (element) {
        gl_.draw_elements(mode, count, type, index_offset, instances);
      } else {
        webgl::Buffer* hidden = gl_.buffer_at(index_buffer_);
        hidden->data.clear();
        if (!hidden->data.append(index_bytes)) { error(GL_OUT_OF_MEMORY); finish_draw(); return; }
        const uint32_t saved = vao->element_buffer;
        vao->element_buffer = index_buffer_;
        gl_.draw_elements(mode, count, type, 0, instances);
        vao->element_buffer = saved;
      }
    } else {
      gl_.draw_arrays(mode, first, count, instances);
    }
    finish_draw();
    return;
  }

  // Quads, quad strips and polygons: triangles whose last vertex is the one flat shading takes, with the inner edges marked for the polygon modes.
  webgl::Buffer* hidden = gl_.buffer_at(index_buffer_);
  hidden->data.clear();
  tri_edges_.clear();
  const auto vertex = [&](int32_t i) -> uint32_t { return indexed ? read_index_at(index_bytes, isize, static_cast<uint64_t>(i)) : static_cast<uint32_t>(first + i); };
  const auto triangle = [&](int32_t a, int32_t b, int32_t c, uint8_t edges) {
    hidden->data.put32(vertex(a));
    hidden->data.put32(vertex(b));
    hidden->data.put32(vertex(c));
    tri_edges_.push_back(edges);
  };
  if (mode == GL_QUADS) {
    for (int32_t i = 0; i + 3 < count; i += 4) {
      triangle(i, i + 1, i + 3, 1 | 4);
      triangle(i + 1, i + 2, i + 3, 1 | 2);
    }
  } else if (mode == GL_QUAD_STRIP) {
    for (int32_t i = 0; i + 3 < count; i += 2) {
      triangle(i, i + 1, i + 3, 1 | 2 << 0);
      triangle(i, i + 3, i + 2, 2 | 4);
    }
  } else {      // polygon: flat shading takes its first vertex, so that one comes last in each triangle
    for (int32_t i = 1; i + 1 < count; ++i) {
      uint8_t edges = 1;
      if (i == 1) edges |= 4;
      if (i + 2 == count) edges |= 2;
      triangle(i, i + 1, 0, edges);
    }
  }
  const uint32_t tri_indices = static_cast<uint32_t>(hidden->data.size() / 4);
  if (tri_indices) {
    const uint32_t saved = vao->element_buffer;
    vao->element_buffer = index_buffer_;
    if (polygon_modes) gl_.desktop.triangle_edges = ConstByteView(tri_edges_.view().data(), tri_edges_.count());
    gl_.draw_elements(GL_TRIANGLES, static_cast<int32_t>(tri_indices), GL_UNSIGNED_INT, 0, instances);
    vao->element_buffer = saved;
  }
  finish_draw();
}

void GlApi::glDrawArrays(GLenum mode, GLint first, GLsizei count) { draw_any(mode, first, count, 0, UserPtr{}, false, 1, 0, 0); }
void GlApi::glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) { draw_any(mode, first, count, 0, UserPtr{}, false, instancecount, 0, 0); }
void GlApi::glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount, GLuint baseinstance) { draw_any(mode, first, count, 0, UserPtr{}, false, instancecount, 0, baseinstance); }
void GlApi::glDrawElements(GLenum mode, GLsizei count, GLenum type, UserPtr indices) { draw_any(mode, 0, count, type, indices, true, 1, 0, 0); }
void GlApi::glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, UserPtr indices, GLsizei instancecount) { draw_any(mode, 0, count, type, indices, true, instancecount, 0, 0); }
void GlApi::glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, UserPtr indices, GLint basevertex) { draw_any(mode, 0, count, type, indices, true, 1, basevertex, 0); }
void GlApi::glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, UserPtr indices, GLsizei instancecount, GLint basevertex) { draw_any(mode, 0, count, type, indices, true, instancecount, basevertex, 0); }
void GlApi::glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, UserPtr indices, GLsizei instancecount, GLuint baseinstance) { draw_any(mode, 0, count, type, indices, true, instancecount, 0, baseinstance); }
void GlApi::glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, UserPtr indices, GLsizei instancecount, GLint basevertex, GLuint baseinstance) { draw_any(mode, 0, count, type, indices, true, instancecount, basevertex, baseinstance); }
void GlApi::glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, UserPtr indices) {
  if (end < start) { error(GL_INVALID_VALUE); return; }
  draw_any(mode, 0, count, type, indices, true, 1, 0, 0);
}
void GlApi::glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, UserPtr indices, GLint basevertex) {
  if (end < start) { error(GL_INVALID_VALUE); return; }
  draw_any(mode, 0, count, type, indices, true, 1, basevertex, 0);
}

void GlApi::glMultiDrawArrays(GLenum mode, UserPtr first, UserPtr count, GLsizei drawcount) {
  if (drawcount < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < drawcount; ++i) {
    int32_t f = 0, c = 0;
    if (!read_values(first.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &f, 1) || !read_values(count.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &c, 1)) { error(GL_INVALID_VALUE); return; }
    draw_any(mode, f, c, 0, UserPtr{}, false, 1, 0, 0);
  }
}

void GlApi::glMultiDrawElements(GLenum mode, UserPtr count, GLenum type, UserPtr indices, GLsizei drawcount) {
  if (drawcount < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < drawcount; ++i) {
    int32_t c = 0;
    uint64_t at = 0;
    if (!read_values(count.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &c, 1) || !read_values(indices.plus(uint64_t{static_cast<uint32_t>(i)} * 8), &at, 1)) { error(GL_INVALID_VALUE); return; }
    draw_any(mode, 0, c, type, UserPtr{at}, true, 1, 0, 0);
  }
}

void GlApi::glMultiDrawElementsBaseVertex(GLenum mode, UserPtr count, GLenum type, UserPtr indices, GLsizei drawcount, UserPtr basevertex) {
  if (drawcount < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < drawcount; ++i) {
    int32_t c = 0, b = 0;
    uint64_t at = 0;
    if (!read_values(count.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &c, 1) || !read_values(indices.plus(uint64_t{static_cast<uint32_t>(i)} * 8), &at, 1) || !read_values(basevertex.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &b, 1)) { error(GL_INVALID_VALUE); return; }
    draw_any(mode, 0, c, type, UserPtr{at}, true, 1, b, 0);
  }
}

// One array element: all enabled arrays fetched as if by the current-value commands.
void GlApi::glArrayElement(GLint i) {
  if (i < 0) { error(GL_INVALID_VALUE); return; }
  webgl::Vao* vao = gl_.current_vao_ptr();
  const auto fetch = [&](uint32_t attr, float out[4]) -> bool {
    const webgl::VertexAttrib& at = vao->attribs[attr];
    if (!at.enabled) return false;
    const uint32_t element = attrib_element_bytes(at);
    const uint64_t stride = at.stride ? at.stride : element;
    uint8_t raw[32];
    bool ok;
    if (at.from_client) {
      ok = element <= sizeof(raw) && read_client(UserPtr{at.client + static_cast<uint64_t>(i) * stride}, ByteView(raw, element));
    } else {
      const webgl::Buffer* b = gl_.buffer_at(at.buffer);
      ConstByteView from;
      ok = b && element <= sizeof(raw) && b->data.view().try_subview(at.offset + static_cast<uint64_t>(i) * stride, element, &from);
      if (ok) ByteView(raw, element).copy_from(from);
    }
    if (!ok) return false;
    uint32_t lanes[4];
    webgl::read_vertex_attrib(at, raw, lanes);
    for (int k = 0; k < 4; ++k) out[k] = __builtin_bit_cast(float, lanes[k]);
    return true;
  };
  float v[4];
  if (fetch(kAttrNormal, v)) set_normal(v[0], v[1], v[2]);
  if (fetch(kAttrColor, v)) set_color(v[0], v[1], v[2], v[3]);
  if (fetch(kAttrSecondary, v)) set_secondary(v[0], v[1], v[2]);
  if (fetch(kAttrFog, v)) set_fog_coord(v[0]);
  if (fetch(kAttrEdge, v)) set_edge(v[0] != 0);
  for (uint32_t u = 0; u < kMaxTexUnits; ++u) if (fetch(kAttrTex0 + u, v)) set_texcoord(u, v[0], v[1], v[2], v[3]);
  for (uint32_t g = 1; g < webgl::kMaxAttribs; ++g) if (fetch(g, v)) set_attrib(g, v[0], v[1], v[2], v[3]);
  if (fetch(kAttrVertex, v)) imm_vertex(v[0], v[1], v[2], v[3]);
}

// A draw inside a display list: what the arrays hold now, stored as glBegin ... glEnd commands. (Replaces the command in the list; a glArrayElement is one vertex.)
bool GlApi::list_record_draw(uint32_t op, ConstByteView command) {
  // Evaluated by running it into a temporary immediate buffer is not possible without drawing, so the rows are written as the commands they are made of.
  return list_expand_draw(op, command);
}

}  // namespace opengl
