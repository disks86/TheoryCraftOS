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

// The context as a whole: construction, the stream decoder, client memory, display lists, enables.
#include "OpenGl/GlApi.hpp"
#include "OpenGl/FixedFunction.hpp"
#include "OpenGl/GlFeedback.hpp"
#include "OpenGl/GlSurface.hpp"
#include "OpenGl/generated/GlEnums.inc"
#include "OpenGl/generated/GlOpIds.inc"

namespace opengl {

namespace {

constexpr uint64_t kListAddressBase = 0xFFFF800000000000ull;      // addresses inside a display list's copies: no user address looks like this
constexpr uint8_t kOpFlags[] = {
#include "OpenGl/generated/GlOpFlags.inc"
    0};
constexpr const char* kOpNames[] = {
#include "OpenGl/generated/GlOpNames.inc"
    ""};

webgl::GlContext::Attributes engine_attributes(const ContextConfig& c) {
  webgl::GlContext::Attributes a;
  a.antialias = false;
  a.alpha = c.alpha_bits != 0;
  a.depth = c.depth_bits != 0;
  a.stencil = c.stencil_bits != 0;
  a.premultiplied = false;
  return a;
}

uint64_t decode_command(GlApi& api, uint32_t op, StreamReader& rd) {
  uint64_t result = 0;
  switch (op) {
#include "OpenGl/generated/GlDecode.inc"
    default: rd.fail(); break;
  }
  return result;
}

}  // namespace

GlApi::GlApi(allocators::IAllocator& allocator, ShareGroup& group, const ContextConfig& config)
    : a_(allocator), group_(&group), config_(config), gl_(allocator, true, 1, 1, engine_attributes(config), &group.objects), client_strings_(allocator), attrib_stack_(allocator), client_stack_(allocator),
      fixed_programs_(allocator), imm_(allocator), tri_edges_(allocator), vao_names_(allocator), fbo_names_(allocator), query_names_(allocator), tf_names_(allocator), mappings_(allocator), live_syncs_(allocator) {
  gl_.desktop.active = true;
  for (uint32_t& hint : fx_.hints) hint = GL_DONT_CARE;
  __builtin_memset(fx_.polygon_pattern, 0xFF, sizeof(fx_.polygon_pattern));
  gl_.desktop.point_max = 2047;
  gl_.desktop.edge_flag_attrib = -1;
  modelview_.depth = kModelviewDepth;
  projection_.depth = kProjectionDepth;
  color_stack_.depth = kColorDepth;
  for (MatrixStack& s : texture_stack_) s.depth = kTextureDepth;
  fx_.lights[0].diffuse[0] = fx_.lights[0].diffuse[1] = fx_.lights[0].diffuse[2] = 1;
  fx_.lights[0].specular[0] = fx_.lights[0].specular[1] = fx_.lights[0].specular[2] = 1;
  for (auto& unit : fx_.units) {
    unit.gen[0].object_plane[0] = 1; unit.gen[0].eye_plane[0] = 1;
    unit.gen[1].object_plane[1] = 1; unit.gen[1].eye_plane[1] = 1;
  }
  for (auto& t : fx_.current_tex) t[3] = 1;
  for (auto& t : fx_.raster_tex) t[3] = 1;
  create_default_textures();
  imm_buffer_ = gl_.create_buffer();
  index_buffer_ = gl_.create_buffer();
  gl_.viewport(0, 0, 0, 0);
  gl_.scissor(0, 0, 0, 0);
  gl_.enable(GL_DITHER, true);
  depth_range_values_[0] = 0;
  depth_range_values_[1] = 1;
}

void GlApi::release_internal_objects() {
  gl_.use_program(0);
  for (uint64_t i = 0; i < fixed_programs_.count(); ++i) gl_.delete_object('p', fixed_programs_[i]->program);
  fixed_programs_.clear();
  fixed_current_ = nullptr;
  gl_.delete_object('b', imm_buffer_);
  gl_.delete_object('b', index_buffer_);
  for (uint32_t& b : client_buffers_) { if (b) gl_.delete_object('b', b); b = 0; }
  for (uint32_t& t : default_textures_) { if (t) gl_.delete_object('t', t); t = 0; }
  if (pixel_program_) gl_.delete_object('p', pixel_program_);
  if (pixel_texture_) gl_.delete_object('t', pixel_texture_);
  if (pixel_depth_texture_) gl_.delete_object('t', pixel_depth_texture_);
  if (pixel_buffer_) gl_.delete_object('b', pixel_buffer_);
  pixel_program_ = pixel_texture_ = pixel_depth_texture_ = pixel_buffer_ = 0;
  imm_buffer_ = index_buffer_ = 0;
}

GlApi::~GlApi() { release_internal_objects(); }

void GlApi::join_group(ShareGroup& group) {
  if (&group == group_) return;
  if (list_building_) { list_building_.reset(); list_compiling_ = 0; }
  release_internal_objects();
  if (deleted_current_program_) gl_.delete_object('p', deleted_current_program_);
  deleted_current_program_ = 0;
  user_program_ = 0;
  // Nothing of the old group stays bound: buffers, textures, samplers, renderbuffers.
  gl_.array_buffer_ = gl_.copy_read_ = gl_.copy_write_ = gl_.pixel_pack_ = gl_.pixel_unpack_ = gl_.tf_buffer_ = gl_.uniform_buffer_ = 0;
  for (uint32_t& b : gl_.extra_buffers_) b = 0;
  for (auto& binding : gl_.ubo_bindings_) binding = webgl::BufferBinding();
  for (auto& binding : gl_.ssbo_bindings_) binding = webgl::BufferBinding();
  for (auto& binding : gl_.atomic_bindings_) binding = webgl::BufferBinding();
  for (webgl::TextureUnit& u : gl_.units_) u = webgl::TextureUnit();
  gl_.rb_bound_ = 0;
  const auto clear_vao = [](webgl::Vao& v) {
    v.element_buffer = 0;
    for (auto& at : v.attribs) if (!at.from_client) at.buffer = 0;
  };
  clear_vao(gl_.default_vao_);
  for (auto& v : gl_.vaos_) if (v) clear_vao(*v);
  group_ = &group;
  gl_.set_objects(&group.objects);
  create_default_textures();
  imm_buffer_ = gl_.create_buffer();
  index_buffer_ = gl_.create_buffer();
  ++fx_generation_;
}

void GlApi::bind_surface(GlSurface* surface) {
  surface_ = surface;
  surface_generation_ = 0;
  surface_changed();
  if (surface && !surface_seen_) {
    surface_seen_ = true;
    gl_.viewport(0, 0, static_cast<int32_t>(surface->width()), static_cast<int32_t>(surface->height()));
    gl_.scissor(0, 0, static_cast<int32_t>(surface->width()), static_cast<int32_t>(surface->height()));
    default_draw_buffer_ = surface->double_buffered() ? GL_BACK : GL_FRONT;
    default_read_buffer_ = default_draw_buffer_;
    surface_changed();
  }
}

void GlApi::surface_changed() {
  if (!surface_) {
    gl_.set_default_images(nullptr, nullptr, nullptr, 0, 0);
    return;
  }
  if (surface_generation_ == surface_->generation()) return;
  surface_generation_ = surface_->generation();
  const auto image = [&](uint32_t mode) -> webgl::Image* {
    switch (mode) {
      case GL_FRONT: case GL_FRONT_LEFT: case GL_LEFT: return &surface_->front();
      case GL_BACK: case GL_BACK_LEFT: case GL_RIGHT: return &surface_->back();
      case GL_FRONT_AND_BACK: return &surface_->back();
      default: return nullptr;
    }
  };
  gl_.set_default_images(image(default_draw_buffer_) ? image(default_draw_buffer_) : &surface_->back(), image(default_read_buffer_), &surface_->depth_stencil(), surface_->width(), surface_->height());
  gl_.default_draw_back_ = default_draw_buffer_ != GL_NONE;
  gl_.default_read_back_ = default_read_buffer_ != GL_NONE;
  gl_.desktop.draw_front_and_back = default_draw_buffer_ == GL_FRONT_AND_BACK && surface_->double_buffered();
  gl_.desktop.front_also = gl_.desktop.draw_front_and_back ? &surface_->front() : nullptr;
}

bool GlApi::read_client(UserPtr at, ByteView out) {
  if (out.empty()) return true;
  if (at.address >= kListAddressBase) {
    const uint64_t offset = at.address - kListAddressBase;
    ConstByteView from;
    if (!list_client_.try_subview(offset, out.size(), &from)) return false;
    out.copy_from(from);
    return true;
  }
  return memory_ && at.address != 0 && memory_->read(at.address, out);
}

bool GlApi::write_client(UserPtr at, ConstByteView data) {
  if (data.empty()) return true;
  return memory_ && at.address != 0 && at.address < kListAddressBase && memory_->write(at.address, data);
}

bool GlApi::read_string(UserPtr at, int64_t length, String* out) {
  out->clear();
  char buf[256];
  uint64_t done = 0;
  const uint64_t limit = 16ull * 1024 * 1024;
  while (done < limit) {
    uint64_t want = sizeof(buf);
    const uint64_t to_page = 4096 - ((at.address + done) & 4095);      // (a block never runs into the next page, which may not be there)
    if (length < 0 && to_page < want) want = to_page;
    if (length >= 0 && static_cast<uint64_t>(length) - done < want) want = static_cast<uint64_t>(length) - done;
    if (want == 0) return true;
    // Near the end of a mapping a block read fails while the string itself is fine: go down to bytes then.
    ByteView view(reinterpret_cast<uint8_t*>(buf), want);
    if (!read_client(at.plus(done), view)) {
      if (want == 1) return false;
      uint64_t n = 0;
      while (n < want && read_client(at.plus(done + n), ByteView(reinterpret_cast<uint8_t*>(buf) + n, 1))) {
        if (length < 0 && buf[n] == 0) { out->append(StringView(buf, n)); return true; }
        ++n;
      }
      out->append(StringView(buf, n));
      return n == want;
    }
    if (length < 0) {
      for (uint64_t i = 0; i < want; ++i) {
        if (buf[i] == 0) { out->append(StringView(buf, i)); return true; }
      }
    }
    out->append(StringView(buf, want));
    done += want;
  }
  return false;
}

bool GlApi::write_string(UserPtr at, int32_t capacity, UserPtr length_out, StringView text) {
  uint64_t n = text.size();
  if (capacity <= 0) n = 0;
  else if (n > static_cast<uint64_t>(capacity) - 1) n = static_cast<uint64_t>(capacity) - 1;
  if (capacity > 0 && !at.null()) {
    if (!write_client(at, ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), n))) return false;
    const uint8_t zero = 0;
    if (!write_client(at.plus(n), ConstByteView(&zero, 1))) return false;
  }
  if (!length_out.null()) {
    const int32_t written = static_cast<int32_t>(n);
    return write_values(length_out, &written, 1);
  }
  return true;
}

uint64_t GlApi::client_string(const char* text) {
  for (uint64_t i = 0; i < client_strings_.count(); ++i)
    if (client_strings_[i].text == text) return client_strings_[i].address;
  const StringView view(text);
  if (!memory_) return 0;
  const uint64_t address = memory_->allocate(view.size() + 1);
  if (!address) return 0;
  const uint8_t zero = 0;
  memory_->write(address, ConstByteView(reinterpret_cast<const uint8_t*>(view.data()), view.size()));
  memory_->write(address + view.size(), ConstByteView(&zero, 1));
  client_strings_.push_back(ClientString{text, address});
  return address;
}

uint64_t GlApi::execute(ConstByteView stream, IClientMemory& memory) {
  memory_ = &memory;
  surface_changed();
  return run_stream(stream);
}

uint64_t GlApi::run_stream(ConstByteView stream) {
  uint64_t result = 0;
  uint64_t at = 0;
  while (at + 4 <= stream.size()) {
    const ConstByteView head_bytes = stream.subview(at, 4);
    const uint32_t head = uint32_t{head_bytes[0]} | uint32_t{head_bytes[1]} << 8 | uint32_t{head_bytes[2]} << 16 | uint32_t{head_bytes[3]} << 24;
    const uint32_t op = head & 0xFFFF;
    const uint64_t bytes = uint64_t{head >> 16} * 4;
    if (bytes < 4 || bytes > stream.size() - at || op == 0 || op > kOpCount) break;
    const ConstByteView command = stream.subview(at, bytes);
    at += bytes;
    const uint8_t flags = kOpFlags[op - 1];
    if (config_.core_profile && (flags & 4)) { error(GL_INVALID_OPERATION); continue; }
    if (list_building_ && (flags & 1)) {
      list_record(op, command);
      if (!list_execute_too_) continue;
    }
    if (in_begin_end_ && !(flags & 2)) { error(GL_INVALID_OPERATION); continue; }
    StreamReader rd(command.subview(4, bytes - 4));
    result = decode_command(*this, op, rd);
  }
  return result;
}

const char* gl_op_name(uint32_t op) { return op >= 1 && op <= kOpCount ? kOpNames[op - 1] : "?"; }

// --- Display lists ----------------------------------------------------------------------------------------------------------------------------------------

namespace {
uint32_t word_at(ConstByteView command, uint32_t word) {
  const uint64_t at = uint64_t{word} * 4;
  if (at + 4 > command.size()) return 0;
  return uint32_t{command[at]} | uint32_t{command[at + 1]} << 8 | uint32_t{command[at + 2]} << 16 | uint32_t{command[at + 3]} << 24;
}
uint32_t texel_bytes_of(uint32_t format, uint32_t type) {
  uint32_t n = 4;
  switch (format) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE: case GL_INTENSITY: case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX: case GL_COLOR_INDEX: case GL_RED_INTEGER: n = 1; break;
    case GL_RG: case GL_LUMINANCE_ALPHA: case GL_RG_INTEGER: n = 2; break;
    case GL_RGB: case GL_BGR: case GL_RGB_INTEGER: case GL_BGR_INTEGER: n = 3; break;
    default: n = 4; break;
  }
  switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return n;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: return n * 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return n * 4;
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: return 1;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV: case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV: return 2;
    default: return 4;
  }
}
}  // namespace

// How many bytes of client data a command that display lists record reads, and which argument word holds the address. Pixel data is read at the list's own unpack state at compile
// time, so a rectangle is stored with its row padding: it is replayed with the state saved in front of it (kept simple: tightly packed when compiled with default unpack state).
uint64_t GlApi::client_bytes_of(uint32_t op, ConstByteView c, uint32_t* pointer_word) {
  const auto arg = [&](uint32_t i) { return word_at(c, i + 1); };
  const uint64_t row_align = static_cast<uint64_t>(unpack_.alignment ? unpack_.alignment : 1);
  const auto image = [&](uint32_t w, uint32_t h, uint32_t d, uint32_t format, uint32_t type) {
    const uint64_t row_length = unpack_.row_length ? static_cast<uint64_t>(unpack_.row_length) : w;
    const uint64_t rows = unpack_.image_height ? static_cast<uint64_t>(unpack_.image_height) : h;
    const uint64_t row = (row_length * texel_bytes_of(format, type) + row_align - 1) / row_align * row_align;
    return row * rows * (d + static_cast<uint64_t>(unpack_.skip_images)) + row * static_cast<uint64_t>(unpack_.skip_rows) + static_cast<uint64_t>(unpack_.skip_pixels) * texel_bytes_of(format, type);
  };
  switch (op) {
#define CASE_VALUES(name, ptr_word, count_expr, size) case kOp_##name: *pointer_word = (ptr_word) + 1; return uint64_t(count_expr) * (size);
    CASE_VALUES(glLightfv, 2, light_value_count(arg(1)), 4)
    CASE_VALUES(glLightiv, 2, light_value_count(arg(1)), 4)
    CASE_VALUES(glLightModelfv, 1, model_value_count(arg(0)), 4)
    CASE_VALUES(glLightModeliv, 1, model_value_count(arg(0)), 4)
    CASE_VALUES(glMaterialfv, 2, material_value_count(arg(1)), 4)
    CASE_VALUES(glMaterialiv, 2, material_value_count(arg(1)), 4)
    CASE_VALUES(glFogfv, 1, arg(0) == GL_FOG_COLOR ? 4 : 1, 4)
    CASE_VALUES(glFogiv, 1, arg(0) == GL_FOG_COLOR ? 4 : 1, 4)
    CASE_VALUES(glTexEnvfv, 2, arg(1) == GL_TEXTURE_ENV_COLOR ? 4 : 1, 4)
    CASE_VALUES(glTexEnviv, 2, arg(1) == GL_TEXTURE_ENV_COLOR ? 4 : 1, 4)
    CASE_VALUES(glTexGenfv, 2, arg(1) == GL_TEXTURE_GEN_MODE ? 1 : 4, 4)
    CASE_VALUES(glTexGeniv, 2, arg(1) == GL_TEXTURE_GEN_MODE ? 1 : 4, 4)
    CASE_VALUES(glTexGendv, 2, arg(1) == GL_TEXTURE_GEN_MODE ? 1 : 4, 8)
    CASE_VALUES(glTexParameterfv, 2, arg(1) == GL_TEXTURE_BORDER_COLOR ? 4 : 1, 4)
    CASE_VALUES(glTexParameteriv, 2, arg(1) == GL_TEXTURE_BORDER_COLOR ? 4 : 1, 4)
    CASE_VALUES(glPointParameterfv, 1, arg(0) == GL_POINT_DISTANCE_ATTENUATION ? 3 : 1, 4)
    CASE_VALUES(glPointParameteriv, 1, arg(0) == GL_POINT_DISTANCE_ATTENUATION ? 3 : 1, 4)
    CASE_VALUES(glPolygonStipple, 0, 128, 1)
    CASE_VALUES(glPixelMapfv, 2, arg(1), 4)
    CASE_VALUES(glPixelMapuiv, 2, arg(1), 4)
    CASE_VALUES(glPixelMapusv, 2, arg(1), 2)
    CASE_VALUES(glCallLists, 2, arg(0), arg(1) == GL_BYTE || arg(1) == GL_UNSIGNED_BYTE ? 1 : (arg(1) == GL_SHORT || arg(1) == GL_UNSIGNED_SHORT || arg(1) == GL_2_BYTES ? 2 : (arg(1) == GL_3_BYTES ? 3 : 4)))
    case kOp_glBitmap: {      // (as pixels_in_bitmap reads it: rows padded to the alignment, skipped rows and bits in front)
      *pointer_word = 7;
      const uint64_t w = arg(0), h = arg(1);
      if (!w || !h) return 0;
      const uint64_t row_length = unpack_.row_length > 0 ? static_cast<uint64_t>(unpack_.row_length) : w;
      const uint64_t row = ((row_length + 7) / 8 + row_align - 1) / row_align * row_align;
      return static_cast<uint64_t>(unpack_.skip_rows) * row + (h - 1) * row + (static_cast<uint64_t>(unpack_.skip_pixels) + w + 7) / 8;
    }
    case kOp_glDrawPixels: *pointer_word = 5; return image(arg(0), arg(1), 1, arg(2), arg(3));
    case kOp_glTexImage1D: *pointer_word = 8; return image(arg(3), 1, 1, arg(5), arg(6));
    case kOp_glTexImage2D: *pointer_word = 9; return image(arg(3), arg(4), 1, arg(6), arg(7));
    case kOp_glTexImage3D: *pointer_word = 10; return image(arg(3), arg(4), arg(5), arg(7), arg(8));
    case kOp_glTexSubImage1D: *pointer_word = 7; return image(arg(3), 1, 1, arg(4), arg(5));
    case kOp_glTexSubImage2D: *pointer_word = 9; return image(arg(4), arg(5), 1, arg(6), arg(7));
    case kOp_glTexSubImage3D: *pointer_word = 11; return image(arg(5), arg(6), arg(7), arg(8), arg(9));
    default: *pointer_word = 0; return 0;
#undef CASE_VALUES
  }
}

bool GlApi::list_record(uint32_t op, ConstByteView command) {
  DisplayList& list = *list_building_;
  uint32_t pointer_word = 0;
  const uint64_t bytes = client_bytes_of(op, command, &pointer_word);
  if (!pointer_word) {
    // A draw from client arrays reads them now: it is stored as the vertices it gives (GlApiDraw.cpp).
    if (op == kOp_glDrawArrays || op == kOp_glDrawElements || op == kOp_glDrawRangeElements || op == kOp_glArrayElement || op == kOp_glMultiDrawArrays || op == kOp_glMultiDrawElements) return list_record_draw(op, command);
    return list.commands.append(command);
  }
  const uint64_t address = uint64_t{word_at(command, pointer_word)} | uint64_t{word_at(command, pointer_word + 1)} << 32;
  const uint64_t offset = list.client.size();
  if (address != 0 && bytes != 0) {
    if (bytes > (256ull << 20) || !list.client.put_zeros(bytes)) { error(GL_OUT_OF_MEMORY); return false; }
    if (!read_client(UserPtr{address}, list.client.writable_view().subview(offset, bytes))) { error(GL_INVALID_VALUE); return false; }
  }
  uint64_t start = list.commands.size();
  if (!list.commands.append(command)) { error(GL_OUT_OF_MEMORY); return false; }
  if (address != 0) {
    const uint64_t replay = kListAddressBase + offset;
    ByteView w = list.commands.writable_view().subview(start + uint64_t{pointer_word} * 4, 8);
    for (int i = 0; i < 8; ++i) w[static_cast<uint64_t>(i)] = static_cast<uint8_t>(replay >> (8 * i));
  }
  return true;
}

void GlApi::glNewList(GLuint list, GLenum mode) {
  if (list == 0) { error(GL_INVALID_VALUE); return; }
  if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) { error(GL_INVALID_ENUM); return; }
  if (list_building_) { error(GL_INVALID_OPERATION); return; }
  list_building_ = SharedPointer<DisplayList>::make(a_, a_);
  if (!list_building_) { error(GL_OUT_OF_MEMORY); return; }
  list_compiling_ = list;
  list_execute_too_ = mode == GL_COMPILE_AND_EXECUTE;
}

void GlApi::glEndList() {
  if (!list_building_) { error(GL_INVALID_OPERATION); return; }
  group_->lists.put(list_compiling_, list_building_);
  list_building_.reset();
  list_compiling_ = 0;
}

void GlApi::list_run(uint32_t name) {
  const SharedPointer<DisplayList>* found = group_->lists.find(name);
  if (!found || !*found) return;
  if (list_depth_ >= 64) return;      // GL_MAX_LIST_NESTING
  const SharedPointer<DisplayList> list = *found;      // (it may delete itself)
  const ConstByteView saved_client = list_client_;
  list_client_ = list->client.view();
  ++list_depth_;
  run_stream(list->commands.view());
  --list_depth_;
  list_client_ = saved_client;
}

void GlApi::glCallList(GLuint list) { list_run(list); }

void GlApi::glCallLists(GLsizei n, GLenum type, UserPtr lists) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  uint32_t size = 0;
  switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: size = 1; break;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_2_BYTES: size = 2; break;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: case GL_4_BYTES: size = 4; break;
    case GL_3_BYTES: size = 3; break;
    default: error(GL_INVALID_ENUM); return;
  }
  for (int32_t i = 0; i < n; ++i) {
    uint8_t v[4] = {0, 0, 0, 0};
    if (!read_client(lists.plus(uint64_t{static_cast<uint32_t>(i)} * size), ByteView(v, size))) { error(GL_INVALID_VALUE); return; }
    uint32_t name = 0;
    switch (type) {
      case GL_BYTE: name = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v[0]))); break;
      case GL_UNSIGNED_BYTE: name = v[0]; break;
      case GL_SHORT: name = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v[0] | v[1] << 8))); break;
      case GL_UNSIGNED_SHORT: name = uint32_t{v[0]} | uint32_t{v[1]} << 8; break;
      case GL_2_BYTES: name = uint32_t{v[0]} << 8 | v[1]; break;
      case GL_3_BYTES: name = uint32_t{v[0]} << 16 | uint32_t{v[1]} << 8 | v[2]; break;
      case GL_4_BYTES: name = uint32_t{v[0]} << 24 | uint32_t{v[1]} << 16 | uint32_t{v[2]} << 8 | v[3]; break;
      case GL_FLOAT: { uint32_t raw = uint32_t{v[0]} | uint32_t{v[1]} << 8 | uint32_t{v[2]} << 16 | uint32_t{v[3]} << 24; name = static_cast<uint32_t>(__builtin_bit_cast(float, raw)); break; }
      default: name = uint32_t{v[0]} | uint32_t{v[1]} << 8 | uint32_t{v[2]} << 16 | uint32_t{v[3]} << 24; break;
    }
    list_run(name + list_base_);
  }
}

void GlApi::glListBase(GLuint base) { list_base_ = base; }

uint64_t GlApi::glGenLists(GLsizei range) {
  if (range < 0) { error(GL_INVALID_VALUE); return 0; }
  if (range == 0) return 0;
  // the lowest run of `range` free names
  uint32_t start = 1;
  for (;;) {
    uint32_t i = 0;
    while (i < static_cast<uint32_t>(range) && !group_->lists.contains(start + i)) ++i;
    if (i == static_cast<uint32_t>(range)) break;
    start += i + 1;
    if (start > 0x7FFFFFFFu) { error(GL_OUT_OF_MEMORY); return 0; }
  }
  for (int32_t i = 0; i < range; ++i) group_->lists.put(start + static_cast<uint32_t>(i), SharedPointer<DisplayList>());
  return start;
}

void GlApi::glDeleteLists(GLuint list, GLsizei range) {
  if (range < 0) { error(GL_INVALID_VALUE); return; }
  for (int64_t i = 0; i < range; ++i) {
    const uint32_t name = list + static_cast<uint32_t>(i);
    group_->lists.remove(name);
  }
}

uint64_t GlApi::glIsList(GLuint list) {
  return group_->lists.contains(list) ? 1 : 0;
}

}  // namespace opengl
