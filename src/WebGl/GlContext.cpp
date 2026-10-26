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

// The GL object tables and state machine (see GlContext.hpp): buffers, textures, framebuffers, programs, state.
#include "WebGl/GlContext.hpp"
#include "WebGl/GlCompressed.hpp"
#include "WebGl/GlPixels.hpp"

namespace webgl {

namespace {
constexpr uint32_t GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_UNIFORM_BUFFER = 0x8A11, GL_COPY_READ = 0x8F36, GL_COPY_WRITE = 0x8F37,
                   GL_PIXEL_PACK = 0x88EB, GL_PIXEL_UNPACK = 0x88EC, GL_TF_BUFFER = 0x8C8E;
constexpr uint32_t INVALID_ENUM = 0x500, INVALID_VALUE = 0x501, INVALID_OPERATION = 0x502, OUT_OF_MEMORY = 0x505;

bool is_cube_face(uint32_t t) { return t >= 0x8515 && t <= 0x851A; }
}  // namespace

GlContext::GlContext(allocators::IAllocator& a, bool es3, uint32_t width, uint32_t height, const Attributes& attrs)
    : a_(a), es3_(es3), attrs_(attrs), width_(width), height_(height), buffers_(a), textures_(a), fbos_(a), rbos_(a), shaders_(a), programs_(a), vaos_(a), samplers_(a),
      queries_(a), tfs_(a), def_color_(a), def_depth_(a) {
  resize(width, height);
  vp_[0] = vp_[1] = 0;
  vp_[2] = static_cast<int32_t>(width_);
  vp_[3] = static_cast<int32_t>(height_);
  sc_[0] = sc_[1] = 0;
  sc_[2] = static_cast<int32_t>(width_);
  sc_[3] = static_cast<int32_t>(height_);
  valid_ = def_color_.defined();
}

GlContext::~GlContext() { hw_drop(); }

void GlContext::restore() { lost_ = false; }

void GlContext::resize(uint32_t w, uint32_t h) {
  if (w == 0) w = 1;
  if (h == 0) h = 1;
  if (w > 8192) w = 8192;
  if (h > 8192) h = 8192;
  hw_drop();   // a resized drawing buffer starts over
  width_ = w;
  height_ = h;
  const bool had_vp = vp_[2] != 0;
  // an antialiased drawing buffer keeps 2 x 2 samples per pixel (not for very large ones)
  const uint32_t ss = attrs_.antialias && uint64_t{w} * h <= 2048ull * 2048 ? 2 : 1;
  def_color_.allocate(find_format(0x8058), w * ss, h * ss, 1);
  def_color_.ss = ss;
  if (attrs_.depth || attrs_.stencil) { def_depth_.allocate(find_format(0x88F0), w * ss, h * ss, 1); def_depth_.ss = ss; }
  // a freshly sized drawing buffer is transparent black (or opaque black without alpha) and has a clear depth of 1
  if (def_color_.defined() && !attrs_.alpha) {
    uint8_t* p = def_color_.px.writable_view().data();
    for (uint64_t i = 3; i < def_color_.px.size(); i += 4) p[i] = 255;
  }
  if (def_depth_.defined()) {
    uint32_t v[4] = {__builtin_bit_cast(uint32_t, 1.0f), 0, 0, 0};
    for (uint32_t y = 0; y < def_depth_.h; ++y)
      for (uint32_t x = 0; x < def_depth_.w; ++x) write_texel(def_depth_, x, y, 0, v);
  }
  if (had_vp) {   // the viewport and scissor of a resized buffer follow it, as the specification says only for the initial values
    vp_[2] = static_cast<int32_t>(w);
    vp_[3] = static_cast<int32_t>(h);
    sc_[2] = static_cast<int32_t>(w);
    sc_[3] = static_cast<int32_t>(h);
  }
  all_dirty_ = true;
  ++version_;
}

uint32_t GlContext::get_error() {
  if (lost_) return 0;
  const uint32_t e = error_;
  error_ = 0;
  return e;
}

// --- Object tables ---------------------------------------------------------------------------------------------------------------------------------------

uint32_t GlContext::create_buffer() {
  UniquePointer<Buffer> b = UniquePointer<Buffer>::make(a_, a_);
  if (!b || !buffers_.push_back(static_cast<UniquePointer<Buffer>&&>(b))) { set_error(OUT_OF_MEMORY); return 0; }
  return static_cast<uint32_t>(buffers_.count());
}
uint32_t GlContext::create_texture() {
  UniquePointer<Texture> b = UniquePointer<Texture>::make(a_, a_);
  if (!b || !textures_.push_back(static_cast<UniquePointer<Texture>&&>(b))) { set_error(OUT_OF_MEMORY); return 0; }
  return static_cast<uint32_t>(textures_.count());
}
uint32_t GlContext::create_framebuffer() {
  UniquePointer<Framebuffer> b = UniquePointer<Framebuffer>::make(a_);
  if (!b || !fbos_.push_back(static_cast<UniquePointer<Framebuffer>&&>(b))) { set_error(OUT_OF_MEMORY); return 0; }
  return static_cast<uint32_t>(fbos_.count());
}
uint32_t GlContext::create_renderbuffer() {
  UniquePointer<Renderbuffer> b = UniquePointer<Renderbuffer>::make(a_, a_);
  if (!b || !rbos_.push_back(static_cast<UniquePointer<Renderbuffer>&&>(b))) { set_error(OUT_OF_MEMORY); return 0; }
  return static_cast<uint32_t>(rbos_.count());
}
uint32_t GlContext::create_shader(uint32_t type) {
  if (type != 0x8B31 && type != 0x8B30) { set_error(INVALID_ENUM); return 0; }
  UniquePointer<ShaderObj> b = UniquePointer<ShaderObj>::make(a_, a_);
  if (!b) { set_error(OUT_OF_MEMORY); return 0; }
  b->type = type;
  shaders_.push_back(static_cast<UniquePointer<ShaderObj>&&>(b));
  return static_cast<uint32_t>(shaders_.count());
}
uint32_t GlContext::create_program() {
  UniquePointer<ProgramObj> b = UniquePointer<ProgramObj>::make(a_, a_);
  if (!b || !programs_.push_back(static_cast<UniquePointer<ProgramObj>&&>(b))) { set_error(OUT_OF_MEMORY); return 0; }
  return static_cast<uint32_t>(programs_.count());
}
uint32_t GlContext::create_vao() {
  UniquePointer<Vao> b = UniquePointer<Vao>::make(a_);
  if (!b || !vaos_.push_back(static_cast<UniquePointer<Vao>&&>(b))) { set_error(OUT_OF_MEMORY); return 0; }
  return static_cast<uint32_t>(vaos_.count());
}
uint32_t GlContext::create_sampler() {
  UniquePointer<SamplerObj> b = UniquePointer<SamplerObj>::make(a_);
  if (!b || !samplers_.push_back(static_cast<UniquePointer<SamplerObj>&&>(b))) { set_error(OUT_OF_MEMORY); return 0; }
  return static_cast<uint32_t>(samplers_.count());
}
uint32_t GlContext::create_query() {
  UniquePointer<QueryObj> b = UniquePointer<QueryObj>::make(a_);
  if (!b || !queries_.push_back(static_cast<UniquePointer<QueryObj>&&>(b))) { set_error(OUT_OF_MEMORY); return 0; }
  return static_cast<uint32_t>(queries_.count());
}
uint32_t GlContext::create_transform_feedback() {
  UniquePointer<TransformFeedbackObj> b = UniquePointer<TransformFeedbackObj>::make(a_);
  if (!b || !tfs_.push_back(static_cast<UniquePointer<TransformFeedbackObj>&&>(b))) { set_error(OUT_OF_MEMORY); return 0; }
  return static_cast<uint32_t>(tfs_.count());
}

void GlContext::delete_object(uint32_t kind, uint32_t id) {
  if (!id) return;
  switch (kind) {
    case 'b':
      if (id <= buffers_.count()) {
        if (buffer_at(id)) { total_bytes_ -= buffer_at(id)->data.size() < total_bytes_ ? buffer_at(id)->data.size() : total_bytes_; }
        buffers_[id - 1].reset();
        if (array_buffer_ == id) array_buffer_ = 0;
        if (copy_read_ == id) copy_read_ = 0;
        if (copy_write_ == id) copy_write_ = 0;
        if (pixel_pack_ == id) pixel_pack_ = 0;
        if (pixel_unpack_ == id) pixel_unpack_ = 0;
        if (uniform_buffer_ == id) uniform_buffer_ = 0;
        if (default_vao_.element_buffer == id) default_vao_.element_buffer = 0;
        for (auto& v : vaos_) {
          if (!v) continue;
          if (v->element_buffer == id) v->element_buffer = 0;
          for (auto& at : v->attribs) if (at.buffer == id) at.buffer = 0;
        }
        for (auto& at : default_vao_.attribs) if (at.buffer == id) at.buffer = 0;
        for (auto& bb : ubo_bindings_) if (bb.buffer == id) bb = BufferBinding();
      }
      break;
    case 't':
      if (id <= textures_.count()) {
        textures_[id - 1].reset();
        for (auto& u : units_) {
          if (u.tex2d == id) u.tex2d = 0;
          if (u.cube == id) u.cube = 0;
          if (u.tex3d == id) u.tex3d = 0;
          if (u.array == id) u.array = 0;
        }
        for (auto& f : fbos_) {
          if (!f) continue;
          for (auto& at : f->color) if (at.kind == 1 && at.id == id) at = Attachment();
          if (f->depth.kind == 1 && f->depth.id == id) f->depth = Attachment();
          if (f->stencil.kind == 1 && f->stencil.id == id) f->stencil = Attachment();
        }
      }
      break;
    case 'f':
      if (id <= fbos_.count()) {
        fbos_[id - 1].reset();
        if (draw_fb_ == id) draw_fb_ = 0;
        if (read_fb_ == id) read_fb_ = 0;
      }
      break;
    case 'r':
      if (id <= rbos_.count()) {
        rbos_[id - 1].reset();
        if (rb_bound_ == id) rb_bound_ = 0;
        for (auto& f : fbos_) {
          if (!f) continue;
          for (auto& at : f->color) if (at.kind == 2 && at.id == id) at = Attachment();
          if (f->depth.kind == 2 && f->depth.id == id) f->depth = Attachment();
          if (f->stencil.kind == 2 && f->stencil.id == id) f->stencil = Attachment();
        }
      }
      break;
    case 's':
      if (id <= shaders_.count() && shaders_[id - 1]) {
        shaders_[id - 1]->deleted = true;
        bool attached = false;
        for (auto& p : programs_) if (p && (p->vs == id || p->fs == id)) attached = true;
        if (!attached) shaders_[id - 1].reset();
      }
      break;
    case 'p':
      if (id <= programs_.count() && programs_[id - 1]) {
        if (program_ == id) programs_[id - 1]->deleted = true;
        else programs_[id - 1].reset();
      }
      break;
    case 'v':
      if (id <= vaos_.count()) {
        vaos_[id - 1].reset();
        if (vao_id_ == id) vao_id_ = 0;
      }
      break;
    case 'a': if (id <= samplers_.count()) samplers_[id - 1].reset(); for (auto& u : units_) if (u.sampler == id) u.sampler = 0; break;
    case 'q': if (id <= queries_.count()) queries_[id - 1].reset(); break;
    case 'x': if (id <= tfs_.count()) tfs_[id - 1].reset(); if (tf_bound_ == id) tf_bound_ = 0; break;
    default: break;
  }
}

bool GlContext::is_object(uint32_t kind, uint32_t id) const {
  if (!id) return false;
  switch (kind) {
    case 'b': return id <= buffers_.count() && buffers_[id - 1] && buffers_[id - 1]->ever_bound;
    case 't': return id <= textures_.count() && textures_[id - 1] && textures_[id - 1]->target != 0;
    case 'f': return id <= fbos_.count() && fbos_[id - 1];
    case 'r': return id <= rbos_.count() && rbos_[id - 1];
    case 's': return id <= shaders_.count() && shaders_[id - 1];
    case 'p': return id <= programs_.count() && programs_[id - 1];
    case 'v': return id <= vaos_.count() && vaos_[id - 1];
    case 'a': return id <= samplers_.count() && samplers_[id - 1];
    case 'q': return id <= queries_.count() && queries_[id - 1];
    case 'x': return id <= tfs_.count() && tfs_[id - 1];
    default: return false;
  }
}

// --- Buffers -----------------------------------------------------------------------------------------------------------------------------------------------

void GlContext::bind_buffer(uint32_t target, uint32_t id) {
  if (id && !buffer_at(id)) { set_error(INVALID_OPERATION); return; }
  if (id) buffer_at(id)->ever_bound = true;
  switch (target) {
    case GL_ARRAY_BUFFER: array_buffer_ = id; break;
    case GL_ELEMENT_ARRAY_BUFFER: current_vao_ptr()->element_buffer = id; break;
    case GL_UNIFORM_BUFFER: uniform_buffer_ = id; break;
    case GL_COPY_READ: copy_read_ = id; break;
    case GL_COPY_WRITE: copy_write_ = id; break;
    case GL_PIXEL_PACK: pixel_pack_ = id; break;
    case GL_PIXEL_UNPACK: pixel_unpack_ = id; break;
    case GL_TF_BUFFER: tf_buffer_ = id; break;
    default: set_error(INVALID_ENUM); break;
  }
}

uint32_t GlContext::bound_buffer(uint32_t target) const {
  switch (target) {
    case GL_ARRAY_BUFFER: return array_buffer_;
    case GL_ELEMENT_ARRAY_BUFFER: return (vao_id_ == 0 ? &default_vao_ : vaos_[vao_id_ - 1].get())->element_buffer;
    case GL_UNIFORM_BUFFER: return uniform_buffer_;
    case GL_COPY_READ: return copy_read_;
    case GL_COPY_WRITE: return copy_write_;
    case GL_PIXEL_PACK: return pixel_pack_;
    case GL_PIXEL_UNPACK: return pixel_unpack_;
    case GL_TF_BUFFER: return tf_buffer_;
    default: return 0;
  }
}

void GlContext::bind_buffer_range(uint32_t target, uint32_t index, uint32_t id, uint64_t offset, uint64_t size) {
  if (target == GL_UNIFORM_BUFFER) {
    if (index >= kMaxUniformBindings) { set_error(INVALID_VALUE); return; }
    if (id && !buffer_at(id)) { set_error(INVALID_OPERATION); return; }
    ubo_bindings_[index] = BufferBinding{id, offset, size};
    uniform_buffer_ = id;
    if (id) buffer_at(id)->ever_bound = true;
  } else if (target == GL_TF_BUFFER) {
    if (index >= 4) { set_error(INVALID_VALUE); return; }
    tf_bindings_[index] = BufferBinding{id, offset, size};
    tf_buffer_ = id;
    if (id) buffer_at(id)->ever_bound = true;
  } else {
    set_error(INVALID_ENUM);
  }
}

void GlContext::buffer_data(uint32_t target, ConstByteView data, uint64_t size, uint32_t usage) {
  Buffer* b = buffer_at(bound_buffer(target));
  if (!b) { set_error(INVALID_OPERATION); return; }
  if (usage != 0x88E0 && usage != 0x88E4 && usage != 0x88E8 && usage != 0x88E1 && usage != 0x88E5 && usage != 0x88E9 && usage != 0x88E2 && usage != 0x88E6 && usage != 0x88EA) {
    set_error(INVALID_ENUM);
    return;
  }
  const uint64_t n = data.size() ? data.size() : size;
  if (n > (256ull << 20) || total_bytes_ - (b->data.size() < total_bytes_ ? b->data.size() : total_bytes_) + n > kMaxTotalBytes) { set_error(OUT_OF_MEMORY); return; }
  total_bytes_ -= b->data.size() < total_bytes_ ? b->data.size() : total_bytes_;
  b->data.clear();
  if (data.size()) b->data.append(data);
  else b->data.put_zeros(n);
  total_bytes_ += n;
  b->usage = usage;
}

void GlContext::buffer_sub_data(uint32_t target, uint64_t offset, ConstByteView data) {
  Buffer* b = buffer_at(bound_buffer(target));
  if (!b) { set_error(INVALID_OPERATION); return; }
  if (offset > b->data.size() || data.size() > b->data.size() - offset) { set_error(INVALID_VALUE); return; }
  if (data.size()) b->data.writable_view().subview(offset, data.size()).copy_from(data);
}

bool GlContext::get_buffer_sub_data(uint32_t target, uint64_t offset, ByteView out) {
  Buffer* b = buffer_at(bound_buffer(target));
  if (!b) { set_error(INVALID_OPERATION); return false; }
  if (offset > b->data.size() || out.size() > b->data.size() - offset) { set_error(INVALID_VALUE); return false; }
  if (out.size()) out.copy_from(b->data.view().subview(offset, out.size()));
  return true;
}

void GlContext::copy_buffer_sub_data(uint32_t rt, uint32_t wt, uint64_t ro, uint64_t wo, uint64_t size) {
  Buffer* r = buffer_at(bound_buffer(rt));
  Buffer* w = buffer_at(bound_buffer(wt));
  if (!r || !w) { set_error(INVALID_OPERATION); return; }
  if (ro > r->data.size() || size > r->data.size() - ro || wo > w->data.size() || size > w->data.size() - wo) { set_error(INVALID_VALUE); return; }
  if (r == w && ro < wo + size && wo < ro + size) { set_error(INVALID_VALUE); return; }
  if (size) w->data.writable_view().subview(wo, size).copy_from(r->data.view().subview(ro, size));
}

bool GlContext::pixel_unpack_view(uint64_t offset, ConstByteView* out) {
  Buffer* b = buffer_at(pixel_unpack_);
  if (!b || offset > b->data.size()) return false;
  *out = b->data.view().subview(offset);
  return true;
}

bool GlContext::pixel_pack_view(uint64_t offset, ByteView* out) {
  Buffer* b = buffer_at(pixel_pack_);
  if (!b || offset > b->data.size()) return false;
  *out = b->data.writable_view().subview(offset);
  return true;
}

uint64_t GlContext::buffer_size(uint32_t id) const { return buffer_at(id) ? buffer_at(id)->data.size() : 0; }
uint32_t GlContext::buffer_usage(uint32_t id) const { return buffer_at(id) ? buffer_at(id)->usage : 0; }

// --- Textures --------------------------------------------------------------------------------------------------------------------------------------------

uint32_t GlContext::face_index(uint32_t target) { return is_cube_face(target) ? target - 0x8515 : 0; }

void GlContext::active_texture(uint32_t unit) {
  if (unit < 0x84C0 || unit >= 0x84C0 + kMaxTextureUnits) { set_error(INVALID_ENUM); return; }
  active_unit_ = unit - 0x84C0;
}

uint32_t* GlContext::bound_texture_slot(uint32_t target) {
  TextureUnit& u = units_[active_unit_];
  if (target == 0x0DE1) return &u.tex2d;
  if (target == 0x8513 || is_cube_face(target)) return &u.cube;
  if (target == 0x806F) return es3_ ? &u.tex3d : nullptr;
  if (target == 0x8C1A) return es3_ ? &u.array : nullptr;
  return nullptr;
}

Texture* GlContext::bound_texture_for(uint32_t target, uint32_t* face) {
  uint32_t* slot = bound_texture_slot(target);
  if (!slot) { set_error(INVALID_ENUM); return nullptr; }
  *face = face_index(target);
  Texture* t = texture_at(*slot);
  if (!t) set_error(INVALID_OPERATION);
  return t;
}

void GlContext::bind_texture(uint32_t target, uint32_t id) {
  uint32_t* slot = bound_texture_slot(target);
  if (!slot || is_cube_face(target)) { set_error(INVALID_ENUM); return; }
  if (id) {
    Texture* t = texture_at(id);
    if (!t) { set_error(INVALID_OPERATION); return; }
    if (t->target == 0) {
      t->target = target;
      const uint64_t n = (target == 0x8513 ? 6u : 1u) * kMaxLevels;
      for (uint64_t i = 0; i < n; ++i) t->images.push_back(UniquePointer<Image>());
    } else if (t->target != target) {
      set_error(INVALID_OPERATION);
      return;
    }
  }
  *slot = id;
}

void GlContext::bind_sampler(uint32_t unit, uint32_t id) {
  if (unit >= kMaxTextureUnits || (id && !sampler_at(id))) { set_error(INVALID_VALUE); return; }
  units_[unit].sampler = id;
}

Image* GlContext::texture_image(Texture* t, uint32_t target, int32_t level) {
  if (level < 0 || level >= static_cast<int32_t>(kMaxLevels)) return nullptr;
  const uint64_t idx = uint64_t{face_index(target)} * kMaxLevels + static_cast<uint32_t>(level);
  if (idx >= t->images.count()) return nullptr;
  Image* im = t->images[idx].get();
  return im && im->defined() ? im : nullptr;
}

Image* GlContext::make_texture_image(Texture* t, uint32_t target, int32_t level) {
  const uint64_t idx = uint64_t{face_index(target)} * kMaxLevels + static_cast<uint32_t>(level);
  if (idx >= t->images.count()) return nullptr;
  if (!t->images[idx]) t->images[idx] = UniquePointer<Image>::make(a_, a_);
  return t->images[idx].get();
}

namespace {
const FormatInfo* resolve_format(bool es3, uint32_t internal, uint32_t format, uint32_t type) {
  (void)format;
  const bool unsized = internal == 0x1906 || internal == 0x1907 || internal == 0x1908 || internal == 0x1909 || internal == 0x190A;
  if (unsized && (type == 0x1406 || type == 0x140B || type == 0x8D61)) return find_format(0x20000 + internal);
  if (internal == 0x1902 && (type == 0x1403 || type == 0x1405 || type == 0x1406)) return find_format(0x1902);
  (void)es3;
  return find_format(internal);
}
}  // namespace

void GlContext::tex_image(uint32_t target, int32_t level, uint32_t internal, uint32_t w, uint32_t h, uint32_t d, int32_t border, uint32_t format, uint32_t type, ConstByteView data,
                          bool has_data, bool three_d) {
  uint32_t face;
  Texture* t = bound_texture_for(target, &face);
  if (!t) return;
  if (border != 0 || level < 0 || level >= static_cast<int32_t>(kMaxLevels)) { set_error(INVALID_VALUE); return; }
  const bool layered = t->target == 0x806F || t->target == 0x8C1A;
  if (three_d != layered) { set_error(INVALID_ENUM); return; }
  if (w > kMaxTextureSize || h > kMaxTextureSize || d > (t->target == 0x806F ? 256u : 2048u)) { set_error(INVALID_VALUE); return; }
  if (t->target == 0x8513 && w != h) { set_error(INVALID_VALUE); return; }
  const FormatInfo* fi = resolve_format(es3_, internal, format, type);
  if (!fi) { set_error(INVALID_ENUM); return; }
  if (t->immutable) { set_error(INVALID_OPERATION); return; }
  if (!es3_ && internal != format) { set_error(INVALID_OPERATION); return; }
  uint32_t nc, bpp;
  bool packed;
  if (!pixel_layout(format, type, &nc, &bpp, &packed)) { set_error(INVALID_ENUM); return; }
  Image* im = make_texture_image(t, target, level);
  if (!im) { set_error(INVALID_OPERATION); return; }
  const uint64_t need = uint64_t{w} * h * d * texel_bytes(fi->store);
  if (need > (256ull << 20) || total_bytes_ + need > kMaxTotalBytes) { set_error(OUT_OF_MEMORY); return; }
  if (im->defined()) total_bytes_ -= im->px.size() < total_bytes_ ? im->px.size() : total_bytes_;
  if (w == 0 || h == 0 || d == 0) {
    im->info = nullptr;
    im->w = im->h = 0;
    return;
  }
  if (!im->allocate(fi, w, h, d)) { set_error(OUT_OF_MEMORY); return; }
  im->compressed = 0;
  total_bytes_ += im->px.size();
  if (has_data) {
    uint32_t err = 0;
    if (!write_region(*im, 0, 0, 0, w, h, d, format, type, data, unpack, &err)) set_error(err);
  } else if (fi->store == Store::Depth || fi->store == Store::DepthStencil) {
    // an undefined depth texture reads as zero
  }
  ++version_;
}

void GlContext::tex_sub_image(uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t zo, uint32_t w, uint32_t h, uint32_t d, uint32_t format, uint32_t type, ConstByteView data,
                              bool three_d) {
  uint32_t face;
  Texture* t = bound_texture_for(target, &face);
  if (!t) return;
  Image* im = texture_image(t, target, level);
  if (!im || im->compressed) { set_error(INVALID_OPERATION); return; }
  const bool layered = t->target == 0x806F || t->target == 0x8C1A;
  if (three_d != layered) { set_error(INVALID_ENUM); return; }
  if (xo < 0 || yo < 0 || zo < 0 || uint64_t{static_cast<uint32_t>(xo)} + w > im->w || uint64_t{static_cast<uint32_t>(yo)} + h > im->h || uint64_t{static_cast<uint32_t>(zo)} + d > im->d) {
    set_error(INVALID_VALUE);
    return;
  }
  uint32_t err = 0;
  if (!write_region(*im, static_cast<uint32_t>(xo), static_cast<uint32_t>(yo), static_cast<uint32_t>(zo), w, h, d, format, type, data, unpack, &err)) set_error(err);
  ++version_;
}

void GlContext::compressed_tex_image(uint32_t target, int32_t level, uint32_t internal, uint32_t w, uint32_t h, uint32_t d, int32_t border, ConstByteView data, bool three_d) {
  uint32_t face;
  Texture* t = bound_texture_for(target, &face);
  if (!t) return;
  const CompressedFormat* cf = find_compressed(internal);
  if (!cf) { set_error(INVALID_ENUM); return; }
  const bool layered = t->target == 0x806F || t->target == 0x8C1A;
  if (border != 0 || level < 0 || level >= static_cast<int32_t>(kMaxLevels) || w > kMaxTextureSize || h > kMaxTextureSize || three_d != layered) { set_error(INVALID_VALUE); return; }
  if (t->target == 0x806F && !sliced_3d_codec(cf->codec)) { set_error(INVALID_OPERATION); return; }   // only RGTC and BPTC are allowed in a 3D texture
  if (t->immutable) { set_error(INVALID_OPERATION); return; }
  if (data.size() != compressed_size(*cf, w, h, d)) { set_error(INVALID_VALUE); return; }
  Image* im = make_texture_image(t, target, level);
  const FormatInfo* fi = find_format(cf->decoded);
  if (!im || !fi) { set_error(INVALID_OPERATION); return; }
  const uint64_t need = uint64_t{w} * h * d * texel_bytes(fi->store);
  if (need > (256ull << 20) || total_bytes_ + need > kMaxTotalBytes) { set_error(OUT_OF_MEMORY); return; }
  if (im->defined()) total_bytes_ -= im->px.size() < total_bytes_ ? im->px.size() : total_bytes_;
  if (w == 0 || h == 0 || d == 0) { im->info = nullptr; im->w = im->h = 0; return; }
  if (!im->allocate(fi, w, h, d)) { set_error(OUT_OF_MEMORY); return; }
  total_bytes_ += im->px.size();
  im->compressed = internal;
  if (!decode_compressed(*cf, data, w, h, d, im, 0, 0, 0)) set_error(INVALID_VALUE);
  ++version_;
}

void GlContext::compressed_tex_sub_image(uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t zo, uint32_t w, uint32_t h, uint32_t d, uint32_t format, ConstByteView data, bool three_d) {
  uint32_t face;
  Texture* t = bound_texture_for(target, &face);
  if (!t) return;
  const CompressedFormat* cf = find_compressed(format);
  if (!cf) { set_error(INVALID_ENUM); return; }
  Image* im = texture_image(t, target, level);
  if (!im || im->compressed != format) { set_error(INVALID_OPERATION); return; }
  const bool layered = t->target == 0x806F || t->target == 0x8C1A;
  if (three_d != layered) { set_error(INVALID_ENUM); return; }
  if (xo < 0 || yo < 0 || zo < 0 || (static_cast<uint32_t>(xo) % cf->bw) || (static_cast<uint32_t>(yo) % cf->bh) || ((w % cf->bw) && static_cast<uint32_t>(xo) + w != im->w) || ((h % cf->bh) && static_cast<uint32_t>(yo) + h != im->h) ||
      uint64_t{static_cast<uint32_t>(xo)} + w > im->w || uint64_t{static_cast<uint32_t>(yo)} + h > im->h || uint64_t{static_cast<uint32_t>(zo)} + d > im->d) {
    set_error(INVALID_VALUE);
    return;
  }
  if (data.size() != compressed_size(*cf, w, h, d)) { set_error(INVALID_VALUE); return; }
  decode_compressed(*cf, data, w, h, d, im, static_cast<uint32_t>(xo), static_cast<uint32_t>(yo), static_cast<uint32_t>(zo));
  ++version_;
}

void GlContext::tex_storage(uint32_t target, uint32_t levels, uint32_t internal, uint32_t w, uint32_t h, uint32_t d) {
  uint32_t face;
  Texture* t = bound_texture_for(target, &face);
  if (!t) return;
  const CompressedFormat* cf = find_compressed(internal);
  const FormatInfo* fi = cf ? find_format(cf->decoded) : find_format(internal);
  if (!fi || levels == 0 || levels > kMaxLevels || w == 0 || h == 0 || d == 0 || w > kMaxTextureSize || h > kMaxTextureSize) { set_error(fi ? INVALID_VALUE : INVALID_ENUM); return; }
  if (t->immutable) { set_error(INVALID_OPERATION); return; }
  const uint32_t faces = t->target == 0x8513 ? 6 : 1;
  const bool layered = t->target == 0x806F;
  for (uint32_t f = 0; f < faces; ++f)
    for (uint32_t l = 0; l < levels; ++l) {
      const uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = layered ? (d >> l ? d >> l : 1) : d;
      Image* im = make_texture_image(t, t->target == 0x8513 ? 0x8515 + f : t->target, static_cast<int32_t>(l));
      if (!im || !im->allocate(fi, lw, lh, ld)) { set_error(OUT_OF_MEMORY); return; }
      if (cf) im->compressed = internal;
      total_bytes_ += im->px.size();
    }
  t->immutable = true;
  t->immutable_levels = levels;
  ++version_;
}

void GlContext::tex_parameter(uint32_t target, uint32_t pname, double value) {
  uint32_t* slot = bound_texture_slot(target);
  if (!slot || is_cube_face(target)) { set_error(INVALID_ENUM); return; }
  Texture* t = texture_at(*slot);
  if (!t) { set_error(INVALID_OPERATION); return; }
  const uint32_t v = static_cast<uint32_t>(value);
  switch (pname) {
    case 0x2800: if (v != 0x2600 && v != 0x2601) { set_error(INVALID_ENUM); return; } t->mag_filter = v; break;
    case 0x2801: if (v < 0x2600 || (v > 0x2601 && (v < 0x2700 || v > 0x2703))) { set_error(INVALID_ENUM); return; } t->min_filter = v; break;
    case 0x2802: case 0x2803: case 0x8072:
      if (v != 0x2901 && v != 0x812F && v != 0x8370) { set_error(INVALID_ENUM); return; }
      (pname == 0x2802 ? t->wrap_s : pname == 0x2803 ? t->wrap_t : t->wrap_r) = v;
      break;
    case 0x884C: t->compare_mode = v == 0x884E ? v : 0; break;
    case 0x884D: t->compare_func = v; break;
    case 0x813C: t->base_level = static_cast<int32_t>(value); break;
    case 0x813D: t->max_level = static_cast<int32_t>(value); break;
    case 0x813A: t->min_lod = static_cast<float>(value); break;
    case 0x813B: t->max_lod = static_cast<float>(value); break;
    case 0x84FE: break;   // anisotropy is accepted and ignored
    default: set_error(INVALID_ENUM); break;
  }
  ++version_;
}

bool GlContext::get_tex_parameter(uint32_t target, uint32_t pname, double* out) {
  uint32_t* slot = bound_texture_slot(target);
  if (!slot) { set_error(INVALID_ENUM); return false; }
  Texture* t = texture_at(*slot);
  if (!t) { set_error(INVALID_OPERATION); return false; }
  switch (pname) {
    case 0x2800: *out = t->mag_filter; return true;
    case 0x2801: *out = t->min_filter; return true;
    case 0x2802: *out = t->wrap_s; return true;
    case 0x2803: *out = t->wrap_t; return true;
    case 0x8072: *out = t->wrap_r; return true;
    case 0x884C: *out = t->compare_mode ? t->compare_mode : 0x0; return true;
    case 0x884D: *out = t->compare_func; return true;
    case 0x813C: *out = t->base_level; return true;
    case 0x813D: *out = t->max_level; return true;
    case 0x813A: *out = t->min_lod; return true;
    case 0x813B: *out = t->max_lod; return true;
    case 0x84FE: *out = 1; return true;
    case 0x912F: *out = t->immutable; return true;
    case 0x82DF: *out = t->immutable_levels; return true;
    default: set_error(INVALID_ENUM); return false;
  }
}

bool GlContext::texture_level_info(uint32_t target, int32_t level, uint32_t* w, uint32_t* h, uint32_t* d, uint32_t* internal) {
  uint32_t* slot = bound_texture_slot(target);
  if (!slot) return false;
  Texture* t = texture_at(*slot);
  if (!t) return false;
  Image* im = texture_image(t, target, level);
  if (!im) return false;
  *w = im->w;
  *h = im->h;
  *d = im->d;
  *internal = im->info->internal;
  return true;
}

void GlContext::generate_mipmap(uint32_t target) {
  uint32_t face;
  Texture* t = bound_texture_for(target, &face);
  if (!t) return;
  const uint32_t faces = t->target == 0x8513 ? 6 : 1;
  for (uint32_t f = 0; f < faces; ++f) {
    const uint32_t tgt = t->target == 0x8513 ? 0x8515 + f : t->target;
    Image* base = texture_image(t, tgt, t->base_level < 0 ? 0 : t->base_level);
    if (!base) { set_error(INVALID_OPERATION); return; }
    if (base->info->store == Store::DepthStencil || base->info->store == Store::Depth || base->compressed) { set_error(INVALID_OPERATION); return; }
    const bool layered3d = t->target == 0x806F;
    uint32_t w = base->w, h = base->h, d = base->d;
    for (int32_t l = (t->base_level < 0 ? 0 : t->base_level) + 1; l < static_cast<int32_t>(kMaxLevels) && (w > 1 || h > 1 || (layered3d && d > 1)); ++l) {
      w = w > 1 ? w / 2 : 1;
      h = h > 1 ? h / 2 : 1;
      if (layered3d) d = d > 1 ? d / 2 : 1;
      Image* src = texture_image(t, tgt, l - 1);
      Image* dst = make_texture_image(t, tgt, l);
      if (!src || !dst) break;
      if (dst->defined()) total_bytes_ -= dst->px.size() < total_bytes_ ? dst->px.size() : total_bytes_;
      if (!dst->allocate(src->info, w, h, d)) { set_error(OUT_OF_MEMORY); return; }
      total_bytes_ += dst->px.size();
      downsample(*src, dst);
    }
  }
  ++version_;
}

void GlContext::sampler_parameter(uint32_t id, uint32_t pname, double value) {
  SamplerObj* s = sampler_at(id);
  if (!s) { set_error(INVALID_OPERATION); return; }
  const uint32_t v = static_cast<uint32_t>(value);
  switch (pname) {
    case 0x2800: s->mag_filter = v; break;
    case 0x2801: s->min_filter = v; break;
    case 0x2802: s->wrap_s = v; break;
    case 0x2803: s->wrap_t = v; break;
    case 0x8072: s->wrap_r = v; break;
    case 0x884C: s->compare_mode = v == 0x884E ? v : 0; break;
    case 0x884D: s->compare_func = v; break;
    case 0x813A: s->min_lod = static_cast<float>(value); break;
    case 0x813B: s->max_lod = static_cast<float>(value); break;
    default: break;
  }
}

void GlContext::pixel_store(uint32_t pname, double value) {
  const uint32_t v = static_cast<uint32_t>(value);
  switch (pname) {
    case 0x0CF5: if (v != 1 && v != 2 && v != 4 && v != 8) { set_error(INVALID_VALUE); return; } unpack.alignment = v; break;
    case 0x0D05: if (v != 1 && v != 2 && v != 4 && v != 8) { set_error(INVALID_VALUE); return; } pack.alignment = v; break;
    case 0x9240: unpack.flip_y = value != 0; break;
    case 0x9241: unpack.premultiply = value != 0; break;
    case 0x9243: break;
    case 0x0CF2: unpack.row_length = v; break;
    case 0x0CF4: unpack.skip_pixels = v; break;
    case 0x0CF3: unpack.skip_rows = v; break;
    case 0x806E: unpack.image_height = v; break;
    case 0x806D: unpack.skip_images = v; break;
    case 0x0D02: pack.row_length = v; break;
    case 0x0D04: pack.skip_pixels = v; break;
    case 0x0D03: pack.skip_rows = v; break;
    default: set_error(INVALID_ENUM); break;
  }
}

// --- Renderbuffers and framebuffers ---------------------------------------------------------------------------------------------------------------------

void GlContext::bind_renderbuffer(uint32_t id) {
  if (id && !rb_at(id)) { set_error(INVALID_OPERATION); return; }
  rb_bound_ = id;
}

void GlContext::renderbuffer_storage(uint32_t internal, uint32_t w, uint32_t h, uint32_t samples) {
  Renderbuffer* rb = rb_at(rb_bound_);
  if (!rb) { set_error(INVALID_OPERATION); return; }
  const FormatInfo* fi = find_format(internal);
  if (internal == 0x84F9) fi = find_format(0x88F0);
  if (!fi) { set_error(INVALID_ENUM); return; }
  if (w > kMaxTextureSize || h > kMaxTextureSize) { set_error(INVALID_VALUE); return; }
  if (rb->image.defined()) total_bytes_ -= rb->image.px.size() < total_bytes_ ? rb->image.px.size() : total_bytes_;
  if (w == 0 || h == 0) { rb->image.info = nullptr; rb->image.w = rb->image.h = 0; return; }
  const uint32_t ss = samples > 1 ? 2 : 1;
  if (!rb->image.allocate(fi, w * ss, h * ss, 1)) { set_error(OUT_OF_MEMORY); return; }
  rb->image.ss = ss;
  total_bytes_ += rb->image.px.size();
  rb->internal = internal;
  rb->samples = samples;
}

bool GlContext::get_renderbuffer_parameter(uint32_t pname, double* out) {
  Renderbuffer* rb = rb_at(rb_bound_);
  if (!rb) { set_error(INVALID_OPERATION); return false; }
  switch (pname) {
    case 0x8D42: *out = rb->image.w / rb->image.ss; return true;
    case 0x8D43: *out = rb->image.h / rb->image.ss; return true;
    case 0x8D44: *out = rb->internal; return true;
    case 0x8CAB: *out = rb->samples; return true;
    case 0x8D50: *out = rb->image.info && rb->image.info->base != 0x1902 && rb->image.info->base != 0x84F9 ? 8 : 0; return true;
    case 0x8D51: *out = rb->image.info && rb->image.info->base != 0x1902 && rb->image.info->base != 0x84F9 ? 8 : 0; return true;
    case 0x8D52: *out = rb->image.info && rb->image.info->base != 0x1902 && rb->image.info->base != 0x84F9 ? 8 : 0; return true;
    case 0x8D53: *out = rb->image.info && rb->image.info->base != 0x1902 && rb->image.info->base != 0x84F9 ? 8 : 0; return true;
    case 0x8D54: *out = rb->image.info && rb->image.info->store != Store::U8x4 && rb->image.info->base != 0x84F9 ? 24 : (rb->image.info && rb->image.info->base == 0x84F9 ? 24 : 0); return true;
    case 0x8D55: *out = rb->image.info && rb->image.info->base == 0x84F9 ? 8 : 0; return true;
    default: set_error(INVALID_ENUM); return false;
  }
}

void GlContext::bind_framebuffer(uint32_t target, uint32_t id) {
  if (id && !fb_at(id)) { set_error(INVALID_OPERATION); return; }
  if (target == 0x8D40) { draw_fb_ = id; read_fb_ = id; }
  else if (target == 0x8CA9 && es3_) draw_fb_ = id;
  else if (target == 0x8CA8 && es3_) read_fb_ = id;
  else set_error(INVALID_ENUM);
}

void GlContext::framebuffer_texture(uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t tex, int32_t level, int32_t layer) {
  const uint32_t id = target == 0x8CA8 ? read_fb_ : draw_fb_;
  Framebuffer* fb = fb_at(id);
  if (!fb) { set_error(INVALID_OPERATION); return; }
  Attachment at;
  if (tex) {
    Texture* t = texture_at(tex);
    if (!t) { set_error(INVALID_OPERATION); return; }
    at.kind = 1;
    at.id = tex;
    at.face_target = textarget;
    at.level = level;
    at.layer = layer;
  }
  if (attachment >= 0x8CE0 && attachment < 0x8CE0 + kMaxDrawBuffers) fb->color[attachment - 0x8CE0] = at;
  else if (attachment == 0x8D00) fb->depth = at;
  else if (attachment == 0x8D20) fb->stencil = at;
  else if (attachment == 0x821A) { fb->depth = at; fb->stencil = at; }
  else set_error(INVALID_ENUM);
}

void GlContext::framebuffer_renderbuffer(uint32_t target, uint32_t attachment, uint32_t rb) {
  const uint32_t id = target == 0x8CA8 ? read_fb_ : draw_fb_;
  Framebuffer* fb = fb_at(id);
  if (!fb) { set_error(INVALID_OPERATION); return; }
  Attachment at;
  if (rb) {
    if (!rb_at(rb)) { set_error(INVALID_OPERATION); return; }
    at.kind = 2;
    at.id = rb;
  }
  if (attachment >= 0x8CE0 && attachment < 0x8CE0 + kMaxDrawBuffers) fb->color[attachment - 0x8CE0] = at;
  else if (attachment == 0x8D00) fb->depth = at;
  else if (attachment == 0x8D20) fb->stencil = at;
  else if (attachment == 0x821A) { fb->depth = at; fb->stencil = at; }
  else set_error(INVALID_ENUM);
}

bool GlContext::resolve_image(const Attachment& at, Image** img, uint32_t* layer) {
  *layer = 0;
  if (at.kind == 1) {
    Texture* t = texture_at(at.id);
    if (!t) return false;
    Image* im = texture_image(t, at.face_target, at.level);
    if (!im) return false;
    *img = im;
    *layer = static_cast<uint32_t>(at.layer < 0 ? 0 : at.layer);
    return *layer < im->d;
  }
  if (at.kind == 2) {
    Renderbuffer* rb = rb_at(at.id);
    if (!rb || !rb->image.defined()) return false;
    *img = &rb->image;
    return true;
  }
  return false;
}

void GlContext::resolve_target(uint32_t fb_id, bool draw, Target* t) {
  *t = Target();
  if (fb_id == 0) {
    t->is_default = true;
    t->w = def_color_.w;
    t->h = def_color_.h;
    t->ss = def_color_.ss;
    t->color_count = 1;
    if (!draw || default_draw_back_) t->color[0] = &def_color_;
    if (def_depth_.defined()) { t->depth = &def_depth_; t->stencil = &def_depth_; }
    t->ok = true;
    return;
  }
  Framebuffer* fb = fb_at(fb_id);
  if (!fb) return;
  bool any = false;
  uint32_t dw = 0, dh = 0;
  t->color_count = kMaxDrawBuffers;
  for (uint32_t i = 0; i < kMaxDrawBuffers; ++i) {
    const uint32_t src = draw ? fb->draw_buffers[i] : (fb->read_buffer == 0x8CE0 + i ? fb->read_buffer : 0);
    if (src != 0x8CE0 + i) continue;
    Image* im;
    uint32_t layer;
    if (!resolve_image(fb->color[i], &im, &layer)) continue;
    t->color[i] = im;
    t->layer[i] = layer;
    if (!any) { dw = im->w; dh = im->h; any = true; }
  }
  Image* dimg;
  uint32_t layer;
  if (fb->depth.kind && resolve_image(fb->depth, &dimg, &layer)) {
    t->depth = dimg;
    t->depth_layer = layer;
    if (!any) { dw = dimg->w; dh = dimg->h; any = true; }
  }
  if (fb->stencil.kind && resolve_image(fb->stencil, &dimg, &layer)) {
    t->stencil = dimg;
    if (!any) { dw = dimg->w; dh = dimg->h; any = true; }
  }
  t->w = dw;
  t->h = dh;
  t->ok = any;
  Image* first = t->depth ? t->depth : t->stencil;
  for (uint32_t i = 0; i < kMaxDrawBuffers; ++i)
    if (t->color[i]) { first = t->color[i]; break; }
  if (first) t->ss = first->ss;
}

bool GlContext::framebuffer_complete(uint32_t fb_id, uint32_t* status) {
  if (fb_id == 0) { *status = 0x8CD5; return true; }
  Framebuffer* fb = fb_at(fb_id);
  if (!fb) { *status = 0x8CD7; return false; }
  bool any = false;
  uint32_t w = 0, h = 0, ss = 1;
  auto check = [&](const Attachment& at, bool color, bool depth, bool stencil) -> uint32_t {
    if (!at.kind) return 0x8CD5;
    Image* im;
    uint32_t layer;
    if (!resolve_image(at, &im, &layer)) return 0x8CD6;
    const FormatInfo* fi = im->info;
    if (color && (!fi->color_renderable || fi->store == Store::Depth || fi->store == Store::DepthStencil)) return 0x8CD6;
    if (depth && fi->store != Store::Depth && fi->store != Store::DepthStencil) return 0x8CD6;
    if (stencil && fi->store != Store::DepthStencil) return 0x8CD6;
    if (!any) { w = im->w; h = im->h; ss = im->ss; any = true; }
    else if (im->ss != ss) return 0x8D56;
    else if (im->w != w || im->h != h) return 0x8CD9;
    return 0x8CD5;
  };
  for (const Attachment& at : fb->color) {
    const uint32_t s = check(at, true, false, false);
    if (s != 0x8CD5) { *status = s; return false; }
  }
  uint32_t s = check(fb->depth, false, true, false);
  if (s != 0x8CD5) { *status = s; return false; }
  s = check(fb->stencil, false, false, true);
  if (s != 0x8CD5) { *status = s; return false; }
  if (!any) { *status = 0x8CD7; return false; }
  *status = 0x8CD5;
  return true;
}

uint32_t GlContext::check_framebuffer_status(uint32_t target) {
  uint32_t st;
  framebuffer_complete(target == 0x8CA8 ? read_fb_ : draw_fb_, &st);
  return st;
}

bool GlContext::get_framebuffer_attachment_parameter(uint32_t target, uint32_t attachment, uint32_t pname, double* out) {
  const uint32_t id = target == 0x8CA8 ? read_fb_ : draw_fb_;
  Framebuffer* fb = fb_at(id);
  if (!fb) {
    if (id == 0) {   // default framebuffer
      if (pname == 0x8CD0) { *out = attachment == 0x0405 || attachment == 0x1800 ? 0x1702 : 0; return true; }
      *out = 0;
      return true;
    }
    return false;
  }
  const Attachment* at = nullptr;
  if (attachment >= 0x8CE0 && attachment < 0x8CE0 + kMaxDrawBuffers) at = &fb->color[attachment - 0x8CE0];
  else if (attachment == 0x8D00 || attachment == 0x821A) at = &fb->depth;
  else if (attachment == 0x8D20) at = &fb->stencil;
  else { set_error(INVALID_ENUM); return false; }
  switch (pname) {
    case 0x8CD0: *out = at->kind == 1 ? 0x1702 : (at->kind == 2 ? 0x8D41 : 0); return true;
    case 0x8CD1: *out = at->id; return true;
    case 0x8CD2: *out = at->level; return true;
    case 0x8CD3: *out = at->face_target >= 0x8515 ? at->face_target : 0; return true;
    case 0x8CD4: *out = at->layer; return true;
    default: {
      Image* im;
      uint32_t layer;
      if (!resolve_image(*at, &im, &layer)) { *out = 0; return true; }
      switch (pname) {
        case 0x8215: case 0x8216: case 0x8217: case 0x8218: *out = im->info->base == 0x1902 || im->info->base == 0x84F9 ? 0 : 8; return true;
        case 0x8219: *out = im->info->base == 0x1902 || im->info->base == 0x84F9 ? 0 : 8; return true;
        case 0x8212: case 0x8213: case 0x8214: *out = 8; return true;
        case 0x8210: *out = im->info->srgb ? 0x8C40 : 0x8C01; return true;
        case 0x8211: *out = im->info->store == Store::F32x4 ? 0x1406 : (im->info->store == Store::I32x4 ? 0x1404 : (im->info->store == Store::U32x4 ? 0x1405 : 0x8C17)); return true;
        default: set_error(INVALID_ENUM); return false;
      }
    }
  }
}

void GlContext::draw_buffers(ArrayView<const uint32_t> bufs) {
  if (bufs.size() > kMaxDrawBuffers) { set_error(INVALID_VALUE); return; }
  if (draw_fb_ == 0) {
    if (bufs.size() != 1 || (bufs[0] != 0x0405 && bufs[0] != 0)) { set_error(INVALID_OPERATION); return; }
    default_draw_back_ = bufs[0] == 0x0405;
    return;
  }
  Framebuffer* fb = fb_at(draw_fb_);
  if (!fb) return;
  for (uint32_t i = 0; i < kMaxDrawBuffers; ++i) fb->draw_buffers[i] = i < bufs.size() ? bufs[i] : 0;
}

void GlContext::read_buffer(uint32_t src) {
  if (read_fb_ == 0) { default_read_back_ = src == 0x0405; return; }
  Framebuffer* fb = fb_at(read_fb_);
  if (fb) fb->read_buffer = src;
}

void GlContext::invalidate_framebuffer(uint32_t, ArrayView<const uint32_t>) {}

// --- State -------------------------------------------------------------------------------------------------------------------------------------------------

void GlContext::enable(uint32_t cap, bool on) {
  switch (cap) {
    case 0x0BE2: blend_ = on; break;
    case 0x0B44: cull_ = on; break;
    case 0x0B71: depth_test_ = on; break;
    case 0x0B90: stencil_test_ = on; break;
    case 0x0C11: scissor_test_ = on; break;
    case 0x8037: offset_fill_ = on; break;
    case 0x0BD0: dither_ = on; break;
    case 0x809E: alpha_to_coverage_ = on; break;
    case 0x80A0: sample_coverage_en_ = on; break;
    case 0x8C89: rasterizer_discard_ = on; break;
    default: set_error(INVALID_ENUM); break;
  }
}

bool GlContext::is_enabled(uint32_t cap) const {
  switch (cap) {
    case 0x0BE2: return blend_;
    case 0x0B44: return cull_;
    case 0x0B71: return depth_test_;
    case 0x0B90: return stencil_test_;
    case 0x0C11: return scissor_test_;
    case 0x8037: return offset_fill_;
    case 0x0BD0: return dither_;
    case 0x809E: return alpha_to_coverage_;
    case 0x80A0: return sample_coverage_en_;
    case 0x8C89: return rasterizer_discard_;
    default: return false;
  }
}

void GlContext::viewport(int32_t x, int32_t y, int32_t w, int32_t h) {
  if (w < 0 || h < 0) { set_error(INVALID_VALUE); return; }
  vp_[0] = x; vp_[1] = y; vp_[2] = w; vp_[3] = h;
}
void GlContext::scissor(int32_t x, int32_t y, int32_t w, int32_t h) {
  if (w < 0 || h < 0) { set_error(INVALID_VALUE); return; }
  sc_[0] = x; sc_[1] = y; sc_[2] = w; sc_[3] = h;
}
void GlContext::depth_range(float n, float f) {
  depth_n_ = n < 0 ? 0 : (n > 1 ? 1 : n);
  depth_f_ = f < 0 ? 0 : (f > 1 ? 1 : f);
}
void GlContext::blend_color(float r, float g, float b, float a) { blend_color_[0] = r; blend_color_[1] = g; blend_color_[2] = b; blend_color_[3] = a; }
void GlContext::blend_equation_separate(uint32_t rgb, uint32_t a) {
  auto ok = [&](uint32_t e) { return e == 0x8006 || e == 0x800A || e == 0x800B || (es3_ && (e == 0x8007 || e == 0x8008)) || e == 0x8007 || e == 0x8008; };
  if (!ok(rgb) || !ok(a)) { set_error(INVALID_ENUM); return; }
  blend_eq_rgb_ = rgb;
  blend_eq_a_ = a;
}
void GlContext::blend_func_separate(uint32_t sr, uint32_t dr, uint32_t sa, uint32_t da) {
  blend_sr_ = sr; blend_dr_ = dr; blend_sa_ = sa; blend_da_ = da;
}
void GlContext::color_mask(bool r, bool g, bool b, bool a) { cmask_[0] = r; cmask_[1] = g; cmask_[2] = b; cmask_[3] = a; }
void GlContext::depth_mask(bool m) { dmask_ = m; }
void GlContext::depth_func(uint32_t f) {
  if (f < 0x200 || f > 0x207) { set_error(INVALID_ENUM); return; }
  dfunc_ = f;
}
void GlContext::stencil_func_separate(uint32_t face, uint32_t func, int32_t ref, uint32_t mask) {
  for (int i = 0; i < 2; ++i)
    if ((i == 0 && (face == 0x404 || face == 0x408)) || (i == 1 && (face == 0x405 || face == 0x408))) { sfunc_[i] = func; sref_[i] = ref; smask_[i] = mask; }
}
void GlContext::stencil_op_separate(uint32_t face, uint32_t sfail, uint32_t dpfail, uint32_t dppass) {
  for (int i = 0; i < 2; ++i)
    if ((i == 0 && (face == 0x404 || face == 0x408)) || (i == 1 && (face == 0x405 || face == 0x408))) { sfail_[i] = sfail; sdpfail_[i] = dpfail; sdppass_[i] = dppass; }
}
void GlContext::stencil_mask_separate(uint32_t face, uint32_t mask) {
  for (int i = 0; i < 2; ++i)
    if ((i == 0 && (face == 0x404 || face == 0x408)) || (i == 1 && (face == 0x405 || face == 0x408))) swmask_[i] = mask;
}

void GlContext::use_program(uint32_t id) {
  if (id) {
    ProgramObj* p = program_at(id);
    if (!p) { set_error(INVALID_VALUE); return; }
    if (!p->linked) { set_error(INVALID_OPERATION); return; }
  }
  const uint32_t old = program_;
  program_ = id;
  if (old && old != id) {
    ProgramObj* o = program_at(old);
    if (o && o->deleted) programs_[old - 1].reset();
  }
}

void GlContext::bind_vao(uint32_t id) {
  if (id && !vao_at(id)) { set_error(INVALID_OPERATION); return; }
  vao_id_ = id;
}

// --- Vertex arrays ------------------------------------------------------------------------------------------------------------------------------------------

void GlContext::enable_vertex_attrib(uint32_t index, bool on) {
  if (index >= kMaxAttribs) { set_error(INVALID_VALUE); return; }
  current_vao_ptr()->attribs[index].enabled = on;
}

void GlContext::vertex_attrib_pointer(uint32_t index, int32_t size, uint32_t type, bool normalized, uint32_t stride, uint64_t offset, bool integer) {
  if (index >= kMaxAttribs || size < 1 || size > 4 || stride > 255 * 4) { set_error(INVALID_VALUE); return; }
  switch (type) {
    case 0x1400: case 0x1401: case 0x1402: case 0x1403: case 0x1406: break;
    case 0x1404: case 0x1405: case 0x140B: case 0x8368: case 0x8D9F:
      if (!es3_) { set_error(INVALID_ENUM); return; }
      break;
    default: set_error(INVALID_ENUM); return;
  }
  if (array_buffer_ == 0 && offset != 0) { set_error(INVALID_OPERATION); return; }
  VertexAttrib& at = current_vao_ptr()->attribs[index];
  at.buffer = array_buffer_;
  at.size = static_cast<uint32_t>(size);
  at.type = type;
  at.normalized = normalized;
  at.integer = integer;
  at.stride = stride;
  at.offset = offset;
}

void GlContext::vertex_attrib_divisor(uint32_t index, uint32_t divisor) {
  if (index >= kMaxAttribs) { set_error(INVALID_VALUE); return; }
  current_vao_ptr()->attribs[index].divisor = divisor;
}

void GlContext::vertex_attrib_value(uint32_t index, const float* v, const int32_t* iv) {
  if (index >= kMaxAttribs) { set_error(INVALID_VALUE); return; }
  VertexAttrib& at = default_vao_.attribs[index];
  if (iv) {
    for (int i = 0; i < 4; ++i) at.cur_i[i] = iv[i];
    at.cur_is_int = true;
  } else {
    for (int i = 0; i < 4; ++i) at.cur_f[i] = v[i];
    at.cur_is_int = false;
  }
  // The current value is context state, shared by every vertex array object.
  for (auto& vao : vaos_)
    if (vao) { vao->attribs[index].cur_f[0] = at.cur_f[0]; vao->attribs[index].cur_f[1] = at.cur_f[1]; vao->attribs[index].cur_f[2] = at.cur_f[2]; vao->attribs[index].cur_f[3] = at.cur_f[3];
               for (int i = 0; i < 4; ++i) vao->attribs[index].cur_i[i] = at.cur_i[i]; vao->attribs[index].cur_is_int = at.cur_is_int; }
}

const VertexAttrib* GlContext::attrib(uint32_t index) const {
  if (index >= kMaxAttribs) return nullptr;
  return &(vao_id_ == 0 ? &default_vao_ : vaos_[vao_id_ - 1].get())->attribs[index];
}

bool GlContext::get_vertex_attrib(uint32_t index, uint32_t pname, double* out, uint32_t* count) {
  const VertexAttrib* at = attrib(index);
  if (!at) { set_error(INVALID_VALUE); return false; }
  *count = 1;
  switch (pname) {
    case 0x8622: *out = at->enabled; return true;
    case 0x8623: *out = at->size; return true;
    case 0x8624: *out = at->stride; return true;
    case 0x8625: *out = at->type; return true;
    case 0x886A: *out = at->normalized; return true;
    case 0x889F: *out = at->buffer; return true;
    case 0x88FD: *out = at->integer; return true;
    case 0x88FE: *out = at->divisor; return true;
    case 0x8626:
      *count = 4;
      for (int i = 0; i < 4; ++i) out[i] = at->cur_is_int ? at->cur_i[i] : at->cur_f[i];
      return true;
    default: set_error(INVALID_ENUM); return false;
  }
}

// --- Queries -----------------------------------------------------------------------------------------------------------------------------------------------

void GlContext::begin_query(uint32_t target, uint32_t id) {
  QueryObj* q = query_at(id);
  if (!q) { set_error(INVALID_OPERATION); return; }
  q->target = target;
  q->active = true;
  q->ended = false;
  q->result = 0;
  const uint32_t slot = target == 0x8C2F ? 0 : (target == 0x8D6A ? 1 : 2);
  query_active_[slot] = id;
}

void GlContext::end_query(uint32_t target) {
  const uint32_t slot = target == 0x8C2F ? 0 : (target == 0x8D6A ? 1 : 2);
  QueryObj* q = query_at(query_active_[slot]);
  if (!q) { set_error(INVALID_OPERATION); return; }
  q->active = false;
  q->ended = true;
  query_active_[slot] = 0;
}

bool GlContext::query_result(uint32_t id, uint32_t pname, double* out) {
  QueryObj* q = query_at(id);
  if (!q) { set_error(INVALID_OPERATION); return false; }
  if (pname == 0x8867) *out = q->ended ? 1 : 0;
  else *out = static_cast<double>(q->result);
  return true;
}

uint32_t GlContext::current_query(uint32_t target) const {
  const uint32_t slot = target == 0x8C2F ? 0 : (target == 0x8D6A ? 1 : 2);
  return query_active_[slot];
}

// --- Parameters ---------------------------------------------------------------------------------------------------------------------------------------------

bool GlContext::get_integer(uint32_t pname, double* o, uint32_t* count) {
  *count = 1;
  switch (pname) {
    case 0x0D33: *o = kMaxTextureSize; return true;
    case 0x851C: *o = kMaxTextureSize; return true;
    case 0x84E8: *o = kMaxTextureSize; return true;
    case 0x8073: *o = 256; return true;
    case 0x88FF: *o = 2048; return true;
    case 0x8869: *o = kMaxAttribs; return true;
    case 0x8872: case 0x8B4C: *o = es3_ ? 16 : (pname == 0x8872 ? 16 : 0); return true;
    case 0x8B4D: *o = 32; return true;
    case 0x8DFC: *o = 30; return true;
    case 0x8DFB: case 0x8DFD: *o = 1024; return true;
    case 0x8B4B: *o = 120; return true;
    case 0x9122: *o = 120; return true;
    case 0x9125: *o = 120; return true;
    case 0x8824: *o = kMaxDrawBuffers; return true;
    case 0x8CDF: *o = kMaxDrawBuffers; return true;
    case 0x8A2F: *o = kMaxUniformBindings; return true;
    case 0x8D57: *o = 4; return true;
    case 0x8A2B: case 0x8A2D: *o = 12; return true;
    case 0x8A2E: case 0x8A33: *o = 12; return true;
    case 0x8A30: *o = 65536; return true;
    case 0x8A34: *o = 16; return true;
    case 0x80E8: case 0x80E9: *o = 1 << 20; return true;
    case 0x84FD: *o = 16; return true;
    case 0x8904: *o = -8; return true;
    case 0x8905: *o = 7; return true;
    case 0x8C8A: case 0x8C8B: case 0x8C80: *o = 4; return true;
    case 0x0D3A: *count = 2; o[0] = 8192; o[1] = 8192; return true;
    case 0x0BA2: *count = 4; for (int i = 0; i < 4; ++i) o[i] = vp_[i]; return true;
    case 0x0C10: *count = 4; for (int i = 0; i < 4; ++i) o[i] = sc_[i]; return true;
    case 0x846E: *count = 2; o[0] = 1; o[1] = 16; return true;
    case 0x846D: *count = 2; o[0] = 1; o[1] = 64; return true;
    case 0x0B70: *count = 2; o[0] = depth_n_; o[1] = depth_f_; return true;
    case 0x0C22: *count = 4; for (int i = 0; i < 4; ++i) o[i] = clear_color_[i]; return true;
    case 0x8005: *count = 4; for (int i = 0; i < 4; ++i) o[i] = blend_color_[i]; return true;
    case 0x0C23: *count = 4; for (int i = 0; i < 4; ++i) o[i] = cmask_[i]; return true;
    case 0x0B72: *o = dmask_; return true;
    case 0x0B74: *o = dfunc_; return true;
    case 0x0B73: *o = clear_depth_; return true;
    case 0x0B91: *o = clear_stencil_; return true;
    case 0x80C9: *o = blend_sr_; return true;
    case 0x80C8: *o = blend_dr_; return true;
    case 0x80CB: *o = blend_sa_; return true;
    case 0x80CA: *o = blend_da_; return true;
    case 0x8009: *o = blend_eq_rgb_; return true;
    case 0x883D: *o = blend_eq_a_; return true;
    case 0x0B45: *o = cull_mode_; return true;
    case 0x0B46: *o = front_face_; return true;
    case 0x0B21: *o = line_width_; return true;
    case 0x8038: *o = po_factor_; return true;
    case 0x2A00: *o = po_units_; return true;
    case 0x0B92: *o = sfunc_[0]; return true;
    case 0x8800: *o = sfunc_[1]; return true;
    case 0x0B97: *o = sref_[0]; return true;
    case 0x8CA3: *o = sref_[1]; return true;
    case 0x0B93: *o = smask_[0]; return true;
    case 0x8CA4: *o = smask_[1]; return true;
    case 0x0B98: *o = swmask_[0]; return true;
    case 0x8CA5: *o = swmask_[1]; return true;
    case 0x0B94: *o = sfail_[0]; return true;
    case 0x0B95: *o = sdpfail_[0]; return true;
    case 0x0B96: *o = sdppass_[0]; return true;
    case 0x8801: *o = sfail_[1]; return true;
    case 0x8802: *o = sdpfail_[1]; return true;
    case 0x8803: *o = sdppass_[1]; return true;
    case 0x84E0: *o = 0x84C0 + active_unit_; return true;
    case 0x8B8D: *o = program_; return true;
    case 0x8894: *o = array_buffer_; return true;
    case 0x8895: *o = bound_buffer(GL_ELEMENT_ARRAY_BUFFER); return true;
    case 0x8A28: *o = uniform_buffer_; return true;
    case 0x8F36: *o = copy_read_; return true;
    case 0x8F37: *o = copy_write_; return true;
    case 0x88ED: *o = pixel_pack_; return true;
    case 0x88EF: *o = pixel_unpack_; return true;
    case 0x8CA6: *o = draw_fb_; return true;
    case 0x8CAA: *o = read_fb_; return true;
    case 0x8CA7: *o = rb_bound_; return true;
    case 0x8069: *o = units_[active_unit_].tex2d; return true;
    case 0x8514: *o = units_[active_unit_].cube; return true;
    case 0x806A: *o = units_[active_unit_].tex3d; return true;
    case 0x8C1D: *o = units_[active_unit_].array; return true;
    case 0x8919: *o = units_[active_unit_].sampler; return true;
    case 0x85B5: *o = vao_id_; return true;
    case 0x0CF5: *o = unpack.alignment; return true;
    case 0x0D05: *o = pack.alignment; return true;
    case 0x9240: *o = unpack.flip_y; return true;
    case 0x9241: *o = unpack.premultiply; return true;
    case 0x9243: *o = 0x9244; return true;
    case 0x0CF2: *o = unpack.row_length; return true;
    case 0x0CF4: *o = unpack.skip_pixels; return true;
    case 0x0CF3: *o = unpack.skip_rows; return true;
    case 0x806E: *o = unpack.image_height; return true;
    case 0x806D: *o = unpack.skip_images; return true;
    case 0x0D02: *o = pack.row_length; return true;
    case 0x0D04: *o = pack.skip_pixels; return true;
    case 0x0D03: *o = pack.skip_rows; return true;
    case 0x0D50: *o = 4; return true;
    case 0x0D52: case 0x0D53: case 0x0D54: *o = 8; return true;
    case 0x0D55: *o = attrs_.alpha ? 8 : 0; return true;
    case 0x0D56: *o = attrs_.depth ? 24 : 0; return true;
    case 0x0D57: *o = attrs_.stencil ? 8 : 0; return true;
    case 0x80A9: *o = def_color_.ss > 1 && draw_fb_ == 0 ? 4 : 0; return true;
    case 0x80A8: *o = def_color_.ss > 1 && draw_fb_ == 0 ? 1 : 0; return true;
    case 0x80AA: *o = sc_value_; return true;
    case 0x80AB: *o = sc_invert_; return true;
    case 0x8192: case 0x8B8B: *o = 0x1100; return true;
    case 0x0C02: *o = read_fb_ == 0 ? (default_read_back_ ? 0x0405 : 0) : (fb_at(read_fb_) ? fb_at(read_fb_)->read_buffer : 0); return true;
    case 0x8B9B: *o = 0x1908; return true;
    case 0x8B9A: *o = 0x1401; return true;
    case 0x8C27: *o = 0; return true;
    case 0x8C8F: *o = tf_buffer_; return true;
    case 0x8E25: *o = tf_bound_; return true;
    case 0x8DF8: case 0x8DF9: case 0x8DFA: *count = 0; return true;
    default:
      if (pname >= 0x8825 && pname < 0x8825 + kMaxDrawBuffers) {   // DRAW_BUFFERi
        const uint32_t i = pname - 0x8825;
        Framebuffer* fb = fb_at(draw_fb_);
        *o = draw_fb_ == 0 ? (i == 0 ? (default_draw_back_ ? 0x0405 : 0) : 0) : (fb ? fb->draw_buffers[i] : 0);
        return true;
      }
      set_error(INVALID_ENUM);
      return false;
  }
}

}  // namespace webgl
