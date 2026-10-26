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
#include "WebGl/TgsiTranslator.hpp"

namespace graphics {

WebGlAccelerator::WebGlAccelerator(IGraphicsDevice& device, allocators::IAllocator& allocator)
    : device_(device), allocator_(allocator), programs_(allocator), targets_(allocator) {}

WebGlAccelerator::~WebGlAccelerator() {
  for (auto& p : programs_)
    if (p) destroy_program_locked(*p);
  for (auto& t : targets_) {
    if (!t) continue;
    if (t->color) device_.destroy_image(t->color);
    if (t->depth) device_.destroy_image(t->depth);
  }
  if (commands_) device_.destroy_command_buffer(commands_);
  if (vertex_buffer_) device_.destroy_buffer(vertex_buffer_);
}

void WebGlAccelerator::destroy_program_locked(Program& p) {
  for (IPipeline*& pl : p.pipelines)
    if (pl) device_.destroy_pipeline(pl);
  if (p.vs) device_.destroy_shader_module(p.vs);
  if (p.fs) device_.destroy_shader_module(p.fs);
  p = Program();
}

uint32_t WebGlAccelerator::create_program(const char* vs_tgsi, const char* fs_tgsi, uint32_t attributes, uint32_t varyings) {
  if (attributes == 0 || attributes > kMaxVertexAttributes || varyings > kMaxVaryings) return 0;
  kernel::ScopedMutex guard(lock_);
  UniquePointer<Program> p = UniquePointer<Program>::make(allocator_);
  if (!p) return 0;
  ShaderModuleDesc vd;
  vd.stage = ShaderStage::Vertex;
  vd.tgsi = vs_tgsi;
  ShaderModuleDesc fd;
  fd.stage = ShaderStage::Fragment;
  fd.tgsi = fs_tgsi;
  p->vs = device_.create_shader_module(vd);
  p->fs = p->vs ? device_.create_shader_module(fd) : nullptr;
  if (!p->vs || !p->fs) {   // a device without TGSI refuses here, and the draw stays on the CPU
    destroy_program_locked(*p);
    return 0;
  }
  p->attributes = attributes;
  p->varyings = varyings;
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

IPipeline* WebGlAccelerator::pipeline_for(Program& p, const webgl::AccelState& s) {
  // GL's window rows are the device's rows in the same order (row 0 is the bottom of the GL window, which the device draws as its first row), so the picture is the
  // device's upside down and a counter-clockwise triangle in GL is a clockwise one for the device.
  const uint32_t cull = s.cull > 2 ? 0 : s.cull;
  const uint32_t key = cull | (s.front_ccw ? 4u : 0u) | (s.depth_test ? 8u : 0u) | (s.depth_write ? 16u : 0u) | (s.blend ? 32u : 0u);
  IPipeline*& slot = p.pipelines[key];
  if (slot) return slot;
  GraphicsPipelineDesc d;
  d.vertex_shader = p.vs;
  d.fragment_shader = p.fs;
  d.vertex_stride = p.attributes * 16;
  d.attribute_count = p.attributes;
  for (uint32_t i = 0; i < p.attributes; ++i) d.attributes[i] = {i, VertexFormat::Float4, i * 16};
  d.varying_count = p.varyings;
  d.cull_mode = cull == 0 ? CullMode::None : (cull == 1 ? CullMode::Front : CullMode::Back);
  d.front_face = s.front_ccw ? FrontFace::Clockwise : FrontFace::CounterClockwise;
  d.depth_test = s.depth_test;
  d.depth_write = s.depth_write;
  d.alpha_blend = s.blend;
  slot = device_.create_graphics_pipeline(d);
  return slot;
}

// One render pass on a target that loads what is there: a clear, or a draw.
bool WebGlAccelerator::run_pass(Target& t, bool clear_color, const Vec4& color, bool clear_depth, float depth, Program* program, const webgl::AccelState* state,
                                ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) {
  if (!commands_) commands_ = device_.create_command_buffer();
  if (!commands_) return false;
  IPipeline* pipeline = nullptr;
  if (program) {
    pipeline = pipeline_for(*program, *state);
    if (!pipeline) return false;
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
  RenderPassBegin pass;
  pass.color = t.color;
  pass.depth = t.depth;
  pass.clear_color = clear_color;
  pass.clear_color_value = color;
  pass.clear_depth = clear_depth;
  pass.clear_depth_value = depth;
  commands_->begin();
  commands_->begin_render_pass(pass);
  if (program) {
    commands_->bind_pipeline(pipeline);
    commands_->bind_vertex_buffer(vertex_buffer_);
    if (!constants.empty()) commands_->push_constants(constants.data(), static_cast<uint32_t>(constants.size()));
    Viewport vp;
    vp.x = state->vp_x;
    vp.y = state->vp_y;
    vp.width = state->vp_w;
    vp.height = state->vp_h;
    vp.min_depth = state->depth_near;
    vp.max_depth = state->depth_far;
    commands_->set_viewport(vp);
    Rect2D sc{0, 0, t.width, t.height};
    if (state->scissor) sc = Rect2D{state->sc_x, state->sc_y, state->sc_w, state->sc_h};
    commands_->set_scissor(sc);
    commands_->draw(vertex_count);
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

bool WebGlAccelerator::draw(uint32_t handle, uint32_t program, const webgl::AccelState& state, ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) {
  kernel::ScopedMutex guard(lock_);
  Target* t = target_at(handle);
  Program* p = program_at(program);
  if (!t || !p || vertex_count == 0 || vertices.size() != uint64_t{vertex_count} * p->attributes * 16 || constants.size() > kMaxPushConstantBytes || constants.size() % 16) return false;
  return run_pass(*t, false, Vec4(), false, 1.0f, p, &state, vertices, vertex_count, constants);
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
