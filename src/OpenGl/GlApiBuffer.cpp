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

// Buffer objects, vertex array objects, renderbuffers and framebuffers.
#include "OpenGl/GlApi.hpp"
#include "OpenGl/GlSurface.hpp"
#include "OpenGl/generated/GlEnums.inc"

namespace opengl {

// --- Buffers --------------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::glGenBuffers(GLsizei n, UserPtr buffers) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t name = group_->buffers.generate();
    if (!write_values(buffers.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
  }
}
void GlApi::glCreateBuffers(GLsizei n, UserPtr buffers) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t name = group_->buffers.generate();
    engine_buffer(name);
    if (!write_values(buffers.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
  }
}

uint32_t GlApi::engine_buffer(uint32_t name) {
  uint32_t object = group_->buffers.object(name);
  if (!object) {
    object = gl_.create_buffer();
    group_->buffers.set(name, object);
  }
  return object;
}

void GlApi::glDeleteBuffers(GLsizei n, UserPtr buffers) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    uint32_t name = 0;
    if (!read_values(buffers.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
    if (!name) continue;
    const uint32_t object = group_->buffers.object(name);
    if (object) {
      unmap_engine_buffer(object, false);
      gl_.delete_object('b', object);
    }
    group_->buffers.erase(name);
  }
}

uint64_t GlApi::glIsBuffer(GLuint buffer) { return buffer && group_->buffers.object(buffer) != 0; }

void GlApi::glBindBuffer(GLenum target, GLuint buffer) {
  if (buffer && config_.core_profile && !group_->buffers.known(buffer)) { error(GL_INVALID_OPERATION); return; }
  gl_.bind_buffer(target, buffer ? engine_buffer(buffer) : 0);
}

void GlApi::buffer_data_named(uint32_t object, int64_t size, UserPtr data, uint32_t usage) {
  if (size < 0) { error(GL_INVALID_VALUE); return; }
  webgl::Buffer* b = gl_.buffer_at(object);
  if (!b) { error(GL_INVALID_OPERATION); return; }
  if (b->immutable) { error(GL_INVALID_OPERATION); return; }
  unmap_engine_buffer(object, false);
  b->usage = usage;
  b->data.clear();
  if (!b->data.put_zeros(static_cast<uint64_t>(size))) { error(GL_OUT_OF_MEMORY); return; }
  if (!data.null() && size && !read_client(data, b->data.writable_view())) error(GL_INVALID_VALUE);
}

void GlApi::glBufferData(GLenum target, GLsizeiptr size, UserPtr data, GLenum usage) {
  const uint32_t object = gl_.bound_buffer(target);
  buffer_data_named(object, size, data, usage);
}
void GlApi::glNamedBufferData(GLuint buffer, GLsizeiptr size, UserPtr data, GLenum usage) { buffer_data_named(group_->buffers.object(buffer), size, data, usage); }

void GlApi::buffer_storage_named(uint32_t object, int64_t size, UserPtr data, uint32_t flags) {
  if (size <= 0) { error(GL_INVALID_VALUE); return; }
  webgl::Buffer* b = gl_.buffer_at(object);
  if (!b || b->immutable) { error(GL_INVALID_OPERATION); return; }
  b->immutable = true;
  b->storage_flags = flags;
  b->data.clear();
  if (!b->data.put_zeros(static_cast<uint64_t>(size))) { error(GL_OUT_OF_MEMORY); return; }
  if (!data.null() && !read_client(data, b->data.writable_view())) error(GL_INVALID_VALUE);
}
void GlApi::glBufferStorage(GLenum target, GLsizeiptr size, UserPtr data, GLbitfield flags) { buffer_storage_named(gl_.bound_buffer(target), size, data, flags); }
void GlApi::glNamedBufferStorage(GLuint buffer, GLsizeiptr size, UserPtr data, GLbitfield flags) { buffer_storage_named(group_->buffers.object(buffer), size, data, flags); }

void GlApi::buffer_sub_data_named(uint32_t object, int64_t offset, int64_t size, UserPtr data) {
  if (offset < 0 || size < 0) { error(GL_INVALID_VALUE); return; }
  webgl::Buffer* b = gl_.buffer_at(object);
  if (!b) { error(GL_INVALID_OPERATION); return; }
  ByteView part;
  if (!b->data.writable_view().try_subview(static_cast<uint64_t>(offset), static_cast<uint64_t>(size), &part)) { error(GL_INVALID_VALUE); return; }
  if (size && !read_client(data, part)) error(GL_INVALID_VALUE);
}
void GlApi::glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, UserPtr data) { buffer_sub_data_named(gl_.bound_buffer(target), offset, size, data); }
void GlApi::glNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, UserPtr data) { buffer_sub_data_named(group_->buffers.object(buffer), offset, size, data); }

void GlApi::get_buffer_sub_data_named(uint32_t object, int64_t offset, int64_t size, UserPtr data) {
  if (offset < 0 || size < 0) { error(GL_INVALID_VALUE); return; }
  const webgl::Buffer* b = gl_.buffer_at(object);
  if (!b) { error(GL_INVALID_OPERATION); return; }
  ConstByteView part;
  if (!b->data.view().try_subview(static_cast<uint64_t>(offset), static_cast<uint64_t>(size), &part)) { error(GL_INVALID_VALUE); return; }
  if (!write_client(data, part)) error(GL_INVALID_VALUE);
}
void GlApi::glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, UserPtr data) { get_buffer_sub_data_named(gl_.bound_buffer(target), offset, size, data); }
void GlApi::glGetNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, UserPtr data) { get_buffer_sub_data_named(group_->buffers.object(buffer), offset, size, data); }

void GlApi::glCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size) {
  copy_buffer_named(gl_.bound_buffer(readTarget), gl_.bound_buffer(writeTarget), readOffset, writeOffset, size);
}
void GlApi::glCopyNamedBufferSubData(GLuint readBuffer, GLuint writeBuffer, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size) {
  copy_buffer_named(group_->buffers.object(readBuffer), group_->buffers.object(writeBuffer), readOffset, writeOffset, size);
}
void GlApi::copy_buffer_named(uint32_t from, uint32_t to, int64_t ro, int64_t wo, int64_t size) {
  webgl::Buffer* r = gl_.buffer_at(from);
  webgl::Buffer* w = gl_.buffer_at(to);
  if (!r || !w) { error(GL_INVALID_OPERATION); return; }
  if (ro < 0 || wo < 0 || size < 0) { error(GL_INVALID_VALUE); return; }
  ConstByteView src;
  ByteView dst;
  if (!r->data.view().try_subview(static_cast<uint64_t>(ro), static_cast<uint64_t>(size), &src) || !w->data.writable_view().try_subview(static_cast<uint64_t>(wo), static_cast<uint64_t>(size), &dst)) { error(GL_INVALID_VALUE); return; }
  if (r == w) {      // overlap in one buffer goes by way of a copy
    ByteBuffer temp(a_);
    if (!temp.append(src)) { error(GL_OUT_OF_MEMORY); return; }
    dst.copy_from(temp.view());
    return;
  }
  dst.copy_from(src);
}

// A mapping is memory in the program, filled from the buffer; the contents return on unmap or flush. A persistent, coherent mapping is also read back before each draw.
uint64_t GlApi::map_named(uint32_t object, int64_t offset, int64_t length, uint32_t access) {
  webgl::Buffer* b = gl_.buffer_at(object);
  if (!b) { error(GL_INVALID_OPERATION); return 0; }
  if (offset < 0 || length < 0 || uint64_t(offset) > b->data.size() || uint64_t(length) > b->data.size() - uint64_t(offset)) { error(GL_INVALID_VALUE); return 0; }
  if (!(access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT))) { error(GL_INVALID_OPERATION); return 0; }
  if (find_mapping(object)) { error(GL_INVALID_OPERATION); return 0; }
  if (!memory_) return 0;
  const uint64_t pages = (static_cast<uint64_t>(length) + 4095) & ~uint64_t{4095};
  const uint64_t address = memory_->allocate(pages ? pages : 4096);
  if (!address) { error(GL_OUT_OF_MEMORY); return 0; }
  if (!(access & GL_MAP_INVALIDATE_RANGE_BIT) && !(access & GL_MAP_INVALIDATE_BUFFER_BIT) && length) {
    memory_->write(address, b->data.view().subview(static_cast<uint64_t>(offset), static_cast<uint64_t>(length)));
  }
  Mapping m;
  m.object = object;
  m.address = address;
  m.pages = pages ? pages : 4096;
  m.offset = static_cast<uint64_t>(offset);
  m.length = static_cast<uint64_t>(length);
  m.access = access;
  mappings_.push_back(m);
  if ((access & GL_MAP_PERSISTENT_BIT) && (access & GL_MAP_COHERENT_BIT) && (access & GL_MAP_WRITE_BIT)) coherent_maps_ = true;
  return address;
}

GlApi::Mapping* GlApi::find_mapping(uint32_t object) {
  for (uint64_t i = 0; i < mappings_.count(); ++i) if (mappings_[i].object == object) return &mappings_[i];
  return nullptr;
}

bool GlApi::unmap_engine_buffer(uint32_t object, bool copy_back) {
  for (uint64_t i = 0; i < mappings_.count(); ++i) {
    Mapping& m = mappings_[i];
    if (m.object != object) continue;
    webgl::Buffer* b = gl_.buffer_at(object);
    if (copy_back && b && (m.access & GL_MAP_WRITE_BIT) && !(m.access & GL_MAP_FLUSH_EXPLICIT_BIT) && m.length) {
      memory_->read(m.address, b->data.writable_view().subview(m.offset, m.length));
    }
    if (memory_) memory_->release(m.address, m.pages);
    mappings_.remove_at(i);
    coherent_maps_ = false;
    for (uint64_t k = 0; k < mappings_.count(); ++k) if ((mappings_[k].access & GL_MAP_PERSISTENT_BIT) && (mappings_[k].access & GL_MAP_COHERENT_BIT) && (mappings_[k].access & GL_MAP_WRITE_BIT)) coherent_maps_ = true;
    return true;
  }
  return false;
}

void GlApi::sync_coherent_maps() {
  for (uint64_t i = 0; i < mappings_.count(); ++i) {
    const Mapping& m = mappings_[i];
    if (!(m.access & GL_MAP_PERSISTENT_BIT) || !(m.access & GL_MAP_COHERENT_BIT) || !(m.access & GL_MAP_WRITE_BIT) || !m.length) continue;
    webgl::Buffer* b = gl_.buffer_at(m.object);
    if (b) memory_->read(m.address, b->data.writable_view().subview(m.offset, m.length));
  }
}

uint64_t GlApi::glMapBuffer(GLenum target, GLenum access) {
  const uint32_t object = gl_.bound_buffer(target);
  webgl::Buffer* b = gl_.buffer_at(object);
  if (!b) { error(GL_INVALID_OPERATION); return 0; }
  uint32_t bits = 0;
  if (access == GL_READ_ONLY) bits = GL_MAP_READ_BIT;
  else if (access == GL_WRITE_ONLY) bits = GL_MAP_WRITE_BIT;
  else if (access == GL_READ_WRITE) bits = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
  else { error(GL_INVALID_ENUM); return 0; }
  return map_named(object, 0, static_cast<int64_t>(b->data.size()), bits);
}
uint64_t GlApi::glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) { return map_named(gl_.bound_buffer(target), offset, length, access); }
uint64_t GlApi::glMapNamedBuffer(GLuint buffer, GLenum access) {
  const uint32_t object = group_->buffers.object(buffer);
  const webgl::Buffer* b = gl_.buffer_at(object);
  if (!b) { error(GL_INVALID_OPERATION); return 0; }
  return map_named(object, 0, static_cast<int64_t>(b->data.size()), access == GL_READ_ONLY ? GL_MAP_READ_BIT : access == GL_WRITE_ONLY ? GL_MAP_WRITE_BIT : GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
}
uint64_t GlApi::glMapNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access) { return map_named(group_->buffers.object(buffer), offset, length, access); }
uint64_t GlApi::glUnmapBuffer(GLenum target) {
  if (!unmap_engine_buffer(gl_.bound_buffer(target), true)) { error(GL_INVALID_OPERATION); return 0; }
  return 1;
}
uint64_t GlApi::glUnmapNamedBuffer(GLuint buffer) {
  if (!unmap_engine_buffer(group_->buffers.object(buffer), true)) { error(GL_INVALID_OPERATION); return 0; }
  return 1;
}
void GlApi::flush_range(uint32_t object, int64_t offset, int64_t length) {
  Mapping* m = find_mapping(object);
  if (!m || !(m->access & GL_MAP_FLUSH_EXPLICIT_BIT)) { error(GL_INVALID_OPERATION); return; }
  if (offset < 0 || length < 0 || uint64_t(offset) > m->length || uint64_t(length) > m->length - uint64_t(offset)) { error(GL_INVALID_VALUE); return; }
  webgl::Buffer* b = gl_.buffer_at(object);
  if (b && length) memory_->read(m->address + static_cast<uint64_t>(offset), b->data.writable_view().subview(m->offset + static_cast<uint64_t>(offset), static_cast<uint64_t>(length)));
}
void GlApi::glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) { flush_range(gl_.bound_buffer(target), offset, length); }
void GlApi::glFlushMappedNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length) { flush_range(group_->buffers.object(buffer), offset, length); }

void GlApi::buffer_param_get(uint32_t object, uint32_t pname, int64_t* out) {
  const webgl::Buffer* b = gl_.buffer_at(object);
  if (!b) { error(GL_INVALID_OPERATION); return; }
  const Mapping* m = find_mapping(object);
  switch (pname) {
    case GL_BUFFER_SIZE: *out = static_cast<int64_t>(b->data.size()); break;
    case GL_BUFFER_USAGE: *out = b->usage; break;
    case GL_BUFFER_ACCESS: *out = !m ? GL_READ_WRITE : ((m->access & GL_MAP_READ_BIT) && (m->access & GL_MAP_WRITE_BIT)) ? GL_READ_WRITE : (m->access & GL_MAP_WRITE_BIT) ? GL_WRITE_ONLY : GL_READ_ONLY; break;
    case GL_BUFFER_ACCESS_FLAGS: *out = m ? m->access : 0; break;
    case GL_BUFFER_MAPPED: *out = m != nullptr; break;
    case GL_BUFFER_MAP_OFFSET: *out = m ? static_cast<int64_t>(m->offset) : 0; break;
    case GL_BUFFER_MAP_LENGTH: *out = m ? static_cast<int64_t>(m->length) : 0; break;
    case GL_BUFFER_IMMUTABLE_STORAGE: *out = b->immutable; break;
    case GL_BUFFER_STORAGE_FLAGS: *out = b->storage_flags; break;
    default: error(GL_INVALID_ENUM); break;
  }
}
void GlApi::glGetBufferParameteriv(GLenum target, GLenum pname, UserPtr params) {
  int64_t v = 0;
  buffer_param_get(gl_.bound_buffer(target), pname, &v);
  const int32_t i = static_cast<int32_t>(v);
  write_values(params, &i, 1);
}
void GlApi::glGetBufferParameteri64v(GLenum target, GLenum pname, UserPtr params) {
  int64_t v = 0;
  buffer_param_get(gl_.bound_buffer(target), pname, &v);
  write_values(params, &v, 1);
}
void GlApi::glGetNamedBufferParameteriv(GLuint buffer, GLenum pname, UserPtr params) {
  int64_t v = 0;
  buffer_param_get(group_->buffers.object(buffer), pname, &v);
  const int32_t i = static_cast<int32_t>(v);
  write_values(params, &i, 1);
}
void GlApi::glGetNamedBufferParameteri64v(GLuint buffer, GLenum pname, UserPtr params) {
  int64_t v = 0;
  buffer_param_get(group_->buffers.object(buffer), pname, &v);
  write_values(params, &v, 1);
}
void GlApi::glGetBufferPointerv(GLenum target, GLenum pname, UserPtr params) {
  if (pname != GL_BUFFER_MAP_POINTER) { error(GL_INVALID_ENUM); return; }
  const Mapping* m = find_mapping(gl_.bound_buffer(target));
  const uint64_t address = m ? m->address : 0;
  write_values(params, &address, 1);
}
void GlApi::glGetNamedBufferPointerv(GLuint buffer, GLenum pname, UserPtr params) {
  if (pname != GL_BUFFER_MAP_POINTER) { error(GL_INVALID_ENUM); return; }
  const Mapping* m = find_mapping(group_->buffers.object(buffer));
  const uint64_t address = m ? m->address : 0;
  write_values(params, &address, 1);
}

void GlApi::glBindBufferBase(GLenum target, GLuint index, GLuint buffer) { bind_range(target, index, buffer, 0, 0); }
void GlApi::glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
  if (buffer && (offset < 0 || size <= 0)) { error(GL_INVALID_VALUE); return; }
  bind_range(target, index, buffer, static_cast<uint64_t>(offset), static_cast<uint64_t>(size));
}
void GlApi::bind_range(uint32_t target, uint32_t index, uint32_t buffer, uint64_t offset, uint64_t size) {
  const uint32_t object = buffer ? engine_buffer(buffer) : 0;
  if (target == GL_UNIFORM_BUFFER || target == GL_TRANSFORM_FEEDBACK_BUFFER) {
    gl_.bind_buffer_range(target, index, object, offset, size);
    return;
  }
  if (target == GL_SHADER_STORAGE_BUFFER || target == GL_ATOMIC_COUNTER_BUFFER) {
    webgl::BufferBinding* list = target == GL_SHADER_STORAGE_BUFFER ? gl_.ssbo_bindings_ : gl_.atomic_bindings_;
    const uint32_t count = target == GL_SHADER_STORAGE_BUFFER ? webgl::kMaxStorageBindings : webgl::kMaxAtomicBindings;
    if (index >= count) { error(GL_INVALID_VALUE); return; }
    list[index].buffer = object;
    list[index].offset = offset;
    list[index].size = size;
    gl_.bind_buffer(target, object);
    return;
  }
  error(GL_INVALID_ENUM);
}

// --- Vertex array objects ---------------------------------------------------------------------------------------------------------------------------------------

void GlApi::glGenVertexArrays(GLsizei n, UserPtr arrays) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t name = vao_names_.generate();
    if (!write_values(arrays.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
  }
}
void GlApi::glCreateVertexArrays(GLsizei n, UserPtr arrays) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t name = vao_names_.generate();
    vao_names_.set(name, gl_.create_vao());
    if (!write_values(arrays.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
  }
}
void GlApi::glDeleteVertexArrays(GLsizei n, UserPtr arrays) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    uint32_t name = 0;
    if (!read_values(arrays.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
    if (!name) continue;
    const uint32_t object = vao_names_.object(name);
    if (object) gl_.delete_object('v', object);
    vao_names_.erase(name);
  }
}
uint64_t GlApi::glIsVertexArray(GLuint array) { return array && vao_names_.object(array) != 0; }
void GlApi::glBindVertexArray(GLuint array) {
  if (array == 0) { gl_.bind_vao(0); return; }
  if (!vao_names_.known(array)) { error(GL_INVALID_OPERATION); return; }
  uint32_t object = vao_names_.object(array);
  if (!object) {
    object = gl_.create_vao();
    vao_names_.set(array, object);
  }
  gl_.bind_vao(object);
}

// --- Renderbuffers ----------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::glGenRenderbuffers(GLsizei n, UserPtr renderbuffers) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t name = group_->renderbuffers.generate();
    if (!write_values(renderbuffers.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
  }
}
void GlApi::glDeleteRenderbuffers(GLsizei n, UserPtr renderbuffers) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    uint32_t name = 0;
    if (!read_values(renderbuffers.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
    if (!name) continue;
    const uint32_t object = group_->renderbuffers.object(name);
    if (object) gl_.delete_object('r', object);
    group_->renderbuffers.erase(name);
  }
}
uint64_t GlApi::glIsRenderbuffer(GLuint renderbuffer) { return renderbuffer && group_->renderbuffers.object(renderbuffer) != 0; }
void GlApi::glBindRenderbuffer(GLenum target, GLuint renderbuffer) {
  if (target != GL_RENDERBUFFER) { error(GL_INVALID_ENUM); return; }
  if (!renderbuffer) { gl_.bind_renderbuffer(0); return; }
  uint32_t object = group_->renderbuffers.object(renderbuffer);
  if (!object) {
    object = gl_.create_renderbuffer();
    group_->renderbuffers.set(renderbuffer, object);
  }
  gl_.bind_renderbuffer(object);
}
void GlApi::glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
  if (target != GL_RENDERBUFFER) { error(GL_INVALID_ENUM); return; }
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  gl_.renderbuffer_storage(internalformat, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 0);
}
void GlApi::glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height) {
  if (target != GL_RENDERBUFFER) { error(GL_INVALID_ENUM); return; }
  if (width < 0 || height < 0 || samples < 0) { error(GL_INVALID_VALUE); return; }
  gl_.renderbuffer_storage(internalformat, static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(samples));
}
void GlApi::glGetRenderbufferParameteriv(GLenum target, GLenum pname, UserPtr params) {
  if (target != GL_RENDERBUFFER) { error(GL_INVALID_ENUM); return; }
  double v = 0;
  if (!gl_.get_renderbuffer_parameter(pname, &v)) { error(GL_INVALID_ENUM); return; }
  const int32_t i = static_cast<int32_t>(v);
  write_values(params, &i, 1);
}

// --- Framebuffers -----------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::glGenFramebuffers(GLsizei n, UserPtr framebuffers) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t name = fbo_names_.generate();
    if (!write_values(framebuffers.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
  }
}
void GlApi::glDeleteFramebuffers(GLsizei n, UserPtr framebuffers) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    uint32_t name = 0;
    if (!read_values(framebuffers.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
    if (!name) continue;
    const uint32_t object = fbo_names_.object(name);
    if (object) gl_.delete_object('f', object);
    fbo_names_.erase(name);
  }
}
uint64_t GlApi::glIsFramebuffer(GLuint framebuffer) { return framebuffer && fbo_names_.object(framebuffer) != 0; }
void GlApi::glBindFramebuffer(GLenum target, GLuint framebuffer) {
  if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) { error(GL_INVALID_ENUM); return; }
  uint32_t object = 0;
  if (framebuffer) {
    object = fbo_names_.object(framebuffer);
    if (!object) {
      object = gl_.create_framebuffer();
      fbo_names_.set(framebuffer, object);
    }
  }
  gl_.bind_framebuffer(target, object);
}
uint64_t GlApi::glCheckFramebufferStatus(GLenum target) {
  if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) { error(GL_INVALID_ENUM); return 0; }
  return gl_.check_framebuffer_status(target);
}
void GlApi::glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
  if (renderbuffertarget != GL_RENDERBUFFER) { error(GL_INVALID_ENUM); return; }
  gl_.framebuffer_renderbuffer(target, attachment, renderbuffer ? group_->renderbuffers.object(renderbuffer) : 0);
}
void GlApi::attach_texture(uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t texture, int32_t level, int32_t layer) {
  gl_.framebuffer_texture(target, attachment, textarget, texture ? group_->textures.object(texture) : 0, level, layer);
}
void GlApi::glFramebufferTexture1D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) { attach_texture(target, attachment, textarget, texture, level, 0); }
void GlApi::glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) { attach_texture(target, attachment, textarget, texture, level, 0); }
void GlApi::glFramebufferTexture3D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLint zoffset) { attach_texture(target, attachment, textarget, texture, level, zoffset); }
void GlApi::glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
  const webgl::Texture* t = texture ? gl_.texture_at(group_->textures.object(texture)) : nullptr;
  attach_texture(target, attachment, t ? t->target : GL_TEXTURE_2D_ARRAY, texture, level, layer);
}
void GlApi::glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level) {
  const webgl::Texture* t = texture ? gl_.texture_at(group_->textures.object(texture)) : nullptr;
  attach_texture(target, attachment, t ? (t->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : t->target) : GL_TEXTURE_2D, texture, level, 0);
}
void GlApi::glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, UserPtr params) {
  double v = 0;
  if (!gl_.get_framebuffer_attachment_parameter(target, attachment, pname, &v)) { error(GL_INVALID_ENUM); return; }
  const int32_t i = static_cast<int32_t>(v);
  write_values(params, &i, 1);
}
void GlApi::glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
  gl_.blit_framebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}
void GlApi::glInvalidateFramebuffer(GLenum, GLsizei numAttachments, UserPtr) { if (numAttachments < 0) error(GL_INVALID_VALUE); }
void GlApi::glInvalidateSubFramebuffer(GLenum, GLsizei numAttachments, UserPtr, GLint, GLint, GLsizei, GLsizei) { if (numAttachments < 0) error(GL_INVALID_VALUE); }

void GlApi::glDrawBuffer(GLenum buf) {
  if (gl_.draw_fb_ != 0) {
    const uint32_t list[1] = {buf};
    gl_.draw_buffers(ArrayView<const uint32_t>(list, 1));
    return;
  }
  switch (buf) {
    case GL_NONE: case GL_FRONT: case GL_BACK: case GL_FRONT_LEFT: case GL_BACK_LEFT: case GL_FRONT_AND_BACK: case GL_LEFT: break;
    case GL_RIGHT: case GL_FRONT_RIGHT: case GL_BACK_RIGHT: error(GL_INVALID_OPERATION); return;
    default: error(GL_INVALID_ENUM); return;
  }
  default_draw_buffer_ = buf;
  surface_generation_ = 0;
  surface_changed();
}
void GlApi::glDrawBuffers(GLsizei n, UserPtr bufs) {
  if (n < 0 || n > static_cast<int32_t>(webgl::kMaxDrawBuffers)) { error(GL_INVALID_VALUE); return; }
  uint32_t list[webgl::kMaxDrawBuffers];
  if (!read_values(bufs, list, static_cast<uint64_t>(n))) { error(GL_INVALID_VALUE); return; }
  if (gl_.draw_fb_ == 0) {
    if (n != 1) { error(GL_INVALID_OPERATION); return; }
    glDrawBuffer(list[0]);
    return;
  }
  gl_.draw_buffers(ArrayView<const uint32_t>(list, static_cast<uint64_t>(n)));
}
void GlApi::glReadBuffer(GLenum src) {
  if (gl_.read_fb_ != 0) { gl_.read_buffer(src); return; }
  switch (src) {
    case GL_NONE: case GL_FRONT: case GL_BACK: case GL_FRONT_LEFT: case GL_BACK_LEFT: case GL_LEFT: break;
    default: error(GL_INVALID_ENUM); return;
  }
  default_read_buffer_ = src;
  surface_generation_ = 0;
  surface_changed();
}

// --- Whole-framebuffer state -------------------------------------------------------------------------------------------------------------------------------------

void GlApi::glClear(GLbitfield mask) {
  if (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT)) { error(GL_INVALID_VALUE); return; }
  if (render_mode_ != GL_RENDER) return;
  if (mask & GL_ACCUM_BUFFER_BIT) clear_accum();
  const uint32_t rest = mask & ~GL_ACCUM_BUFFER_BIT;
  if (rest) {
    gl_.clear(rest);
    if ((rest & GL_COLOR_BUFFER_BIT) && gl_.desktop.draw_front_and_back && surface_) {
      // The other buffer takes the same clear.
      webgl::Image* saved_draw = gl_.dcolor_;
      gl_.dcolor_ = gl_.desktop.front_also;
      gl_.clear(GL_COLOR_BUFFER_BIT);
      gl_.dcolor_ = saved_draw;
    }
  }
}
void GlApi::glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { gl_.clear_color(r, g, b, a); }
void GlApi::glClearDepth(GLdouble depth) { gl_.clear_depth(static_cast<float>(depth < 0 ? 0 : (depth > 1 ? 1 : depth))); }
void GlApi::glClearDepthf(GLfloat d) { gl_.clear_depth(d < 0 ? 0 : (d > 1 ? 1 : d)); }
void GlApi::glClearStencil(GLint s) { gl_.clear_stencil(s); }
void GlApi::glClearIndex(GLfloat c) { fx_.clear_index = c; }
void GlApi::glIndexMask(GLuint mask) { fx_.index_mask = mask; }
void GlApi::glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
  gl_.color_mask(r != 0, g != 0, b != 0, a != 0);
  gl_.desktop.per_buffer_masks = false;
  const uint8_t m = static_cast<uint8_t>((r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0));
  for (uint8_t& e : gl_.desktop.color_mask) e = m;
}
void GlApi::glColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
  if (index >= webgl::kMaxDrawBuffers) { error(GL_INVALID_VALUE); return; }
  gl_.desktop.color_mask[index] = static_cast<uint8_t>((r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0));
  gl_.desktop.per_buffer_masks = true;
}
void GlApi::glDepthMask(GLboolean flag) { gl_.depth_mask(flag != 0); }
void GlApi::glDepthFunc(GLenum func) {
  if (func < GL_NEVER || func > GL_ALWAYS) { error(GL_INVALID_ENUM); return; }
  gl_.depth_func(func);
}
void GlApi::glDepthRange(GLdouble n, GLdouble f) {
  depth_range_values_[0] = static_cast<float>(n < 0 ? 0 : (n > 1 ? 1 : n));
  depth_range_values_[1] = static_cast<float>(f < 0 ? 0 : (f > 1 ? 1 : f));
  gl_.depth_range(depth_range_values_[0], depth_range_values_[1]);
  ++fx_generation_;
}
void GlApi::glDepthRangef(GLfloat n, GLfloat f) { glDepthRange(n, f); }
void GlApi::glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  gl_.vp_[0] = x; gl_.vp_[1] = y; gl_.vp_[2] = width; gl_.vp_[3] = height;
}
void GlApi::glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  gl_.sc_[0] = x; gl_.sc_[1] = y; gl_.sc_[2] = width; gl_.sc_[3] = height;
}
void GlApi::glBlendFunc(GLenum sfactor, GLenum dfactor) { gl_.blend_func_separate(sfactor, dfactor, sfactor, dfactor); for (auto& b : gl_.desktop.blend) { b.sr = b.sa = sfactor; b.dr = b.da = dfactor; } }
void GlApi::glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) { gl_.blend_func_separate(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha); for (auto& b : gl_.desktop.blend) { b.sr = sfactorRGB; b.dr = dfactorRGB; b.sa = sfactorAlpha; b.da = dfactorAlpha; } }
void GlApi::glBlendEquation(GLenum mode) { gl_.blend_equation_separate(mode, mode); for (auto& b : gl_.desktop.blend) b.eq_rgb = b.eq_a = mode; }
void GlApi::glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) { gl_.blend_equation_separate(modeRGB, modeAlpha); for (auto& b : gl_.desktop.blend) { b.eq_rgb = modeRGB; b.eq_a = modeAlpha; } }
void GlApi::glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { gl_.blend_color(r, g, b, a); }
void GlApi::glBlendFunci(GLuint buf, GLenum src, GLenum dst) {
  if (buf >= webgl::kMaxDrawBuffers) { error(GL_INVALID_VALUE); return; }
  webgl::DesktopState::BufferBlend& b = gl_.desktop.blend[buf];
  b.sr = b.sa = src; b.dr = b.da = dst;
  gl_.desktop.per_buffer_blend = true;
}
void GlApi::glBlendFuncSeparatei(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
  if (buf >= webgl::kMaxDrawBuffers) { error(GL_INVALID_VALUE); return; }
  webgl::DesktopState::BufferBlend& b = gl_.desktop.blend[buf];
  b.sr = srcRGB; b.dr = dstRGB; b.sa = srcAlpha; b.da = dstAlpha;
  gl_.desktop.per_buffer_blend = true;
}
void GlApi::glBlendEquationi(GLuint buf, GLenum mode) {
  if (buf >= webgl::kMaxDrawBuffers) { error(GL_INVALID_VALUE); return; }
  gl_.desktop.blend[buf].eq_rgb = gl_.desktop.blend[buf].eq_a = mode;
  gl_.desktop.per_buffer_blend = true;
}
void GlApi::glBlendEquationSeparatei(GLuint buf, GLenum modeRGB, GLenum modeAlpha) {
  if (buf >= webgl::kMaxDrawBuffers) { error(GL_INVALID_VALUE); return; }
  gl_.desktop.blend[buf].eq_rgb = modeRGB;
  gl_.desktop.blend[buf].eq_a = modeAlpha;
  gl_.desktop.per_buffer_blend = true;
}
void GlApi::glEnablei(GLenum target, GLuint index) {
  if (target == GL_BLEND) {
    if (index >= webgl::kMaxDrawBuffers) { error(GL_INVALID_VALUE); return; }
    gl_.desktop.blend[index].enabled = true;
    gl_.desktop.per_buffer_blend = true;
    return;
  }
  error(GL_INVALID_ENUM);
}
void GlApi::glDisablei(GLenum target, GLuint index) {
  if (target == GL_BLEND) {
    if (index >= webgl::kMaxDrawBuffers) { error(GL_INVALID_VALUE); return; }
    gl_.desktop.blend[index].enabled = false;
    gl_.desktop.per_buffer_blend = true;
    return;
  }
  error(GL_INVALID_ENUM);
}
uint64_t GlApi::glIsEnabledi(GLenum target, GLuint index) {
  if (target == GL_BLEND && index < webgl::kMaxDrawBuffers) return gl_.desktop.per_buffer_blend ? gl_.desktop.blend[index].enabled : gl_.is_enabled(GL_BLEND);
  error(GL_INVALID_ENUM);
  return 0;
}
void GlApi::glStencilFunc(GLenum func, GLint ref, GLuint mask) { gl_.stencil_func_separate(GL_FRONT_AND_BACK, func, ref, mask); }
void GlApi::glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) { gl_.stencil_func_separate(face, func, ref, mask); }
void GlApi::glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) { gl_.stencil_op_separate(GL_FRONT_AND_BACK, fail, zfail, zpass); }
void GlApi::glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) { gl_.stencil_op_separate(face, sfail, dpfail, dppass); }
void GlApi::glStencilMask(GLuint mask) { gl_.stencil_mask_separate(GL_FRONT_AND_BACK, mask); }
void GlApi::glStencilMaskSeparate(GLenum face, GLuint mask) { gl_.stencil_mask_separate(face, mask); }
void GlApi::glSampleCoverage(GLfloat value, GLboolean invert) { gl_.sample_coverage(value, invert != 0); }
void GlApi::glPrimitiveRestartIndex(GLuint index) { gl_.desktop.restart_index = index; }
void GlApi::glProvokingVertex(GLenum mode) {
  if (mode != GL_FIRST_VERTEX_CONVENTION && mode != GL_LAST_VERTEX_CONVENTION) { error(GL_INVALID_ENUM); return; }
  gl_.desktop.first_vertex_convention = mode == GL_FIRST_VERTEX_CONVENTION;
}
void GlApi::glClampColor(GLenum target, GLenum clamp) {
  if (target != GL_CLAMP_READ_COLOR && target != GL_CLAMP_VERTEX_COLOR && target != GL_CLAMP_FRAGMENT_COLOR) { error(GL_INVALID_ENUM); return; }
  if (target == GL_CLAMP_READ_COLOR) fx_.clamp_read_color = clamp != GL_FALSE;
  // (GL_FIXED_ONLY: clamped, as the default framebuffer is fixed-point)
  if (target == GL_CLAMP_VERTEX_COLOR) gl_.desktop.clamp_vertex_color = clamp != GL_FALSE;
}
void GlApi::glFlush() { flush_to_surface(); }
void GlApi::glFinish() { gl_.finish(); flush_to_surface(); }
void GlApi::glClearBufferfv(GLenum buffer, GLint drawbuffer, UserPtr value) {
  if (buffer == GL_DEPTH) {
    float d = 0;
    if (!read_values(value, &d, 1)) { error(GL_INVALID_VALUE); return; }
    const float saved = gl_.clear_depth_;
    gl_.clear_depth_ = d;
    gl_.clear(GL_DEPTH_BUFFER_BIT);
    gl_.clear_depth_ = saved;
    return;
  }
  uint32_t v[4];
  if (!read_values(value, v, 4)) { error(GL_INVALID_VALUE); return; }
  gl_.clear_buffer(buffer, drawbuffer, v, 0);
}
void GlApi::glClearBufferiv(GLenum buffer, GLint drawbuffer, UserPtr value) {
  if (buffer == GL_STENCIL) {
    int32_t s = 0;
    if (!read_values(value, &s, 1)) { error(GL_INVALID_VALUE); return; }
    const int32_t saved = gl_.clear_stencil_;
    gl_.clear_stencil_ = s;
    gl_.clear(GL_STENCIL_BUFFER_BIT);
    gl_.clear_stencil_ = saved;
    return;
  }
  uint32_t v[4];
  if (!read_values(value, v, 4)) { error(GL_INVALID_VALUE); return; }
  gl_.clear_buffer(buffer, drawbuffer, v, 1);
}
void GlApi::glClearBufferuiv(GLenum buffer, GLint drawbuffer, UserPtr value) {
  uint32_t v[4];
  if (!read_values(value, v, 4)) { error(GL_INVALID_VALUE); return; }
  gl_.clear_buffer(buffer, drawbuffer, v, 2);
}
void GlApi::glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
  if (buffer != GL_DEPTH_STENCIL) { error(GL_INVALID_ENUM); return; }
  gl_.clear_buffer_fi(drawbuffer, depth, stencil);
}

}  // namespace opengl

namespace opengl {

// --- Framebuffer parameters (GL 4.3/4.5, EXT_direct_state_access). Framebuffers without attachments are not drawn to: their default size reads back as set.

void GlApi::glCreateFramebuffers(GLsizei n, UserPtr framebuffers) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t name = fbo_names_.generate();
    fbo_names_.set(name, gl_.create_framebuffer());
    if (!write_values(framebuffers.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
  }
}

void GlApi::glCreateRenderbuffers(GLsizei n, UserPtr renderbuffers) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t name = group_->renderbuffers.generate();
    group_->renderbuffers.set(name, gl_.create_renderbuffer());
    if (!write_values(renderbuffers.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
  }
}

// `object` the engine's framebuffer (0 the default one). `draw_read`: GL_DRAW_BUFFER and GL_READ_BUFFER may be asked (only the EXT_direct_state_access query allows them).
bool GlApi::framebuffer_parameter(uint32_t object, uint32_t pname, bool draw_read, int32_t* out) {
  switch (pname) {
    case GL_DOUBLEBUFFER: *out = object == 0 && surface_ && surface_->double_buffered(); return true;
    case GL_STEREO: *out = 0; return true;
    case GL_SAMPLES: case GL_SAMPLE_BUFFERS: *out = 0; return true;
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT: *out = GL_RGBA; return true;
    case GL_IMPLEMENTATION_COLOR_READ_TYPE: *out = GL_UNSIGNED_BYTE; return true;
    case GL_FRAMEBUFFER_DEFAULT_WIDTH: case GL_FRAMEBUFFER_DEFAULT_HEIGHT: case GL_FRAMEBUFFER_DEFAULT_LAYERS: case GL_FRAMEBUFFER_DEFAULT_SAMPLES:
    case GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS:
      if (object == 0) { error(GL_INVALID_OPERATION); return false; }
      *out = fbo_defaults_[pname - GL_FRAMEBUFFER_DEFAULT_WIDTH];
      return true;
    case GL_DRAW_BUFFER: case GL_READ_BUFFER:
      if (!draw_read) break;
      if (object == 0) { *out = static_cast<int32_t>(pname == GL_DRAW_BUFFER ? default_draw_buffer_ : default_read_buffer_); return true; }
      {
        const webgl::Framebuffer* fb = gl_.fb_at(object);
        if (!fb) { error(GL_INVALID_OPERATION); return false; }
        *out = static_cast<int32_t>(pname == GL_DRAW_BUFFER ? fb->draw_buffers[0] : fb->read_buffer);
        return true;
      }
    default: break;
  }
  error(GL_INVALID_ENUM);
  return false;
}

void GlApi::glGetFramebufferParameteriv(GLenum target, GLenum pname, UserPtr params) {
  if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) { error(GL_INVALID_ENUM); return; }
  int32_t v = 0;
  if (framebuffer_parameter(gl_.bound_framebuffer(target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER), pname, false, &v)) write_values(params, &v, 1);
}
void GlApi::glGetNamedFramebufferParameteriv(GLuint framebuffer, GLenum pname, UserPtr param) {
  if (framebuffer && !fbo_names_.object(framebuffer)) { error(GL_INVALID_OPERATION); return; }
  int32_t v = 0;
  if (framebuffer_parameter(fbo_names_.object(framebuffer), pname, false, &v)) write_values(param, &v, 1);
}
void GlApi::glGetNamedFramebufferParameterivEXT(GLuint framebuffer, GLenum pname, UserPtr params) { glGetNamedFramebufferParameteriv(framebuffer, pname, params); }
void GlApi::glGetFramebufferParameterivEXT(GLuint framebuffer, GLenum pname, UserPtr params) {
  if (framebuffer && !fbo_names_.object(framebuffer)) { error(GL_INVALID_OPERATION); return; }
  int32_t v = 0;
  if (framebuffer_parameter(fbo_names_.object(framebuffer), pname, true, &v)) write_values(params, &v, 1);
}
void GlApi::glFramebufferParameteri(GLenum target, GLenum pname, GLint param) {
  if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) { error(GL_INVALID_ENUM); return; }
  if (pname < GL_FRAMEBUFFER_DEFAULT_WIDTH || pname > GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS) { error(GL_INVALID_ENUM); return; }
  if (gl_.bound_framebuffer(target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER) == 0) { error(GL_INVALID_OPERATION); return; }
  if (param < 0) { error(GL_INVALID_VALUE); return; }
  fbo_defaults_[pname - GL_FRAMEBUFFER_DEFAULT_WIDTH] = param;
}

}  // namespace opengl
