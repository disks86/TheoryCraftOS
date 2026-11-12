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
inline float u2f(uint32_t u) { return __builtin_bit_cast(float, u); }
inline uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }
constexpr uint32_t kMaxAccelVertices = 3u << 20;   // vertices of one draw the accelerated path assembles (more run on the CPU)
}  // namespace

void GlContext::hw_forget_texture(uint32_t name) {
  if (hw_owner_) hw_textures_.forget(*hw_owner_, name);
}

// The texture bound to `unit` as the device can read sampler `ts`: a 2D (or 1D, rectangle) texture with no mipmapping in use and a filter and wrap mode the device has, of
// 8-bit RGBA texels - or, for a shadow sampler, of depth, sent as a float texture with the depth in R and read with nearest filtering (the shader filters and
// compares, by `params`: the sampler's four CONST rows, see TgsiSampler). Its texels are on the device when this returns true. False: the CPU samples it.
bool GlContext::hw_texture(const TgsiSampler& ts, uint32_t unit, uint32_t* handle, AccelSampler* sampler, float params[16]) {
  constexpr uint32_t kMaxSide = 1024;
  if (unit >= kMaxTextureUnits) return false;
  const TextureUnit& u = units_[unit];
  const uint32_t name = ts.dim == shader::kDim1D ? u.tex1d : (ts.dim == shader::kDimRect ? u.rect : u.tex2d);
  const uint32_t target = ts.dim == shader::kDim1D ? 0x0DE0 : (ts.dim == shader::kDimRect ? 0x84F5 : 0x0DE1);
  const Texture* t = texture_at(name);
  if (!t || t->target != target || t->base_level != 0) return false;
  SamplerObj so;
  if (u.sampler && u.sampler <= objects_->samplers.count() && objects_->samplers[u.sampler - 1]) so = *objects_->samplers[u.sampler - 1];
  else {
    so.min_filter = t->min_filter;
    so.mag_filter = t->mag_filter;
    so.wrap_s = t->wrap_s;
    so.wrap_t = t->wrap_t;
    so.compare_mode = t->compare_mode;
    so.compare_func = t->compare_func;
  }
  const bool nearest = so.min_filter == 0x2600 && so.mag_filter == 0x2600, linear = so.min_filter == 0x2601 && so.mag_filter == 0x2601;
  if (!nearest && !linear) return false;   // (a mipmapped minification filter, or different filters for the two)
  auto wrap = [](uint32_t mode, bool* repeat) {
    *repeat = mode == 0x2901;
    return mode == 0x2901 || mode == 0x812F;
  };
  if (!wrap(so.wrap_s, &sampler->repeat_s) || !wrap(so.wrap_t, &sampler->repeat_t)) return false;
  if (!ts.shadow && so.compare_mode != 0) return false;      // (a comparison through a sampler that is not a shadow one: the CPU's)
  if (ts.dim == shader::kDim1D) sampler->repeat_t = true;
  sampler->linear = linear && !ts.shadow;
  bool cube;
  if (!texture_complete(*t, so, &cube) || t->images.count() == 0 || !t->images[0] || !t->images[0]->defined()) return false;
  const Image& im = *t->images[0];
  if (im.w > kMaxSide || im.h > kMaxSide || im.d != 1 || im.ss != 1) return false;
  IAccelerator* acc = hw_owner_;
  if (!acc) return false;
  if (ts.params >= 0) {
    for (uint32_t i = 0; i < 16; ++i) params[i] = 0;
    // row 0: the weights of reference < depth, ==, > (GL_TEXTURE_COMPARE_FUNC less the value of GL_NEVER is those three bits: LESS 1, EQUAL 2, LEQUAL 3 ... ALWAYS 7),
    // and linear filtering
    const uint32_t bits = so.compare_func >= 0x0200 && so.compare_func <= 0x0207 ? so.compare_func - 0x0200 : 3;
    params[0] = bits & 1 ? 1.0f : 0.0f;
    params[1] = bits & 2 ? 1.0f : 0.0f;
    params[2] = bits & 4 ? 1.0f : 0.0f;
    params[3] = linear ? 1.0f : 0.0f;
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
  if (!ts.shadow) {
    if (im.info->store != Store::U8x4 || im.info->srgb || im.info->is_signed) return false;
    *handle = hw_textures_.get(*acc, name, im.w, im.h, im.px.view());
    return *handle != 0;
  }
  // a shadow sampler: the depth texels as floats
  if ((im.info->store != Store::Depth && im.info->store != Store::DepthStencil) || !acc->limits().float_textures) return false;
  ByteBuffer floats(a_);
  for (uint32_t y = 0; y < im.h; ++y)
    for (uint32_t x = 0; x < im.w; ++x) {
      uint32_t raw[4];
      read_texel(im, x, y, 0, raw);
      if (!floats.put32(raw[0]) || !floats.put32(0) || !floats.put32(0) || !floats.put32(f2u(1.0f))) return false;
    }
  *handle = hw_textures_.get(*acc, name, im.w, im.h, floats.view(), true);
  return *handle != 0;
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
  hw_valid_ = acc->clear(hw_target_, true, rgba, clear_depth, d);
  hw_ahead_ = false;   // def_color_ was just cleared to the same color
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
  TgsiStages stages;
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
  if (!target.is_default || draw_fb_ != 0) return no("a framebuffer object");
  if (!default_draw_back_ || target.color[0] != dcolor_ || target.color_count != 1 || !dcolor_->defined() || desktop.front_also) return no("not one default colour buffer");
  if (instances != 1) return no("instancing");
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
    if (desktop.per_buffer_masks || desktop.per_buffer_blend) return no("per-buffer blending or masks");
  }
  if (stencil_test_) return no("the stencil test");
  if (offset_fill_ || (desktop.active && (desktop.offset_line || desktop.offset_point))) return no("polygon offset");
  if (alpha_to_coverage_ || sample_coverage_en_) return no("sample coverage");
  const uint8_t color_mask = static_cast<uint8_t>((cmask_[0] ? 1 : 0) | (cmask_[1] ? 2 : 0) | (cmask_[2] ? 4 : 0) | (cmask_[3] ? 8 : 0));
  const bool depth = ddepth_->defined();
  const bool depth_test = depth_test_ && depth;
  // Blending, the colour mask and the depth function: the device's own state when it has it; else only source over destination, every channel and LEQUAL
  // (LESS is taken as LEQUAL: they differ only for equal depths).
  bool custom = false;
  uint8_t factors[4] = {1, 0, 1, 0}, ops[2] = {0, 0};
  if (limits.blend_state) {
    // GL's factors and equations as the device's (graphics::BlendFactor / BlendOp numbers); a constant colour has no device factor
    const auto factor = [](uint32_t f, uint8_t* out) {
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
    };
    const auto equation = [](uint32_t e, uint8_t* out) {
      switch (e) {
        case 0x8006: *out = 0; return true;
        case 0x800A: *out = 1; return true;
        case 0x800B: *out = 2; return true;
        case 0x8007: *out = 3; return true;
        case 0x8008: *out = 4; return true;
        default: return false;
      }
    };
    if (blend_) {
      if (!factor(blend_sr_, &factors[0]) || !factor(blend_dr_, &factors[1]) || !factor(blend_sa_, &factors[2]) || !factor(blend_da_, &factors[3])) return no("a blend factor");
      if (!equation(blend_eq_rgb_, &ops[0]) || !equation(blend_eq_a_, &ops[1])) return no("a blend equation");
      // (a drawing buffer without alpha reads its alpha as 1: the device's image keeps whatever was written there)
      if (!attrs_.alpha)
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
      if (!add || !over || (attrs_.alpha && !alpha_same)) return no("a blend function");
    }
  }
  if (!hw_program(p, *acc) || !p.tgsi) return no(p.accel_why ? p.accel_why : "the shaders");
  if (!hw_target(acc)) return no("no device target");
  if (!hw_valid_) return no("the device picture is out of date");
  const TgsiProgram& t = *p.tgsi;
  if (t.uses_frag_coord && target.ss != 1) return no("gl_FragCoord on an antialiased drawing buffer");

  // the attributes of the translated shader, in the order of its IN[] registers
  Vao* vao = current_vao_ptr();
  const VertexAttrib* attribs[kTgsiMaxAttributes] = {};
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
    if (attribs[k]->divisor) return no("an instanced attribute");
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

  // vertex data: four floats per attribute per vertex
  ByteBuffer vertices(a_);
  for (uint64_t v = 0; v < tris.count(); ++v) {
    const uint32_t index = static_cast<uint32_t>(tris[v]);
    for (uint32_t k = 0; k < nattr; ++k) {
      if (!attribs[k]) {
        const uint32_t x = t.attributes[k].slot == kTgsiVertexId ? f2u(static_cast<float>(index)) : 0;
        if (!vertices.put32(x)) return no("out of memory");
        for (uint32_t i = 1; i < 4; ++i)
          if (!vertices.put32(0)) return no("out of memory");
        continue;
      }
      const VertexAttrib& at = *attribs[k];
      uint32_t lanes[4];
      if (at.enabled) {
        const Buffer* b = buffer_at(at.buffer);
        const uint32_t esize = vertex_attrib_element_bytes(at);
        const uint32_t stride = at.stride ? at.stride : esize;
        if (!b || at.offset + uint64_t{index} * stride + esize > b->data.size()) return no("an attribute past its buffer");
        if (t.attributes[k].convert == kTgsiAsIs) {
          read_vertex_attrib(at, b->data.view().data() + at.offset + uint64_t{index} * stride, lanes);
        } else {      // an integer input: read as the CPU reads it (integers), sent as their float values
          read_vertex_attrib_int(at, b->data.view().data() + at.offset + uint64_t{index} * stride, lanes);
          for (uint32_t& l : lanes) l = f2u(t.attributes[k].convert == kTgsiFromUint ? static_cast<float>(l) : static_cast<float>(static_cast<int32_t>(l)));
        }
      } else if (t.attributes[k].convert != kTgsiAsIs) {
        for (int i = 0; i < 4; ++i) lanes[i] = f2u(static_cast<float>(at.cur_is_int ? at.cur_i[i] : static_cast<int32_t>(at.cur_f[i])));
      } else {
        for (int i = 0; i < 4; ++i) lanes[i] = f2u(at.cur_is_int ? static_cast<float>(at.cur_i[i]) : at.cur_f[i]);
      }
      for (uint32_t i = 0; i < 4; ++i)
        if (!vertices.put32(lanes[i])) return no("out of memory");
    }
  }

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
        // (the device keeps integers and booleans as float values)
        switch (c.convert) {
          case kTgsiFromInt: lanes[i] = f2u(static_cast<float>(static_cast<int32_t>(bits))); break;
          case kTgsiFromUint: lanes[i] = f2u(static_cast<float>(bits)); break;
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
    if (t.samplers[k].slot >= p.fs_shared.count() || !hw_texture(t.samplers[k], p.fs_shared[t.samplers[k].slot], &st.textures[k], &st.samplers[k], params))
      return no("a texture the device cannot sample");
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
  st.blend = blend_;
  st.topology = topology;
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
    st.color_mask = color_mask;
    st.depth_compare = static_cast<uint8_t>(dfunc_ >= 0x0200 && dfunc_ <= 0x0207 ? dfunc_ - 0x0200 : 3);
  }
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
