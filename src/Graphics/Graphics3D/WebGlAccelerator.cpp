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

#include "Graphics/Graphics3D/WebGlAccelerator.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"
#include "WebGl/TgsiTranslator.hpp"

namespace graphics {

WebGlAccelerator::WebGlAccelerator(IGraphicsDevice& device, allocators::IAllocator& allocator)
    : blanks_(allocator), device_(device), allocator_(allocator), programs_(allocator), targets_(allocator), textures_(allocator) {}

WebGlAccelerator::~WebGlAccelerator() {
  for (auto& p : programs_)
    if (p) destroy_program_locked(*p);
  for (auto& t : textures_)
    if (t && t->image) device_.destroy_image(t->image);
  for (Blank& b : blanks_) device_.destroy_image(b.image);
  for (auto& t : targets_) {
    if (!t) continue;
    if (t->color) device_.destroy_image(t->color);
    if (t->depth) device_.destroy_image(t->depth);
  }
  if (commands_) device_.destroy_command_buffer(commands_);
  if (vertex_buffer_) device_.destroy_buffer(vertex_buffer_);
}

void WebGlAccelerator::destroy_program_locked(Program& p) {
  for (Variant& v : p.variants)
    if (v.pipeline) device_.destroy_pipeline(v.pipeline);
  p.variants.clear();
  IShaderModule** const modules[5] = {&p.vs, &p.fs, &p.gs, &p.hs, &p.ds};
  for (IShaderModule** m : modules) {
    if (*m) device_.destroy_shader_module(*m);
    *m = nullptr;
  }
  p.attributes = p.varyings = p.textures = 0;
}

// What the device takes: its push constants as CONST rows, its vertex attributes, varyings and texture units.
webgl::AccelLimits WebGlAccelerator::limits() const {
  const DeviceInfo info = device_.info();
  webgl::AccelLimits l;
  // (a device with larger constant blocks: those - the GL path's programs have many uniforms)
  const uint32_t most = info.limits.shader_constant_bytes > info.max_push_constant_bytes ? info.limits.shader_constant_bytes : info.max_push_constant_bytes;
  const uint32_t rows = (most < kMaxShaderConstantBytes ? most : kMaxShaderConstantBytes) / 16;
  l.const_rows = rows < 8 ? 8 : rows;
  l.attributes = info.limits.vertex_attributes < kMaxVertexAttributes ? info.limits.vertex_attributes : kMaxVertexAttributes;
  l.varyings = info.limits.varyings < kMaxVaryings ? info.limits.varyings : kMaxVaryings;
  const uint32_t units = info.limits.texture_units < kMaxTextureUnits ? info.limits.texture_units : kMaxTextureUnits;
  l.textures = units < webgl::kAccelMaxTextures ? units : webgl::kAccelMaxTextures;
  l.blend_state = (info.features & kFeatureBlendState) != 0;
  l.lines = (info.features & kFeatureLines) != 0;
  l.geometry = l.lines && (info.features & kFeatureGeometryShaders) != 0;
  l.tessellation = (info.features & kFeatureTessellation) != 0;
  l.clip_distances = (info.features & kFeatureClipDistances) != 0;
  l.float_textures = (info.features & kFeatureFloatFormats) != 0;
  l.control_varyings = info.limits.control_varyings;
  l.depth_transfer = (info.features2 & kFeature2DepthCopies) != 0;   // (IImage::write_depth_subresource / read_depth_subresource)
  l.color_upload = true;      // (write_color: an image write the device may still refuse, and the context then stays on the CPU)
  // (device images: a pass has at least one colour image; several need kFeatureMultipleTargets)
  l.image_colors = (info.features & kFeatureMultipleTargets) ? (info.limits.color_targets < webgl::kAccelMaxColors ? info.limits.color_targets : webgl::kAccelMaxColors) : 1;
  if (l.image_colors > kMaxColorTargets) l.image_colors = kMaxColorTargets;
  l.float_images = l.float_textures;
  l.srgb_images = (info.features & kFeatureSrgbFormats) != 0;
  l.sampled_depth = info.limits.sampled_depth;
  l.mip_images = true;      // (a device without mipmaps refuses such an image, and the texture is then the CPU's)
  l.instancing = (info.features & kFeatureInstancing) && info.limits.vertex_buffers >= 2;
  l.independent_blend = (info.features & kFeatureIndependentBlend) && l.blend_state;
  l.depth_bias = info.limits.depth_bias;
  l.layered_textures = (info.features & kFeatureTextureKinds) != 0;
  l.stencil = info.limits.stencil;
  return l;
}

uint32_t WebGlAccelerator::create_program(const char* vs_tgsi, const char* fs_tgsi, uint32_t attributes, uint32_t varyings, uint32_t textures) {
  return create_program_stages(vs_tgsi, fs_tgsi, attributes, varyings, textures, webgl::AccelStages());
}

uint32_t WebGlAccelerator::create_program_stages(const char* vs_tgsi, const char* fs_tgsi, uint32_t attributes, uint32_t varyings, uint32_t textures,
                                                 const webgl::AccelStages& stages) {
  const webgl::AccelLimits l = limits();
  if (attributes == 0 || attributes > l.attributes || varyings > l.varyings || textures > l.textures) return 0;
  const bool tess = stages.tcs || stages.tes;
  if ((stages.gs && !l.geometry) || (tess && (!l.tessellation || !stages.tcs || !stages.tes))) return 0;
  if (stages.gs && (stages.gs_input > 4 || stages.gs_output > 2 || stages.gs_max_vertices == 0 || stages.gs_invocations == 0 || stages.gs_invocations > 32)) return 0;
  if (tess && (stages.patch_vertices == 0 || stages.patch_vertices > kMaxPatchControlPoints || stages.tcs_vertices == 0 || stages.tcs_vertices > kMaxPatchControlPoints ||
               stages.tess_domain > 2 || stages.tess_partitioning > 3 || stages.tess_output > 3))
    return 0;
  kernel::ScopedMutex guard(lock_);
  UniquePointer<Program> p = UniquePointer<Program>::make(allocator_, allocator_);
  if (!p) return 0;
  ShaderModuleDesc vd;
  vd.stage = ShaderStage::Vertex;
  vd.tgsi = vs_tgsi;
  vd.tgsi_gl = true;
  ShaderModuleDesc fd;
  fd.stage = ShaderStage::Fragment;
  fd.tgsi = fs_tgsi;
  fd.tgsi_gl = true;
  p->vs = device_.create_shader_module(vd);
  p->fs = p->vs ? device_.create_shader_module(fd) : nullptr;
  if (!p->vs || !p->fs) {   // a device without TGSI refuses here, and the draw stays on the CPU
    destroy_program_locked(*p);
    return 0;
  }
  if (stages.gs) {
    if (!p->gs_text.assign(StringView(stages.gs))) {
      destroy_program_locked(*p);
      return 0;
    }
    ShaderModuleDesc gd;
    gd.stage = ShaderStage::Geometry;
    gd.tgsi = p->gs_text.c_str();
    gd.tgsi_gl = true;
    gd.geometry_input = static_cast<GeometryPrimitive>(stages.gs_input);
    gd.geometry_output = static_cast<GeometryTopology>(stages.gs_output);
    gd.geometry_max_vertices = stages.gs_max_vertices;
    gd.geometry_instances = stages.gs_invocations;
    p->gs = device_.create_shader_module(gd);
    p->gs_input = stages.gs_input;
    if (!p->gs) {
      destroy_program_locked(*p);
      return 0;
    }
  }
  if (tess) {
    if (!p->tcs_text.assign(StringView(stages.tcs)) || !p->tes_text.assign(StringView(stages.tes))) {
      destroy_program_locked(*p);
      return 0;
    }
    ShaderModuleDesc td;      // (both stages say the tessellator's settings, as the device wants)
    td.tess_input_points = stages.patch_vertices;
    td.tess_output_points = stages.tcs_vertices;
    td.tess_domain = static_cast<TessDomain>(stages.tess_domain);
    td.tess_partitioning = static_cast<TessPartitioning>(stages.tess_partitioning);
    td.tess_output = static_cast<TessOutput>(stages.tess_output);
    td.stage = ShaderStage::Hull;
    td.tgsi = p->tcs_text.c_str();
    td.tgsi_gl = true;
    p->hs = device_.create_shader_module(td);
    td.stage = ShaderStage::Domain;
    td.tgsi = p->tes_text.c_str();
    p->ds = p->hs ? device_.create_shader_module(td) : nullptr;
    p->patch_vertices = stages.patch_vertices;
    if (!p->hs || !p->ds) {
      destroy_program_locked(*p);
      return 0;
    }
  }
  p->attributes = attributes;
  p->varyings = varyings;
  p->textures = textures;
  if (textures && !pipeline_for(*p, webgl::AccelState())) {   // a device without textures (or this many) refuses here, and the draw stays on the CPU
    destroy_program_locked(*p);
    return 0;
  }
  for (uint64_t i = 0; i < programs_.count(); ++i)
    if (!programs_[i]) {
      programs_[i] = static_cast<UniquePointer<Program>&&>(p);
      return static_cast<uint32_t>(i + 1);
    }
  if (!programs_.push_back(static_cast<UniquePointer<Program>&&>(p))) return 0;
  return static_cast<uint32_t>(programs_.count());
}

void WebGlAccelerator::destroy_program(uint32_t handle) {
  kernel::ScopedMutex guard(lock_);
  Program* p = program_at(handle);
  if (!p) return;
  destroy_program_locked(*p);
  programs_[handle - 1].reset();
}

uint32_t WebGlAccelerator::create_texture(uint32_t w, uint32_t h) { return make_texture(w, h, webgl::AccelFormat::Rgba8, 1, false); }

uint32_t WebGlAccelerator::create_texture_float(uint32_t w, uint32_t h) {
  if (!(device_.info().features & kFeatureFloatFormats)) return 0;
  return make_texture(w, h, webgl::AccelFormat::Rgba32F, 1, false);
}

uint32_t WebGlAccelerator::create_image(uint32_t w, uint32_t h, webgl::AccelFormat format, uint32_t levels) {
  const uint32_t features = device_.info().features;
  using webgl::AccelFormat;
  if ((format == AccelFormat::Rgba16F || format == AccelFormat::Rgba32F) && !(features & kFeatureFloatFormats)) return 0;
  if (format == AccelFormat::Rgba8Srgb && !(features & kFeatureSrgbFormats)) return 0;
  if (levels != 1 && format != AccelFormat::Rgba8 && format != AccelFormat::Rgba8Srgb) return 0;
  // (a mipmapped image is only sampled; the others can be rendered into as well)
  return make_texture(w, h, format, levels, levels == 1);
}

uint32_t WebGlAccelerator::make_texture(uint32_t w, uint32_t h, webgl::AccelFormat format, uint32_t levels, bool renderable) {
  if (w == 0 || h == 0 || levels == 0 || levels > mip_level_limit({w, h})) return 0;
  kernel::ScopedMutex guard(lock_);
  UniquePointer<Texture> t = UniquePointer<Texture>::make(allocator_);
  if (!t) return 0;
  t->width = w;
  t->height = h;
  t->format = format;
  t->levels = levels;
  t->renderable = renderable;
  ImageDesc d;
  d.extent = {w, h};
  d.mip_levels = levels;
  switch (format) {
    case webgl::AccelFormat::Rgba8: d.format = Format::B8G8R8A8Unorm; break;
    case webgl::AccelFormat::Rgba8Srgb: d.format = Format::B8G8R8A8Srgb; break;
    case webgl::AccelFormat::Rgba16F: d.format = Format::R16G16B16A16Float; break;
    case webgl::AccelFormat::Rgba32F: d.format = Format::R32G32B32A32Float; break;
    case webgl::AccelFormat::Depth: d.format = Format::D32Float; break;
    case webgl::AccelFormat::DepthStencil: d.format = Format::D32Float; d.stencil = true; break;
  }
  if (format == webgl::AccelFormat::Depth || format == webgl::AccelFormat::DepthStencil) {
    if (format == webgl::AccelFormat::DepthStencil && !device_.info().limits.stencil) return 0;
    if (!renderable) return 0;
    // (sampled as well when the device can: a depth texture a later draw reads)
    d.usage = kImageDepthAttachment | (device_.info().limits.sampled_depth ? kImageSampled : 0u);
  } else {
    d.usage = kImageSampled | (renderable ? kImageColorAttachment : 0u);
  }
  t->image = device_.create_image(d);
  if (!t->image) return 0;
  for (uint64_t i = 0; i < textures_.count(); ++i)
    if (!textures_[i]) {
      textures_[i] = static_cast<UniquePointer<Texture>&&>(t);
      return static_cast<uint32_t>(i + 1);
    }
  if (!textures_.push_back(static_cast<UniquePointer<Texture>&&>(t))) return 0;
  return static_cast<uint32_t>(textures_.count());
}

void WebGlAccelerator::destroy_texture(uint32_t handle) {
  kernel::ScopedMutex guard(lock_);
  Texture* t = texture_at(handle);
  if (!t) return;
  if (t->image) device_.destroy_image(t->image);
  textures_[handle - 1].reset();
}

bool WebGlAccelerator::write_texture(uint32_t handle, ConstByteView rgba) {
  kernel::ScopedMutex guard(lock_);
  Texture* t = texture_at(handle);
  if (!t || (t->format != webgl::AccelFormat::Rgba8 && t->format != webgl::AccelFormat::Rgba8Srgb)) return false;
  const uint64_t texels = uint64_t{t->width} * t->height;
  if (rgba.size() != texels * 4) return false;
  ArrayView<uint32_t> scratch = allocate_view<uint32_t>(allocator_, texels);
  if (scratch.empty()) return false;
  for (uint64_t i = 0; i < texels; ++i)   // 0xAARRGGBB
    scratch[i] = (uint32_t{rgba[i * 4 + 3]} << 24) | (uint32_t{rgba[i * 4]} << 16) | (uint32_t{rgba[i * 4 + 1]} << 8) | rgba[i * 4 + 2];
  const bool ok = t->image->write_pixels(scratch);
  release_view(allocator_, scratch);
  return ok;
}

bool WebGlAccelerator::write_texture_float(uint32_t handle, ConstByteView rgba32f) {
  kernel::ScopedMutex guard(lock_);
  Texture* t = texture_at(handle);
  if (!t || (t->format != webgl::AccelFormat::Rgba16F && t->format != webgl::AccelFormat::Rgba32F)) return false;
  const uint64_t texels = uint64_t{t->width} * t->height;
  if (rgba32f.size() != texels * 16) return false;
  ArrayView<Vec4> scratch = allocate_view<Vec4>(allocator_, texels);
  if (scratch.empty()) return false;
  ByteReader r(rgba32f);
  for (Vec4& v : scratch) {
    uint32_t c[4] = {0, 0, 0, 0};
    for (uint32_t& w : c) w = r.le32();      // (the size was checked: every read is inside)
    v.x = __builtin_bit_cast(float, c[0]);
    v.y = __builtin_bit_cast(float, c[1]);
    v.z = __builtin_bit_cast(float, c[2]);
    v.w = __builtin_bit_cast(float, c[3]);
  }
  const bool ok = t->image->write_pixels_float(scratch);
  release_view(allocator_, scratch);
  return ok;
}

uint32_t WebGlAccelerator::create_target(uint32_t w, uint32_t h) {
  if (w == 0 || h == 0) return 0;
  kernel::ScopedMutex guard(lock_);
  UniquePointer<Target> t = UniquePointer<Target>::make(allocator_);
  if (!t) return 0;
  t->width = w;
  t->height = h;
  t->color = device_.create_image({{w, h}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  t->depth = t->color ? device_.create_image({{w, h}, Format::D32Float, kImageDepthAttachment}) : nullptr;
  if (!t->color || !t->depth) {
    if (t->color) device_.destroy_image(t->color);
    return 0;
  }
  for (uint64_t i = 0; i < targets_.count(); ++i)
    if (!targets_[i]) {
      targets_[i] = static_cast<UniquePointer<Target>&&>(t);
      return static_cast<uint32_t>(i + 1);
    }
  if (!targets_.push_back(static_cast<UniquePointer<Target>&&>(t))) return 0;
  return static_cast<uint32_t>(targets_.count());
}

void WebGlAccelerator::destroy_target(uint32_t handle) {
  kernel::ScopedMutex guard(lock_);
  Target* t = target_at(handle);
  if (!t) return;
  if (t->color) device_.destroy_image(t->color);
  if (t->depth) device_.destroy_image(t->depth);
  targets_[handle - 1].reset();
}

IPipeline* WebGlAccelerator::pipeline_for(Program& p, const webgl::AccelState& s, uint32_t color_count) {
  // GL's window rows are the device's rows in the same order (row 0 is the bottom of the GL window, which the device draws as its first row), so the picture is the
  // device's upside down and a counter-clockwise triangle in GL is a clockwise one for the device.
  const uint32_t cull = s.cull > 2 ? 0 : s.cull;
  if (color_count == 0 || color_count > kMaxColorTargets) return nullptr;
  uint64_t key = cull | (s.front_ccw ? 4u : 0u) | (s.depth_test ? 8u : 0u) | (s.depth_write ? 16u : 0u) | (s.blend ? 32u : 0u) | uint64_t{s.clip_mask} << 40;
  key |= uint64_t{color_count - 1} << 48;
  // (the stencil state: in the second key, bits 20 on)
  const uint64_t stencil_key = !s.stencil_test ? 0
                               : 1ull | uint64_t{s.stencil_compare & 7} << 1 | uint64_t{s.stencil_fail & 7} << 4 | uint64_t{s.stencil_depth_fail & 7} << 7 |
                                     uint64_t{s.stencil_pass & 7} << 10 | uint64_t{s.stencil_reference} << 13 | uint64_t{s.stencil_read_mask} << 21 | uint64_t{s.stencil_write_mask} << 29;
  if (s.custom_blend) {
    key |= uint64_t{1} << 6;
    key |= uint64_t{s.src_rgb & 15} << 8 | uint64_t{s.dst_rgb & 15} << 12 | uint64_t{s.src_alpha & 15} << 16 | uint64_t{s.dst_alpha & 15} << 20;
    key |= uint64_t{s.op_rgb & 7} << 24 | uint64_t{s.op_alpha & 7} << 27 | uint64_t{s.color_mask & 15} << 30 | uint64_t{s.depth_compare & 7} << 34;
  }
  // (the rest: the per-instance attributes - below 16, as the device's - the depth bias's primitives, and the blending of each target)
  const uint64_t key2 = uint64_t{s.instanced & 0xFFFF} | uint64_t{s.depth_bias_modes & 7u} << 16 | (s.per_target ? 1ull << 19 : 0) | stencil_key << 20;
  const float bias_slope = s.depth_bias_modes ? s.depth_bias_factor : 0.0f, bias_units = s.depth_bias_modes ? s.depth_bias_units : 0.0f;
  const auto same_targets = [&](const Variant& v) {
    if (!s.per_target) return true;
    for (uint32_t i = 0; i < color_count && i < webgl::kAccelMaxColors; ++i) {
      const webgl::AccelState::TargetBlend &a = v.targets[i], &b = s.targets[i];
      if (a.blend != b.blend || a.src_rgb != b.src_rgb || a.dst_rgb != b.dst_rgb || a.src_alpha != b.src_alpha || a.dst_alpha != b.dst_alpha || a.op_rgb != b.op_rgb ||
          a.op_alpha != b.op_alpha || a.color_mask != b.color_mask)
        return false;
    }
    return true;
  };
  for (const Variant& v : p.variants)
    if (v.key == key && v.key2 == key2 && v.bias_slope == bias_slope && v.bias_units == bias_units && same_targets(v)) return v.pipeline;
  GraphicsPipelineDesc d;
  d.vertex_shader = p.vs;
  d.fragment_shader = p.fs;
  d.geometry_shader = p.gs;
  d.hull_shader = p.hs;
  d.domain_shader = p.ds;
  // Per-vertex attributes in slot 0, one after another; per-instance ones (AccelState::instanced) in slot 1, the same buffer after the vertices (run_pass_on binds it).
  uint32_t per_vertex = 0, per_instance = 0;
  d.attribute_count = p.attributes;
  for (uint32_t i = 0; i < p.attributes; ++i) {
    if (s.instanced & (1u << i)) {
      d.attributes[i] = {i, VertexFormat::Float4, per_instance * 16, true, 1};
      ++per_instance;
    } else {
      d.attributes[i] = {i, VertexFormat::Float4, per_vertex * 16};
      ++per_vertex;
    }
  }
  d.vertex_stride = (per_vertex ? per_vertex : 1) * 16;
  d.slot_strides[1] = per_instance * 16;
  d.color_target_count = color_count;
  d.varying_count = p.varyings;
  d.texture_count = p.textures;
  d.cull_mode = cull == 0 ? CullMode::None : (cull == 1 ? CullMode::Front : CullMode::Back);
  d.front_face = s.front_ccw ? FrontFace::Clockwise : FrontFace::CounterClockwise;
  d.depth_test = s.depth_test;
  d.depth_write = s.depth_write;
  d.clip_distances = s.clip_mask;
  if (s.custom_blend) {      // (the numbers are the device's: Accelerator.hpp)
    if (s.src_rgb > 10 || s.dst_rgb > 10 || s.src_alpha > 10 || s.dst_alpha > 10 || s.op_rgb > 4 || s.op_alpha > 4 || s.depth_compare > 7) return nullptr;
    d.blend.custom = true;
    d.blend.enable = s.blend;
    d.blend.src_color = static_cast<BlendFactor>(s.src_rgb);
    d.blend.dst_color = static_cast<BlendFactor>(s.dst_rgb);
    d.blend.src_alpha = static_cast<BlendFactor>(s.src_alpha);
    d.blend.dst_alpha = static_cast<BlendFactor>(s.dst_alpha);
    d.blend.color_op = static_cast<BlendOp>(s.op_rgb);
    d.blend.alpha_op = static_cast<BlendOp>(s.op_alpha);
    d.blend.write_mask = s.color_mask & 15;
    d.depth_compare = static_cast<CompareOp>(s.depth_compare);
  } else {
    d.alpha_blend = s.blend;
  }
  if (s.per_target) {      // (each colour target its own blending and mask: target 0 the fields above, the others target_blend)
    if (!s.custom_blend || !(device_.info().features & kFeatureIndependentBlend)) return nullptr;
    const auto blend_of = [](const webgl::AccelState::TargetBlend& t, BlendState* b) {
      if (t.src_rgb > 10 || t.dst_rgb > 10 || t.src_alpha > 10 || t.dst_alpha > 10 || t.op_rgb > 4 || t.op_alpha > 4) return false;
      b->custom = true;
      b->enable = t.blend;
      b->src_color = static_cast<BlendFactor>(t.src_rgb);
      b->dst_color = static_cast<BlendFactor>(t.dst_rgb);
      b->src_alpha = static_cast<BlendFactor>(t.src_alpha);
      b->dst_alpha = static_cast<BlendFactor>(t.dst_alpha);
      b->color_op = static_cast<BlendOp>(t.op_rgb);
      b->alpha_op = static_cast<BlendOp>(t.op_alpha);
      b->write_mask = t.color_mask & 15;
      return true;
    };
    d.independent_blend = true;
    if (!blend_of(s.targets[0], &d.blend)) return nullptr;
    for (uint32_t i = 1; i < color_count && i < kMaxColorTargets; ++i)
      if (!blend_of(s.targets[i], &d.target_blend[i - 1])) return nullptr;
  }
  if (s.stencil_test) {
    if (!device_.info().limits.stencil || s.stencil_compare > 7 || s.stencil_fail > 7 || s.stencil_depth_fail > 7 || s.stencil_pass > 7) return nullptr;
    d.stencil.enable = true;
    d.stencil.compare = static_cast<CompareOp>(s.stencil_compare);
    d.stencil.fail_op = static_cast<StencilOp>(s.stencil_fail);
    d.stencil.depth_fail_op = static_cast<StencilOp>(s.stencil_depth_fail);
    d.stencil.pass_op = static_cast<StencilOp>(s.stencil_pass);
    d.stencil.reference = s.stencil_reference;
    d.stencil.compare_mask = s.stencil_read_mask;
    d.stencil.write_mask = s.stencil_write_mask;
  }
  if (s.depth_bias_modes) {
    if (!device_.info().limits.depth_bias) return nullptr;
    d.depth_bias_modes = s.depth_bias_modes & 7;
    d.depth_bias_slope = bias_slope;
    d.depth_bias_units = bias_units;
  }
  IPipeline* made = device_.create_graphics_pipeline(d);
  if (!made) return nullptr;
  Variant variant;
  variant.key = key;
  variant.key2 = key2;
  variant.bias_slope = bias_slope;
  variant.bias_units = bias_units;
  variant.per_target = s.per_target;
  for (uint32_t i = 0; i < webgl::kAccelMaxColors; ++i) variant.targets[i] = s.targets[i];
  variant.pipeline = made;
  if (!p.variants.push_back(variant)) {
    device_.destroy_pipeline(made);
    return nullptr;
  }
  return made;
}

// One render pass on a target that loads what is there: a clear, or a draw.
bool WebGlAccelerator::run_pass(Target& t, bool clear_color, const Vec4& color, bool clear_depth, float depth, Program* program, const webgl::AccelState* state,
                                ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) {
  RenderPassBegin pass;
  pass.color = t.color;
  pass.depth = t.depth;
  pass.clear_color = clear_color;
  pass.clear_color_value = color;
  pass.clear_depth = clear_depth;
  pass.clear_depth_value = depth;
  return run_pass_on(pass, t.width, t.height, 1, program, state, vertices, vertex_count, constants);
}

bool WebGlAccelerator::run_pass_on(const RenderPassBegin& pass, uint32_t width, uint32_t height, uint32_t color_count, Program* program, const webgl::AccelState* state,
                                   ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) {
  if (!commands_) commands_ = device_.create_command_buffer();
  if (!commands_) return false;
  IPipeline* pipeline = nullptr;
  uint64_t per_vertex_bytes = vertices.size();      // (the per-instance values follow the vertices: AccelState::instanced)
  if (program) {
    pipeline = pipeline_for(*program, *state, color_count);
    if (!pipeline) return false;
    if (state->texture_count != program->textures) return false;
    for (uint32_t i = 0; i < program->textures; ++i) {
      const Texture* tex = texture_at(state->textures[i]);
      if (!tex) return false;
      // (an image the pass draws into is not also read by it)
      if (tex->image == pass.depth || tex->image == pass.color) return false;
      for (IImage* extra : pass.extra_colors)
        if (extra && tex->image == extra) return false;
    }
    if (state->instanced) {
      const uint32_t instanced = static_cast<uint32_t>(__builtin_popcount(state->instanced));
      const uint64_t instance_bytes = uint64_t{state->instances} * instanced * 16;
      if (instance_bytes > vertices.size()) return false;
      per_vertex_bytes = vertices.size() - instance_bytes;
    }
    if (vertices.size() > vertex_capacity_) {
      if (vertex_buffer_) device_.destroy_buffer(vertex_buffer_);
      vertex_buffer_ = nullptr;
      vertex_capacity_ = 0;
      uint64_t want = 64 * 1024;
      while (want < vertices.size()) want *= 2;
      vertex_buffer_ = device_.create_buffer(want, kBufferVertex);
      if (!vertex_buffer_) return false;
      vertex_capacity_ = want;
    }
    ByteView mapped = vertex_buffer_->map();
    if (mapped.size() < vertices.size()) {
      vertex_buffer_->unmap();
      return false;
    }
    mapped.first(vertices.size()).copy_from(vertices);
    vertex_buffer_->unmap();
  }
  commands_->begin();
  commands_->begin_render_pass(pass);
  if (program) {
    commands_->bind_pipeline(pipeline);
    commands_->bind_vertex_buffer(vertex_buffer_);
    if (state->instanced) commands_->bind_vertex_buffer_slot(1, vertex_buffer_, per_vertex_bytes);
    for (uint32_t i = 0; i < program->textures; ++i) {
      const webgl::AccelSampler& as = state->samplers[i];
      SamplerDesc sampler;
      sampler.filter = as.linear ? Filter::Linear : Filter::Nearest;
      const auto mode = [](bool repeat, bool mirror) { return mirror ? AddressMode::MirrorRepeat : (repeat ? AddressMode::Repeat : AddressMode::ClampToEdge); };
      sampler.address_u = mode(as.repeat_s, as.mirror_s);
      sampler.address_v = mode(as.repeat_t, as.mirror_t);
      sampler.mip_filter = as.mip == 2 ? MipFilter::Linear : (as.mip == 1 ? MipFilter::Nearest : MipFilter::None);
      sampler.min_lod = as.min_lod;
      sampler.max_lod = as.max_lod;
      sampler.lod_bias = as.lod_bias;
      commands_->bind_texture(i, texture_at(state->textures[i])->image, sampler);
    }
    if (!constants.empty()) commands_->push_constants(constants.data(), static_cast<uint32_t>(constants.size()));
    Viewport vp;
    vp.x = state->vp_x;
    vp.y = state->vp_y;
    vp.width = state->vp_w;
    vp.height = state->vp_h;
    vp.min_depth = state->depth_near;
    vp.max_depth = state->depth_far;
    commands_->set_viewport(vp);
    Rect2D sc{0, 0, width, height};
    if (state->scissor) sc = Rect2D{state->sc_x, state->sc_y, state->sc_w, state->sc_h};
    commands_->set_scissor(sc);
    static constexpr PrimitiveTopology kTopologies[6] = {PrimitiveTopology::TriangleList, PrimitiveTopology::LineList, PrimitiveTopology::PointList,
                                                         PrimitiveTopology::PatchList, PrimitiveTopology::LineListAdjacency, PrimitiveTopology::TriangleListAdjacency};
    if (state->topology == 3) commands_->set_patch_control_points(program->patch_vertices);
    if (state->topology) commands_->set_primitive_topology(kTopologies[state->topology]);
    commands_->draw(vertex_count, 0, state->instances ? state->instances : 1);
  }
  commands_->end_render_pass();
  return commands_->end() && device_.submit(commands_);
}

bool WebGlAccelerator::clear(uint32_t handle, bool color, const float rgba[4], bool depth, float depth_value) {
  kernel::ScopedMutex guard(lock_);
  Target* t = target_at(handle);
  if (!t) return false;
  if (!color && !depth) return true;
  Vec4 c;
  c.x = rgba[0];
  c.y = rgba[1];
  c.z = rgba[2];
  c.w = rgba[3];
  return run_pass(*t, color, c, depth, depth_value, nullptr, nullptr, ConstByteView(), 0, ConstByteView());
}

bool WebGlAccelerator::draw_checks(Program& p, const webgl::AccelState& state, ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) {
  if (vertex_count == 0 || constants.size() > kMaxShaderConstantBytes || constants.size() % 16) return false;
  const DeviceInfo info = device_.info();
  // the vertex data: per vertex the attributes that are not per instance, then (AccelState::instanced) per instance the others
  if (p.attributes > 16 || (state.instanced >> p.attributes)) return false;
  const uint32_t instanced = static_cast<uint32_t>(__builtin_popcount(state.instanced));
  if ((state.instanced || state.instances != 1) && (!(info.features & kFeatureInstancing) || info.limits.vertex_buffers < 2 || state.instances == 0)) return false;
  if (instanced && instanced == p.attributes) return false;      // (a vertex has at least one attribute of its own)
  const uint64_t expected = uint64_t{vertex_count} * (p.attributes - instanced) * 16 + uint64_t{state.instances} * instanced * 16;
  if (vertices.size() != expected) return false;
  if (constants.size() > (info.limits.shader_constant_bytes > info.max_push_constant_bytes ? info.limits.shader_constant_bytes : info.max_push_constant_bytes)) return false;
  if (state.topology > 5 || (state.topology && !(info.features & kFeatureLines))) return false;
  // (patches go to the tessellation stages and only there, with as many vertices as the program was made for; adjacency only to a geometry stage that takes it)
  if ((state.topology == 3) != (p.hs != nullptr) || (state.topology == 3 && (state.patch_vertices != p.patch_vertices || vertex_count % p.patch_vertices))) return false;
  if (p.gs && !p.hs) {
    static constexpr uint8_t kTakes[6] = {2, 1, 0, 0xFF, 3, 4};   // (the GeometryPrimitive each topology makes)
    if (kTakes[state.topology] != p.gs_input) return false;
  }
  if ((state.topology == 4 || state.topology == 5) && !p.gs) return false;
  if (state.custom_blend && !(info.features & kFeatureBlendState)) return false;
  if (state.clip_mask && !(info.features & kFeatureClipDistances)) return false;
  return true;
}

bool WebGlAccelerator::draw(uint32_t handle, uint32_t program, const webgl::AccelState& state, ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) {
  kernel::ScopedMutex guard(lock_);
  Target* t = target_at(handle);
  Program* p = program_at(program);
  if (!t || !p || !draw_checks(*p, state, vertices, vertex_count, constants)) return false;
  return run_pass(*t, false, Vec4(), false, 1.0f, p, &state, vertices, vertex_count, constants);
}

// The colour image of a depth-only pass of w x h (the device's passes have one): made once per size, never shown, written with every channel masked.
IImage* WebGlAccelerator::blank_color(uint32_t w, uint32_t h) {
  for (const Blank& b : blanks_)
    if (b.width == w && b.height == h) return b.image;
  // (sampled too: stored upright, as the images it is drawn with - see VirglDevice's row order)
  IImage* image = device_.create_image({{w, h}, Format::B8G8R8A8Unorm, kImageColorAttachment | kImageSampled});
  if (!image) return nullptr;
  if (blanks_.count() >= 8) {      // (a few sizes at once at most: the oldest goes)
    device_.destroy_image(blanks_[0].image);
    blanks_.remove_at(0);
  }
  if (!blanks_.push_back(Blank{image, w, h})) {
    device_.destroy_image(image);
    return nullptr;
  }
  return image;
}

bool WebGlAccelerator::images_pass(const webgl::AccelAttachments& a, RenderPassBegin* pass, uint32_t* width, uint32_t* height) {
  if (a.color_count > kMaxColorTargets || a.color_count > webgl::kAccelMaxColors) return false;
  const DeviceInfo info = device_.info();
  if (a.color_count > 1 && (!(info.features & kFeatureMultipleTargets) || a.color_count > info.limits.color_targets)) return false;
  uint32_t w = 0, h = 0;
  const auto same_size = [&](const Texture& t) {
    if (w == 0) {
      w = t.width;
      h = t.height;
    }
    return t.width == w && t.height == h;
  };
  for (uint32_t i = 0; i < a.color_count; ++i) {
    const Texture* t = texture_at(a.colors[i]);
    if (!t || !t->renderable || t->format == webgl::AccelFormat::Depth || t->format == webgl::AccelFormat::DepthStencil || !same_size(*t)) return false;
    if (i == 0) pass->color = t->image;
    else pass->extra_colors[i - 1] = t->image;
  }
  if (a.depth) {
    const Texture* t = texture_at(a.depth);
    if (!t || (t->format != webgl::AccelFormat::Depth && t->format != webgl::AccelFormat::DepthStencil) || !same_size(*t)) return false;
    pass->depth = t->image;
  }
  if (w == 0) return false;
  if (!pass->color) pass->color = blank_color(w, h);
  if (!pass->color) return false;
  *width = w;
  *height = h;
  return true;
}

bool WebGlAccelerator::clear_images(const webgl::AccelAttachments& a, bool color, const float rgba[4], bool depth, float depth_value) {
  kernel::ScopedMutex guard(lock_);
  RenderPassBegin pass;
  uint32_t w = 0, h = 0;
  if (!images_pass(a, &pass, &w, &h)) return false;
  pass.clear_color = color && a.color_count > 0;
  pass.clear_color_value = Vec4{rgba[0], rgba[1], rgba[2], rgba[3]};
  pass.clear_depth = depth && a.depth;
  pass.clear_depth_value = depth_value;
  pass.clear_stencil = a.depth && a.stencil_clear >= 0 && a.stencil_clear <= 255;
  pass.clear_stencil_value = static_cast<uint8_t>(pass.clear_stencil ? a.stencil_clear : 0);
  if (!color && !depth && !pass.clear_stencil) return true;
  return run_pass_on(pass, w, h, a.color_count ? a.color_count : 1, nullptr, nullptr, ConstByteView(), 0, ConstByteView());
}

bool WebGlAccelerator::draw_images(const webgl::AccelAttachments& a, uint32_t program, const webgl::AccelState& state, ConstByteView vertices, uint32_t vertex_count,
                                   ConstByteView constants) {
  kernel::ScopedMutex guard(lock_);
  Program* p = program_at(program);
  RenderPassBegin pass;
  uint32_t w = 0, h = 0;
  if (!p || !draw_checks(*p, state, vertices, vertex_count, constants) || !images_pass(a, &pass, &w, &h)) return false;
  pass.clear_color = false;
  pass.clear_depth = false;
  pass.clear_stencil = false;
  // (a depth-only draw has the blank colour image, which must never be written: the colour mask is the device's blend state, so such a draw needs it)
  if (a.color_count == 0 && (!state.custom_blend || state.color_mask != 0)) return false;
  return run_pass_on(pass, w, h, a.color_count ? a.color_count : 1, p, &state, vertices, vertex_count, constants);
}

uint32_t WebGlAccelerator::add_texture(UniquePointer<Texture>&& t) {
  for (uint64_t i = 0; i < textures_.count(); ++i)
    if (!textures_[i]) {
      textures_[i] = static_cast<UniquePointer<Texture>&&>(t);
      return static_cast<uint32_t>(i + 1);
    }
  if (!textures_.push_back(static_cast<UniquePointer<Texture>&&>(t))) return 0;
  return static_cast<uint32_t>(textures_.count());
}

uint32_t WebGlAccelerator::create_layered(uint32_t w, uint32_t h, uint32_t layers, webgl::AccelKind kind) {
  if (!(device_.info().features & kFeatureTextureKinds) || w == 0 || h == 0 || layers == 0 || layers > kMaxTextureLayers) return 0;
  if (kind == webgl::AccelKind::Cube && (layers != 6 || w != h)) return 0;
  kernel::ScopedMutex guard(lock_);
  UniquePointer<Texture> t = UniquePointer<Texture>::make(allocator_);
  if (!t) return 0;
  t->width = w;
  t->height = h;
  t->layers = layers;
  ImageDesc d;
  d.extent = {w, h};
  d.format = Format::B8G8R8A8Unorm;
  d.usage = kImageSampled;
  d.kind = kind == webgl::AccelKind::Volume ? TextureKind::Texture3D : (kind == webgl::AccelKind::Cube ? TextureKind::TextureCube : TextureKind::Texture2DArray);
  d.layers = layers;
  t->image = device_.create_image(d);
  if (!t->image) return 0;
  const uint32_t handle = add_texture(static_cast<UniquePointer<Texture>&&>(t));
  return handle;
}

bool WebGlAccelerator::write_layer(uint32_t handle, uint32_t layer, ConstByteView rgba) {
  kernel::ScopedMutex guard(lock_);
  Texture* t = texture_at(handle);
  if (!t || !t->layers || layer >= t->layers) return false;
  const uint64_t texels = uint64_t{t->width} * t->height;
  if (rgba.size() != texels * 4) return false;
  ArrayView<uint32_t> scratch = allocate_view<uint32_t>(allocator_, texels);
  if (scratch.empty()) return false;
  for (uint64_t i = 0; i < texels; ++i)   // 0xAARRGGBB
    scratch[i] = (uint32_t{rgba[i * 4 + 3]} << 24) | (uint32_t{rgba[i * 4]} << 16) | (uint32_t{rgba[i * 4 + 1]} << 8) | rgba[i * 4 + 2];
  const bool ok = t->image->write_layer_pixels(layer, scratch);
  release_view(allocator_, scratch);
  return ok;
}

bool WebGlAccelerator::write_image(uint32_t handle, uint32_t level, ConstByteView pixels) {
  kernel::ScopedMutex guard(lock_);
  Texture* t = texture_at(handle);
  if (!t || level >= t->levels) return false;
  if (t->format == webgl::AccelFormat::Depth || t->format == webgl::AccelFormat::DepthStencil) return level == 0 && depth_planes(*t->image, t->format == webgl::AccelFormat::DepthStencil, pixels, nullptr);
  const uint32_t w = t->width >> level ? t->width >> level : 1, h = t->height >> level ? t->height >> level : 1;
  const uint64_t texels = uint64_t{w} * h;
  const bool floats = t->format == webgl::AccelFormat::Rgba16F || t->format == webgl::AccelFormat::Rgba32F;
  if (pixels.size() != texels * (floats ? 16 : 4)) return false;
  bool ok = false;
  if (floats) {
    ArrayView<Vec4> scratch = allocate_view<Vec4>(allocator_, texels);
    if (scratch.empty()) return false;
    ByteReader r(pixels);
    for (Vec4& v : scratch) {
      v.x = __builtin_bit_cast(float, r.le32());      // (the size was checked: every read is inside)
      v.y = __builtin_bit_cast(float, r.le32());
      v.z = __builtin_bit_cast(float, r.le32());
      v.w = __builtin_bit_cast(float, r.le32());
    }
    ok = t->image->write_pixels_float(scratch);
    release_view(allocator_, scratch);
    return ok;
  }
  ArrayView<uint32_t> scratch = allocate_view<uint32_t>(allocator_, texels);
  if (scratch.empty()) return false;
  for (uint64_t i = 0; i < texels; ++i)   // 0xAARRGGBB
    scratch[i] = (uint32_t{pixels[i * 4 + 3]} << 24) | (uint32_t{pixels[i * 4]} << 16) | (uint32_t{pixels[i * 4 + 1]} << 8) | pixels[i * 4 + 2];
  ok = level == 0 ? t->image->write_pixels(scratch) : t->image->write_mip_pixels(level, scratch);
  release_view(allocator_, scratch);
  return ok;
}

bool WebGlAccelerator::read_image(uint32_t handle, ByteView pixels) {
  kernel::ScopedMutex guard(lock_);
  Texture* t = texture_at(handle);
  if (!t) return false;
  if (t->format == webgl::AccelFormat::Depth || t->format == webgl::AccelFormat::DepthStencil) return depth_planes(*t->image, t->format == webgl::AccelFormat::DepthStencil, ConstByteView(), &pixels);
  const uint64_t texels = uint64_t{t->width} * t->height;
  const bool floats = t->format == webgl::AccelFormat::Rgba16F || t->format == webgl::AccelFormat::Rgba32F;
  if (pixels.size() != texels * (floats ? 16 : 4)) return false;
  if (floats) {
    ArrayView<Vec4> scratch = allocate_view<Vec4>(allocator_, texels);
    if (scratch.empty()) return false;
    const bool ok = t->image->read_pixels_float(scratch);
    if (ok) {
      ByteWriter w(pixels);
      for (const Vec4& v : scratch) {
        w.le32(__builtin_bit_cast(uint32_t, v.x));      // (the size was checked: every write is inside)
        w.le32(__builtin_bit_cast(uint32_t, v.y));
        w.le32(__builtin_bit_cast(uint32_t, v.z));
        w.le32(__builtin_bit_cast(uint32_t, v.w));
      }
    }
    release_view(allocator_, scratch);
    return ok;
  }
  ArrayView<uint32_t> scratch = allocate_view<uint32_t>(allocator_, texels);
  if (scratch.empty()) return false;
  const bool ok = t->image->read_pixels(scratch);
  if (ok) {
    for (uint64_t i = 0; i < texels; ++i) {
      const uint32_t v = scratch[i];   // 0xAARRGGBB
      pixels[i * 4] = static_cast<uint8_t>(v >> 16);
      pixels[i * 4 + 1] = static_cast<uint8_t>(v >> 8);
      pixels[i * 4 + 2] = static_cast<uint8_t>(v);
      pixels[i * 4 + 3] = static_cast<uint8_t>(v >> 24);
    }
  }
  release_view(allocator_, scratch);
  return ok;
}

// The inverse of read_color: the same rows, the pixels as 0xAARRGGBB.
// The planes of a depth image to or from `bytes` (`out` null: written to the image from `in`; else read into *out): 4 bytes a texel - the depth as a float -, or with
// `stencil` 8 - the depth, then the stencil in a word -, all little-endian, level 0, rows as the image's colour transfers have them.
bool WebGlAccelerator::depth_planes(IImage& image, bool stencil, ConstByteView in, ByteView* out) {
  if (!(device_.info().features2 & kFeature2DepthCopies)) return false;
  const Extent2D e = image.extent();
  const uint64_t texels = uint64_t{e.width} * e.height, texel = stencil ? 8u : 4u;
  if ((out ? out->size() : in.size()) != texels * texel) return false;
  ArrayView<float> depth = allocate_view<float>(allocator_, texels);
  ArrayView<uint8_t> stencil_plane = stencil ? allocate_view<uint8_t>(allocator_, texels) : ArrayView<uint8_t>();
  bool ok = !depth.empty() && (!stencil || !stencil_plane.empty());
  if (ok && out) {
    ok = image.read_depth_subresource(0, 0, depth, stencil_plane);
    ByteWriter w(*out);
    for (uint64_t i = 0; ok && i < texels; ++i) {
      w.le32(__builtin_bit_cast(uint32_t, depth[i]));
      if (stencil) w.le32(stencil_plane[i]);
    }
    ok = ok && w.ok();
  } else if (ok) {
    ByteReader r(in);
    for (uint64_t i = 0; i < texels; ++i) {
      depth[i] = __builtin_bit_cast(float, r.le32());
      if (stencil) stencil_plane[i] = static_cast<uint8_t>(r.le32());
    }
    ok = r.ok() && image.write_depth_subresource(0, 0, depth, stencil_plane);
  }
  if (!depth.empty()) release_view(allocator_, depth);
  if (!stencil_plane.empty()) release_view(allocator_, stencil_plane);
  return ok;
}

bool WebGlAccelerator::write_depth(uint32_t handle, ConstByteView depth) {
  kernel::ScopedMutex guard(lock_);
  Target* t = target_at(handle);
  return t && t->depth && depth_planes(*t->depth, false, depth, nullptr);
}

bool WebGlAccelerator::read_depth(uint32_t handle, ByteView depth) {
  kernel::ScopedMutex guard(lock_);
  Target* t = target_at(handle);
  return t && t->depth && depth_planes(*t->depth, false, ConstByteView(), &depth);
}

bool WebGlAccelerator::write_color(uint32_t handle, ConstByteView rgba) {
  kernel::ScopedMutex guard(lock_);
  Target* t = target_at(handle);
  if (!t) return false;
  const uint64_t pixels = uint64_t{t->width} * t->height;
  if (rgba.size() != pixels * 4) return false;
  ArrayView<uint32_t> scratch = allocate_view<uint32_t>(allocator_, pixels);
  if (scratch.empty()) return false;
  for (uint64_t i = 0; i < pixels; ++i)
    scratch[i] = (uint32_t{rgba[i * 4 + 3]} << 24) | (uint32_t{rgba[i * 4]} << 16) | (uint32_t{rgba[i * 4 + 1]} << 8) | rgba[i * 4 + 2];
  const bool ok = t->color->write_pixels(scratch);
  release_view(allocator_, scratch);
  return ok;
}

bool WebGlAccelerator::read_color(uint32_t handle, ByteView rgba) {
  kernel::ScopedMutex guard(lock_);
  Target* t = target_at(handle);
  if (!t) return false;
  const uint64_t pixels = uint64_t{t->width} * t->height;
  if (rgba.size() != pixels * 4) return false;
  ArrayView<uint32_t> scratch = allocate_view<uint32_t>(allocator_, pixels);
  if (scratch.empty()) return false;
  bool ok = t->color->read_pixels(scratch);
  if (ok) {
    for (uint64_t i = 0; i < pixels; ++i) {
      const uint32_t v = scratch[i];   // 0xAARRGGBB
      rgba[i * 4] = static_cast<uint8_t>(v >> 16);
      rgba[i * 4 + 1] = static_cast<uint8_t>(v >> 8);
      rgba[i * 4 + 2] = static_cast<uint8_t>(v);
      rgba[i * 4 + 3] = static_cast<uint8_t>(v >> 24);
    }
  }
  release_view(allocator_, scratch);
  return ok;
}

}  // namespace graphics
