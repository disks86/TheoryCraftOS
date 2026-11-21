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

// The default framebuffer of a GL context drawn by the 3D hardware (see Accelerator.hpp). The device picture and def_color_ are kept in step like this:
//  - a clear that covers the whole drawing buffer clears both, so they agree (hw_valid_) and the device can take the draws that follow;
//  - an accelerated draw changes only the device picture (hw_ahead_);
//  - everything that reads def_color_ calls hw_sync() first, which copies the device picture over it;
//  - everything on the CPU that writes def_color_ calls hw_cpu_write(): sync, and from then on the device picture is out of date (a device image cannot be written
//    from the CPU), so the draws that follow run on the CPU until the next whole-buffer clear.
// The device depth buffer cannot be read back either, so def_depth_ is only what the CPU draws and clears put there; after a mix of both in one frame the depth
// test of the CPU draws knows nothing of the hardware draws that came before them.

#include "WebGl/GlContext.hpp"
#include "WebGl/GlAssemble.hpp"

namespace webgl {

namespace {
// GL's blend factors and equations as the device's (graphics::BlendFactor / BlendOp numbers); a constant colour has no device factor
bool factor(uint32_t f, uint8_t* out) {
  switch (f) {
    case 0: *out = 0; return true;
    case 1: *out = 1; return true;
    case 0x0300: *out = 2; return true;
    case 0x0301: *out = 3; return true;
    case 0x0302: *out = 4; return true;
    case 0x0303: *out = 5; return true;
    case 0x0306: *out = 6; return true;
    case 0x0307: *out = 7; return true;
    case 0x0304: *out = 8; return true;
    case 0x0305: *out = 9; return true;
    case 0x0308: *out = 10; return true;
    default: return false;
  }
}
bool equation(uint32_t e, uint8_t* out) {
  switch (e) {
    case 0x8006: *out = 0; return true;
    case 0x800A: *out = 1; return true;
    case 0x800B: *out = 2; return true;
    case 0x8007: *out = 3; return true;
    case 0x8008: *out = 4; return true;
    default: return false;
  }
}
inline float u2f(uint32_t u) { return __builtin_bit_cast(float, u); }
inline uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }

// The device keeps integers as float values (TgsiTranslator.cpp): every integer of magnitude up to 2^24 is exact. A uniform or input beyond that would not compare or
// wrap as GLSL says, so such a draw is the CPU's.
constexpr uint32_t kExactInt = 1u << 24;
inline bool exact_int(int32_t v) { return v >= -static_cast<int32_t>(kExactInt) && v <= static_cast<int32_t>(kExactInt); }
constexpr uint32_t kMaxAccelVertices = 3u << 20;   // vertices of one draw the accelerated path assembles (more run on the CPU)
}  // namespace

void GlContext::hw_forget_texture(uint32_t name) {
  if (hw_owner_) hw_textures_.forget(*hw_owner_, name);
}

// --- Device copies of images ---------------------------------------------------------------------------------------------------------------------------------

// The kind of device image an image's pixels go to: 8-bit RGBA (sRGB encoded or not), floats, or depth. False: the device has no such image (integer and signed
// normalised formats).
bool GlContext::hw_format(const Image& im, AccelFormat* format, const char** why) const {
  const IAccelerator* acc = accelerator();
  const AccelLimits l = acc ? acc->limits() : AccelLimits();
  switch (im.info->store) {
    case Store::U8x4:
      if (im.info->is_signed) break;
      if (im.info->srgb) {
        if (!l.srgb_images) break;
        *format = AccelFormat::Rgba8Srgb;
        return true;
      }
      *format = AccelFormat::Rgba8;
      return true;
    case Store::F32x4:
      if (!l.float_images) break;
      *format = AccelFormat::Rgba32F;
      return true;
    case Store::Depth:
    case Store::DepthStencil:
      *format = AccelFormat::Depth;
      return true;
    default: break;
  }
  *why = "an image format the device does not have";
  return false;
}

// The device copy of `im` as a `format` image: the one it has, or a new one (which holds nothing yet: hw_current fills it).
AccelImage* GlContext::hw_image(Image& im, AccelFormat format, const char** why) {
  IAccelerator* acc = accelerator();
  if (!acc) {
    *why = "no accelerator";
    return nullptr;
  }
  if (im.hw && im.hw->owner == acc && im.hw->format == format && im.hw->w == im.w && im.hw->h == im.h && im.hw->levels == 1) return im.hw.get();
  if (im.hw && im.hw->ahead) hw_sync_image(im);      // (a copy of another kind that is newer: its pixels come back before it goes)
  im.hw.reset();
  if (im.d != 1 || im.ss != 1) {
    *why = "a layered or multisampled image";
    return nullptr;
  }
  if (im.w > 8192 || im.h > 8192) {
    *why = "an image larger than the device takes";
    return nullptr;
  }
  const uint32_t handle = acc->create_image(im.w, im.h, format, 1);
  if (!handle) {
    *why = "the device refused an image";
    return nullptr;
  }
  im.hw = UniquePointer<AccelImage>::make(a_);
  if (!im.hw) {
    acc->destroy_texture(handle);
    *why = "out of memory";
    return nullptr;
  }
  im.hw->owner = acc;
  im.hw->handle = handle;
  im.hw->format = format;
  im.hw->w = im.w;
  im.hw->h = im.h;
  im.hw->ahead_count = &objects_->hw_ahead;
  return im.hw.get();
}

// Brings the device copy up to date with the CPU's pixels if they are newer (a colour image: written up; a depth image cannot be, and is then out of date).
bool GlContext::hw_current(Image& im, AccelImage& copy) {
  if (copy.ahead) {
    if (!copy.cpu_seen || copy.cpu_seen == im.version()) return true;
    copy.set_ahead(false);      // (the CPU wrote the depth after it was handed its older one: neither copy is whole, the device's is out of date)
    copy.cpu_seen = 0;
    copy.synced = 0;
    return false;
  }
  const uint64_t version = im.version();
  if (copy.synced == version) return true;
  if (copy.format == AccelFormat::Depth) return false;
  const uint64_t bytes = uint64_t{im.w} * im.h * texel_bytes(im.info->store);
  if (im.px.size() != bytes || !copy.owner->write_image(copy.handle, 0, im.px.view())) return false;
  copy.synced = version;
  copy.cleared = false;
  return true;
}

// The device copy is newer than the pixels: they come back. A depth copy cannot be read; if a clear is all the device did to it since, the CPU does the same clear,
// else the depth the device drew is lost to the CPU (counted in the draw statistics) and both copies go on from the CPU's.
void GlContext::hw_sync_image(Image& im) {
  if (!im.hw || !im.hw->ahead) return;
  AccelImage& copy = *im.hw;
  if (copy.format == AccelFormat::Depth && !copy.cleared) {
    // (the device drew depth the CPU cannot have: the CPU goes on with what it has, the device keeps its own until the CPU writes the image - hw_current)
    if (!copy.cpu_seen) hw_stats_.note("depth the device drew that the CPU then needed");
    copy.cpu_seen = im.version();
    return;
  }
  copy.set_ahead(false);
  if (copy.format == AccelFormat::Depth) {
    if (copy.cleared) {
      const uint32_t depth = f2u(copy.cleared_value);
      for (uint32_t y = 0; y < im.h; ++y)
        for (uint32_t x = 0; x < im.w; ++x) {
          uint32_t raw[4];
          read_texel(im, x, y, 0, raw);
          raw[0] = depth;
          write_texel(im, x, y, 0, raw);
        }
      copy.synced = im.version();
      copy.cleared = false;
    }
    return;
  }
  ByteView dst = im.px.writable_view();
  im.touched();
  if (dst.size() == uint64_t{im.w} * im.h * texel_bytes(im.info->store) && copy.owner->read_image(copy.handle, dst)) {
    copy.synced = im.version();
    copy.cleared = false;
  } else {
    hw_stats_.note("an image the device would not give back");
    copy.synced = 0;
  }
}

void GlContext::hw_sync_texture(Texture& t) {
  if (objects_->hw_ahead <= 0) return;
  for (const UniquePointer<Image>& im : t.images)
    if (im) hw_sync_image(*im);
}

void GlContext::hw_sync_images() {
  if (objects_->hw_ahead <= 0) return;
  for (const UniquePointer<Texture>& t : objects_->textures)
    if (t) hw_sync_texture(*t);
  for (const UniquePointer<Renderbuffer>& rb : objects_->rbos)
    if (rb) hw_sync_image(rb->image);
}

void GlContext::hw_sync_target(const Target& t) {
  if (objects_->hw_ahead <= 0 || t.is_default) return;
  for (Image* im : t.color)
    if (im) hw_sync_image(*im);
  if (t.depth) hw_sync_image(*t.depth);
  if (t.stencil && t.stencil != t.depth) hw_sync_image(*t.stencil);
}

// The colour images of a framebuffer object as the device draws into them (fragment output n to image n, which hw_program's translation gives each draw buffer):
// draw buffers 0 .. n-1 with an image each, none left out between, all 2D images (or one face of a cube) of one size. `masks`: the channels the device may write of
// each (an 8-bit image without alpha keeps its alpha at 1).
bool GlContext::hw_fbo_colors(const Target& t, uint32_t* count, const char** why) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < kMaxDrawBuffers; ++i)
    if (t.color[i]) n = i + 1;
  for (uint32_t i = 0; i < n; ++i) {
    if (!t.color[i]) {
      *why = "a draw buffer without an image between two with one";
      return false;
    }
    if (t.color[i]->w != t.w || t.color[i]->h != t.h || t.color[i]->d != 1 || t.layer[i] != 0) {
      *why = "framebuffer images of different sizes, or a layer of a texture";
      return false;
    }
  }
  *count = n;
  return true;
}

namespace {
uint8_t channels_kept(const Image& im) {
  const uint32_t base = im.info->base;
  return im.info->store == Store::U8x4 && (base == 0x1907 || base == 0x8227 || base == 0x1903) ? 7 : 15;
}
}  // namespace

// A clear of a whole framebuffer object on the device: its colour images to the clear colour (every channel; one colour for all, as GL clears them), its depth to
// the clear depth. The device copies are then newer than the pixels; a stencil plane (which the device never uses) is cleared on the CPU when the depth comes back,
// or now. False: the CPU clears (after hw_sync_target).
bool GlContext::hw_fbo_clear(uint32_t mask, const Target& t) {
  IAccelerator* acc = accelerator();
  if (!acc || acc->limits().image_colors == 0 || t.ss != 1 || t.layered || (desktop.active && desktop.per_buffer_masks)) return false;
  const char* why = nullptr;
  const bool color = (mask & 0x4000) != 0;
  const bool depth = (mask & 0x100) != 0 && t.depth && dmask_;
  if (color && !(cmask_[0] && cmask_[1] && cmask_[2] && cmask_[3])) return false;
  uint32_t n = 0;
  if (color && !hw_fbo_colors(t, &n, &why)) return false;
  if (n > acc->limits().image_colors) return false;
  AccelAttachments att;
  AccelImage* copies[kMaxDrawBuffers] = {};
  float rgba[4] = {clear_color_[0], clear_color_[1], clear_color_[2], clear_color_[3]};
  for (uint32_t i = 0; i < n; ++i) {
    Image& im = *t.color[i];
    AccelFormat f;
    if (!hw_format(im, &f, &why) || f == AccelFormat::Depth) return false;
    if (f == AccelFormat::Rgba8Srgb) return false;      // (the CPU encodes the clear colour itself: see clear())
    if (channels_kept(im) != 15 && rgba[3] != 1.0f) {      // (an image without alpha keeps it at 1: only a clear colour with alpha 1 is the same for all)
      bool all_without = true;
      for (uint32_t k = 0; k < n; ++k) all_without = all_without && channels_kept(*t.color[k]) != 15;
      if (!all_without) return false;
      rgba[3] = 1.0f;
    }
    copies[i] = hw_image(im, f, &why);
    if (!copies[i]) return false;
    att.colors[i] = copies[i]->handle;
  }
  att.color_count = n;
  AccelImage* dcopy = nullptr;
  if (depth) {
    if (t.depth->w != t.w || t.depth->h != t.h || t.depth->d != 1 || t.depth_layer != 0) return false;
    dcopy = hw_image(*t.depth, AccelFormat::Depth, &why);
    if (!dcopy) return false;
    att.depth = dcopy->handle;
  }
  if (!n && !dcopy) return false;
  const bool stencil = (mask & 0x400) && t.stencil && t.stencil->info->store == Store::DepthStencil;
  if (stencil && !dcopy) return false;      // (a stencil clear alone: the CPU's)
  const float d = clear_depth_ < 0 ? 0 : (clear_depth_ > 1 ? 1 : clear_depth_);
  if (!acc->clear_images(att, n > 0, rgba, dcopy != nullptr, d)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    copies[i]->set_ahead(true);
    copies[i]->cleared = false;
  }
  if (dcopy) {
    // (a depth image the device has the only copy of: what the CPU would have, a clear, is remembered for hw_sync_image)
    dcopy->set_ahead(true);
    dcopy->cleared = true;
    dcopy->cleared_value = d;
    dcopy->cpu_seen = 0;
  }
  if (stencil) {      // (the stencil is the CPU's: cleared there, but its image's version must not make the device depth look out of date)
    Image& s = *t.stencil;
    const uint32_t wm = swmask_[0] & 255;
    for (uint32_t y = 0; y < s.h; ++y)
      for (uint32_t x = 0; x < s.w; ++x) {
        uint32_t raw[4];
        read_texel(s, x, y, 0, raw);
        raw[1] = (raw[1] & ~wm) | (static_cast<uint32_t>(clear_stencil_) & wm & 255);
        write_texel(s, x, y, 0, raw);
      }
  }
  ++hw_stats_.gpu_clears;
  return true;
}

// A texture sampled with a mipmapped filter: one device image with its levels (from the base level up, as far as they are complete and the device takes them), each
// written up when its pixels changed. Only 8-bit colour images have device mipmaps.
bool GlContext::hw_mip_texture(Texture& t, const SamplerObj& so, uint32_t* handle, AccelSampler* sampler, const char** why) {
  IAccelerator* acc = accelerator();
  const auto no = [why](const char* reason) { *why = reason; return false; };
  if (!acc || !acc->limits().mip_images) return no("a mipmapped texture filter");
  const int32_t base = t.base_level < 0 ? 0 : t.base_level;
  if (base >= static_cast<int32_t>(kMaxLevels) || static_cast<uint64_t>(base) >= t.images.count() || !t.images[static_cast<uint64_t>(base)]) return no("an incomplete texture");
  Image& b = *t.images[static_cast<uint64_t>(base)];
  AccelFormat f;
  if (!hw_format(b, &f, why)) return false;
  if (f != AccelFormat::Rgba8 && f != AccelFormat::Rgba8Srgb) return no("a mipmapped texture of a format the device has no mipmaps of");
  if (b.d != 1 || b.ss != 1 || b.w > 8192 || b.h > 8192) return no("a texture larger than the device takes");
  // the levels: base, base + 1 ... while each is half the one before (texture_complete checked them), up to max_level and the 1 x 1 level
  const int32_t last = t.max_level < base ? base : (t.max_level > static_cast<int32_t>(kMaxLevels) - 1 ? static_cast<int32_t>(kMaxLevels) - 1 : t.max_level);
  uint32_t levels = 1, w = b.w, h = b.h;
  for (int32_t l = base + 1; l <= last && (w > 1 || h > 1); ++l) {
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
    if (static_cast<uint64_t>(l) >= t.images.count() || !t.images[static_cast<uint64_t>(l)] || !t.images[static_cast<uint64_t>(l)]->defined()) break;
    ++levels;
  }
  AccelImage* copy = t.hw_mips.get();
  if (!copy || copy->owner != acc || copy->format != f || copy->w != b.w || copy->h != b.h || copy->levels != levels || t.hw_mip_base != base) {
    t.hw_mips.reset();
    const uint32_t made = acc->create_image(b.w, b.h, f, levels);
    if (!made) return no("the device refused a mipmapped texture");
    t.hw_mips = UniquePointer<AccelImage>::make(a_);
    if (!t.hw_mips) {
      acc->destroy_texture(made);
      return no("out of memory");
    }
    copy = t.hw_mips.get();
    copy->owner = acc;
    copy->handle = made;
    copy->format = f;
    copy->w = b.w;
    copy->h = b.h;
    copy->levels = levels;
    t.hw_mip_base = base;
    for (uint64_t& v : t.hw_mip_versions) v = 0;
  }
  for (uint32_t k = 0; k < levels; ++k) {
    Image& im = *t.images[static_cast<uint64_t>(base) + k];
    hw_sync_image(im);      // (a level the device drew into comes back: the mipmapped copy is made from the pixels)
    const uint64_t v = im.version();
    if (t.hw_mip_versions[k] == v) continue;
    if (im.info != b.info || !acc->write_image(copy->handle, k, im.px.view())) return no("the device refused a mipmapped texture");
    t.hw_mip_versions[k] = v;
  }
  const bool nearest_within = so.min_filter == 0x2700 || so.min_filter == 0x2702;
  const bool linear_mag = so.mag_filter == 0x2601;
  if (nearest_within == linear_mag) return no("different filters within and between levels");      // (the device has one filter for both)
  sampler->linear = linear_mag;
  sampler->mip = so.min_filter == 0x2702 || so.min_filter == 0x2703 ? 2 : 1;
  sampler->min_lod = so.min_lod < 0 ? 0 : so.min_lod;
  sampler->max_lod = so.max_lod;
  sampler->lod_bias = so.lod_bias + t.lod_bias;
  *handle = copy->handle;
  return true;
}

// The texture bound to `unit` as the device can read sampler `ts`: a 2D (or 1D, rectangle) texture with a filter and wrap mode the device has. Its image goes to the
// device as its copy there (Image::hw), which is what a framebuffer object drew into when it did (no CPU copy then); a mipmapped filter takes the texture's levels
// (hw_mip_texture). A shadow sampler reads depth (a depth image the device drew into, or the CPU's depth sent as floats in R) with nearest filtering; the shader
// filters and compares, by `params`: the sampler's four CONST rows, see TgsiSampler. False: the CPU samples it.
bool GlContext::hw_texture(const TgsiSampler& ts, uint32_t unit, uint32_t* handle, AccelSampler* sampler, float params[16], const char** why) {
  const auto no = [why](const char* reason) { *why = reason; return false; };
  // (a larger texture than the device can make is refused, and the draw is then the CPU's; the cost of a large one is its upload once, far less than drawing with it
  // on the CPU: Unigine Heaven's 2048-wide loading picture took 4 s a frame there)
  constexpr uint32_t kMaxSide = 8192;
  if (unit >= kMaxTextureUnits) return no("a texture unit past the last");
  const TextureUnit& u = units_[unit];
  const uint32_t name = ts.dim == shader::kDim1D ? u.tex1d : (ts.dim == shader::kDimRect ? u.rect : u.tex2d);
  const uint32_t target = ts.dim == shader::kDim1D ? 0x0DE0 : (ts.dim == shader::kDimRect ? 0x84F5 : 0x0DE1);
  Texture* t = name && name <= objects_->textures.count() ? objects_->textures[name - 1].get() : nullptr;      // (no hw_sync_texture: the device reads its copies)
  if (!t || t->target != target) return no("a texture of a kind the device cannot sample");
  SamplerObj so;
  if (u.sampler && u.sampler <= objects_->samplers.count() && objects_->samplers[u.sampler - 1]) so = *objects_->samplers[u.sampler - 1];
  else {
    so.min_filter = t->min_filter;
    so.mag_filter = t->mag_filter;
    so.wrap_s = t->wrap_s;
    so.wrap_t = t->wrap_t;
    so.compare_mode = t->compare_mode;
    so.compare_func = t->compare_func;
    so.min_lod = t->min_lod;
    so.max_lod = t->max_lod;
    so.lod_bias = t->lod_bias;
  }
  if (t->swizzle[0] != 0x1903 || t->swizzle[1] != 0x1904 || t->swizzle[2] != 0x1905 || t->swizzle[3] != 0x1906) return no("a texture swizzle");
  const bool mipmapped = so.min_filter >= 0x2700 && so.min_filter <= 0x2703;
  const bool nearest = so.min_filter == 0x2600 && so.mag_filter == 0x2600, linear = so.min_filter == 0x2601 && so.mag_filter == 0x2601;
  if (!mipmapped && !nearest && !linear) return no("different filters for minification and magnification");
  auto wrap = [](uint32_t mode, bool* repeat, bool* mirror) {
    *repeat = mode == 0x2901 || mode == 0x8370;
    *mirror = mode == 0x8370;
    return mode == 0x2901 || mode == 0x812F || mode == 0x8370;
  };
  if (!wrap(so.wrap_s, &sampler->repeat_s, &sampler->mirror_s) || !wrap(so.wrap_t, &sampler->repeat_t, &sampler->mirror_t)) return no("a texture wrap mode");
  if (!ts.shadow && so.compare_mode != 0) return no("a texture comparison");      // (a comparison through a sampler that is not a shadow one: the CPU's)
  if (ts.dim == shader::kDim1D) sampler->repeat_t = true;
  sampler->linear = linear && !ts.shadow;
  bool cube;
  if (!texture_complete(*t, so, &cube) || t->images.count() == 0) return no("an incomplete texture");
  const int32_t base = t->base_level < 0 ? 0 : t->base_level;
  if (static_cast<uint64_t>(base) >= t->images.count() || !t->images[static_cast<uint64_t>(base)] || !t->images[static_cast<uint64_t>(base)]->defined()) return no("an incomplete texture");
  Image& im = *t->images[static_cast<uint64_t>(base)];
  if (im.w > kMaxSide || im.h > kMaxSide) return no("a texture larger than the device takes");
  if (im.d != 1 || im.ss != 1) return no("a layered or multisampled texture");
  IAccelerator* acc = accelerator();
  if (!acc) return no("no accelerator");
  if (ts.params >= 0) {
    for (uint32_t i = 0; i < 16; ++i) params[i] = 0;
    // row 0: the weights of reference < depth, ==, > (GL_TEXTURE_COMPARE_FUNC less the value of GL_NEVER is those three bits: LESS 1, EQUAL 2, LEQUAL 3 ... ALWAYS 7),
    // and linear filtering
    const uint32_t bits = so.compare_func >= 0x0200 && so.compare_func <= 0x0207 ? so.compare_func - 0x0200 : 3;
    params[0] = bits & 1 ? 1.0f : 0.0f;
    params[1] = bits & 2 ? 1.0f : 0.0f;
    params[2] = bits & 4 ? 1.0f : 0.0f;
    params[3] = linear || (mipmapped && so.mag_filter == 0x2601) ? 1.0f : 0.0f;
    // row 1: the size
    params[4] = static_cast<float>(im.w);
    params[5] = static_cast<float>(im.h);
    params[6] = 1.0f / static_cast<float>(im.w);
    params[7] = 1.0f / static_cast<float>(im.h);
    // rows 2 and 3: shadow1D / shadow2D's vec4 = r * row 2 + row 3 (GL_DEPTH_TEXTURE_MODE)
    const float m[4][8] = {{1, 1, 1, 0, 0, 0, 0, 1}, {1, 1, 1, 1, 0, 0, 0, 0}, {0, 0, 0, 1, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 1}};
    const uint32_t mode = t->depth_mode == 0x8049 ? 1 : (t->depth_mode == 0x1906 ? 2 : (t->depth_mode == 0x1903 ? 3 : 0));
    for (uint32_t i = 0; i < 8; ++i) params[8 + i] = m[mode][i];
  }
  const bool depth_image = im.info->store == Store::Depth || im.info->store == Store::DepthStencil;
  if (!ts.shadow && !depth_image && acc->limits().image_colors == 0) {
    // (a device without images: a texture of 8-bit texels, sent again when they changed - the cache compares them)
    if (mipmapped) return no("a mipmapped texture filter");
    if (im.info->store != Store::U8x4 || im.info->srgb || im.info->is_signed) return no("a texture format the device cannot sample");
    if (sampler->mirror_s || sampler->mirror_t) return no("a texture wrap mode");
    hw_sync_image(im);
    *handle = hw_textures_.get(*acc, name, im.w, im.h, im.px.view());
    return *handle != 0 || no("the device refused the texture");
  }
  if (!ts.shadow && !depth_image) {
    if (mipmapped && !(im.hw && im.hw->ahead)) return hw_mip_texture(*t, so, handle, sampler, why);
    // (a mipmapped filter on a texture the device drew into: its level 0 alone, which is what a framebuffer's picture sampled at its own size reads)
    if (mipmapped) {
      const bool nearest_within = so.min_filter == 0x2700 || so.min_filter == 0x2702;
      sampler->linear = !nearest_within;
    }
    AccelFormat f;
    if (!hw_format(im, &f, why)) return false;
    AccelImage* copy = hw_image(im, f, why);
    if (!copy) return false;
    if (!hw_current(im, *copy)) return no("the device refused the texture");
    *handle = copy->handle;
    return true;
  }
  if (!depth_image) return no("a shadow sampler of a colour texture");
  // A depth texture: its copy the device drew into, if it can sample that; else the CPU's depth as floats in R.
  sampler->linear = false;
  sampler->mip = 0;
  if (im.hw && im.hw->format == AccelFormat::Depth && im.hw->owner == acc && acc->limits().sampled_depth && (im.hw->ahead || im.hw->synced == im.version())) {
    *handle = im.hw->handle;
    return true;
  }
  if (!acc->limits().float_textures) return no("a depth texture the device cannot sample");
  if (im.hw && im.hw->ahead) hw_sync_image(im);
  ByteBuffer floats(a_);
  for (uint32_t y = 0; y < im.h; ++y)
    for (uint32_t x = 0; x < im.w; ++x) {
      uint32_t raw[4];
      read_texel(im, x, y, 0, raw);
      if (!floats.put32(raw[0]) || !floats.put32(0) || !floats.put32(0) || !floats.put32(f2u(1.0f))) return no("out of memory");
    }
  *handle = hw_textures_.get(*acc, name, im.w, im.h, floats.view(), true);
  return *handle != 0 || no("the device refused the texture");
}

void GlContext::hw_drop() {
  if (hw_owner_) hw_textures_.release(*hw_owner_);
  if (hw_owner_ && hw_target_) hw_owner_->destroy_target(hw_target_);
  hw_owner_ = nullptr;
  hw_target_ = 0;
  hw_w_ = hw_h_ = 0;
  hw_valid_ = false;
  hw_ahead_ = false;
}

// The default framebuffer is about to be other images (a window's buffers after a swap, or a resize). The picture on the device is the old images' (whoever needs it
// has synced it: GlService::swap does before the swap), so it says nothing of the new ones; a target of the same size is kept for the next frame's clear to start over in.
void GlContext::hw_retarget(uint32_t w, uint32_t h) {
  if (!hw_target_ || w != hw_w_ || h != hw_h_) {
    hw_drop();
    return;
  }
  hw_valid_ = false;
  hw_ahead_ = false;
}

void GlContext::hw_sync() {
  hw_apply_pending_clears();
  if (!hw_ahead_) return;
  hw_ahead_ = false;
  const uint64_t bytes = uint64_t{dcolor_->w} * dcolor_->h * 4;
  ByteView dst = dcolor_->px.writable_view();
  if (!hw_owner_ || !hw_target_ || dst.size() != bytes || !hw_owner_->read_color(hw_target_, dst)) {
    hw_valid_ = false;   // the device would not give the picture back: what the CPU has is the best there is
    return;
  }
  if (!attrs_.alpha)
    for (uint64_t i = 3; i < bytes; i += 4) dst[i] = 255;
  all_dirty_ = true;
}

// The CPU's picture becomes the device's (its colour; the depth stays behind: hw_depth_stale_), so draws can go on there after the CPU drew.
bool GlContext::hw_upload(IAccelerator* acc) {
  hw_apply_pending_clears();
  if (!hw_target(acc) || dcolor_->px.size() != uint64_t{hw_w_} * hw_h_ * 4 || !acc->write_color(hw_target_, dcolor_->px.view())) return false;
  hw_valid_ = true;
  hw_ahead_ = false;
  hw_depth_stale_ = ddepth_->defined();
  return true;
}

bool GlContext::hw_target(IAccelerator* acc) {
  if (hw_owner_ != acc) hw_drop();
  if (hw_target_) return true;
  if (!dcolor_->defined()) return false;
  hw_target_ = acc->create_target(dcolor_->w, dcolor_->h);
  if (!hw_target_) return false;
  hw_w_ = dcolor_->w;
  hw_h_ = dcolor_->h;
  hw_owner_ = acc;
  hw_valid_ = false;
  hw_ahead_ = false;
  return true;
}

// A clear of the whole default framebuffer on the device instead of the CPU, when its colour is cleared (all channels) and the device has the picture or can start one.
// The CPU's colour is left as it is (hw_ahead_: hw_sync() brings the device's); a depth or stencil clear the CPU owes is remembered for hw_apply_pending_clears().
bool GlContext::hw_fast_clear(uint32_t mask) {
  if (hw_clearing_cpu_ || !(mask & 0x4000) || !(cmask_[0] && cmask_[1] && cmask_[2] && cmask_[3])) return false;
  if (draw_fb_ != 0 || !default_draw_back_ || desktop.front_also || desktop.draw_front_and_back || !dcolor_->defined()) return false;
  IAccelerator* acc = accelerator();
  if (!acc || !hw_target(acc)) return false;
  float rgba[4] = {clear_color_[0], clear_color_[1], clear_color_[2], clear_color_[3]};
  if (!attrs_.alpha) rgba[3] = 1;
  const bool depth = ddepth_->defined();
  const bool depth_asked = (mask & 0x100) != 0 && dmask_ && depth;
  // (as hw_after_clear: a new picture starts with a depth of 1, a colour-only clear of the device's picture leaves its depth)
  const bool clear_depth = depth && (depth_asked || !hw_valid_);
  const float d = depth_asked ? (clear_depth_ < 0 ? 0 : (clear_depth_ > 1 ? 1 : clear_depth_)) : 1.0f;
  if (!acc->clear(hw_target_, true, rgba, clear_depth, d)) {
    hw_valid_ = false;
    return false;
  }
  hw_valid_ = true;
  hw_ahead_ = true;
  all_dirty_ = true;
  if (clear_depth) hw_depth_stale_ = false;
  if (depth_asked) {
    hw_pending_depth_ = true;
    hw_pending_depth_value_ = d;
  }
  if ((mask & 0x400) && ddepth_->defined() && ddepth_->info->store == Store::DepthStencil) {
    hw_pending_stencil_ = true;
    hw_pending_stencil_value_ = clear_stencil_;
    hw_pending_stencil_mask_ = swmask_[0];
  }
  return true;
}

// The depth and stencil clears hw_fast_clear() left the CPU's images owing: done before anything on the CPU reads or draws into them.
void GlContext::hw_apply_pending_clears() {
  if (!hw_pending_depth_ && !hw_pending_stencil_) return;
  const uint32_t mask = (hw_pending_depth_ ? 0x100u : 0u) | (hw_pending_stencil_ ? 0x400u : 0u);
  hw_pending_depth_ = hw_pending_stencil_ = false;
  // the CPU clear with the values of then, over the whole buffer
  const float saved_depth = clear_depth_;
  const int32_t saved_stencil = clear_stencil_;
  const uint32_t saved_mask = swmask_[0];
  const bool saved_dmask = dmask_, saved_scissor = scissor_test_;
  clear_depth_ = hw_pending_depth_value_;
  clear_stencil_ = hw_pending_stencil_value_;
  swmask_[0] = hw_pending_stencil_mask_;
  dmask_ = true;
  scissor_test_ = false;
  const uint32_t saved_fb = draw_fb_;
  draw_fb_ = 0;
  hw_clearing_cpu_ = true;
  clear(mask);
  hw_clearing_cpu_ = false;
  draw_fb_ = saved_fb;
  clear_depth_ = saved_depth;
  clear_stencil_ = saved_stencil;
  swmask_[0] = saved_mask;
  dmask_ = saved_dmask;
  scissor_test_ = saved_scissor;
}

// Called after a clear of the default framebuffer ran on the CPU: if it covered the drawing buffer, the device is cleared the same way and the two agree.
void GlContext::hw_after_clear(uint32_t mask, bool whole) {
  IAccelerator* acc = accelerator();
  const bool color = (mask & 0x4000) != 0 && cmask_[0] && cmask_[1] && cmask_[2] && cmask_[3];
  const bool depth = ddepth_->defined();
  if (!acc || !whole || !color || !hw_target(acc)) {
    hw_valid_ = false;
    return;
  }
  float rgba[4] = {clear_color_[0], clear_color_[1], clear_color_[2], clear_color_[3]};
  if (!attrs_.alpha) rgba[3] = 1;
  // The device depth is cleared with the color. A clear of the color alone starts a new picture on the device (hw_valid_ was false) with a depth of 1, the value of
  // the drawing buffer's first frame; when the picture was already the device's, a color-only clear leaves its depth alone.
  const bool depth_asked = (mask & 0x100) != 0 && dmask_;
  const bool clear_depth = depth && (depth_asked || !hw_valid_);
  const float d = depth_asked ? (clear_depth_ < 0 ? 0 : (clear_depth_ > 1 ? 1 : clear_depth_)) : 1.0f;
  const bool was_valid = hw_valid_;
  hw_valid_ = acc->clear(hw_target_, true, rgba, clear_depth, d);
  hw_ahead_ = false;   // def_color_ was just cleared to the same color
  if (clear_depth || !was_valid) hw_depth_stale_ = false;      // (a new picture: its depth is the clear's, as above)
}

bool GlContext::hw_program(ProgramObj& p, IAccelerator& acc) {
  // (a program with tessellation stages is translated for one patch size: the control stage's input, or the evaluation stage's without one)
  const bool tess = p.extra[kStageTessEval] != nullptr;
  const bool clip = desktop.active && desktop.clip_mask != 0;
  if (p.accel_tried && ((tess && p.accel_patch_vertices != desktop.patch_vertices) || (clip && !p.accel_clip))) {
    const char* why = p.accel_why;
    p.release_accel();
    p.accel_why = why;
  }
  if (p.accel_tried) return p.accel_program != 0;
  p.accel_tried = true;
  p.accel = &acc;
  p.accel_patch_vertices = desktop.patch_vertices;
  p.accel_clip = clip;
  if (!p.vsm || !p.fsm) return false;
  List<TgsiLink> links(a_);
  for (const VaryingLink& l : p.links) {
    // (a clamped colour is saturated in the shader: built with clamping on, see hw_draw)
    TgsiLink tl{l.vs_slot, l.fs_slot, l.count, l.clamp};
    tl.flat = l.flat;
    if (!links.push_back(tl)) return false;
  }
  UniquePointer<TgsiProgram> t = UniquePointer<TgsiProgram>::make(a_, a_);
  if (!t) return false;
  const AccelLimits al = acc.limits();
  TgsiLimits limits;
  limits.const_rows = al.const_rows;
  limits.attributes = al.attributes;
  limits.varyings = al.varyings;
  limits.control_varyings = al.control_varyings;
  limits.samplers = al.textures;
  limits.colors = al.image_colors ? al.image_colors : 1;
  TgsiStages stages;
  List<TgsiOutput> outputs(a_);
  for (const ProgramObj::Output& o : p.outputs)
    if (!outputs.push_back(TgsiOutput{o.slot, o.location})) return false;
  stages.outputs = outputs.view();
  stages.gs = p.extra[kStageGeometry] ? p.extra[kStageGeometry]->module.get() : nullptr;
  stages.tcs = p.extra[kStageTessControl] ? p.extra[kStageTessControl]->module.get() : nullptr;
  stages.tes = tess ? p.extra[kStageTessEval]->module.get() : nullptr;
  stages.patch_vertices = desktop.patch_vertices;
  stages.clip = clip;
  if ((stages.gs && !al.geometry) || (stages.tes && !al.tessellation)) {
    p.accel_why = "geometry or tessellation shaders on a device without them";
    return false;
  }
  if (!translate_program_to_tgsi(a_, *p.vsm, *p.fsm, stages, links.view(), t.get(), &p.accel_why, limits)) return false;
  p.accel_why = "the device refused the shaders";
  AccelStages as;
  as.gs = t->gs.size() ? t->gs.c_str() : nullptr;
  as.tcs = t->tcs.size() ? t->tcs.c_str() : nullptr;
  as.tes = t->tes.size() ? t->tes.c_str() : nullptr;
  as.gs_input = t->gs_input;
  as.gs_output = t->gs_output;
  as.gs_max_vertices = t->gs_max_vertices;
  as.gs_invocations = t->gs_invocations;
  as.patch_vertices = t->patch_vertices;
  as.tcs_vertices = t->tcs_vertices;
  as.tess_domain = t->tess_domain;
  as.tess_partitioning = t->tess_partitioning;
  as.tess_output = t->tess_output;
  p.accel_program = acc.create_program_stages(t->vs.c_str(), t->fs.c_str(), static_cast<uint32_t>(t->attributes.count()), t->varyings, static_cast<uint32_t>(t->samplers.count()), as);
  if (p.accel_program) {
    p.tgsi = static_cast<UniquePointer<TgsiProgram>&&>(t);
    p.accel_why = nullptr;
  }
  return p.accel_program != 0;
}

// Tries to run a draw on the accelerator. False: not done (nothing was drawn), the caller draws on the CPU.
bool GlContext::hw_draw(ProgramObj& p, const Target& target, uint32_t mode, int32_t first, int32_t count, uint32_t index_type, const uint8_t* idx, bool indexed,
                        int32_t instances) {
  const char* why = nullptr;
  const bool done = hw_draw_inner(p, target, mode, first, count, index_type, idx, indexed, instances, &why);
  if (done) {
    ++hw_stats_.gpu_draws;
  } else {
    hw_stats_.refused(why ? why : "refused");
  }
  return done;
}

bool GlContext::hw_draw_inner(ProgramObj& p, const Target& target, uint32_t mode, int32_t first, int32_t count, uint32_t index_type, const uint8_t* idx, bool indexed,
                              int32_t instances, const char** why) {
  const auto no = [why](const char* reason) { *why = reason; return false; };
  IAccelerator* acc = accelerator();
  if (!acc) return no("no accelerator");
  // The default framebuffer's picture (hw_target_), or a framebuffer object's images (their device copies, Image::hw).
  const bool fbo = !target.is_default;
  if (!fbo && (draw_fb_ != 0 || !default_draw_back_ || target.color[0] != dcolor_ || target.color_count != 1 || !dcolor_->defined() || desktop.front_also))
    return no("not one default colour buffer");
  if (fbo) {
    if (acc->limits().image_colors == 0) return no("framebuffer objects on a device without images");
    if (target.ss != 1) return no("a multisampled framebuffer object");
    if (target.layered) return no("a layered framebuffer object");
  }
  if (instances < 1) return no("instancing");
  if (instances != 1 && !acc->limits().instancing) return no("instancing");
  const AccelLimits limits = acc->limits();
  // What the rasterizer gets: the last stage's primitives (the geometry shader's, the tessellator's, or the draw's own).
  const ExtraStage* gs = p.extra[kStageGeometry].get();
  const ExtraStage* tes = p.extra[kStageTessEval].get();
  uint32_t raster = mode;
  if (gs) raster = gs->module->gs_output == 0 ? 0 : (gs->module->gs_output == 3 ? 1 : 4);
  else if (tes) raster = tes->module->tes_point_mode ? 0 : (tes->module->tes_mode == 0x8E7A ? 1 : 4);
  const bool lines = raster >= 1 && raster <= 3, points = raster == 0;
  if ((lines || points) && !limits.lines) return no("points or lines");
  if (lines && (line_width_ > 1.0f || (desktop.active && desktop.line_stipple))) return no("wide or stippled lines");
  if (points) {      // (one pixel: a size the program or the state sets is the CPU's)
    const shader::Module& last = p.last_module();
    const int32_t ps = last.find_builtin_global(shader::kBvPointSize);
    if (ps >= 0 && last.globals[static_cast<uint64_t>(ps)]->referenced) return no("a point size");
    if (desktop.active && (desktop.point_size != 1.0f || desktop.point_sprite || desktop.point_atten[1] != 0 || desktop.point_atten[2] != 0)) return no("a point size");
  }
  if (tf_active_ || rasterizer_discard_) return no("transform feedback or rasterizer discard");
  if (desktop.active && desktop.clip_mask && (!limits.clip_distances || !desktop.clip_indexed || (desktop.clip_mask & ~0xFFu))) return no("clip planes");
  if (query_active_[0] || query_active_[1]) return no("an occlusion query");
  for (uint32_t q : prim_queries_[0]) if (q) return no("a primitive query");      // (primitives generated: counted on the CPU)
  // (the device's shaders saturate the compatibility colours: with clamping turned off, the CPU draws)
  if (!desktop.clamp_vertex_color) for (const VaryingLink& l : p.links) if (l.clamp) return no("unclamped vertex colours");
  if (desktop.two_side_color && p.back_count) return no("two-sided colour");      // (two-sided colour: the CPU picks each triangle's side)
  if (desktop.active) {
    const int32_t pid = p.fsm->find_global("gl_PrimitiveID");      // (gl_PrimitiveID: counted by the CPU rasterizer)
    if (pid >= 0 && p.fsm->globals[static_cast<uint64_t>(pid)]->referenced) return no("gl_PrimitiveID");
    if (desktop.polygon_mode_front != 0x1B02 || desktop.polygon_mode_back != 0x1B02) return no("a polygon mode");
    if (desktop.flat_shade && desktop.flat_count) return no("flat shading");
    if (desktop.alpha_test) return no("the alpha test");
    if (desktop.logic_op && desktop.logic_mode != 0x1503) return no("a logic op");
    if (desktop.polygon_stipple && !lines && !points) return no("polygon stipple");
    if (desktop.depth_clamp) return no("depth clamp");
    if ((desktop.per_buffer_masks || desktop.per_buffer_blend) && !(limits.independent_blend && limits.blend_state)) return no("per-buffer blending or masks");
  }
  if (stencil_test_) return no("the stencil test");
  const uint8_t bias_modes = static_cast<uint8_t>((offset_fill_ ? 1 : 0) | (desktop.active && desktop.offset_line ? 2 : 0) | (desktop.active && desktop.offset_point ? 4 : 0));
  if (bias_modes && !limits.depth_bias) return no("polygon offset");
  if (alpha_to_coverage_ || sample_coverage_en_) return no("sample coverage");
  const uint8_t color_mask = static_cast<uint8_t>((cmask_[0] ? 1 : 0) | (cmask_[1] ? 2 : 0) | (cmask_[2] ? 4 : 0) | (cmask_[3] ? 8 : 0));
  const bool depth = fbo ? target.depth != nullptr : ddepth_->defined();
  const bool depth_test = depth_test_ && depth;
  const bool dst_alpha = fbo || attrs_.alpha;      // (a framebuffer object's images without alpha read it as 1, as the device's do: see channels_kept)
  // Blending, the colour mask and the depth function: the device's own state when it has it; else only source over destination, every channel and LEQUAL
  // (LESS is taken as LEQUAL: they differ only for equal depths).
  bool custom = false;
  uint8_t factors[4] = {1, 0, 1, 0}, ops[2] = {0, 0};
  if (limits.blend_state) {
    if (blend_) {
      if (!factor(blend_sr_, &factors[0]) || !factor(blend_dr_, &factors[1]) || !factor(blend_sa_, &factors[2]) || !factor(blend_da_, &factors[3])) return no("a blend factor");
      if (!equation(blend_eq_rgb_, &ops[0]) || !equation(blend_eq_a_, &ops[1])) return no("a blend equation");
      // (a drawing buffer without alpha reads its alpha as 1: the device's image keeps whatever was written there)
      if (!dst_alpha)
        for (uint8_t f : factors) if (f == 8 || f == 9 || f == 10) return no("a destination alpha factor without alpha");
    }
    custom = true;
  } else {
    if (color_mask != 15) return no("a colour mask");
    if (depth_test && dfunc_ != 0x0201 && dfunc_ != 0x0203) return no("a depth function");
    if (blend_) {
      const bool add = blend_eq_rgb_ == 0x8006 && blend_eq_a_ == 0x8006;
      const bool over = blend_sr_ == 0x0302 && blend_dr_ == 0x0303;
      const bool alpha_same = blend_sa_ == blend_sr_ && blend_da_ == blend_dr_;
      if (!add || !over || (dst_alpha && !alpha_same)) return no("a blend function");
    }
  }
  if (!hw_program(p, *acc) || !p.tgsi) return no(p.accel_why ? p.accel_why : "the shaders");
  const TgsiProgram& t = *p.tgsi;
  AccelAttachments att;
  AccelImage* copies[kMaxDrawBuffers] = {};
  AccelImage* dcopy = nullptr;
  if (!fbo) {
    if (!hw_target(acc)) return no("no device target");
    // A device picture that is out of date is written up from the CPU's before the draw (hw_upload), but not its depth: a draw with the depth test then stays here.
    if (depth_test && (!hw_valid_ || hw_depth_stale_)) return no(hw_valid_ ? "the device depth is out of date" : "the device picture is out of date");
    if (!hw_valid_ && !acc->limits().color_upload) return no("the device picture is out of date");
  } else {
    // A framebuffer object: its images' device copies, made or brought up to date (a colour image's pixels are written up when they are newer; a depth image's
    // cannot be, and a draw that needs the depth then stays here until the device clears it).
    uint32_t n = 0;
    if (!hw_fbo_colors(target, &n, why)) return false;
    if (n > acc->limits().image_colors) return no("more draw buffers than the device has");
    if (p.frag_writes_all && n > 1) return no("gl_FragColor into several draw buffers");
    for (uint32_t i = 0; i < n; ++i) {
      Image& im = *target.color[i];
      AccelFormat f;
      if (!hw_format(im, &f, why)) return false;
      if (f == AccelFormat::Depth) return no("an image format the device does not have");
      if (channels_kept(im) != 15) return no("a colour image without alpha");      // (its alpha must stay 1: a mask per image the state here does not have yet)
      copies[i] = hw_image(im, f, why);
      if (!copies[i]) return false;
      if (!hw_current(im, *copies[i])) return no("the device refused an image");
      att.colors[i] = copies[i]->handle;
    }
    att.color_count = n;
    if (depth && (depth_test || dmask_)) {
      Image& dim = *target.depth;
      if (dim.w != target.w || dim.h != target.h || dim.d != 1 || target.depth_layer != 0) return no("framebuffer images of different sizes, or a layer of a texture");
      dcopy = hw_image(dim, AccelFormat::Depth, why);
      if (!dcopy) return false;
      if (!hw_current(dim, *dcopy)) {
        if (depth_test) return no("the device depth is out of date");
        dcopy = nullptr;      // (no test: drawn without the depth, which stays out of date on the device until it is cleared there)
      } else {
        att.depth = dcopy->handle;
      }
    }
    if (n == 0 && !dcopy) return no("a framebuffer object without images the draw writes");
    if (n == 0 && !custom) return no("a depth-only draw on a device without a colour mask");
  }
  if (t.no_color && !custom) return no("a fragment shader without a color output");      // (its draws mask every channel: the device's colour mask)
  if (t.uses_frag_coord && target.ss != 1) return no("gl_FragCoord on an antialiased drawing buffer");
  if (fbo && t.color_outputs > att.color_count && att.color_count) {}      // (outputs past the images: the device drops them, as GL does)

  // the attributes of the translated shader, in the order of its IN[] registers
  Vao* vao = current_vao_ptr();
  const VertexAttrib* attribs[kTgsiMaxAttributes] = {};
  uint32_t instanced = 0;      // (bit k: IN[k] steps per instance)
  const uint32_t nattr = static_cast<uint32_t>(t.attributes.count());
  if (nattr == 0 || nattr > kTgsiMaxAttributes) return no("the number of attributes");
  for (uint32_t k = 0; k < nattr; ++k) {
    if (t.attributes[k].slot == kTgsiNoAttribute || t.attributes[k].slot == kTgsiVertexId) continue;      // (zeros, or the vertex's number: below)
    const AttribInfo* info = nullptr;
    for (const auto& ai : p.attribs)
      if (ai->slot == t.attributes[k].slot) info = ai.get();
    // (the fixed-function arrays of desktop GL are the slots past kMaxAttribs)
    if (!info || info->location < 0 || static_cast<uint32_t>(info->location) >= (desktop.active ? kMaxAttribSlots : kMaxAttribs)) return no("an attribute location");
    attribs[k] = &vao->attribs[info->location];
    if (attribs[k]->divisor && attribs[k]->enabled) {      // (a per-instance array: its values follow the vertices, one per instance - see below)
      if (!limits.instancing) return no("an instanced attribute");
      instanced |= 1u << k;
    }
  }

  // the primitives of the draw, as a list of vertex indices (the primitive modes and restart markers of the software path)
  const uint32_t fixed_restart = index_type == 0x1401 ? 0xFF : (index_type == 0x1403 ? 0xFFFF : 0xFFFFFFFFu);
  const bool restarts = indexed && (!desktop.active || desktop.primitive_restart || desktop.primitive_restart_fixed);
  const uint32_t restart = !desktop.active || desktop.primitive_restart_fixed ? fixed_restart : desktop.restart_index;
  auto index_at = [&](int32_t i) -> int64_t {
    if (!indexed) return int64_t{first} + i;
    const uint32_t v = read_index_value(idx + uint64_t{static_cast<uint32_t>(i)} * (index_type == 0x1401 ? 1 : (index_type == 0x1403 ? 2 : 4)), index_type);
    if (restarts && v == restart) return -1;
    const int64_t rebased = static_cast<int64_t>(v) + desktop.base_vertex;
    return rebased < 0 ? 0 : rebased;
  };
  List<int64_t> tris(a_);
  // (flat varyings take the last vertex's value on the device; with GL_FIRST_VERTEX_CONVENTION the first vertex is moved to the end, which keeps the winding)
  bool any_flat = false;
  for (const VaryingLink& l : p.links) any_flat = any_flat || l.flat;
  const bool rotate = any_flat && desktop.active && desktop.first_vertex_convention;
  if (rotate && p.has_extra()) return no("flat varyings with the first vertex convention");      // (the provoking vertex is then the last stage's: nothing to rotate)
  auto tri = [&](int32_t i0, int32_t i1, int32_t i2) {
    const int64_t a0 = index_at(i0), a1 = index_at(i1), a2 = index_at(i2);
    if (a0 < 0 || a1 < 0 || a2 < 0) return true;
    if (rotate) return tris.push_back(a1) && tris.push_back(a2) && tris.push_back(a0);
    return tris.push_back(a0) && tris.push_back(a1) && tris.push_back(a2);
  };
  auto seg = [&](int32_t i0, int32_t i1) {
    const int64_t a0 = index_at(i0), a1 = index_at(i1);
    if (a0 < 0 || a1 < 0) return true;
    if (rotate) return tris.push_back(a1) && tris.push_back(a0);
    return tris.push_back(a0) && tris.push_back(a1);
  };
  bool ok = true;
  uint8_t topology = static_cast<uint8_t>(points ? 2 : (lines ? 1 : 0));
  if (p.has_extra()) {
    // The draw's primitives as the first stage after the vertex shader takes them: patches for tessellation, the geometry shader's input primitive (lists of the
    // device's topologies; adjacency strips become lists as the CPU makes them).
    uint32_t takes;      // the GL primitive the stages take (as gs_input numbers them)
    if (tes) {
      if (mode != 0xE) return no("a primitive the program does not take");
      if (!limits.tessellation || desktop.patch_vertices < 1 || desktop.patch_vertices > 32) return no("patches the device cannot take");
      takes = 0xE;
      topology = 3;
    } else {
      takes = mode == 0 ? 0 : (mode <= 3 ? 1 : (mode <= 6 ? 4 : (mode <= 0xB ? 0xA : (mode <= 0xD ? 0xC : 0xE))));
      if (!gs || takes != gs->module->gs_input) return no("a primitive the program does not take");      // (an error: the CPU path says so)
      topology = static_cast<uint8_t>(takes == 0 ? 2 : takes == 1 ? 1 : takes == 0xA ? 4 : takes == 0xC ? 5 : 0);
    }
    if (!limits.geometry && gs) return no("geometry shaders on a device without them");
    auto vertex = [&](int32_t i) -> int64_t { return index_at(i); };
    auto primitive = [&](const int32_t* pos, uint32_t n) {
      for (uint32_t k = 0; k < n && ok; ++k) ok = tris.push_back(index_at(pos[k]));
      if (tris.count() > kMaxAccelVertices) ok = false;
    };
    assemble_primitives(mode, count, desktop.patch_vertices, vertex, primitive);
  } else if (mode == 0) {
    for (int32_t i = 0; i < count && ok; ++i) {
      const int64_t v = index_at(i);
      if (v >= 0) ok = tris.push_back(v);
      if (tris.count() > kMaxAccelVertices) ok = false;
    }
  } else if (mode == 1) {
    for (int32_t i = 0; i + 1 < count && ok; i += 2) ok = seg(i, i + 1);
  } else if (mode == 2 || mode == 3) {      // a loop or a strip, each piece between restarts
    int32_t start = 0;
    for (int32_t i = 0; i <= count && ok; ++i) {
      if (i < count && index_at(i) >= 0) {
        if (i > start) ok = seg(i - 1, i);
      } else {
        if (mode == 2 && i - start > 2) ok = seg(i - 1, start);
        start = i + 1;
      }
      if (tris.count() > kMaxAccelVertices) ok = false;
    }
  } else if (mode == 4) {
    for (int32_t i = 0; i + 2 < count && ok; i += 3) ok = tri(i, i + 1, i + 2);
  } else {
    int32_t start = 0;
    for (int32_t i = 0; i <= count && ok; ++i) {
      if (i < count && index_at(i) >= 0) {
        if (i >= start + 2) {
          if (mode == 5) ok = ((i - start - 2) & 1) ? tri(i - 1, i - 2, i) : tri(i - 2, i - 1, i);
          else ok = tri(start, i - 1, i);
        }
      } else {
        start = i + 1;
      }
      if (tris.count() > kMaxAccelVertices) ok = false;
    }
  }
  if (!ok) return no("too many vertices");
  if (tris.count() == 0) return true;

  // vertex data: four floats per attribute per vertex (IN[] order, the per-instance ones left out), then four per per-instance attribute per instance
  ByteBuffer vertices(a_);
  const auto put_attribute = [&](uint32_t k, uint32_t index) {
    if (!attribs[k]) {
      const uint32_t x = t.attributes[k].slot == kTgsiVertexId ? f2u(static_cast<float>(index)) : 0;
      return vertices.put32(x) && vertices.put32(0) && vertices.put32(0) && vertices.put32(0);
    }
    const VertexAttrib& at = *attribs[k];
    uint32_t lanes[4];
    if (at.enabled) {
      const Buffer* b = buffer_at(at.buffer);
      const uint32_t esize = vertex_attrib_element_bytes(at);
      const uint32_t stride = at.stride ? at.stride : esize;
      if (!b || at.offset + uint64_t{index} * stride + esize > b->data.size()) {
        *why = "an attribute past its buffer";
        return false;
      }
      if (t.attributes[k].convert == kTgsiAsIs) {
        read_vertex_attrib(at, b->data.view().data() + at.offset + uint64_t{index} * stride, lanes);
      } else {      // an integer input: read as the CPU reads it (integers), sent as their float values
        read_vertex_attrib_int(at, b->data.view().data() + at.offset + uint64_t{index} * stride, lanes);
        for (uint32_t l : lanes)
          if (t.attributes[k].convert == kTgsiFromUint ? l > kExactInt : !exact_int(static_cast<int32_t>(l))) {
            *why = "an integer the device cannot hold exactly";
            return false;
          }
        for (uint32_t& l : lanes) l = f2u(t.attributes[k].convert == kTgsiFromUint ? static_cast<float>(l) : static_cast<float>(static_cast<int32_t>(l)));
      }
    } else if (t.attributes[k].convert != kTgsiAsIs) {
      for (int i = 0; i < 4; ++i) lanes[i] = f2u(static_cast<float>(at.cur_is_int ? at.cur_i[i] : static_cast<int32_t>(at.cur_f[i])));
    } else {
      for (int i = 0; i < 4; ++i) lanes[i] = f2u(at.cur_is_int ? static_cast<float>(at.cur_i[i]) : at.cur_f[i]);
    }
    for (uint32_t i = 0; i < 4; ++i)
      if (!vertices.put32(lanes[i])) {
        *why = "out of memory";
        return false;
      }
    return true;
  };
  if (instanced && instanced == (nattr >= 32 ? ~0u : (1u << nattr) - 1u)) return no("an instanced draw without a per-vertex attribute");
  for (uint64_t v = 0; v < tris.count(); ++v) {
    const uint32_t index = static_cast<uint32_t>(tris[v]);
    for (uint32_t k = 0; k < nattr; ++k)
      if (!(instanced & (1u << k)) && !put_attribute(k, index)) return false;
  }
  for (uint32_t i = 0; instanced && i < static_cast<uint32_t>(instances); ++i)      // (the element of instance i: i / divisor, after the base instance)
    for (uint32_t k = 0; k < nattr; ++k)
      if ((instanced & (1u << k)) && !put_attribute(k, i / attribs[k]->divisor + desktop.base_instance)) return false;

  // uniforms: the CONST rows
  ByteBuffer constants(a_);
  for (uint32_t row = 0; row < t.const_rows; ++row) {
    uint32_t lanes[4] = {0, 0, 0, 0};
    for (const TgsiConstant& c : t.constants) {
      if (c.row != row) continue;
      if (c.stage == kTgsiClipPlanes) {      // (the user clip planes, eye space)
        for (uint32_t i = 0; i < c.comps && i < 4; ++i) lanes[i] = f2u(c.slot / 4 < 8 ? desktop.clip_planes[c.slot / 4][i] : 0.0f);
        continue;
      }
      if (c.stage == kTgsiPatchLevels) {      // (the default tessellation levels: outer, then inner)
        for (uint32_t i = 0; i < c.comps && i < 4; ++i) {
          const uint32_t at = c.slot + i;
          lanes[i] = f2u(at < 4 ? desktop.patch_outer[at] : (at < 6 ? desktop.patch_inner[at - 4] : 0.0f));
        }
        continue;
      }
      const ExtraStage* extra = c.stage >= 2 && c.stage < 5 ? p.extra[c.stage - 2].get() : nullptr;
      if (c.stage >= 2 && !extra) continue;
      const List<uint32_t>& shared = c.stage == 0 ? p.vs_shared : (c.stage == 1 ? p.fs_shared : extra->shared);
      for (uint32_t i = 0; i < c.comps && i < 4; ++i) {
        if (c.slot + i >= shared.count()) continue;
        const uint32_t bits = shared[c.slot + i];
        // (the device keeps integers and booleans as float values, exact up to 2^24: a larger integer draws on the CPU, which wraps as GLSL says)
        switch (c.convert) {
          case kTgsiFromInt:
            if (!exact_int(static_cast<int32_t>(bits))) return no("an integer the device cannot hold exactly");
            lanes[i] = f2u(static_cast<float>(static_cast<int32_t>(bits)));
            break;
          case kTgsiFromUint:
            if (bits > kExactInt) return no("an integer the device cannot hold exactly");
            lanes[i] = f2u(static_cast<float>(bits));
            break;
          case kTgsiFromBool: lanes[i] = f2u(bits ? 1.0f : 0.0f); break;
          default: lanes[i] = bits; break;
        }
      }
    }
    for (uint32_t i = 0; i < 4; ++i)
      if (!constants.put32(lanes[i])) return no("out of memory");
  }

  const float ss = static_cast<float>(target.ss);
  AccelState st;
  // the textures: each sampler uniform holds a texture unit
  st.texture_count = static_cast<uint32_t>(t.samplers.count());
  if (st.texture_count > kAccelMaxTextures) return no("too many textures");
  for (uint32_t k = 0; k < st.texture_count; ++k) {
    float params[16];
    const char* texture_why = "a texture the device cannot sample";
    if (t.samplers[k].slot >= p.fs_shared.count() || !hw_texture(t.samplers[k], p.fs_shared[t.samplers[k].slot], &st.textures[k], &st.samplers[k], params, &texture_why))
      return no(texture_why);
    if (t.samplers[k].params < 0) continue;
    const uint64_t at = uint64_t{static_cast<uint32_t>(t.samplers[k].params)} * 16;      // (its rows: after the uniforms were packed)
    if (at + sizeof params > constants.size()) return no("a texture the device cannot sample");
    ByteView rows = constants.writable_view().subview(at, sizeof params);
    for (uint32_t i = 0; i < 16; ++i) {
      const uint32_t bits = f2u(params[i]);
      for (uint32_t b = 0; b < 4; ++b) rows[uint64_t{i} * 4 + b] = static_cast<uint8_t>(bits >> (8 * b));
    }
  }
  st.vp_x = static_cast<float>(vp_[0]) * ss;
  st.vp_y = static_cast<float>(vp_[1]) * ss;
  st.vp_w = static_cast<float>(vp_[2]) * ss;
  st.vp_h = static_cast<float>(vp_[3]) * ss;
  st.depth_near = depth_n_;
  st.depth_far = depth_f_;
  st.scissor = scissor_test_;
  if (scissor_test_) {
    st.sc_x = sc_[0] * static_cast<int32_t>(target.ss);
    st.sc_y = sc_[1] * static_cast<int32_t>(target.ss);
    st.sc_w = static_cast<uint32_t>(sc_[2]) * target.ss;
    st.sc_h = static_cast<uint32_t>(sc_[3]) * target.ss;
  }
  st.cull = cull_ ? (cull_mode_ == 0x404 ? 1 : (cull_mode_ == 0x405 ? 2 : 3)) : 0;
  if (st.cull == 3 && !lines && !points) return true;   // both faces culled: nothing to draw
  st.front_ccw = front_face_ == 0x0901;
  st.depth_test = depth_test;
  st.depth_write = dmask_ && depth;
  if (fbo && !dcopy) st.depth_test = st.depth_write = false;
  st.blend = blend_;
  st.topology = topology;
  st.instances = static_cast<uint32_t>(instances);
  if (bias_modes && (po_factor_ != 0 || po_units_ != 0)) {
    st.depth_bias_modes = bias_modes;
    st.depth_bias_factor = po_factor_;
    st.depth_bias_units = po_units_;
  }
  st.instanced = instanced;
  st.patch_vertices = topology == 3 ? desktop.patch_vertices : 0;
  st.clip_mask = desktop.active ? static_cast<uint8_t>(desktop.clip_mask) : 0;
  if (lines || points) st.cull = 0;      // (only polygons are culled)
  if (custom) {
    st.custom_blend = true;
    st.src_rgb = factors[0];
    st.dst_rgb = factors[1];
    st.src_alpha = factors[2];
    st.dst_alpha = factors[3];
    st.op_rgb = ops[0];
    st.op_alpha = ops[1];
    st.color_mask = t.no_color || (fbo && att.color_count == 0) ? 0 : color_mask;
    st.depth_compare = static_cast<uint8_t>(dfunc_ >= 0x0200 && dfunc_ <= 0x0207 ? dfunc_ - 0x0200 : 3);
  }
  // glBlendFunci / glColorMaski / glEnablei(GL_BLEND, i): each colour image's own blending and mask (AccelState::per_target)
  if (desktop.active && (desktop.per_buffer_masks || desktop.per_buffer_blend)) {
    const uint32_t n = fbo ? att.color_count : 1;
    st.per_target = true;
    for (uint32_t i = 0; i < n && i < kAccelMaxColors; ++i) {
      AccelState::TargetBlend& tb = st.targets[i];
      const DesktopState::BufferBlend& bb = desktop.blend[i];
      tb.blend = desktop.per_buffer_blend ? bb.enabled : blend_;
      const uint32_t sr = desktop.per_buffer_blend ? bb.sr : blend_sr_, dr = desktop.per_buffer_blend ? bb.dr : blend_dr_;
      const uint32_t sa = desktop.per_buffer_blend ? bb.sa : blend_sa_, da = desktop.per_buffer_blend ? bb.da : blend_da_;
      const uint32_t er = desktop.per_buffer_blend ? bb.eq_rgb : blend_eq_rgb_, ea = desktop.per_buffer_blend ? bb.eq_a : blend_eq_a_;
      if (tb.blend) {
        if (!factor(sr, &tb.src_rgb) || !factor(dr, &tb.dst_rgb) || !factor(sa, &tb.src_alpha) || !factor(da, &tb.dst_alpha)) return no("a blend factor");
        if (!equation(er, &tb.op_rgb) || !equation(ea, &tb.op_alpha)) return no("a blend equation");
      }
      tb.color_mask = t.no_color ? 0 : (desktop.per_buffer_masks ? static_cast<uint8_t>(desktop.color_mask[i] & 15) : color_mask);
    }
    // (target 0 is also the pipeline's own state)
    st.blend = st.targets[0].blend;
    st.src_rgb = st.targets[0].src_rgb;
    st.dst_rgb = st.targets[0].dst_rgb;
    st.src_alpha = st.targets[0].src_alpha;
    st.dst_alpha = st.targets[0].dst_alpha;
    st.op_rgb = st.targets[0].op_rgb;
    st.op_alpha = st.targets[0].op_alpha;
    st.color_mask = st.targets[0].color_mask;
  }
  if (fbo) {
    // (a draw that samples one of its own images is the CPU's: the device refuses it, and nothing is drawn)
    for (uint32_t k = 0; k < st.texture_count; ++k) {
      for (uint32_t i = 0; i < att.color_count; ++i)
        if (st.textures[k] == att.colors[i]) return no("a texture the draw also renders into");
      if (dcopy && st.textures[k] == att.depth) return no("a texture the draw also renders into");
    }
    if (!acc->draw_images(att, p.accel_program, st, vertices.view(), static_cast<uint32_t>(tris.count()), constants.view())) return no("the device failed the draw");
    for (uint32_t i = 0; i < att.color_count; ++i) copies[i]->set_ahead(true);
    if (dcopy && st.depth_write) {
      dcopy->set_ahead(true);
      dcopy->cleared = false;
    }
    ++version_;
    return true;
  }
  if (!hw_valid_ && !hw_upload(acc)) return no("the device picture is out of date");
  if (!acc->draw(hw_target_, p.accel_program, st, vertices.view(), static_cast<uint32_t>(tris.count()), constants.view())) {
    hw_valid_ = false;   // the device may have drawn part of it: the caller reads the picture back and goes on on the CPU
    return no("the device failed the draw");
  }
  hw_ahead_ = true;
  all_dirty_ = true;
  ++version_;
  return true;
}

}  // namespace webgl
