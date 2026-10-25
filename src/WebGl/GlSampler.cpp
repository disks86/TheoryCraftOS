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

// Texture sampling for shaders: filtering, mipmaps, wrapping, cube maps, 3D and array textures, depth comparison.
#include "WebGl/GlContext.hpp"
#include "WebGl/GlPixels.hpp"

namespace webgl {

namespace {

inline float u2f(uint32_t u) { return __builtin_bit_cast(float, u); }
inline uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }

int32_t wrap_index(int32_t i, int32_t n, uint32_t mode) {
  switch (mode) {
    case 0x812F: return i < 0 ? 0 : (i >= n ? n - 1 : i);
    case 0x8370: {   // mirrored repeat
      int32_t period = 2 * n;
      int32_t m = i % period;
      if (m < 0) m += period;
      return m < n ? m : period - 1 - m;
    }
    default: {
      int32_t m = i % n;
      return m < 0 ? m + n : m;
    }
  }
}

// One texel as four floats (or raw integer lanes for integer images).
void fetch_f(const Image& im, int32_t x, int32_t y, int32_t z, float out[4]) {
  uint32_t raw[4];
  read_texel(im, static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z), raw);
  switch (im.info->store) {
    case Store::U8x4:
      for (int i = 0; i < 4; ++i) {
        float v = raw[i] / 255.0f;
        if (im.info->srgb && i < 3) v = srgb_to_linear(v);
        out[i] = v;
      }
      // components the format does not have read as 0, 0, 0, 1 (luminance and alpha expand at upload)
      break;
    case Store::F32x4: for (int i = 0; i < 4; ++i) out[i] = u2f(raw[i]); break;
    case Store::Depth: case Store::DepthStencil: out[0] = u2f(raw[0]); out[1] = 0; out[2] = 0; out[3] = 1; break;
    default: for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(static_cast<int32_t>(raw[i])); break;
  }
}

bool compare(uint32_t func, float ref, float d) {
  switch (func) {
    case 0x200: return false;
    case 0x201: return ref < d;
    case 0x202: return ref == d;
    case 0x203: return ref <= d;
    case 0x204: return ref > d;
    case 0x205: return ref != d;
    case 0x206: return ref >= d;
    default: return true;
  }
}

float log2f_(float x) { return shader::flog(x) * 1.4426950408889634f; }

}  // namespace

bool GlContext::texture_complete(const Texture& t, const SamplerObj& s, bool* is_cube) const {
  *is_cube = t.target == 0x8513;
  const int32_t base = t.base_level < 0 ? 0 : t.base_level;
  const uint32_t faces = *is_cube ? 6 : 1;
  const bool mips = s.min_filter >= 0x2700 && s.min_filter <= 0x2703;
  for (uint32_t f = 0; f < faces; ++f) {
    const uint64_t idx = uint64_t{f} * kMaxLevels + static_cast<uint32_t>(base);
    if (base >= static_cast<int32_t>(kMaxLevels) || idx >= t.images.count() || !t.images[idx] || !t.images[idx]->defined()) return false;
    const Image& b = *t.images[idx];
    if (*is_cube) {
      const Image& b0 = *t.images[static_cast<uint64_t>(base)];
      if (!b0.defined() || b.w != b0.w || b.h != b.w || b.info != b0.info) return false;
    }
    if (mips) {
      uint32_t w = b.w, h = b.h;
      const int32_t last = t.max_level < base ? base : (t.max_level > static_cast<int32_t>(kMaxLevels) - 1 ? static_cast<int32_t>(kMaxLevels) - 1 : t.max_level);
      for (int32_t l = base + 1; l <= last && (w > 1 || h > 1); ++l) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        const uint64_t li = uint64_t{f} * kMaxLevels + static_cast<uint32_t>(l);
        if (li >= t.images.count() || !t.images[li] || !t.images[li]->defined() || t.images[li]->w != w || t.images[li]->h != h) return false;
      }
    }
  }
  return true;
}

void GlContext::sample(const shader::SampleRequest& rq, uint32_t out[4]) {
  const uint32_t unit = rq.texture;
  const bool is_int_sampler = rq.texel != shader::Base::Float;
  out[0] = out[1] = out[2] = 0;
  out[3] = is_int_sampler ? 1 : f2u(1.0f);
  if (unit >= kMaxTextureUnits) return;
  const TextureUnit& u = units_[unit];
  uint32_t id;
  switch (rq.dim) {
    case shader::kDimCube: id = u.cube; break;
    case shader::kDim3D: id = u.tex3d; break;
    case shader::kDim2DArray: id = u.array; break;
    default: id = u.tex2d; break;
  }
  const Texture* t = texture_at(id);
  if (!t) return;
  SamplerObj tmp;
  const SamplerObj* so = &tmp;
  if (u.sampler && u.sampler <= samplers_.count() && samplers_[u.sampler - 1]) so = samplers_[u.sampler - 1].get();
  else {
    tmp.min_filter = t->min_filter; tmp.mag_filter = t->mag_filter; tmp.wrap_s = t->wrap_s; tmp.wrap_t = t->wrap_t; tmp.wrap_r = t->wrap_r;
    tmp.compare_mode = t->compare_mode; tmp.compare_func = t->compare_func; tmp.min_lod = t->min_lod; tmp.max_lod = t->max_lod;
  }
  sample_texture(*t, *so, rq, out);
}

static const Image* texture_image_at(const void* ctx, uint32_t face, int32_t level) {
  const Texture& t = *static_cast<const Texture*>(ctx);
  if (level < 0 || level >= static_cast<int32_t>(kMaxLevels)) return nullptr;
  const uint64_t idx = uint64_t{face} * kMaxLevels + static_cast<uint32_t>(level);
  return idx < t.images.count() && t.images[idx] && t.images[idx]->defined() ? t.images[idx].get() : nullptr;
}

void GlContext::sample_texture(const Texture& t, const SamplerObj& s, const shader::SampleRequest& rq, uint32_t out[4]) const {
  bool is_cube;
  if (!texture_complete(t, s, &is_cube)) {
    if (rq.mode == shader::SampleMode::Size || rq.mode == shader::SampleMode::Levels) {} else return;
  }
  SampleSource src;
  src.get = &texture_image_at;
  src.ctx = &t;
  src.target = t.target;
  src.base_level = t.base_level;
  src.max_level = t.max_level;
  sample_images(src, s, rq, out);
}

void sample_images(const SampleSource& t, const SamplerObj& s, const shader::SampleRequest& rq, uint32_t out[4]) {
  using shader::SampleMode;
  const bool is_cube = t.target == 0x8513;
  const int32_t base = t.base_level < 0 ? 0 : t.base_level;
  auto image_at = [&](uint32_t face, int32_t level) -> const Image* {
    return t.get(t.ctx, t.faces_in_layers ? 0 : face, level);
  };
  const Image* b0 = image_at(0, base);
  if (!b0) return;
  // how many levels exist
  int32_t max_level = base;
  {
    uint32_t w = b0->w, h = b0->h, d = b0->d;
    const bool three = t.target == 0x806F;
    int32_t last = t.max_level > static_cast<int32_t>(kMaxLevels) - 1 ? static_cast<int32_t>(kMaxLevels) - 1 : t.max_level;
    for (int32_t l = base; l < last && (w > 1 || h > 1 || (three && d > 1)); ++l) {
      w = w > 1 ? w / 2 : 1;
      h = h > 1 ? h / 2 : 1;
      d = three && d > 1 ? d / 2 : d;
      if (!image_at(0, l + 1)) break;
      max_level = l + 1;
    }
  }
  if (rq.mode == SampleMode::Levels) { out[0] = static_cast<uint32_t>(max_level - base + 1); return; }
  if (rq.mode == SampleMode::Size) {
    const Image* im = image_at(0, base + (rq.level < 0 ? 0 : rq.level));
    if (!im) im = b0;
    out[0] = im->w; out[1] = im->h; out[2] = im->d; out[3] = 0;
    if (t.target == 0x8513) { out[2] = 0; }
    return;
  }
  const bool int_tex = b0->info->store == Store::I32x4 || b0->info->store == Store::U32x4;
  const bool depth_tex = b0->info->store == Store::Depth || b0->info->store == Store::DepthStencil;
  float result[4] = {0, 0, 0, 1};

  // --- texel fetch
  if (rq.mode == SampleMode::Fetch) {
    const Image* im = image_at(0, base + rq.level);
    if (!im) return;
    int32_t x = rq.icoord[0] + rq.offset[0], y = rq.icoord[1] + rq.offset[1], z = rq.icoord[2] + rq.offset[2];
    if (t.target == 0x8C1A || t.target == 0x806F) {}
    if (x < 0 || y < 0 || z < 0 || x >= static_cast<int32_t>(im->w) || y >= static_cast<int32_t>(im->h) || z >= static_cast<int32_t>(im->d)) return;   // out of range: zero
    if (int_tex) { read_texel(*im, x, y, z, out); return; }
    fetch_f(*im, x, y, z, result);
    for (int i = 0; i < 4; ++i) out[i] = f2u(result[i]);
    return;
  }

  // --- choose the level and filter
  float lambda = 0;
  bool have_lod = false;
  if (rq.mode == SampleMode::Lod) { lambda = rq.lod; have_lod = true; }
  else if (rq.has_derivatives) {
    const uint32_t dims = t.target == 0x806F ? 3 : 2;
    float rx = 0, ry = 0;
    const float sz[3] = {static_cast<float>(b0->w), static_cast<float>(b0->h), static_cast<float>(b0->d)};
    for (uint32_t i = 0; i < dims; ++i) {
      rx += rq.ddx[i] * sz[i] * rq.ddx[i] * sz[i];
      ry += rq.ddy[i] * sz[i] * rq.ddy[i] * sz[i];
    }
    float rho = shader::fsqrt(rx > ry ? rx : ry);
    if (is_cube) rho = rho * (sz[0] * 0.5f) / (sz[0] > 0 ? sz[0] : 1);   // (cube derivatives are in direction space: approximate)
    lambda = rho > 0 ? log2f_(rho) : -1000.0f;
    have_lod = true;
  }
  if (rq.mode == SampleMode::Bias) lambda += rq.bias;
  if (rq.mode == SampleMode::Lod) lambda += 0;
  if (lambda < s.min_lod) lambda = s.min_lod;
  if (lambda > s.max_lod) lambda = s.max_lod;
  if (!have_lod) lambda = 0;
  uint32_t filter = lambda <= 0 ? s.mag_filter : s.min_filter;
  if (int_tex) filter = 0x2600;
  const bool linear_in_level = filter == 0x2601 || filter == 0x2701 || filter == 0x2703;
  const bool mip = filter >= 0x2700 && lambda > 0;
  int32_t lv0 = base;
  int32_t lv1 = base;
  float lfrac = 0;
  if (filter >= 0x2700 && have_lod) {
    float ll = lambda < 0 ? 0 : lambda;
    if (filter == 0x2700 || filter == 0x2701) {
      lv0 = base + static_cast<int32_t>(ll + 0.5f);
      if (lv0 > max_level) lv0 = max_level;
      lv1 = lv0;
    } else {
      const float fl = shader::ffloor(ll);
      lv0 = base + static_cast<int32_t>(fl);
      lfrac = ll - fl;
      if (lv0 >= max_level) { lv0 = max_level; lv1 = max_level; lfrac = 0; }
      else lv1 = lv0 + 1;
    }
  } else if (filter >= 0x2700) {
    // no derivatives: the base level, filtered by the within-level part of the min filter
  }
  (void)mip;

  const bool compare_on = rq.compare || (s.compare_mode == 0x884E && depth_tex);
  const uint32_t cfunc = s.compare_func;

  // --- coordinates
  uint32_t face = 0;
  float cs = rq.coord[0], ct = rq.coord[1], cr = rq.coord[2];
  float layer_f = rq.layer;
  if (is_cube) {
    const float ax = rq.coord[0], ay = rq.coord[1], az = rq.coord[2];
    const float aax = ax < 0 ? -ax : ax, aay = ay < 0 ? -ay : ay, aaz = az < 0 ? -az : az;
    float sc, tc, ma;
    if (aax >= aay && aax >= aaz) { ma = aax; if (ax > 0) { face = 0; sc = -az; tc = -ay; } else { face = 1; sc = az; tc = -ay; } }
    else if (aay >= aaz) { ma = aay; if (ay > 0) { face = 2; sc = ax; tc = az; } else { face = 3; sc = ax; tc = -az; } }
    else { ma = aaz; if (az > 0) { face = 4; sc = ax; tc = -ay; } else { face = 5; sc = -ax; tc = -ay; } }
    if (ma == 0) ma = 1;
    cs = 0.5f * (sc / ma + 1.0f);
    ct = 0.5f * (tc / ma + 1.0f);
  }
  if (t.target == 0x8C1A) layer_f = rq.coord[2];

  auto sample_level = [&](int32_t level, bool linear, float res[4]) {
    const Image* im = image_at(face, level);
    if (!im) im = image_at(face, base);
    const int32_t w = static_cast<int32_t>(im->w), h = static_cast<int32_t>(im->h), d = static_cast<int32_t>(im->d);
    const bool is3d = t.target == 0x806F;
    int32_t layer = 0;
    if (t.target == 0x8C1A) {
      layer = static_cast<int32_t>(shader::ffloor(layer_f + 0.5f));
      if (layer < 0) layer = 0;
      if (layer >= d) layer = d - 1;
    }
    auto tap = [&](int32_t x, int32_t y, int32_t z, float o[4]) {
      x = wrap_index(x + rq.offset[0], w, s.wrap_s);
      y = wrap_index(y + rq.offset[1], h, s.wrap_t);
      z = is3d ? wrap_index(z + rq.offset[2], d, s.wrap_r) : (t.faces_in_layers && is_cube ? static_cast<int32_t>(t.layer_offset + face) : layer + static_cast<int32_t>(t.layer_offset));
      if (is_cube) { x = x < 0 ? 0 : (x >= w ? w - 1 : x); y = y < 0 ? 0 : (y >= h ? h - 1 : y); }
      fetch_f(*im, x, y, z, o);
    };
    auto tap_cmp = [&](int32_t x, int32_t y, int32_t z) -> float {
      float o[4];
      tap(x, y, z, o);
      return compare(cfunc, rq.reference, o[0]) ? 1.0f : 0.0f;
    };
    const float u = cs * static_cast<float>(w), v = ct * static_cast<float>(h), r = cr * static_cast<float>(d);
    if (rq.mode == SampleMode::Gather) {
      const int32_t i0 = static_cast<int32_t>(shader::ffloor(u - 0.5f)), j0 = static_cast<int32_t>(shader::ffloor(v - 0.5f));
      const int32_t xs[4] = {i0, i0 + 1, i0 + 1, i0}, ys[4] = {j0 + 1, j0 + 1, j0, j0};
      for (int k = 0; k < 4; ++k) {
        if (compare_on) { res[k] = tap_cmp(xs[k], ys[k], 0); continue; }
        float o[4];
        tap(xs[k], ys[k], 0, o);
        res[k] = o[rq.component & 3];
      }
      return;
    }
    if (!linear) {
      const int32_t i = static_cast<int32_t>(shader::ffloor(u)), j = static_cast<int32_t>(shader::ffloor(v)), k = static_cast<int32_t>(shader::ffloor(r));
      if (compare_on) {
        const float c = tap_cmp(i, j, k);
        res[0] = res[1] = res[2] = c;
        res[3] = 1;
        return;
      }
      tap(i, j, k, res);
      return;
    }
    const float uf = u - 0.5f, vf = v - 0.5f, rf = r - 0.5f;
    const int32_t i0 = static_cast<int32_t>(shader::ffloor(uf)), j0 = static_cast<int32_t>(shader::ffloor(vf)), k0 = static_cast<int32_t>(shader::ffloor(rf));
    const float a = uf - static_cast<float>(i0), b = vf - static_cast<float>(j0), c = rf - static_cast<float>(k0);
    float acc[4] = {0, 0, 0, 0};
    const int zn = is3d ? 2 : 1;
    for (int kz = 0; kz < zn; ++kz)
      for (int jy = 0; jy < 2; ++jy)
        for (int ix = 0; ix < 2; ++ix) {
          const float wgt = (ix ? a : 1 - a) * (jy ? b : 1 - b) * (is3d ? (kz ? c : 1 - c) : 1.0f);
          if (wgt == 0) continue;
          if (compare_on) {
            const float cv = tap_cmp(i0 + ix, j0 + jy, k0 + kz);
            acc[0] += wgt * cv; acc[1] += wgt * cv; acc[2] += wgt * cv; acc[3] += wgt;
          } else {
            float o[4];
            tap(i0 + ix, j0 + jy, k0 + kz, o);
            for (int q = 0; q < 4; ++q) acc[q] += wgt * o[q];
          }
        }
    for (int q = 0; q < 4; ++q) res[q] = acc[q];
  };

  float r0[4], r1[4];
  if (int_tex) {
    // integer textures: nearest at the base level
    const Image* im = image_at(face, base);
    const int32_t w = static_cast<int32_t>(im->w), h = static_cast<int32_t>(im->h);
    int32_t x = wrap_index(static_cast<int32_t>(shader::ffloor(cs * static_cast<float>(w))) + rq.offset[0], w, s.wrap_s);
    int32_t y = wrap_index(static_cast<int32_t>(shader::ffloor(ct * static_cast<float>(h))) + rq.offset[1], h, s.wrap_t);
    int32_t z = 0;
    if (t.target == 0x8C1A) z = static_cast<int32_t>(layer_f + 0.5f);
    else if (t.target == 0x806F) z = wrap_index(static_cast<int32_t>(shader::ffloor(cr * static_cast<float>(im->d))) + rq.offset[2], static_cast<int32_t>(im->d), s.wrap_r);
    if (z < 0) z = 0;
    if (z >= static_cast<int32_t>(im->d)) z = static_cast<int32_t>(im->d) - 1;
    read_texel(*im, x, y, z, out);
    return;
  }
  sample_level(lv0, linear_in_level, r0);
  if (lv1 != lv0 && lfrac > 0) {
    sample_level(lv1, linear_in_level, r1);
    for (int q = 0; q < 4; ++q) result[q] = r0[q] * (1 - lfrac) + r1[q] * lfrac;
  } else {
    for (int q = 0; q < 4; ++q) result[q] = r0[q];
  }
  if (depth_tex && !compare_on) { result[1] = 0; result[2] = 0; result[3] = 1; }
  for (int i = 0; i < 4; ++i) out[i] = f2u(result[i]);
}

}  // namespace webgl
