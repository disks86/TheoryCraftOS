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

// Textures and samplers. Pixels arrive through GlApiPixels' converter as four floats per pixel, then are stored by the engine.
#include "OpenGl/GlApi.hpp"
#include "OpenGl/generated/GlEnums.inc"
#include "WebGl/GlCompressed.hpp"
#include "WebGl/GlPixels.hpp"

namespace opengl {

namespace {
bool is_cube_face(uint32_t t) { return t >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && t <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; }

constexpr uint32_t kMaxSize = 16384;

// The internal format a request stands for, by the source data when it does not name one precisely.
const webgl::FormatInfo* format_for(uint32_t internal) {
  const webgl::FormatInfo* f = webgl::find_format(internal);
  return f;
}

bool integer_format(uint32_t format) {
  switch (format) {
    case GL_RED_INTEGER: case GL_GREEN_INTEGER: case GL_BLUE_INTEGER: case GL_ALPHA_INTEGER: case GL_RG_INTEGER: case GL_RGB_INTEGER: case GL_BGR_INTEGER: case GL_RGBA_INTEGER: case GL_BGRA_INTEGER: return true;
    default: return false;
  }
}

// What a texel of the format holds from four source values: the components it does not have take the values GL says they read as.
void shape_for_base(uint32_t base, float v[4]) {
  switch (base) {
    case GL_RED: v[1] = v[2] = 0; v[3] = 1; break;
    case GL_RG: v[2] = 0; v[3] = 1; break;
    case GL_RGB: v[3] = 1; break;
    case GL_ALPHA: v[0] = v[1] = v[2] = 0; break;
    case GL_LUMINANCE: v[1] = v[2] = v[0]; v[3] = 1; break;
    case GL_LUMINANCE_ALPHA: v[1] = v[2] = v[0]; break;
    case GL_INTENSITY: v[1] = v[2] = v[3] = v[0]; break;
    default: break;
  }
}
}  // namespace

// --- Objects --------------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::glGenTextures(GLsizei n, UserPtr textures) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t name = group_.textures.generate();
    if (!write_values(textures.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
  }
}

void GlApi::glCreateTextures(GLenum target, GLsizei n, UserPtr textures) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t name = group_.textures.generate();
    engine_texture(name, target);
    if (!write_values(textures.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
  }
}

void GlApi::glDeleteTextures(GLsizei n, UserPtr textures) {
  if (n < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < n; ++i) {
    uint32_t name = 0;
    if (!read_values(textures.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
    if (name == 0) continue;
    const uint32_t object = group_.textures.object(name);
    if (object) {
      // Bound textures go back to the default ones.
      for (webgl::TextureUnit& u : gl_.units_) {
        if (u.tex2d == object) u.tex2d = default_textures_[1];
        if (u.tex1d == object) u.tex1d = default_textures_[0];
        if (u.tex3d == object) u.tex3d = default_textures_[2];
        if (u.cube == object) u.cube = default_textures_[3];
        if (u.rect == object) u.rect = default_textures_[4];
        if (u.array == object) u.array = default_textures_[5];
        if (u.array1d == object) u.array1d = default_textures_[6];
        if (u.cube_array == object) u.cube_array = default_textures_[7];
        if (u.buffer == object) u.buffer = default_textures_[8];
        if (u.ms2d == object) u.ms2d = default_textures_[9];
        if (u.ms2d_array == object) u.ms2d_array = default_textures_[10];
      }
      gl_.delete_object('t', object);
    }
    group_.textures.erase(name);
  }
}

uint64_t GlApi::glIsTexture(GLuint texture) { return texture != 0 && group_.textures.object(texture) != 0; }

uint32_t GlApi::default_texture_index(uint32_t target) {
  switch (target) {
    case GL_TEXTURE_1D: return 0;
    case GL_TEXTURE_2D: return 1;
    case GL_TEXTURE_3D: return 2;
    case GL_TEXTURE_CUBE_MAP: return 3;
    case GL_TEXTURE_RECTANGLE: return 4;
    case GL_TEXTURE_2D_ARRAY: return 5;
    case GL_TEXTURE_1D_ARRAY: return 6;
    case GL_TEXTURE_CUBE_MAP_ARRAY: return 7;
    case GL_TEXTURE_BUFFER: return 8;
    case GL_TEXTURE_2D_MULTISAMPLE: return 9;
    case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return 10;
    default: return 0xFFFFFFFFu;
  }
}

void GlApi::create_default_textures() {
  static constexpr uint32_t kTargets[11] = {GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_RECTANGLE, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_1D_ARRAY, GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_BUFFER, GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_2D_MULTISAMPLE_ARRAY};
  for (uint32_t i = 0; i < 11; ++i) {
    default_textures_[i] = gl_.create_texture();
    gl_.bind_texture(kTargets[i], default_textures_[i]);
  }
  for (webgl::TextureUnit& u : gl_.units_) {
    u.tex1d = default_textures_[0]; u.tex2d = default_textures_[1]; u.tex3d = default_textures_[2]; u.cube = default_textures_[3]; u.rect = default_textures_[4]; u.array = default_textures_[5];
    u.array1d = default_textures_[6]; u.cube_array = default_textures_[7]; u.buffer = default_textures_[8]; u.ms2d = default_textures_[9]; u.ms2d_array = default_textures_[10];
  }
}

uint32_t GlApi::engine_texture(uint32_t name, uint32_t target) {
  uint32_t object = group_.textures.object(name);
  if (object == 0) {
    object = gl_.create_texture();
    group_.textures.set(name, object);
    const uint32_t saved_active = gl_.active_unit_;
    // (The engine names the target of a texture when it is first bound.)
    uint32_t* slot = gl_.bound_texture_slot(target);
    if (slot) {
      const uint32_t saved = *slot;
      gl_.bind_texture(target, object);
      *slot = saved;
    }
    gl_.active_unit_ = saved_active;
  }
  return object;
}

void GlApi::glBindTexture(GLenum target, GLuint texture) {
  const uint32_t index = default_texture_index(target);
  if (index == 0xFFFFFFFFu) { error(GL_INVALID_ENUM); return; }
  if (texture == 0) {
    gl_.bind_texture(target, default_textures_[index]);
    return;
  }
  if (!group_.textures.known(texture) && config_.core_profile) { error(GL_INVALID_OPERATION); return; }
  gl_.bind_texture(target, engine_texture(texture, target));
}

void GlApi::glBindTextureUnit(GLuint unit, GLuint texture) {
  if (unit >= webgl::kMaxTextureUnits) { error(GL_INVALID_VALUE); return; }
  const uint32_t saved = gl_.active_unit_;
  gl_.active_unit_ = unit;
  if (texture == 0) {
    gl_.units_[unit].tex2d = default_textures_[1];
  } else {
    const uint32_t object = group_.textures.object(texture);
    const webgl::Texture* t = object ? gl_.texture_at(object) : nullptr;
    if (!t) { error(GL_INVALID_OPERATION); gl_.active_unit_ = saved; return; }
    gl_.bind_texture(t->target, object);
  }
  gl_.active_unit_ = saved;
}

void GlApi::glActiveTexture(GLenum texture) {
  if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + webgl::kMaxTextureUnits) { error(GL_INVALID_ENUM); return; }
  gl_.active_unit_ = texture - GL_TEXTURE0;
  ++fx_generation_;
}

void GlApi::glPrioritizeTextures(GLsizei n, UserPtr, UserPtr) { if (n < 0) error(GL_INVALID_VALUE); }

uint64_t GlApi::glAreTexturesResident(GLsizei n, UserPtr textures, UserPtr residences) {
  if (n < 0) { error(GL_INVALID_VALUE); return 0; }
  for (int32_t i = 0; i < n; ++i) {
    uint32_t name = 0;
    if (!read_values(textures.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1) || name == 0 || !group_.textures.object(name)) { error(GL_INVALID_VALUE); return 0; }
  }
  (void)residences;
  return 1;
}

// --- Images ---------------------------------------------------------------------------------------------------------------------------------------------------

webgl::Texture* GlApi::texture_for_target(uint32_t target, uint32_t* face) {
  *face = 0;
  if (is_cube_face(target)) *face = target - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
  uint32_t* slot = gl_.bound_texture_slot(target);
  if (!slot) { error(GL_INVALID_ENUM); return nullptr; }
  webgl::Texture* t = gl_.texture_at(*slot);
  if (!t) { error(GL_INVALID_OPERATION); return nullptr; }
  return t;
}

// Stores rgba (w * h * d * 4 floats, in order) at an offset in an image, the format's shape applied.
void GlApi::store_floats(webgl::Image& image, uint32_t xo, uint32_t yo, uint32_t zo, uint32_t w, uint32_t h, uint32_t d, ConstByteView rgba, bool srgb_source) {
  (void)srgb_source;
  const webgl::FormatInfo* fi = image.info;
  uint64_t at = 0;
  for (uint32_t z = 0; z < d; ++z) {
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x, at += 16) {
        float v[4];
        __builtin_memcpy(v, rgba.data() + at, 16);
        shape_for_base(fi->base, v);
        uint32_t lanes[4] = {__builtin_bit_cast(uint32_t, v[0]), __builtin_bit_cast(uint32_t, v[1]), __builtin_bit_cast(uint32_t, v[2]), __builtin_bit_cast(uint32_t, v[3])};
        uint32_t out[4];
        if (fi->store == webgl::Store::I32x4 || fi->store == webgl::Store::U32x4) {
          for (int k = 0; k < 4; ++k) lanes[k] = static_cast<uint32_t>(static_cast<int32_t>(v[k]));
          webgl::store_lanes(fi, lanes, true, false, out);
        } else {
          webgl::store_lanes(fi, lanes, false, false, out);
        }
        webgl::write_texel(image, xo + x, yo + y, zo + z, out);
      }
    }
  }
}

void GlApi::texture_image(uint32_t target, int32_t level, int32_t internal, uint32_t w, uint32_t h, uint32_t d, int32_t border, uint32_t format, uint32_t type, UserPtr pixels, uint32_t dims) {
  if (level < 0 || level >= static_cast<int32_t>(webgl::kMaxLevels)) { error(GL_INVALID_VALUE); return; }
  if (border != 0 && border != 1) { error(GL_INVALID_VALUE); return; }
  if (w > kMaxSize || h > kMaxSize || d > 2048) { error(GL_INVALID_VALUE); return; }
  uint32_t proxy_real = 0;
  const bool proxy = proxy_target(target, &proxy_real);
  if (proxy) {
    // A proxy only says whether it would fit; the level state is kept apart.
    ProxyLevel& p = proxy_levels_[proxy_index(target)];
    const bool fits = format_for(static_cast<uint32_t>(internal)) != nullptr && w <= kMaxSize && h <= kMaxSize;
    p.w = fits ? w : 0; p.h = fits ? h : 0; p.d = fits ? d : 0;
    p.internal = fits ? static_cast<uint32_t>(internal) : 0;
    return;
  }
  uint32_t face;
  webgl::Texture* t = texture_for_target(target, &face);
  if (!t) return;
  if (t->immutable) { error(GL_INVALID_OPERATION); return; }
  if (t->target == GL_TEXTURE_CUBE_MAP && (w != h || !is_cube_face(target))) { error(is_cube_face(target) ? GL_INVALID_VALUE : GL_INVALID_ENUM); return; }
  const webgl::FormatInfo* fi = format_for(static_cast<uint32_t>(internal));
  if (!fi) {
    // A compressed format named with uncompressed data, or something unknown.
    error(GL_INVALID_VALUE);
    return;
  }
  const bool int_data = integer_format(format);
  const bool int_store = fi->store == webgl::Store::I32x4 || fi->store == webgl::Store::U32x4;
  if (int_data != int_store && (format != GL_STENCIL_INDEX)) { error(GL_INVALID_OPERATION); return; }
  webgl::Image* im = gl_.make_texture_image(t, target, level);
  if (!im) { error(GL_OUT_OF_MEMORY); return; }
  if (w == 0 || h == 0 || d == 0) { im->info = nullptr; im->w = im->h = im->d = 0; return; }
  if (!im->allocate(fi, w, h, d)) { error(GL_OUT_OF_MEMORY); return; }
  im->compressed = 0;
  im->requested_internal = static_cast<uint32_t>(internal);
  ++gl_.version_;
  (void)dims;
  if (pixels.null() && gl_.pixel_unpack_ == 0) return;
  ByteBuffer data(a_);
  if (!pixels_in(w, h, d, format, type, pixels, &data, unpack_)) return;
  if (!int_data && format != GL_DEPTH_COMPONENT && format != GL_DEPTH_STENCIL && format != GL_STENCIL_INDEX) {
    pixels_transfer(float_view(data));
  }
  store_floats(*im, 0, 0, 0, w, h, d, data.view(), false);
  if (t->generate_mipmap_auto && level == t->base_level) { uint32_t real = t->target; gl_.generate_mipmap(real); }
}

void GlApi::texture_sub_image(uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t zo, uint32_t w, uint32_t h, uint32_t d, uint32_t format, uint32_t type, UserPtr pixels) {
  uint32_t face;
  webgl::Texture* t = texture_for_target(target, &face);
  if (!t) return;
  webgl::Image* im = gl_.texture_image(t, target, level);
  if (!im || !im->defined()) { error(GL_INVALID_OPERATION); return; }
  if (xo < 0 || yo < 0 || zo < 0 || uint64_t(xo) + w > im->w || uint64_t(yo) + h > im->h || uint64_t(zo) + d > im->d) { error(GL_INVALID_VALUE); return; }
  if (w == 0 || h == 0 || d == 0) return;
  ByteBuffer data(a_);
  if (!pixels_in(w, h, d, format, type, pixels, &data, unpack_)) return;
  if (pixels.null() && gl_.pixel_unpack_ == 0) return;
  if (!integer_format(format) && format != GL_DEPTH_COMPONENT && format != GL_DEPTH_STENCIL && format != GL_STENCIL_INDEX) pixels_transfer(float_view(data));
  store_floats(*im, static_cast<uint32_t>(xo), static_cast<uint32_t>(yo), static_cast<uint32_t>(zo), w, h, d, data.view(), false);
  if (t->generate_mipmap_auto && level == t->base_level) gl_.generate_mipmap(t->target);
  ++gl_.version_;
}

void GlApi::glTexImage1D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border, GLenum format, GLenum type, UserPtr pixels) {
  if (width < 0) { error(GL_INVALID_VALUE); return; }
  texture_image(target, level, internalformat, static_cast<uint32_t>(width), 1, 1, border, format, type, pixels, 1);
}
void GlApi::glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, UserPtr pixels) {
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  texture_image(target, level, internalformat, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1, border, format, type, pixels, 2);
}
void GlApi::glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, UserPtr pixels) {
  if (width < 0 || height < 0 || depth < 0) { error(GL_INVALID_VALUE); return; }
  texture_image(target, level, internalformat, static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(depth), border, format, type, pixels, 3);
}
void GlApi::glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, UserPtr pixels) {
  if (width < 0) { error(GL_INVALID_VALUE); return; }
  texture_sub_image(target, level, xoffset, 0, 0, static_cast<uint32_t>(width), 1, 1, format, type, pixels);
}
void GlApi::glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, UserPtr pixels) {
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  texture_sub_image(target, level, xoffset, yoffset, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1, format, type, pixels);
}
void GlApi::glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, UserPtr pixels) {
  if (width < 0 || height < 0 || depth < 0) { error(GL_INVALID_VALUE); return; }
  texture_sub_image(target, level, xoffset, yoffset, zoffset, static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(depth), format, type, pixels);
}

void GlApi::texture_storage(uint32_t target, uint32_t levels, uint32_t internal, uint32_t w, uint32_t h, uint32_t d) {
  if (levels == 0 || w == 0 || h == 0 || d == 0) { error(GL_INVALID_VALUE); return; }
  uint32_t face;
  webgl::Texture* t = texture_for_target(target, &face);
  if (!t) return;
  if (t->immutable) { error(GL_INVALID_OPERATION); return; }
  const webgl::FormatInfo* fi = format_for(internal);
  if (!fi) { error(GL_INVALID_ENUM); return; }
  const bool cube = t->target == GL_TEXTURE_CUBE_MAP;
  const bool layered = t->target == GL_TEXTURE_2D_ARRAY || t->target == GL_TEXTURE_CUBE_MAP_ARRAY;
  const bool array1 = t->target == GL_TEXTURE_1D_ARRAY;
  for (uint32_t lv = 0; lv < levels && lv < webgl::kMaxLevels; ++lv) {
    const uint32_t lw = w >> lv ? w >> lv : 1;
    const uint32_t lh = array1 ? h : (h >> lv ? h >> lv : 1);
    const uint32_t ld = layered || array1 ? d : (d >> lv ? d >> lv : 1);
    for (uint32_t f = 0; f < (cube ? 6u : 1u); ++f) {
      webgl::Image* im = gl_.make_texture_image(t, cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : t->target, static_cast<int32_t>(lv));
      if (!im || !im->allocate(fi, lw, lh, ld)) { error(GL_OUT_OF_MEMORY); return; }
      im->requested_internal = internal;
    }
  }
  t->immutable = true;
  t->immutable_levels = levels;
  ++gl_.version_;
}

void GlApi::glTexStorage1D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width) {
  if (levels < 1 || width < 1) { error(GL_INVALID_VALUE); return; }
  texture_storage(target, static_cast<uint32_t>(levels), internalformat, static_cast<uint32_t>(width), 1, 1);
}
void GlApi::glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
  if (levels < 1 || width < 1 || height < 1) { error(GL_INVALID_VALUE); return; }
  texture_storage(target, static_cast<uint32_t>(levels), internalformat, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1);
}
void GlApi::glTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth) {
  if (levels < 1 || width < 1 || height < 1 || depth < 1) { error(GL_INVALID_VALUE); return; }
  texture_storage(target, static_cast<uint32_t>(levels), internalformat, static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(depth));
}

void GlApi::glCopyTexImage1D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLint border) {
  if (width < 0) { error(GL_INVALID_VALUE); return; }
  copy_image(target, level, internalformat, x, y, static_cast<uint32_t>(width), 1, border);
}
void GlApi::glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border) {
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  copy_image(target, level, internalformat, x, y, static_cast<uint32_t>(width), static_cast<uint32_t>(height), border);
}

void GlApi::copy_image(uint32_t target, int32_t level, uint32_t internal, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t border) {
  if (level < 0 || level >= static_cast<int32_t>(webgl::kMaxLevels) || (border != 0 && border != 1)) { error(GL_INVALID_VALUE); return; }
  uint32_t face;
  webgl::Texture* t = texture_for_target(target, &face);
  if (!t) return;
  const webgl::FormatInfo* fi = format_for(internal);
  if (!fi) { error(GL_INVALID_VALUE); return; }
  gl_.hw_sync();
  ByteBuffer data(a_);
  const bool ds = fi->store == webgl::Store::Depth || fi->store == webgl::Store::DepthStencil;
  if (!read_rectangle(x, y, w, h, ds, &data)) { error(GL_INVALID_OPERATION); return; }
  webgl::Image* im = gl_.make_texture_image(t, target, level);
  if (!im || !im->allocate(fi, w, h, 1)) { error(GL_OUT_OF_MEMORY); return; }
  im->requested_internal = internal;
  store_floats(*im, 0, 0, 0, w, h, 1, data.view(), false);
  ++gl_.version_;
}

void GlApi::copy_sub_image(uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t zo, int32_t x, int32_t y, uint32_t w, uint32_t h) {
  uint32_t face;
  webgl::Texture* t = texture_for_target(target, &face);
  if (!t) return;
  webgl::Image* im = gl_.texture_image(t, target, level);
  if (!im || !im->defined()) { error(GL_INVALID_OPERATION); return; }
  if (xo < 0 || yo < 0 || zo < 0 || uint64_t(xo) + w > im->w || uint64_t(yo) + h > im->h || uint64_t(zo) >= im->d) { error(GL_INVALID_VALUE); return; }
  if (w == 0 || h == 0) return;
  gl_.hw_sync();
  ByteBuffer data(a_);
  const bool ds = im->info->store == webgl::Store::Depth || im->info->store == webgl::Store::DepthStencil;
  if (!read_rectangle(x, y, w, h, ds, &data)) { error(GL_INVALID_OPERATION); return; }
  store_floats(*im, static_cast<uint32_t>(xo), static_cast<uint32_t>(yo), static_cast<uint32_t>(zo), w, h, 1, data.view(), false);
  ++gl_.version_;
}

void GlApi::glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width) {
  if (width < 0) { error(GL_INVALID_VALUE); return; }
  copy_sub_image(target, level, xoffset, 0, 0, x, y, static_cast<uint32_t>(width), 1);
}
void GlApi::glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) {
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  copy_sub_image(target, level, xoffset, yoffset, 0, x, y, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}
void GlApi::glCopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height) {
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  copy_sub_image(target, level, xoffset, yoffset, zoffset, x, y, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

void GlApi::glGenerateMipmap(GLenum target) {
  uint32_t face;
  webgl::Texture* t = texture_for_target(target, &face);
  if (!t) return;
  gl_.generate_mipmap(target);
}

void GlApi::glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, UserPtr pixels) {
  uint32_t face;
  webgl::Texture* t = texture_for_target(target, &face);
  if (!t) return;
  const webgl::Image* im = gl_.texture_image(t, target, level);
  if (!im || !im->defined()) { error(GL_INVALID_OPERATION); return; }
  const PixelStore saved = pack_;
  // (Every slice in a row: a layered image is written as image_height rows per slice.)
  const uint64_t count = uint64_t{im->w} * im->h * im->d;
  ByteBuffer data(a_);
  if (!data.put_zeros(count * 16)) { error(GL_OUT_OF_MEMORY); return; }
  ByteView out = data.writable_view();
  uint64_t at = 0;
  for (uint32_t z = 0; z < im->d; ++z) {
    for (uint32_t y = 0; y < im->h; ++y) {
      for (uint32_t x = 0; x < im->w; ++x, at += 16) {
        uint32_t raw[4];
        webgl::read_texel(*im, x, y, z, raw);
        float v[4];
        switch (im->info->store) {
          case webgl::Store::U8x4:
            for (int k = 0; k < 4; ++k) v[k] = static_cast<float>(raw[k]) / 255.0f;
            break;
          case webgl::Store::F32x4: for (int k = 0; k < 4; ++k) v[k] = __builtin_bit_cast(float, raw[k]); break;
          case webgl::Store::Depth: v[0] = __builtin_bit_cast(float, raw[0]); v[1] = v[2] = 0; v[3] = 1; break;
          case webgl::Store::DepthStencil: v[0] = __builtin_bit_cast(float, raw[0]); v[1] = static_cast<float>(raw[1]); v[2] = 0; v[3] = 1; break;
          case webgl::Store::I32x4: for (int k = 0; k < 4; ++k) v[k] = static_cast<float>(static_cast<int32_t>(raw[k])); break;
          default: for (int k = 0; k < 4; ++k) v[k] = static_cast<float>(raw[k]); break;
        }
        __builtin_memcpy(out.data() + at, v, 16);
      }
    }
  }
  pixels_out(im->w, im->h * im->d, format, type, data.view(), pixels);
  pack_ = saved;
}

// --- Parameters -----------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::tex_parameter_values(uint32_t target, uint32_t pname, const double* v, uint32_t) {
  uint32_t face;
  webgl::Texture* t = texture_for_target(target, &face);
  if (!t) return;
  switch (pname) {
    case GL_TEXTURE_BORDER_COLOR: for (int i = 0; i < 4; ++i) t->border_color[i] = static_cast<float>(v[i]); ++gl_.version_; return;
    case GL_GENERATE_MIPMAP: t->generate_mipmap_auto = v[0] != 0; return;
    case GL_TEXTURE_PRIORITY: case GL_TEXTURE_LOD_BIAS: t->lod_bias = pname == GL_TEXTURE_LOD_BIAS ? static_cast<float>(v[0]) : t->lod_bias; return;
    case GL_DEPTH_TEXTURE_MODE: t->depth_mode = static_cast<uint32_t>(v[0]); ++fx_generation_; return;
    case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A: t->swizzle[pname - GL_TEXTURE_SWIZZLE_R] = static_cast<uint32_t>(v[0]); ++gl_.version_; return;
    case GL_TEXTURE_SWIZZLE_RGBA: for (int i = 0; i < 4; ++i) t->swizzle[i] = static_cast<uint32_t>(v[i]); ++gl_.version_; return;
    case GL_DEPTH_STENCIL_TEXTURE_MODE: t->stencil_mode = static_cast<uint32_t>(v[0]) == GL_STENCIL_INDEX; ++gl_.version_; return;
    default: break;
  }
  gl_.tex_parameter(target, pname, v[0]);
  ++fx_generation_;
}

uint32_t GlApi::tex_param_count(uint32_t pname) { return pname == GL_TEXTURE_BORDER_COLOR || pname == GL_TEXTURE_SWIZZLE_RGBA ? 4 : 1; }

void GlApi::glTexParameterf(GLenum target, GLenum pname, GLfloat param) { const double v = param; tex_parameter_values(target, pname, &v, 1); }
void GlApi::glTexParameteri(GLenum target, GLenum pname, GLint param) { const double v = param; tex_parameter_values(target, pname, &v, 1); }
void GlApi::glTexParameterfv(GLenum target, GLenum pname, UserPtr params) {
  float f[4];
  const uint32_t n = tex_param_count(pname);
  if (!read_values(params, f, n)) { error(GL_INVALID_VALUE); return; }
  double v[4];
  for (uint32_t i = 0; i < n; ++i) v[i] = f[i];
  tex_parameter_values(target, pname, v, n);
}
void GlApi::glTexParameteriv(GLenum target, GLenum pname, UserPtr params) {
  int32_t f[4];
  const uint32_t n = tex_param_count(pname);
  if (!read_values(params, f, n)) { error(GL_INVALID_VALUE); return; }
  double v[4];
  for (uint32_t i = 0; i < n; ++i) v[i] = pname == GL_TEXTURE_BORDER_COLOR ? (2.0 * f[i] + 1.0) / 4294967295.0 : f[i];
  tex_parameter_values(target, pname, v, n);
}

bool GlApi::tex_parameter_get(uint32_t target, uint32_t pname, double* out, uint32_t* count) {
  uint32_t face;
  webgl::Texture* t = texture_for_target(target, &face);
  if (!t) return false;
  *count = 1;
  switch (pname) {
    case GL_TEXTURE_BORDER_COLOR: for (int i = 0; i < 4; ++i) out[i] = t->border_color[i]; *count = 4; return true;
    case GL_GENERATE_MIPMAP: *out = t->generate_mipmap_auto; return true;
    case GL_TEXTURE_PRIORITY: *out = 1; return true;
    case GL_TEXTURE_RESIDENT: *out = 1; return true;
    case GL_TEXTURE_LOD_BIAS: *out = t->lod_bias; return true;
    case GL_DEPTH_TEXTURE_MODE: *out = t->depth_mode; return true;
    case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A: *out = t->swizzle[pname - GL_TEXTURE_SWIZZLE_R]; return true;
    case GL_TEXTURE_SWIZZLE_RGBA: for (int i = 0; i < 4; ++i) out[i] = t->swizzle[i]; *count = 4; return true;
    case GL_DEPTH_STENCIL_TEXTURE_MODE: *out = t->stencil_mode ? GL_STENCIL_INDEX : GL_DEPTH_COMPONENT; return true;
    case GL_TEXTURE_IMMUTABLE_FORMAT: *out = t->immutable; return true;
    case GL_TEXTURE_IMMUTABLE_LEVELS: *out = t->immutable_levels; return true;
    case GL_TEXTURE_MAX_ANISOTROPY: *out = 1; return true;
    default: break;
  }
  return gl_.get_tex_parameter(target, pname, out);
}

void GlApi::glGetTexParameterfv(GLenum target, GLenum pname, UserPtr params) {
  double v[4];
  uint32_t n = 1;
  if (!tex_parameter_get(target, pname, v, &n)) return;
  float f[4];
  for (uint32_t i = 0; i < n; ++i) f[i] = static_cast<float>(v[i]);
  write_values(params, f, n);
}
void GlApi::glGetTexParameteriv(GLenum target, GLenum pname, UserPtr params) {
  double v[4];
  uint32_t n = 1;
  if (!tex_parameter_get(target, pname, v, &n)) return;
  int32_t f[4];
  for (uint32_t i = 0; i < n; ++i) f[i] = pname == GL_TEXTURE_BORDER_COLOR ? static_cast<int32_t>(v[i] * 2147483647.0) : static_cast<int32_t>(v[i]);
  write_values(params, f, n);
}

bool GlApi::proxy_target(uint32_t target, uint32_t* real) {
  switch (target) {
    case GL_PROXY_TEXTURE_1D: *real = GL_TEXTURE_1D; return true;
    case GL_PROXY_TEXTURE_2D: *real = GL_TEXTURE_2D; return true;
    case GL_PROXY_TEXTURE_3D: *real = GL_TEXTURE_3D; return true;
    case GL_PROXY_TEXTURE_CUBE_MAP: *real = GL_TEXTURE_CUBE_MAP; return true;
    case GL_PROXY_TEXTURE_RECTANGLE: *real = GL_TEXTURE_RECTANGLE; return true;
    case GL_PROXY_TEXTURE_1D_ARRAY: *real = GL_TEXTURE_1D_ARRAY; return true;
    case GL_PROXY_TEXTURE_2D_ARRAY: *real = GL_TEXTURE_2D_ARRAY; return true;
    default: return false;
  }
}

uint32_t GlApi::proxy_index(uint32_t target) {
  switch (target) {
    case GL_PROXY_TEXTURE_1D: return 0;
    case GL_PROXY_TEXTURE_2D: return 1;
    case GL_PROXY_TEXTURE_3D: return 2;
    case GL_PROXY_TEXTURE_CUBE_MAP: return 3;
    case GL_PROXY_TEXTURE_RECTANGLE: return 4;
    case GL_PROXY_TEXTURE_1D_ARRAY: return 5;
    default: return 6;
  }
}

bool GlApi::tex_level_get(uint32_t target, int32_t level, uint32_t pname, double* out) {
  uint32_t real = 0;
  uint32_t w = 0, h = 0, d = 0, internal = 0;
  const webgl::FormatInfo* fi = nullptr;
  bool compressed = false;
  if (proxy_target(target, &real)) {
    const ProxyLevel& p = proxy_levels_[proxy_index(target)];
    w = p.w; h = p.h; d = p.d; internal = p.internal;
    fi = internal ? format_for(internal) : nullptr;
  } else {
    uint32_t face;
    webgl::Texture* t = texture_for_target(target, &face);
    if (!t) return false;
    if (level < 0 || level >= static_cast<int32_t>(webgl::kMaxLevels)) { error(GL_INVALID_VALUE); return false; }
    const webgl::Image* im = gl_.texture_image(t, target, level);
    if (im && im->defined()) {
      w = im->w; h = im->h; d = im->d;
      fi = im->info;
      internal = im->requested_internal ? im->requested_internal : (im->compressed ? im->compressed : im->info->internal);
      compressed = im->compressed != 0;
    }
  }
  const auto red_like = [&](uint32_t want_bits, bool has) { return has ? want_bits : 0u; };
  const uint32_t base = fi ? fi->base : 0;
  const uint32_t bits = !fi ? 0 : fi->store == webgl::Store::U8x4 ? 8 : (fi->is_float ? 32 : 16);
  const bool has_red = base == GL_RED || base == GL_RG || base == GL_RGB || base == GL_RGBA, has_green = base == GL_RG || base == GL_RGB || base == GL_RGBA, has_blue = base == GL_RGB || base == GL_RGBA,
             has_alpha = base == GL_RGBA || base == GL_ALPHA || base == GL_LUMINANCE_ALPHA;
  switch (pname) {
    case GL_TEXTURE_WIDTH: *out = w; return true;
    case GL_TEXTURE_HEIGHT: *out = h; return true;
    case GL_TEXTURE_DEPTH: *out = d; return true;
    case GL_TEXTURE_INTERNAL_FORMAT: *out = internal ? internal : 1; return true;
    case GL_TEXTURE_BORDER: *out = 0; return true;
    case GL_TEXTURE_RED_SIZE: *out = red_like(bits, has_red); return true;
    case GL_TEXTURE_GREEN_SIZE: *out = red_like(bits, has_green); return true;
    case GL_TEXTURE_BLUE_SIZE: *out = red_like(bits, has_blue); return true;
    case GL_TEXTURE_ALPHA_SIZE: *out = red_like(bits, has_alpha); return true;
    case GL_TEXTURE_LUMINANCE_SIZE: *out = red_like(bits, base == GL_LUMINANCE || base == GL_LUMINANCE_ALPHA); return true;
    case GL_TEXTURE_INTENSITY_SIZE: *out = red_like(bits, base == GL_INTENSITY); return true;
    case GL_TEXTURE_DEPTH_SIZE: *out = base == GL_DEPTH_COMPONENT || base == GL_DEPTH_STENCIL ? 24 : 0; return true;
    case GL_TEXTURE_STENCIL_SIZE: *out = base == GL_DEPTH_STENCIL ? 8 : 0; return true;
    case GL_TEXTURE_COMPRESSED: *out = compressed; return true;
    case GL_TEXTURE_RED_TYPE: case GL_TEXTURE_GREEN_TYPE: case GL_TEXTURE_BLUE_TYPE: case GL_TEXTURE_ALPHA_TYPE: case GL_TEXTURE_LUMINANCE_TYPE: case GL_TEXTURE_INTENSITY_TYPE: case GL_TEXTURE_DEPTH_TYPE:
      *out = !fi ? GL_NONE : fi->store == webgl::Store::I32x4 ? GL_INT : fi->store == webgl::Store::U32x4 ? GL_UNSIGNED_INT : fi->is_float ? GL_FLOAT : fi->is_signed ? GL_SIGNED_NORMALIZED : GL_UNSIGNED_NORMALIZED;
      return true;
    case GL_TEXTURE_SAMPLES: *out = 0; return true;
    case GL_TEXTURE_FIXED_SAMPLE_LOCATIONS: *out = 1; return true;
    case GL_TEXTURE_BUFFER_OFFSET: case GL_TEXTURE_BUFFER_SIZE: case GL_TEXTURE_BUFFER_DATA_STORE_BINDING: *out = 0; return true;
    default: error(GL_INVALID_ENUM); return false;
  }
}

void GlApi::glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, UserPtr params) {
  double v = 0;
  if (!tex_level_get(target, level, pname, &v)) return;
  const float f = static_cast<float>(v);
  write_values(params, &f, 1);
}
void GlApi::glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, UserPtr params) {
  double v = 0;
  if (!tex_level_get(target, level, pname, &v)) return;
  const int32_t i = static_cast<int32_t>(v);
  write_values(params, &i, 1);
}

// --- Compressed images ------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::compressed_image(uint32_t target, int32_t level, uint32_t internal, uint32_t w, uint32_t h, uint32_t d, int32_t border, int32_t size, UserPtr data, bool three_d) {
  if (size < 0 || border != 0) { error(GL_INVALID_VALUE); return; }
  ByteBuffer bytes(a_);
  const webgl::Buffer* pbo = gl_.buffer_at(gl_.pixel_unpack_);
  ConstByteView view;
  if (pbo) {
    if (!pbo->data.view().try_subview(data.address, static_cast<uint64_t>(size), &view)) { error(GL_INVALID_OPERATION); return; }
  } else {
    if (!bytes.put_zeros(static_cast<uint64_t>(size)) || !read_client(data, bytes.writable_view())) { error(GL_INVALID_OPERATION); return; }
    view = bytes.view();
  }
  const uint32_t saved_unpack = gl_.pixel_unpack_;
  gl_.pixel_unpack_ = 0;
  gl_.compressed_tex_image(target, level, internal, w, h, d, border, view, three_d);
  gl_.pixel_unpack_ = saved_unpack;
}

void GlApi::glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, UserPtr data) {
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  compressed_image(target, level, internalformat, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1, border, imageSize, data, false);
}
void GlApi::glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, UserPtr data) {
  if (width < 0 || height < 0 || depth < 0) { error(GL_INVALID_VALUE); return; }
  compressed_image(target, level, internalformat, static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(depth), border, imageSize, data, true);
}
void GlApi::glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, UserPtr data) {
  if (width < 0 || height < 0 || imageSize < 0) { error(GL_INVALID_VALUE); return; }
  ByteBuffer bytes(a_);
  if (!bytes.put_zeros(static_cast<uint64_t>(imageSize)) || !read_client(data, bytes.writable_view())) { error(GL_INVALID_OPERATION); return; }
  gl_.compressed_tex_sub_image(target, level, xoffset, yoffset, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1, format, bytes.view(), false);
}
void GlApi::glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, UserPtr data) {
  if (width < 0 || height < 0 || depth < 0 || imageSize < 0) { error(GL_INVALID_VALUE); return; }
  ByteBuffer bytes(a_);
  if (!bytes.put_zeros(static_cast<uint64_t>(imageSize)) || !read_client(data, bytes.writable_view())) { error(GL_INVALID_OPERATION); return; }
  gl_.compressed_tex_sub_image(target, level, xoffset, yoffset, zoffset, static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(depth), format, bytes.view(), true);
}

// --- Samplers ---------------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::glGenSamplers(GLsizei count, UserPtr samplers) {
  if (count < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < count; ++i) {
    const uint32_t name = group_.samplers.generate();
    group_.samplers.set(name, gl_.create_sampler());
    if (!write_values(samplers.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
  }
}
void GlApi::glCreateSamplers(GLsizei n, UserPtr samplers) { glGenSamplers(n, samplers); }
void GlApi::glDeleteSamplers(GLsizei count, UserPtr samplers) {
  if (count < 0) { error(GL_INVALID_VALUE); return; }
  for (int32_t i = 0; i < count; ++i) {
    uint32_t name = 0;
    if (!read_values(samplers.plus(uint64_t{static_cast<uint32_t>(i)} * 4), &name, 1)) { error(GL_INVALID_VALUE); return; }
    if (!name) continue;
    const uint32_t object = group_.samplers.object(name);
    if (object) {
      for (webgl::TextureUnit& u : gl_.units_) if (u.sampler == object) u.sampler = 0;
      gl_.delete_object('a', object);
    }
    group_.samplers.erase(name);
  }
}
uint64_t GlApi::glIsSampler(GLuint sampler) { return sampler && group_.samplers.object(sampler) != 0; }
void GlApi::glBindSampler(GLuint unit, GLuint sampler) {
  if (unit >= webgl::kMaxTextureUnits) { error(GL_INVALID_VALUE); return; }
  if (sampler != 0 && !group_.samplers.object(sampler)) { error(GL_INVALID_OPERATION); return; }
  gl_.units_[unit].sampler = sampler ? group_.samplers.object(sampler) : 0;
}
void GlApi::glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param) { gl_.sampler_parameter(group_.samplers.object(sampler), pname, param); }
void GlApi::glSamplerParameteri(GLuint sampler, GLenum pname, GLint param) { gl_.sampler_parameter(group_.samplers.object(sampler), pname, param); }
void GlApi::glSamplerParameterfv(GLuint sampler, GLenum pname, UserPtr param) {
  float v[4];
  if (!read_values(param, v, tex_param_count(pname))) { error(GL_INVALID_VALUE); return; }
  if (pname == GL_TEXTURE_BORDER_COLOR) {
    webgl::SamplerObj* so = gl_.sampler_at(group_.samplers.object(sampler));
    if (so) for (int i = 0; i < 4; ++i) so->border[i] = v[i];
    return;
  }
  gl_.sampler_parameter(group_.samplers.object(sampler), pname, v[0]);
}
void GlApi::glSamplerParameteriv(GLuint sampler, GLenum pname, UserPtr param) {
  int32_t v[4];
  if (!read_values(param, v, tex_param_count(pname))) { error(GL_INVALID_VALUE); return; }
  if (pname == GL_TEXTURE_BORDER_COLOR) {
    webgl::SamplerObj* so = gl_.sampler_at(group_.samplers.object(sampler));
    if (so) for (int i = 0; i < 4; ++i) so->border[i] = static_cast<float>((2.0 * v[i] + 1.0) / 4294967295.0);
    return;
  }
  gl_.sampler_parameter(group_.samplers.object(sampler), pname, v[0]);
}

ArrayView<float> GlApi::float_view(ByteBuffer& data) {
  // (A buffer of 32-bit floats written and read only as such; the allocator's alignment is at least 16.)
  return ArrayView<float>(reinterpret_cast<float*>(data.writable_view().data()), data.size() / 4);
}

}  // namespace opengl
