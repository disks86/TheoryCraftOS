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

// WebGPU render passes on the 3D hardware (see Accelerator.hpp). The unit is the pass: when a pass clears its one color attachment (an 8-bit RGBA/BGRA texture, not
// sRGB, not multisampled) and has at most a depth attachment that nothing can look at afterwards, the device gets a cleared picture, every draw the hardware can
// take is drawn on it, and the picture is read back into the texture once, when the pass ends. A draw the hardware cannot take (a texture or storage buffer in the
// shaders, a loop, a stencil, points or lines, a blend mode the device lacks ...) reads the picture back at that point and the rest of the pass runs on the CPU
// rasterizer on top of it - except for the depth buffer, which cannot be read back, so a CPU draw after hardware draws in the same pass does not see their depth.
// A pass that loads its color attachment is entirely the CPU's: a device image cannot be written from the CPU.

#include "WebGl/GpuDevice.hpp"

namespace wgpu {

namespace {
inline uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }
}  // namespace

void Device::hw_release() {
  if (hw_) hw_textures_.release(*hw_);
  if (hw_ && hw_target_) hw_->destroy_target(hw_target_);
  hw_ = nullptr;
  hw_target_ = 0;
  hw_w_ = hw_h_ = 0;
  hw_pass_ = hw_ahead_ = false;
}

void Device::hw_begin(const RenderPassDesc& d) {
  hw_pass_ = hw_ahead_ = hw_depth_ = false;
  IAccelerator* acc = webgl::accelerator();
  if (!acc || d.colors.count() != 1 || d.occlusion_set) return;
  const ColorAttachment& c = d.colors[0];
  if (c.load || c.resolve) return;
  const TextureView* v = view(c.view);
  const Texture* t = v ? texture(v->texture) : nullptr;
  if (!t || t->destroyed || t->samples != 1 || t->dimension != 1 || t->depth != 1 || v->base_layer != 0 || v->base_mip >= t->mips.count() || !t->format) return;
  if (t->format->gl != 0x8058) return;   // RGBA8 / BGRA8 Unorm (the sRGB ones convert on write, which the device would not)
  const webgl::Image* im = t->mips[v->base_mip].get();
  if (!im || !im->info || im->info->store != webgl::Store::U8x4 || im->ss != 1 || im->d != 1 || im->px.size() != uint64_t{im->w} * im->h * 4) return;
  bool depth = false;
  if (d.depth_view) {
    const TextureView* dv = view(d.depth_view);
    const Texture* dt = dv ? texture(dv->texture) : nullptr;
    if (!dt || dt->destroyed || !dt->format || !dt->format->depth || dt->format->stencil || d.depth_load || d.depth_read_only || dt->samples != 1) return;
    // the device depth cannot be read back: nobody may be able to look at the texture's depth afterwards (copy source 1, texture binding 4)
    if (dt->usage & 5u) return;
    if (dv->base_mip >= dt->mips.count() || dt->mips[dv->base_mip]->w != im->w || dt->mips[dv->base_mip]->h != im->h) return;
    depth = true;
  }
  if (hw_ != acc || hw_w_ != im->w || hw_h_ != im->h) {
    hw_release();
    hw_target_ = acc->create_target(im->w, im->h);
    if (!hw_target_) return;
    hw_ = acc;
    hw_w_ = im->w;
    hw_h_ = im->h;
  }
  if (!acc->clear(hw_target_, true, c.clear, depth, d.depth_clear)) return;
  hw_pass_ = true;
  hw_depth_ = depth;
  hw_view_ = c.view;
}

// The device picture into the color attachment's texture.
void Device::hw_sync() {
  if (!hw_ahead_) return;
  hw_ahead_ = false;
  const TextureView* v = view(hw_view_);
  Texture* t = v ? texture_mut(v->texture) : nullptr;
  if (!t || t->destroyed || v->base_mip >= t->mips.count() || !hw_) return;
  webgl::Image& im = *t->mips[v->base_mip];
  ByteView dst = im.px.writable_view();
  if (im.w != hw_w_ || im.h != hw_h_ || dst.size() != uint64_t{hw_w_} * hw_h_ * 4) return;
  hw_->read_color(hw_target_, dst);
}

void Device::hw_end() {
  if (hw_pass_) hw_sync();
  hw_pass_ = hw_ahead_ = false;
}

// The texture a bound view and sampler make, as the device can read it: one mip level of a single-layer 2D RGBA8 (not sRGB) texture, sampled with a plain
// nearest or linear filter and wrap or clamp addressing.
bool Device::hw_texture(const Bound& b, uint32_t* handle, webgl::AccelSampler* sampler) {
  constexpr uint32_t kMaxSide = 1024;
  const TextureView* v = b.view;
  const Texture* t = b.tex;
  const Sampler* s = b.sampler;
  if (!hw_ || !v || !t || !s || t->destroyed || !t->format || t->format->gl != 0x8058) return false;
  if (t->samples != 1 || t->dimension != 1 || t->depth != 1 || v->dim != 1 || v->mip_count != 1 || v->base_layer != 0 || v->aspect != 0 || v->base_mip >= t->mips.count()) return false;
  if (v->texture != 0 && view(hw_view_) && v->texture == view(hw_view_)->texture) return false;   // the picture being drawn: the texture is not up to date
  if (s->comparison || s->anisotropy > 1 || s->params.compare_mode != 0) return false;
  auto linear = [](uint32_t filter, bool* out) {
    *out = filter == 0x2601 || filter == 0x2701 || filter == 0x2703;
    return filter >= 0x2600 && filter <= 0x2703 && (filter <= 0x2601 || filter >= 0x2700);
  };
  bool min_linear, mag_linear;
  if (!linear(s->params.min_filter, &min_linear) || !linear(s->params.mag_filter, &mag_linear) || min_linear != mag_linear) return false;
  auto wrap = [](uint32_t mode, bool* repeat) {
    *repeat = mode == 0x2901;
    return mode == 0x2901 || mode == 0x812F;
  };
  if (!wrap(s->params.wrap_s, &sampler->repeat_s) || !wrap(s->params.wrap_t, &sampler->repeat_t)) return false;
  sampler->linear = min_linear;
  const webgl::Image* im = t->mips[v->base_mip].get();
  if (!im || !im->info || im->info->store != webgl::Store::U8x4 || im->info->srgb || im->ss != 1 || im->d != 1 || im->w > kMaxSide || im->h > kMaxSide) return false;
  *handle = hw_textures_.get(*hw_, (uint64_t{v->texture} << 32) | v->base_mip, im->w, im->h, im->px.view());
  return *handle != 0;
}

bool Device::hw_program(RenderPipeline& rp, IAccelerator& acc) {
  if (rp.accel_tried) return rp.accel_program != 0;
  rp.accel_tried = true;
  rp.accel = &acc;
  if (!rp.vs || !rp.fs || rp.vs_fn < 0 || rp.fs_fn < 0) return false;
  UniquePointer<TgsiProgram> t = UniquePointer<TgsiProgram>::make(a_, a_);
  if (!t || !webgl::translate_wgsl_to_tgsi(a_, *rp.vs, rp.vs_fn, *rp.fs, rp.fs_fn, t.get())) return false;
  rp.accel_program = acc.create_program(t->vs.c_str(), t->fs.c_str(), static_cast<uint32_t>(t->attributes.count()), t->varyings, static_cast<uint32_t>(t->samplers.count()));
  if (rp.accel_program) rp.tgsi = static_cast<UniquePointer<TgsiProgram>&&>(t);
  return rp.accel_program != 0;
}

// Tries to draw on the hardware; false (nothing drawn) sends the draw to the CPU.
bool Device::hw_draw(RenderPipeline& rp, uint32_t count, uint32_t first, int32_t base_vertex, uint32_t instances, uint32_t first_instance, bool indexed) {
  IAccelerator* acc = hw_;
  if (!acc || !hw_pass_ || !pass_storage_ || instances != 1 || first_instance != 0 || (rp.topology != 3 && rp.topology != 4)) return false;
  if (!rp.fs || rp.sample_count != 1 || rp.unclipped_depth || rp.targets.count() != 1 || rp.targets[0].write_mask != 15 || occlusion_index_ != 0xFFFFFFFFu) return false;
  const ColorTarget& ct = rp.targets[0];
  if (ct.blend) {
    const bool over = ct.color.op == 0 && ct.color.src == 4 && ct.color.dst == 5;
    const bool same = ct.alpha.op == ct.color.op && ct.alpha.src == ct.color.src && ct.alpha.dst == ct.color.dst;
    if (!over || !same) return false;
  }
  AccelState st;
  st.depth_test = st.depth_write = false;
  if (hw_depth_) {
    if (!rp.has_depth) return false;
    const DepthStencilState& ds = rp.ds;
    const bool stencil_off = ds.front.compare == 7 && ds.back.compare == 7 && ds.front.fail == 0 && ds.front.depth_fail == 0 && ds.front.pass == 0 && ds.back.fail == 0 &&
                             ds.back.depth_fail == 0 && ds.back.pass == 0;
    if (!stencil_off || ds.bias != 0 || ds.bias_slope != 0) return false;
    if (ds.depth_compare == 1 || ds.depth_compare == 3) st.depth_test = true;   // less is taken as less-equal: they differ only for equal depths
    else if (ds.depth_compare != 7) return false;
    st.depth_write = ds.depth_write;
  } else if (rp.has_depth) {
    return false;
  }
  if (!hw_program(rp, *acc) || !rp.tgsi) return false;
  const TgsiProgram& t = *rp.tgsi;
  const uint32_t nattr = static_cast<uint32_t>(t.attributes.count());
  if (nattr == 0 || nattr > webgl::kTgsiMaxAttributes) return false;

  // the vertices of the draw, as a triangle list of vertex numbers
  List<int64_t> tris(a_);
  const uint8_t* idx = nullptr;
  const uint32_t index_size = ibuf_format_ == 0 ? 2 : 4;
  const uint32_t restart = ibuf_format_ == 0 ? 0xFFFF : 0xFFFFFFFFu;
  const bool strip = rp.topology == 4;
  if (indexed) {
    const Buffer* ib = buf(ibuf_.buffer);
    if (!ib || ib->destroyed || ibuf_.offset > ib->data.size() || (first + uint64_t{count}) * index_size > ib->data.size() - ibuf_.offset) return false;
    idx = ib->data.view().data() + ibuf_.offset;
  }
  auto vertex_at = [&](uint32_t i) -> int64_t {   // -1: a primitive restart
    if (!indexed) return int64_t{first} + i;
    uint32_t raw = 0;
    if (index_size == 2) {
      uint16_t r;
      __builtin_memcpy(&r, idx + (first + i) * 2u, 2);
      raw = r;
    } else {
      __builtin_memcpy(&raw, idx + (first + i) * 4u, 4);
    }
    if (strip && raw == restart) return -1;
    return static_cast<int64_t>(raw) + base_vertex;
  };
  auto tri = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
    const int64_t a0 = vertex_at(i0), a1 = vertex_at(i1), a2 = vertex_at(i2);
    if (a0 < 0 || a1 < 0 || a2 < 0) return true;
    return tris.push_back(a0) && tris.push_back(a1) && tris.push_back(a2);
  };
  bool ok = true;
  if (!strip) {
    for (uint32_t i = 0; i + 2 < count && ok; i += 3) ok = tri(i, i + 1, i + 2);
  } else {
    uint32_t start = 0;
    for (uint32_t i = 0; i <= count && ok; ++i) {
      if (i < count && vertex_at(i) >= 0) {
        if (i >= start + 2) ok = ((i - start - 2) & 1) ? tri(i - 1, i - 2, i) : tri(i - 2, i - 1, i);
      } else {
        start = i + 1;
      }
      if (tris.count() > (3u << 20)) ok = false;
    }
  }
  if (!ok) return false;
  if (tris.count() == 0) return true;

  // the uniforms: bind the groups like the CPU path does and read the CONST rows from the loaded regions
  bound_.clear();
  List<uint32_t> vregion(a_), fregion(a_);
  if (!setup_stage(*rp.vs, &vregion, nullptr) || !setup_stage(*rp.fs, &fregion, nullptr)) return false;
  ByteBuffer constants(a_);
  for (uint32_t row = 0; row < t.const_rows; ++row) {
    uint32_t lanes[4] = {0, 0, 0, 0};
    for (const TgsiConstant& c : t.constants) {
      if (c.row != row) continue;
      const List<uint32_t>& region = c.stage == 0 ? vregion : fregion;
      for (uint32_t i = 0; i < c.comps && i < 4; ++i)
        if (c.slot + i < region.count()) lanes[i] = region[c.slot + i];
    }
    for (uint32_t i = 0; i < 4; ++i)
      if (!constants.put32(lanes[i])) return false;
  }

  // the textures: the handles the bind groups gave the fragment shader's bindings
  webgl::AccelState tex_state;
  tex_state.texture_count = static_cast<uint32_t>(t.samplers.count());
  if (tex_state.texture_count > webgl::kAccelMaxTextures) return false;
  for (uint32_t k = 0; k < tex_state.texture_count; ++k) {
    const webgl::TgsiSampler& ts = t.samplers[k];
    if (!ts.wgsl || ts.slot >= fregion.count() || ts.sampler_slot >= fregion.count()) return false;
    const uint32_t th = fregion[ts.slot], sh = fregion[ts.sampler_slot];
    if (th == 0 || th > bound_.count() || sh == 0 || sh > bound_.count()) return false;
    Bound b = bound_[th - 1];
    b.sampler = bound_[sh - 1].sampler;
    if (!hw_texture(b, &tex_state.textures[k], &tex_state.samplers[k])) return false;
  }

  // the vertex data: four float lanes per attribute (a location no buffer feeds reads 0, 0, 0, 1 like the CPU path)
  ByteBuffer vertices(a_);
  for (uint64_t vi = 0; vi < tris.count(); ++vi) {
    for (uint32_t k = 0; k < nattr; ++k) {
      uint32_t lanes[4] = {0, 0, 0, f2u(1.0f)};
      for (uint64_t bi = 0; bi < rp.buffers.count(); ++bi) {
        const VertexBufferLayout& layout = *rp.buffers[bi];
        for (const VertexAttribute& at : layout.attributes.view()) {
          if (at.shader_location != t.attributes[k].slot) continue;
          if (layout.instance) return false;
          const Buffer* b = buf(vbuf_[bi].buffer);
          const uint64_t pos = vbuf_[bi].offset + static_cast<uint64_t>(tris[vi]) * layout.stride + at.offset;
          if (!b || b->destroyed || tris[vi] < 0 || pos + vertex_attribute_bytes(at.format) > b->data.size()) return false;
          bool ints;
          decode_vertex_attribute(at.format, b->data.view().data() + pos, lanes, &ints);
          if (ints) return false;
        }
      }
      for (uint32_t i = 0; i < 4; ++i)
        if (!vertices.put32(lanes[i])) return false;
    }
  }

  // the state; y is flipped into window space (row 0 is the top), which mirrors the winding
  const float vx = have_viewport_ ? viewport_[0] : 0, vy = have_viewport_ ? viewport_[1] : 0;
  st.vp_x = vx;
  st.vp_y = vy;
  st.vp_w = have_viewport_ ? viewport_[2] : static_cast<float>(hw_w_);
  st.vp_h = have_viewport_ ? viewport_[3] : static_cast<float>(hw_h_);
  st.depth_near = have_viewport_ ? viewport_[4] : 0;
  st.depth_far = have_viewport_ ? viewport_[5] : 1;
  st.scissor = have_scissor_;
  if (have_scissor_) {
    st.sc_x = static_cast<int32_t>(scissor_[0]);
    st.sc_y = static_cast<int32_t>(scissor_[1]);
    st.sc_w = scissor_[2];
    st.sc_h = scissor_[3];
  }
  st.cull = rp.cull == 1 ? 1 : (rp.cull == 2 ? 2 : 0);
  st.front_ccw = !rp.front_ccw;
  st.blend = ct.blend;
  st.texture_count = tex_state.texture_count;
  for (uint32_t k = 0; k < st.texture_count; ++k) {
    st.textures[k] = tex_state.textures[k];
    st.samplers[k] = tex_state.samplers[k];
  }
  if (!acc->draw(hw_target_, rp.accel_program, st, vertices.view(), static_cast<uint32_t>(tris.count()), constants.view())) return false;
  hw_ahead_ = true;
  return true;
}

}  // namespace wgpu
