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

#include "Desktop/CubeRenderer.hpp"
#include "Desktop/CubeSpirv.hpp"
#include "Graphics/Logo/Logo.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Dxgkrnl/CommandStream.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"

namespace desktop {

using namespace dxgk;
using graphics::ColoredVertex;
using graphics::TexturedVertex;

namespace {
constexpr uint32_t kBackground = kCubeBackground;

// The color of each face of the flat cube (the order of build_textured_cube's faces).
constexpr float kFaceColors[6][4] = {
    {0.95f, 0.30f, 0.30f, 1}, {0.30f, 0.85f, 0.35f, 1}, {0.30f, 0.50f, 0.95f, 1}, {0.95f, 0.85f, 0.25f, 1}, {0.85f, 0.35f, 0.90f, 1}, {0.30f, 0.85f, 0.90f, 1},
};

// Indices into the allocation list every stream is given.
enum : uint32_t { kListColor, kListDepth, kListVertices, kListIndices, kListLogo, kListSize };

uint64_t now_us() { return hardware::uptime_us(); }
}  // namespace

const char* cube_api_name(CubeApi api) {
  switch (api) {
    case CubeApi::Direct3D: return "Direct3D";
    case CubeApi::Vulkan: return "Vulkan";
    case CubeApi::OpenGL: return "OpenGL";
  }
  return "?";
}

void build_cube_geometry(ArrayView<ColoredVertex> colored, ArrayView<TexturedVertex> textured, ArrayView<uint16_t> indices) {
  graphics::build_textured_cube(textured, indices);
  for (uint32_t i = 0; i < graphics::kCubeVertices; ++i) {
    for (uint32_t k = 0; k < 3; ++k) colored[i].position[k] = textured[i].position[k];
    for (uint32_t k = 0; k < 4; ++k) colored[i].color[k] = kFaceColors[i / 4][k];
  }
}

bool render_cube_logo(allocators::IAllocator& allocator, ArrayView<uint32_t> texels) {
  graphics::Logo logo(allocator);
  return texels.size() == uint64_t{kCubeLogoSize} * kCubeLogoSize && logo.load(graphics::embedded_logo_png()) && logo.render(kCubeLogoSize, kBackground & 0xFFFFFF, texels);
}

struct CubeRenderer::State {
  Handle adapter = kNullHandle, device = kNullHandle, context = kNullHandle;
  Handle colored_pipeline = kNullHandle, textured_pipeline = kNullHandle;
  Handle color = kNullHandle, depth = kNullHandle;
  Handle colored_vertices = kNullHandle, textured_vertices = kNullHandle, indices = kNullHandle, logo = kNullHandle;
};

CubeRenderer::CubeRenderer(allocators::IAllocator& allocator, Kernel& kernel, ProcessId pid, CubeApi api)
    : allocator_(allocator), kernel_(kernel), pid_(pid), api_(api == CubeApi::Vulkan ? CubeApi::Vulkan : CubeApi::Direct3D) {
  process_open_ = status::ok(kernel_.open_process(pid_));
}

CubeRenderer::~CubeRenderer() {
  close();
  if (process_open_) kernel_.close_process(pid_);
}

graphics::Mat4 CubeRenderer::transform(float angle, float aspect) {
  return graphics::Mat4::scaling(1, -1, 1) * graphics::Mat4::perspective(0.9f, aspect, 0.1f, 20.0f) * graphics::Mat4::translation(0, 0, -4.5f) * graphics::Mat4::rotation_y(angle) *
         graphics::Mat4::rotation_x(0.6f * angle);
}

uint32_t CubeRenderer::find_adapter(Kernel& kernel, bool accelerated, const graphics::IGraphicsDevice* preferred) {
  const uint32_t count = kernel.adapter_count();
  uint32_t first = kInvalidIndex;
  for (uint32_t i = 0; i < count; ++i) {
    Adapter* a = kernel.adapter(i);
    if (!a || a->caps().accelerated != accelerated) continue;
    if (preferred && &a->device() == preferred) return i;
    if (first == kInvalidIndex) first = i;
  }
  return first;
}

void CubeRenderer::close() {
  if (state_) {
    // (the device owns the context, allocations, shaders and pipelines: they go with it)
    if (state_->device) kernel_.destroy_device(pid_, state_->device);
    if (state_->adapter) kernel_.close_adapter(pid_, state_->adapter);
    state_.reset();
  }
  release_view(allocator_, picture_.pixels);
  picture_ = graphics::Surface();
  texture_available_ = false;
  last_textured_ = false;
  shareable_ = false;
  target_lent_ = false;
  width_ = height_ = 0;
  adapter_index_ = kInvalidIndex;
}

bool CubeRenderer::create_buffer(uint32_t flags, uint64_t size, Handle* out) {
  AllocationDesc d;
  d.flags = flags;
  d.size = size;
  return status::ok(kernel_.create_allocation(pid_, state_->device, d, out));
}

// Fills an allocation (its old contents are not needed) with the bytes.
bool CubeRenderer::write(Handle allocation, ConstByteView bytes) {
  LockResult r;
  if (!status::ok(kernel_.lock(pid_, allocation, kLockDiscard, 0, &r))) return false;
  const bool ok = r.data.copy_from(bytes);
  kernel_.unlock(pid_, allocation);
  return ok;
}

// The pipeline of the built-in colored or textured shaders: what a user-mode driver does with the private-data escape, here through the typed calls. On the Vulkan path
// the shaders are SPIR-V and go through the escape (create_spirv_pipeline).
bool CubeRenderer::create_pipeline(bool textured, Handle* pipeline) {
  if (api_ == CubeApi::Vulkan) return create_spirv_pipeline(textured, pipeline);
  Handle vs = kNullHandle, fs = kNullHandle;
  if (!status::ok(kernel_.create_shader(pid_, state_->device, {textured ? ShaderKind::TexturedVertex : ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs)) ||
      !status::ok(kernel_.create_shader(pid_, state_->device, {textured ? ShaderKind::TexturedFragment : ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs)))
    return false;
  graphics::GraphicsPipelineDesc d;
  if (textured) graphics::describe_textured_pipeline(&d);
  else graphics::describe_colored_pipeline(&d);
  PipelineDesc pd;
  pd.vertex_shader = vs;
  pd.fragment_shader = fs;
  pd.vertex_stride = d.vertex_stride;
  pd.attribute_count = d.attribute_count;
  pd.varying_count = d.varying_count;
  pd.texture_count = d.texture_count;
  for (uint32_t i = 0; i < d.attribute_count; ++i) pd.attributes[i] = d.attributes[i];
  pd.cull_mode = graphics::CullMode::Back;   // (a closed cube: the far faces are never seen, so they are not rasterized or shaded)
  pd.depth_test = pd.depth_write = true;
  return status::ok(kernel_.create_pipeline(pid_, state_->device, pd, pipeline));
}

// A shader made through the private-data escape, as vulkan-1.dll makes it: CreateShader, the kind (DXBC), the stage (the bytecode says it), the size, the bytecode.
bool CubeRenderer::escape_create_shader(ConstByteView dxbc, Handle* shader) {
  ByteBuffer request(allocator_);
  if (dxbc.size() > 0xFFFFFFFFu || !request.put32(static_cast<uint32_t>(EscapeOp::CreateShader)) || !request.put32(static_cast<uint32_t>(ShaderKind::Dxbc)) || !request.put32(0) ||
      !request.put32(static_cast<uint32_t>(dxbc.size())) || !request.append(dxbc))
    return false;
  uint8_t reply[4] = {};
  uint32_t reply_bytes = 0;
  if (!status::ok(kernel_.escape(pid_, state_->device, request.view(), ByteView(reply), &reply_bytes)) || reply_bytes != 4) return false;
  ByteReader r{ConstByteView(reply)};
  *shader = r.le32();
  return r.ok() && *shader != kNullHandle;
}

// The Vulkan path: the cube's SPIR-V translated to DXBC (as vkCreateGraphicsPipelines does), the shaders and the pipeline made with the escape's words - the layout
// vulkan-1.dll writes (vk_pipeline.inc) and the kernel's EscapeOp::CreatePipeline reads.
bool CubeRenderer::create_spirv_pipeline(bool textured, Handle* pipeline) {
  constexpr uint32_t kPushBytes = sizeof(graphics::ColoredConstants);
  ByteBuffer vs_dxbc(allocator_), fs_dxbc(allocator_);
  SpirvTranslation vs_info, fs_info;
  if (!translate_spirv(allocator_, cube_spirv(textured, SpirvStage::Vertex), SpirvStage::Vertex, kPushBytes, vs_dxbc, &vs_info) ||
      !translate_spirv(allocator_, cube_spirv(textured, SpirvStage::Fragment), SpirvStage::Fragment, kPushBytes, fs_dxbc, &fs_info))
    return false;
  Handle vs = kNullHandle, fs = kNullHandle;
  if (!escape_create_shader(vs_dxbc.view(), &vs) || !escape_create_shader(fs_dxbc.view(), &fs)) return false;
  // (the layout of the vertices is the built-in pipelines' own: position at location 0, the color or the coordinates at 1)
  graphics::GraphicsPipelineDesc d;
  if (textured) graphics::describe_textured_pipeline(&d);
  else graphics::describe_colored_pipeline(&d);
  uint32_t varyings = 0;
  for (uint32_t l = 0; l < 32; ++l)
    if ((vs_info.outputs >> l) & 1) varyings = l + 1;
  ByteBuffer request(allocator_);
  bool ok = request.put32(static_cast<uint32_t>(EscapeOp::CreatePipeline)) && request.put32(vs) && request.put32(fs) && request.put32(d.vertex_stride) &&
            request.put32(d.attribute_count) && request.put32(varyings) && request.put32(fs_info.texture_units) &&
            request.put32(static_cast<uint32_t>(graphics::CullMode::Back)) && request.put32(static_cast<uint32_t>(graphics::FrontFace::CounterClockwise)) &&
            request.put32(1 | 2);   // (depth test and depth write, no blending)
  for (uint32_t a = 0; a < 4; ++a) {
    const bool used = a < d.attribute_count;
    ok = ok && request.put32(used ? d.attributes[a].location : 0) && request.put32(used ? static_cast<uint32_t>(d.attributes[a].format) : 0) &&
         request.put32(used ? d.attributes[a].offset : 0);
  }
  // (no stencil: enable, compare Always, the three ops Keep, the masks, the reference)
  ok = ok && request.put32(0) && request.put32(static_cast<uint32_t>(graphics::CompareOp::Always)) && request.put32(0) && request.put32(0) && request.put32(0) && request.put32(0xFF) &&
       request.put32(0xFF) && request.put32(0);
  if (!ok) return false;
  uint8_t reply[4] = {};
  uint32_t reply_bytes = 0;
  if (!status::ok(kernel_.escape(pid_, state_->device, request.view(), ByteView(reply), &reply_bytes)) || reply_bytes != 4) return false;
  ByteReader r{ConstByteView(reply)};
  *pipeline = r.le32();
  return r.ok() && *pipeline != kNullHandle;
}

// The cube's vertices for both looks, and the indices both share.
bool CubeRenderer::create_geometry() {
  TexturedVertex textured[graphics::kCubeVertices];
  ColoredVertex colored[graphics::kCubeVertices];
  uint16_t indices[graphics::kCubeIndices];
  build_cube_geometry(ArrayView<ColoredVertex>(colored), ArrayView<TexturedVertex>(textured), ArrayView<uint16_t>(indices));
  State& s = *state_;
  return create_buffer(kAllocVertexBuffer, sizeof colored, &s.colored_vertices) && create_buffer(kAllocVertexBuffer, sizeof textured, &s.textured_vertices) &&
         create_buffer(kAllocIndexBuffer, sizeof indices, &s.indices) && write(s.colored_vertices, bytes_of(colored)) && write(s.textured_vertices, bytes_of(textured)) &&
         write(s.indices, bytes_of(indices));
}

// The logo texture: best effort, an adapter that cannot do it just keeps the colors.
bool CubeRenderer::create_logo() {
  ArrayView<uint32_t> texels = allocate_view<uint32_t>(allocator_, uint64_t{kLogoSize} * kLogoSize);
  bool ok = !texels.empty() && render_cube_logo(allocator_, texels);
  State& s = *state_;
  if (ok) {
    AllocationDesc d;
    d.flags = kAllocTexture;
    d.format = Format::A8R8G8B8;
    d.width = d.height = kLogoSize;
    ok = status::ok(kernel_.create_allocation(pid_, s.device, d, &s.logo)) && write(s.logo, ConstByteView(reinterpret_cast<const uint8_t*>(texels.data()), texels.size_bytes()));   // (the texels are plain words, read as their bytes)
    if (!ok) {
      if (s.logo) kernel_.destroy_allocation(pid_, s.logo);
      s.logo = kNullHandle;
    }
  }
  release_view(allocator_, texels);
  return ok;
}

bool CubeRenderer::open(uint32_t adapter_index, uint32_t width, uint32_t height, bool shareable) {
  close();
  if (!process_open_ || !width || !height) return false;
  state_ = UniquePointer<State>::make(allocator_);
  if (!state_) return false;
  State& s = *state_;
  Adapter* adapter = kernel_.adapter(adapter_index);
  if (!adapter || !status::ok(kernel_.open_adapter_from_index(pid_, adapter_index, &s.adapter)) || !status::ok(kernel_.query_adapter_caps(pid_, s.adapter, &caps_)) ||
      !status::ok(kernel_.create_device(pid_, s.adapter, &s.device)) || !status::ok(kernel_.create_context(pid_, s.device, ContextPriority::Normal, &s.context))) {
    close();
    return false;
  }
  AllocationDesc target;
  target.flags = kAllocRenderTarget;
  target.format = Format::A8R8G8B8;
  target.width = width;
  target.height = height;
  AllocationDesc depth;
  depth.flags = kAllocDepthStencil;
  depth.format = Format::D32Float;
  depth.width = width;
  depth.height = height;
  picture_.pixels = allocate_view<uint32_t>(allocator_, uint64_t{width} * height);
  picture_.width = width;
  picture_.height = height;
  picture_.pitch = width;
  picture_.format = graphics::Format::B8G8R8A8Unorm;
  // (a device that cannot make a target that is also sampled gets the plain one, and its frames are read back)
  if (shareable) {
    AllocationDesc sampled = target;
    sampled.flags |= kAllocTexture;
    if (status::ok(kernel_.create_allocation(pid_, s.device, sampled, &s.color))) shareable_ = true;
  }
  if (!picture_.valid() || (!shareable_ && !status::ok(kernel_.create_allocation(pid_, s.device, target, &s.color))) || !status::ok(kernel_.create_allocation(pid_, s.device, depth, &s.depth)) ||
      !create_pipeline(false, &s.colored_pipeline) || !create_geometry()) {
    close();
    return false;
  }
  width_ = width;
  height_ = height;
  adapter_index_ = adapter_index;
  texture_available_ = false;
  if (create_pipeline(true, &s.textured_pipeline) && create_logo()) {
    texture_available_ = true;
  } else {
    if (s.textured_pipeline) kernel_.destroy_pipeline(pid_, s.textured_pipeline);
    s.textured_pipeline = kNullHandle;
  }
  return true;
}

// Brings the color target to the CPU and copies it into the picture.
bool CubeRenderer::read_back() {
  LockResult r;
  if (!status::ok(kernel_.lock(pid_, state_->color, kLockReadOnly, 0, &r))) return false;
  bool ok = r.pitch >= uint64_t{width_} * 4;
  for (uint32_t y = 0; ok && y < height_; ++y) {
    ByteView row;
    ArrayView<uint32_t> words;
    ArrayView<uint32_t> out;
    ok = r.data.try_subview(uint64_t{y} * r.pitch, uint64_t{width_} * 4, &row) && row.try_cast<uint32_t>(&words) && picture_.pixels.try_subview(uint64_t{y} * width_, width_, &out) &&
         out.copy_from(ArrayView<const uint32_t>(words.data(), words.size()));
  }
  kernel_.unlock(pid_, state_->color);
  return ok;
}

bool CubeRenderer::render(float angle, bool textured, CubeFrameTimes* times, bool read_back_frame) {
  if (!state_ || target_lent_) return false;
  State& s = *state_;
  const bool use_logo = textured && texture_available_ && s.logo;
  last_textured_ = use_logo;
  const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
  const graphics::ColoredConstants constants{transform(angle, aspect)};
  CommandWriter w(allocator_);
  w.begin_pass(kListColor, kListDepth, kPassClearColor | kPassClearDepth, {0.063f, 0.063f, 0.102f, 1.0f}, 1.0f, 0);
  w.bind_pipeline(use_logo ? s.textured_pipeline : s.colored_pipeline);
  w.bind_vertex_buffer(kListVertices);
  if (use_logo) {
    graphics::SamplerDesc sampler;
    sampler.filter = graphics::Filter::Linear;
    sampler.address_u = sampler.address_v = graphics::AddressMode::ClampToEdge;
    w.bind_texture(0, kListLogo, sampler);
  }
  w.bind_index_buffer(kListIndices, graphics::IndexType::Uint16);
  w.push_constants(bytes_of(constants));
  w.draw_indexed(graphics::kCubeIndices);
  w.end_pass();
  if (!w.ok()) return false;

  const Handle list[kListSize] = {s.color, s.depth, use_logo ? s.textured_vertices : s.colored_vertices, s.indices, s.logo};
  const uint64_t start = now_us();
  const bool submitted = status::ok(kernel_.render(pid_, s.context, w.view(), ArrayView<const Handle>(list, use_logo ? kListSize : kListLogo)));
  const uint64_t submitted_at = now_us();
  const bool read = submitted && (!read_back_frame || read_back());
  const uint64_t done = now_us();
  if (times) {
    times->submit_us = submitted_at - start;
    times->readback_us = done - submitted_at;
  }
  return read;
}

bool CubeRenderer::acquire_target(graphics::IGraphicsDevice** device, graphics::IImage** image) {
  if (!state_ || !shareable_ || target_lent_) return false;
  AcquiredImage lent;
  if (!status::ok(kernel_.acquire_image(pid_, state_->color, &lent))) return false;
  *device = lent.device;
  *image = lent.image;
  target_lent_ = true;
  return true;
}

void CubeRenderer::release_target() {
  if (!target_lent_) return;
  kernel_.release_image(pid_, state_->color);
  target_lent_ = false;
}

}  // namespace desktop
