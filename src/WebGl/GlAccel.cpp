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

namespace webgl {

namespace {
inline float u2f(uint32_t u) { return __builtin_bit_cast(float, u); }
inline uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }
constexpr uint32_t kMaxAccelVertices = 3u << 20;   // vertices of one draw the accelerated path assembles (more run on the CPU)
}  // namespace

void GlContext::hw_forget_texture(uint32_t name) {
  if (hw_owner_) hw_textures_.forget(*hw_owner_, name);
}

// The texture bound to `unit` as the device can read it: a 2D texture of 8-bit RGBA texels with no mipmapping in use and a filter and wrap mode the device has.
// Its texels are on the device when this returns true. False: the CPU samples it.
bool GlContext::hw_texture(uint32_t unit, uint32_t* handle, AccelSampler* sampler) {
  constexpr uint32_t kMaxSide = 1024;
  if (unit >= kMaxTextureUnits) return false;
  const TextureUnit& u = units_[unit];
  const Texture* t = texture_at(u.tex2d);
  if (!t || t->target != 0x0DE1 || t->base_level != 0) return false;
  SamplerObj so;
  if (u.sampler && u.sampler <= samplers_.count() && samplers_[u.sampler - 1]) so = *samplers_[u.sampler - 1];
  else {
    so.min_filter = t->min_filter;
    so.mag_filter = t->mag_filter;
    so.wrap_s = t->wrap_s;
    so.wrap_t = t->wrap_t;
    so.compare_mode = t->compare_mode;
  }
  const bool nearest = so.min_filter == 0x2600 && so.mag_filter == 0x2600, linear = so.min_filter == 0x2601 && so.mag_filter == 0x2601;
  if (!nearest && !linear) return false;   // (a mipmapped minification filter, or different filters for the two)
  auto wrap = [](uint32_t mode, bool* repeat) {
    *repeat = mode == 0x2901;
    return mode == 0x2901 || mode == 0x812F;
  };
  if (!wrap(so.wrap_s, &sampler->repeat_s) || !wrap(so.wrap_t, &sampler->repeat_t) || so.compare_mode != 0) return false;
  sampler->linear = linear;
  bool cube;
  if (!texture_complete(*t, so, &cube) || t->images.count() == 0 || !t->images[0] || !t->images[0]->defined()) return false;
  const Image& im = *t->images[0];
  if (im.info->store != Store::U8x4 || im.info->srgb || im.info->is_signed || im.w > kMaxSide || im.h > kMaxSide || im.d != 1 || im.ss != 1) return false;
  IAccelerator* acc = hw_owner_;
  if (!acc) return false;
  *handle = hw_textures_.get(*acc, u.tex2d, im.w, im.h, im.px.view());
  return *handle != 0;
}

void GlContext::hw_drop() {
  if (hw_owner_) hw_textures_.release(*hw_owner_);
  if (hw_owner_ && hw_target_) hw_owner_->destroy_target(hw_target_);
  hw_owner_ = nullptr;
  hw_target_ = 0;
  hw_valid_ = false;
  hw_ahead_ = false;
}

void GlContext::hw_sync() {
  if (!hw_ahead_) return;
  hw_ahead_ = false;
  const uint64_t bytes = uint64_t{def_color_.w} * def_color_.h * 4;
  ByteView dst = def_color_.px.writable_view();
  if (!hw_owner_ || !hw_target_ || dst.size() != bytes || !hw_owner_->read_color(hw_target_, dst)) {
    hw_valid_ = false;   // the device would not give the picture back: what the CPU has is the best there is
    return;
  }
  if (!attrs_.alpha)
    for (uint64_t i = 3; i < bytes; i += 4) dst[i] = 255;
  all_dirty_ = true;
}

bool GlContext::hw_target(IAccelerator* acc) {
  if (hw_owner_ != acc) hw_drop();
  if (hw_target_) return true;
  if (!def_color_.defined()) return false;
  hw_target_ = acc->create_target(def_color_.w, def_color_.h);
  if (!hw_target_) return false;
  hw_owner_ = acc;
  hw_valid_ = false;
  hw_ahead_ = false;
  return true;
}

// Called after a clear of the default framebuffer ran on the CPU: if it covered the drawing buffer, the device is cleared the same way and the two agree.
void GlContext::hw_after_clear(uint32_t mask, bool whole) {
  IAccelerator* acc = accelerator();
  const bool color = (mask & 0x4000) != 0 && cmask_[0] && cmask_[1] && cmask_[2] && cmask_[3];
  const bool depth = def_depth_.defined();
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
  hw_valid_ = acc->clear(hw_target_, true, rgba, clear_depth, d);
  hw_ahead_ = false;   // def_color_ was just cleared to the same color
}

bool GlContext::hw_program(ProgramObj& p, IAccelerator& acc) {
  if (p.accel_tried) return p.accel_program != 0;
  p.accel_tried = true;
  p.accel = &acc;
  if (!p.vsm || !p.fsm) return false;
  List<TgsiLink> links(a_);
  for (const VaryingLink& l : p.links) {
    if (l.flat || !links.push_back(TgsiLink{l.vs_slot, l.fs_slot, l.count})) return false;
  }
  UniquePointer<TgsiProgram> t = UniquePointer<TgsiProgram>::make(a_, a_);
  if (!t || !translate_to_tgsi(a_, *p.vsm, *p.fsm, links.view(), t.get())) return false;
  p.accel_program = acc.create_program(t->vs.c_str(), t->fs.c_str(), static_cast<uint32_t>(t->attributes.count()), t->varyings, static_cast<uint32_t>(t->samplers.count()));
  if (p.accel_program) p.tgsi = static_cast<UniquePointer<TgsiProgram>&&>(t);
  return p.accel_program != 0;
}

// Tries to run a draw on the accelerator. False: not done (nothing was drawn), the caller draws on the CPU.
bool GlContext::hw_draw(ProgramObj& p, const Target& target, uint32_t mode, int32_t first, int32_t count, uint32_t index_type, const uint8_t* idx, bool indexed,
                        int32_t instances) {
  IAccelerator* acc = accelerator();
  if (!acc || !target.is_default || draw_fb_ != 0 || !default_draw_back_ || instances != 1 || mode < 4 || mode > 6) return false;
  if (target.color[0] != &def_color_ || target.color_count != 1 || !def_color_.defined() || tf_active_ || rasterizer_discard_) return false;
  if (query_active_[0] || query_active_[1]) return false;
  if (stencil_test_ || offset_fill_ || alpha_to_coverage_ || sample_coverage_en_) return false;
  if (!(cmask_[0] && cmask_[1] && cmask_[2] && cmask_[3])) return false;
  const bool depth = def_depth_.defined();
  const bool depth_test = depth_test_ && depth;
  if (depth_test && dfunc_ != 0x0201 && dfunc_ != 0x0203) return false;   // LESS is taken as LEQUAL: they differ only for equal depths
  if (blend_) {
    const bool add = blend_eq_rgb_ == 0x8006 && blend_eq_a_ == 0x8006;
    const bool over = blend_sr_ == 0x0302 && blend_dr_ == 0x0303;
    const bool alpha_same = blend_sa_ == blend_sr_ && blend_da_ == blend_dr_;
    if (!add || !over || (attrs_.alpha && !alpha_same)) return false;
  }
  if (!hw_program(p, *acc) || !p.tgsi || !hw_target(acc) || !hw_valid_) return false;
  const TgsiProgram& t = *p.tgsi;

  // the attributes of the translated shader, in the order of its IN[] registers
  Vao* vao = current_vao_ptr();
  const VertexAttrib* attribs[kTgsiMaxAttributes] = {};
  const uint32_t nattr = static_cast<uint32_t>(t.attributes.count());
  if (nattr == 0 || nattr > kTgsiMaxAttributes) return false;
  for (uint32_t k = 0; k < nattr; ++k) {
    const AttribInfo* info = nullptr;
    for (const auto& ai : p.attribs)
      if (ai->slot == t.attributes[k].slot) info = ai.get();
    if (!info || info->location < 0 || static_cast<uint32_t>(info->location) >= kMaxAttribs) return false;
    attribs[k] = &vao->attribs[info->location];
    if (attribs[k]->divisor) return false;
  }

  // the triangles of the draw, as a list of vertex indices (the primitive modes and restart markers of the software path)
  const uint32_t restart = index_type == 0x1401 ? 0xFF : (index_type == 0x1403 ? 0xFFFF : 0xFFFFFFFFu);
  auto index_at = [&](int32_t i) -> int64_t {
    if (!indexed) return int64_t{first} + i;
    const uint32_t v = read_index_value(idx + uint64_t{static_cast<uint32_t>(i)} * (index_type == 0x1401 ? 1 : (index_type == 0x1403 ? 2 : 4)), index_type);
    return v == restart ? -1 : static_cast<int64_t>(v);
  };
  List<int64_t> tris(a_);
  auto tri = [&](int32_t i0, int32_t i1, int32_t i2) {
    const int64_t a0 = index_at(i0), a1 = index_at(i1), a2 = index_at(i2);
    if (a0 < 0 || a1 < 0 || a2 < 0) return true;
    return tris.push_back(a0) && tris.push_back(a1) && tris.push_back(a2);
  };
  bool ok = true;
  if (mode == 4) {
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
  if (!ok) return false;
  if (tris.count() == 0) return true;

  // vertex data: four floats per attribute per vertex
  ByteBuffer vertices(a_);
  for (uint64_t v = 0; v < tris.count(); ++v) {
    const uint32_t index = static_cast<uint32_t>(tris[v]);
    for (uint32_t k = 0; k < nattr; ++k) {
      const VertexAttrib& at = *attribs[k];
      uint32_t lanes[4];
      if (at.enabled) {
        const Buffer* b = buffer_at(at.buffer);
        const uint32_t esize = vertex_attrib_element_bytes(at);
        const uint32_t stride = at.stride ? at.stride : esize;
        if (!b || at.offset + uint64_t{index} * stride + esize > b->data.size()) return false;
        read_vertex_attrib(at, b->data.view().data() + at.offset + uint64_t{index} * stride, lanes);
      } else {
        for (int i = 0; i < 4; ++i) lanes[i] = f2u(at.cur_is_int ? static_cast<float>(at.cur_i[i]) : at.cur_f[i]);
      }
      for (uint32_t i = 0; i < 4; ++i)
        if (!vertices.put32(lanes[i])) return false;
    }
  }

  // uniforms: the CONST rows
  ByteBuffer constants(a_);
  for (uint32_t row = 0; row < t.const_rows; ++row) {
    uint32_t lanes[4] = {0, 0, 0, 0};
    for (const TgsiConstant& c : t.constants) {
      if (c.row != row) continue;
      const List<uint32_t>& shared = c.stage == 0 ? p.vs_shared : p.fs_shared;
      for (uint32_t i = 0; i < c.comps && i < 4; ++i)
        if (c.slot + i < shared.count()) lanes[i] = shared[c.slot + i];
    }
    for (uint32_t i = 0; i < 4; ++i)
      if (!constants.put32(lanes[i])) return false;
  }

  const float ss = static_cast<float>(target.ss);
  AccelState st;
  // the textures: each sampler uniform holds a texture unit
  st.texture_count = static_cast<uint32_t>(t.samplers.count());
  if (st.texture_count > kAccelMaxTextures) return false;
  for (uint32_t k = 0; k < st.texture_count; ++k) {
    if (t.samplers[k].slot >= p.fs_shared.count() || !hw_texture(p.fs_shared[t.samplers[k].slot], &st.textures[k], &st.samplers[k])) return false;
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
  if (st.cull == 3) return true;   // both faces culled: nothing to draw
  st.front_ccw = front_face_ == 0x0901;
  st.depth_test = depth_test;
  st.depth_write = dmask_ && depth;
  st.blend = blend_;
  if (!acc->draw(hw_target_, p.accel_program, st, vertices.view(), static_cast<uint32_t>(tris.count()), constants.view())) {
    hw_valid_ = false;   // the device may have drawn part of it: the caller reads the picture back and goes on on the CPU
    return false;
  }
  hw_ahead_ = true;
  all_dirty_ = true;
  ++version_;
  return true;
}

}  // namespace webgl
